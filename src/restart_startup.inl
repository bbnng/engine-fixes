// Restart crash fix ([Fixes] restart_crash) and the startup window ([Startup] startup_trace),
// inside which the startup speedups run.
#include "retired_python_heap.h"
namespace restart_startup {
retired_python::Node retired;
std::uintptr_t python_base=0,engine_base=0;
bool pending=false; int previous_gc=0; DWORD thread=0;
using Free=BOOL(WINAPI*)(HMODULE);
Free original_free=nullptr;
bool exact_module(HMODULE module,const char* first,const char* second=nullptr) {
    if (!module) return false;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(module,path,MAX_PATH)) return false;
    unsigned char digest[32]; std::vector<unsigned char> head; std::vector<std::uint32_t> blocks;
    if (!read_client(path,digest,head,blocks)) return false;
    char hex[65]; for (unsigned i=0;i<32;++i) sprintf_s(hex+2*i,3,"%02x",digest[i]);
    return !std::strcmp(hex,first) || (second && !std::strcmp(hex,second));
}
int& gc_enabled() { return *reinterpret_cast<int*>(python_base+0x888e4); }
void suspend_gc() {
    if (pending) return;
    previous_gc=gc_enabled(); gc_enabled()=0; pending=true;
}
BOOL WINAPI free_module(HMODULE module) {
    const DWORD incoming_error=GetLastError();
    const bool game=GetCurrentThreadId()==thread && module && module==GetModuleHandleW(L"vampire.dll");
    const bool armed=game && !pending;
    if (game) suspend_gc();
    SetLastError(incoming_error);
    const BOOL result=original_free(module);
    const DWORD error=GetLastError();
    if (armed && !result && pending) { gc_enabled()=previous_gc; pending=false; }
    SetLastError(error); return result;
}
bool finish_retirement() {
    if (!pending) return false;
    // BLSE emits engine_restart after the loader reset and Python rebind. Retire
    // the reset's hold containers too. Normal gameplay allocations resume only
    // after this callback; no object/type pointer is read by list splicing.
    for (unsigned rva:{0x888c0u,0x888ccu,0x888d8u})
        retired_python::transfer(*reinterpret_cast<retired_python::Node*>(python_base+rva),retired);
    gc_enabled()=previous_gc; pending=false;
    char message[160]; sprintf_s(message,"Restart crash fix: the previous run's Python objects were set aside; garbage collection resumed (gc_enabled=%d).",gc_enabled());
    host->log(message); return true;
}
bool attach_free() {
    auto engine=GetModuleHandleW(L"engine.dll");
    if (!exact_module(engine,"9d00b2c1e5edbad052fb0b514634ba54574666e12d1b408c46ce141d51ed2313")) return false;
    engine_base=reinterpret_cast<std::uintptr_t>(engine);
    auto slot=reinterpret_cast<void**>(engine_base+0x1730c8);
    if (*slot==reinterpret_cast<void*>(free_module)) return true;
    auto native=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"FreeLibrary");
    if (*slot!=reinterpret_cast<void*>(native)) return false;
    return host->hook_pointer(host->context,slot,reinterpret_cast<void*>(free_module),reinterpret_cast<void**>(&original_free))==BLSE_HOOK_OK;
}
// A registration labels the following initialization phase. Differences between
// markers are intervals, not exact function self times. No logs in this hook.
using Register=int(__cdecl*)(const char*,const char*,int);
Register original_register=nullptr;
struct Phase { char name[80]; LARGE_INTEGER tick; } phases[64];
unsigned count=0; bool tracing=false; LARGE_INTEGER frequency,start;
#include "startup_detail.inl"
int __cdecl register_phase(const char* init,const char* stop,int group) {
    if (tracing && GetCurrentThreadId()==thread && count<64) {
        auto& phase=phases[count++]; strncpy_s(phase.name,init?init:"(null)",_TRUNCATE); QueryPerformanceCounter(&phase.tick);
    }
    return original_register(init,stop,group);
}
void BLSE_CALL report(void*) {
    if (!tracing) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    char line[240];
    sprintf_s(line,"Startup measured plugin-to-first-task_ms=%.3f markers=%u (not pure CPU time or full process startup)",1000.0*(now.QuadPart-start.QuadPart)/frequency.QuadPart,count); host->log(line);
    for(unsigned i=0;i<count;++i) {
        const auto end=i+1<count?phases[i+1].tick:now;
        sprintf_s(line,"Startup phase=%.79s until_next_marker_ms=%.3f",phases[i].name,1000.0*(end.QuadPart-phases[i].tick.QuadPart)/frequency.QuadPart); host->log(line);
    }
    detail::report(now.QuadPart);
    tracing=false;
}
void start_trace() {
    if (!enabled("startup_trace","0") || !BLSE_HOST_HAS(host,add_task) || !host->add_task) return;
    const unsigned char bytes[]={0x53,0x55,0x56,0x57,0x6a,0x28};
    auto entry=reinterpret_cast<const unsigned char*>(engine_base+0x103d90);
    std::int32_t displacement=0; std::memcpy(&displacement,entry+1,4);
    const bool ours=entry[0]==0xe9 && reinterpret_cast<std::uintptr_t>(entry+5)+displacement==reinterpret_cast<std::uintptr_t>(register_phase);
    if (!ours) original_register=nullptr;
    if (!original_register && host->hook_function(host->context,reinterpret_cast<void*>(engine_base+0x103d90),bytes,sizeof bytes,
          reinterpret_cast<void*>(register_phase),reinterpret_cast<void**>(&original_register))!=BLSE_HOOK_OK) {
        host->log("Startup markers unavailable: initialization registration hook refused."); return;
    }
    detail::begin();
    count=0; tracing=true; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
    if (!host->add_task(host->context,report,nullptr)) { tracing=false; host->log("Startup markers unavailable: report task refused."); }
    else host->log("Startup initialization markers enabled.");
}
void BLSE_CALL restarted(const char*,const void*,size_t,void*) {
    // BLSE drops patches for unloaded DLLs. Python and the retirement
    // list survive, but engine imports may be new.
    if (original_free && !finish_retirement()) host->log("Restart crash fix: no game DLL unload was seen during this restart.");
    auto engine=GetModuleHandleW(L"engine.dll");
    if (!exact_module(engine,"9d00b2c1e5edbad052fb0b514634ba54574666e12d1b408c46ce141d51ed2313")) return;
    engine_base=reinterpret_cast<std::uintptr_t>(engine);
    if (original_free && !attach_free()) host->log("Restart crash fix: could not attach to the restarted engine; the next restart is not covered.");
    start_trace();
}
void install() {
    if (!enabled("restart_crash") && !enabled("startup_trace","0")) return;
    thread=GetCurrentThreadId();
    auto engine=GetModuleHandleW(L"engine.dll");
    if (!exact_module(engine,"9d00b2c1e5edbad052fb0b514634ba54574666e12d1b408c46ce141d51ed2313")) {
        host->log("Startup speedups and restart crash fix disabled: unknown engine.dll build."); return;
    }
    engine_base=reinterpret_cast<std::uintptr_t>(engine);
    start_trace(); // independent of the repair's hook availability
    if (!host->subscribe(host->context,BLSE_MSG_ENGINE_RESTART,restarted,nullptr)) {
        host->log("Restart crash fix disabled: engine restart event unavailable."); return;
    }
    if (!enabled("restart_crash")) return;
    auto python=GetModuleHandleW(L"vampire_python21.dll");
    auto exe=GetModuleHandleW(nullptr);
    const bool legacy=exact_module(exe,"80d73e7454241b64c494ded4782b0a5f9d6e2da06856c95dbd1f7a78bc36eaf9");
    if (!BLSE_HOST_HAS(host,hook_pointer) || !host->hook_pointer ||
        (!legacy && !exact_module(exe,"e195b0a6f676b9400a47eee2df020da0b0beba8293ecdc3ebb6f6dcbf44f2ad7")) ||
        !exact_module(python,"2ce854ddd5191721f38ccbe6c19988655632c5c9a79fdfef03a4cd8f49b04645","65a92dda7b8fd1ca8668cc8924d6300a895603b719e19c3b537751e434ec4ea3")) {
        host->log("Restart crash fix disabled: unknown Vampire.exe or vampire_python21.dll, or BLSE too old."); return;
    }
    python_base=reinterpret_cast<std::uintptr_t>(python);
    retired_python::initialize(retired);
    if (!attach_free()) { host->log("Restart crash fix disabled: engine.dll's FreeLibrary import is changed or hooked by another mod."); return; }
    host->log("Restart crash fix enabled.");
}
}
