// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Scoped KeyValues sibling append optimization. Included inside detail namespace.
namespace table_append {
using Parse=const char*(__thiscall*)(void*,void*,void*);
using Add=void*(__thiscall*)(void*,const char*);
Parse original_parse=nullptr; Add original_add=nullptr;
struct Frame { void* parent=nullptr; void* head=nullptr; void* tail=nullptr; std::uint64_t children=0; bool known=false; } frames[64];
unsigned depth=0,window=0; bool requested=false,installed=false;
std::uint64_t calls=0,avoided_links=0,fallbacks=0;
// Full native AddChild body. Only its pool's absolute operand needs relocation.
const unsigned char native_add[]={
0x56,0x8b,0xf1,0x6a,0x38,0xb9,0x08,0xb3,0x5e,0x10,0xe8,0xf1,0x9e,0xfb,0xff,0x85,0xc0,0x74,0x12,0x8b,0x4c,0x24,0x08,0x6a,0x01,0x51,0x8b,0xc8,0xe8,0xaf,0xf5,0xff,0xff,0x8b,0xd0,0xeb,0x02,0x33,0xd2,0x8b,0x4e,0x1c,0x85,0xc9,0x75,0x0c,0x89,0x56,0x1c,0x89,0x4a,0x18,0x8b,0xc2,0x5e,0xc2,0x04,0x00,0x8b,0x41,0x18,0x85,0xc0,0x74,0x09,0x8b,0xc8,0x8b,0x41,0x18,0x85,0xc0,0x75,0xf7,0x52,0xe8,0xa0,0xff,0xff,0xff,0xc7,0x42,0x18,0x00,0x00,0x00,0x00,0x8b,0xc2,0x5e,0xc2,0x04,0x00};
const unsigned char native_parser[]={
#include "startup_parser_body.inc"
};
void*& head(void* parent) { return static_cast<void**>(parent)[7]; }
void*& next(void* child) { return static_cast<void**>(child)[6]; }
struct Window {
    bool live=false;
    Window() { if (installed && GetCurrentThreadId()==thread) { live=true; if (!window++) { depth=0; calls=avoided_links=fallbacks=0; } } }
    ~Window() { if (live && !--window) depth=0; }
};
const char* __fastcall parse(void* self,void*,void* text,void* filesystem) {
    if (!window || GetCurrentThreadId()!=thread || depth==64) return original_parse(self,text,filesystem);
    const unsigned saved=depth++; frames[saved]={}; frames[saved].parent=self;
    struct Pop { unsigned value; ~Pop() { depth=value; } } pop{saved};
    return original_parse(self,text,filesystem);
}
void* __fastcall add(void* self,void*,const char* name) {
    if (!window || GetCurrentThreadId()!=thread || !depth || frames[depth-1].parent!=self) {
        if (window && GetCurrentThreadId()==thread) ++fallbacks;
        return original_add(self,name);
    }
    auto& frame=frames[depth-1]; ++calls;
    // The native recursive parser only appends. A changed head/tail invalidates
    // the hint; find the real tail rather than replacing an external insertion.
    if (!frame.known || frame.head!=head(self) || (frame.tail && next(frame.tail))) {
        frame.head=head(self); frame.tail=frame.head; frame.children=0;
        for (auto p=frame.head;p;p=next(p)) { frame.tail=p; ++frame.children; }
        frame.known=true;
    }
    // Native AddChild reads only parent+0x1c. A scratch empty parent takes its
    // allocation/constructor/empty-list branch without traversing the real list.
    void* scratch[8]={}; void* child=original_add(scratch,name);
    if (!child) return child; // native allocation failure normally faults first
    if (frame.tail) next(frame.tail)=child; else head(self)=frame.head=child;
    if (frame.children) avoided_links+=frame.children-1;
    frame.tail=child; ++frame.children;
    return child;
}
bool install(std::uintptr_t base) {
    if (!requested) return false;
    const auto entry=reinterpret_cast<const unsigned char*>(base+0x1544e0);
    std::int32_t delta; std::memcpy(&delta,entry+1,4);
    if (entry[0]==0xe9 && reinterpret_cast<std::uintptr_t>(entry+5)+delta==reinterpret_cast<std::uintptr_t>(add) && original_add && original_parse) return installed=true;
    unsigned char expected[sizeof native_add]; std::memcpy(expected,native_add,sizeof expected);
    const auto pool=base+0x5eb308; std::memcpy(expected+6,&pool,4);
    unsigned char parse_body[sizeof native_parser]; std::memcpy(parse_body,native_parser,sizeof parse_body);
    const auto close=base+0x2c3340,open=base+0x2c333c;
    std::memcpy(parse_body+49,&close,4); std::memcpy(parse_body+104,&open,4);
    const unsigned char parser[]={0x83,0xec,0x10,0x53,0x55,0x56};
    installed=false;
    if (std::memcmp(entry,expected,sizeof expected)) { host->log("Startup table append disabled: full native add body differs."); return false; }
    if (std::memcmp(reinterpret_cast<const void*>(base+0x153dd0),parse_body,sizeof parse_body)) { host->log("Startup table append disabled: full recursive parser body differs."); return false; }
    if (host->hook_function(host->context,reinterpret_cast<void*>(base+0x153dd0),parser,sizeof parser,reinterpret_cast<void*>(parse),reinterpret_cast<void**>(&original_parse))!=BLSE_HOOK_OK) return false;
    if (host->hook_function(host->context,const_cast<unsigned char*>(entry),expected,5,reinterpret_cast<void*>(add),reinterpret_cast<void**>(&original_add))!=BLSE_HOOK_OK) {
        host->unhook(host->context,reinterpret_cast<void*>(base+0x153dd0)); original_parse=nullptr; return false;
    }
    installed=true; host->log("Startup table append enabled (native allocation, scoped sibling tail)."); return true;
}
}
