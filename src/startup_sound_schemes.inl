// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Scoped sound-scheme directory enumeration (client+0x154c90). Included inside
// restart_startup::detail after startup_table_append.inl (before probe_tables).
//
// The native routine lists each scheme directory twice through the game file
// system (files pass, then subdirectories pass). FileSystem_Stdio's pack search
// walks every pack directory tree to its end on each listing, so the second
// walk repeats the dominant cost. Inside the client data-table window only, list
// once through the same live IFileSystem vtable (pack/mod aware), copy the names
// to bounded plugin storage, then call the unchanged native file handler for
// every non-directory and the unchanged native directory handler for every
// directory except "." and "..", in the original order. Any unexpected state
// calls the original routine instead. Native objects, allocation and lookup
// tables are not touched by plugin code.
//
// Optional listing reuse (requested_cache, key startup_sound_scheme_cache):
// the FS reports a directory once per search path/pack that holds it, so the
// native recursion builds several child nodes with the same path and lists the
// identical pattern again. Inside ONE category tree walk (the call at
// client+0x15548d from the category loader to the tree), the first complete
// listing of an exact pattern is copied into bounded scoped storage and later
// nodes with the byte-identical pattern replay it: every entry, duplicates and
// order included, still goes through the same native handlers. The cache is
// cleared at every tree-walk begin/end and released at the Window end and in
// reset(). Static proof (IDA, client.dll): the tree walk is reached only from
// that loader call and itself, and inside it only 0x154c90 touches the FS.
#ifndef ENGINE_FIXES_SOUND_SCHEMES_INL
#define ENGINE_FIXES_SOUND_SCHEMES_INL
// Defined later in startup_detail.inl; declared so this file can precede probe_tables.
bool call_hook(std::uintptr_t base,unsigned site,unsigned target,void* detour,void** original,const char* label);
namespace sound_schemes {
using Enumerate=char(__thiscall*)(void*);
using Handler=int(__thiscall*)(void*,const char*,const char*);
using Resolve=int(__thiscall*)(void*,const char*,int*,int*);
using Pair=int(__thiscall*)(void*,int,int);
using First=const char*(__thiscall*)(void*,const char*,int*);
using Next=const char*(__thiscall*)(void*,int);
using Directory=unsigned char(__thiscall*)(void*,int); // native tests AL only
using Close=void(__thiscall*)(void*,int);

// IDA-verified bodies (client.dll image base 0x10000000), identical to
// (IDA export of the native routine).
// client+0x154c40: per-directory tree step (enumerate, then recurse children).
const unsigned char native_tree[]={
0x57,0x8b,0xf9,0xe8,0x48,0x00,0x00,0x00,0x84,0xc0,0x75,0x02,0x5f,0xc3,0x8b,0x47,0x1c,0x56,0x33,0xf6,
0x85,0xc0,0x7e,0x13,0x8b,0x47,0x20,0x8b,0x0c,0xb0,0xe8,0xdd,0xff,0xff,0xff,0x8b,0x47,0x1c,0x46,0x3b,
0xf0,0x7c,0xed,0x8b,0x47,0x04,0x5e,0x85,0xc0,0x75,0x12,0x68,0x40,0xa8,0x2c,0x10,0xff,0x15,0x14,0x31,
0x1e,0x10,0x83,0xc4,0x04,0x32,0xc0,0x5f,0xc3,0xb0,0x01,0x5f,0xc3};
// client+0x154c90: two-pass directory enumeration (thiscall, no stack args, AL=1).
const unsigned char native_enumerate[]={
0x81,0xec,0x0c,0x02,0x00,0x00,0x53,0x55,0x8b,0xe9,0x56,0x8d,0x8c,0x24,0x14,0x01,0x00,0x00,0xc7,0x44,
0x24,0x0c,0xff,0xff,0xff,0xff,0x8b,0x45,0x0c,0x50,0x68,0x88,0xa8,0x2c,0x10,0x51,0xe8,0xe4,0xc8,0x07,
0x00,0x8b,0x0d,0x0c,0x58,0x4a,0x10,0x83,0xc4,0x0c,0x8d,0x44,0x24,0x0c,0x8b,0x11,0x50,0x8d,0x84,0x24,
0x18,0x01,0x00,0x00,0x50,0xff,0x52,0x58,0x8b,0xf0,0x85,0xf6,0x74,0x4e,0x8b,0x0d,0x0c,0x58,0x4a,0x10,
0x8b,0x44,0x24,0x0c,0x50,0x8b,0x11,0xff,0x52,0x60,0x84,0xc0,0x75,0x24,0x8b,0x4d,0x0c,0x56,0x51,0x8d,
0x54,0x24,0x18,0x68,0x80,0xa8,0x2c,0x10,0x52,0xe8,0x9b,0xc8,0x07,0x00,0x83,0xc4,0x10,0x8d,0x44,0x24,
0x10,0x8b,0xcd,0x56,0x50,0xe8,0x5e,0x03,0x00,0x00,0x8b,0x0d,0x0c,0x58,0x4a,0x10,0x8b,0x44,0x24,0x0c,
0x50,0x8b,0x11,0xff,0x52,0x5c,0x8b,0xf0,0x85,0xf6,0x75,0xb2,0x8b,0x0d,0x0c,0x58,0x4a,0x10,0x8b,0x44,
0x24,0x0c,0x50,0x8b,0x11,0xff,0x52,0x64,0x8b,0x0d,0x0c,0x58,0x4a,0x10,0x8d,0x44,0x24,0x0c,0x50,0x8d,
0x84,0x24,0x18,0x01,0x00,0x00,0x8b,0x11,0x50,0xff,0x52,0x58,0x8b,0xd8,0x85,0xdb,0x74,0x74,0x57,0x8b,
0x0d,0x0c,0x58,0x4a,0x10,0x8b,0x44,0x24,0x10,0x50,0x8b,0x11,0xff,0x52,0x60,0x84,0xc0,0x74,0x48,0xbf,
0x7c,0xa8,0x2c,0x10,0x8b,0xf3,0xb9,0x02,0x00,0x00,0x00,0x33,0xd2,0xf3,0xa6,0x74,0x36,0xbf,0x78,0xa8,
0x2c,0x10,0x8b,0xf3,0xb9,0x03,0x00,0x00,0x00,0x33,0xc0,0xf3,0xa6,0x74,0x24,0x8b,0x4d,0x0c,0x53,0x51,
0x8d,0x54,0x24,0x1c,0x68,0x80,0xa8,0x2c,0x10,0x52,0xe8,0xfa,0xc7,0x07,0x00,0x83,0xc4,0x10,0x8d,0x44,
0x24,0x14,0x8b,0xcd,0x53,0x50,0xe8,0x1d,0x03,0x00,0x00,0x8b,0x0d,0x0c,0x58,0x4a,0x10,0x8b,0x44,0x24,
0x10,0x50,0x8b,0x11,0xff,0x52,0x5c,0x8b,0xd8,0x85,0xdb,0x75,0x8e,0x5f,0x8b,0x0d,0x0c,0x58,0x4a,0x10,
0x8b,0x44,0x24,0x0c,0x50,0x8b,0x11,0xff,0x52,0x64,0x5e,0x5d,0xb0,0x01,0x5b,0x81,0xc4,0x0c,0x02,0x00,
0x00,0xc3};
// client+0x155070: per-file handler (resolver, prefix sum, grow, flag write).
const unsigned char native_file[]={
0x51,0x8b,0x54,0x24,0x0c,0x56,0x8b,0xf1,0x8d,0x44,0x24,0x10,0x8d,0x4c,0x24,0x04,0x50,0x51,0x8b,0x4e,
0x04,0x52,0xe8,0xb5,0x04,0x00,0x00,0x8b,0x44,0x24,0x04,0x83,0xf8,0xff,0x74,0x2b,0x8b,0x4c,0x24,0x10,
0x57,0x51,0x8b,0x4e,0x04,0x50,0xe8,0xfd,0x05,0x00,0x00,0x8b,0x54,0x24,0x14,0x8b,0x4e,0x04,0x8b,0xf8,
0x8b,0x44,0x24,0x08,0x52,0x50,0xe8,0x59,0x04,0x00,0x00,0x8b,0x4e,0x14,0xc6,0x04,0x0f,0x01,0x5f,0x5e,
0x59,0xc2,0x08,0x00};
// The directory handler must also remain native: processing it after closing
// the listing relies on its inspected allocation-only behavior (no FS calls).
const unsigned char native_dir[]={
0x56,0x8b,0xf1,0x57,0x8b,0x46,0x1c,0x8d,0x0c,0x85,0x04,0x00,0x00,0x00,0x51,0xe8,0xbd,0xc1,0x07,0x00,
0x8b,0x4e,0x1c,0x83,0xc4,0x04,0x85,0xc9,0x8b,0xf8,0x7e,0x25,0x33,0xc0,0x85,0xc9,0x7e,0x13,0x8b,0x56,
0x20,0x40,0x8b,0x4c,0x82,0xfc,0x89,0x4c,0x87,0xfc,0x8b,0x4e,0x1c,0x3b,0xc1,0x7c,0xed,0x8b,0x56,0x20,
0x52,0xe8,0x84,0xc1,0x07,0x00,0x83,0xc4,0x04,0x6a,0x24,0x89,0x7e,0x20,0xe8,0x82,0xc1,0x07,0x00,0x83,
0xc4,0x04,0x85,0xc0,0x74,0x09,0x8b,0xc8,0xe8,0xb3,0xf9,0xff,0xff,0xeb,0x02,0x33,0xc0,0x8b,0x4e,0x1c,
0x8b,0x56,0x20,0x89,0x04,0x8a,0x8b,0x44,0x24,0x10,0x8b,0x4c,0x24,0x0c,0x8b,0x56,0x04,0x50,0x8b,0x46,
0x1c,0x51,0x8b,0x4e,0x20,0x56,0x52,0x8b,0x0c,0x81,0xe8,0x19,0xfa,0xff,0xff,0x8b,0x46,0x1c,0x5f,0x40,
0x89,0x46,0x1c,0x5e,0xc2,0x08,0x00};
// client+0x155400: category loader (counts, root node via 0x154b70 with parent 0,
// then `call 0x154c40` at +141, the only outside entry to the tree walk).
const unsigned char native_loader[]={
0x81,0xec,0x04,0x01,0x00,0x00,0x56,0x8b,0xf1,0x57,0x8b,0x46,0x08,0x8b,0x78,0x14,0x8d,0x0c,0xbd,0x00,
0x00,0x00,0x00,0x51,0xe8,0x84,0xbe,0x07,0x00,0x89,0x46,0x0c,0x83,0xc4,0x04,0x33,0xc0,0x85,0xff,0x7e,
0x10,0x8b,0x56,0x0c,0x40,0x3b,0xc7,0xc7,0x44,0x82,0xfc,0x01,0x00,0x00,0x00,0x7c,0xf0,0x8b,0x46,0x04,
0x85,0xc0,0x8b,0xc8,0x75,0x05,0xb9,0x00,0xe3,0x2d,0x10,0x8b,0x46,0x08,0x83,0xc0,0x0c,0x8b,0x00,0x85,
0xc0,0x75,0x05,0xb8,0x00,0xe3,0x2d,0x10,0x51,0x50,0x8d,0x44,0x24,0x10,0x68,0x80,0xa8,0x2c,0x10,0x50,
0xe8,0x34,0xc1,0x07,0x00,0x8b,0x46,0x04,0x83,0xc4,0x10,0x85,0xc0,0x75,0x05,0xb8,0x00,0xe3,0x2d,0x10,
0x8d,0x4c,0x24,0x08,0x50,0x8d,0x7e,0x10,0x51,0x6a,0x00,0x56,0x8b,0xcf,0xe8,0xe5,0xf6,0xff,0xff,0x8b,
0xcf,0xe8,0xae,0xf7,0xff,0xff,0x5f,0x5e,0x81,0xc4,0x04,0x01,0x00,0x00,0xc3};
struct Fixup { unsigned short offset; unsigned rva; };   // absolute operand -> base+rva
struct Site { unsigned short offset; unsigned target; void* detour; }; // E8 at offset
const Fixup tree_fixups[]={{52,0x2ca840},{58,0x1e3114}};
const Fixup enumerate_fixups[]={{31,0x2ca888},{43,0x4a580c},{76,0x4a580c},{104,0x2ca880},{132,0x4a580c},
    {154,0x4a580c},{170,0x4a580c},{201,0x4a580c},{220,0x2ca87c},{238,0x2ca878},{265,0x2ca880},{293,0x4a580c},{316,0x4a580c}};
const Fixup loader_fixups[]={{67,0x2de300},{84,0x2de300},{95,0x2ca880},{116,0x2de300}};
const unsigned tree_rva=0x154c40,enumerate_rva=0x154c90,file_rva=0x155070,dir_rva=0x1550d0;
const unsigned resolve_rva=0x155540,prefix_rva=0x1556a0,grow_rva=0x155510,filesystem_rva=0x4a580c;
const unsigned loader_rva=0x155400,loader_tree_site=141;
const unsigned entry_capacity=8192,arena_capacity=1u<<18;
// Scoped reuse budget (one VirtualAlloc block, ~412 KiB, only while a Window is open).
// Local catalog model: ~142 unique patterns, ~4.7k entries, ~73 KB of names in total.
const unsigned cache_listing_capacity=1024,cache_entry_capacity=16384,cache_arena_capacity=1u<<18;

struct Counter { std::uint64_t calls=0; LONGLONG ticks=0; };
Counter enumerate_time,fs_time,file_time,dir_time,resolve_time,prefix_time,grow_time,replay_time;
std::uint64_t optimized=0,entries=0,files=0,directories=0,fallbacks=0,avoided_fs_calls=0;
// Cache aggregates (whole trace): hits replayed, misses listed through the FS while
// caching, stored = unique cached listings (summed over tree walks), rejected =
// failed/unexpected/overflowed/oversize listings never cached, bypass_* = FS
// listings made without reuse for that reason.
std::uint64_t cache_scopes=0,cache_hits=0,cache_misses=0,cache_stored=0,cache_stored_entries=0,cache_stored_bytes=0;
std::uint64_t cache_rejected=0,cache_bypass_full=0,cache_bypass_identity=0,cache_bypass_scope=0,cache_nested=0,cache_alloc_failed=0,cache_avoided_fs_calls=0;
unsigned cache_peak_bytes=0;
bool requested=false,ready=false,busy=false; unsigned window=0,hooks=0;
bool requested_cache=false,cache_ready=false,scope_live=false; unsigned scope_depth=0;
Enumerate original_enumerate=nullptr,original_tree=nullptr; Handler original_file=nullptr,original_dir=nullptr;
Resolve original_resolve=nullptr; Pair original_prefix=nullptr,original_grow=nullptr;
Handler native_file_handler=nullptr,native_dir_handler=nullptr; void* const* filesystem=nullptr;
struct Entry { unsigned offset; bool directory; } listing[entry_capacity];
char arena[arena_capacity];

// Cached listing: pattern bytes, then names exactly as the single pass stored them
// (entry offsets relative to the names block, so replay is two memcpy calls).
struct Cached { unsigned hash,pattern,length,names,bytes,first,count; };
struct Cache { Cached listings[cache_listing_capacity]; Entry entries[cache_entry_capacity]; char arena[cache_arena_capacity]; };
Cache* cache=nullptr; unsigned cached_listings=0,cached_entries=0,cached_bytes=0;
// Live IFileSystem identity: object, vtable, FindFirst/FindNext/IsDirectory/FindClose.
struct Identity { void* object; const void* table; const void* slots[4]; };
Identity cache_identity{}; bool identity_set=false;

bool main_thread() { return GetCurrentThreadId()==thread; }
bool measuring() { return tracing && window && main_thread(); }
struct Timer {
    Counter* counter; LONGLONG begin=0;
    explicit Timer(Counter& value):counter(measuring()?&value:nullptr) { if (counter) begin=now(); }
    ~Timer() { if (counter) { ++counter->calls; counter->ticks+=now()-begin; } }
};
void clear_cache() { cached_listings=cached_entries=cached_bytes=0; identity_set=false; cache_identity=Identity{}; }
// Drops every cached listing and the storage. Also recovers scope state that an
// abnormal unwind (SEH without C++ destructors) could otherwise leave set.
void release_cache() {
    if (cache) VirtualFree(cache,0,MEM_RELEASE);
    cache=nullptr; clear_cache(); scope_live=false; scope_depth=0;
}
// Plugin-owned RAII scope for the client data-table call. Nested scopes count.
// Cache storage never outlives the outermost Window.
struct Window {
    bool live=false;
    Window() { if (main_thread()) { live=true; if (!window++) release_cache(); } }
    ~Window() { if (live && !--window) release_cache(); }
};
// One category tree walk. Nested or re-entered walks invalidate reuse until the
// outer walk ends; every begin and end clears all cached listings.
struct CacheScope {
    CacheScope() {
        if (scope_depth++) { if (scope_live) ++cache_nested; scope_live=false; clear_cache(); return; }
        clear_cache(); scope_live=false;
        if (!cache_ready || !window) return;
        if (!cache) cache=static_cast<Cache*>(VirtualAlloc(nullptr,sizeof(Cache),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        if (!cache) { ++cache_alloc_failed; return; }
        scope_live=true; ++cache_scopes;
    }
    ~CacheScope() { if (scope_depth) --scope_depth; if (!scope_depth) { clear_cache(); scope_live=false; } }
    CacheScope(const CacheScope&)=delete; CacheScope& operator=(const CacheScope&)=delete;
};
struct Busy {
    Busy() { busy=true; } ~Busy() { busy=false; }
    Busy(const Busy&)=delete; Busy& operator=(const Busy&)=delete;
};

int __fastcall timed_file(void* node,void*,const char* path,const char* name) { Timer t(file_time); return original_file(node,path,name); }
int __fastcall timed_dir(void* node,void*,const char* path,const char* name) { Timer t(dir_time); return original_dir(node,path,name); }
int __fastcall timed_resolve(void* table,void*,const char* name,int* id,int* variant) { Timer t(resolve_time); return original_resolve(table,name,id,variant); }
int __fastcall timed_prefix(void* table,void*,int id,int variant) { Timer t(prefix_time); return original_prefix(table,id,variant); }
int __fastcall timed_grow(void* table,void*,int id,int variant) { Timer t(grow_time); return original_grow(table,id,variant); }

// Native "%s\\%s" (and the pattern) into a bounded buffer. Oversize results are
// refused, never truncated: the caller then runs the unchanged native routine,
// so any pre-existing native behaviour for such names is kept, not introduced.
bool join(char* out,size_t capacity,const char* first,const char* middle,const char* last,const char* tail="") {
    const size_t a=std::strlen(first),b=std::strlen(middle),c=std::strlen(last),d=std::strlen(tail);
    if (a+b+c+d+1>capacity) return false;
    std::memcpy(out,first,a); std::memcpy(out+a,middle,b); std::memcpy(out+a+b,last,c); std::memcpy(out+a+b+c,tail,d+1);
    return true;
}
void* const* table() { return *static_cast<void* const* const*>(*filesystem); }
Identity identity() {
    Identity id{}; id.object=*filesystem; const auto slots=table(); id.table=slots;
    for (unsigned i=0;i<4;++i) id.slots[i]=slots[22+i];
    return id;
}
bool same(const Identity& a,const Identity& b) { return !std::memcmp(&a,&b,sizeof a); }
unsigned fnv(const char* text,size_t length) { unsigned h=2166136261u; for (size_t i=0;i<length;++i) h=(h^static_cast<unsigned char>(text[i]))*16777619u; return h; }
// Exact byte comparison: no case folding or slash normalization.
const Cached* lookup(const char* pattern,unsigned length,unsigned hash) {
    for (unsigned i=0;i<cached_listings;++i) {
        const Cached& c=cache->listings[i];
        if (c.hash==hash && c.length==length && !std::memcmp(cache->arena+c.pattern,pattern,length)) return &c;
    }
    return nullptr;
}
bool store(const char* pattern,unsigned length,unsigned hash,unsigned listed,unsigned used) {
    if (cached_listings==cache_listing_capacity || listed>cache_entry_capacity-cached_entries ||
        length>cache_arena_capacity-cached_bytes || used>cache_arena_capacity-cached_bytes-length) return false;
    Cached& c=cache->listings[cached_listings];
    c={hash,cached_bytes,length,cached_bytes+length,used,cached_entries,listed};
    std::memcpy(cache->arena+c.pattern,pattern,length); std::memcpy(cache->arena+c.names,arena,used);
    std::memcpy(cache->entries+c.first,listing,listed*sizeof(Entry));
    ++cached_listings; cached_entries+=listed; cached_bytes+=length+used;
    ++cache_stored; cache_stored_entries+=listed; cache_stored_bytes+=length+used;
    const unsigned footprint=cached_bytes+cached_entries*static_cast<unsigned>(sizeof(Entry))+cached_listings*static_cast<unsigned>(sizeof(Cached));
    if (footprint>cache_peak_bytes) cache_peak_bytes=footprint;
    return true;
}
char fallback(void* node) { ++fallbacks; return original_enumerate(node); }
char single_pass(void* node) {
    const char* path=*reinterpret_cast<const char* const*>(static_cast<char*>(node)+12);
    char pattern[260];
    if (!path || !*filesystem || !join(pattern,sizeof pattern,"sound\\",path,"\\*.*")) return fallback(node);
    const auto length=static_cast<unsigned>(std::strlen(pattern));
    bool caching=scope_live && scope_depth==1 && cache;
    if (cache_ready && !caching) ++cache_bypass_scope;
    if (caching) {
        const Identity now_id=identity();
        if (!identity_set) { cache_identity=now_id; identity_set=true; }
        else if (!same(now_id,cache_identity)) { ++cache_bypass_identity; clear_cache(); scope_live=caching=false; }
    }
    const unsigned hash=caching?fnv(pattern,length):0;
    const Cached* hit=caching?lookup(pattern,length,hash):nullptr;
    unsigned listed=0,used=0; bool overflow=false,found=false; int handle=-1;
    if (hit) {
        Timer t(replay_time); listed=hit->count; used=hit->bytes;
        std::memcpy(arena,cache->arena+hit->names,used); std::memcpy(listing,cache->entries+hit->first,listed*sizeof(Entry));
    } else {
        Timer t(fs_time); pack_prefix::Active prefix;
        // Same calls and order as the native first pass: FindFirst, then per entry
        // IsDirectory and FindNext; FindClose once even when FindFirst fails.
        for (const char* name=reinterpret_cast<First>(table()[22])(*filesystem,pattern,&handle);name;
             name=reinterpret_cast<Next>(table()[23])(*filesystem,handle)) {
            found=true;
            const bool directory=reinterpret_cast<Directory>(table()[24])(*filesystem,handle)!=0;
            const size_t size=std::strlen(name)+1;
            if (listed==entry_capacity || size>arena_capacity-used) { overflow=true; break; }
            std::memcpy(arena+used,name,size); listing[listed++]={used,directory}; used+=static_cast<unsigned>(size);
        }
        reinterpret_cast<Close>(table()[25])(*filesystem,handle);
        if (caching) ++cache_misses;
    }
    if (overflow) { if (caching) ++cache_rejected; return fallback(node); }
    char buffer[260];
    for (unsigned i=0;i<listed;++i) { // validate every native buffer before any handler runs
        const char* name=arena+listing[i].offset;
        if (!listing[i].directory ? !join(buffer,260,path,"\\",name) :
            (std::strcmp(name,".") && std::strcmp(name,"..") && !join(buffer,256,path,"\\",name))) {
            if (caching && !hit) ++cache_rejected;
            return fallback(node);
        }
    }
    if (caching && !hit) {
        // Only a complete, successful listing under an unchanged FS identity is kept.
        if (!found || handle==-1) ++cache_rejected;
        else if (!same(identity(),cache_identity)) { ++cache_bypass_identity; clear_cache(); scope_live=false; }
        else if (!store(pattern,length,hash,listed,used)) ++cache_bypass_full;
    }
    ++optimized; entries+=listed; avoided_fs_calls+=hit?4ull+4ull*listed:2ull+2ull*listed;
    if (hit) { ++cache_hits; cache_avoided_fs_calls+=2ull+2ull*listed; }
    for (unsigned i=0;i<listed;++i) if (!listing[i].directory) {
        const char* name=arena+listing[i].offset; join(buffer,260,path,"\\",name);
        Timer t(file_time); ++files; native_file_handler(node,buffer,name);
    }
    for (unsigned i=0;i<listed;++i) if (listing[i].directory) {
        const char* name=arena+listing[i].offset;
        if (!std::strcmp(name,".") || !std::strcmp(name,"..")) continue;
        join(buffer,256,path,"\\",name);
        Timer t(dir_time); ++directories; native_dir_handler(node,buffer,name);
    }
    return 1;
}
char __fastcall enumerate(void* node,void*) {
    Timer t(enumerate_time);
    if (!ready || !window || !main_thread()) return original_enumerate(node);
    if (busy) return fallback(node); // listing storage is single-owner
    Busy owner; return single_pass(node);
}
// Replaces the loader's call to the tree walk; AL result passes through unchanged.
char __fastcall tree_scope(void* node,void*) {
    if (!cache_ready || !main_thread()) return original_tree(node);
    CacheScope scope; return original_tree(node);
}

bool body(std::uintptr_t base,unsigned rva,const unsigned char* native,size_t size,const Fixup* fixups,size_t fixup_count,const Site* sites,size_t site_count) {
    unsigned char expected[400]; if (size>sizeof expected) return false;
    std::memcpy(expected,native,size);
    for (size_t i=0;i<fixup_count;++i) { const auto value=static_cast<std::uint32_t>(base+fixups[i].rva); std::memcpy(expected+fixups[i].offset,&value,4); }
    const auto code=reinterpret_cast<const unsigned char*>(base+rva);
    for (size_t i=0;i<site_count;++i) { // a call site may already carry our own detour
        const auto at=sites[i].offset; std::int32_t delta; std::memcpy(&delta,code+at+1,4);
        const auto destination=reinterpret_cast<std::uintptr_t>(code+at+5)+delta;
        if (code[at]==0xe8 && (destination==base+sites[i].target || destination==reinterpret_cast<std::uintptr_t>(sites[i].detour))) std::memcpy(expected+at+1,&delta,4);
    }
    return !std::memcmp(code,expected,size);
}
bool strings(std::uintptr_t base) {
    const struct { unsigned rva; const char* text; } list[]={{0x2ca888,"sound\\%s\\*.*"},{0x2ca880,"%s\\%s"},{0x2ca87c,"."},{0x2ca878,".."}};
    for (const auto& s:list) if (std::memcmp(reinterpret_cast<const void*>(base+s.rva),s.text,std::strlen(s.text)+1)) return false;
    return true;
}
void install(std::uintptr_t base) {
    ready=false; cache_ready=false; hooks=0;
    const Site tree_sites[]={{3,enumerate_rva,reinterpret_cast<void*>(enumerate)}};
    const Site enumerate_sites[]={{125,file_rva,reinterpret_cast<void*>(timed_file)},{286,dir_rva,reinterpret_cast<void*>(timed_dir)}};
    const Site file_sites[]={{22,resolve_rva,reinterpret_cast<void*>(timed_resolve)},{46,prefix_rva,reinterpret_cast<void*>(timed_prefix)},{66,grow_rva,reinterpret_cast<void*>(timed_grow)}};
    const bool listing_ok=body(base,tree_rva,native_tree,sizeof native_tree,tree_fixups,2,tree_sites,1) &&
        body(base,enumerate_rva,native_enumerate,sizeof native_enumerate,enumerate_fixups,13,enumerate_sites,2) && strings(base);
    const bool file_ok=body(base,file_rva,native_file,sizeof native_file,nullptr,0,file_sites,3);
    const bool dir_ok=body(base,dir_rva,native_dir,sizeof native_dir,nullptr,0,nullptr,0);
    if(requested && (!file_ok || !dir_ok))host->log("Startup sound schemes: single pass off because a native handler changed.");
    char line[220];
    if (listing_ok) {
        const bool entry=call_hook(base,tree_rva+3,enumerate_rva,reinterpret_cast<void*>(enumerate),reinterpret_cast<void**>(&original_enumerate),"sound_scheme_enumerate");
        if (entry) ++hooks;
        if (call_hook(base,enumerate_rva+125,file_rva,reinterpret_cast<void*>(timed_file),reinterpret_cast<void**>(&original_file),"sound_scheme_file")) ++hooks;
        if (call_hook(base,enumerate_rva+286,dir_rva,reinterpret_cast<void*>(timed_dir),reinterpret_cast<void**>(&original_dir),"sound_scheme_directory")) ++hooks;
        if (entry) {
            native_file_handler=reinterpret_cast<Handler>(base+file_rva); native_dir_handler=reinterpret_cast<Handler>(base+dir_rva);
            filesystem=reinterpret_cast<void* const*>(base+filesystem_rva); ready=requested && file_ok && dir_ok;
        }
    } else host->log("Startup sound schemes: enumeration left native (tree/enumeration body or format strings differ).");
    if (file_ok) {
        if (call_hook(base,file_rva+22,resolve_rva,reinterpret_cast<void*>(timed_resolve),reinterpret_cast<void**>(&original_resolve),"sound_scheme_resolve")) ++hooks;
        if (call_hook(base,file_rva+46,prefix_rva,reinterpret_cast<void*>(timed_prefix),reinterpret_cast<void**>(&original_prefix),"sound_scheme_prefix")) ++hooks;
        if (call_hook(base,file_rva+66,grow_rva,reinterpret_cast<void*>(timed_grow),reinterpret_cast<void**>(&original_grow),"sound_scheme_grow")) ++hooks;
    } else host->log("Startup sound schemes: file handler body differs; resolver/prefix/grow timers off.");
    if (requested_cache) {
        const Site loader_sites[]={{loader_tree_site,tree_rva,reinterpret_cast<void*>(tree_scope)}};
        const bool loader_ok=body(base,loader_rva,native_loader,sizeof native_loader,loader_fixups,4,loader_sites,1);
        if (!ready) host->log("Startup sound scheme cache off: single pass is not enabled.");
        else if (!loader_ok) host->log("Startup sound scheme cache off: category loader body differs.");
        else cache_ready=call_hook(base,loader_rva+loader_tree_site,tree_rva,reinterpret_cast<void*>(tree_scope),reinterpret_cast<void**>(&original_tree),"sound_scheme_cache_scope");
        sprintf_s(line,"Startup sound scheme cache requested=1 enabled=%d budget_bytes=%u (scoped to one category tree walk)",cache_ready?1:0,static_cast<unsigned>(sizeof(Cache)));
        host->log(line);
    }
    sprintf_s(line,"Startup sound schemes requested=%d single_pass=%s timers=%u/6",requested?1:0,ready?"enabled":"off",hooks);
    host->log(line);
}
// Called at each trace begin (also after engine restart, before the new client
// is attached). Drops module addresses; the next install re-verifies bytes.
void reset() {
    release_cache(); ready=cache_ready=false; busy=false;
    native_file_handler=native_dir_handler=nullptr; filesystem=nullptr; hooks=0;
    enumerate_time=fs_time=file_time=dir_time=resolve_time=prefix_time=grow_time=replay_time=Counter{};
    optimized=entries=files=directories=fallbacks=avoided_fs_calls=0;
    cache_scopes=cache_hits=cache_misses=cache_stored=cache_stored_entries=cache_stored_bytes=0;
    cache_rejected=cache_bypass_full=cache_bypass_identity=cache_bypass_scope=cache_nested=cache_alloc_failed=cache_avoided_fs_calls=0;
    cache_peak_bytes=0;
}
// Two lines (the parent's report test counts them). Cache counters are folded in.
void report(double scale) {
    char line[900];
    sprintf_s(line,"Startup sound schemes requested=%d single_pass=%d optimized_listings=%llu entries=%llu files=%llu subdirectories=%llu fallbacks=%llu avoided_fs_calls=%llu"
        " cache_requested=%d cache=%d cache_scopes=%llu cache_hits=%llu cache_misses=%llu cache_unique_listings=%llu cache_entries=%llu cache_bytes=%llu cache_peak_bytes=%u"
        " cache_rejected=%llu cache_bypass_full=%llu cache_bypass_identity=%llu cache_bypass_scope=%llu cache_nested=%llu cache_alloc_failed=%llu cache_avoided_fs_calls=%llu",
        requested?1:0,ready?1:0,optimized,entries,files,directories,fallbacks,avoided_fs_calls,
        requested_cache?1:0,cache_ready?1:0,cache_scopes,cache_hits,cache_misses,cache_stored,cache_stored_entries,cache_stored_bytes,cache_peak_bytes,
        cache_rejected,cache_bypass_full,cache_bypass_identity,cache_bypass_scope,cache_nested,cache_alloc_failed,cache_avoided_fs_calls); host->log(line);
    sprintf_s(line,"Startup sound schemes inclusive_ms enumerate=%.3f/%llu single_pass_fs=%.3f/%llu cache_replay=%.3f/%llu file_handler=%.3f/%llu dir_handler=%.3f/%llu resolver=%.3f/%llu prefix=%.3f/%llu grow=%.3f/%llu"
        " (enumerate includes single_pass_fs, cache_replay and both handlers; single_pass_fs is every remaining actual FS listing; file_handler includes resolver/prefix/grow)",
        enumerate_time.ticks*scale,enumerate_time.calls,fs_time.ticks*scale,fs_time.calls,replay_time.ticks*scale,replay_time.calls,file_time.ticks*scale,file_time.calls,dir_time.ticks*scale,dir_time.calls,
        resolve_time.ticks*scale,resolve_time.calls,prefix_time.ticks*scale,prefix_time.calls,grow_time.ticks*scale,grow_time.calls); host->log(line);
}
}
#endif
