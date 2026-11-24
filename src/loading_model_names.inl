// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Loading-only cache of UP FindModel's per-slot name normalization.
// Original slot order and byte validation preserve aliases/first-match semantics.
// Misses, brush names, non-C locale, foreign objects and gameplay remain native.
// [Loading] model_name_cache=1, requires loading_trace=1; an engine restart stops it.
#ifndef ENGINE_FIXES_LOADING_MODEL_NAMES_INL
#define ENGINE_FIXES_LOADING_MODEL_NAMES_INL
namespace loading { namespace model_names {
namespace names=EngineFixesModelNames;
using Lookup=Raw(__thiscall*)(void*,const char*); // pointer in EAX; EDX is caller-clobbered
Lookup original=nullptr;
names::Cache cache;
std::uintptr_t exe=0,expected_self=0;

bool attempted=false,installed=false,gone=false,busy=false,resolved=false;
unsigned open_id=0,pending_count=0,dropped=0;
Tick fast_ticks=0,native_ticks=0;
std::uint64_t native_calls=0;
struct Record { unsigned id=0; std::uint64_t queries=0,hits=0,misses=0,checks=0,updates=0,unsupported=0,native=0; Tick fast=0,slow=0; };
Record pending[4];
void close_record() {
    if(!open_id) return;
    const Record r={open_id,cache.queries,cache.hits,cache.misses,cache.checks,cache.updates,cache.unsupported,native_calls,fast_ticks,native_ticks};
    if(r.id<=logged_captures) { if(pending_count<_countof(pending)) pending[pending_count++]=r; else ++dropped; }
    open_id=0; cache.clear(); fast_ticks=native_ticks=0; native_calls=0;
}
void finished(const Session& s) { if(GetCurrentThreadId()==main_thread && s.id==open_id) close_record(); }
void reported(const Session& s) {
    if(GetCurrentThreadId()!=main_thread) return;
    for(unsigned i=0;i<pending_count;++i) if(pending[i].id==s.id) {
        const auto r=pending[i]; for(unsigned j=i+1;j<pending_count;++j) pending[j-1]=pending[j]; --pending_count;
        char line[600]; _snprintf_s(line,_TRUNCATE,
            "Loading model_names id=%u queries=%llu hits=%llu misses=%llu slot_checks=%llu renormalized=%llu unsupported=%llu native_calls=%llu cached_ms=%.3f native_ms=%.3f bytes=%u (all visited slot names byte-validated; first match and native miss handling preserved)",
            r.id,r.queries,r.hits,r.misses,r.checks,r.updates,r.unsupported,r.native,ms(r.fast),ms(r.slow),static_cast<unsigned>(sizeof cache));
        host->log(line); break;
    }
    if(dropped) { char line[100]; sprintf_s(line,"Loading model_names dropped_reports=%u",dropped); host->log(line); dropped=0; }
}
bool c_locale() {
    __try { return exe && !*reinterpret_cast<const int*>(exe+0x3f660); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct Busy { Busy(){busy=true;} ~Busy(){busy=false;} };
Raw __fastcall lookup(void* self,void*,const char* query) {
    if(!installed || gone || busy || GetCurrentThreadId()!=main_thread || !enabled_probe || !active || reporting ||
       current.kind==startup || current.first_sim || reinterpret_cast<std::uintptr_t>(self)!=expected_self || !c_locale()) return original(self,query);
    const DWORD incoming=GetLastError(); Busy guard;
    if(open_id!=current.id) { close_record(); open_id=current.id; }
    const Tick begin=clock_now(); void* found=nullptr; Raw result;
    if(cache.find(self,query,found)) {
        result=static_cast<Raw>(reinterpret_cast<std::uintptr_t>(found));
        fast_ticks+=clock_now()-begin; SetLastError(incoming);
    } else {
        fast_ticks+=clock_now()-begin; const Tick native_begin=clock_now();
        SetLastError(incoming); result=original(self,query); const DWORD error=GetLastError();
        native_ticks+=clock_now()-native_begin; ++native_calls; SetLastError(error);
    }
    // Raw retains native registers on fallthrough. A cached hit supplies the pointer in EAX;
    // the UP hit leaves EDX pointing into its dead stack buffer, which is outside the pointer ABI.
    return result;
}
void BLSE_CALL restarted_event(const char*,const void*,size_t,void*) {
    if(GetCurrentThreadId()!=main_thread) return;
    close_record(); gone=true;
    host->log("Loading model-name cache stopped after an engine restart; it works again after restarting the game.");
}
#include "loading_model_names_bytes.inc"
bool layout(std::uintptr_t executable,std::uintptr_t engine) {
    unsigned char compare[sizeof compare_body]; std::memcpy(compare,compare_body,sizeof compare);
    absolute(compare+14,executable+0x3f658);
    if(!same_bytes(executable+0x86a0,lookup_body,sizeof lookup_body) || !same_bytes(executable+0x13520,compare,sizeof compare) ||
       !same_bytes(engine+0xb8930,get_model_body,sizeof get_model_body)) return false;
    __try {
        const auto site=reinterpret_cast<const unsigned char*>(engine+0xb87a0); std::int32_t relative;
        std::memcpy(&relative,site+1,4);
        if(site[0]!=0xe9 || engine+0xb87a5+relative!=executable+0x86a0) return false;
        for(unsigned i=5;i<100;++i) if(site[i]!=0x90) return false;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool configured() {
    if(!BLSE_HOST_HAS(host,ini_get) || !host->ini_get) return false;
    char value[8]={}; size_t size=0;
    return host->ini_get(host->context,"engine_fixes","Loading","model_name_cache",value,sizeof value,&size) && size<sizeof value && !std::strcmp(value,"1");
}
void install_at(std::uintptr_t executable,std::uintptr_t engine) {
    if(!main_thread || GetCurrentThreadId()!=main_thread) { host->log("Loading model-name cache disabled: probe main thread unavailable."); return; }
    if(!layout(executable,engine)) { host->log("Loading model-name cache off: the game's model lookup is already changed by another mod (which may speed it up itself), or this game build differs. The game's own lookup is used; nothing is wrong."); return; }
    if(session_finished || session_reported) { host->log("Loading model-name cache disabled: diagnostic observer already owned."); return; }
    exe=executable; expected_self=engine+0x1007598;
    const auto subscription=host->subscribe(host->context,BLSE_MSG_ENGINE_RESTART,restarted_event,nullptr);
    if(!subscription) { host->log("Loading model-name cache disabled: restart not followed."); return; }
    if(!code_hook(executable,0x86a0,lookup_body,6,reinterpret_cast<void*>(lookup),reinterpret_cast<void**>(&original),"model_name_cache")) {
        host->unsubscribe(host->context,subscription); return;
    }
    session_finished=finished; session_reported=reported; installed=true;
    host->log("Loading model-name cache enabled: validated UP slot names during traced loads only; gameplay and native misses unchanged.");
}
void BLSE_CALL first_load(const char*,const void*,size_t,void*) {
    if(resolved || gone || !enabled_probe || GetCurrentThreadId()!=main_thread) return;
    resolved=true; // By the first load, UP's engine startup patches have completed.
    install_at(exe,engine_base);
}
void install() {
    if(attempted) return; attempted=true;
    if(!configured()) return;
    if(!enabled_probe || !api_ready() || !engine_base) { host->log("Loading model-name cache disabled: requires loading_trace=1 and supported probe/API."); return; }
    exe=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto before=host->subscribe(host->context,BLSE_MSG_BEFORE_LOAD,first_load,nullptr);
    if(!before) { host->log("Loading model-name cache disabled: before-load subscription failed."); return; }
    if(!host->subscribe(host->context,BLSE_MSG_LEVEL_CHANGE,first_load,nullptr)) {
        host->unsubscribe(host->context,before); host->log("Loading model-name cache disabled: level-change subscription failed."); return;
    }
    host->log("Loading model-name cache pending: installing at the first save/area load after UP startup patches.");
}
} }
#endif
