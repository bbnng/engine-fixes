// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Signon tail of a load (2026-11-14): what runs between the native load function's return and
// signon 3/3. Include inside engine_fixes.cpp's unnamed namespace AFTER loading_probe.inl and call
// loading::signon::install() right after loading::install() in BLSE_PluginLoad.
//
// Static evidence (engine.dll 9d00b2c1..., IDA headless + capstone):
// the map load (engine+0x8f2e0) or transition (+0x8f120) returns to the host-state loop, and the
// local client then connects over the loopback netchan in ordinary host frames. Each frame runs
// CL_Move (send) -> SV_Frame (read, reply) -> client read (_Host_RunFrame, +0x8e450). Reliable data
// larger than 1200 bytes is split into fragments (+0xbd6e0) of the per-client block size, which is
// 1024 bytes until the client is active and spawned (+0xf0820); only one reliable packet is in
// flight (Netchan_TransmitBits +0xbc710 waits for the ack), so the bulk signon data (serverinfo with
// string tables, then the prespawn signon buffer) moves about 1 KB per host frame. Host frames are
// limited by Host_FilterTime (+0x8ba30) to fps_max, which UP's autoexec sets to 65.
//
// Diagnostics ([Loading] loading_trace=1): per load, for each client state (connecting, signon 0,
// 1, 2, 3/3 ready+loader wait) the number of frames, frame work, time the native fps_max limiter
// held the next frame, and the server's fragmented reliable bytes. Read-only.
//
// Opt-in experiment ([Loading] signon_fps_max=N, 66..1000; missing or 0 = off, unchanged game):
// while the local single-player client is at signon 0 and the server has neither spawned nor
// activated it, Host_FilterTime sees fps_max raised to N for that one call only (the native value
// is put back bit for bit before the call returns to the engine). N=300 is the engine's own
// default. In that window the server does not simulate (SV_Frame +0xf6379: think only at client
// state 4 or 0 in single player) and passes ignore=1 for usercmds (+0xf9aa5: client neither active
// nor spawned), so no game state advances per frame. Nothing is skipped: every native frame,
// message, fragment, callback and the loader's Enter wait still run; signon 1/2, spawn/restore
// and gameplay keep the native cap.
namespace loading {
namespace signon {
enum Phase { connecting, signon0, signon1, signon2, ready_wait, phase_count };
constexpr int untracked=phase_count;
const char* const phase_names[]={"connecting","signon0","signon1","signon2","ready_wait"};
struct PhaseStats { Tick first=0,frames=0,work=0,idle=0,max_work=0,checks=0,paced=0,fragment_messages=0,fragment_bytes=0; };
struct Record {
    unsigned id=0; Tick base=0,start=0,complete=0; PhaseStats phases[phase_count];
    std::uint32_t fps_bits=0; bool fps_known=false; char end_state=0;
};
// Engine globals, set from the verified engine base at install (tests point them at fakes).
struct Globals {
    const volatile int* cls_state=nullptr;           // +0x1dbe50: 2 connecting, 3 connected, 4 active
    const volatile int* cl_signon=nullptr;           // +0x20b658: 0..3
    const volatile int* cls_remote=nullptr;          // +0x1dbe5c: cls.netchan remote type, 1 = loopback
    const volatile int* sv_active=nullptr;           // +0x12b0754
    const volatile int* maxclients=nullptr;          // +0x12af1c8
    const volatile std::uintptr_t* clients=nullptr;  // +0x12af1d8: client_t array (+0xc active, +0x10 spawned, +0x24 remote type)
    const volatile std::uintptr_t* fps_parent=nullptr; std::uintptr_t fps_self=0; // fps_max ConVar +4 must be itself
    volatile std::uint32_t* fps_value=nullptr;       // fps_max ConVar float (+0x28), read only by Host_FilterTime
};
Globals g;
using Filter=Raw(__cdecl*)(std::uint32_t);                  // Host_FilterTime(float seconds), plain ret
using Fragments=Raw(__cdecl*)(int,void*,const void*,int);   // Netchan_CreateFragments_(server, chan, msg); 4th slot only on its error path
Filter orig_filter=nullptr; Fragments orig_fragments=nullptr;
DWORD thread=0; bool attempted=false,gone=false,tracing=false;
unsigned pacing_fps=0; std::uint32_t pacing_bits=0;
Record open_record,done[4]; bool record_open=false; unsigned done_count=0,done_dropped=0; bool task_queued=false;
Tick prev_pass=0,reject_at=0,checks_since=0; int prev_phase=untracked; bool prev_paced=false;

bool read(const volatile int* at,int& value) {
    __try { if (!at) return false; value=*at; return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int classify(bool known,int state,int number) {
    if (!known) return untracked;
    if (state==2) return connecting;
    if (state==3 && number>=0 && number<=3) return signon0+number;
    return untracked;
}
// True only while the local single-player client is at signon 0 and the server's client record is
// neither active nor spawned (the native usercmd-ignore condition) on a loopback channel.
bool unspawned_local_signon0(int state,int number) {
    if (state!=3 || number!=0) return false;
    __try {
        if (!g.sv_active || !*g.sv_active || !g.maxclients || *g.maxclients!=1) return false;
        if (!g.cls_remote || *g.cls_remote!=1 || !g.clients) return false;
        const auto record=reinterpret_cast<const volatile int*>(*g.clients); if (!record) return false;
        return record[0xc/4]==0 && record[0x10/4]==0 && record[0x24/4]==1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Raises (never lowers) the value Host_FilterTime reads, only for this call.
bool pace_begin(std::uint32_t& saved) {
    if (!pacing_fps || !g.fps_value || !g.fps_parent) return false;
    __try {
        if (*g.fps_parent!=g.fps_self) return false;
        saved=*g.fps_value; float value; std::memcpy(&value,&saved,4);
        if (!(value>0.0f && value<static_cast<float>(pacing_fps))) return false; // also rejects NaN; 0 = unlimited
        *g.fps_value=pacing_bits; return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void pace_end(std::uint32_t saved) { __try { *g.fps_value=saved; } __except(EXCEPTION_EXECUTE_HANDLER) {} }

void BLSE_CALL report(void*);
void schedule_report() {
    if (task_queued || !done_count && !done_dropped) return;
    if (host->add_task(host->context,report,nullptr)) task_queued=true;
}
void open(Tick now,const std::uint32_t* fps) {
    open_record={}; record_open=true; open_record.start=now;
    if (enabled_probe && active && current.kind!=startup) { open_record.id=current.id; open_record.base=current.begin; }
    else open_record.base=now;
    if (fps) { open_record.fps_bits=*fps; open_record.fps_known=true; }
}
void close(Tick now,int state) {
    open_record.complete=now; open_record.end_state=static_cast<char>(state); record_open=false;
    if (done_count<4) done[done_count++]=open_record; else ++done_dropped;
    schedule_report();
}
// Frame accounting at each Host_FilterTime return. A pass starts a frame; the interval to the next
// pass belongs to the phase seen at its start: work until the first limiter refusal, then idle.
void account(Tick now,bool pass,int state,int phase,bool paced,const std::uint32_t* fps) {
    ++checks_since;
    if (!pass) { if (!reject_at) reject_at=now; return; }
    if (prev_pass && record_open && prev_phase!=untracked) {
        auto& p=open_record.phases[prev_phase];
        const Tick work_end=reject_at?reject_at:now, work=work_end-prev_pass;
        ++p.frames; p.work+=work; p.idle+=reject_at?now-reject_at:0; p.checks+=checks_since;
        if (work>p.max_work) p.max_work=work;
        if (prev_paced) ++p.paced;
    }
    if (phase!=untracked) {
        if (!record_open) open(now,fps);
        auto& p=open_record.phases[phase]; if (!p.first) p.first=now;
    } else if (record_open && (state==4 || state==0 || state==1)) close(now,state);
    prev_pass=now; reject_at=0; checks_since=0; prev_phase=phase; prev_paced=paced;
}

// The wrapper returns Host_FilterTime's edx:eax unchanged and passes the float argument bit for bit.
Raw __cdecl probe_filter(std::uint32_t seconds) {
    if (gone || GetCurrentThreadId()!=thread) return orig_filter(seconds);
    int state=-1,number=-1; const bool known=read(g.cls_state,state) && read(g.cl_signon,number);
    std::uint32_t saved=0; const bool paced=known && unspawned_local_signon0(state,number) && pace_begin(saved);
    const Raw raw=orig_filter(seconds);
    if (paced) pace_end(saved);
    if (tracing && enabled_probe) {
        std::uint32_t fps=0; bool fps_read=paced;
        if (paced) fps=saved; else __try { if (g.fps_value) { fps=*g.fps_value; fps_read=true; } } __except(EXCEPTION_EXECUTE_HANDLER) {}
        account(clock_now(),low(raw)!=0,known?state:-1,classify(known,state,number),paced,fps_read?&fps:nullptr);
    }
    return raw;
}
int message_bits(const void* message) {
    __try { return message?static_cast<const volatile int*>(message)[3]:0; } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
Raw __cdecl probe_fragments(int server,void* chan,const void* message,int extra) {
    if (!gone && tracing && enabled_probe && server && record_open && prev_phase!=untracked && GetCurrentThreadId()==thread) {
        const int bits=message_bits(message); auto& p=open_record.phases[prev_phase];
        ++p.fragment_messages; if (bits>0) p.fragment_bytes+=(static_cast<unsigned>(bits)+7)/8;
    }
    return orig_fragments(server,chan,message,extra);
}

double since(Tick base,Tick at) { return at && at>=base?ms(at-base):-1.0; }
void BLSE_CALL report(void*) {
    task_queued=false;
    if (GetCurrentThreadId()!=thread) return;
    char line[512];
    for (unsigned i=0;i<done_count;++i) {
        const auto& r=done[i]; float fps=-1.0f; if (r.fps_known) std::memcpy(&fps,&r.fps_bits,4);
        sprintf_s(line,"Loading signon id=%u start_ms=%.3f complete_ms=%.3f end_state=%d fps_max=%.3f signon_fps_max=%u (host frames from the native load's return to client state 4; work = frame start to the next limiter check, limiter_idle = time the native fps_max limiter held the next frame; ready_wait includes the loader's Enter wait)",
            r.id,since(r.base,r.start),since(r.base,r.complete),static_cast<int>(r.end_state),static_cast<double>(fps),pacing_fps);
        host->log(line);
        for (unsigned k=0;k<phase_count;++k) {
            const auto& p=r.phases[k]; if (!p.first) continue;
            sprintf_s(line,"Loading signon_phase id=%u phase=%s first_ms=%.3f frames=%llu work_ms=%.3f limiter_idle_ms=%.3f max_frame_work_ms=%.3f limiter_checks=%llu paced_frames=%llu fragment_messages=%llu fragment_bytes=%llu",
                r.id,phase_names[k],since(r.base,p.first),p.frames,ms(p.work),ms(p.idle),ms(p.max_work),p.checks,p.paced,p.fragment_messages,p.fragment_bytes);
            host->log(line);
        }
    }
    done_count=0;
    if (done_dropped) { sprintf_s(line,"Loading signon dropped records=%u",done_dropped); host->log(line); done_dropped=0; }
}
void BLSE_CALL on_restart(const char*,const void*,size_t,void*) {
    if (gone || GetCurrentThreadId()!=thread) return;
    // BLSE drops patches in the unloaded engine; nothing here touches the old engine's memory again.
    gone=true; tracing=false; record_open=false; g={};
    host->log("Loading signon hooks stopped after engine restart (fps_max is native; use a fresh process for further signon captures or pacing).");
}

// [Loading] signon_fps_max: missing/empty/0 = off; 66..1000 (the engine's own clamp is 1000).
unsigned configured_pacing() {
    if (!BLSE_HOST_HAS(host,ini_get) || !host->ini_get) return 0;
    char value[16]={}; size_t size=0;
    if (!host->ini_get(host->context,"engine_fixes","Loading","signon_fps_max",value,sizeof value,&size) || size>=sizeof value || !value[0]) return 0;
    unsigned n=0; size_t i=0;
    for (;value[i];++i) { if (value[i]<'0' || value[i]>'9' || i>=5) { n=0xffffffffu; break; } n=n*10+static_cast<unsigned>(value[i]-'0'); }
    if (!n) return 0;
    if (n<66 || n>1000) { host->log("Loading signon_fps_max ignored: use 0 (off) or 66..1000 (300 is the engine default)."); return 0; }
    return n;
}
// fps_max ConVar (registered at engine+0x8ae4c): parent (+4) is itself and name (+0x10) is "fps_max".
bool identify_fps(std::uintptr_t base) {
    __try {
        const auto object=base+0xb42880; const auto name=*reinterpret_cast<const char* const*>(object+0x10);
        if (*reinterpret_cast<const std::uintptr_t*>(object+4)!=object || name!=reinterpret_cast<const char*>(base+0x1a27a0) || std::strcmp(name,"fps_max")) return false;
        g.fps_parent=reinterpret_cast<const volatile std::uintptr_t*>(object+4); g.fps_self=object;
        g.fps_value=reinterpret_cast<volatile std::uint32_t*>(object+0x28); return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void set_globals(std::uintptr_t base) {
    g.cls_state=reinterpret_cast<const volatile int*>(base+0x1dbe50);
    g.cl_signon=reinterpret_cast<const volatile int*>(base+0x20b658);
    g.cls_remote=reinterpret_cast<const volatile int*>(base+0x1dbe5c);
    g.sv_active=reinterpret_cast<const volatile int*>(base+0x12b0754);
    g.maxclients=reinterpret_cast<const volatile int*>(base+0x12af1c8);
    g.clients=reinterpret_cast<const volatile std::uintptr_t*>(base+0x12af1d8);
}
void install_at(std::uintptr_t base,bool trace,unsigned pacing);
// Once per process, in BLSE_PluginLoad after loading::install() (main thread, engine loaded).
void install() {
    if (attempted) return; attempted=true;
    const bool trace=enabled_probe; const unsigned pacing=configured_pacing();
    if (!trace && !pacing) return;
    if (!BLSE_HOST_HAS(host,add_task) || !host->add_task || !BLSE_HOST_HAS(host,hook_function) || !host->hook_function ||
        !BLSE_HOST_HAS(host,subscribe) || !host->subscribe) { host->log("Loading signon disabled: unsupported API."); return; }
    auto module=GetModuleHandleW(L"engine.dll");
    if (!known(module,build_check::engine_profile)) { host->log("Loading signon disabled: unsupported engine."); return; }
    install_at(reinterpret_cast<std::uintptr_t>(module),trace,pacing);
}
// The verified engine image at `base`.
void install_at(std::uintptr_t base,bool trace,unsigned pacing) {
    if (!frequency) { LARGE_INTEGER f; if (QueryPerformanceFrequency(&f) && f.QuadPart>0) frequency=static_cast<Tick>(f.QuadPart); }
    thread=GetCurrentThreadId(); set_globals(base);
    if (pacing && !identify_fps(base)) { host->log("Loading signon_fps_max unavailable: fps_max not identified (native pacing kept)."); pacing=0; }
    if (!host->subscribe(host->context,BLSE_MSG_ENGINE_RESTART,on_restart,nullptr) && pacing) {
        host->log("Loading signon_fps_max unavailable: engine restart not followed (native pacing kept)."); pacing=0;
    }
    if (!trace && !pacing) { g={}; return; }
    if (trace && !pacing && !g.fps_value) identify_fps(base); // reported value only
    const unsigned char filter_code[]={0x55,0x8b,0xec,0x83,0xe4,0xf8};  // push ebp; mov ebp,esp; and esp,-8
    if (!code_hook(base,0x8ba30,filter_code,sizeof filter_code,reinterpret_cast<void*>(probe_filter),reinterpret_cast<void**>(&orig_filter),"host_filter_time")) {
        host->log("Loading signon disabled: Host_FilterTime not hooked (native pacing kept)."); g={}; return;
    }
    pacing_fps=pacing; const float value=static_cast<float>(pacing); std::memcpy(&pacing_bits,&value,4);
    if (trace) {
        const unsigned char fragment_code[]={0x83,0xec,0x10,0x53,0x55}; // sub esp,10h; push ebx; push ebp
        tracing=true;
        if (!code_hook(base,0xbd6e0,fragment_code,sizeof fragment_code,reinterpret_cast<void*>(probe_fragments),reinterpret_cast<void**>(&orig_fragments),"netchan_fragments"))
            host->log("Loading signon: fragment bytes unavailable (frames are still counted).");
    }
    char line[320];
    sprintf_s(line,"Loading signon %s; signon_fps_max=%u%s",trace?"tail diagnostics enabled":"diagnostics off",pacing_fps,
        pacing_fps?" (opt-in: fps_max raised for the frame limiter only while the local client is at signon 0 and unspawned; native value restored each call)":" (native frame pacing)");
    host->log(line);
}
} // namespace signon
} // namespace loading
