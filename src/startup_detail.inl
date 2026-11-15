// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Included inside restart_startup after its clock/trace state. Read-only timings.
namespace detail {
enum Id { tables, systems, panels, sentences, device, dsp, activation, size };
const char* names[]={"client_data_tables","client_game_systems","client_panels","sound_sentences","sound_device","sound_dsp","native_ui_activation"};
struct Stat { unsigned calls=0; LONGLONG total=0,maximum=0,first=0; } stats[size]; // first: begin tick of call 1
unsigned generation=0,coverage=0; bool factory_ready=false;
LONGLONG activation_return=0,attachment_ticks=0;
#ifdef ENGINE_FIXES_STARTUP_TEST
LONGLONG test_tick=0;
LONGLONG now() { return test_tick; }
#else
LONGLONG now() { LARGE_INTEGER tick; QueryPerformanceCounter(&tick); return tick.QuadPart; }
#endif
struct Scope {
    Id id; LONGLONG begin=0; unsigned epoch=0; bool live=false;
    explicit Scope(Id value):id(value) {
        if (!tracing || GetCurrentThreadId()!=thread) return;
        live=true; epoch=generation; begin=now();
    }
    ~Scope() {
        if (!live || !tracing || epoch!=generation) return;
        const auto elapsed=now()-begin; auto& s=stats[id]; if (!s.calls) s.first=begin; ++s.calls; s.total+=elapsed;
        if (elapsed>s.maximum) s.maximum=elapsed;
    }
};
using Int0=int(__cdecl*)(); using Char0=char(__cdecl*)();
using Device=void*(__cdecl*)(char); using Dsp=char(__cdecl*)(char);
using Activate=int(__cdecl*)(char); using Factory=FARPROC(__cdecl*)(HMODULE);
Int0 original_tables=nullptr,original_panels=nullptr,original_sentences=nullptr;
Char0 original_systems=nullptr; Device original_device=nullptr;
Dsp original_dsp=nullptr; Activate original_activation=nullptr; Factory original_factory=nullptr;
#include "startup_table_append.inl"
#include "startup_pack_prefix.inl"
#include "startup_sound_schemes.inl"
int __cdecl probe_tables() { Scope scope(tables); table_append::Window window; sound_schemes::Window sounds; pack_prefix::Lifetime prefix; return original_tables(); }
char __cdecl probe_systems() { Scope scope(systems); return original_systems(); }
int __cdecl probe_panels() { Scope scope(panels); return original_panels(); }
int __cdecl probe_sentences() { Scope scope(sentences); return original_sentences(); }
void* __cdecl probe_device(char mode) { Scope scope(device); return original_device(mode); }
char __cdecl probe_dsp(char reset) { Scope scope(dsp); return original_dsp(reset); }
int __cdecl probe_activation(char active) {
    Scope scope(activation); const int result=original_activation(active);
    if (scope.live && tracing && scope.epoch==generation) activation_return=now();
    return result;
}
bool call_hook(std::uintptr_t base,unsigned site,unsigned target,void* detour,void** original,const char* label) {
    const auto code=reinterpret_cast<const unsigned char*>(base+site);
    std::int32_t displacement; std::memcpy(&displacement,code+1,4);
    const auto destination=reinterpret_cast<std::uintptr_t>(code+5)+displacement;
    if (code[0]==0xe8 && destination==reinterpret_cast<std::uintptr_t>(detour) && *original) return true;
    const bool ok=code[0]==0xe8 && destination==base+target &&
        host->hook_call(host->context,const_cast<unsigned char*>(code),detour,original)==BLSE_HOOK_OK;
    char line[180]; sprintf_s(line,"Startup child hook %s: %s",label,ok?"enabled":"unavailable (call changed or conflict)"); host->log(line);
    return ok;
}
#include "startup_table_children.inl"
FARPROC __cdecl probe_factory(HMODULE module) {
    FARPROC result=original_factory(module);
    const DWORD error=GetLastError();
    if (tracing && GetCurrentThreadId()==thread && module && module==GetModuleHandleW(L"client.dll")) {
        const auto begin=now();
        if (supported(module)) {
            const auto base=reinterpret_cast<std::uintptr_t>(module);
            const bool tables_hooked=call_hook(base,0xcb8e4,0x15f860,reinterpret_cast<void*>(probe_tables),reinterpret_cast<void**>(&original_tables),"client_data_tables");
            if (tables_hooked) coverage|=1u<<tables;
            if (tables_hooked) table_children::install(base);
            table_append::installed=false;
            if (tables_hooked) table_append::install(base);
            if (tables_hooked) sound_schemes::install(base);
            if (tables_hooked) pack_prefix::install();
            if (call_hook(base,0xcb908,0xf40e0,reinterpret_cast<void*>(probe_systems),reinterpret_cast<void**>(&original_systems),"client_game_systems")) coverage|=1u<<systems;
            if (call_hook(base,0xcb93b,0x166f50,reinterpret_cast<void*>(probe_panels),reinterpret_cast<void**>(&original_panels),"client_panels")) coverage|=1u<<panels;
        } else host->log("Startup client child hooks unavailable: unsupported client build.");
        attachment_ticks+=now()-begin;
    }
    SetLastError(error); return result;
}
void begin() {
    ++generation; for (auto& s:stats) s={}; coverage=0; activation_return=attachment_ticks=0;
    table_children::reset();
    table_append::installed=false;
    table_append::requested=enabled("startup_table_append","0");
    sound_schemes::reset(); sound_schemes::requested=enabled("startup_sound_schemes","0");
    sound_schemes::requested_cache=enabled("startup_sound_scheme_cache","0");
    pack_prefix::reset(); pack_prefix::requested=enabled("startup_pack_prefix","0");
    factory_ready=call_hook(engine_base,0x1bd68,0xa2470,reinterpret_cast<void*>(probe_factory),reinterpret_cast<void**>(&original_factory),"client_factory_boundary");
    if (call_hook(engine_base,0x118d12,0x13eef0,reinterpret_cast<void*>(probe_sentences),reinterpret_cast<void**>(&original_sentences),"sound_sentences")) coverage|=1u<<sentences;
    if (call_hook(engine_base,0x118db5,0x13b9b0,reinterpret_cast<void*>(probe_device),reinterpret_cast<void**>(&original_device),"sound_device")) coverage|=1u<<device;
    if (call_hook(engine_base,0x118e7b,0x1318c0,reinterpret_cast<void*>(probe_dsp),reinterpret_cast<void**>(&original_dsp),"sound_dsp")) coverage|=1u<<dsp;
    if (call_hook(engine_base,0xfa8dd,0x10d4f0,reinterpret_cast<void*>(probe_activation),reinterpret_cast<void**>(&original_activation),"native_ui_activation")) coverage|=1u<<activation;
}
void report(LONGLONG end) {
    char line[320];
    const double scale=1000.0/frequency.QuadPart;
    // trace_origin_qpc is the raw QueryPerformanceCounter value of the trace start, so other
    // logs on the same machine (the intro companion's qpc_origin) can be aligned exactly.
    sprintf_s(line,"Startup detail coverage=0x%x client_factory=%d attachment_ms=%.3f native_activation_return_ms=%.3f activation_to_first_task_ms=%.3f trace_origin_qpc=%lld qpc_frequency=%lld (activation is not an authentication timing)",
        coverage,factory_ready?1:0,attachment_ticks*scale,
        activation_return?(activation_return-start.QuadPart)*scale:-1.0,activation_return?(end-activation_return)*scale:-1.0,
        start.QuadPart,frequency.QuadPart); host->log(line);
    sprintf_s(line,"Startup table append requested=%d enabled=%d calls=%llu avoided_sibling_steps=%llu fallbacks=%llu",table_append::requested?1:0,table_append::installed?1:0,table_append::calls,table_append::avoided_links,table_append::fallbacks); host->log(line);
    for (unsigned i=0;i<size;++i) if (stats[i].calls) {
        sprintf_s(line,"Startup child phase=%s calls=%u inclusive_ms=%.3f max_ms=%.3f first_begin_ms=%.3f",names[i],stats[i].calls,stats[i].total*scale,stats[i].maximum*scale,(stats[i].first-start.QuadPart)*scale); host->log(line);
    }
    table_children::report(scale);
    sound_schemes::report(scale);
    pack_prefix::report(scale);
}
}
