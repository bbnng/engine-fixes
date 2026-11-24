// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Fix broken Environmental Effects (dsp_on 1) audio in engine.dll.
//
// Enabling "Environmental Effects" in Options -> Audio sets dsp_on 1, activating the Source 2003
// software DSP mixer in S_PaintChannels (engine.dll+0x135AB0). Three compounding bugs in the
// master-bus spatial wall-delay effect (dsp_spatial, default preset 40) corrupt all game audio
// (especially NPC dialogue and music) with metallic comb-filtering, hollow/tinny echo, and pitch
// warble:
//
// 1. S_PaintChannels (engine.dll+0x135F15) runs idsp_spatial (preset 40: MDY LINEAR 100.0 ms,
//    mix 1.0 = 100% wet) on IPAINTBUFFER -- the entire final master mix including dry voices,
//    music, and UI.
// 2. SX_Spatial (engine.dll+0x11AFC0) traces wall distances every 0.25 s, artificially separates
//    the L/R distances (with a sign bug when L <= R that pushes R past 600 units up to 900), and
//    drives MDY_Mod (engine.dll+0x123FC0) to apply different 0..75 ms delays to L and R while
//    gliding them whenever the player moves or turns.
// 3. MDY_Init (engine.dll+0x129880) computes 1.0f / modrate unconditionally. Preset 40 specifies
//    modrate = 0.0, yielding +INF; MDY_Alloc (engine.dll+0x123DD0) converts +INF * 44100 via
//    __ftol() to 0x80000000 (-2147483648) instead of 0, accidentally arming random delay
//    modulation in MDY_GetNext / MDY_GetNextN (which check modcount != 0).
//
// Fix:
// - Sanitize non-finite/non-positive rate_sec in MDY_Alloc so modrate=0 presets produce
//   modcount=0 instead of 0x80000000.
// - Skip SX_Spatial wall-delay ray-tracing and MDY_Mod pokes.
// - Bypass idsp_spatial in DSP_Process (engine.dll+0x12F310) while keeping all genuine
//   environmental effects intact (dsp_room / RoomDSP, dsp_water, dsp_player / Frenzy, dsp_speaker,
//   dsp_facingaway).

#include <cmath>
#include <cstdint>
#include <cstring>

namespace dsp_audio {

using SxSpatialFn = void (__cdecl*)();
using MdyAllocFn = void* (__cdecl*)(void* dly, float glide_sec, float rate_sec, float depth, float mix);
using DspProcessFn = void (__cdecl*)(int idsp, int* pfront, int* prear, int count);

constexpr std::uintptr_t kSxSpatialRva = 0x11AFC0;
constexpr std::uintptr_t kSxSpatialInitFlagRva = 0x1315C3C;
constexpr std::uintptr_t kMdyAllocRva = 0x123DD0;
constexpr std::uintptr_t kMdyPoolRva = 0x1316430;
constexpr std::uintptr_t kDspProcessRva = 0x12F310;
constexpr std::uintptr_t kSpatialIdLoadRva = 0x135F08;
constexpr std::uintptr_t kIdspSpatialRva = 0x13176D0;

MdyAllocFn orig_mdy_alloc = nullptr;
DspProcessFn orig_dsp_process = nullptr;
const int* g_idsp_spatial = nullptr;

inline void __cdecl sx_spatial_hook() {
    // Master-bus wall-delay modulation disabled; genuine environmental presets (room/water/player/speaker) unaffected.
}

inline void* __cdecl mdy_alloc_hook(void* dly, float glide_sec, float rate_sec, float depth, float mix) {
    if (!std::isfinite(rate_sec) || rate_sec <= 0.0f) {
        rate_sec = 0.0f;
    }
    return orig_mdy_alloc(dly, glide_sec, rate_sec, depth, mix);
}

inline void __cdecl dsp_process_hook(int idsp, int* pfront, int* prear, int count) {
    if (g_idsp_spatial && idsp > 0 && idsp == *g_idsp_spatial) {
        return;
    }
    orig_dsp_process(idsp, pfront, prear, count);
}

inline void reset() {
    orig_mdy_alloc = nullptr;
    orig_dsp_process = nullptr;
    g_idsp_spatial = nullptr;
}

inline void install() {
    HMODULE engine = GetModuleHandleW(L"engine.dll");
    if (!engine) return;
    if (!perf_runtime::module_range_ok(engine, kSxSpatialRva, 17) ||
        !perf_runtime::module_range_ok(engine, kMdyAllocRva, 16) ||
        !perf_runtime::module_range_ok(engine, kDspProcessRva, 16) ||
        !perf_runtime::module_range_ok(engine, kSpatialIdLoadRva, 6) ||
        !perf_runtime::module_range_ok(engine, kIdspSpatialRva, 4)) {
        host->log("DSP spatial fix: engine.dll layout out of range; leaving off.");
        return;
    }

    auto* base = reinterpret_cast<unsigned char*>(engine);
    const auto b = reinterpret_cast<std::uintptr_t>(base);

    static const unsigned char kSxSpatialHead[12] = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x81, 0xEC, 0x28, 0x01, 0x00, 0x00
    };
    static const unsigned char kMdyAllocHead[10] = {
        0x8B, 0x54, 0x24, 0x04, 0x85, 0xD2, 0x74, 0x26, 0x33, 0xC9
    };
    static const unsigned char kDspProcessHead[15] = {
        0x83, 0xEC, 0x0C, 0x53, 0x8B, 0x5C, 0x24, 0x14, 0x85, 0xDB, 0x55, 0x56, 0x57, 0x0F, 0x8C
    };

