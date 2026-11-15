// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Intro-first video worker. Owns one thread, one popup window and every Bink call made
// while it runs. It never calls engine, Python or BLSE code.
//
// Bink facts (IDA, binkw32.dll sha256 6e40138b..., exports are __stdcall/decorated):
//  - HBINK: +0 Width, +4 Height, +8 Frames, +0x0C FrameNum (1-based current frame).
//    Engine +0xfbd10 ends a video when FrameNum==Frames after the blit, before NextFrame.
//  - BinkCopyToBuffer(bink,dest,pitch,height,x,y,flags): surface type 3 is 32-bit with
//    R<<16|G<<8|B, i.e. a BI_RGB 32bpp DIB (converter table at 0x1802951C).
//  - BinkSetSoundSystem(BinkOpenDirectSound,param): param!=0 is a caller-owned IDirectSound*
//    (no DirectSoundCreate, no SetCooperativeLevel, not released by Bink). param==0 makes
//    Bink create its own device bound to a process-wide HWND it caches forever
//    (sub_1800A390), which would go stale on restart, so the worker always passes its own.
//    Sound buffers are created with DSBCAPS_GLOBALFOCUS (0x180E0).
//  - BinkOpen flag 0x4000 (BINKSNDTRACK) with BinkSetSoundTrack(0,...) opens no audio track.
#include "intro_worker.h"
#include <dsound.h>
#include <process.h>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <new>
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")

