// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Entity-restore breakdown during a timed save load (2026-11-14). Read-only diagnostic.
// Include inside engine_fixes.cpp's unnamed namespace AFTER loading_restore.inl and call
// loading::entities::install() right after loading::restore::install() in BLSE_PluginLoad.
//
// Why: the 1.1.0.13 capture puts nearly all of server_init's own time in the entity block handler
// (vampire.dll+0x1a2e40, CEntitySaveRestoreBlockHandler::Restore): 2695 ms own on the first save
// load of a process and 361 ms on the reload, outside every existing probe scope. IDA headless on
// the hash-verified copy shows two loops:
//   loop 1, per saved entity: CreateEntityByName (+0x136580: GetProcAddress(vampire.dll, class)
//           then a tail jump into the exported factory). worldspawn is also restored here through
//           RestoreEntity (+0x1a3380); the player is created through +0x15d790.
//   loop 2, per entity: CHandle set (+0xa0ae0), Restore (vfunc +1FCh: datamap field reads),
//           ObjectCaps (+1D4h), Spawn (+19Ch, FCAP_MUST_SPAWN) or Precache (+1A0h), the first
//           handle lookup (+0x290c0), think stamp and global-entity checks, then the
//           to-activate list insert (+0x11a580).
// The handler's own call sites split each entity into
//   create  = CreateEntityByName (incl. the player creation)
//   restore = CHandle set + Restore (field reads and the class's Restore override)
//   spawn   = ObjectCaps + Spawn or Precache (for worldspawn also RestoreEntity's tail)
//   post    = handle lookups, think stamp, global checks, list insert
//   other   = the rest of the handler (loop bookkeeping, IRestore read position, edict index)
// per entity class (m_iClassname +11Ch, read after Restore), with time inside existing probe
// scopes (model_load, disk_open, ...) separated exactly as the restore phases do. A sampler thread
// records the main thread's instruction pointer and validated return addresses, in memory and
// only while the handler runs, so one capture names the native code that holds the time.
//
// Nothing is skipped, cached, reordered or retried. Every call-site wrapper forwards the arguments
// and returns the callee's raw edx:eax with the native stack cleanup; the two mid-function markers
// (whole instructions "mov edx,[esi]; mov ecx,esi; call [edx+1D4h]", moved into BLSE's trampoline)
// preserve all registers, flags and GetLastError. Active only while the restore phases time the
// entity handler of a load (loading_trace=1, restore_trace=1); [Loading] entity_trace=0 turns it
// off, entity_sampler=0 keeps the breakdown without the sampler. Results are logged by a
// main-thread task after the capture ends; nothing is logged or allocated during the load.
#ifndef ENGINE_FIXES_LOADING_ENTITIES_INL
#define ENGINE_FIXES_LOADING_ENTITIES_INL
namespace loading {
namespace entities {
enum Part : unsigned { part_create, part_restore, part_spawn, part_post, part_other, part_count };
const char* const part_names[]={"create","restore","spawn","post","other"};
static_assert(_countof(part_names)==part_count,"names");
constexpr unsigned class_capacity=160;              // the last slot collects classes beyond the table
constexpr int rest_class=static_cast<int>(class_capacity)-1;
struct Stat { Tick calls=0,total=0,probe=0,maximum=0,models=0,disks=0; };
struct Class { std::uint32_t hash=0; bool used=false; char name[48]={}; Tick entities=0,creates=0,entity_max=0; Stat parts[part_count]; };
enum SamplerState : unsigned { sampler_off, sampler_on, sampler_unavailable, sampler_busy };
const char* const sampler_states[]={"off","on","unavailable","busy"};
struct Record {
    unsigned id=0,phases=0,class_count=0,class_overflow=0,sequence_errors=0,post_unended=0,probe_unknown=0,world=0,name_faults=0;
    Tick entities=0,creates=0,total=0,setup=0;
    SamplerState sampler=sampler_off;
    Stat parts[part_count]; Class classes[class_capacity];
};
static_assert(std::is_trivially_copyable<Record>::value,"records are reset with memset");
Record open_record,done[2]; bool record_open=false; unsigned done_count=0,done_dropped=0;
DWORD thread=0;
bool attempted=false,enabled=false,markers=false,gone=false,report_queued=false,observing=false,sampler_wanted=false;

// --- main-thread sampler (bounded, in memory; reads only) -------------------------------------
namespace sampling {
constexpr unsigned frames=32,capacity=6144,range_capacity=192,module_capacity=128;
constexpr std::uintptr_t scan_bytes=32*1024,stack_window=8u*1024*1024;
constexpr DWORD period_ms=4;
struct Sample { std::uint32_t eip=0; std::uint8_t part=0,count=0; std::uint16_t cls=0; std::uint32_t ra[frames]={}; };
struct Range { std::uintptr_t begin=0,end=0; unsigned module=0; };
Sample samples[capacity];
volatile LONG count=0,overflow=0,failures=0,armed=0,stop_requested=0,tag_part=part_other,tag_cls=-1;
volatile LONGLONG pause_ticks=0;
Range ranges[range_capacity]; unsigned range_count=0,range_overflow=0;
std::uintptr_t modules[module_capacity]={}; unsigned module_count=0;
const NT_TIB* target_tib=nullptr; // the main thread's TIB (StackBase/StackLimit; always mapped)
HANDLE worker=nullptr,wake=nullptr,target=nullptr,timer=nullptr;
unsigned owner=0;            // record id whose samples the buffer holds (0 = free)
Tick armed_ticks=0,arm_start=0;

// Return-address check: v is inside an executable, readable module section and the bytes before
// it are a call (E8 rel32; FF /2 with modrm/sib/disp forms: 2, 3, 4, 6 or 7 bytes).
const Range* find(std::uintptr_t v) {
    unsigned lo=0,hi=range_count;
    while (lo<hi) { const unsigned mid=(lo+hi)/2; if (v<ranges[mid].begin) hi=mid; else if (v>=ranges[mid].end) lo=mid+1; else return &ranges[mid]; }
    return nullptr;
}
bool call_before(const unsigned char* p,std::uintptr_t room) {
    auto call_modrm=[](unsigned char m) { return ((m>>3)&7)==2; };
    if (room>=5 && p[-5]==0xe8) return true;
    if (room>=2 && p[-2]==0xff && call_modrm(p[-1]) && ((p[-1]>>6)==3 || ((p[-1]>>6)==0 && (p[-1]&7)!=4 && (p[-1]&7)!=5))) return true;
    if (room>=3 && p[-3]==0xff && call_modrm(p[-2]) && (((p[-2]>>6)==1 && (p[-2]&7)!=4) || ((p[-2]>>6)==0 && (p[-2]&7)==4 && (p[-1]&7)!=5))) return true;
    if (room>=4 && p[-4]==0xff && call_modrm(p[-3]) && (p[-3]>>6)==1 && (p[-3]&7)==4) return true;
    if (room>=6 && p[-6]==0xff && call_modrm(p[-5]) && (((p[-5]>>6)==2 && (p[-5]&7)!=4) || ((p[-5]>>6)==0 && (p[-5]&7)==5))) return true;
    if (room>=7 && p[-7]==0xff && call_modrm(p[-6]) && (((p[-6]>>6)==2 && (p[-6]&7)==4) || ((p[-6]>>6)==0 && (p[-6]&7)==4 && (p[-5]&7)==5))) return true;
    return false;
}
bool is_return(std::uintptr_t v) {
    const Range* r=find(v);
    return r && v>r->begin && call_before(reinterpret_cast<const unsigned char*>(v),v-r->begin);
}
// Up to `frames` return addresses from esp upward. The thread is suspended and [esp, base) lies
// within its committed stack (esp at or above the TIB's StackLimit). Ranges were built while the
// sampler was disarmed.
unsigned scan(std::uintptr_t esp,std::uintptr_t limit,std::uintptr_t base,std::uint32_t* out) {
    if (!base || esp>=base || esp<limit || base-esp>stack_window) return 0;
    std::uintptr_t p=(esp+3)&~static_cast<std::uintptr_t>(3);
    const std::uintptr_t end=base-p>scan_bytes?p+scan_bytes:base;
    unsigned n=0;
    for (;p+4<=end && n<frames;p+=4) {
        const std::uintptr_t v=*reinterpret_cast<const std::uint32_t*>(p);
        if (is_return(v)) out[n++]=static_cast<std::uint32_t>(v);
    }
    return n;
}
// One sample: everything between SuspendThread and ResumeThread reads memory only (no heap, no
// locks, no logging), so the suspended main thread cannot hold anything the sampler needs.
void take() {
    const Tick start=clock_now();
    if (SuspendThread(target)==static_cast<DWORD>(-1)) { InterlockedIncrement(&failures); return; }
    __try {
    __try {
    CONTEXT context; std::memset(&context,0,sizeof context); context.ContextFlags=CONTEXT_CONTROL;
    const BOOL got=GetThreadContext(target,&context); // also waits until the suspension took effect
    if (!got) InterlockedIncrement(&failures);
    else if (InterlockedCompareExchange(&armed,0,0)) {
        const LONG n=InterlockedCompareExchange(&count,0,0);
        if (n<static_cast<LONG>(capacity)) {
            Sample& s=samples[n];
            s.eip=static_cast<std::uint32_t>(context.Eip);
            s.part=static_cast<std::uint8_t>(InterlockedCompareExchange(&tag_part,0,0));
            s.cls=static_cast<std::uint16_t>(InterlockedCompareExchange(&tag_cls,0,0)&0xffff);
            s.count=static_cast<std::uint8_t>(scan(context.Esp,reinterpret_cast<std::uintptr_t>(target_tib->StackLimit),
                                                   reinterpret_cast<std::uintptr_t>(target_tib->StackBase),s.ra));
            InterlockedExchange(&count,n+1);
        } else InterlockedIncrement(&overflow);
    }
    } __finally {
        // A stack/code page can disappear after range enumeration. Always release the
        // main thread even if a guarded sample faults; never leave it suspended.
        if (ResumeThread(target)==static_cast<DWORD>(-1)) InterlockedIncrement(&failures);
    }
    } __except(EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&failures); }
    InterlockedExchangeAdd64(&pause_ticks,static_cast<LONGLONG>(clock_now()-start));
}
DWORD WINAPI run(void*) {
    while (!InterlockedCompareExchange(&stop_requested,0,0)) {
        if (!InterlockedCompareExchange(&armed,0,0)) { WaitForSingleObject(wake,INFINITE); continue; }
        // About 4 ms between samples: a high-resolution waitable timer where Windows has one
        // (10 1803+), else the process timer resolution (the game runs at 1 ms). Each sample holds
        // the main thread for the suspend/context/resume round trip (WOW64: roughly 0.1-0.2 ms);
        // 4 ms keeps that near 3 % while a 3.7 s cold handler still yields about 900 samples.
        LARGE_INTEGER due; due.QuadPart=-static_cast<LONGLONG>(period_ms)*10000;
        if (!timer || !SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE) || WaitForSingleObject(timer,50)!=WAIT_OBJECT_0) WaitForSingleObject(wake,period_ms);
        if (InterlockedCompareExchange(&stop_requested,0,0)) break;
        if (InterlockedCompareExchange(&armed,0,0)) take();
    }
    return 0;
}
bool start(HANDLE thread_handle,const NT_TIB* tib) {
    if (worker) return true;
    if (!thread_handle || !tib) return false;
    InterlockedExchange(&stop_requested,0); InterlockedExchange(&armed,0);
    wake=CreateEventW(nullptr,FALSE,FALSE,nullptr); if (!wake) return false;
    target=thread_handle; target_tib=tib;
    if (!timer) timer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/,TIMER_MODIFY_STATE|SYNCHRONIZE);
    worker=CreateThread(nullptr,64*1024,run,nullptr,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
    if (!worker) { CloseHandle(wake); wake=nullptr; return false; }
    return true;
}
bool stop(DWORD timeout) {
    InterlockedExchange(&armed,0);
    if (!worker) return true;
    InterlockedExchange(&stop_requested,1); SetEvent(wake);
    if (WaitForSingleObject(worker,timeout)!=WAIT_OBJECT_0) return false; // retained, never terminated
    CloseHandle(worker); CloseHandle(wake); worker=nullptr; wake=nullptr;
    if (timer) { CloseHandle(timer); timer=nullptr; }
    return true;
}
void tag(unsigned part,int cls) { InterlockedExchange(&tag_part,static_cast<LONG>(part)); InterlockedExchange(&tag_cls,cls); }
// Executable sections of a loaded image whose pages are readable (no execute-only pages).
void add_module(std::uintptr_t base) {
    if (module_count>=module_capacity) return;
    const unsigned index=module_count;
    bool any=false;
    __try {
        const auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic!=IMAGE_DOS_SIGNATURE) return;
        const auto nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
        if (nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386) return;
        const IMAGE_SECTION_HEADER* section=IMAGE_FIRST_SECTION(nt);
        for (unsigned i=0;i<nt->FileHeader.NumberOfSections;++i,++section) {
            if (!(section->Characteristics&IMAGE_SCN_MEM_EXECUTE) || !section->Misc.VirtualSize) continue;
            const std::uintptr_t begin=base+section->VirtualAddress,end=begin+section->Misc.VirtualSize;
            bool readable=true;
            for (std::uintptr_t p=begin;p<end && readable;) {
                MEMORY_BASIC_INFORMATION info; if (!VirtualQuery(reinterpret_cast<const void*>(p),&info,sizeof info)) { readable=false; break; }
                const DWORD protect=info.Protect&0xff;
                readable=info.State==MEM_COMMIT && !(info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) &&
                         (protect==PAGE_EXECUTE_READ || protect==PAGE_EXECUTE_READWRITE || protect==PAGE_EXECUTE_WRITECOPY ||
                          protect==PAGE_READONLY || protect==PAGE_READWRITE || protect==PAGE_WRITECOPY);
                p=reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
            }
            if (!readable) continue;
            if (range_count>=range_capacity) { ++range_overflow; continue; }
            ranges[range_count++]={begin,end,index}; any=true;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    if (any) modules[module_count++]=base;
}
void sort_ranges() {
    for (unsigned i=1;i<range_count;++i) { const Range r=ranges[i]; unsigned j=i; while (j && ranges[j-1].begin>r.begin) { ranges[j]=ranges[j-1]; --j; } ranges[j]=r; }
}
using EnumModules=BOOL(WINAPI*)(HANDLE,HMODULE*,DWORD,LPDWORD);
EnumModules enum_modules=nullptr;
// Main thread, sampler disarmed: the module list may have changed since the last load.
void build_ranges() {
    range_count=range_overflow=module_count=0;
    if (enum_modules) {
        HMODULE list[module_capacity]; DWORD needed=0;
        if (enum_modules(GetCurrentProcess(),list,sizeof list,&needed)) {
            const unsigned n=needed/sizeof(HMODULE)<module_capacity?needed/sizeof(HMODULE):module_capacity;
            for (unsigned i=0;i<n;++i) add_module(reinterpret_cast<std::uintptr_t>(list[i]));
        }
    }
    sort_ranges();
}
void release() {
    InterlockedExchange(&count,0); InterlockedExchange(&overflow,0); InterlockedExchange(&failures,0);
    InterlockedExchange64(&pause_ticks,0); owner=0; armed_ticks=0;
}
} // namespace sampling

// --- attribution state (main thread) ---------------------------------------------------------
struct Segment { Part part=part_other; int cls=-1; Tick begin=0,probe_start=0,models=0,disks=0; bool probe_ok=false,open=false; };
Segment segment; void* entity=nullptr; int entity_cls=-1; Tick entity_ticks=0; bool entity_pending=false,world_entity=false;
unsigned main_depth=0; Tick enter_tick=0;

bool probe_now(Tick& value) {
    if (!main_depth || main_depth>_countof(loading::stack) || main_depth!=loading::depth) return false;
    value=loading::stack[main_depth-1].child; return true;
}
bool live() {
    return observing && !gone && GetCurrentThreadId()==thread && loading::collecting() && record_open && open_record.id==loading::current.id;
}
// Class names are read under SEH (game strings); FNV-1a over the whole name (case-sensitive, as
// the game's GetProcAddress lookup), the first 47 bytes kept for the report.
bool read_name(const char* name,char* copy,size_t capacity,std::uint32_t& hash) {
    hash=2166136261u; copy[0]=0;
    __try {
        size_t i=0;
        for (;name[i] && i<256;++i) {
            hash=(hash^static_cast<unsigned char>(name[i]))*16777619u;
            if (i+1<capacity) { const unsigned char c=static_cast<unsigned char>(name[i]); copy[i]=(c<32 || c==127)?'_':static_cast<char>(c); }
        }
        copy[i+1<capacity?i:capacity-1]=0;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { copy[0]=0; return false; }
}
int class_slot(const char* name) {
    char copy[48]; std::uint32_t hash=0;
    if (!name) { strcpy_s(copy,"(null)"); hash=1; }
    else if (!read_name(name,copy,sizeof copy,hash)) { ++open_record.name_faults; strcpy_s(copy,"(unreadable)"); hash=2; }
    const unsigned slots=class_capacity-1;
    for (unsigned i=0,at=hash%slots;i<slots;++i,at=(at+1)%slots) {
        Class& c=open_record.classes[at];
        if (!c.used) { c.used=true; c.hash=hash; strcpy_s(c.name,copy); ++open_record.class_count; return static_cast<int>(at); }
        if (c.hash==hash && !std::strcmp(c.name,copy)) return static_cast<int>(at);
    }
    ++open_record.class_overflow;
    Class& rest=open_record.classes[rest_class];
    if (!rest.used) { rest.used=true; strcpy_s(rest.name,"(other classes)"); }
    return rest_class;
}
int class_of(void* e) {
    const char* name=nullptr;
    __try { name=*reinterpret_cast<const char* const*>(static_cast<const unsigned char*>(e)+0x11c); }
    __except(EXCEPTION_EXECUTE_HANDLER) { ++open_record.name_faults; return class_slot(nullptr); }
    return class_slot(name);
}
void add(Stat& s,Tick elapsed,Tick probe,Tick models,Tick disks) {
    ++s.calls; s.total+=elapsed; s.probe+=probe; s.models+=models; s.disks+=disks; if (elapsed>s.maximum) s.maximum=elapsed;
}
void open_segment(Part part,int cls,Tick now) {
    segment.part=part; segment.cls=cls; segment.begin=now; segment.open=true;
    segment.probe_ok=probe_now(segment.probe_start);
    segment.models=loading::current.metrics[loading::model].calls; segment.disks=loading::current.metrics[loading::disk_open].calls;
    sampling::tag(part,cls);
}
void close_segment(Tick now,int cls) {
    if (!segment.open) return; segment.open=false;
    const Tick elapsed=now-segment.begin; Tick value=0,probe=0;
    if (segment.probe_ok && probe_now(value) && value>=segment.probe_start) probe=value-segment.probe_start; else ++open_record.probe_unknown;
    const Tick models=loading::current.metrics[loading::model].calls-segment.models;
    const Tick disks=loading::current.metrics[loading::disk_open].calls-segment.disks;
    add(open_record.parts[segment.part],elapsed,probe,models,disks);
    if (cls<0) cls=segment.cls;
    if (cls>=0) add(open_record.classes[cls].parts[segment.part],elapsed,probe,models,disks);
    if (segment.part==part_restore || segment.part==part_spawn || segment.part==part_post) entity_ticks+=elapsed;
}
void finish_entity() {
    if (!entity_pending) return;
    entity_pending=false; ++open_record.entities;
    if (entity_cls>=0) { Class& c=open_record.classes[entity_cls]; ++c.entities; if (entity_ticks>c.entity_max) c.entity_max=entity_ticks; }
    entity=nullptr; entity_cls=-1; entity_ticks=0; world_entity=false;
}
// A previous entity still in post means the native skipped its list insert (a global entity
// removed by the global-state check): not an error. Anything else out of order is counted.
void settle_previous(Tick now) {
    if (segment.part==part_post) ++open_record.post_unended;
    else if (segment.part!=part_other) ++open_record.sequence_errors;
    close_segment(now,-1); finish_entity();
}

// --- markers (main thread; each preserves GetLastError) ---
void begin_entity(void* e,bool world) {
    const DWORD error=GetLastError(); const Tick now=clock_now();
    settle_previous(now);
    entity=e; entity_pending=true; world_entity=world; entity_ticks=0; entity_cls=-1; if (world) ++open_record.world;
    open_segment(part_restore,-1,now);
    SetLastError(error);
}
void restored(bool world) {
    const DWORD error=GetLastError();
    if (segment.part!=part_restore || !entity_pending || world_entity!=world) ++open_record.sequence_errors;
    else { const Tick now=clock_now(); entity_cls=class_of(entity); close_segment(now,entity_cls); open_segment(part_spawn,entity_cls,now); }
    SetLastError(error);
}
void spawned() {
    const DWORD error=GetLastError();
    if (segment.part!=part_spawn || world_entity) ++open_record.sequence_errors;
    else { const Tick now=clock_now(); close_segment(now,entity_cls); open_segment(part_post,entity_cls,now); }
    SetLastError(error);
}
void entity_done(bool world,void* e) {
    const DWORD error=GetLastError();
    const bool expected=world?(world_entity && entity_pending && (segment.part==part_spawn || segment.part==part_restore))
                              :(!world_entity && segment.part==part_post);
    if (!expected) ++open_record.sequence_errors;
    else {
        const Tick now=clock_now();
        if (world && entity_cls<0) entity_cls=class_of(e);
        close_segment(now,entity_cls); finish_entity(); open_segment(part_other,-1,now);
    }
    SetLastError(error);
}
void create_begin(const char* name) {
    const DWORD error=GetLastError(); const Tick now=clock_now();
    settle_previous(now);
    const int cls=class_slot(name); ++open_record.classes[cls].creates; ++open_record.creates;
    open_segment(part_create,cls,now);
    SetLastError(error);
}
void create_end() {
    const DWORD error=GetLastError();
    if (segment.part!=part_create) ++open_record.sequence_errors;
    else { const Tick now=clock_now(); close_segment(now,-1); open_segment(part_other,-1,now); }
    SetLastError(error);
}

// --- call-site wrappers (thiscall natives through __fastcall; edx is never read by the targets) ---
using CreateFn=Raw(__cdecl*)(const char*);               // CreateEntityByName thunk, cdecl
using PlayerFn=Raw(__cdecl*)(const char*,int);           // player creation, cdecl
using WorldFn=Raw(__thiscall*)(void*,void*,void*);       // RestoreEntity, ret 8 (ecx unused)
using ThisArg=Raw(__thiscall*)(void*,void*);             // CHandle set / handle lookup, ret 4
using EndFn=Raw(__cdecl*)(void*);                        // to-activate list insert, cdecl
CreateFn orig_create_world=nullptr,orig_create=nullptr; PlayerFn orig_create_player=nullptr; WorldFn orig_world=nullptr;
ThisArg orig_begin=nullptr,orig_spawned=nullptr; EndFn orig_end=nullptr;
void* orig_restored=nullptr; void* orig_world_restored=nullptr;
Raw create_through(CreateFn fn,const char* name) {
    const bool on=live(); if (on) create_begin(name);
    const Raw result=fn(name);
    if (on && live()) create_end();
    return result;
}
Raw __cdecl on_create_world(const char* name) { return create_through(orig_create_world,name); }
Raw __cdecl on_create(const char* name) { return create_through(orig_create,name); }
Raw __cdecl on_create_player(const char* name,int edict) {
    const bool on=live(); if (on) create_begin(name);
    const Raw result=orig_create_player(name,edict);
    if (on && live()) create_end();
    return result;
}
Raw __fastcall on_world(void* self,void*,void* e,void* restore) {
    const bool on=live(); if (on) begin_entity(e,true);
    const Raw result=orig_world(self,e,restore);
    if (on && live()) entity_done(true,e);
    return result;
}
Raw __fastcall on_begin(void* self,void*,void* e) { if (live()) begin_entity(e,false); return orig_begin(self,e); }
Raw __fastcall on_spawned(void* self,void*,void* handle) { if (live()) spawned(); return orig_spawned(self,handle); }
Raw __cdecl on_end(void* e) { const Raw result=orig_end(e); if (live()) entity_done(false,e); return result; }
// Mid-function markers: entered by BLSE's jump in place of the three moved instructions, which
// then run unchanged in the trampoline. All registers, flags (DF too) and the x87 state survive.
void __cdecl restored_marker() { if (live()) restored(false); }
void __cdecl world_restored_marker() { if (live()) restored(true); }
__declspec(naked) void restored_detour() {
    __asm {
        pushfd
        pushad
        cld
        call restored_marker
        popad
        popfd
        jmp dword ptr [orig_restored]
    }
}
__declspec(naked) void world_restored_detour() {
    __asm {
        pushfd
        pushad
        cld
        call world_restored_marker
        popad
        popfd
        jmp dword ptr [orig_world_restored]
    }
}

// --- observer callbacks (from loading_restore.inl's entities phase) ---
void BLSE_CALL report(void*);
void schedule_report() {
    if (report_queued || gone || (!record_open && !done_count && !done_dropped)) return;
    if (host->add_task(host->context,report,nullptr)) report_queued=true;
}
void close_record() {
    if (!record_open) return;
    record_open=false;
    if (done_count<_countof(done)) std::memcpy(&done[done_count++],&open_record,sizeof open_record);
    else { ++done_dropped; if (sampling::owner==open_record.id) sampling::release(); }
}
void enter() {
    if (!enabled || !markers || gone || observing || GetCurrentThreadId()!=thread || !loading::collecting()) return;
    const Tick start=clock_now();
    if (record_open && open_record.id!=loading::current.id) close_record();
    if (!record_open) { std::memset(&open_record,0,sizeof open_record); open_record.id=loading::current.id; record_open=true; }
    ++open_record.phases; main_depth=loading::depth;
    if (sampler_wanted) {
        if (!sampling::worker) open_record.sampler=sampler_unavailable;
        else if (sampling::owner && sampling::owner!=open_record.id) open_record.sampler=sampler_busy;
        else {
            if (!sampling::owner) { sampling::release(); sampling::owner=open_record.id; }
            sampling::build_ranges(); open_record.sampler=sampler_on;
        }
    }
    segment={}; entity=nullptr; entity_pending=false; entity_cls=-1; entity_ticks=0; world_entity=false;
    observing=true;
    const Tick now=clock_now(); open_record.setup+=now-start; enter_tick=start;
    open_segment(part_other,-1,now);
    if (open_record.sampler==sampler_on) { sampling::arm_start=now; InterlockedExchange(&sampling::armed,1); SetEvent(sampling::wake); }
}
void leave() {
    if (!observing) return;
    InterlockedExchange(&sampling::armed,0);
    const Tick now=clock_now();
    if (open_record.sampler==sampler_on) sampling::armed_ticks+=now-sampling::arm_start;
    settle_previous(now);
    open_record.total+=now-enter_tick;
    observing=false; entity=nullptr;
    schedule_report();
}

// --- report (after the capture ended; main thread) ---
Tick own(const Stat& s) { return s.total>s.probe?s.total-s.probe:0; }
Tick class_own(const Class& c) { Tick t=0; for (const auto& s:c.parts) t+=own(s); return t; }
namespace aggregate {
struct Count { std::uint32_t key=0; unsigned n=0; };
constexpr unsigned table_size=8192;
Count table[table_size]; unsigned used=0,dropped=0;
void clear() { std::memset(table,0,sizeof table); used=dropped=0; }
void bump(std::uint32_t key) {
    if (!key) return;
    for (unsigned i=0,at=(key*2654435761u)%table_size;i<table_size;++i,at=(at+1)%table_size) {
        if (table[at].key==key) { ++table[at].n; return; }
        if (!table[at].key) { if (used*4>=table_size*3) { ++dropped; return; } table[at]={key,1}; ++used; return; }
    }
    ++dropped;
}
// Takes the largest remaining entry (0 when none); repeated calls give a descending list.
Count take_top() {
    unsigned best=table_size;
    for (unsigned i=0;i<table_size;++i) if (table[i].key && table[i].n && (best==table_size || table[i].n>table[best].n || (table[i].n==table[best].n && table[i].key<table[best].key))) best=i;
    if (best==table_size) return {};
    const Count c=table[best]; table[best].n=0; return c;
}
}
void module_leaf(std::uintptr_t base,char* out,size_t capacity) {
    char path[MAX_PATH]; const DWORD n=GetModuleFileNameA(reinterpret_cast<HMODULE>(base),path,MAX_PATH);
    if (!n || n>=MAX_PATH) { sprintf_s(out,capacity,"module_%08x",static_cast<unsigned>(base)); return; }
    const char* leaf=path; for (const char* p=path;*p;++p) if (*p=='\\' || *p=='/') leaf=p+1;
    copy_name(out,capacity,leaf);
}
void where(std::uintptr_t address,char* out,size_t capacity) {
    char name[64];
    if (const auto* r=sampling::find(address)) {
        const std::uintptr_t base=sampling::modules[r->module]; module_leaf(base,name,sizeof name);
        sprintf_s(out,capacity,"%s+0x%x",name,static_cast<unsigned>(address-base)); return;
    }
    HMODULE owner=nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(address),&owner) && owner) {
        module_leaf(reinterpret_cast<std::uintptr_t>(owner),name,sizeof name);
        sprintf_s(out,capacity,"%s+0x%x",name,static_cast<unsigned>(address-reinterpret_cast<std::uintptr_t>(owner))); return;
    }
    sprintf_s(out,capacity,"unknown+0x%08x",static_cast<unsigned>(address));
}
unsigned module_index(std::uintptr_t address) { const auto* r=sampling::find(address); return r?r->module:sampling::module_capacity; }
void log_top(unsigned id,const char* kind,const char* part,unsigned limit) {
    char line[256],at[96];
    for (unsigned rank=1;rank<=limit;++rank) {
        const auto c=aggregate::take_top(); if (!c.n) break;
        where(c.key,at,sizeof at);
        sprintf_s(line,"Loading entities_%s id=%u part=%s rank=%u samples=%u at=%s",kind,id,part,rank,c.n,at); host->log(line);
    }
}
// "<prefix> top=module:count,..." for the samples selected by `keep` (up to 8 modules).
template<class Keep> void log_modules(const char* prefix,unsigned n,Keep keep) {
    using namespace sampling;
    unsigned counts[module_capacity+1]={};
    for (unsigned i=0;i<n;++i) if (keep(samples[i])) ++counts[module_index(samples[i].eip)];
    char line[700]; int length=sprintf_s(line,"%s top=",prefix);
    for (unsigned k=0;k<8 && length>0 && length<600;++k) {
        unsigned best=module_capacity+1; for (unsigned m=0;m<=module_capacity;++m) if (counts[m] && (best>module_capacity || counts[m]>counts[best])) best=m;
        if (best>module_capacity) break;
        char name[64]; if (best<module_count) module_leaf(modules[best],name,sizeof name); else strcpy_s(name,"unlisted");
        length+=sprintf_s(line+length,sizeof line-length,"%s%s:%u",k?",":"",name,counts[best]); counts[best]=0;
    }
    host->log(line);
}
void log_samples(const Record& r) {
    using namespace sampling;
    char line[700];
    const unsigned n=static_cast<unsigned>(InterlockedCompareExchange(&count,0,0));
    const double interval=n?ms(armed_ticks)/n:0.0;
    sprintf_s(line,"Loading entities_sampler id=%u state=%s samples=%u overflow=%ld failures=%ld pause_ms=%.3f interval_ms=%.3f modules=%u ranges=%u range_overflow=%u (main-thread instruction pointer and validated return addresses, taken only inside the entity handler; pause = time the sampler held the main thread, already inside the times above)",
        r.id,sampler_states[r.sampler],n,overflow,failures,ms(static_cast<Tick>(pause_ticks)),interval,module_count,range_count,range_overflow);
    host->log(line);
    if (r.sampler!=sampler_on || !n) return;
    unsigned per_part[part_count]={};
    for (unsigned i=0;i<n;++i) ++per_part[samples[i].part<part_count?samples[i].part:part_other];
    // Per part (every part with samples): the modules holding the instruction pointer. Parts with at
    // least a tenth of the samples also get leaf (instruction pointer, 16-byte buckets), caller
    // (first return address: the call site that entered the sampled function) and stack (every
    // distinct return address of a sample: inclusive) rankings.
    for (unsigned p=0;p<part_count;++p) {
        if (!per_part[p]) continue;
        char prefix[128]; sprintf_s(prefix,"Loading entities_modules id=%u part=%s samples=%u",r.id,part_names[p],per_part[p]);
        log_modules(prefix,n,[p](const Sample& s){ return (s.part<part_count?s.part:part_other)==p; });
    }
    for (unsigned p=0;p<part_count;++p) {
        if (!per_part[p] || per_part[p]*10<n) continue;
        auto in=[p](const Sample& s){ return (s.part<part_count?s.part:part_other)==p; };
        aggregate::clear(); for (unsigned i=0;i<n;++i) if (in(samples[i])) aggregate::bump(samples[i].eip&~15u);
        log_top(r.id,"leaf",part_names[p],12);
        aggregate::clear(); for (unsigned i=0;i<n;++i) if (in(samples[i]) && samples[i].count) aggregate::bump(samples[i].ra[0]);
        log_top(r.id,"caller",part_names[p],12);
        aggregate::clear();
        for (unsigned i=0;i<n;++i) {
            const auto& s=samples[i]; if (!in(s)) continue;
            for (unsigned f=0;f<s.count;++f) { bool seen=false; for (unsigned g=0;g<f;++g) if (s.ra[g]==s.ra[f]) seen=true; if (!seen) aggregate::bump(s.ra[f]); }
        }
        log_top(r.id,"stack",part_names[p],24);
    }
    // The four classes with the most samples: their modules.
    unsigned per_class[class_capacity]={};
    for (unsigned i=0;i<n;++i) if (samples[i].cls<class_capacity) ++per_class[samples[i].cls];
    for (unsigned k=0;k<4;++k) {
        unsigned best=class_capacity; for (unsigned c=0;c<class_capacity;++c) if (per_class[c] && (best==class_capacity || per_class[c]>per_class[best])) best=c;
        if (best==class_capacity) break;
        char prefix[160]; sprintf_s(prefix,"Loading entities_class_modules id=%u class=%s samples=%u",r.id,r.classes[best].used?r.classes[best].name:"?",per_class[best]);
        log_modules(prefix,n,[best](const Sample& s){ return s.cls==best; });
        per_class[best]=0;
    }
    if (aggregate::dropped) { sprintf_s(line,"Loading entities_sampler id=%u aggregate_dropped=%u",r.id,aggregate::dropped); host->log(line); }
}
void log_record(const Record& r) {
    char line[700];
    Tick parts_total=0; for (const auto& s:r.parts) parts_total+=s.total;
    sprintf_s(line,"Loading entities id=%u phases=%u entities=%llu world=%u creates=%llu classes=%u class_overflow=%u sequence_errors=%u post_unended=%u probe_unknown=%u name_faults=%u inclusive_ms=%.3f parts_ms=%.3f setup_ms=%.3f (entity block handler split at its own call sites; probe = nested loading-probe scopes such as model_load/disk_open, own = the rest)",
        r.id,r.phases,r.entities,r.world,r.creates,r.class_count,r.class_overflow,r.sequence_errors,r.post_unended,r.probe_unknown,r.name_faults,ms(r.total),ms(parts_total),ms(r.setup));
    host->log(line);
    for (unsigned p=0;p<part_count;++p) {
        const Stat& s=r.parts[p]; if (!s.calls) continue;
        sprintf_s(line,"Loading entities_part id=%u part=%s calls=%llu inclusive_ms=%.3f own_ms=%.3f probe_ms=%.3f max_ms=%.3f model_loads=%llu disk_opens=%llu",
            r.id,part_names[p],s.calls,ms(s.total),ms(own(s)),ms(s.probe),ms(s.maximum),s.models,s.disks);
        host->log(line);
    }
    // Classes by own time (all parts), top 24, then the rest summed.
    bool listed[class_capacity]={};
    for (unsigned rank=1;rank<=24;++rank) {
        int best=-1;
        for (unsigned i=0;i<class_capacity;++i) {
            if (!r.classes[i].used || listed[i]) continue;
            if (best<0 || class_own(r.classes[i])>class_own(r.classes[best]) ||
                (class_own(r.classes[i])==class_own(r.classes[best]) && std::strcmp(r.classes[i].name,r.classes[best].name)<0)) best=static_cast<int>(i);
        }
        if (best<0) break;
        listed[best]=true; const Class& c=r.classes[best];
        Tick probe=0,models=0,disks=0; for (const auto& s:c.parts) { probe+=s.probe; models+=s.models; disks+=s.disks; }
        sprintf_s(line,"Loading entities_class id=%u rank=%u class=%s entities=%llu creates=%llu own_ms=%.3f probe_ms=%.3f create_ms=%.3f restore_ms=%.3f restore_probe_ms=%.3f spawn_ms=%.3f spawn_probe_ms=%.3f post_ms=%.3f max_entity_ms=%.3f model_loads=%llu disk_opens=%llu",
            r.id,rank,c.name,c.entities,c.creates,ms(class_own(c)),ms(probe),ms(c.parts[part_create].total),ms(c.parts[part_restore].total),ms(c.parts[part_restore].probe),
            ms(c.parts[part_spawn].total),ms(c.parts[part_spawn].probe),ms(c.parts[part_post].total),ms(c.entity_max),models,disks);
        host->log(line);
    }
    unsigned rest=0; Tick rest_entities=0,rest_own=0,rest_probe=0;
    for (unsigned i=0;i<class_capacity;++i) {
        if (!r.classes[i].used || listed[i]) continue;
        ++rest; rest_entities+=r.classes[i].entities; rest_own+=class_own(r.classes[i]); for (const auto& s:r.classes[i].parts) rest_probe+=s.probe;
    }
    if (rest) {
        sprintf_s(line,"Loading entities_class id=%u rank=rest classes=%u entities=%llu own_ms=%.3f probe_ms=%.3f",r.id,rest,rest_entities,ms(rest_own),ms(rest_probe));
        host->log(line);
    }
    if (r.sampler!=sampler_off) {
        if (sampling::owner==r.id) { log_samples(r); sampling::release(); }
        else { sprintf_s(line,"Loading entities_sampler id=%u state=%s samples=0",r.id,sampler_states[r.sampler]); host->log(line); }
    }
}
void log_done() {
    for (unsigned i=0;i<done_count;++i) log_record(done[i]);
    done_count=0;
    if (done_dropped) { char line[96]; sprintf_s(line,"Loading entities dropped records=%u",done_dropped); host->log(line); done_dropped=0; }
}
void BLSE_CALL report(void*) {
    report_queued=false;
    if (GetCurrentThreadId()!=thread || gone) return;
    const bool finished=record_open && !observing && (!loading::active || loading::current.id!=open_record.id);
    log_done();
    if (finished) { close_record(); log_done(); }
    if (record_open) schedule_report(); // capture still running: look again next frame
}

