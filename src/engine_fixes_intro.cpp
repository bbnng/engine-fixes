// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Startup companion for the UP loader (Bin/loader/engine_fixes_intro.vtm). No BLSE dependency and no engine calls from
// loaded_engine: the executable may not have resolved the engine imports yet.
#include "intro_worker.h"
#include "build_check.h"
#include <shellapi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"shell32.lib")

namespace intro_loader {
using namespace engine_fixes_intro;
using Paint=int(__thiscall*)(void*);
using Load=char(__thiscall*)(void*,unsigned char,const char*,const char*);
using Swap=int(__thiscall*)(void*);
Swap original_swap=nullptr;
bool present_armed=false;
unsigned presents=0;
DWORD main_thread=0;
HMODULE instance=nullptr;
std::uintptr_t base=0;
Paint original_paint=nullptr; Load original_load=nullptr;
Worker worker;
HWND game=nullptr;
bool active=false,attempted=false,waiting_ready=false,load_returned=false,suppressed=false,enabled=false;
bool skip_videos=false;
unsigned char initial_branch=0x75;
wchar_t ini_path[MAX_PATH]={},log_path[MAX_PATH]={},root[MAX_PATH]={};
LARGE_INTEGER frequency={},origin={};
SRWLOCK log_lock=SRWLOCK_INIT;
// Resume address for paint_bridge: engine.dll's paint routine just past the 5 bytes the
// detour replaced. Set by install(); the engine is checked to hold exactly those bytes.
std::uintptr_t paint_resume=0;
// Runs the paint routine's original first instructions (sub esp,14h; push ebx; push ebp),
// then continues inside engine.dll. A fixed stub in this module, so no code is written to
// newly allocated memory.
__declspec(naked) void paint_bridge() {
    __asm {
        sub esp, 0x14
        push ebx
        push ebp
        jmp dword ptr [paint_resume]
    }
}

void log(const char* message) {
    if (!log_path[0]) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    char text[768]; const int n=sprintf_s(text,"%.3f ms %s\r\n",frequency.QuadPart?1000.0*static_cast<double>(now.QuadPart-origin.QuadPart)/frequency.QuadPart:0,message);
    if (n<0) return;
    AcquireSRWLockExclusive(&log_lock);
    HANDLE file=CreateFileW(log_path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file!=INVALID_HANDLE_VALUE) { DWORD written; WriteFile(file,text,static_cast<DWORD>(n),&written,nullptr); CloseHandle(file); }
    ReleaseSRWLockExclusive(&log_lock);
}
bool remove_leaf(wchar_t* path) {
    auto slash=wcsrchr(path,L'\\'); if (!slash) return false; *slash=0; return true;
}
bool paths() {
    wchar_t folder[MAX_PATH]; const DWORD n=GetModuleFileNameW(instance,folder,MAX_PATH);
    if (!n || n>=MAX_PATH || !remove_leaf(folder)) return false;
    if (swprintf_s(ini_path,L"%s\\engine_fixes_intro.ini",folder)<0 || swprintf_s(log_path,L"%s\\engine_fixes_intro.log",folder)<0) return false;
    const DWORD exe_length=GetModuleFileNameW(nullptr,root,MAX_PATH);
    if (!exe_length || exe_length>=MAX_PATH || !remove_leaf(root)) return false;
    return true;
}
bool write_bytes(void* address,const void* bytes,size_t size) {
    DWORD old; if(!VirtualProtect(address,size,PAGE_EXECUTE_READWRITE,&old)) return false;
    std::memcpy(address,bytes,size); DWORD ignored;
    const BOOL restored=VirtualProtect(address,size,old,&ignored); FlushInstructionCache(GetCurrentProcess(),address,size);
    // Once bytes are committed the patch is live. Do not report failure and
    // free its trampoline merely because restoring protection failed.
    if(!restored) log("warning: patched memory protection could not be restored");
    return true;
}
void jump(unsigned char* bytes,std::uintptr_t where,const void* target) {
    bytes[0]=0xe9; const auto delta=static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(target)-where-5); std::memcpy(bytes+1,&delta,4);
}
bool windowed(void* mode) {
    __try { using Fn=bool(__thiscall*)(void*); return reinterpret_cast<Fn>((*static_cast<void***>(mode))[7])(mode); }
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
HWND game_window() {
    __try {
        auto object=*reinterpret_cast<void**>(base+0x1ad5a8);
        using Fn=HWND(__thiscall*)(void*);
        return object?reinterpret_cast<Fn>((*static_cast<void***>(object))[5])(object):nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return nullptr;}
}
bool developer() {
    __try {
        auto var=*reinterpret_cast<unsigned char**>(base+0xb426c4);
        if(!var) return true;
        using Fn=bool(__thiscall*)(void*);
        return !reinterpret_cast<Fn>((*reinterpret_cast<void***>(var))[1])(var) && *reinterpret_cast<int*>(var+0x2c)!=0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return true;}
}
bool argument(const wchar_t* wanted) {
    int count=0; auto args=CommandLineToArgvW(GetCommandLineW(),&count); bool found=false;
    if(args) {for(int i=1;i<count;++i) if(!_wcsicmp(args[i],wanted)) found=true; LocalFree(args);} return found;
}
bool mod_directory(wchar_t (&out)[MAX_PATH]) {
    wcscpy_s(out,L"Vampire"); int count=0;
    auto args=CommandLineToArgvW(GetCommandLineW(),&count);
    if(args) {
        for(int i=1;i+1<count;++i) if(!_wcsicmp(args[i],L"-game")) {
            if(wcslen(args[i+1])>=MAX_PATH) {LocalFree(args);return false;}
            wcscpy_s(out,args[i+1]);break;
        }
        LocalFree(args);
    }
    wchar_t candidate[MAX_PATH],resolved[MAX_PATH],prefix[MAX_PATH];
    if(swprintf_s(candidate,L"%s\\%s",root,out)<0 || swprintf_s(prefix,L"%s\\",root)<0) return false;
    const DWORD n=GetFullPathNameW(candidate,MAX_PATH,resolved,nullptr);
    if(!n || n>=MAX_PATH || _wcsnicmp(resolved,prefix,wcslen(prefix))) return false;
    return wcscpy_s(out,resolved)==0;
}
RECT intro_rect(const RECT& client,const RECT& window,const RECT& monitor) {
    // Early native sizing still includes a caption even when a later mod turns
    // the game borderless. A monitor-sized window needs a monitor-sized cover.
    if(window.right-window.left>=monitor.right-monitor.left &&
       window.bottom-window.top>=monitor.bottom-monitor.top) return monitor;
    return client;
}
void disable_transitions(HWND window) {
    auto module=LoadLibraryExW(L"dwmapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module)return;
    using Set=HRESULT(WINAPI*)(HWND,DWORD,LPCVOID,DWORD);
    auto set=reinterpret_cast<Set>(reinterpret_cast<void*>(GetProcAddress(module,"DwmSetWindowAttribute")));
    const BOOL value=TRUE;
    if(set && FAILED(set(window,3,&value,sizeof value))) log("native window transition suppression unavailable");
    FreeLibrary(module);
}
bool config(Config& cfg) {
    cfg={}; cfg.game_window=game; cfg.log=log; cfg.skip_videos=skip_videos;
    if(!IsWindow(game) || !GetClientRect(game,&cfg.screen_rect)) return false;
    POINT corner={0,0}; if(!ClientToScreen(game,&corner)) return false;
    OffsetRect(&cfg.screen_rect,corner.x,corner.y);
    MONITORINFO monitor={sizeof monitor}; RECT outer;
    if(GetWindowRect(game,&outer) && GetMonitorInfoW(MonitorFromWindow(game,MONITOR_DEFAULTTONEAREST),&monitor))
        cfg.screen_rect=intro_rect(cfg.screen_rect,outer,monitor.rcMonitor);
    if(cfg.screen_rect.right<=cfg.screen_rect.left || cfg.screen_rect.bottom<=cfg.screen_rect.top) return false;
    cfg.icon=reinterpret_cast<HICON>(GetClassLongPtrW(game,GCLP_HICON));
    cfg.cover_timeout_ms=3000;
    wchar_t mod[MAX_PATH],media[MAX_PATH]; if(!mod_directory(mod)) return false;
    if(swprintf_s(media,L"%s\\media",mod)<0) return false;
    if(GetFileAttributesW(media)!=INVALID_FILE_ATTRIBUTES) wcscpy_s(cfg.media_root,media);
    else if(swprintf_s(cfg.media_root,L"%s\\Vampire\\media",root)<0) return false;
    // Loader installs vary: use the same local library already loaded by the
    // game if available, otherwise only its known Bin/root locations.
    auto bink=GetModuleHandleW(L"binkw32.dll");
    if(bink) {const DWORD n=GetModuleFileNameW(bink,cfg.bink_path,MAX_PATH); if(!n || n>=MAX_PATH)return false;}
    else {
        if(swprintf_s(cfg.bink_path,L"%s\\Bin\\binkw32.dll",root)<0) return false;
        if(GetFileAttributesW(cfg.bink_path)==INVALID_FILE_ATTRIBUTES && swprintf_s(cfg.bink_path,L"%s\\binkw32.dll",root)<0) return false;
    }
    return true;
}
bool suppress() {
    auto branch=reinterpret_cast<unsigned char*>(base+0xfb515);
    if(branch[1]!=0x44 || (branch[0]!=0x75 && branch[0]!=0xeb)) return false;
    const unsigned char skip=0xeb;
    if(branch[0]!=skip && !write_bytes(branch,&skip,1)) return false;
    suppressed=true; return true;
}
bool thread_exited() { return !worker.thread || WaitForSingleObject(worker.thread,0)==WAIT_OBJECT_0; }
void show_game() {
    if(IsWindow(game)) {
        disable_transitions(game);
        ShowWindow(game,SW_SHOW);
        SetForegroundWindow(game); log("native game window shown without transition animation");
    }
}
int __fastcall swap(void* self,void*) {
    const int result=original_swap(self);
    if(GetCurrentThreadId()==main_thread && present_armed) {
        if(++presents>=2 || worker.close_requested.load()) {
            present_armed=false;
            if(worker.release)SetEvent(worker.release);
            if(worker.close_requested.load())PostMessageW(game,WM_CLOSE,0,0);
            log("native frames presented; intro cover released");
        }
    }
    return result;
}
bool install_present_slot(std::uintptr_t material) {
    auto slot=reinterpret_cast<void**>(material+0x498b0);
    if(*slot==reinterpret_cast<void*>(swap))return original_swap!=nullptr;
    if(*slot!=reinterpret_cast<void*>(material+0xea10))return false;
    auto previous=reinterpret_cast<Swap>(*slot);
    void* replacement=reinterpret_cast<void*>(swap);
    original_swap=previous;
    if(!write_bytes(slot,&replacement,sizeof replacement))return false;
    return true;
}
bool install_present() {
    auto material=GetModuleHandleW(L"materialsystem.dll");
    if(!material || !build_check::known(material,build_check::material_profile,log))return false;
    return install_present_slot(reinterpret_cast<std::uintptr_t>(material));
}
void stop_worker(bool abort) {
    if(abort && worker.abort) SetEvent(worker.abort);
    else if(worker.release) SetEvent(worker.release);
    if(worker.thread && WaitForSingleObject(worker.thread,1500)!=WAIT_OBJECT_0) {log("worker still running; retained safely, no termination/unload");return;}
    destroy(worker);
}
char __fastcall load(void*,void*,unsigned char,const char*,const char*);
DWORD wait_ready(DWORD timeout) {
    HANDLE handles[]={worker.first_frame,worker.failed,worker.thread};
    const ULONGLONG deadline=GetTickCount64()+timeout;
    for(;;) {
        const auto now=GetTickCount64();
        const DWORD remaining=now>=deadline?0:static_cast<DWORD>(deadline-now);
        const DWORD state=MsgWaitForMultipleObjects(3,handles,FALSE,remaining,QS_SENDMESSAGE);
        if(state!=WAIT_OBJECT_0+3) return state;
        // Services synchronous cross-thread activation/window messages. Do not
        // dispatch posted game input/commands before engine Init is complete.
        MSG message; PeekMessageW(&message,nullptr,WM_NULL,WM_NULL,PM_NOREMOVE);
    }
}
bool begin_handoff() {
    if(!worker.handoff || !worker.covering || !worker.thread || !install_present()) {
        log("present handoff unavailable; immediate native-window fallback"); return false;
    }
    SetEvent(worker.handoff);
    const ULONGLONG deadline=GetTickCount64()+250;
    HANDLE handles[]={worker.covering,worker.thread};
    for(;;) {
        const auto now=GetTickCount64();
        const DWORD remaining=now>=deadline?0:static_cast<DWORD>(deadline-now);
        const DWORD state=MsgWaitForMultipleObjects(2,handles,FALSE,remaining,QS_SENDMESSAGE);
        if(state==WAIT_OBJECT_0) {
            presents=0;present_armed=true;
            log("intro cover ready; returning to native frame loop");return true;
        }
        if(state!=WAIT_OBJECT_0+2)return false;
        MSG message;PeekMessageW(&message,nullptr,WM_NULL,WM_NULL,PM_NOREMOVE);
    }
}
int __fastcall paint(void* self,void*) {
    // Native flags and all native sizing/init still execute. Only the explicit
    // SWP_SHOWWINDOW bit is temporarily omitted during this startup instance.
    if(!enabled || load_returned || !windowed(self) || argument(L"-dev") || (!skip_videos && initial_branch==0xeb && GetPrivateProfileIntW(L"IntroFirst",L"honor_novid",1,ini_path))) return original_paint(self);
    if(*reinterpret_cast<void**>(base+0x186b80)!=reinterpret_cast<void*>(load)) {
        log("Load handoff hook replaced; leaving native startup visible"); return original_paint(self);
    }
    auto flag=reinterpret_cast<unsigned char*>(base+0xfcc88);
    const unsigned char hidden=0x25,visible=0x65;
    if(*flag!=visible || !write_bytes(flag,&hidden,1)) return original_paint(self);
    const int result=original_paint(self);
    if(!write_bytes(flag,&visible,1)) log("could not restore native show flag");
    game=game_window();
    if(attempted) {if(!active && !waiting_ready) show_game();return result;}
    attempted=true; log("native sizing complete; gray paint kept hidden");
    Config cfg;
    if(!config(cfg) || !start(worker,cfg)) {show_game();log("worker unavailable; native intro path preserved");return result;}
    waiting_ready=true;
    const DWORD wait=wait_ready(1500);
    waiting_ready=false;
    if(wait==WAIT_OBJECT_0 && suppress()) {active=true;log("first intro frame ready; native late intro block suppressed");return result;}
    log("first intro frame unavailable; aborting worker before fallback");
    stop_worker(true);
    if(!thread_exited()) {suppress();log("native intros suppressed to avoid concurrent Bink use with stalled worker");}
    show_game(); return result;
}
char __fastcall load(void* self,void*,unsigned char dedicated,const char* game_path,const char* base_path) {
    const char result=original_load(self,dedicated,game_path,base_path);
    load_returned=true; log("native engine Load returned");
    if(!active) return result;
    if(!result || developer()) {if(result)show_game();stop_worker(true);active=false;return result;}
    log("waiting for intros with initialization complete");
    const ULONGLONG deadline=GetTickCount64()+120000;
    bool quit=false,close_sent=false;
    if(worker.close_requested.load()) {close_sent=true;PostMessageW(game,WM_CLOSE,0,0);if(worker.abort)SetEvent(worker.abort);}
    while(worker.videos_done && WaitForSingleObject(worker.videos_done,0)!=WAIT_OBJECT_0) {
        if(worker.close_requested.load() && !close_sent) {close_sent=true;PostMessageW(game,WM_CLOSE,0,0); if(worker.abort)SetEvent(worker.abort);}
        if(GetTickCount64()>=deadline) {log("intro completion deadline exceeded; aborting");if(worker.abort)SetEvent(worker.abort);break;}
        HANDLE events[]={worker.videos_done,worker.thread};
        const DWORD state=MsgWaitForMultipleObjects(2,events,FALSE,100,QS_ALLINPUT);
        if(state==WAIT_OBJECT_0 || state==WAIT_OBJECT_0+1) break;
        if(state==WAIT_FAILED) {if(worker.abort)SetEvent(worker.abort);break;}
        MSG message;
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT) {quit=true;PostQuitMessage(static_cast<int>(message.wParam));break;}
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if(quit) {if(worker.abort)SetEvent(worker.abort);break;}
    }
    if(!quit && !close_sent) {
        const bool covered=begin_handoff();
        show_game();
        if(covered) {active=false;return result;}
    }
    stop_worker(quit||close_sent); active=false;
    return result;
}
bool install(HMODULE engine) {
    base=reinterpret_cast<std::uintptr_t>(engine);
    auto entry=reinterpret_cast<unsigned char*>(base+0xfcba0);
    auto slot=reinterpret_cast<void**>(base+0x186b80);
    auto branch=reinterpret_cast<unsigned char*>(base+0xfb515);
    const unsigned char expected[]={0x83,0xec,0x14,0x53,0x55};
    if(std::memcmp(entry,expected,5) || *slot!=reinterpret_cast<void*>(base+0xfc0c0) ||
       *reinterpret_cast<unsigned char*>(base+0xfcc87)!=0x6a || *reinterpret_cast<unsigned char*>(base+0xfcc88)!=0x65 ||
       (branch[0]!=0x75 && branch[0]!=0xeb) || branch[1]!=0x44) return false;
    initial_branch=branch[0];
    const std::uintptr_t previous_resume=paint_resume; paint_resume=reinterpret_cast<std::uintptr_t>(entry+5);
    unsigned char detour[5]; jump(detour,reinterpret_cast<std::uintptr_t>(entry),reinterpret_cast<void*>(paint));
    auto old_load=*slot; void* new_load=reinterpret_cast<void*>(load);
    if(!write_bytes(slot,&new_load,sizeof new_load)) {paint_resume=previous_resume;return false;}
    original_paint=reinterpret_cast<Paint>(reinterpret_cast<void*>(paint_bridge)); original_load=reinterpret_cast<Load>(old_load);
    if(!write_bytes(entry,detour,5)) {
        write_bytes(slot,&old_load,sizeof old_load); paint_resume=previous_resume; return false;
    }
    return true;
}
}