#ifdef ENGINE_FIXES_INTRO_WORKER_TESTING
#include "intro_worker_test_hooks.h"
#endif

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace engine_fixes_intro {
namespace {

constexpr std::uint32_t kBinkSurface32=3;
constexpr std::uint32_t kBinkSoundTrack=0x4000;
constexpr int kMaxVideoDimension=8192;
constexpr wchar_t kTitle[]=L"Vampire: Bloodlines - Intro"; // not the game's exact title

struct BinkHeader { std::uint32_t width,height,frames,frame_num; };

using BinkOpenFn=void*(__stdcall*)(const char*,std::uint32_t);
using BinkCloseFn=void(__stdcall*)(void*);
using BinkWaitFn=int(__stdcall*)(void*);
using BinkDoFrameFn=int(__stdcall*)(void*);
using BinkNextFrameFn=void(__stdcall*)(void*);
using BinkCopyToBufferFn=int(__stdcall*)(void*,void*,int,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t);
using BinkGetErrorFn=const char*(__stdcall*)();
using BinkSoundOpenFn=void*(__stdcall*)(std::uintptr_t);
using BinkSetSoundSystemFn=int(__stdcall*)(BinkSoundOpenFn,std::uintptr_t);
using BinkSetSoundTrackFn=std::uint32_t(__stdcall*)(std::uint32_t,const std::uint32_t*);
using DirectSoundCreateFn=HRESULT(WINAPI*)(LPCGUID,LPDIRECTSOUND*,LPUNKNOWN);
using DwmSetWindowAttributeFn=HRESULT(WINAPI*)(HWND,DWORD,LPCVOID,DWORD);
using DwmFlushFn=HRESULT(WINAPI*)();
constexpr DWORD kDwmTransitionsForceDisabled=3; // DWMWA_TRANSITIONS_FORCEDISABLED (per window)
constexpr DWORD kDefaultCoverMs=3000,kMinCoverMs=250,kMaxCoverMs=10000;

struct BinkApi {
    BinkOpenFn open=nullptr;
    BinkCloseFn close=nullptr;
    BinkWaitFn wait=nullptr;
    BinkDoFrameFn do_frame=nullptr;
    BinkNextFrameFn next_frame=nullptr;
    BinkCopyToBufferFn copy_to_buffer=nullptr;
    BinkGetErrorFn get_error=nullptr;
    BinkSoundOpenFn open_direct_sound=nullptr;
    BinkSetSoundSystemFn set_sound_system=nullptr;
    BinkSetSoundTrackFn set_sound_track=nullptr;
};

struct Video { const wchar_t* file; bool fit_4_3; };
// Engine +0xfb524..+0xfb553: activision/whitewolf scale=1 (Auto Bink AR keeps 4:3),
// NvidiaBloodLogo/troika scale=0 (centred at native size).
constexpr Video kVideos[]={
    {L"activision.bik",true},
    {L"whitewolf.bik",true},
    {L"NvidiaBloodLogo.bik",false},
    {L"troika.bik",false},
};

struct Session {
    Worker* worker=nullptr;
    Log log=nullptr;
    LARGE_INTEGER frequency={},origin={};
    // Owned icon copies (made in start(), destroyed by the thread after the window).
    HICON icon_big=nullptr,icon_small=nullptr;
    HMODULE self=nullptr;
    HMODULE bink=nullptr;
    bool bink_pinned=false;
    BinkApi api;
    wchar_t class_name[96]={};
    ATOM window_class=0;
    HWND window=nullptr;
    HDC memory_dc=nullptr;
    HBITMAP dib=nullptr;
    HGDIOBJ old_bitmap=nullptr;
    void* bits=nullptr;
    int video_width=0,video_height=0;
    bool fit_4_3=false,have_frame=false,need_clear=true;
    IDirectSound* sound=nullptr;
    HANDLE timer=nullptr;
    bool shown=false,first_frame=false,stop_all=false,aborted=false;
    std::atomic<bool> covering{false}; // read by window_proc (same thread) and tests
    unsigned videos_opened=0;
    HMODULE dwm=nullptr;
    DwmSetWindowAttributeFn dwm_set_attribute=nullptr;
    DwmFlushFn dwm_flush=nullptr;
};

LONG g_class_counter=0;

void logf(const Session& s,const char* format,...) noexcept {
    if (!s.log) return;
    char text[640];
    int prefix=std::snprintf(text,sizeof text,"intro worker: ");
    if (prefix<0) return;
    va_list args; va_start(args,format);
    std::vsnprintf(text+prefix,sizeof text-static_cast<size_t>(prefix),format,args);
    va_end(args);
    s.log(text);
}
void log_plain(Log log,const char* text) noexcept { if (log) log(text); }

double elapsed_ms(const Session& s) noexcept {
    if (!s.frequency.QuadPart) return 0;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return 1000.0*static_cast<double>(now.QuadPart-s.origin.QuadPart)/static_cast<double>(s.frequency.QuadPart);
}

bool is_set(HANDLE event) noexcept { return event && WaitForSingleObject(event,0)==WAIT_OBJECT_0; }
bool should_stop(Session& s) noexcept {
    if (is_set(s.worker->abort)) s.aborted=true;
    return s.aborted || s.stop_all || s.worker->close_requested.load();
}

long long scaled(long long value,long long numerator,long long denominator) noexcept {
    return (value*numerator+denominator/2)/denominator;
}

RECT compute_dest(int cw,int ch,int vw,int vh,bool fit_4_3) noexcept {
    RECT r={0,0,0,0};
    if (cw<=0 || ch<=0 || vw<=0 || vh<=0) return r;
    long long w,h;
    if (fit_4_3) {
        if (static_cast<long long>(cw)*3>=static_cast<long long>(ch)*4) { h=ch; w=scaled(ch,4,3); }
        else { w=cw; h=scaled(cw,3,4); }
    } else {
        w=vw; h=vh;
        if (w>cw || h>ch) {
            if (static_cast<long long>(cw)*vh>=static_cast<long long>(ch)*vw) { h=ch; w=scaled(ch,vw,vh); }
            else { w=cw; h=scaled(cw,vh,vw); }
        }
    }
    if (w>cw) w=cw;
    if (h>ch) h=ch;
    r.left=static_cast<LONG>((cw-w)/2); r.top=static_cast<LONG>((ch-h)/2);
    r.right=r.left+static_cast<LONG>(w); r.bottom=r.top+static_cast<LONG>(h);
    return r;
}

bool paint(Session& s,HDC dc,bool full) noexcept {
    RECT client; if (!GetClientRect(s.window,&client)) return false;
    HBRUSH black=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!s.have_frame || !s.dib) { return FillRect(dc,&client,black)!=0 && s.worker->config.skip_videos; }
    const RECT d=compute_dest(client.right,client.bottom,s.video_width,s.video_height,s.fit_4_3);
    if (full || s.need_clear) {
        const int saved=SaveDC(dc);
        ExcludeClipRect(dc,d.left,d.top,d.right,d.bottom);
        FillRect(dc,&client,black);
        if (saved) RestoreDC(dc,saved);
        s.need_clear=false;
    }
    const int w=d.right-d.left,h=d.bottom-d.top;
    if (w<=0 || h<=0) return false;
    BOOL ok;
    if (w==s.video_width && h==s.video_height) ok=BitBlt(dc,d.left,d.top,w,h,s.memory_dc,0,0,SRCCOPY);
    else {
        SetStretchBltMode(dc,HALFTONE); SetBrushOrgEx(dc,0,0,nullptr);
        ok=StretchBlt(dc,d.left,d.top,w,h,s.memory_dc,0,0,s.video_width,s.video_height,SRCCOPY);
    }
    if (!ok) logf(s,"error: frame blit failed (%lu)",GetLastError());
    return ok!=FALSE;
}

bool present_now(Session& s) noexcept {
#ifdef ENGINE_FIXES_INTRO_WORKER_TESTING
    if (test_hooks::hooks.present_frame) return test_hooks::hooks.present_frame(s.window)!=FALSE;
#endif
    HDC dc=GetDC(s.window);
    if (!dc) { logf(s,"error: GetDC failed (%lu)",GetLastError()); return false; }
    const bool drawn=paint(s,dc,false);
    ReleaseDC(s.window,dc);
    return GdiFlush()!=FALSE && drawn;
}

void clear_window(Session& s) noexcept {
    if (!s.shown) return;
    HDC dc=GetDC(s.window);
    if (!dc) return;
    RECT client; if (GetClientRect(s.window,&client)) FillRect(dc,&client,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    ReleaseDC(s.window,dc);
    GdiFlush();
}

LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message==WM_NCCREATE) {
        auto create=reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto s=reinterpret_cast<Session*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    switch (message) {
    case WM_CLOSE:
        // Never destroy here: main forwards the close to the game after native Init returns.
        if (s) {
            if (!s->worker->close_requested.exchange(true)) logf(*s,"close requested at %.1f ms; ending videos",elapsed_ms(*s));
            s->stop_all=true;
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        // While covering the shown game window, a click must neither activate the cover
        // nor reach the game through it.
        if (s && s->covering.load()) return MA_NOACTIVATEANDEAT;
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(window,&ps);
        if (dc) {
            if (s && s->window) paint(*s,dc,true);
            else FillRect(dc,&ps.rcPaint,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        EndPaint(window,&ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

// Drains this thread's queue (bounded per call). Returns true when the original skip input
// (WM_KEYDOWN or WM_LBUTTONUP, engine +0xfbd10) was seen.
bool pump(Session& s) noexcept {
    bool skip=false; MSG msg;
    for (int budget=512; budget>0 && PeekMessageW(&msg,nullptr,0,0,PM_REMOVE); --budget) {
        if (msg.message==WM_QUIT) { logf(s,"WM_QUIT on worker queue; ending videos"); s.stop_all=true; continue; }
        if (msg.message==WM_KEYDOWN || msg.message==WM_LBUTTONUP) skip=true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return skip;
}

// Waits about 1 ms (high-resolution timer when available) or until abort/input arrives.
void wait_short(Session& s) noexcept {
    HANDLE handles[2]={s.worker->abort,s.timer};
    DWORD count=1,timeout=1;
    if (s.timer) {
        LARGE_INTEGER due; due.QuadPart=-10000; // 1 ms relative
        if (SetWaitableTimer(s.timer,&due,0,nullptr,nullptr,FALSE)) { count=2; timeout=50; }
        else { logf(s,"error: SetWaitableTimer failed (%lu); using message wait timeout",GetLastError()); CloseHandle(s.timer); s.timer=nullptr; }
    }
    if (MsgWaitForMultipleObjectsEx(count,handles,timeout,QS_ALLINPUT,MWMO_INPUTAVAILABLE)==WAIT_FAILED) {
        logf(s,"error: frame wait failed (%lu)",GetLastError());
        Sleep(1);
    }
}

bool show(Session& s) noexcept {
#ifdef ENGINE_FIXES_INTRO_WORKER_TESTING
    if (test_hooks::hooks.show_window) return test_hooks::hooks.show_window(s.window)!=FALSE;
#endif
    ShowWindow(s.window,SW_SHOWNORMAL);
    if (!SetForegroundWindow(s.window)) logf(s,"SetForegroundWindow refused; skip input needs a click on the intro window");
    UpdateWindow(s.window);
    return IsWindowVisible(s.window)!=FALSE;
}

bool wide_to_ansi_path(const wchar_t* path,char* out,int capacity) noexcept {
    BOOL lossy=FALSE;
    int n=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path,-1,out,capacity,nullptr,&lossy);
    if (n>0 && !lossy) return true;
    wchar_t short_path[MAX_PATH*2];
    const DWORD length=GetShortPathNameW(path,short_path,MAX_PATH*2);
    if (!length || length>=MAX_PATH*2) return false;
    lossy=FALSE;
    n=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,short_path,-1,out,capacity,nullptr,&lossy);
    return n>0 && !lossy;
}

bool absolute_path(const wchar_t* p) noexcept {
    return p[0] && ((p[1]==L':' && (p[2]==L'\\' || p[2]==L'/')) || (p[0]==L'\\' && p[1]==L'\\'));
}

template<class T> bool resolve(Session& s,T& target,const char* name) noexcept {
    target=reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(s.bink,name)));
    if (!target) logf(s,"error: binkw32 export %s missing",name);
    return target!=nullptr;
}

bool load_bink(Session& s) noexcept {
    const wchar_t* path=s.worker->config.bink_path;
    if (!absolute_path(path)) { logf(s,"error: bink path is not absolute"); return false; }
    HMODULE existing=nullptr;
    const bool preloaded=GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,path,&existing)!=FALSE;
    s.bink=LoadLibraryExW(path,nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!s.bink) { logf(s,"error: LoadLibrary binkw32 failed (%lu)",GetLastError()); return false; }
    // Bink parks background I/O threads inside binkw32 after BinkClose; only drop our
    // reference when another owner (the engine import) keeps the module mapped.
    s.bink_pinned=!preloaded;
    logf(s,"binkw32 %p %s",static_cast<void*>(s.bink),preloaded?"already loaded (reference released after use)":"not previously loaded at this path (kept loaded after use)");
    bool ok=true;
    ok&=resolve(s,s.api.open,"_BinkOpen@8");
    ok&=resolve(s,s.api.close,"_BinkClose@4");
    ok&=resolve(s,s.api.wait,"_BinkWait@4");
    ok&=resolve(s,s.api.do_frame,"_BinkDoFrame@4");
    ok&=resolve(s,s.api.next_frame,"_BinkNextFrame@4");
    ok&=resolve(s,s.api.copy_to_buffer,"_BinkCopyToBuffer@28");
    ok&=resolve(s,s.api.get_error,"_BinkGetError@0");
    ok&=resolve(s,s.api.open_direct_sound,"_BinkOpenDirectSound@4");
    ok&=resolve(s,s.api.set_sound_system,"_BinkSetSoundSystem@8");
    ok&=resolve(s,s.api.set_sound_track,"_BinkSetSoundTrack@8");
    if (!ok) {
        // No Bink function has been called, so releasing our reference is safe.
        FreeLibrary(s.bink); s.bink=nullptr; s.api=BinkApi{};
    }
    return ok;
}

void unload_bink(Session& s) noexcept {
    if (!s.bink) return;
    s.api=BinkApi{};
    if (s.bink_pinned) logf(s,"binkw32 reference kept (worker was its first loader)");
    else if (!FreeLibrary(s.bink)) logf(s,"error: FreeLibrary binkw32 failed (%lu)",GetLastError());
    s.bink=nullptr;
}

// Per-window only (no global animation setting changes): the intro window neither grows in
// on its first show nor fades out over the game window when it is destroyed.
void load_dwm(Session& s) noexcept {
    s.dwm=LoadLibraryExW(L"dwmapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s.dwm) { logf(s,"dwmapi unavailable (%lu); window transitions unchanged",GetLastError()); return; }
    s.dwm_set_attribute=reinterpret_cast<DwmSetWindowAttributeFn>(reinterpret_cast<void*>(GetProcAddress(s.dwm,"DwmSetWindowAttribute")));
    s.dwm_flush=reinterpret_cast<DwmFlushFn>(reinterpret_cast<void*>(GetProcAddress(s.dwm,"DwmFlush")));
}
void unload_dwm(Session& s) noexcept {
    s.dwm_set_attribute=nullptr; s.dwm_flush=nullptr;
    if (s.dwm) { FreeLibrary(s.dwm); s.dwm=nullptr; }
}
void disable_transitions(Session& s) noexcept {
    if (!s.dwm_set_attribute) return;
    const BOOL disabled=TRUE;
    const HRESULT hr=s.dwm_set_attribute(s.window,kDwmTransitionsForceDisabled,&disabled,sizeof disabled);
    if (FAILED(hr)) logf(s,"DWM transitions not disabled (0x%08lx); intro window may animate",static_cast<unsigned long>(hr));
}

bool create_window(Session& s) noexcept {
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&window_proc),&s.self)) {
        logf(s,"error: own module lookup failed (%lu)",GetLastError()); return false;
    }
    const LONG serial=InterlockedIncrement(&g_class_counter);
    std::swprintf(s.class_name,sizeof s.class_name/sizeof s.class_name[0],L"EngineFixesIntroWorker.%p.%ld",static_cast<void*>(s.self),serial);
    WNDCLASSEXW wc={};
    wc.cbSize=sizeof wc;
    wc.lpfnWndProc=window_proc;
    wc.hInstance=s.self;
    wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName=s.class_name;
    s.window_class=RegisterClassExW(&wc);
    if (!s.window_class) { logf(s,"error: RegisterClassEx failed (%lu)",GetLastError()); return false; }
    const RECT& r=s.worker->config.screen_rect;
    s.window=CreateWindowExW(WS_EX_APPWINDOW,s.class_name,kTitle,WS_POPUP,r.left,r.top,r.right-r.left,r.bottom-r.top,
        nullptr,nullptr,s.self,&s);
    if (!s.window) { logf(s,"error: CreateWindowEx failed (%lu)",GetLastError()); return false; }
    disable_transitions(s);
    if (s.icon_big) SendMessageW(s.window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(s.icon_big));
    if (s.icon_small) SendMessageW(s.window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(s.icon_small));
    HDC dc=GetDC(s.window);
    s.memory_dc=dc?CreateCompatibleDC(dc):nullptr;
    if (dc) ReleaseDC(s.window,dc);
    if (!s.memory_dc) { logf(s,"error: CreateCompatibleDC failed (%lu)",GetLastError()); return false; }
    s.worker->window.store(s.window);
    return true;
}

void destroy_window(Session& s) noexcept {
    if (s.memory_dc) { DeleteDC(s.memory_dc); s.memory_dc=nullptr; }
    if (s.window) {
        s.worker->window.store(nullptr);
        if (!DestroyWindow(s.window)) logf(s,"error: DestroyWindow failed (%lu)",GetLastError());
        s.window=nullptr;
    }
    if (s.window_class) {
        if (!UnregisterClassW(s.class_name,s.self)) logf(s,"error: UnregisterClass failed (%lu)",GetLastError());
        s.window_class=0;
    }
    if (s.icon_big) { DestroyIcon(s.icon_big); s.icon_big=nullptr; }
    if (s.icon_small) { DestroyIcon(s.icon_small); s.icon_small=nullptr; }
}

void init_sound(Session& s) noexcept {
    DirectSoundCreateFn create=nullptr;
#ifdef ENGINE_FIXES_INTRO_WORKER_TESTING
    create=test_hooks::hooks.direct_sound_create;
#endif
    if (!create) {
        wchar_t path[MAX_PATH]; const UINT n=GetSystemDirectoryW(path,MAX_PATH);
        if (!n || n>=MAX_PATH-16) { logf(s,"error: system directory unavailable; intros silent"); return; }
        wcscat_s(path,L"\\dsound.dll");
        // dsound.dll is deliberately kept loaded (the game loads it for Miles anyway) so no
        // DirectSound-internal thread can outlive its code.
        HMODULE module=LoadLibraryW(path);
        if (!module) { logf(s,"error: dsound.dll load failed (%lu); intros silent",GetLastError()); return; }
        create=reinterpret_cast<DirectSoundCreateFn>(reinterpret_cast<void*>(GetProcAddress(module,"DirectSoundCreate")));
        if (!create) { logf(s,"error: DirectSoundCreate missing; intros silent"); return; }
    }
    IDirectSound* sound=nullptr;
    HRESULT hr=create(nullptr,&sound,nullptr);
    if (FAILED(hr) || !sound) { logf(s,"error: DirectSoundCreate failed (0x%08lx); intros silent",static_cast<unsigned long>(hr)); return; }
    hr=sound->SetCooperativeLevel(s.window,DSSCL_PRIORITY);
    if (FAILED(hr)) {
        logf(s,"error: SetCooperativeLevel failed (0x%08lx); intros silent",static_cast<unsigned long>(hr));
        sound->Release(); return;
    }
    s.sound=sound;
}

void release_sound(Session& s) noexcept {
    // Only after every Bink handle has been closed (BinkClose releases its buffers).
    if (s.sound) { s.sound->Release(); s.sound=nullptr; }
}

// The engine creates its own DirectSound device in S_Init, deep inside Host_Init. That
// creation takes about 1-2 ms while another DirectSound object in the process that has
// played a stream is still alive, and about 0.1-1.0 s when none is (endpoint wake; measured
// by tests/dsound_warmup_probe.cpp and in-game sound_device timings). A held session
// therefore keeps its now buffer-less IDirectSound until main has finished Load (handoff),
// or until release/abort/timeout, instead of dropping it when the user skips all videos
// before S_Init. Only an idle device object is kept: no buffers, no playback, no Bink.
void release_held_sound(Session& s,const char* when) noexcept {
    if (!s.sound) return;
    release_sound(s);
    logf(s,"DirectSound released %s at %.1f ms",when,elapsed_ms(s));
}

void create_timer(Session& s) noexcept {
    s.timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if (!s.timer) s.timer=CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_ALL_ACCESS);
    if (!s.timer) logf(s,"error: waitable timer unavailable (%lu); using message wait timeout",GetLastError());
}

