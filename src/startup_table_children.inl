// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// IDA-verified direct calls in client.dll+0x15f860. Each takes no stack
// arguments. Preserve ALL integer registers/flags around timing, including
// ECX/EBX/EDI: some native functions use internal register conventions.
namespace table_children {
#define TABLE_CALLS(X) \
 X(0,0x15f878,0x15eea0) X(1,0x15f87d,0x15fa10) X(2,0x15f882,0x156560) \
 X(3,0x15f88c,0x15c4b0) X(4,0x15f896,0x15c4b0) X(5,0x15f8a0,0x15c4b0) \
 X(6,0x15f8aa,0x15c4b0) X(7,0x15f8b4,0x14b870) X(8,0x15f8be,0x13fc80) \
 X(9,0x15f8c8,0x144dc0) X(10,0x15f8d2,0x1532e0) X(11,0x15f8dc,0x157850) \
 X(12,0x15f8e6,0x14e360) X(13,0x15f8f0,0x145e10) X(14,0x15f8fa,0x14ef90) \
 X(15,0x15f904,0x14f480) X(16,0x15f90e,0x14f990) X(17,0x15f918,0x14ff70) \
 X(18,0x15f922,0x150440) X(19,0x15f92c,0x1508f0) X(20,0x15f936,0x150ff0) \
 X(21,0x15f940,0x151320) X(22,0x15f94a,0x1499f0) X(23,0x15f954,0x1526a0) \
 X(24,0x15f95e,0x161fd0) X(25,0x15f968,0x162650) X(26,0x15f972,0x163350) \
 X(27,0x15f97c,0x15c720) X(28,0x15f986,0x15c720) X(29,0x15f990,0x15c720) \
 X(30,0x15f99a,0x15c720) X(31,0x15f9a4,0x140a70) X(32,0x15f9ae,0x14bab0) \
 X(33,0x15f9b8,0x13fdc0) X(34,0x15f9c2,0x157a20) X(35,0x15f9cc,0x13fdc0) \
 X(36,0x15f9d6,0x153500) X(37,0x15f9e0,0x14e4d0) X(38,0x15f9ea,0x149ac0) \
 X(39,0x15f9f4,0x140b40) X(40,0x15f9f9,0x55f50) X(41,0x15f9fe,0x15e630)
void* native[42]={};
Stat timings[42];
LONGLONG starts[42]={};
bool measuring[42]={};
__declspec(noinline) void __cdecl mark_begin(unsigned id) {
    if(!tracing || GetCurrentThreadId()!=thread)return;
    const DWORD error=GetLastError();
    measuring[id]=true;starts[id]=now();
    SetLastError(error);
}
__declspec(noinline) void __cdecl mark_end(unsigned id) {
    if(!tracing || GetCurrentThreadId()!=thread || !measuring[id])return;
    const DWORD error=GetLastError();
    measuring[id]=false;
    const auto elapsed=now()-starts[id];auto& s=timings[id];
    ++s.calls;s.total+=elapsed;if(elapsed>s.maximum)s.maximum=elapsed;
    SetLastError(error);
}
#define TABLE_WRAPPER(id,site,target) \
__declspec(naked) void probe_##id() { \
    __asm {pushfd} __asm {pushad} __asm {push id} __asm {call mark_begin} \
    __asm {add esp,4} __asm {popad} __asm {popfd} \
    __asm {call dword ptr [native+4*id]} \
    __asm {pushfd} __asm {pushad} __asm {push id} __asm {call mark_end} \
    __asm {add esp,4} __asm {popad} __asm {popfd} __asm {ret} \
}
TABLE_CALLS(TABLE_WRAPPER)
#undef TABLE_WRAPPER
#ifdef ENGINE_FIXES_STARTUP_TEST
#define TABLE_POINTER(id,site,target) reinterpret_cast<void*>(probe_##id),
void* probes[]={TABLE_CALLS(TABLE_POINTER)};
#undef TABLE_POINTER
#endif
void install(std::uintptr_t base) {
    unsigned installed=0;
#define INSTALL_TABLE(id,site,target) \
    if(call_hook(base,site,target,reinterpret_cast<void*>(probe_##id),&native[id],"table_" #id))++installed;
    TABLE_CALLS(INSTALL_TABLE)
#undef INSTALL_TABLE
    char line[100];sprintf_s(line,"Startup table child coverage=%u/42 (timing only)",installed);host->log(line);
}
void reset() {for(unsigned i=0;i<42;++i) {timings[i]={};starts[i]=0;measuring[i]=false;}}
void report(double scale) {
    char line[180];
#define REPORT_TABLE(id,site,target) \
    if(timings[id].calls) {sprintf_s(line,"Startup table child id=%u call=0x%x target=0x%x calls=%u inclusive_ms=%.3f",id,site,target,timings[id].calls,timings[id].total*scale);host->log(line);}
    TABLE_CALLS(REPORT_TABLE)
#undef REPORT_TABLE
}
#undef TABLE_CALLS
}
