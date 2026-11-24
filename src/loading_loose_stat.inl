// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Loading-only live negative pre-check for FileSystem_Stdio's loose-root _stat calls.
// Server precache listings (worldspawn: 36 sound directories, ~940 entries per load) call FS
// IsDirectory(bare name) per entry; it _stats "<loose root>\<name>" for every loose search path.
// The static CRT _stat reaches FindFirstFileA (through UP's FS IAT blocklist hook), which costs a
// directory open, query and close per miss. Here, for an exact drive path whose final component is
// a plain ASCII file name, GetFileAttributesExA is asked first, live, at the same point. Only its
// definite ERROR_FILE_NOT_FOUND short-cuts the call, reproducing native's CRT failure: return
// -1, FS CRT errno=ENOENT and _doserrno=2, LastError=2, stat buffer untouched. Existing paths, any
// other error and unsupported shapes run the unchanged native _stat. No answer is cached.
// LastError can differ for traversable but non-listable directories; see the documented limit.
// UP's hook only adds failures (blocklist), so it cannot turn a non-existent path into a success;
// the FS import slot and UP's own import are checked on every call. FS bodies must be native, or
// native plus UP12's startup "Fix Paths" edit (two dedupe separator bytes); anything else refuses and
// logs the differing bytes.
// [Loading] loose_stat_fast=1, requires loading_trace=1; an engine restart stops it.
#ifndef ENGINE_FIXES_LOADING_LOOSE_STAT_INL
#define ENGINE_FIXES_LOADING_LOOSE_STAT_INL
namespace loading { namespace loose_stat {
#include "loading_loose_stat_bytes.inc"
using Stat=Raw(__cdecl*)(const char*,void*); // int __cdecl _stat(path,buffer); raw EDX:EAX on fallthrough
using CrtError=int*(__cdecl*)();
constexpr unsigned isdir_rva=0x3a20,isdir_site=0x3ad1,dedupe_rva=0x3f00,dedupe_site=0x4049,stat_rva=0xe269;
constexpr unsigned errno_rva=0x10937,doserrno_rva=0x10940,fs_find_slot=0x1d034;
constexpr unsigned up_find_rva=0x9260,up_find_slot=0x2107c;
constexpr std::size_t max_name=128;
enum Site { isdir, dedupe };
Stat originals[2]={};
std::uintptr_t fs=0,exe=0,kernel_find=0;
unsigned char up_expected[sizeof up_find_body]={};
bool attempted=false,resolved=false,installed=false,gone=false,busy=false,up_known=false,task_queued=false;
std::uint64_t restart_subscription=0;

struct Record {
    unsigned id=0;
    std::uint64_t queries=0,fast=0,exists=0,other=0,unsupported=0,chain=0,sites[2]={};
    Tick fast_ticks=0,native_ticks=0,probe_ticks=0;
};
Record open,pending[4];
unsigned pending_count=0,dropped=0;
void close_record() {
    if(!open.id) return;
    if(open.id<=logged_captures) { if(pending_count<_countof(pending)) pending[pending_count++]=open; else ++dropped; }
    open=Record{};
}
bool capture_over(unsigned id) { return !(active && current.id==id && current.kind!=startup && !current.first_sim); }
void BLSE_CALL report(void*) {
    task_queued=false;
    if(GetCurrentThreadId()!=main_thread) return;
    if(open.id && capture_over(open.id)) close_record();
    for(unsigned i=0;i<pending_count;++i) {
        const auto& r=pending[i]; char line[640];
        _snprintf_s(line,_TRUNCATE,
            "Loading loose_stat id=%u queries=%llu fast_absent=%llu native_exists=%llu native_other_error=%llu native_unsupported=%llu native_chain=%llu isdir=%llu dedupe=%llu fast_ms=%.3f probe_overhead_ms=%.3f native_ms=%.3f (live GetFileAttributesExA; only ERROR_FILE_NOT_FOUND skips native _stat; nothing cached)",
            r.id,r.queries,r.fast,r.exists,r.other,r.unsupported,r.chain,r.sites[isdir],r.sites[dedupe],ms(r.fast_ticks),ms(r.probe_ticks),ms(r.native_ticks));
        host->log(line);
    }
    pending_count=0;
    if(dropped) { char line[96]; sprintf_s(line,"Loading loose_stat dropped_reports=%u",dropped); host->log(line); dropped=0; }
    if(open.id && !gone && host->add_task(host->context,report,nullptr)) task_queued=true; // capture still running
}
void queue_report() { if(!task_queued && host->add_task(host->context,report,nullptr)) task_queued=true; }

// FS's import slot must be the plain kernel32 export, or UP's verified blocklist hook whose own
// import is the plain export. Anything else (another wrapper) keeps every call native.
bool chain_ok() {
    __try {
        const auto slot=*reinterpret_cast<const volatile std::uintptr_t*>(fs+fs_find_slot);
        if(slot==kernel_find) return true;
        return up_known && slot==exe+up_find_rva && *reinterpret_cast<const volatile std::uintptr_t*>(exe+up_find_slot)==kernel_find &&
            !std::memcmp(reinterpret_cast<const void*>(exe+up_find_rva),up_expected,sizeof up_expected);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool reserved(const char* base,std::size_t n) {
    auto up=[](char c){ return static_cast<char>(c>='a' && c<='z'?c-32:c); };
    if(n==3) {
        const char t[3]={up(base[0]),up(base[1]),up(base[2])};
        for(const char* r:{"CON","PRN","AUX","NUL"}) if(!std::memcmp(t,r,3)) return true;
    }
    if(n==4 && base[3]>='0' && base[3]<='9') {
        const char t[3]={up(base[0]),up(base[1]),up(base[2])};
        if(!std::memcmp(t,"COM",3) || !std::memcmp(t,"LPT",3)) return true;
    }
    return false;
}
// "X:\...\<dir>\<name>" below MAX_PATH; the final component is [A-Za-z0-9_-.], no leading or
// trailing dot, not a DOS device; the separator before it is not a DBCS trail byte. Directory
// components are resolved identically by both APIs (same ANSI file-API conversion and Win32
// path canonicalization); only the final component is a FindFirstFileA pattern.
bool plain(const char* path) {
    std::size_t n=0; while(n<MAX_PATH && path[n]) ++n;
    if(n<6 || n>=MAX_PATH) return false;
    const char d=static_cast<char>(path[0]|0x20);
    if(d<'a' || d>'z' || path[1]!=':' || (path[2]!='\\' && path[2]!='/')) return false;
    std::size_t last=n; while(last>0 && path[last-1]!='\\' && path[last-1]!='/') --last;
    if(last<5 || static_cast<unsigned char>(path[last-2])>=0x80 || path[last-2]=='\\' || path[last-2]=='/') return false;
    const char* name=path+last; const std::size_t length=n-last;
    if(!length || length>max_name || name[0]=='.' || name[length-1]=='.') return false;
    std::size_t dot=length;
    for(std::size_t i=0;i<length;++i) {
        const char c=name[i];
        const bool ok=(c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='.';
        if(!ok) return false;
        if(c=='.' && dot==length) dot=i;
    }
    return !reserved(name,dot);
}
bool live() {
    return GetCurrentThreadId()==main_thread && installed && !gone && !busy && collecting() &&
        current.kind!=startup && !current.first_sim;
}
struct Busy { Busy(){busy=true;} ~Busy(){busy=false;} };
enum Outcome { absent, exists, other, unsupported, chain };
Raw filtered(Site site,const char* path,void* buffer) {
    const Stat original=originals[site];
    if(!live()) return original(path,buffer);
    const DWORD incoming=GetLastError(); Busy guard;
    if(open.id!=current.id) { close_record(); open.id=current.id; queue_report(); }
    ++open.queries; ++open.sites[site];
    const Tick begin=clock_now(); Outcome outcome=absent;
    if(!chain_ok()) outcome=chain;
    else if(!path || !plain(path)) outcome=unsupported;
    else {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if(GetFileAttributesExA(path,GetFileExInfoStandard,&data)) outcome=exists;
        else if(GetLastError()!=ERROR_FILE_NOT_FOUND) outcome=other;
    }
    if(outcome==absent) {
        // Native failure exit (+0xe37d): errno=ENOENT, _doserrno=ERROR_FILE_NOT_FOUND, return -1;
        // LastError is the failed FindFirstFileA's (UP's blocklist also sets 2).
        *reinterpret_cast<CrtError>(fs+errno_rva)()=2;
        *reinterpret_cast<CrtError>(fs+doserrno_rva)()=2;
        ++open.fast; open.fast_ticks+=clock_now()-begin;
        SetLastError(ERROR_FILE_NOT_FOUND);
        return 0xffffffffull; // EAX=-1; EDX is not read by either verified caller
    }
    open.probe_ticks+=clock_now()-begin;
    if(outcome==exists) ++open.exists; else if(outcome==other) ++open.other;
    else if(outcome==unsupported) ++open.unsupported; else ++open.chain;
    const Tick native_begin=clock_now(); SetLastError(incoming);
    const Raw result=original(path,buffer); const DWORD error=GetLastError();
    open.native_ticks+=clock_now()-native_begin; SetLastError(error);
    return result;
}
Raw __cdecl isdir_stat(const char* path,void* buffer) { return filtered(isdir,path,buffer); }
Raw __cdecl dedupe_stat(const char* path,void* buffer) { return filtered(dedupe,path,buffer); }

void relocated(unsigned char* out,const unsigned char* bytes,std::size_t size,const unsigned* fixups,std::size_t count,std::uint32_t delta) {
    std::memcpy(out,bytes,size);
    for(std::size_t i=0;i<count;++i) { std::uint32_t v; std::memcpy(&v,out+fixups[i],4); v+=delta; std::memcpy(out+fixups[i],&v,4); }
}
// Accepted FS layouts: all checked bodies native, or native except UP12's two "Fix Paths" separator
// bytes in dedupe (both, exact value). Any other difference, a hook included, refuses.
enum Layout { refused, native_layout, up_fix_paths };
Layout fs_kind=refused;
const char* layout_name(Layout l) { return l==native_layout?"native":l==up_fix_paths?"up_fix_paths":"refused"; }
bool live_copy(std::uintptr_t at,unsigned char* out,std::size_t size) {
    __try { std::memcpy(out,reinterpret_cast<const void*>(at),size); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Compares one checked region; on a difference writes "<name> +off FS+rva live=xx expected=yy; ..."
// (first four differing bytes) and the total count into why.
bool region(const char* name,std::uintptr_t base,unsigned rva,const unsigned char* expected,std::size_t size,char* why,std::size_t why_size) {
    unsigned char live[sizeof dedupe_body];
    static_assert(sizeof dedupe_body>=sizeof isdir_body && sizeof dedupe_body>=sizeof stat_body,"live buffer covers every region");
    if(size>sizeof live || !live_copy(base+rva,live,size)) { _snprintf_s(why,why_size,_TRUNCATE,"%s FS+0x%x unreadable",name,rva); return false; }
    std::size_t count=0,used=0; why[0]=0;
    for(std::size_t i=0;i<size;++i) if(live[i]!=expected[i]) {
        if(++count<=4 && used<why_size) {
            const int n=_snprintf_s(why+used,why_size-used,_TRUNCATE,"%s%s +0x%x FS+0x%x live=%02x expected=%02x",
                count==1?"":"; ",name,static_cast<unsigned>(i),rva+static_cast<unsigned>(i),live[i],expected[i]);
            used=n<0?why_size:used+static_cast<std::size_t>(n);
        }
    }
    if(!count) return true;
    if(used<why_size) _snprintf_s(why+used,why_size-used,_TRUNCATE," (%u byte(s) differ)",static_cast<unsigned>(count));
    return false;
}
Layout fs_layout(std::uintptr_t base,char* why=nullptr,std::size_t why_size=0) {
    char scratch[160]; if(!why || !why_size) { why=scratch; why_size=sizeof scratch; } why[0]=0;
    unsigned char stat[sizeof stat_body],fixed[sizeof dedupe_body];
    relocated(stat,stat_body,sizeof stat_body,stat_fixups,_countof(stat_fixups),static_cast<std::uint32_t>(base-0x10000000u));
    std::memcpy(fixed,dedupe_body,sizeof fixed);
    for(unsigned o:up_fix_paths_offsets) fixed[o]=up_fix_paths_value;
    if(!region("isdir",base,isdir_rva,isdir_body,sizeof isdir_body,why,why_size)) return refused;
    Layout layout=native_layout;
    if(!same_bytes(base+dedupe_rva,dedupe_body,sizeof dedupe_body)) {
        if(!same_bytes(base+dedupe_rva,fixed,sizeof fixed)) { region("dedupe",base,dedupe_rva,dedupe_body,sizeof dedupe_body,why,why_size); return refused; }
        layout=up_fix_paths;
    }
    if(!region("stat",base,stat_rva,stat,sizeof stat,why,why_size) || !region("errno",base,errno_rva,errno_body,sizeof errno_body,why,why_size) ||
        !region("doserrno",base,doserrno_rva,doserrno_body,sizeof doserrno_body,why,why_size)) return refused;
    return layout;
}
bool up_layout(std::uintptr_t executable) {
    relocated(up_expected,up_find_body,sizeof up_find_body,up_find_fixups,_countof(up_find_fixups),static_cast<std::uint32_t>(executable-0x400000u));
    return same_bytes(executable+up_find_rva,up_expected,sizeof up_expected);
}
void BLSE_CALL restarted(const char*,const void*,size_t,void*) {
    if(GetCurrentThreadId()!=main_thread) return;
    gone=true; close_record(); queue_report();
    host->log("Loading loose_stat stopped after an engine restart; it works again after restarting the game.");
}
bool configured() {
    if(!BLSE_HOST_HAS(host,ini_get) || !host->ini_get) return false;
    char value[8]={}; size_t size=0;
    return host->ini_get(host->context,"engine_fixes","Loading","loose_stat_fast",value,sizeof value,&size) && size<sizeof value && !std::strcmp(value,"1");
}
void install_at(std::uintptr_t filesystem,std::uintptr_t executable) {
    if(!main_thread || GetCurrentThreadId()!=main_thread) { host->log("Loading loose_stat disabled: probe main thread unavailable."); return; }
    char why[256]="module not loaded";
    const Layout layout=filesystem?fs_layout(filesystem,why,sizeof why):refused;
    if(layout==refused) {
        char line[384]; _snprintf_s(line,_TRUNCATE,"Loading loose_stat disabled: FileSystem_Stdio IsDirectory/dedupe/_stat bytes differ: %s.",why);
        host->log(line); return;
    }
    const auto kernel=GetModuleHandleW(L"kernel32.dll");
    kernel_find=reinterpret_cast<std::uintptr_t>(kernel?GetProcAddress(kernel,"FindFirstFileA"):nullptr);
    if(!kernel_find) { host->log("Loading loose_stat disabled: FindFirstFileA export unavailable."); return; }
    fs=filesystem; exe=executable; up_known=executable && up_layout(executable);
    if(!chain_ok()) { host->log("Loading loose_stat off: another mod already handles FileSystem_Stdio's file searches (FindFirstFileA), or this game build differs. The game's own file checks are used; nothing is wrong."); return; }
    restart_subscription=host->subscribe(host->context,BLSE_MSG_ENGINE_RESTART,restarted,nullptr);
    if(!restart_subscription) { host->log("Loading loose_stat disabled: restart not followed."); return; }
    void* const sites[2]={reinterpret_cast<void*>(fs+isdir_site),reinterpret_cast<void*>(fs+dedupe_site)};
    void* const detours[2]={reinterpret_cast<void*>(isdir_stat),reinterpret_cast<void*>(dedupe_stat)};
    for(unsigned i=0;i<2;++i) {
        void* original=nullptr;
        const int result=host->hook_call(host->context,sites[i],detours[i],&original);
        if(result!=BLSE_HOOK_OK || reinterpret_cast<std::uintptr_t>(original)!=fs+stat_rva) {
            if(result==BLSE_HOOK_OK) host->unhook(host->context,sites[i]);
            if(i) host->unhook(host->context,sites[0]);
            host->unsubscribe(host->context,restart_subscription); restart_subscription=0;
            char line[160]; sprintf_s(line,"Loading loose_stat disabled: call site %u refused (%d).",i,result); host->log(line); return;
        }
        originals[i]=reinterpret_cast<Stat>(original);
    }
    installed=true; fs_kind=layout;
    char line[256]; sprintf_s(line,"Loading loose_stat enabled: live negative pre-check for FS IsDirectory/dedupe _stat during traced loads only (find_import=%s fs_dedupe=%s).",
        *reinterpret_cast<const std::uintptr_t*>(fs+fs_find_slot)==kernel_find?"system":"up_blocklist",layout_name(layout));
    host->log(line);
}
void BLSE_CALL first_load(const char*,const void*,size_t,void*) {
    if(resolved || gone || !enabled_probe || GetCurrentThreadId()!=main_thread) return;
    resolved=true; // UP rewrote FS's import at executable startup, long before the first load.
    install_at(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"filesystem_stdio.dll")),reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}
void install() {
    if(attempted) return; attempted=true;
    if(!configured()) return;
    if(!enabled_probe || !api_ready() || !BLSE_HOST_HAS(host,hook_call) || !host->hook_call || !BLSE_HOST_HAS(host,unhook) || !host->unhook) {
        host->log("Loading loose_stat disabled: requires loading_trace=1 and supported probe/API."); return;
    }
    const auto before=host->subscribe(host->context,BLSE_MSG_BEFORE_LOAD,first_load,nullptr);
    if(!before) { host->log("Loading loose_stat disabled: before-load subscription failed."); return; }
    if(!host->subscribe(host->context,BLSE_MSG_LEVEL_CHANGE,first_load,nullptr)) {
        host->unsubscribe(host->context,before); host->log("Loading loose_stat disabled: level-change subscription failed."); return;
    }
    host->log("Loading loose_stat pending: installing at the first save/area load.");
}
} }
#endif