bool create_dib(Session& s,int width,int height) noexcept {
    BITMAPINFO info={};
    info.bmiHeader.biSize=sizeof info.bmiHeader;
    info.bmiHeader.biWidth=width;
    info.bmiHeader.biHeight=-height; // top-down, matches Bink's row order with a positive pitch
    info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;
    info.bmiHeader.biCompression=BI_RGB;
    s.bits=nullptr;
    s.dib=CreateDIBSection(s.memory_dc,&info,DIB_RGB_COLORS,&s.bits,nullptr,0);
    if (!s.dib || !s.bits) {
        logf(s,"error: CreateDIBSection %dx%d failed (%lu)",width,height,GetLastError());
        if (s.dib) DeleteObject(s.dib);
        s.dib=nullptr; s.bits=nullptr; return false;
    }
    s.old_bitmap=SelectObject(s.memory_dc,s.dib);
    s.video_width=width; s.video_height=height;
    return true;
}

void destroy_dib(Session& s) noexcept {
    s.have_frame=false;
    if (!s.dib) return;
    GdiFlush();
    if (s.old_bitmap) SelectObject(s.memory_dc,s.old_bitmap);
    DeleteObject(s.dib);
    s.dib=nullptr; s.bits=nullptr; s.old_bitmap=nullptr;
}

