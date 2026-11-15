// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Offline candidate for the UP model-name lookup. No hooks or game writes here.
// Every visited slot is checked against its raw 128-byte name before reuse;
// slots are still visited in native order. Misses and special names stay native.
#ifndef ENGINE_FIXES_MODEL_NAME_CACHE_H
#define ENGINE_FIXES_MODEL_NAME_CACHE_H
#include <cstdint>
#include <cstring>
namespace EngineFixesModelNames {
constexpr unsigned slots=1024,stride=0x184,name_size=128;
struct Name { char text[name_size]; unsigned hash=0,length=0; };
inline bool normalize(const char* source,Name& out) {
    out.hash=2166136261u; out.length=0; unsigned consumed=0; unsigned char previous=0;
    while(out.length<name_size-1 && *source) {
        if(++consumed>4096) return false; // unusual slash-only strings go native
        unsigned char c=static_cast<unsigned char>(*source++);
        if(c=='\\') c='/'; else if(c>='A' && c<='Z') c+=32;
        if(c=='/' && previous=='/') continue;
        out.text[out.length++]=static_cast<char>(c); previous=c;
        out.hash=(out.hash^c)*16777619u;
    }
    out.text[out.length]=0; return true;
}
struct Entry { char raw[name_size]; Name normalized; bool valid=false; };
struct Cache {
    Entry entries[slots];
    std::uint64_t queries=0,hits=0,misses=0,checks=0,updates=0,unsupported=0;
    void clear() { for(auto& entry:entries) entry.valid=false; queries=hits=misses=checks=updates=unsupported=0; }
    bool find(const void* table,const char* query,void*& result) {
        ++queries;
        if(!query || !*query || *query=='*') { ++unsupported; return false; }
        Name wanted; if(!normalize(query,wanted)) { ++unsupported; return false; }
        const auto first=static_cast<const char*>(table)+4;
        unsigned visited=0;
        for(unsigned i=0;i<slots;++i) {
            const char* live=first+stride*i; auto& cached=entries[i]; ++visited;
            if(!*live) { cached.valid=false; continue; }
            if(!cached.valid || std::memcmp(cached.raw,live,name_size)) {
                std::memcpy(cached.raw,live,name_size);
                if(!std::memchr(cached.raw,0,name_size)) { cached.valid=false; checks+=visited; ++unsupported; return false; }
                if(!normalize(cached.raw,cached.normalized)) { cached.valid=false; checks+=visited; ++unsupported; return false; }
                cached.valid=true; ++updates;
            }
            const auto& name=cached.normalized;
            if(name.hash==wanted.hash && name.length==wanted.length && !std::memcmp(name.text,wanted.text,name.length)) {
                checks+=visited; ++hits; result=const_cast<char*>(live); return true;
            }
        }
        checks+=visited; ++misses; return false;
    }
};
}
#endif
