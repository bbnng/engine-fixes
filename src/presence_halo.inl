// Presence target effects can outlive a vampire's visible body while its server
// entity remains valid. Only the client particle instance is cleaned up here.
std::uintptr_t presence_server=0;
unsigned presence_cleanup_logs=0;
const unsigned* presence_resolve(const unsigned* list,unsigned handle) {
    if (!list || handle==0xffffffffu) return nullptr;
    const auto entry=list+1+3*(handle&0x1fff);
    return entry[1]==(handle>>13)?reinterpret_cast<const unsigned*>(entry[0]):nullptr;
}
bool presence_dead_vampire(const unsigned* self) {
    if (!presence_server) return false;
    __try {
        // Exact C_EnvParticle class; +228 is its own network EHANDLE (IDA
        // 0x1009f060), distinct from its followed entity's handle at +392.
        if (!self || self[0]!=client+0x22ed24 || self[264]!=2) return false;
        const auto list=*reinterpret_cast<const unsigned* const*>(presence_server+0x566458);
        const auto effect=presence_resolve(list,self[57]);
        // Resolve the matching SERVER particle with the same full handle.
        // A client-only effect, another Discipline or a reused slot is excluded.
        if (!effect || effect[0]!=presence_server+0x44ba64 || effect[297]!=10 ||
            effect[302]!=self[98]) return false;
        const auto owner=presence_resolve(list,effect[302]);
        if (!owner || (owner[128]!=1 && owner[128]!=2)) return false;
        const auto cls=reinterpret_cast<const char*>(owner[71]);
        return cls && !std::strcmp(cls,"npc_VVampire");
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void install_presence_lifetime() {
    // BLSE's exact profile includes the server hash. No extra native hook.
    if (!server_profile_supported()) {
        host->log("Presence halo death cleanup disabled: unsupported server profile."); return;
    }
    presence_server=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"vampire.dll"));
    if (presence_server)
        host->log("Presence halo death cleanup enabled.");
}