void play(Session& s,const Video& video) noexcept {
    wchar_t wide[MAX_PATH*2];
    const wchar_t* root=s.worker->config.media_root;
    const size_t root_length=wcsnlen(root,MAX_PATH);
    const bool separator=root_length && (root[root_length-1]==L'\\' || root[root_length-1]==L'/');
    if (std::swprintf(wide,MAX_PATH*2,L"%ls%ls%ls",root,separator?L"":L"\\",video.file)<0) { logf(s,"error: media path too long"); return; }
    char path[MAX_PATH*2];
    if (!wide_to_ansi_path(wide,path,MAX_PATH*2)) { logf(s,"error: %ls has no ANSI path form; skipped",video.file); return; }

    std::uint32_t flags=0;
    bool silent=s.sound==nullptr;
    if (!silent && !s.api.set_sound_system(s.api.open_direct_sound,reinterpret_cast<std::uintptr_t>(s.sound))) {
        logf(s,"error: BinkSetSoundSystem refused; %ls plays silent",video.file); silent=true;
    }
    if (silent) {
        static const std::uint32_t no_tracks[1]={0};
        s.api.set_sound_track(0,no_tracks);
        flags|=kBinkSoundTrack;
    }
    const double open_begin=elapsed_ms(s);
    void* bink=s.api.open(path,flags);
    const double opened=elapsed_ms(s);
    if (!bink) {
        const char* error=s.api.get_error();
        logf(s,"error: BinkOpen %ls failed: %s",video.file,error?error:"(none)");
        return;
    }
    ++s.videos_opened;
    auto header=static_cast<const BinkHeader*>(bink);
    const std::uint32_t frames=header->frames;
    const int width=static_cast<int>(header->width),height=static_cast<int>(header->height);
    if (width<=0 || height<=0 || width>kMaxVideoDimension || height>kMaxVideoDimension || !frames) {
        logf(s,"error: %ls has unusable header %dx%d frames=%lu",video.file,width,height,static_cast<unsigned long>(frames));
        s.api.close(bink); return;
    }
    if (!create_dib(s,width,height)) { s.api.close(bink); return; }
    s.fit_4_3=video.fit_4_3; s.need_clear=true; s.have_frame=false;
    logf(s,"playing %ls %dx%d frames=%lu%s at %.1f ms",video.file,width,height,static_cast<unsigned long>(frames),silent?" (silent)":"",elapsed_ms(s));

    std::uint32_t decoded=0,waits=0; bool skipped=false;
    double decode_begin=0,decode_end=0,show_end=0; // first-frame breakdown (bounded probe)
    for (;;) {
        if (pump(s)) skipped=true;
        if (skipped || should_stop(s)) break;
        if (s.api.wait(bink)) { ++waits; wait_short(s); continue; }
        if (!s.first_frame) decode_begin=elapsed_ms(s);
        s.api.do_frame(bink);
        GdiFlush(); // GDI must be done with the DIB before the decoder writes it
        s.api.copy_to_buffer(bink,s.bits,width*4,static_cast<std::uint32_t>(height),0,0,kBinkSurface32);
        s.have_frame=true; ++decoded;
#ifdef ENGINE_FIXES_INTRO_WORKER_TESTING
        if (test_hooks::hooks.frame_decoded) test_hooks::hooks.frame_decoded(s.bits,width,height,header->frame_num,frames);
#endif
        if (!s.first_frame) decode_end=elapsed_ms(s);
        if (!s.shown) {
            // Late-start guard: never show after main gave up and requested abort.
            if (should_stop(s)) break;
            if (!show(s)) { logf(s,"error: intro window could not be shown"); s.stop_all=true; break; }
            s.shown=true;
        }
        if (!s.first_frame) show_end=elapsed_ms(s);
        if (!present_now(s)) { logf(s,"error: intro frame was not presented; stopping playback"); s.stop_all=true; break; }
        if (!s.first_frame) {
            s.first_frame=true;
            SetEvent(s.worker->first_frame);
            const double now=elapsed_ms(s);
            logf(s,"first frame visible at %.1f ms (open %.1f, waits %lu for %.1f, decode+copy %.1f, show %.1f, present %.1f ms)",
                now,opened-open_begin,static_cast<unsigned long>(waits),decode_begin-opened,decode_end-decode_begin,show_end-decode_end,now-show_end);
        }
        if (header->frame_num>=frames) break;
        if (decoded>frames+16u) { logf(s,"error: %ls frame counter did not reach its end; stopping",video.file); break; }
        s.api.next_frame(bink);
    }
    s.api.close(bink);
    destroy_dib(s);
    clear_window(s);
    logf(s,"%ls %s after %lu frames at %.1f ms",video.file,skipped?"skipped":"ended",static_cast<unsigned long>(decoded),elapsed_ms(s));
}

