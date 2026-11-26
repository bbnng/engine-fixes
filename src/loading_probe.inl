// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Opt-in loading profiler ([Loading] loading_trace=1; off when missing). Port of the 0.10.1
// research probe to the current plugin (2026-11-14). Read-only timing: no loading, save,
// Python, wait or gameplay behavior is changed. Included inside engine_fixes.cpp's unnamed
// namespace AFTER restart_startup.inl (uses host, supported(), server_profile and
// build_check.h). Timed hooks do no heap allocation and no logging; the
// main-thread task writes bounded summaries outside timed scopes.
//
// The UP12 Vampire.exe loader replaced the old loading-pause.vtm: its "Loading Wait For Enter"
// rewrites the engine call at engine+0x8e4f3 (native target engine+0x25420, which needs
// signon state 3/3 and then sets state 4) to Vampire.exe+0x7900. While it waits for
// Enter/Space/click it holds byte Vampire.exe+0x3f490 at 1 (set at +0x7adf, cleared at
// +0x7cd0) and calls engine+0x86fe0 plus Vampire.exe's own Sleep(16). The loader byte-checks
// engine 0x25420/0x25430/0x86fe0/0x1b800 when client.dll loads, so none of those, nor the
// call site, is hooked here. Instead a passive sampler thread reads the two engine signon
// words and that loader byte (the three loader instructions that use it verified).
namespace loading {
using Tick=std::uint64_t;
enum MetricId { transition, map_load, extract, spawn, server_init, server_save, shutdown,
    model, bsp, progress, present, fs_open, fs_close, fs_read, fs_write, disk_open,
    inflate, compress, continue_wait, model_links, model_upload, client_pre, client_post, client_dll_init, engine_sleep, metric_count };
const char* const metric_names[]={"transition","map_load","save_extract","spawn_server","server_init",
    "server_save","server_shutdown","model_load","bsp_build","progress","swap_buffers","fs_open",
    "fs_close","fs_read","fs_write","disk_open","uncompress","compress2","loader_wait",
    "model_dependencies","model_render_setup","client_pre_entity","client_post_entity","client_dll_init","engine_sleep"};
enum Kind { startup, saved_game, area_change, map_command };
const char* const kinds[]={"startup","save_load","transition","map_load"};
enum LoaderWait { wait_unavailable, wait_armed, wait_off, wait_legacy };
const char* const loader_wait_names[]={"unavailable","armed","off","legacy_plugin_unseparated"};
struct Metric { Tick calls=0,total=0,self=0,maximum=0,bytes=0,failed=0; };
struct Bucket { Tick read=0,written=0,opens=0,misses=0,disk_attempts=0,disk_misses=0,disk_ticks=0; };
const char* const bucket_names[]={"models","materials","sound","maps","save_state","config","other","untracked"};
struct Slow { Tick ticks=0; char name[96]={}; };
struct SleepSite { std::uintptr_t rva=0; Tick calls=0,requested_ms=0,ticks=0; };
struct Endpoints { Tick ready=0,active=0,wait_begin=0,wait_end=0,max_gap=0,samples=0; unsigned mask=0; };
struct Session {
    unsigned id=0; Kind kind=startup; char name[96]={},map[96]={},status[48]={};
    Tick begin=0,end=0,native_enter=0,first_sim=0,ui_closed=0,wait_scoped=0;
    Tick model_entries=0,model_loaded_flag=0,model_studio=0;
    Metric metrics[metric_count]; Bucket buckets[8]; Slow slow[5];
    SleepSite sleeps[8]; unsigned sleep_site_overflow=0;
    Endpoints endpoints; LoaderWait loader=wait_unavailable;
    unsigned overflows=0,unbalanced=0,handle_overflows=0,coverage=0,coverage_start=0;
    bool level=false,menu=false;
};
struct Stack { Tick begin=0,child=0; MetricId metric=transition; };
// Only the first loads of a session are written to the log. Later ones are still timed, because the
// speedups that run only inside a timed load (loose_stat, model_names) depend on it.
constexpr unsigned logged_captures=32;
// Optional extension: record at capture end, log only alongside the finished capture.
using SessionObserver=void(*)(const Session&);
SessionObserver session_finished=nullptr,session_reported=nullptr;
struct Handle { void* value=nullptr; unsigned bucket=7; };
Session current,queue[4]; Stack stack[64]; Handle handles[512];
unsigned depth=0,epoch=0,captures=0,queued=0,dropped=0,coverage=0;
Tick frequency=0,process_age_ms=0,install_ticks=0;
bool attempted=false,enabled_probe=false,active=false,late_fs=false,late_mat=false,late_server=false,late_client=false;
bool menu_visible=false,reporting=false,task_queued=false,restarted=false;
DWORD main_thread=0;
std::uintptr_t engine_base=0,exe_base=0,fs_base=0,material_base=0;
struct UiTail { unsigned id=0; Tick start=0,ready=0,closed=0; bool pending=false; } ui;
std::uint64_t subscriptions[8]={}; unsigned subscription_count=0;
#ifdef ENGINE_FIXES_LOADING_TEST
volatile LONGLONG test_tick=0;
Tick clock_now() { return static_cast<Tick>(InterlockedCompareExchange64(&test_tick,0,0)); }
#else
Tick clock_now() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return static_cast<Tick>(n.QuadPart); }
#endif
double ms(Tick ticks) { return frequency?1000.0*static_cast<double>(ticks)/static_cast<double>(frequency):0; }
bool collecting() { return GetCurrentThreadId()==main_thread && enabled_probe && active && !reporting; }
void copy_name(char* to,size_t capacity,const char* from,size_t available=static_cast<size_t>(-1)) {
    if (!capacity) return; to[0]=0;
    __try {
        size_t i=0;
        if (from) for (;i+1<capacity && i<available && from[i];++i) {
            const unsigned char c=static_cast<unsigned char>(from[i]);
            to[i]=(c<32 || c==127)?'_':static_cast<char>(c);
        }
        to[i]=0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { to[0]=0; }
}

// --- passive endpoint sampler (separate thread; reads only, writes only its own results) ---
namespace sampler {
enum Bits : LONG { ready_bit=1, active_bit=2, wait_begin_bit=4, wait_end_bit=8, low_bit=16 };
const volatile int* signon=nullptr;          // engine+0x1dbe50 (3 = signed on, 4 = active)
const volatile int* client_state=nullptr;    // engine+0x20b658 (3 = signed on)
const volatile unsigned char* wait_flag=nullptr; // Vampire.exe+0x3f490 (verified loader only)
volatile LONG armed=0,result_id=0,mask=0,stop_requested=0;
volatile LONGLONG ready=0,active_tick=0,wait_begin=0,wait_end=0,max_gap=0,samples=0;
LONG local_id=0; Tick last=0;
HANDLE thread=nullptr,wake=nullptr;
void store(volatile LONGLONG& field,Tick value) { InterlockedExchange64(&field,static_cast<LONGLONG>(value)); }
Tick load(volatile LONGLONG& field) { return static_cast<Tick>(InterlockedCompareExchange64(&field,0,0)); }
void publish(LONG bit,volatile LONGLONG& field,Tick now) { store(field,now); InterlockedOr(&mask,bit); }
bool read_state(int& a,int& b) {
    __try { if (!signon || !client_state) return false; a=*signon; b=*client_state; return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int read_flag() {
    __try { return wait_flag?*wait_flag:-1; } __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
void sample(Tick now) {
    const LONG id=InterlockedCompareExchange(&armed,0,0);
    if (!id) { local_id=0; last=0; return; }
    if (id!=local_id) {
        InterlockedExchange(&result_id,0); // invalidate the old generation before clearing its fields
        InterlockedExchange(&mask,0);
        store(ready,0); store(active_tick,0); store(wait_begin,0); store(wait_end,0); store(max_gap,0); store(samples,0);
        InterlockedExchange(&result_id,id); local_id=id; last=0;
    }
    if (last && now-last>load(max_gap)) store(max_gap,now-last);
    int a=0,b=0; const bool state=read_state(a,b);
    const LONG m=InterlockedCompareExchange(&mask,0,0);
    if (state && (a<3 || b<3) && !(m&low_bit)) InterlockedOr(&mask,low_bit);
    if (state && a==3 && b==3 && !(m&ready_bit)) publish(ready_bit,ready,now);
    // State 4 counts only after this load was seen below it or at 3/3: a stale state 4
    // from the previous level at arming time is not completion.
    if (state && a==4 && (m&(ready_bit|low_bit)) && !(m&active_bit)) publish(active_bit,active_tick,now);
    const int flag=read_flag();
    if (flag>0 && !(m&wait_begin_bit)) publish(wait_begin_bit,wait_begin,now);
    if (flag==0 && (m&wait_begin_bit) && !(m&wait_end_bit)) publish(wait_end_bit,wait_end,now);
    store(samples,load(samples)+1); last=now;
}
DWORD WINAPI run(void*) {
    while (!InterlockedCompareExchange(&stop_requested,0,0)) {
        const bool busy=InterlockedCompareExchange(&armed,0,0)!=0;
        WaitForSingleObject(wake,busy?1:INFINITE);
        if (InterlockedCompareExchange(&stop_requested,0,0)) break;
        sample(clock_now());
    }
    return 0;
}
bool start() {
    if (thread) return true;
    InterlockedExchange(&stop_requested,0);
    wake=CreateEventW(nullptr,FALSE,FALSE,nullptr); if (!wake) return false;
    thread=CreateThread(nullptr,64*1024,run,nullptr,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
    if (!thread) { CloseHandle(wake); wake=nullptr; return false; }
    return true;
}
bool stop(DWORD timeout) {
    InterlockedExchange(&armed,0);
    if (!thread) return true;
    InterlockedExchange(&stop_requested,1); SetEvent(wake);
    if (WaitForSingleObject(thread,timeout)!=WAIT_OBJECT_0) return false; // retained, never terminated
    CloseHandle(thread); CloseHandle(wake); thread=nullptr; wake=nullptr; return true;
}
void arm(unsigned id) { InterlockedExchange(&armed,static_cast<LONG>(id)); if (wake) SetEvent(wake); }
void disarm() { InterlockedExchange(&armed,0); }
bool pull(unsigned id,Endpoints& out) {
    if (!id || InterlockedCompareExchange(&result_id,0,0)!=static_cast<LONG>(id)) return false;
    Endpoints e; e.mask=static_cast<unsigned>(InterlockedCompareExchange(&mask,0,0));
    e.ready=load(ready); e.active=load(active_tick); e.wait_begin=load(wait_begin); e.wait_end=load(wait_end);
    e.max_gap=load(max_gap); e.samples=load(samples);
    if (InterlockedCompareExchange(&result_id,0,0)!=static_cast<LONG>(id)) return false;
    // Fields are written before their mask bit; a bit seen after the read means the value may
    // be newer than this snapshot, so keep only fields whose bit was already set.
    if (!(e.mask&ready_bit)) e.ready=0;
    if (!(e.mask&active_bit)) e.active=0;
    if (!(e.mask&wait_begin_bit)) e.wait_begin=0;
    if (!(e.mask&wait_end_bit)) e.wait_end=0;
    out=e; return true;
}
}
void refresh_endpoints() { if (active && current.kind!=startup) sampler::pull(current.id,current.endpoints); }
bool loader_waiting() {
    __try { return sampler::wait_flag && *sampler::wait_flag; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void schedule(bool flush=false);
void finish(const char* status,Tick now,bool abandon=false) {
    if (!active || (depth && !abandon)) return;
    refresh_endpoints(); sampler::disarm();
    current.end=now; current.coverage=coverage; copy_name(current.status,sizeof current.status,status);
    if (abandon) { current.unbalanced+=depth; depth=0; ++epoch; }
    // The loader wait is measured directly; scopes that ran inside it are already counted.
    const auto& e=current.endpoints;
    if (e.wait_begin) {
        const Tick until=e.wait_end?e.wait_end:now; const Tick wait=until>e.wait_begin?until-e.wait_begin:0;
        auto& m=current.metrics[continue_wait]; m.calls=1; m.total=wait; m.maximum=wait;
        m.self=wait>current.wait_scoped?wait-current.wait_scoped:0; m.failed=e.wait_end?0:1;
    }
    if (session_finished) session_finished(current);
    if (current.id<=logged_captures) { if (queued<4) queue[queued++]=current; else ++dropped; }
    if (current.kind!=startup && current.id<=logged_captures) ui={current.id,current.begin,current.endpoints.ready,current.ui_closed,current.ui_closed!=0};
    active=false; schedule();
}
void begin(Kind kind,const char* name,Tick now) {
    if (!enabled_probe) return;
    if (active) finish("interrupted",now,true);
    sampler::disarm();
    current={}; current.id=++captures; current.kind=kind; current.begin=now; current.coverage_start=coverage;
    copy_name(current.name,sizeof current.name,name); active=true; depth=0; ++epoch; schedule();
}
void ensure(Kind kind,const char* name) {
    if (GetCurrentThreadId()!=main_thread || !enabled_probe || reporting) return;
    if (active && current.kind==startup) finish("startup_interrupted",clock_now(),true);
    if (!active) begin(kind,name,clock_now());
    if (active && !current.native_enter) current.native_enter=clock_now();
}
class Scope {
    unsigned generation_=0,slot_=0; bool live_=false;
public:
    explicit Scope(MetricId metric) {
        if (!collecting()) return;
        if (depth==64) { ++current.overflows; return; }
        generation_=epoch; slot_=depth++; live_=true; stack[slot_]={clock_now(),0,metric};
    }
    Tick stop(Tick bytes=0,bool failed=false) {
        if (!live_) return 0; live_=false;
        if (!active || generation_!=epoch || depth!=slot_+1) return 0;
        const auto entry=stack[slot_]; const Tick elapsed=clock_now()-entry.begin;
        --depth; if (depth) stack[depth-1].child+=elapsed; else if (loader_waiting()) current.wait_scoped+=elapsed;
        auto& m=current.metrics[entry.metric]; ++m.calls; m.total+=elapsed;
        m.self+=elapsed>=entry.child?elapsed-entry.child:0;
        if (elapsed>m.maximum) m.maximum=elapsed;
        m.bytes+=bytes; m.failed+=failed?1:0; return elapsed;
    }
    ~Scope() { stop(); }
    Scope(const Scope&)=delete; Scope& operator=(const Scope&)=delete;
};
unsigned bucket(const char* name) {
    __try {
        if (!name) return 6;
        const char* extension=nullptr;
        unsigned i=0;
        for (;i<512 && name[i];++i) {
            if (name[i]=='.') extension=name+i;
            if (name[i]=='/' || name[i]=='\\') extension=nullptr;
        }
        if (i==512 || !extension) return 6;
        if (!_stricmp(extension,".mdl") || !_stricmp(extension,".vtx") || !_stricmp(extension,".vvd")) return 0;
        if (!_stricmp(extension,".vmt") || !_stricmp(extension,".vtf")) return 1;
        if (!_stricmp(extension,".wav") || !_stricmp(extension,".mp3")) return 2;
        if (!_stricmp(extension,".bsp") || !_stricmp(extension,".ain")) return 3;
        if (!_stricmp(extension,".sav") || !_stricmp(extension,".hl1") || !_stricmp(extension,".hl2") || !_stricmp(extension,".hl3")) return 4;
        if (!_stricmp(extension,".cfg") || !_stricmp(extension,".ini")) return 5;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return 6;
}
unsigned handle_bucket(void* handle,bool remove=false) {
    const unsigned at=(reinterpret_cast<std::uintptr_t>(handle)>>4)&511;
    for (unsigned i=0;i<8;++i) {
        auto& h=handles[(at+i)&511];
        if (h.value && h.value==handle) { const auto b=h.bucket; if (remove) h={}; return b; }
    }
    return 7;
}
void remember(void* handle,unsigned kind) {
    if (!handle) return;
    const unsigned at=(reinterpret_cast<std::uintptr_t>(handle)>>4)&511; int free=-1;
    for (unsigned i=0;i<8;++i) {
        auto& h=handles[(at+i)&511];
        if (h.value==handle) { h.bucket=kind; return; }
        if (!h.value && free<0) free=static_cast<int>((at+i)&511);
    }
    if (free>=0) handles[free]={handle,kind}; else if (collecting()) ++current.handle_overflows;
}
void slow_model(const char* name,Tick ticks) {
    if (!ticks || !active) return;
    for (unsigned i=0;i<5;++i) if (ticks>current.slow[i].ticks) {
        for (unsigned k=4;k>i;--k) current.slow[k]=current.slow[k-1];
        current.slow[i].ticks=ticks; copy_name(current.slow[i].name,sizeof current.slow[i].name,name); break;
    }
}
void model_entry(const void* record) {
    if (!collecting()) return;
    __try {
        const auto fields=static_cast<const unsigned*>(record);
        ++current.model_entries;
        if (fields[33]&1) ++current.model_loaded_flag;
        if (fields[34]==3) ++current.model_studio;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// Every game-function wrapper returns the callee's edx:eax exactly as the callee left them,
// whatever the C++/SDK declaration says. Callers read more than those declarations show:
// vampire.dll+0x11b080 (server interface slot 18, CServerGameDLL::Save, void in the SDK and in
// BLSE) ends with mov eax,1, and engine SaveGameState (engine+0x964c2: test eax,eax) fails the
// save when eax is 0. Build 1.1.0.10 declared it void and returned Scope::stop()'s value
// instead, which is 0 outside a timed load, so every quicksave/manual save/autosave showed
// ID_SaveFailed_0. Narrower declarations (char/bool) would likewise leave stale upper bits.
// Stack cleanup per wrapper matches each native's ret N (rechecked 2026-11-14).
using Raw=std::uint64_t;
std::uint32_t low(Raw value) { return static_cast<std::uint32_t>(value); }
using Transition=Raw(__cdecl*)(int,const char*,const char*);
using MapLoad=Raw(__cdecl*)(const char*,int);
using Extract=Raw(__thiscall*)(void*,const char*);
using Spawn=Raw(__cdecl*)(const char*,const char*);
using Model=Raw(__thiscall*)(void*,void*,const void*);
using Bsp=Raw(__thiscall*)(void*,void*);
using Progress=Raw(__cdecl*)();
using Present=Raw(__thiscall*)(void*);
using Open=Raw(__thiscall*)(void*,const char*,const char*,const char*);
using Close=Raw(__thiscall*)(void*,void*);
using Read=Raw(__thiscall*)(void*,void*,int,void*);
using Write=Raw(__thiscall*)(void*,const void*,int,void*);
using DiskOpen=Raw(__thiscall*)(void*,const char*,const char*);
using Inflate=Raw(__cdecl*)(unsigned char*,unsigned long*,const unsigned char*,unsigned long);
using Compress=Raw(__cdecl*)(unsigned char*,unsigned long*,const unsigned char*,unsigned long,int);
using Init=Raw(__thiscall*)(void*,const char*,const char*,const char*,const char*,bool);
using Save=Raw(__thiscall*)(void*,void*);
using Shutdown=Raw(__thiscall*)(void*);
using Links=Raw(__cdecl*)(void*,void*);
using Upload=Raw(__stdcall*)(void*);
using ClientPre=Raw(__thiscall*)(void*,const char*);
using ClientPost=Raw(__thiscall*)(void*);
using ClientDllInit=Raw(__cdecl*)();
using SleepFn=void(WINAPI*)(DWORD); // kernel32 Sleep (import slot): returns nothing
Transition orig_transition; MapLoad orig_map; Extract orig_extract; Spawn orig_spawn;
Model orig_model; Bsp orig_bsp; Progress orig_progress; Present orig_present;
Open orig_open; Close orig_close; Read orig_read; Write orig_write; DiskOpen orig_disk;
Inflate orig_inflate; Compress orig_compress; Init orig_init; Save orig_save; Shutdown orig_shutdown;
Links orig_links; Upload orig_upload; ClientPre orig_client_pre; ClientPost orig_client_post; ClientDllInit orig_client_dll; SleepFn orig_sleep;

// `return orig(...)` copies edx:eax before the Scope destructor runs; never call a native as a
// statement and let other code decide the wrapper's return registers.
Raw __cdecl probe_transition(int flag,const char* map,const char* landmark) {
    ensure(area_change,map); if (collecting()) copy_name(current.map,sizeof current.map,map);
    Scope scope(transition); return orig_transition(flag,map,landmark);
}
Raw __cdecl probe_map(const char* map,int restored) {
    ensure(map_command,map); if (collecting()) copy_name(current.map,sizeof current.map,map);
    Scope scope(map_load); return orig_map(map,restored);
}
Raw __fastcall probe_extract(void* self,void*,const char* name) {
    ensure(saved_game,name); Scope scope(extract); return orig_extract(self,name);
}
Raw __cdecl probe_spawn(const char* map,const char* landmark) {
    ensure(map_command,map); if (collecting()) copy_name(current.map,sizeof current.map,map);
    Scope scope(spawn); return orig_spawn(map,landmark);
}
Raw __fastcall probe_model(void* self,void*,void* record,const void* flags) {
    model_entry(record);
    char name[96]={}; if (collecting()) copy_name(name,sizeof name,static_cast<const char*>(record));
    Scope scope(model); const Raw result=orig_model(self,record,flags);
    const Tick elapsed=scope.stop(); if (elapsed) slow_model(name,elapsed); return result;
}
Raw __fastcall probe_bsp(void* self,void*,void* record) { Scope scope(bsp); return orig_bsp(self,record); }
Raw __cdecl probe_progress() { Scope scope(progress); return orig_progress(); }
Raw __fastcall probe_present(void* self,void*) { Scope scope(present); return orig_present(self); }
Raw __fastcall probe_open(void* self,void*,const char* path,const char* mode,const char* id) {
    const bool main=GetCurrentThreadId()==main_thread && enabled_probe;
    const unsigned b=main?bucket(path):7; Scope scope(fs_open);
    const Raw raw=orig_open(self,path,mode,id); void* const result=reinterpret_cast<void*>(static_cast<std::uintptr_t>(low(raw)));
    if (main) remember(result,b);
    if (collecting()) { ++current.buckets[b].opens; if (!result) ++current.buckets[b].misses; }
    scope.stop(0,!result); return raw;
}
Raw __fastcall probe_close(void* self,void*,void* handle) {
    Scope scope(fs_close); const Raw raw=orig_close(self,handle);
    if (GetCurrentThreadId()==main_thread && enabled_probe) handle_bucket(handle,true);
    return raw;
}
Raw __fastcall probe_read(void* self,void*,void* out,int size,void* handle) {
    Scope scope(fs_read); const Raw raw=orig_read(self,out,size,handle); const int result=static_cast<int>(low(raw));
    if (collecting() && result>0) current.buckets[handle_bucket(handle)].read+=static_cast<unsigned>(result);
    scope.stop(result>0?static_cast<unsigned>(result):0,result<0); return raw;
}
Raw __fastcall probe_write(void* self,void*,const void* data,int size,void* handle) {
    Scope scope(fs_write); const Raw raw=orig_write(self,data,size,handle); const int result=static_cast<int>(low(raw));
    if (collecting() && result>0) current.buckets[handle_bucket(handle)].written+=static_cast<unsigned>(result);
    scope.stop(result>0?static_cast<unsigned>(result):0,result<0 || (size>0 && result<size)); return raw;
}
Raw __fastcall probe_disk(void* self,void*,const char* path,const char* mode) {
    const bool collect=collecting(); const unsigned group=collect?bucket(path):6;
    Scope scope(disk_open); const Raw raw=orig_disk(self,path,mode); const bool missed=!low(raw);
    const auto elapsed=scope.stop(0,missed);
    if (collect && collecting()) {
        auto& b=current.buckets[group]; ++b.disk_attempts; b.disk_misses+=missed?1:0; b.disk_ticks+=elapsed;
    }
    return raw;
}
Raw __cdecl probe_inflate(unsigned char* out,unsigned long* size,const unsigned char* data,unsigned long length) {
    Scope scope(inflate); const Raw raw=orig_inflate(out,size,data,length); const bool ok=!low(raw);
    scope.stop(ok && size?*size:0,!ok); return raw;
}
Raw __cdecl probe_compress(unsigned char* out,unsigned long* size,const unsigned char* data,unsigned long length,int level) {
    Scope scope(compress); const Raw raw=orig_compress(out,size,data,length,level); const bool ok=!low(raw);
    scope.stop(ok?length:0,!ok); return raw;
}
Raw __fastcall probe_init(void* self,void*,const char* map,const char* entities,const char* old,const char* landmark,bool restored) {
    Scope scope(server_init); return orig_init(self,map,entities,old,landmark,restored);
}
// Native returns eax=1; SaveGameState treats eax==0 as a failed save. A 0 is counted as a
// failure and still returned unchanged.
Raw __fastcall probe_save(void* self,void*,void* data) {
    Scope scope(server_save); const Raw raw=orig_save(self,data); scope.stop(0,!low(raw)); return raw;
}
Raw __fastcall probe_shutdown(void* self,void*) { Scope scope(shutdown); return orig_shutdown(self); }
Raw __cdecl probe_links(void* header,void* callback) { Scope scope(model_links); return orig_links(header,callback); }
Raw __stdcall probe_upload(void* record) { Scope scope(model_upload); return orig_upload(record); }
Raw __fastcall probe_client_pre(void* self,void*,const char* map) { Scope scope(client_pre); return orig_client_pre(self,map); }
Raw __fastcall probe_client_post(void* self,void*) { Scope scope(client_post); return orig_client_post(self); }
Raw __cdecl probe_client_dll() { Scope scope(client_dll_init); return orig_client_dll(); }
void WINAPI probe_sleep(DWORD requested) {
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool collect=collecting(); const unsigned generation=epoch;
    Scope scope(engine_sleep); orig_sleep(requested); const Tick elapsed=scope.stop();
    if (!collect || !collecting() || generation!=epoch) return;
    const auto rva=caller>=engine_base && caller-engine_base<0x170000?caller-engine_base:0xffffffffu;
    for (auto& site:current.sleeps) if (!site.calls || site.rva==rva) {
        site.rva=rva; ++site.calls; site.requested_ms+=requested; site.ticks+=elapsed; return;
    }
    ++current.sleep_site_overflow;
}
bool native_active() {
    __try { return engine_base && *reinterpret_cast<const int*>(engine_base+0x1dbe50)==4 &&
        *reinterpret_cast<const int*>(engine_base+0xb42318)==0; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// --- loader wait identification (UP12 Vampire.exe, read-only; found by its code, not its hash) ---
void absolute(unsigned char* out,std::uintptr_t value) { std::memcpy(out,&value,4); }
bool same_bytes(std::uintptr_t at,const unsigned char* bytes,size_t size) {
    __try { return !std::memcmp(reinterpret_cast<const void*>(at),bytes,size); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// mov al,[flag_armed]; sub esp,5Ch / mov byte [flag_wait],1 / mov byte [flag_wait],0
bool loader_layout(std::uintptr_t base) {
    unsigned char entry[]={0xa0,0,0,0,0,0x83,0xec,0x5c}; absolute(entry+1,base+0x3f491);
    unsigned char set[]={0xc6,0x05,0,0,0,0,0x01}; absolute(set+2,base+0x3f490);
    unsigned char clear[]={0xc6,0x05,0,0,0,0,0x00}; absolute(clear+2,base+0x3f490);
    return same_bytes(base+0x7900,entry,sizeof entry) && same_bytes(base+0x7adf,set,sizeof set) && same_bytes(base+0x7cd0,clear,sizeof clear);
}
// Armed: the loader set its installed byte and the engine call site jumps to its wait function.
bool loader_armed(std::uintptr_t exe,std::uintptr_t engine) {
    __try {
        if (!exe || !engine || !*reinterpret_cast<const unsigned char*>(exe+0x3f491)) return false;
        const auto site=reinterpret_cast<const unsigned char*>(engine+0x8e4f3);
        std::int32_t displacement; std::memcpy(&displacement,site+1,4);
        return site[0]==0xe8 && reinterpret_cast<std::uintptr_t>(site+5)+displacement==exe+0x7900;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool legacy_pause=false;
LoaderWait loader_state() {
    if (legacy_pause) return wait_legacy;
    if (!sampler::wait_flag || !sampler::thread) return wait_unavailable;
    return loader_armed(exe_base,engine_base)?wait_armed:wait_off;
}

void log_session(const Session& s) {
    char line[900]; Tick self_total=0;
    for (const auto& m:s.metrics) self_total+=m.self;
    const Tick wall=s.end-s.begin; const auto& e=s.endpoints;
    const double wait=s.metrics[continue_wait].calls?ms(s.metrics[continue_wait].total):0.0;
    sprintf_s(line,"Loading summary id=%u kind=%s name=%.95s map=%.95s status=%s wall_ms=%.3f ready_ms=%.3f first_sim_ms=%.3f request_to_native_ms=%.3f loader_wait_ms=%.3f load_excluding_wait_ms=%.3f uninstrumented_ms=%.3f coverage=0x%x coverage_start=0x%x stack_overflows=%u abandoned_scopes=%u handle_overflows=%u",
        s.id,kinds[s.kind],s.name,s.map,s.status,ms(wall),e.ready?ms(e.ready-s.begin):-1.0,
        s.first_sim?ms(s.first_sim-s.begin):-1.0,s.native_enter?ms(s.native_enter-s.begin):-1.0,wait,
        s.first_sim && (s.loader==wait_armed || s.loader==wait_off)?ms(s.first_sim-s.begin)-wait:-1.0,
        ms(wall>self_total?wall-self_total:0),s.coverage,s.coverage_start,s.overflows,s.unbalanced,s.handle_overflows);
    host->log(line);
    if (s.kind!=startup) {
        sprintf_s(line,"Loading endpoints id=%u loader_wait=%s signon_ready_ms=%.3f active_ms=%.3f wait_begin_ms=%.3f wait_end_ms=%.3f ready_to_wait_ms=%.3f wait_end_to_first_sim_ms=%.3f scoped_in_wait_ms=%.3f sampler_samples=%llu sampler_max_gap_ms=%.3f (ready=engine signon 3/3 as the loader's own gate; wait sampled from the loader's wait byte)",
            s.id,loader_wait_names[s.loader],e.ready?ms(e.ready-s.begin):-1.0,e.active?ms(e.active-s.begin):-1.0,
            e.wait_begin?ms(e.wait_begin-s.begin):-1.0,e.wait_end?ms(e.wait_end-s.begin):-1.0,
            e.ready && e.wait_begin>=e.ready?ms(e.wait_begin-e.ready):-1.0,
            e.wait_end && s.first_sim>=e.wait_end?ms(s.first_sim-e.wait_end):-1.0,ms(s.wait_scoped),e.samples,ms(e.max_gap));
        host->log(line);
    }
    sprintf_s(line,"Loading detail id=%u model_entries=%llu model_loaded_flag=%llu model_studio=%llu sleep_site_overflows=%u (loaded flag is not proof of a resident cache hit)",
        s.id,s.model_entries,s.model_loaded_flag,s.model_studio,s.sleep_site_overflow); host->log(line);
    if (s.kind==startup) {
        sprintf_s(line,"Loading startup id=%u estimated_process_to_menu_ms=%.3f preprobe_process_ms=%llu initial_probe_install_ms=%.3f (preprobe time is uninstrumented; combined clocks are approximate)",
            s.id,static_cast<double>(process_age_ms)+ms(wall),process_age_ms,ms(install_ticks)); host->log(line);
    }
    for (unsigned i=0;i<metric_count;++i) if (s.metrics[i].calls) {
        const auto& m=s.metrics[i];
        sprintf_s(line,"Loading metric id=%u phase=%s calls=%llu inclusive_ms=%.3f exclusive_ms=%.3f max_ms=%.3f bytes=%llu failures=%llu",
            s.id,metric_names[i],m.calls,ms(m.total),ms(m.self),ms(m.maximum),m.bytes,m.failed); host->log(line);
    }
    for (unsigned i=0;i<8;++i) if (s.buckets[i].opens || s.buckets[i].read || s.buckets[i].written || s.buckets[i].disk_attempts) {
        const auto& b=s.buckets[i]; sprintf_s(line,"Loading IO id=%u group=%s opens=%llu misses=%llu read_bytes=%llu written_bytes=%llu disk_attempts=%llu disk_misses=%llu disk_ms=%.3f",
            s.id,bucket_names[i],b.opens,b.misses,b.read,b.written,b.disk_attempts,b.disk_misses,ms(b.disk_ticks)); host->log(line);
    }
    for (const auto& site:s.sleeps) if (site.calls) {
        sprintf_s(line,"Loading sleep id=%u engine_return_rva=0x%x calls=%llu requested_ms=%llu actual_ms=%.3f",s.id,static_cast<unsigned>(site.rva),site.calls,site.requested_ms,ms(site.ticks)); host->log(line);
    }
    for (const auto& slow:s.slow) if (slow.ticks) {
        sprintf_s(line,"Loading slow_model id=%u inclusive_ms=%.3f name=%.95s",s.id,ms(slow.ticks),slow.name); host->log(line);
    }
    if (session_reported) session_reported(s);
}

// --- hook helpers ---
void owner_name(const void* address,char* out,size_t capacity) {
    copy_name(out,capacity,"unknown");
    HMODULE owner=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,static_cast<LPCWSTR>(address),&owner) || !owner) return;
    wchar_t path[MAX_PATH]; const DWORD n=GetModuleFileNameW(owner,path,MAX_PATH);
    if (!n || n>=MAX_PATH) return;
    const wchar_t* leaf=path; for (const wchar_t* p=path;*p;++p) if (*p==L'\\' || *p==L'/') leaf=p+1;
    sprintf_s(out,capacity,"%ls",leaf);
}
bool code_hook(std::uintptr_t base,unsigned rva,const unsigned char* bytes,size_t size,void* detour,void** saved_original,const char* label) {
    const int result=host->hook_function(host->context,reinterpret_cast<void*>(base+rva),bytes,size,detour,saved_original);
    char line[192]; sprintf_s(line,"Loading probe hook %s: %s (%d)",label,result==BLSE_HOOK_OK?"enabled":"unavailable",result); host->log(line);
    return result==BLSE_HOOK_OK;
}
// Hooks a pointer slot only while it still holds the expected native pointer; a foreign owner
// (another mod's wrapper, or a previous install) is left alone and named in the log.
bool pointer_hook(std::uintptr_t slot,std::uintptr_t expected,void* detour,void** saved_original,const char* label) {
    int result=BLSE_HOOK_BYTES_DIFFER; std::uintptr_t found=0;
    __try { found=*reinterpret_cast<std::uintptr_t*>(slot); if (found==expected)
        result=host->hook_pointer(host->context,reinterpret_cast<void**>(slot),detour,saved_original); }
    __except(EXCEPTION_EXECUTE_HANDLER) { result=BLSE_HOOK_FAILED; }
    char line[256];
    if (result==BLSE_HOOK_BYTES_DIFFER) {
        char owner[64]; owner_name(reinterpret_cast<const void*>(found),owner,sizeof owner);
        sprintf_s(line,"Loading probe hook %s: unavailable, slot owned by %s (left unchanged)",label,owner);
    } else sprintf_s(line,"Loading probe hook %s: %s (%d)",label,result==BLSE_HOOK_OK?"enabled":"unavailable",result);
    host->log(line);
    return result==BLSE_HOOK_OK;
}
enum PresentChoice { present_hook, present_companion, present_foreign };
PresentChoice present_choice(std::uintptr_t slot_value,std::uintptr_t native,bool companion_loaded) {
    if (companion_loaded) return present_companion; // the intro companion owns this slot's lifetime
    return slot_value==native?present_hook:present_foreign;
}
bool known(HMODULE module,const build_check::Profile& profile) { return build_check::known(module,profile,client_log); }
HMODULE owner_of(const void* address) {
    HMODULE owner=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,static_cast<LPCWSTR>(address),&owner);
    return owner;
}
// True when `code` starts with jmp rel32 into the module that hosts BLSE (owner of host->log).
bool blse_owns_jump(const unsigned char* code) {
    std::uintptr_t target=0;
    __try {
        if (code[0]!=0xe9) return false;
        std::int32_t displacement; std::memcpy(&displacement,code+1,4);
        target=reinterpret_cast<std::uintptr_t>(code+5)+displacement;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    const HMODULE blse=owner_of(reinterpret_cast<const void*>(host->log));
    return blse && owner_of(reinterpret_cast<const void*>(target))==blse;
}
bool blse_owns(std::uintptr_t server) {
    __try {
        auto table=reinterpret_cast<void* const*>(server+0x462504);
        HMODULE owner=nullptr,blse_owner=nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(table[3]),&owner);
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(host->log),&blse_owner);
        return owner && owner==blse_owner && reinterpret_cast<std::uintptr_t>(owner)!=server;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void attach_modules() {
    if (!late_fs) if (auto fs=GetModuleHandleW(L"filesystem_stdio.dll")) {
        late_fs=true;
        if (known(fs,build_check::filesystem_profile)) {
            const auto base=reinterpret_cast<std::uintptr_t>(fs),vtable=base+0x1d1e8; fs_base=base;
            // BLSE's mod folders install (at startup, or later when a mod first provides a file)
            // requires slot 0 to be native Open and checks the code of slots 1/2. Wrap those
            // only once BLSE's own Open hook is in place, so a later BLSE install never fails.
            if (blse_owns_jump(reinterpret_cast<const unsigned char*>(base+0x2fa0))) {
                if (pointer_hook(vtable,base+0x2fa0,reinterpret_cast<void*>(probe_open),reinterpret_cast<void**>(&orig_open),"fs_open")) coverage|=1u<<fs_open;
                if (pointer_hook(vtable+4,base+0x3260,reinterpret_cast<void*>(probe_close),reinterpret_cast<void**>(&orig_close),"fs_close")) coverage|=1u<<fs_close;
                if (pointer_hook(vtable+8,base+0x35d0,reinterpret_cast<void*>(probe_read),reinterpret_cast<void**>(&orig_read),"fs_read")) coverage|=1u<<fs_read;
            } else host->log("Loading probe hooks fs_open/fs_close/fs_read: skipped, BLSE mod-file hooks are not installed (wrapping could block their later install); disk_open still times lower-level opens.");
            if (pointer_hook(vtable+12,base+0x36b0,reinterpret_cast<void*>(probe_write),reinterpret_cast<void**>(&orig_write),"fs_write")) coverage|=1u<<fs_write;
            const unsigned char code[]={0x8b,0x44,0x24,0x08,0x83,0xec,0x08};
            if (code_hook(base,0x1750,code,sizeof code,reinterpret_cast<void*>(probe_disk),reinterpret_cast<void**>(&orig_disk),"disk_open")) coverage|=1u<<disk_open;
        } else host->log("Loading probe filesystem hooks disabled: unknown FileSystem_Stdio.dll build.");
    }
    if (!late_mat) if (auto mat=GetModuleHandleW(L"MaterialSystem.dll")) {
        late_mat=true;
        if (known(mat,build_check::material_profile)) {
            const auto base=reinterpret_cast<std::uintptr_t>(mat); std::uintptr_t value=0;
            __try { value=*reinterpret_cast<const std::uintptr_t*>(base+0x498b0); } __except(EXCEPTION_EXECUTE_HANDLER) {}
            switch (present_choice(value,base+0xea10,GetModuleHandleW(L"engine_fixes_intro.vtm")!=nullptr)) {
            case present_companion: host->log("Loading probe hook swap_buffers: skipped, the intro companion owns this slot (presentation untimed)."); break;
            case present_foreign: {
                char owner[64],line[160]; owner_name(reinterpret_cast<const void*>(value),owner,sizeof owner);
                sprintf_s(line,"Loading probe hook swap_buffers: skipped, slot owned by %s (presentation untimed).",owner); host->log(line); break; }
            case present_hook:
                material_base=base;
                if (pointer_hook(base+0x498b0,base+0xea10,reinterpret_cast<void*>(probe_present),reinterpret_cast<void**>(&orig_present),"swap_buffers")) coverage|=1u<<present;
            }
        } else host->log("Loading probe presentation hook disabled: unknown MaterialSystem.dll build.");
    }
    if (!late_server) if (auto server=GetModuleHandleW(L"vampire.dll")) {
        // Wait for BLSE's vtable wrappers before hooking native bodies: its initial Save
        // verification follows jumps and would otherwise see this probe's detour.
        const auto base=reinterpret_cast<std::uintptr_t>(server);
        if (blse_owns(base)) {
            late_server=true;
            if (!server_profile_supported()) host->log("Loading probe server hooks disabled: unsupported server profile.");
            else {
                const unsigned char init_code[]={0x83,0xec,0x08,0x53,0x55,0x56,0x57};
                const unsigned char save_code[]={0x83,0xec,0x28,0x53,0x56,0x57};
                unsigned char stop_code[]={0xc6,0x05,0,0,0,0,0x01}; absolute(stop_code+2,base+0x70b267);
                if (code_hook(base,0x11a7a0,init_code,sizeof init_code,reinterpret_cast<void*>(probe_init),reinterpret_cast<void**>(&orig_init),"server_init")) coverage|=1u<<server_init;
                if (code_hook(base,0x11b080,save_code,sizeof save_code,reinterpret_cast<void*>(probe_save),reinterpret_cast<void**>(&orig_save),"server_save")) coverage|=1u<<server_save;
                if (code_hook(base,0x11adc0,stop_code,sizeof stop_code,reinterpret_cast<void*>(probe_shutdown),reinterpret_cast<void**>(&orig_shutdown),"server_shutdown")) coverage|=1u<<shutdown;
            }
        }
    }
    if (!late_client) if (auto client_module=GetModuleHandleW(L"client.dll")) {
        late_client=true;
        if (supported(client_module)) {
            const auto base=reinterpret_cast<std::uintptr_t>(client_module),vtable=base+0x233f44;
            if (pointer_hook(vtable+8,base+0xcbd00,reinterpret_cast<void*>(probe_client_pre),reinterpret_cast<void**>(&orig_client_pre),"client_pre_entity")) coverage|=1u<<client_pre;
            if (pointer_hook(vtable+12,base+0xcbdc0,reinterpret_cast<void*>(probe_client_post),reinterpret_cast<void**>(&orig_client_post),"client_post_entity")) coverage|=1u<<client_post;
        } else host->log("Loading probe client phase hooks disabled: unsupported client build.");
    }
}

// --- main-thread task: bounded summaries, attach retries only while a session is open ---
bool menu_listed() {
    if (split_role()==BLSE_SPLIT_WORLD) return false;
    if (!BLSE_HOST_HAS(host,open_menus) || !host->open_menus) return false;
    char names[512]={}; size_t size=0;
    if (!host->open_menus(host->context,names,sizeof names,&size) || size>=sizeof names) return false;
    for (const char* p=names;*p;) {
        const char* end=std::strchr(p,'\n'); const size_t n=end?static_cast<size_t>(end-p):std::strlen(p);
        if (n==9 && !std::memcmp(p,"game_menu",9)) return true;
        if (!end) break; p=end+1;
    }
    return false;
}
void BLSE_CALL task(void*) {
    task_queued=false;
    if (GetCurrentThreadId()!=main_thread) return;
    if (enabled_probe && active && !depth) attach_modules();
    if (enabled_probe && active && !depth) {
        refresh_endpoints();
        if (current.kind==startup && !current.menu && menu_listed()) current.menu=true;
        const auto now=clock_now();
        if (current.kind==startup && current.menu) finish("main_menu_visible",now);
        else if (current.first_sim) finish("first_simulated_frame",now);
        else if (now-current.begin>frequency*600 && !current.endpoints.ready) finish("timeout_incomplete",now,true);
    }
    if (!depth) {
        reporting=true;
        for (unsigned i=0;i<queued;++i) log_session(queue[i]);
        queued=0;
        if (ui.pending) {
            char line[192]; sprintf_s(line,"Loading UI id=%u closed_ms=%.3f after_ready_ms=%.3f (presentation marker, not extra resource work)",
                ui.id,ms(ui.closed-ui.start),ui.ready && ui.closed>=ui.ready?ms(ui.closed-ui.ready):-1.0); host->log(line); ui={};
        }
        if (dropped) { char line[96]; sprintf_s(line,"Loading probe dropped summaries=%u",dropped); host->log(line); dropped=0; }
        reporting=false;
    }
    schedule();
}
void schedule(bool flush) {
    if (task_queued || GetCurrentThreadId()!=main_thread) return;
    const bool needed=(enabled_probe && active) || queued || ui.pending || dropped || depth;
    if (!needed && !flush) return;
    if (host->add_task(host->context,task,nullptr)) { task_queued=true; return; }
    if (enabled_probe) {
        enabled_probe=false; active=false; sampler::disarm();
        host->log("Loading probe stopped: task queue unavailable (hooks pass through).");
    }
}

// --- lifecycle events (one callback per topic; game_frame is a single branch when idle) ---
bool event_thread() { return enabled_probe && GetCurrentThreadId()==main_thread; }
void BLSE_CALL on_before_load(const char*,const void* data,size_t size,void*) {
    if (!event_thread()) return;
    char name[96]; copy_name(name,sizeof name,static_cast<const char*>(data),size); begin(saved_game,name,clock_now());
}
void BLSE_CALL on_level_change(const char*,const void* data,size_t size,void*) {
    if (!event_thread()) return;
    char name[96]; copy_name(name,sizeof name,static_cast<const char*>(data),size); begin(area_change,name,clock_now());
}
void BLSE_CALL on_load_failed(const char*,const void*,size_t,void*) { if (event_thread()) finish("load_failed",clock_now(),true); }
void BLSE_CALL on_level_init(const char*,const void* data,size_t size,void*) {
    if (!event_thread() || !active) return;
    copy_name(current.map,sizeof current.map,static_cast<const char*>(data),size); current.level=true;
    if (current.kind!=startup) { current.loader=loader_state(); sampler::arm(current.id); }
}
void BLSE_CALL on_game_frame(const char*,const void* data,size_t size,void*) {
    if (!active || !collecting() || !current.level || depth || current.first_sim || size!=1 || !data || *static_cast<const char*>(data)!='1') return;
    // Frames used to settle physics inside a load are excluded (depth); completion needs the
    // sampled signon/active endpoint or the native active state.
    refresh_endpoints();
    if (current.endpoints.ready || current.endpoints.active || native_active()) current.first_sim=clock_now();
}
void BLSE_CALL on_menu(const char* topic,const void* data,size_t size,void*) {
    if (!event_thread()) return;
    const auto text=static_cast<const char*>(data);
    size_t offset=0; while (text && offset<size && text[offset]) ++offset;
    if (!text || offset>=size) return; ++offset;
    char screen[48]; copy_name(screen,sizeof screen,text+offset,size-offset);
    const bool open=!std::strcmp(topic,BLSE_MSG_MENU_OPENED);
    if (!std::strcmp(screen,"game_menu")) { menu_visible=open; if (active && current.kind==startup && open) { current.menu=true; schedule(); } }
    if (!std::strcmp(screen,"loading") && !open) {
        refresh_endpoints();
        if (active && current.kind!=startup && current.level && (current.endpoints.ready || native_active())) current.ui_closed=clock_now();
        else if (ui.id) { ui.closed=clock_now(); ui.pending=true; schedule(); }
    }
}
void BLSE_CALL on_restart(const char*,const void*,size_t,void*) {
    if (GetCurrentThreadId()!=main_thread || restarted || !enabled_probe) return;
    restarted=true;
    // BLSE dropped this probe's patches in the unloaded engine/server/client; detours left in
    // still-loaded filesystem/material slots call their saved native targets in those same
    // modules and pass through from now on. The sampler stops before the new engine runs.
    finish("engine_restart",clock_now(),true);
    enabled_probe=false; active=false; sampler::disarm();
    const bool stopped=sampler::stop(250);
    sampler::signon=sampler::client_state=nullptr;
    host->log(stopped?"Loading probe stopped after engine restart; use a fresh process for further captures (hooks pass through).":
                      "Loading probe stopped after engine restart; sampler thread still exiting (retained, not terminated).");
    schedule(true);
}
struct Topic { const char* name; BLSE_MessageFn callback; };
bool subscribe_all() {
    const Topic topics[]={{BLSE_MSG_BEFORE_LOAD,on_before_load},{BLSE_MSG_LOAD_FAILED,on_load_failed},
        {BLSE_MSG_LEVEL_CHANGE,on_level_change},{BLSE_MSG_LEVEL_INIT,on_level_init},{BLSE_MSG_GAME_FRAME,on_game_frame},
        {BLSE_MSG_MENU_OPENED,on_menu},{BLSE_MSG_MENU_CLOSED,on_menu},{BLSE_MSG_ENGINE_RESTART,on_restart}};
    static_assert(_countof(topics)==_countof(subscriptions),"subscription slots");
    subscription_count=0;
    // Process split (BLSE 0.7.2+): the world's process has no window, but BLSE forwards the
    // window's menu events to it about a frame late. Its game_menu/loading screens are not this
    // process's, so they must not end its startup capture or stamp ui_closed; skip them there.
    const bool world_process=split_role()==BLSE_SPLIT_WORLD;
    for (const auto& topic:topics) {
        if (world_process && topic.callback==on_menu) continue;
        const auto id=host->subscribe(host->context,topic.name,topic.callback,nullptr);
        if (!id) {
            for (unsigned i=0;i<subscription_count;++i) host->unsubscribe(host->context,subscriptions[i]);
            subscription_count=0; return false;
        }
        subscriptions[subscription_count++]=id;
    }
    return true;
}
bool requested() {
    if (!BLSE_HOST_HAS(host,ini_get) || !host->ini_get) return false;
    char value[16]={}; size_t size=0;
    if (!host->ini_get(host->context,"engine_fixes","Loading","loading_trace",value,sizeof value,&size) || size>=sizeof value) return false;
    return value[0] && value[0]!='0';
}
bool api_ready() {
    return BLSE_HOST_HAS(host,add_task) && host->add_task && BLSE_HOST_HAS(host,hook_pointer) && host->hook_pointer &&
           BLSE_HOST_HAS(host,hook_function) && host->hook_function && BLSE_HOST_HAS(host,subscribe) && host->subscribe &&
           BLSE_HOST_HAS(host,unsubscribe) && host->unsubscribe && BLSE_HOST_HAS(host,build_profile);
}
void identify_loader() {
    legacy_pause=GetModuleHandleW(L"loading-pause.vtm")!=nullptr;
    if (legacy_pause) { host->log("Loading probe: legacy loading-pause.vtm present; its wait is not separated (first_sim may include it)."); return; }
    auto exe=GetModuleHandleW(nullptr);
    if (!loader_layout(reinterpret_cast<std::uintptr_t>(exe))) {
        host->log("Loading probe: Vampire.exe is not the verified UP12 loader; any loader wait is not separated."); return;
    }
    exe_base=reinterpret_cast<std::uintptr_t>(exe);
    sampler::wait_flag=reinterpret_cast<const volatile unsigned char*>(exe_base+0x3f490);
}
// Installs once per process at BLSE_PluginLoad (before engine client init), after the plugin's
// own registration succeeded. Missing setting: nothing is done and nothing is logged.
void install() {
    if (attempted) return; attempted=true;
    if (!requested()) return;
    if (!api_ready() || !host->build_profile || std::strcmp(host->build_profile,server_profile)) {
        host->log("Loading probe disabled: unsupported profile/API."); return;
    }
    auto module=GetModuleHandleW(L"engine.dll");
    if (!known(module,build_check::engine_profile)) {
        host->log("Loading probe disabled: unsupported engine."); return;
    }
    LARGE_INTEGER f; if (!QueryPerformanceFrequency(&f) || f.QuadPart<=0) { host->log("Loading probe disabled: no performance counter."); return; }
    frequency=static_cast<Tick>(f.QuadPart); main_thread=GetCurrentThreadId();
    engine_base=reinterpret_cast<std::uintptr_t>(module); const Tick started=clock_now();
    if (!subscribe_all()) { host->log("Loading probe disabled: lifecycle subscription unavailable (none kept)."); return; }
    identify_loader();
    sampler::signon=reinterpret_cast<const volatile int*>(engine_base+0x1dbe50);
    sampler::client_state=reinterpret_cast<const volatile int*>(engine_base+0x20b658);
    if (!sampler::start()) host->log("Loading probe: endpoint sampler unavailable; ready/wait endpoints are unknown.");
    else if (sampler::wait_flag) coverage|=1u<<continue_wait;
    unsigned char root_code[]={0xa1,0,0,0,0}; absolute(root_code+1,engine_base+0x12b0754);
    const unsigned char map_code[]={0x56,0x8b,0x74,0x24,0x08,0x56};
    const unsigned char extract_code[]={0x8b,0x44,0x24,0x04,0x81,0xec,0xd0,0x03,0x00,0x00};
    const unsigned char model_code[]={0xb8,0x98,0x00,0x01,0x00};
    const unsigned char bsp_code[]={0x83,0xec,0x10,0x53,0x8b,0x5c,0x24,0x18};
    const unsigned char links_code[]={0x83,0xec,0x14,0x8b,0x4c,0x24,0x1c};
    const unsigned char client_dll_code[]={0xb8,0x04,0x12,0x00,0x00};
    unsigned char upload_code[]={0xa0,0,0,0,0}; absolute(upload_code+1,engine_base+0xa6c018);
    unsigned char progress_code[]={0x51,0xa1,0,0,0,0}; absolute(progress_code+2,engine_base+0xb42ebc);
    if (code_hook(engine_base,0x8f120,root_code,sizeof root_code,reinterpret_cast<void*>(probe_transition),reinterpret_cast<void**>(&orig_transition),"transition")) coverage|=1u<<transition;
    if (code_hook(engine_base,0x8f2e0,map_code,sizeof map_code,reinterpret_cast<void*>(probe_map),reinterpret_cast<void**>(&orig_map),"map_load")) coverage|=1u<<map_load;
    if (code_hook(engine_base,0x96010,extract_code,sizeof extract_code,reinterpret_cast<void*>(probe_extract),reinterpret_cast<void**>(&orig_extract),"save_extract")) coverage|=1u<<extract;
    if (code_hook(engine_base,0xf55f0,root_code,sizeof root_code,reinterpret_cast<void*>(probe_spawn),reinterpret_cast<void**>(&orig_spawn),"spawn_server")) coverage|=1u<<spawn;
    if (code_hook(engine_base,0xb8960,model_code,sizeof model_code,reinterpret_cast<void*>(probe_model),reinterpret_cast<void**>(&orig_model),"model_load")) coverage|=1u<<model;
    if (code_hook(engine_base,0xb95c0,bsp_code,sizeof bsp_code,reinterpret_cast<void*>(probe_bsp),reinterpret_cast<void**>(&orig_bsp),"bsp_build")) coverage|=1u<<bsp;
    if (code_hook(engine_base,0x87110,progress_code,sizeof progress_code,reinterpret_cast<void*>(probe_progress),reinterpret_cast<void**>(&orig_progress),"progress")) coverage|=1u<<progress;
    if (code_hook(engine_base,0xccf0,links_code,sizeof links_code,reinterpret_cast<void*>(probe_links),reinterpret_cast<void**>(&orig_links),"model_dependencies")) coverage|=1u<<model_links;
    if (code_hook(engine_base,0xbb460,upload_code,sizeof upload_code,reinterpret_cast<void*>(probe_upload),reinterpret_cast<void**>(&orig_upload),"model_render_setup")) coverage|=1u<<model_upload;
    if (code_hook(engine_base,0x1bcf0,client_dll_code,sizeof client_dll_code,reinterpret_cast<void*>(probe_client_dll),reinterpret_cast<void**>(&orig_client_dll),"client_dll_init")) coverage|=1u<<client_dll_init;
    auto kernel_module=GetModuleHandleW(L"kernel32.dll");
    const auto sleep=reinterpret_cast<std::uintptr_t>(kernel_module?GetProcAddress(kernel_module,"Sleep"):nullptr);
    if (sleep && pointer_hook(engine_base+0x1730c4,sleep,reinterpret_cast<void*>(probe_sleep),reinterpret_cast<void**>(&orig_sleep),"engine_sleep")) coverage|=1u<<engine_sleep;
    if (auto zlib=GetModuleHandleW(L"zlib1.dll")) {
        const auto uncompress=reinterpret_cast<std::uintptr_t>(GetProcAddress(zlib,"uncompress"));
        const auto compress2=reinterpret_cast<std::uintptr_t>(GetProcAddress(zlib,"compress2"));
        if (uncompress && pointer_hook(engine_base+0x17346c,uncompress,reinterpret_cast<void*>(probe_inflate),reinterpret_cast<void**>(&orig_inflate),"uncompress")) coverage|=1u<<inflate;
        if (compress2 && pointer_hook(engine_base+0x173468,compress2,reinterpret_cast<void*>(probe_compress),reinterpret_cast<void**>(&orig_compress),"compress2")) coverage|=1u<<compress;
    }
    attach_modules(); install_ticks=clock_now()-started;
    FILETIME created,exited,kernel,user,now; GetSystemTimeAsFileTime(&now);
    if (GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) {
        ULARGE_INTEGER a,b; a.LowPart=created.dwLowDateTime; a.HighPart=created.dwHighDateTime;
        b.LowPart=now.dwLowDateTime; b.HighPart=now.dwHighDateTime;
        if (b.QuadPart>=a.QuadPart) process_age_ms=(b.QuadPart-a.QuadPart)/10000;
    }
    enabled_probe=true; begin(startup,"BLSE-ready to main menu",clock_now());
    if (!enabled_probe) return; // task queue refused inside begin(); already logged
    char line[400]; sprintf_s(line,"Loading probe enabled: main-thread summaries of the first 32 captures, 64 timing scopes, 512 handle slots, loader_wait=%s; startup begins at plugin install (process_age_ms=%llu install_ms=%.3f). No loading behavior changed.",
        sampler::wait_flag?(sampler::thread?"sampled":"unsampled"):"not_separated",process_age_ms,ms(install_ticks)); host->log(line);
}
} // namespace loading