extern "C" __declspec(dllexport) void __cdecl loaded_engine() {
    using namespace intro_loader;
    if(!ini_path[0] && !paths())return;
    if(!frequency.QuadPart) {
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&origin);
        // One log per game session: the previous session's log is kept as engine_fixes_intro.log.old.
        wchar_t old_path[MAX_PATH];
        if(swprintf_s(old_path,L"%s.old",log_path)>0) MoveFileExW(log_path,old_path,MOVEFILE_REPLACE_EXISTING);
    }
    // Raw QueryPerformanceCounter origin of this log's clock, so its lines can be aligned
    // exactly with engine_fixes' startup report (trace_origin_qpc, same system-wide counter).
    char anchor[160]; sprintf_s(anchor,"loaded_engine: Engine Fixes and Tweaks intro companion 1.2.1 (qpc_origin=%lld qpc_frequency=%lld)",origin.QuadPart,frequency.QuadPart);
    log(anchor);
    auto engine=GetModuleHandleW(L"engine.dll");
    if(enabled && base==reinterpret_cast<std::uintptr_t>(engine)) {
        auto entry=reinterpret_cast<const unsigned char*>(base+0xfcba0); std::int32_t displacement;
        std::memcpy(&displacement,entry+1,4);
        if(entry[0]==0xe9 && reinterpret_cast<std::uintptr_t>(entry+5)+displacement==reinterpret_cast<std::uintptr_t>(paint) &&
           *reinterpret_cast<void**>(base+0x186b80)==reinterpret_cast<void*>(load)) {log("same engine instance already hooked");return;}
    }
    if(!thread_exited()) {log("previous worker remains active; declining this engine instance");return;}
    destroy(worker); active=attempted=waiting_ready=load_returned=suppressed=enabled=skip_videos=false; game=nullptr;
    present_armed=false;presents=0;main_thread=GetCurrentThreadId();
    if(!GetPrivateProfileIntW(L"IntroFirst",L"enabled",0,ini_path)) {log("disabled in companion INI");return;}
    // The analyzed engine.dll or the same build edited in place by a newer Unofficial Patch
    // (build_check.h); install() and suppress() still check every byte they change.
    if(!engine || !build_check::known(engine,build_check::engine_profile,log)) {log("unsupported engine; unchanged");return;}
    enabled=install(engine); log(enabled?"early hooks installed":"early hook check failed; unchanged");
    if(enabled && GetPrivateProfileIntW(L"IntroFirst",L"skip_videos",0,ini_path)) {
        skip_videos=suppress();
        log(skip_videos?"intro videos disabled in companion INI":"intro skip check failed; retaining video startup");
    }
}
#ifndef ENGINE_FIXES_INTRO_TEST
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void*) {if(reason==DLL_PROCESS_ATTACH)intro_loader::instance=module;return TRUE;}
#endif
