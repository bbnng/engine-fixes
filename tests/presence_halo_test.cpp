#include "../src/engine_fixes.cpp"
#include <cassert>
unsigned char presence_test_client[0x2b9000],presence_test_server[0x566500];
unsigned presence_test_client_list[1+3*8192],presence_test_server_list[1+3*8192];
unsigned presence_test_self[304],presence_test_effect[306],presence_test_owner[256];
unsigned presence_test_calls=0,presence_test_cleaned=0,presence_test_logs=0;
bool presence_test_create=false,presence_test_consume=false;
int __fastcall presence_test_type(void*,void*) { return 11; }
unsigned __fastcall presence_test_think(void* self,void*) {
    ++presence_test_calls;
    if (presence_test_create) static_cast<unsigned*>(self)[283]=0x0e7dd06c;
    if (presence_test_consume) static_cast<unsigned*>(self)[283]=0;
    return 42;
}
int __fastcall presence_test_cleanup(void* self,void*) {
    assert(presence_test_calls==1); // preserve native update before cleanup
    ++presence_test_cleaned; static_cast<unsigned*>(self)[283]=0; return 0;
}
void BLSE_CALL presence_test_log(const char* text) {
    assert(std::strstr(text,"Presence halo cleaned after vampire death")); ++presence_test_logs;
}
int main() {
    BLSE_HostAPI mock={}; mock.log=presence_test_log; host=&mock;
    client=reinterpret_cast<std::uintptr_t>(presence_test_client);
    presence_server=reinterpret_cast<std::uintptr_t>(presence_test_server);
    *reinterpret_cast<void**>(client+0x2b8e98)=presence_test_client_list;
    *reinterpret_cast<void**>(presence_server+0x566458)=presence_test_server_list;
    auto self=presence_test_self; auto effect=presence_test_effect; auto owner=presence_test_owner;
    // Recorded identities: effect 1230, target 1218, serial 0.
    constexpr unsigned effect_handle=1230,target_handle=1218;
    self[0]=client+0x22ed24; self[57]=effect_handle; self[98]=target_handle;
    self[264]=2; self[262]=1; self[283]=0x0e7dd06c;
    std::uintptr_t type_table[9]={}; type_table[8]=reinterpret_cast<std::uintptr_t>(presence_test_type);
    self[3]=reinterpret_cast<unsigned>(type_table);
    effect[0]=presence_server+0x44ba64; effect[297]=10; effect[302]=target_handle;
    owner[128]=1; owner[71]=reinterpret_cast<unsigned>("npc_VVampire");
    presence_test_server_list[1+3*effect_handle]=reinterpret_cast<unsigned>(effect);
    presence_test_server_list[1+3*target_handle]=reinterpret_cast<unsigned>(owner);
    presence_test_client_list[1+3*target_handle]=reinterpret_cast<unsigned>(owner);
    assert(presence_dead_vampire(self));
    owner[128]=2; assert(presence_dead_vampire(self));
    owner[128]=0; assert(!presence_dead_vampire(self));
    owner[128]=3; assert(!presence_dead_vampire(self)); owner[128]=1;
    const char* excluded_classes[]={"player","npc_VHuman","npc_VPedestrian"};
    for (const char* cls:excluded_classes) {
        owner[71]=reinterpret_cast<unsigned>(cls); assert(!presence_dead_vampire(self));
    }
    owner[71]=1; assert(!presence_dead_vampire(self)); // unreadable string is excluded
    owner[71]=reinterpret_cast<unsigned>("npc_VVampire");
    presence_test_server_list[2+3*target_handle]=1; assert(!presence_dead_vampire(self));
    presence_test_server_list[2+3*target_handle]=0;
    presence_test_server_list[2+3*effect_handle]=1; assert(!presence_dead_vampire(self));
    presence_test_server_list[2+3*effect_handle]=0;
    effect[302]=target_handle+1; assert(!presence_dead_vampire(self)); effect[302]=target_handle;
    effect[297]=4; assert(!presence_dead_vampire(self)); effect[297]=10;
    effect[0]=presence_server+0x455f1c; assert(!presence_dead_vampire(self)); effect[0]=presence_server+0x44ba64;
    self[0]=client+0x22effc; assert(!presence_dead_vampire(self)); self[0]=client+0x22ed24;
    self[264]=14; assert(!presence_dead_vampire(self)); self[264]=2;
    self[57]=0xffffffffu; assert(!presence_dead_vampire(self)); self[57]=effect_handle;
    original_think=reinterpret_cast<Think>(presence_test_think); cleanup=reinterpret_cast<Cleanup>(presence_test_cleanup);
    unsigned self_before[304],effect_before[306],owner_before[256];
    std::memcpy(self_before,self,sizeof self_before); std::memcpy(effect_before,effect,sizeof effect_before);
    std::memcpy(owner_before,owner,sizeof owner_before);
    assert(particle_think(self,nullptr)==42 && presence_test_cleaned==1 && presence_test_logs==1);
    self_before[283]=0;
    assert(!std::memcmp(self_before,self,sizeof self_before));
    assert(!std::memcmp(effect_before,effect,sizeof effect_before) && !std::memcmp(owner_before,owner,sizeof owner_before));
    // An instance first created during the native call must also be stopped.
    presence_test_calls=presence_test_cleaned=0; presence_test_create=true;
    assert(particle_think(self,nullptr)==42 && presence_test_cleaned==1 && !self[283]);
    presence_test_create=false; presence_test_calls=presence_test_cleaned=0;
    particle_think(self,nullptr); assert(!presence_test_cleaned); // no double cleanup
    self[283]=123; presence_test_calls=0; presence_test_consume=true;
    particle_think(self,nullptr); assert(!presence_test_cleaned); // original already cleaned
    presence_test_consume=false; owner[128]=0; self[283]=123; presence_test_calls=0;
    particle_think(self,nullptr); assert(!presence_test_cleaned && self[283]==123); // living target preserved
    presence_server=0; assert(!presence_dead_vampire(self));
    std::puts("Presence halo: recorded dead-target case, serial reuse, class/Discipline/attachment/owner gates, living targets, original return/order, first-think creation, double-free avoidance and no server/activation changes passed.");
}