DWORD cover_timeout(const Session& s) noexcept {
    DWORD ms=s.worker->config.cover_timeout_ms;
    if (!ms) ms=kDefaultCoverMs;
    if (ms<kMinCoverMs) ms=kMinCoverMs;
    if (ms>kMaxCoverMs) ms=kMaxCoverMs;
    return ms;
}

// Raises the held window above the game window main is about to show, without activating it,
// and reports once DWM has composed that z-order.
void begin_cover(Session& s) noexcept {
    s.covering.store(true);
    if (!SetWindowPos(s.window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER))
        logf(s,"error: cover SetWindowPos failed (%lu)",GetLastError());
    GdiFlush();
    if (s.dwm_flush) s.dwm_flush(); // at most one composition interval
    SetEvent(s.worker->covering);
    logf(s,"covering game window at %.1f ms",elapsed_ms(s));
}

// The held window (last intro frame cleared to black) stays up until main has shown the game
// window (release) or aborts. With the optional handoff it first becomes a bounded topmost cover.
void hold(Session& s) noexcept {
    HANDLE handles[3]={s.worker->release,s.worker->abort,s.worker->handoff};
    DWORD count=s.worker->handoff?3:2;
    ULONGLONG deadline=0;
    for (;;) {
        DWORD timeout=INFINITE;
        if (deadline) {
            const ULONGLONG now=GetTickCount64();
            timeout=now>=deadline?0:static_cast<DWORD>(deadline-now);
        }
        const DWORD r=MsgWaitForMultipleObjectsEx(count,handles,timeout,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        if (r==WAIT_OBJECT_0) { release_held_sound(s,"on release"); logf(s,"released at %.1f ms",elapsed_ms(s)); return; }
        if (r==WAIT_OBJECT_0+1) { s.aborted=true; release_held_sound(s,"on abort"); logf(s,"aborted while holding at %.1f ms",elapsed_ms(s)); return; }
        if (count==3 && r==WAIT_OBJECT_0+2) {
            count=2; // manual-reset event: stop waiting on it
            begin_cover(s);
            // Main sets handoff only after engine Load returned, so S_Init is complete.
            // Released after `covering` so the cover is never delayed by it.
            release_held_sound(s,"after engine Load (handoff)");
            deadline=GetTickCount64()+cover_timeout(s);
            continue;
        }
        if (r==WAIT_OBJECT_0+count) { pump(s); continue; }
        if (r==WAIT_TIMEOUT) { release_held_sound(s,"on cover timeout"); logf(s,"cover timeout after %lu ms; removing cover without release at %.1f ms",cover_timeout(s),elapsed_ms(s)); return; }
        logf(s,"error: hold wait failed (%lu)",GetLastError());
        return;
    }
}

void run(Session& s) noexcept {
    logf(s,"thread started");
    load_dwm(s);
    const bool skip=s.worker->config.skip_videos;
    bool ready=(skip || load_bink(s)) && create_window(s);
    if (ready) {
        init_sound(s);
        if (skip) {
            // Keep native initialization and the frame-based menu handoff. No video or
            // audio buffer is opened, and abort is checked before showing the cover.
            if (!should_stop(s) && show(s)) {
                s.shown=true;
                if (present_now(s)) {
                    s.first_frame=true;
                    SetEvent(s.worker->first_frame);
                    logf(s,"intro videos disabled; initialization cover visible");
                }
            }
        } else {
            create_timer(s);
            for (const Video& video : kVideos) {
                if (should_stop(s)) break;
                play(s,video);
            }
        }
    }
    // Every Bink handle is closed here; drop binkw32 before videos_done. DirectSound is
    // dropped here too, except by a session that announced first_frame and was not aborted:
    // it keeps the idle device object through the hold (see release_held_sound) so engine
    // S_Init stays fast. Failed sessions are aborted by main for native fallback anyway.
    if (is_set(s.worker->abort)) s.aborted=true;
    if (s.sound && s.shown && s.first_frame && !s.aborted) logf(s,"DirectSound kept idle for engine audio init at %.1f ms",elapsed_ms(s));
    else release_sound(s);
    unload_bink(s);
    if (s.timer) { CloseHandle(s.timer); s.timer=nullptr; }
    if (!s.first_frame) {
        logf(s,"no frame shown (%s, %u videos opened)",s.aborted?"aborted":"failure",s.videos_opened);
        SetEvent(s.worker->failed);
    }
    SetEvent(s.worker->videos_done);
    logf(s,"videos done at %.1f ms%s",elapsed_ms(s),s.worker->close_requested.load()?" (close requested)":"");
    if (s.shown && !s.aborted) hold(s);
    release_held_sound(s,"at worker exit"); // abort raced the hold check, or hold failed
    s.covering.store(false);
    destroy_window(s);
    unload_dwm(s);
    logf(s,"thread exiting at %.1f ms",elapsed_ms(s));
}

unsigned __stdcall thread_main(void* parameter) {
    auto s=static_cast<Session*>(parameter);
    run(*s);
    delete s;
    return 0;
}

void name_thread(HANDLE thread) noexcept {
    using SetDescription=HRESULT(WINAPI*)(HANDLE,PCWSTR);
    HMODULE kernel=GetModuleHandleW(L"kernel32.dll");
    auto set=kernel?reinterpret_cast<SetDescription>(reinterpret_cast<void*>(GetProcAddress(kernel,"SetThreadDescription"))):nullptr;
    if (set) set(thread,L"Engine Fixes intro worker");
}

void close_events(Worker& w) noexcept {
    for (HANDLE* h : {&w.first_frame,&w.failed,&w.videos_done,&w.release,&w.abort,&w.handoff,&w.covering}) {
        if (*h) { CloseHandle(*h); *h=nullptr; }
    }
}

bool valid_config(const Config& c) noexcept {
    const long w=c.screen_rect.right-c.screen_rect.left,h=c.screen_rect.bottom-c.screen_rect.top;
    if (w<=0 || h<=0 || w>32767 || h>32767) { log_plain(c.log,"intro worker: error: invalid screen rect"); return false; }
    if (!c.media_root[0] || wcsnlen(c.media_root,MAX_PATH)>=MAX_PATH) { log_plain(c.log,"intro worker: error: invalid media root"); return false; }
    if (!c.bink_path[0] || wcsnlen(c.bink_path,MAX_PATH)>=MAX_PATH || !absolute_path(c.bink_path)) {
        log_plain(c.log,"intro worker: error: invalid bink path"); return false;
    }
    return true;
}

} // namespace

