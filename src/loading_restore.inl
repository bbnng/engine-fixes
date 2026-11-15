// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Child phases of the server's LevelInit during a timed load (2026-11-14). Read-only diagnostic.
// Include inside engine_fixes.cpp's unnamed namespace AFTER loading_probe.inl and call
// loading::restore::install() right after loading::install() in BLSE_PluginLoad.
//
// Why: 1.1.0.12 captures show server_init (vampire.dll+0x11a7a0, CServerGameDLL::LevelInit)
// with 2641 ms exclusive time on the first save load of a process versus 364 ms on the immediate
// reload of the same save; model loading, model dependencies, render setup and disk opens are
// already separate probe scopes and are NOT in that exclusive time. The static call tree (IDA
// headless on the hash-verified copies) is, for a save load:
//   LevelInit -> engine LoadGameState (CSaveRestore vtable +0x24, engine+0x975f0)
//                  -> read the .HL? state (engine+0x97070)
//                  -> server Restore (CServerGameDLL slot 21, vampire+0x11b300)
//                       -> block handlers' Restore: entities +0x1a2e40, Python +0x19b130,
//                          physics +0x440c0, AI +0x30c210, event queue +0xcff70
//                       -> block handlers' PostRestore (physics +0x447f0; the others are stubs)
//             -> engine LoadAdjacentEnts (+0x97d00, transitions)
//             -> MapEntity_ParseAllEntities (vampire+0x136650, when no saved state exists)
//             -> EndRestoreEntities (vampire+0x11a620: OnRestore of every restored entity)
// Which of these holds the cold-only time decides the next optimization target; this module only
// times them. Each phase records inclusive time, the part spent inside existing probe scopes
// (model_load, disk_open, ...; read from the probe's own scope stack) and its own remaining time,
// keyed by the enclosing phase. Nothing is skipped, cached, reordered or changed: every wrapper
// passes all arguments and the callee's edx:eax through unchanged, and stack cleanup matches each
// native's ret N. Active only while the loading probe is collecting a load (loading_trace=1);
// [Loading] restore_trace=0 turns it off.
#ifndef ENGINE_FIXES_LOADING_RESTORE_INL
#define ENGINE_FIXES_LOADING_RESTORE_INL
namespace loading {
namespace restore {
enum Child { load_state, read_state, server_restore, entities, python, physics, ai, events, physics_post,
    adjacent, on_restore, parse_entities, child_count };
const char* const child_names[]={"engine_load_state","engine_read_state","server_restore","entities_restore",
    "python_restore","physics_restore","ai_restore","event_queue_restore","physics_post_restore",
    "engine_load_adjacent","entities_on_restore","map_entities_parse"};
static_assert(_countof(child_names)==child_count,"names");
constexpr unsigned root=child_count; // enclosing scope is a probe scope (or none), not a phase
struct Stat { Tick calls=0,total=0,own=0,probe=0,maximum=0,models=0,disk_opens=0; int probe_parent=-1; bool mixed=false; };
struct Record { unsigned id=0; Stat stats[child_count][child_count+1]; unsigned overflows=0,unbalanced=0,probe_unknown=0; };
struct Frame { Tick begin=0,child_total=0,child_probe=0,probe_start=0,models=0,disks=0; unsigned main_depth=0,epoch=0,parent=root; Child child=load_state; };
Record open_record,done[4]; bool record_open=false; unsigned done_count=0,done_dropped=0;
Frame frames[16]; unsigned depth=0;
DWORD thread=0; bool attempted=false,gone=false,enabled=false,server_attached=false,report_queued=false,attach_queued=false;
unsigned attach_attempts=0;

// Time the probe's own scopes accumulated as direct children of the probe scope that encloses a
// phase. Phases do not enter the probe's stack, so every probe scope that starts and ends inside
// a phase is added to stack[main_depth-1].child; nothing else changes that counter meanwhile.
bool probe_child(unsigned main_depth,Tick& value) {
    if (!main_depth || main_depth>_countof(loading::stack) || main_depth!=loading::depth) return false;
    value=loading::stack[main_depth-1].child; return true;
}
bool collect() { return enabled && !gone && GetCurrentThreadId()==thread && loading::collecting() && loading::current.kind!=loading::startup; }
void BLSE_CALL report(void*);
void schedule_report() {
    if (report_queued || gone || (!record_open && !done_count && !done_dropped)) return;
    if (host->add_task(host->context,report,nullptr)) report_queued=true;
}
void close_record() {
    if (!record_open) return;
    record_open=false;
    if (done_count<_countof(done)) done[done_count++]=open_record; else ++done_dropped;
}
class Phase {
    bool live_=false; unsigned slot_=0;
public:
    explicit Phase(Child child) {
        if (!collect()) return;
        if (record_open && open_record.id!=loading::current.id) { if (depth) return; close_record(); }
        if (!record_open) { open_record={}; open_record.id=loading::current.id; record_open=true; }
        if (depth==_countof(frames)) { ++open_record.overflows; return; }
        Frame& f=frames[depth]; f={};
        f.child=child; f.parent=depth?frames[depth-1].child:root; f.main_depth=loading::depth; f.epoch=loading::epoch;
        f.models=loading::current.metrics[loading::model].calls; f.disks=loading::current.metrics[loading::disk_open].calls;
        if (!probe_child(f.main_depth,f.probe_start)) f.main_depth=0;
        slot_=depth++; live_=true; f.begin=clock_now();
    }
    void stop() {
        if (!live_) return; live_=false;
        const Tick now=clock_now();
        if (depth!=slot_+1) { ++open_record.unbalanced; depth=0; return; } // never expected: phases nest by call
        const Frame f=frames[--depth];
        if (!record_open || f.epoch!=loading::epoch || !loading::active || open_record.id!=loading::current.id) { ++open_record.unbalanced; return; }
        const Tick elapsed=now-f.begin; Tick probe_now=0,probe=0;
        if (f.main_depth && probe_child(f.main_depth,probe_now) && probe_now>=f.probe_start) probe=probe_now-f.probe_start;
        else ++open_record.probe_unknown; // no enclosing probe scope, or the probe stack moved
        const Tick nested=f.child_total+(probe>f.child_probe?probe-f.child_probe:0);
        Stat& s=open_record.stats[f.child][f.parent];
        ++s.calls; s.total+=elapsed; s.own+=elapsed>nested?elapsed-nested:0; s.probe+=probe;
        if (elapsed>s.maximum) s.maximum=elapsed;
        s.models+=loading::current.metrics[loading::model].calls-f.models;
        s.disk_opens+=loading::current.metrics[loading::disk_open].calls-f.disks;
        if (f.parent==root) {
            const int metric=f.main_depth?static_cast<int>(loading::stack[f.main_depth-1].metric):-1;
            if (s.calls==1) s.probe_parent=metric; else if (s.probe_parent!=metric) s.mixed=true;
        }
        if (depth) { frames[depth-1].child_total+=elapsed; frames[depth-1].child_probe+=probe; }
        else schedule_report();
    }
    bool live() const { return live_; }
    ~Phase() { stop(); }
    Phase(const Phase&)=delete; Phase& operator=(const Phase&)=delete;
};
// Optional observer of the entity block handler (loading_entities.inl). enter/leave run on the main
// thread around the native call, only while the entities phase is being timed (inside its scope);
// attach runs once, after the server phases, with the hash-verified vampire.dll base.
struct EntityObserver { void (*enter)()=nullptr; void (*leave)()=nullptr; void (*attach)(std::uintptr_t)=nullptr; };
EntityObserver entity_observer;

// --- wrappers: thiscall natives through __fastcall (edx unused by every target, checked) ---
using This2=Raw(__thiscall*)(void*,const void*,int);              // ret 8
using This1=Raw(__thiscall*)(void*,const void*);                  // ret 4
using This3=Raw(__thiscall*)(void*,void*,int,int);                // ret 0Ch
using This0=Raw(__thiscall*)(void*);                              // ret
using Plain0=Raw(__cdecl*)();
using Plain1=Raw(__cdecl*)(const char*);
This2 orig_load_state=nullptr,orig_adjacent=nullptr; This1 orig_read_state=nullptr;
This3 orig_server_restore=nullptr,orig_entities=nullptr,orig_python=nullptr,orig_physics=nullptr,orig_ai=nullptr,orig_events=nullptr;
This0 orig_physics_post=nullptr; Plain0 orig_on_restore=nullptr; Plain1 orig_parse=nullptr;
Raw __fastcall probe_load_state(void* self,void*,const void* name,int create) { Phase p(load_state); return orig_load_state(self,name,create); }
Raw __fastcall probe_read_state(void* self,void*,const void* name) { Phase p(read_state); return orig_read_state(self,name); }
Raw __fastcall probe_adjacent(void* self,void*,const void* old,int landmark) { Phase p(adjacent); return orig_adjacent(self,old,landmark); }
Raw __fastcall probe_server_restore(void* self,void*,void* data,int a,int b) { Phase p(server_restore); return orig_server_restore(self,data,a,b); }
Raw __fastcall probe_entities(void* self,void*,void* data,int a,int b) {
    Phase p(entities);
    const bool observed=p.live() && entity_observer.enter && entity_observer.leave;
    if (observed) entity_observer.enter();
    const Raw result=orig_entities(self,data,a,b);
    if (observed) entity_observer.leave();
    return result;
}
Raw __fastcall probe_python(void* self,void*,void* data,int a,int b) { Phase p(python); return orig_python(self,data,a,b); }
Raw __fastcall probe_physics(void* self,void*,void* data,int a,int b) { Phase p(physics); return orig_physics(self,data,a,b); }
Raw __fastcall probe_ai(void* self,void*,void* data,int a,int b) { Phase p(ai); return orig_ai(self,data,a,b); }
Raw __fastcall probe_events(void* self,void*,void* data,int a,int b) { Phase p(events); return orig_events(self,data,a,b); }
Raw __fastcall probe_physics_post(void* self,void*) { Phase p(physics_post); return orig_physics_post(self); }
Raw __cdecl probe_on_restore() { Phase p(on_restore); return orig_on_restore(); }
Raw __cdecl probe_parse(const char* entities_text) { Phase p(parse_entities); return orig_parse(entities_text); }

const char* parent_name(const Stat& s,unsigned parent) {
    if (parent<child_count) return child_names[parent];
    if (s.mixed) return "mixed";
    if (s.probe_parent>=0 && s.probe_parent<static_cast<int>(loading::metric_count)) return loading::metric_names[s.probe_parent];
    return "none";
}
// Logs records whose capture has finished (never while a load is being timed).
void log_done() {
    char line[400];
    for (unsigned i=0;i<done_count;++i) {
        const Record& r=done[i];
        sprintf_s(line,"Loading restore id=%u overflows=%u unbalanced=%u probe_unknown=%u (child phases of server LevelInit; inclusive includes nested phases and probe scopes, probe = time in nested loading-probe scopes such as model_load/disk_open, own = the rest)",
            r.id,r.overflows,r.unbalanced,r.probe_unknown);
        host->log(line);
        for (unsigned c=0;c<child_count;++c) for (unsigned p=0;p<=child_count;++p) {
            const Stat& s=r.stats[c][p]; if (!s.calls) continue;
            sprintf_s(line,"Loading restore_phase id=%u phase=%s parent=%s calls=%llu inclusive_ms=%.3f own_ms=%.3f probe_ms=%.3f max_ms=%.3f model_loads=%llu disk_opens=%llu",
                r.id,child_names[c],parent_name(s,p),s.calls,ms(s.total),ms(s.own),ms(s.probe),ms(s.maximum),s.models,s.disk_opens);
            host->log(line);
        }
    }
    done_count=0;
    if (done_dropped) { sprintf_s(line,"Loading restore dropped records=%u",done_dropped); host->log(line); done_dropped=0; }
}
void BLSE_CALL report(void*) {
    report_queued=false;
    if (GetCurrentThreadId()!=thread || gone) return;
    const bool finished=record_open && !depth && (!loading::active || loading::current.id!=open_record.id);
    log_done();
    if (finished) { close_record(); log_done(); }
    if (record_open) schedule_report(); // capture still running: look again next frame
}

// vampire.dll 996e0245... (UP12): RVAs and bytes verified with IDA headless on the hash-identical
// copy build\loading-research\vampire-UP12.dll. Each byte run is whole instructions.
struct Site { unsigned rva; unsigned char code[8]; unsigned char size; void* detour; void** original; const char* label; };
const char* const server_hash="996e024577290cf0c91d74fcccd144089f1db2a1f8ef207844d1fa078939c3c8";
unsigned hook_sites(std::uintptr_t base,const Site* sites,unsigned count) {
    unsigned hooked=0;
    for (unsigned i=0;i<count;++i)
        if (code_hook(base,sites[i].rva,sites[i].code,sites[i].size,sites[i].detour,sites[i].original,sites[i].label)) ++hooked;
    return hooked;
}
unsigned hook_server(std::uintptr_t base) {
    Site sites[]={
        {0x11b300,{0x83,0xec,0x30,0x53,0x56,0x57},6,reinterpret_cast<void*>(probe_server_restore),reinterpret_cast<void**>(&orig_server_restore),"restore_server"},
        {0x1a2e40,{0x83,0xec,0x14,0x89,0x4c,0x24,0x08},7,reinterpret_cast<void*>(probe_entities),reinterpret_cast<void**>(&orig_entities),"restore_entities"},
        {0x19b130,{0x51,0x8a,0x41,0x04,0x56},5,reinterpret_cast<void*>(probe_python),reinterpret_cast<void**>(&orig_python),"restore_python"},
        {0x0440c0,{0x83,0xec,0x64,0x53,0x8b,0xd9},6,reinterpret_cast<void*>(probe_physics),reinterpret_cast<void**>(&orig_physics),"restore_physics"},
        {0x30c210,{0x81,0xec,0x94,0x00,0x00,0x00},6,reinterpret_cast<void*>(probe_ai),reinterpret_cast<void**>(&orig_ai),"restore_ai"},
        {0x0cff70,{0x8a,0x41,0x04,0x84,0xc0},5,reinterpret_cast<void*>(probe_events),reinterpret_cast<void**>(&orig_events),"restore_event_queue"},
        {0x0447f0,{0x51,0x53,0x55,0x56,0x8b,0xf1},6,reinterpret_cast<void*>(probe_physics_post),reinterpret_cast<void**>(&orig_physics_post),"restore_physics_post"},
        {0x11a620,{0xa0,0,0,0,0},5,reinterpret_cast<void*>(probe_on_restore),reinterpret_cast<void**>(&orig_on_restore),"restore_on_restore"},
        {0x136650,{0xb8,0x04,0x01,0x01,0x00},5,reinterpret_cast<void*>(probe_parse),reinterpret_cast<void**>(&orig_parse),"restore_parse_entities"},
    };
    absolute(sites[7].code+1,base+0x70b265); // mov al,[restoring flag]
    return hook_sites(base,sites,_countof(sites));
}
unsigned hook_engine(std::uintptr_t base) {
    const Site sites[]={
        {0x975f0,{0x81,0xec,0x98,0x01,0x00,0x00},6,reinterpret_cast<void*>(probe_load_state),reinterpret_cast<void**>(&orig_load_state),"restore_load_state"},
        {0x97070,{0x8b,0x01,0x81,0xec,0x1c,0x01,0x00,0x00},8,reinterpret_cast<void*>(probe_read_state),reinterpret_cast<void**>(&orig_read_state),"restore_read_state"},
        {0x97d00,{0xb8,0xec,0x14,0x00,0x00},5,reinterpret_cast<void*>(probe_adjacent),reinterpret_cast<void**>(&orig_adjacent),"restore_load_adjacent"},
    };
    return hook_sites(base,sites,_countof(sites));
}
// Server bodies are hooked only after BLSE's server-interface wrappers are in place (the same
// order the probe uses), from a main-thread task that retries while the menu runs.
void BLSE_CALL attach_task(void*) {
    attach_queued=false;
    if (GetCurrentThreadId()!=thread || gone || server_attached) return;
    auto server=GetModuleHandleW(L"vampire.dll");
    if (server && blse_owns(reinterpret_cast<std::uintptr_t>(server))) {
        server_attached=true;
        if (!server_profile_supported() || !exact(server,server_hash)) { host->log("Loading restore: server phases unavailable (unverified vampire.dll); engine phases only."); return; }
        const unsigned n=hook_server(reinterpret_cast<std::uintptr_t>(server));
        char line[128]; sprintf_s(line,"Loading restore: %u/9 server phase hooks installed.",n); host->log(line);
        if (orig_entities && entity_observer.attach) entity_observer.attach(reinterpret_cast<std::uintptr_t>(server));
        return;
    }
    if (++attach_attempts>=4096) { server_attached=true; host->log("Loading restore: server phases unavailable (server interface not ready)."); return; }
    if (host->add_task(host->context,attach_task,nullptr)) attach_queued=true;
}
void BLSE_CALL on_restart(const char*,const void*,size_t,void*) {
    if (gone || GetCurrentThreadId()!=thread) return;
    // BLSE dropped the patches in the unloaded engine/server; detours are never entered again.
    gone=true; record_open=false; depth=0;
    host->log("Loading restore phases stopped after engine restart (use a fresh process for further captures).");
}
bool configured_off() {
    if (!BLSE_HOST_HAS(host,ini_get) || !host->ini_get) return false;
    char value[8]={}; size_t size=0;
    return host->ini_get(host->context,"engine_fixes","Loading","restore_trace",value,sizeof value,&size) && size<sizeof value && value[0]=='0';
}
void install_at(std::uintptr_t engine);
// Once per process, in BLSE_PluginLoad after loading::install() (main thread, engine loaded).
void install() {
    if (attempted) return; attempted=true;
    if (!loading::enabled_probe) return;            // probe off or refused: nothing to attribute to
    if (configured_off()) { host->log("Loading restore phases off ([Loading] restore_trace=0)."); return; }
    install_at(loading::engine_base);
}
void install_at(std::uintptr_t engine) {
    thread=GetCurrentThreadId();
    if (!host->subscribe(host->context,BLSE_MSG_ENGINE_RESTART,on_restart,nullptr)) {
        host->log("Loading restore phases disabled: engine restart not followed."); return;
    }
    enabled=true;
    const unsigned n=hook_engine(engine);
    char line[160]; sprintf_s(line,"Loading restore phases enabled: %u/3 engine phase hooks; server phases after BLSE's server hooks (read-only timing).",n); host->log(line);
    if (host->add_task(host->context,attach_task,nullptr)) attach_queued=true;
    else { server_attached=true; host->log("Loading restore: server phases unavailable (task queue refused)."); }
}
} // namespace restore
} // namespace loading
#endif
