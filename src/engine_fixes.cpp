// Engine Fixes for Vampire: The Masquerade - Bloodlines (BLSE plugin).
// Code-level bug fixes plus optional smoothing tweaks. Every hook is gated by the exact game build
// (BLSE build profile for vampire.dll, SHA-256 for client.dll) and by whole-instruction byte checks,
// so an unknown or already patched game leaves the affected fix off and logs why.
#include "blse_api.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#pragma comment(lib, "bcrypt.lib")

BLSE_PLUGIN_INFO("engine_fixes", "1.0.0", "Engine Fixes", "Engine and game-code bug fixes for Bloodlines", "0.6", nullptr);

namespace {
const BLSE_HostAPI* host;
const char* const server_profile = "vtmb-x86-80d73e74-9d00b2c1-c546f4de-2ce854dd";

// --- settings ---
// Settings live in themed INI sections. A key is looked up in every section (names are unique)
// and in a legacy [Fixes] section, and is never written back: a missing key uses its default.
const char* const setting_sections[] = {"Effects", "Eyes", "NPCAnimation", "EyeGlances", "Fixes"};
bool read_setting(const char* key, char* out, size_t capacity) {
    for (const char* section : setting_sections) {
        size_t size = 0;
        if (host->ini_get(host->context, "engine_fixes", section, key, out, capacity, &size) && size < capacity) return true;
    }
    return false;
}
float setting_number(const char* key, const char* fallback, float low, float high) {
    char setting[32] = {};
    if (!read_setting(key, setting, sizeof(setting))) strcpy_s(setting, fallback);
    char* end = nullptr;
    const double value = std::strtod(setting, &end);
    if (end == setting || !std::isfinite(value) || value < low || value > high) return static_cast<float>(std::strtod(fallback, nullptr));
    return static_cast<float>(value);
}
bool enabled(const char* key, const char* fallback = "1") {
    char setting[16] = {};
    if (!read_setting(key, setting, sizeof(setting))) setting[0] = fallback[0];
    return setting[0] != '0';
}
bool server_profile_supported() {
    return host->build_profile && !std::strcmp(host->build_profile, server_profile);
}

// --- build check for client.dll (BLSE's profile covers vampire.dll) ---
// Hash the on-disk module before using any known layout or address. The result is cached.
bool hash_matches(HMODULE module, const unsigned char* expected) {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    unsigned char buffer[16384], digest[32];
    DWORD got;
    while (ok) {
        if (!ReadFile(file, buffer, sizeof(buffer), &got, nullptr)) { ok = false; break; }
        if (!got) break;
        ok = BCryptHashData(hash, buffer, got, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    return ok && std::memcmp(digest, expected, sizeof(digest)) == 0;
}
const unsigned char client_sha256[32] = {
    0xe8,0x8b,0xea,0xe0,0xdd,0x03,0xaf,0x06,0x49,0x3c,0x71,0xc5,0xe8,0xd8,0x7a,0x69,
    0x93,0xb5,0x4e,0x59,0x0c,0xb6,0xad,0x37,0xcd,0x35,0x13,0xc5,0x88,0x58,0x28,0x70};
HMODULE checked_module = nullptr;
bool checked_result = false;
bool supported(HMODULE module) {
    if (!module) return false;
    if (module != checked_module) { checked_result = hash_matches(module, client_sha256); checked_module = module; }
    return checked_result;
}

std::uintptr_t server_base;
std::uintptr_t client;

// --- NPC idle pool: NPCs must not pick the player's pc_idles animations ---
using CollectSequences = int (__cdecl*)(const void*, int, int*, int*, int, int, int, int);
CollectSequences original_collect;
bool exclude_player_idle(const void* model, int activity, int is_npc) {
    if (!model || !(is_npc & 0xff) || activity != 1) return false;
    const auto words = static_cast<const std::uint32_t*>(model);
    if (words[0] != 0x54534449 || words[1] != 2531 || words[68] != 1 || words[101] != 0) return false;
    const auto name = static_cast<const char*>(model) + 12;
    const char male[] = "character/shared/male/pc_idles.mdl";
    const char female[] = "character/shared/female/pc_idles.mdl";
    return (words[2] == 0x05bbf60e && !std::memcmp(name, male, sizeof(male))) ||
           (words[2] == 0x716e5125 && !std::memcmp(name, female, sizeof(female)));
}
int __cdecl collect_npc_sequences(const void* model, int activity, int* sequences, int* weights,
                                  int offset, int is_player, int is_npc, int skill) {
    if (exclude_player_idle(model, activity, is_npc)) return 0;
    return original_collect(model, activity, sequences, weights, offset, is_player, is_npc, skill);
}
void install_npc_idle_pool() {
    if (!server_profile_supported()) { host->log("NPC idle pool fix disabled: unsupported server profile."); return; }
    const auto module = GetModuleHandleW(L"vampire.dll");
    if (!module) return;
    const unsigned char expected[] = {0x51,0x8b,0x44,0x24,0x0c,0x56};
    void* trampoline = nullptr;
    if (host->hook_function(host->context, reinterpret_cast<unsigned char*>(module) + 0x427df0,
            expected, sizeof(expected), reinterpret_cast<void*>(collect_npc_sequences), &trampoline) != BLSE_HOOK_OK) {
        host->log("NPC idle pool fix disabled: byte check or hook conflict."); return;
    }
    original_collect = reinterpret_cast<CollectSequences>(trampoline);
    host->log("NPC idle pool fix enabled.");
}

// --- Same-tick walk phase: walk -> idle -> walk within one simulation tick keeps the walk phase ---
// vampire.dll 0x10272490 (SetActivityAndSequence) is only called for NPCs; it reads the same NPC
// fields itself, so the offsets below stay inside the object.
using SetActivitySequence = int (__thiscall*)(void*, int, int, int, int);
SetActivitySequence original_activity_sequence;
struct WalkPhase {
    void* entity = nullptr;
    float time = 0, cycle = 0, event_check = 0;
    int activity = 0, sequence = -1, idle_sequence = -1;
    unsigned char finished = 0;
};
WalkPhase walk_phases[256];

int transition_at(void* self, int activity, int sequence, int translated, int weapon, float time) {
    auto words = static_cast<int*>(self);
    auto values = static_cast<float*>(self);
    auto bytes = static_cast<unsigned char*>(self);
    auto& slot = walk_phases[(reinterpret_cast<std::uintptr_t>(self) >> 4) % 256];
    const WalkPhase previous = slot;
    slot.entity = nullptr; // every unrelated transition invalidates the pending phase
    const int old_activity = words[1019];
    const bool allowed = words[5936] != 4 && words[5936] != 7; // not scripted / dead
    const bool returning = allowed && previous.entity == self && previous.time == time &&
        old_activity == 1 && activity == previous.activity && sequence == previous.sequence &&
        words[444] == previous.idle_sequence && std::isfinite(values[446]) &&
        values[446] >= 0.0f && values[446] < 1.0f && !bytes[1628];
    WalkPhase outgoing;
    const bool stopping = allowed && (old_activity == 9 || old_activity == 19) && activity == 1 &&
        std::isfinite(values[446]) && values[446] >= 0.0f && values[446] <= 1.0f;
    if (stopping) outgoing = {self, time, values[446], values[406], old_activity, words[444], sequence, bytes[1628]};
    const int result = original_activity_sequence(self, activity, sequence, translated, weapon);
    if (words[1019] == activity && words[444] == sequence) {
        if (returning) {
            // Same simulation time proves the temporary idle has not occupied a later simulated
            // frame. Internal animation maintenance can advance its cycle slightly within this
            // call chain, so a non-zero cycle is accepted.
            values[446] = previous.cycle;
            values[406] = previous.event_check; // avoid replaying already passed animation events
            bytes[1628] = previous.finished;
        } else if (stopping && values[446] == 0.0f && !bytes[1628]) {
            slot = outgoing;
        }
    }
    return result;
}
int __fastcall activity_sequence(void* self, void*, int activity, int sequence, int translated, int weapon) {
    const auto globals = *reinterpret_cast<const unsigned char**>(server_base + 0x70b228);
    const float time = *reinterpret_cast<const float*>(globals + 12);
    return transition_at(self, activity, sequence, translated, weapon, time);
}
void install_walk_phase() {
    if (!server_profile_supported()) { host->log("Walk phase fix disabled: unsupported server profile."); return; }
    const auto module = GetModuleHandleW(L"vampire.dll");
    if (!module) return;
    server_base = reinterpret_cast<std::uintptr_t>(module);
    const unsigned char expected[] = {0x8b,0x44,0x24,0x0c,0x83,0xec,0x0c};
    void* trampoline = nullptr;
    if (host->hook_function(host->context, reinterpret_cast<void*>(server_base + 0x272490), expected, sizeof(expected),
            reinterpret_cast<void*>(activity_sequence), &trampoline) != BLSE_HOOK_OK) {
        host->log("Walk phase fix disabled: byte check or hook conflict.");
        return;
    }
    original_activity_sequence = reinterpret_cast<SetActivitySequence>(trampoline);
    host->log("Same-tick walk phase preservation enabled.");
}

// --- Idle sequence consistency: idle reselection must also update the ideal sequence ---
using ResetSequence = int (__thiscall*)(void*, int);
ResetSequence original_idle_reset;
int __fastcall reset_idle_consistently(void* self, void*, int sequence) {
    auto words = static_cast<int*>(self);
    const int old_ideal = words[5939];
    const bool idle = words[1019] == 1 && words[1020] == 1 && words[444] == old_ideal;
    const int result = original_idle_reset(self, sequence);
    // Only adopt the idle playback choice when nothing changed the AI intent in the call.
    if (idle && sequence >= 0 && words[444] == sequence && words[5939] == old_ideal &&
        words[1019] == 1 && words[1020] == 1) words[5939] = sequence;
    return result;
}
void install_idle_consistency() {
    if (!server_profile_supported()) { host->log("Idle sequence consistency fix disabled: unsupported server profile."); return; }
    const auto module = GetModuleHandleW(L"vampire.dll");
    if (!module) return;
    auto site = reinterpret_cast<unsigned char*>(module) + 0x26c635;
    const unsigned char expected[] = {0xe8,0x78,0x0a,0xda,0xff};
    if (std::memcmp(site, expected, sizeof(expected))) { host->log("Idle sequence consistency fix disabled: call bytes differ."); return; }
    void* target = nullptr;
    if (host->hook_call(host->context, site, reinterpret_cast<void*>(reset_idle_consistently), &target) != BLSE_HOOK_OK) {
        host->log("Idle sequence consistency fix disabled: hook refused."); return;
    }
    original_idle_reset = reinterpret_cast<ResetSequence>(target);
    host->log("Idle sequence consistency fix enabled.");
}

#include "pose_interpolation.inl"
#include "npc_steering.inl"
#include "eye_glance.inl"

// --- Client pose build (client.dll 0x100979b0): scope for NPC pose interpolation ---
using PoseBuild = int (__thiscall*)(void*, int, int, int);
PoseBuild original_pose_build;
int __fastcall pose_build(void* self, void*, int a, int b, int c) {
    PoseBlendScope pose_scope;
    begin_pose_blend(self, pose_scope);
    const int result = original_pose_build(self, a, b, c);
    end_pose_blend(pose_scope);
    return result;
}
void install_pose_interpolation() {
    const auto module = GetModuleHandleW(L"client.dll");
    const auto server = GetModuleHandleW(L"vampire.dll");
    if (!module || !supported(module) || !server_profile_supported() || !server) {
        host->log("NPC pose interpolation disabled: unsupported or unavailable game build.");
        return;
    }
    const unsigned char expected[] = {0xb8,0xe0,0x26,0x00,0x00};
    void* trampoline = nullptr;
    if (host->hook_function(host->context, reinterpret_cast<unsigned char*>(module) + 0x979b0,
            expected, sizeof(expected), reinterpret_cast<void*>(pose_build), &trampoline) != BLSE_HOOK_OK) {
        host->log("NPC pose interpolation disabled: byte check or hook conflict.");
        return;
    }
    original_pose_build = reinterpret_cast<PoseBuild>(trampoline);
    server_base = reinterpret_cast<std::uintptr_t>(server);
    pose_interpolation_ms = setting_number("npc_pose_interpolation_ms", "100", 10.0f, 500.0f);
    pose_interpolation = true;
    host->log("NPC pose interpolation enabled.");
}

// --- Eye target blend clamp (client.dll 0x100c412f) ---
// The client blends the old and new eye target with alpha = (client time - stamp) * 10, which is
// about -0.8 on every target change, so the eyes first jump the wrong way for a frame. Entered
// right after alpha is computed in ST(0); clamps it to [0,1]. NaN passes through unchanged.
// Flags and EAX are preserved because the displaced FLD changes neither.
void* original_eye_blend;
__declspec(naked) void eye_blend_clamp() {
    __asm {
        pushfd
        push eax
        fld1
        fcom st(1)
        fnstsw ax
        sahf
        jp eye_unordered
        jb eye_clamped
        fstp st(0)
        fldz
        fcom st(1)
        fnstsw ax
        sahf
        ja eye_clamped
    eye_unordered:
        fstp st(0)
        pop eax
        popfd
        jmp dword ptr [original_eye_blend]
    eye_clamped:
        fstp st(1)
        pop eax
        popfd
        jmp dword ptr [original_eye_blend]
    }
}
bool install_eye_blend() {
    const auto module = GetModuleHandleW(L"client.dll");
    if (!module || !supported(module)) {
        host->log("Eye target interpolation fix disabled: unsupported or unavailable client.dll.");
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    // Displaced instruction: fld dword ptr [client + 0x1e34ec] (constant 1); operand relocated for ASLR.
    unsigned char expected[6] = {0xd9, 0x05};
    const auto operand = static_cast<std::uint32_t>(base + 0x1e34ec);
    std::memcpy(expected + 2, &operand, sizeof(operand));
    if (host->hook_function(host->context, reinterpret_cast<void*>(base + 0xc412f),
            expected, sizeof(expected), reinterpret_cast<void*>(eye_blend_clamp), &original_eye_blend) != BLSE_HOOK_OK) {
        host->log("Eye target interpolation fix disabled: byte check or hook conflict.");
        return false;
    }
    host->log("Eye target interpolation fix enabled.");
    return true;
}

// --- Orphaned attached particles and the Presence halo (client.dll 0x100af150, particle think) ---
using Think = unsigned int (__thiscall*)(void*);
using Cleanup = int (__thiscall*)(void*);
Think original_think;
Cleanup cleanup;
#include "presence_halo.inl"
unsigned int __fastcall particle_think(void* self, void*) {
    auto words = static_cast<std::uint32_t*>(self);
    const bool dead_presence = presence_dead_vampire(words);
    bool orphan = false;
    if (words[283]) {
        // Networkable subobject's type accessor; mirrors the game's own type-11 guard.
        auto networkable = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(self) + 12);
        auto vtable = *reinterpret_cast<std::uintptr_t**>(networkable);
        using ClassID = int (__thiscall*)(void*);
        if (reinterpret_cast<ClassID>(vtable[8])(networkable) == 11) {
            const auto handle = words[98];
            const auto entries = *reinterpret_cast<std::uint32_t**>(client + 0x2b8e98);
            const auto index = (handle & 0x1fff) * 3;
            orphan = handle == 0xffffffff || entries[index + 2] != (handle >> 13) || !entries[index + 1];
        }
    }
    // Keep the game's detach, disable and scheduling behavior, then free the orphaned instance.
    const auto result = original_think(self);
    if ((orphan || dead_presence) && words[283]) {
        cleanup(self);
        if (dead_presence && presence_cleanup_logs < 16) {
            char message[192];
            sprintf_s(message, "Presence halo cleaned after vampire death: effect=0x%08x follow=0x%08x", words[57], words[98]);
            host->log(message);
            ++presence_cleanup_logs;
        }
    }
    return result;
}
bool install_particle_cleanup() {
    const auto module = GetModuleHandleW(L"client.dll");
    if (!module || !supported(module)) {
        host->log("Orphan particle fix disabled: unsupported or unavailable client.dll.");
        return false;
    }
    client = reinterpret_cast<std::uintptr_t>(module);
    cleanup = reinterpret_cast<Cleanup>(client + 0xaeed0);
    const unsigned char expected[] = {0x83,0xec,0x44,0x53,0x56};
    void* trampoline = nullptr;
    if (host->hook_function(host->context, reinterpret_cast<void*>(client + 0xaf150),
            expected, sizeof(expected), reinterpret_cast<void*>(particle_think), &trampoline) != BLSE_HOOK_OK) {
        host->log("Orphan particle fix disabled: hook byte check or patch conflict.");
        return false;
    }
    original_think = reinterpret_cast<Think>(trampoline);
    install_presence_lifetime();
    host->log("Orphan particle fix enabled.");
    return true;
}

// --- NPC move_yaw blend: the game ships with debug_npc_move_yaw_snap on ---
bool apply_move_yaw_blend() {
    if (!BLSE_HOST_HAS(host, cvar_set) || !host->cvar_get || !host->cvar_set) return false;
    char before[64] = {}, after[64] = {};
    size_t size = 0;
    if (!host->cvar_get(host->context, "debug_npc_move_yaw_snap", before, sizeof before, &size) || size >= sizeof before) return false;
    if (!host->cvar_set(host->context, "debug_npc_move_yaw_snap", "0")) return false;
    if (!host->cvar_get(host->context, "debug_npc_move_yaw_snap", after, sizeof after, &size) || size >= sizeof after) return false;
    char* end = nullptr;
    const double number = std::strtod(after, &end);
    return end != after && end && !*end && number == 0.0;
}
void BLSE_CALL refresh_move_yaw(const char*, const void*, size_t, void*) {
    if (!apply_move_yaw_blend()) host->log("NPC move_yaw blend: CVar unavailable or change not verified.");
}
void install_move_yaw_blend() {
    const auto module = GetModuleHandleW(L"client.dll");
    if (!module || !supported(module)) { host->log("NPC move_yaw blend disabled: unsupported client build."); return; }
    if (!apply_move_yaw_blend()) { host->log("NPC move_yaw blend disabled: CVar unavailable or change not verified."); return; }
    // Reapply on lifecycle changes only, not every frame.
    if (!host->subscribe(host->context, BLSE_MSG_LEVEL_INIT, refresh_move_yaw, nullptr) ||
        !host->subscribe(host->context, BLSE_MSG_GAME_LOADED, refresh_move_yaw, nullptr))
        host->log("NPC move_yaw blend: lifecycle subscription failed; initial change only.");
    host->log("NPC move_yaw blend enabled (debug_npc_move_yaw_snap 0).");
}

// Per-NPC state is keyed by object address or entity index. A new map reuses both, so clear it.
void BLSE_CALL reset_npc_state(const char*, const void*, size_t, void*) {
    for (auto& s : walk_phases) s = {};
    for (auto& s : steering_states) s = {};
    for (auto& s : clamp_guards) s = {};
    for (auto& s : move_yaw_states) s = {};
    for (auto& s : pose_blends) s = {};
}

std::uint64_t startup_subscription = 0;
void BLSE_CALL start_when_ready(const char*, const void*, size_t, void*) {
    if (!GetModuleHandleW(L"client.dll")) return;
    host->unsubscribe(host->context, startup_subscription);
    startup_subscription = 0;
    if (enabled("orphan_particles")) install_particle_cleanup();
    if (enabled("eye_target_interpolation")) install_eye_blend();
    if (enabled("idle_sequence_consistency")) install_idle_consistency();
    if (enabled("same_tick_walk_phase")) install_walk_phase();
    if (enabled("npc_idle_pool")) install_npc_idle_pool();
    if (enabled("npc_move_yaw_blend")) install_move_yaw_blend();
    if (enabled("npc_steering_damping")) install_steering_damping();
    if (enabled("npc_move_yaw_smoothing")) install_move_yaw_smoothing();
    if (enabled("npc_pose_interpolation")) install_pose_interpolation();
    install_eye_glance(); // [EyeGlances] enabled=1/0
    host->subscribe(host->context, BLSE_MSG_LEVEL_SHUTDOWN, reset_npc_state, nullptr);
}
}

extern "C" BLSE_EXPORT int BLSE_CALL BLSE_PluginLoad(const BLSE_HostAPI* api) {
    // BLSE's rule: check every field after `log` with BLSE_HOST_HAS before reading it.
    if (!api || api->api_version != BLSE_API_VERSION || !api->log) return 0;
    if (!BLSE_HOST_HAS(api, subscribe) || !BLSE_HOST_HAS(api, unsubscribe) || !BLSE_HOST_HAS(api, build_profile) ||
        !BLSE_HOST_HAS(api, hook_function) || !BLSE_HOST_HAS(api, hook_call) || !BLSE_HOST_HAS(api, unhook) ||
        !BLSE_HOST_HAS(api, ini_get) || !api->subscribe || !api->unsubscribe || !api->hook_function ||
        !api->hook_call || !api->unhook || !api->ini_get) {
        api->log("Engine Fixes not loaded: this BLSE is too old (0.6.1 or newer is required).");
        return 0;
    }
    host = api;
    startup_subscription = host->subscribe(host->context, BLSE_MSG_GAME_FRAME, start_when_ready, nullptr);
    if (!startup_subscription) return 0;
    host->log("Waiting for client.dll before installing fixes.");
    return 1;
}