#ifdef ENGINE_FIXES_INTRO_WORKER_TESTING
namespace test_hooks {
Hooks hooks;
RECT dest_rect(int cw,int ch,int vw,int vh,bool fit_4_3) noexcept { return compute_dest(cw,ch,vw,vh,fit_4_3); }
}
#endif

bool start(Worker& worker,const Config& config) noexcept {
    if (worker.thread || worker.first_frame || worker.failed || worker.videos_done || worker.release || worker.abort ||
        worker.handoff || worker.covering) {
        const bool live=worker.thread && WaitForSingleObject(worker.thread,0)!=WAIT_OBJECT_0;
        log_plain(config.log,live?"intro worker: start refused: previous worker thread still active"
                                 :"intro worker: start refused: previous worker not destroyed");
        return false;
    }
    if (!valid_config(config)) return false;
    worker.config=config;
    worker.config.media_root[MAX_PATH-1]=0;
    worker.config.bink_path[MAX_PATH-1]=0;
    worker.window.store(nullptr);
    worker.close_requested.store(false);
    for (HANDLE* h : {&worker.first_frame,&worker.failed,&worker.videos_done,&worker.release,&worker.abort,&worker.handoff,&worker.covering}) {
        *h=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if (!*h) {
            char text[96]; std::snprintf(text,sizeof text,"intro worker: error: CreateEvent failed (%lu)",GetLastError());
            log_plain(config.log,text);
            close_events(worker); return false;
        }
    }
    auto session=new (std::nothrow) Session;
    if (!session) { log_plain(config.log,"intro worker: error: out of memory"); close_events(worker); return false; }
    session->worker=&worker;
    session->log=config.log;
    QueryPerformanceFrequency(&session->frequency);
    QueryPerformanceCounter(&session->origin);
    // The borrowed game icon is copied now, on the caller's thread, so its lifetime no longer matters.
    if (config.icon) {
        session->icon_big=CopyIcon(config.icon);
        session->icon_small=static_cast<HICON>(CopyImage(config.icon,IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),0));
        if (!session->icon_big || !session->icon_small) log_plain(config.log,"intro worker: error: icon copy failed; continuing without it");
    }
    // _beginthreadex keeps this module loaded until the thread returns.
    unsigned id=0;
    const uintptr_t handle=_beginthreadex(nullptr,0,thread_main,session,0,&id);
    if (!handle) {
        char text[96]; std::snprintf(text,sizeof text,"intro worker: error: thread creation failed (errno %d)",errno);
        log_plain(config.log,text);
        if (session->icon_big) DestroyIcon(session->icon_big);
        if (session->icon_small) DestroyIcon(session->icon_small);
        delete session;
        close_events(worker); return false;
    }
    worker.thread=reinterpret_cast<HANDLE>(handle);
    name_thread(worker.thread);
    return true;
}

void destroy(Worker& worker) noexcept {
    if (worker.thread) {
        const DWORD state=WaitForSingleObject(worker.thread,0);
        if (state!=WAIT_OBJECT_0) {
            log_plain(worker.config.log,"intro worker: destroy refused: thread still active; state retained");
            return;
        }
        CloseHandle(worker.thread);
        worker.thread=nullptr;
    }
    close_events(worker);
    worker.window.store(nullptr);
    // close_requested is intentionally kept so main can still read it after destroy.
}

} // namespace engine_fixes_intro