    const bool sx_ok =
        std::memcmp(base + kSxSpatialRva, kSxSpatialHead, sizeof(kSxSpatialHead)) == 0 &&
        base[kSxSpatialRva + 0x0C] == 0xA0 &&
        perf_runtime::u32_at_equals(base + kSxSpatialRva + 0x0D, b + kSxSpatialInitFlagRva);

    const bool mdy_ok =
        std::memcmp(base + kMdyAllocRva, kMdyAllocHead, sizeof(kMdyAllocHead)) == 0 &&
        base[kMdyAllocRva + 0x0A] == 0xB8 &&
        perf_runtime::u32_at_equals(base + kMdyAllocRva + 0x0B, b + kMdyPoolRva);

    const bool dsp_ok =
        std::memcmp(base + kDspProcessRva, kDspProcessHead, sizeof(kDspProcessHead)) == 0 &&
        base[kSpatialIdLoadRva] == 0xA1 &&
        perf_runtime::u32_at_equals(base + kSpatialIdLoadRva + 1, b + kIdspSpatialRva);

    static const unsigned char kSxSpatialDisplaced[6] = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8
    };
    static const unsigned char kMdyAllocDisplaced[6] = {
        0x8B, 0x54, 0x24, 0x04, 0x85, 0xD2
    };
    static const unsigned char kDspProcessDisplaced[8] = {
        0x83, 0xEC, 0x0C, 0x53, 0x8B, 0x5C, 0x24, 0x14
    };

    int hooked = 0;
    if (mdy_ok) {
        void* orig = nullptr;
        if (host->hook_function(host->context, base + kMdyAllocRva,
                kMdyAllocDisplaced, sizeof(kMdyAllocDisplaced),
                reinterpret_cast<void*>(&mdy_alloc_hook), &orig) == BLSE_HOOK_OK) {
            orig_mdy_alloc = reinterpret_cast<MdyAllocFn>(orig);
            ++hooked;
        }
    }
    if (sx_ok && dsp_ok) {
        void* orig_sx = nullptr;
        void* orig_dsp = nullptr;
        g_idsp_spatial = reinterpret_cast<const int*>(base + kIdspSpatialRva);
        if (host->hook_function(host->context, base + kSxSpatialRva,
                kSxSpatialDisplaced, sizeof(kSxSpatialDisplaced),
                reinterpret_cast<void*>(&sx_spatial_hook), &orig_sx) == BLSE_HOOK_OK) {
            ++hooked;
        }
        if (host->hook_function(host->context, base + kDspProcessRva,
                kDspProcessDisplaced, sizeof(kDspProcessDisplaced),
                reinterpret_cast<void*>(&dsp_process_hook), &orig_dsp) == BLSE_HOOK_OK) {
            orig_dsp_process = reinterpret_cast<DspProcessFn>(orig_dsp);
            ++hooked;
        }
    }

    if (hooked == 3) {
        host->log("DSP spatial fix installed: master-bus dsp_spatial wall-delay bypassed and MDY_Alloc +INF rate guarded.");
    } else {
        host->log("DSP spatial fix: partial or mismatched engine.dll bytes; check engine.dll build.");
    }
}

} // namespace dsp_audio