// --- install ---
struct CallSite { unsigned site,thunk; void* detour; void** original; const char* label; };
bool call_site_hook(std::uintptr_t base,const CallSite& c) {
    const auto code=reinterpret_cast<unsigned char*>(base+c.site);
    std::int32_t displacement=0; std::memcpy(&displacement,code+1,4);
    const bool native=code[0]==0xe8 && reinterpret_cast<std::uintptr_t>(code+5)+displacement==base+c.thunk;
    const int result=native?host->hook_call(host->context,code,c.detour,c.original):BLSE_HOOK_BYTES_DIFFER;
    char line[160]; sprintf_s(line,"Loading probe hook %s: %s (%d)",c.label,result==BLSE_HOOK_OK?"enabled":"unavailable",result); host->log(line);
    return result==BLSE_HOOK_OK;
}
// vampire.dll 996e0245... (UP12), verified with IDA headless on the hash-identical copy and by the
// test: call sites and their incremental-link thunks inside the entity handler +0x1a2e40 and
// RestoreEntity +0x1a3380, and the two identical 10-byte runs after each Restore call (no branch
// lands inside either run).
unsigned hook_all(std::uintptr_t base) {
    const CallSite calls[]={
        {0x1a2ec5,0x002e32,reinterpret_cast<void*>(on_create_world),reinterpret_cast<void**>(&orig_create_world),"entity_create_world"},
        {0x1a3023,0x002e32,reinterpret_cast<void*>(on_create),reinterpret_cast<void**>(&orig_create),"entity_create"},
        {0x1a3006,0x014d62,reinterpret_cast<void*>(on_create_player),reinterpret_cast<void**>(&orig_create_player),"entity_create_player"},
        {0x1a2ee5,0x011063,reinterpret_cast<void*>(on_world),reinterpret_cast<void**>(&orig_world),"entity_world_restore"},
        {0x1a3129,0x0045d4,reinterpret_cast<void*>(on_begin),reinterpret_cast<void**>(&orig_begin),"entity_begin"},
        {0x1a316a,0x009c8c,reinterpret_cast<void*>(on_spawned),reinterpret_cast<void**>(&orig_spawned),"entity_spawned"},
        {0x1a31ec,0x014add,reinterpret_cast<void*>(on_end),reinterpret_cast<void**>(&orig_end),"entity_end"},
    };
    static const unsigned char after_restore[]={0x8b,0x16, 0x8b,0xce, 0xff,0x92,0xd4,0x01,0x00,0x00};
    unsigned n=0;
    for (const auto& c:calls) if (call_site_hook(base,c)) ++n;
    if (code_hook(base,0x1a313d,after_restore,sizeof after_restore,reinterpret_cast<void*>(restored_detour),&orig_restored,"entity_restored")) ++n;
    if (code_hook(base,0x1a33a7,after_restore,sizeof after_restore,reinterpret_cast<void*>(world_restored_detour),&orig_world_restored,"entity_world_restored")) ++n;
    return n;
}
void unhook_all(std::uintptr_t base) {
    if (!BLSE_HOST_HAS(host,unhook) || !host->unhook) return;
    for (unsigned site:{0x1a2ec5u,0x1a3023u,0x1a3006u,0x1a2ee5u,0x1a3129u,0x1a316au,0x1a31ecu,0x1a313du,0x1a33a7u})
        host->unhook(host->context,reinterpret_cast<void*>(base+site));
}
void attach(std::uintptr_t base) {
    if (!enabled || gone || markers || GetCurrentThreadId()!=thread) return;
    const unsigned n=hook_all(base);
    char line[200];
    if (n!=9) {
        unhook_all(base);
        sprintf_s(line,"Loading entities: breakdown unavailable (%u/9 entity hooks; partial hooks removed).",n); host->log(line); return;
    }
    markers=true;
    if (sampler_wanted) {
        HANDLE self=nullptr;
        if (!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&self,THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,0)) self=nullptr;
        if (!sampling::enum_modules) if (auto k=GetModuleHandleW(L"kernel32.dll")) sampling::enum_modules=reinterpret_cast<sampling::EnumModules>(GetProcAddress(k,"K32EnumProcessModules"));
        if (!self || !sampling::enum_modules || !sampling::start(self,reinterpret_cast<const NT_TIB*>(NtCurrentTeb()))) { if (self) CloseHandle(self); host->log("Loading entities: sampler unavailable; class/part breakdown only."); }
    }
    sprintf_s(line,"Loading entities: 9/9 entity hooks installed; sampler %s (read-only timing).",sampling::worker?"ready":"off"); host->log(line);
}
void BLSE_CALL on_restart(const char*,const void*,size_t,void*) {
    if (gone || GetCurrentThreadId()!=thread) return;
    gone=true; observing=false; record_open=false; InterlockedExchange(&sampling::armed,0);
    const bool stopped=sampling::stop(250);
    host->log(stopped?"Loading entities stopped after engine restart (use a fresh process for further captures).":
                      "Loading entities stopped after engine restart; sampler thread still exiting (retained, not terminated).");
}
bool setting_off(const char* key) {
    if (!BLSE_HOST_HAS(host,ini_get) || !host->ini_get) return false;
    char value[8]={}; size_t size=0;
    return host->ini_get(host->context,"engine_fixes","Loading",key,value,sizeof value,&size) && size<sizeof value && value[0]=='0';
}
// Once per process, in BLSE_PluginLoad after loading::restore::install() (main thread).
void install() {
    if (attempted) return; attempted=true;
    if (!loading::restore::enabled) return;          // restore phases off: nothing to attribute to
    if (setting_off("entity_trace")) { host->log("Loading entities off ([Loading] entity_trace=0)."); return; }
    if (!BLSE_HOST_HAS(host,hook_call) || !host->hook_call || !BLSE_HOST_HAS(host,hook_function) || !host->hook_function) {
        host->log("Loading entities disabled: host lacks call hooks."); return;
    }
    thread=GetCurrentThreadId();
    if (!host->subscribe(host->context,BLSE_MSG_ENGINE_RESTART,on_restart,nullptr)) { host->log("Loading entities disabled: engine restart not followed."); return; }
    sampler_wanted=!setting_off("entity_sampler");
    enabled=true;
    loading::restore::entity_observer.enter=enter; loading::restore::entity_observer.leave=leave; loading::restore::entity_observer.attach=attach;
    host->log(sampler_wanted?"Loading entities enabled: per-class entity restore breakdown and main-thread sampler during the entity handler (read-only timing; hooks after the server phases).":
                             "Loading entities enabled: per-class entity restore breakdown (sampler off: [Loading] entity_sampler=0).");
}
} // namespace entities
} // namespace loading
#endif
