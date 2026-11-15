// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Worldspawn sound precache: reuse the validated pack prefix index, including its exact native
// extension test, only inside vampire+0x23c020 (CWorld::Precache; thiscall, no arguments, plain
// retn). That function lists 36 "<dir>/*.wav" directories through the generic helper +0x1d0f10
// (FindFirst "%s/*%s", IsDirectory, PrecacheSound, FindNext): 30 sound/Surfaces dirs via +0x23bd20
// and 6 Interesting_Places/Interface dirs via +0x33f750. Every map spawn runs it (save loads and
// transitions alike). Native listings, handlers, loose-file queries, duplicates, search-path
// order and precache calls remain intact; only pack-tree nodes that cannot match are skipped and
// each proposed answer is re-checked by the unchanged native scan.
// [Loading] sound_precache_index=1.
#ifndef ENGINE_FIXES_LOADING_SOUND_PRECACHE_INL
#define ENGINE_FIXES_LOADING_SOUND_PRECACHE_INL
namespace loading { namespace sound_precache {
namespace prefix=restart_startup::detail::pack_prefix;
using Precache=Raw(__thiscall*)(void*);
Precache original=nullptr;
DWORD thread=0;
bool attempted=false,requested=false,gone=false,busy=false,report_queued=false;
unsigned pending_count=0,dropped=0;
LONGLONG frequency=0;
const unsigned precache_rva=0x23c020,precache_global_rva=0x70b234;
struct Counts {
    std::uint64_t scans,hits,none,extension,builds,failed,stale,mismatches,avoided;
    std::uint64_t pattern,start,guard,index,disabled,unverified,off_thread;
    LONGLONG fast_ticks,native_ticks,build_ticks;
};
Counts counts() { return {prefix::calls,prefix::fast_hit,prefix::fast_none,prefix::extension_fast,prefix::builds,
    prefix::build_failed,prefix::stale,prefix::mismatches,prefix::avoided_nodes,
    prefix::native_pattern,prefix::native_start,prefix::native_guard,prefix::native_index,prefix::native_disabled,prefix::extension_unverified,
    static_cast<std::uint64_t>(InterlockedCompareExchange(&prefix::off_thread,0,0)),
    prefix::fast_time.ticks,prefix::native_time.ticks,prefix::build_time.ticks}; }
enum Reason { available, not_ready, index_disabled, incomplete_hooks, missing_target, thread_differs, filesystem_differs, extension_unverified };
const char* const reasons[]={"available","index_not_ready","index_disabled","incomplete_hooks","missing_target","thread_differs","filesystem_differs","extension_unverified"};
struct Record { unsigned id=0,peak=0; Tick ticks=0; Counts before{},after{}; Reason reason=not_ready; bool optimized=false; };
Record pending[8];
double ms(LONGLONG ticks) { return frequency?1000.0*static_cast<double>(ticks)/frequency:0; }
void BLSE_CALL report(void*) {
    report_queued=false;
    if (GetCurrentThreadId()!=thread) return;
    for (unsigned i=0;i<pending_count;++i) {
        const auto& r=pending[i]; const auto& a=r.before; const auto& b=r.after;
        char line[1100];
        _snprintf_s(line,_TRUNCATE,"Loading sound_precache id=%u indexed=%d precache_ms=%.3f scans=%llu hits=%llu no_match=%llu extension_answers=%llu builds=%llu build_failed=%llu stale=%llu mismatches=%llu avoided_nodes=%llu reason=%s arena_bytes=%u native_pattern=%llu native_start=%llu native_guard=%llu native_index=%llu native_disabled=%llu extension_unverified=%llu off_thread=%llu fast_ms=%.3f native_ms=%.3f build_ms=%.3f (worldspawn precache only; native listings/handlers unchanged; index released on return)",
            r.id,r.optimized?1:0,ms(r.ticks),
            b.scans-a.scans,b.hits-a.hits,b.none-a.none,b.extension-a.extension,b.builds-a.builds,b.failed-a.failed,
            b.stale-a.stale,b.mismatches-a.mismatches,b.avoided-a.avoided,reasons[r.reason],r.peak,
            b.pattern-a.pattern,b.start-a.start,b.guard-a.guard,b.index-a.index,b.disabled-a.disabled,b.unverified-a.unverified,b.off_thread-a.off_thread,
            ms(b.fast_ticks-a.fast_ticks),ms(b.native_ticks-a.native_ticks),ms(b.build_ticks-a.build_ticks));
        host->log(line);
    }
    pending_count=0;
    if (dropped) { char line[96]; sprintf_s(line,"Loading sound_precache dropped reports=%u",dropped); host->log(line); dropped=0; }
}
void enqueue(const Record& r) {
    if (pending_count<_countof(pending)) pending[pending_count++]=r; else ++dropped;
    if (!report_queued && host->add_task(host->context,report,nullptr)) report_queued=true;
}
struct Error { DWORD value=GetLastError(); ~Error(){ SetLastError(value); } };
struct Busy { Busy(){busy=true;} ~Busy(){busy=false;} };
Reason index_reason() {
    if (!prefix::ready) return not_ready;
    if (prefix::disabled) return index_disabled;
    if (prefix::hooks!=3) return incomplete_hooks;
    if (!prefix::original) return missing_target;
    if (restart_startup::thread!=thread) return thread_differs;
    if (reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"filesystem_stdio.dll"))!=prefix::base) return filesystem_differs;
    if (!prefix::extension_ok) return extension_unverified;
    return available;
}
Raw __fastcall precache(void* self,void*) {
    if (GetCurrentThreadId()!=thread || gone || !requested || busy) return original(self);
    Error error; Busy call;
    Record r; r.id=loading::active && loading::current.kind!=loading::startup?loading::current.id:0;
    r.before=counts(); const Tick begin=clock_now(); Raw result;
    r.reason=index_reason(); r.optimized=r.reason==available;
    if (r.optimized) {
        prefix::Lifetime lifetime;
        prefix::Active window;
        prefix::Extensions extensions;
        SetLastError(error.value);
        result=original(self); error.value=GetLastError(); r.peak=prefix::used;
    } else {
        SetLastError(error.value);
        result=original(self); error.value=GetLastError();
    }
    r.ticks=clock_now()-begin; r.after=counts(); enqueue(r);
    return result;
}
void BLSE_CALL restarted(const char*,const void*,size_t,void*) {
    if (GetCurrentThreadId()!=thread) return;
    gone=true;
    host->log("Loading sound precache index stopped after an engine restart; it works again after restarting the game.");
}
#include "loading_sound_precache_bytes.inc"
bool bodies_match(std::uintptr_t base) {
    for (const auto& b:precache_bodies) {
        unsigned char expected[1024]; if (b.size>sizeof expected) return false;
        std::memcpy(expected,b.code,b.size);
        for (unsigned i=0;i<b.fixup_count;++i) {
            std::uint32_t value=0; std::memcpy(&value,expected+b.fixups[i],4);
            value+=static_cast<std::uint32_t>(base-0x10000000u);
            std::memcpy(expected+b.fixups[i],&value,4);
        }
        if (!same_bytes(base+b.rva,expected,b.size)) return false;
    }
    return true;
}
bool configured() {
    if (!BLSE_HOST_HAS(host,ini_get) || !host->ini_get) return false;
    char value[8]={}; size_t size=0;
    return host->ini_get(host->context,"engine_fixes","Loading","sound_precache_index",value,sizeof value,&size) &&
        size<sizeof value && !std::strcmp(value,"1");
}
void install_at(std::uintptr_t base) {
    if (!bodies_match(base)) { host->log("Loading sound precache index disabled: native worldspawn precache bytes differ."); return; }
    thread=GetCurrentThreadId(); LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart<=0) { host->log("Loading sound precache index disabled: performance clock unavailable."); return; }
    frequency=f.QuadPart;
    const auto subscription=host->subscribe(host->context,BLSE_MSG_ENGINE_RESTART,restarted,nullptr);
    if (!subscription) { host->log("Loading sound precache index disabled: restart not followed."); return; }
    // push ecx; push esi; mov esi,ecx; mov ecx,[global]: four whole instructions, no relative operand.
    unsigned char entry[10]={0x51,0x56,0x8b,0xf1,0x8b,0x0d};
    const auto global=static_cast<std::uint32_t>(base+precache_global_rva); std::memcpy(entry+6,&global,4);
    if (!code_hook(base,precache_rva,entry,sizeof entry,reinterpret_cast<void*>(precache),reinterpret_cast<void**>(&original),"sound_precache_index")) {
        host->unsubscribe(host->context,subscription); return;
    }
    requested=true;
    host->log("Loading sound precache index enabled: worldspawn precache *.wav listings only; needs startup_trace=1 and startup_pack_prefix=1.");
}
void install() {
    if (attempted) return; attempted=true;
    if (!configured()) return;
    if (!api_ready() || !server_profile_supported()) { host->log("Loading sound precache index disabled: unsupported API/profile."); return; }
    const auto module=GetModuleHandleW(L"vampire.dll");
    if (!module) { host->log("Loading sound precache index disabled: vampire.dll unavailable."); return; }
    install_at(reinterpret_cast<std::uintptr_t>(module));
}
} }
#endif
