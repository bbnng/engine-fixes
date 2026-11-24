// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Runtime CPU optimizations for hot gameplay, UI, timer, and rendering paths identified by 1000 Hz
// profiling:
// 1. SSE2 vectorized AngleMatrix & AngleVectors (client.dll+0x1094E0, +0x1090A0, vampire.dll+0x139610):
//    replaces 3x slow x87 fsincos (~350 cycles) with a 4-wide SSE2 minimax sincos (< 2e-6 error).
// 2. Vtable-cached __RTDynamicCast in CViewRender::DrawHighEndMonitors (client.dll+0x191B6D):
//    eliminates ~1,450+ RTTI hierarchy walks & _strcmp calls per frame on non-camera entities.
// 3. CBaseEntity::GetFlags / AddFlag / RemoveFlag / Relink (vampire.dll): removes shipped debug
//    g_ScopeTraceStack push/pop around trivial flag ops and no-op Relink.
// 4. O(log N) binary search for RichText::GetCursorLine (GameUI.dll+0x29250): replaces O(N) linear
//    scan over m_LineBreaks that caused O(N^2) console scrollback stalls (3.7% normal, up to 89%
//    CPU / 9 FPS under heavy console output).
// 5. Fast SSE2 Plat_FloatTime (tier0.dll+0x38D0): eliminates per-call InitTime(), x87 fild, x87
//    fdiv, and store-forwarding stalls by multiplying (QPC - g_ClockStart) by precomputed 1/freq.

#include <emmintrin.h>
#include <cstdint>
#include <cstring>

namespace perf_runtime {

// Vectorized 4-wide SSE2 sincos for float32 angles in radians.
// Uses Cody-Waite range reduction by pi/4 and Cephes minimax polynomials (max error < 1.5e-7).
// Altered version of sincos_ps from sse_mathfun.h (Copyright (C) 2007 Julien Pommier, zlib license,
// based on the Cephes math library): see NOTICE.
static __forceinline void sincos_ps(__m128 x, __m128& out_sin, __m128& out_cos) {
    const __m128 sign_mask = _mm_set1_ps(-0.0f);
    const __m128 inv_sign_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    const __m128 four_over_pi = _mm_set1_ps(1.27323954473516f); // 4/pi

    // Extract sign of x and work with |x|
    __m128 sign_bit_sin = _mm_and_ps(x, sign_mask);
    x = _mm_and_ps(x, inv_sign_mask);

    // j = (int)(|x| * 4/pi), rounded up to even octant (j = (j + 1) & ~1)
    __m128 y = _mm_mul_ps(x, four_over_pi);
    __m128i emm2 = _mm_cvttps_epi32(y);
    emm2 = _mm_add_epi32(emm2, _mm_set1_epi32(1));
    emm2 = _mm_and_si128(emm2, _mm_set1_epi32(~1));
    y = _mm_cvtepi32_ps(emm2);

    // Quadrant sign & poly selection
    __m128i emm4 = emm2;
    // swap sign bit for sin if (j & 4) != 0
    __m128i emm0 = _mm_and_si128(emm2, _mm_set1_epi32(4));
    emm0 = _mm_slli_epi32(emm0, 29);
    __m128 swap_sign_sin = _mm_castsi128_ps(emm0);

    // poly mask: 0xFFFFFFFF where (j & 2) == 0 -> use sin poly for sin, cos poly for cos
    emm2 = _mm_and_si128(emm2, _mm_set1_epi32(2));
    emm2 = _mm_cmpeq_epi32(emm2, _mm_setzero_si128());
    __m128 poly_mask = _mm_castsi128_ps(emm2);

    // cos sign bit: negative when ((j - 2) & 4) == 0
    emm4 = _mm_sub_epi32(emm4, _mm_set1_epi32(2));
    emm4 = _mm_andnot_si128(emm4, _mm_set1_epi32(4));
    emm4 = _mm_slli_epi32(emm4, 29);
    __m128 sign_bit_cos = _mm_castsi128_ps(emm4);

    sign_bit_sin = _mm_xor_ps(sign_bit_sin, swap_sign_sin);

    // Extended-precision Cody-Waite reduction: x = ((x - y*DP1) - y*DP2) - y*DP3
    const __m128 dp1 = _mm_set1_ps(-0.78515625f);
    const __m128 dp2 = _mm_set1_ps(-2.4187564849853515625e-4f);
    const __m128 dp3 = _mm_set1_ps(-3.77489497744594108e-8f);
    x = _mm_add_ps(x, _mm_mul_ps(y, dp1));
    x = _mm_add_ps(x, _mm_mul_ps(y, dp2));
    x = _mm_add_ps(x, _mm_mul_ps(y, dp3));

    __m128 z = _mm_mul_ps(x, x);

    // Cosine polynomial on [-pi/4, pi/4]: 1 - 0.5*z + z^2*(c0 + z*(c1 + z*c2))
    __m128 yc = _mm_set1_ps(2.443315711809948e-5f);
    yc = _mm_add_ps(_mm_mul_ps(yc, z), _mm_set1_ps(-1.388731625493765e-3f));
    yc = _mm_add_ps(_mm_mul_ps(yc, z), _mm_set1_ps(4.166664568298827e-2f));
    yc = _mm_mul_ps(yc, _mm_mul_ps(z, z));
    yc = _mm_sub_ps(yc, _mm_mul_ps(z, _mm_set1_ps(0.5f)));
    yc = _mm_add_ps(yc, _mm_set1_ps(1.0f));

    // Sine polynomial on [-pi/4, pi/4]: x + x*z*(s0 + z*(s1 + z*s2))
    __m128 ys = _mm_set1_ps(-1.9515295891e-4f);
    ys = _mm_add_ps(_mm_mul_ps(ys, z), _mm_set1_ps(8.3321608736e-3f));
    ys = _mm_add_ps(_mm_mul_ps(ys, z), _mm_set1_ps(-1.6666654611e-1f));
    ys = _mm_add_ps(_mm_mul_ps(_mm_mul_ps(ys, z), x), x);

    // Select polynomials by octant & apply signs
    __m128 s_res = _mm_or_ps(_mm_and_ps(poly_mask, ys), _mm_andnot_ps(poly_mask, yc));
    __m128 c_res = _mm_or_ps(_mm_and_ps(poly_mask, yc), _mm_andnot_ps(poly_mask, ys));
    out_sin = _mm_xor_ps(s_res, sign_bit_sin);
    out_cos = _mm_xor_ps(c_res, sign_bit_cos);
}

inline void __cdecl fast_angle_matrix(const float* angles, float* matrix) {
    const float kDeg2Rad = 0.017453292519943295f;
    alignas(16) float sv[4], cv[4];
    __m128 rad = _mm_mul_ps(_mm_setr_ps(angles[0], angles[1], angles[2], 0.0f), _mm_set1_ps(kDeg2Rad));
    __m128 s, c;
    sincos_ps(rad, s, c);
    _mm_store_ps(sv, s);
    _mm_store_ps(cv, c);
    const float sp = sv[0], sy = sv[1], sr = sv[2];
    const float cp = cv[0], cy = cv[1], cr = cv[2];

    matrix[0] = cp * cy;
    matrix[4] = cp * sy;
    matrix[8] = -sp;

    const float crcy = cr * cy, crsy = cr * sy, srcy = sr * cy, srsy = sr * sy;
    matrix[1] = sp * srcy - crsy;
    matrix[5] = sp * srsy + crcy;
    matrix[9] = sr * cp;

    matrix[2] = sp * crcy + srsy;
    matrix[6] = sp * crsy - srcy;
    matrix[10] = cr * cp;

    matrix[3] = 0.0f;
    matrix[7] = 0.0f;
    matrix[11] = 0.0f;
}

inline void __cdecl fast_angle_vectors(const float* angles, float* forward, float* right, float* up) {
    const float kDeg2Rad = 0.017453292519943295f;
    alignas(16) float sv[4], cv[4];
    __m128 rad = _mm_mul_ps(_mm_setr_ps(angles[0], angles[1], angles[2], 0.0f), _mm_set1_ps(kDeg2Rad));
    __m128 s, c;
    sincos_ps(rad, s, c);
    _mm_store_ps(sv, s);
    _mm_store_ps(cv, c);
    const float sp = sv[0], sy = sv[1], sr = sv[2];
    const float cp = cv[0], cy = cv[1], cr = cv[2];

    if (forward) {
        forward[0] = cp * cy;
        forward[1] = cp * sy;
        forward[2] = -sp;
    }
    if (right) {
        right[0] = -1.0f * sr * sp * cy + -1.0f * cr * -sy;
        right[1] = -1.0f * sr * sp * sy + -1.0f * cr * cy;
        right[2] = -1.0f * sr * cp;
    }
    if (up) {
        up[0] = cr * sp * cy + -sr * -sy;
        up[1] = cr * sp * sy + -sr * cy;
        up[2] = cr * cp;
    }
}

// --- Vtable-cached __RTDynamicCast for CViewRender::DrawHighEndMonitors ---
using RtDynamicCastFn = void* (__cdecl*)(void*, int, void*, void*, int);
inline RtDynamicCastFn orig_monitor_cast = nullptr;

struct MonitorCastCache {
    static constexpr std::size_t kCap = 256; // power of 2; VTMB has ~50-80 client entity classes
    static constexpr std::int32_t kMiss = static_cast<std::int32_t>(0x80000000u);
    std::uintptr_t vtable[kCap] = {};
    std::int32_t delta[kCap] = {};

    void clear() {
        std::memset(vtable, 0, sizeof(vtable));
    }

    static __forceinline std::size_t hash_vt(std::uintptr_t vt) {
        return ((vt >> 4) ^ (vt >> 12)) & (kCap - 1);
    }

    void* lookup_or_cast(void* obj, int vfdelta, void* src_type, void* dst_type, int is_ref, RtDynamicCastFn real_cast) {
        if (!obj) return nullptr;
        const std::uintptr_t vt = *reinterpret_cast<const std::uintptr_t*>(obj);
        if (!vt) return real_cast(obj, vfdelta, src_type, dst_type, is_ref);
        std::size_t idx = hash_vt(vt);
        for (std::size_t probe = 0; probe < 16; ++probe) {
            const std::uintptr_t cur = vtable[idx];
            if (cur == vt) {
                const std::int32_t d = delta[idx];
                return d == kMiss ? nullptr : reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(obj) + d);
            }
            if (cur == 0) {
                void* res = real_cast(obj, vfdelta, src_type, dst_type, is_ref);
                vtable[idx] = vt;
                delta[idx] = res ? static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(res) - reinterpret_cast<std::intptr_t>(obj)) : kMiss;
                return res;
            }
            idx = (idx + 1) & (kCap - 1);
        }
        return real_cast(obj, vfdelta, src_type, dst_type, is_ref);
    }
};

inline MonitorCastCache g_monitor_cache;

inline void* __cdecl cached_monitor_cast(void* obj, int vfdelta, void* src_type, void* dst_type, int is_ref) {
    return g_monitor_cache.lookup_or_cast(obj, vfdelta, src_type, dst_type, is_ref, orig_monitor_cast);
}

// --- O(log N) binary search for GameUI.dll RichText::GetCursorLine() ---
// Original at GameUI.dll+0x29250 scans m_LineBreaks[0..n) linearly on every console line/frame.
inline int fast_richtext_cursor_line_impl(int n, const int* arr, int cursor, unsigned char flag, int text_len) {
    int line = 0;
    if (n > 0 && arr) {
        int lo = 0, hi = n;
        while (lo < hi) {
            const int mid = lo + ((hi - lo) >> 1);
            if (cursor < arr[mid]) hi = mid;
            else lo = mid + 1;
        }
        line = lo;
    }
    if (flag != 0 && cursor != text_len) {
        --line;
    }
    return line;
}

inline int __fastcall fast_richtext_get_cursor_line(const unsigned char* self) {
    const int n = *reinterpret_cast<const int*>(self + 0x64);
    const int* arr = *reinterpret_cast<const int* const*>(self + 0x6C);
    const int cursor = *reinterpret_cast<const int*>(self + 0x70);
    const unsigned char flag = self[0x75];
    const int text_len = *reinterpret_cast<const int*>(self + 0x58);
    return fast_richtext_cursor_line_impl(n, arr, cursor, flag, text_len);
}

// --- Fast SSE2 Plat_FloatTime (tier0.dll+0x38D0) ---
// Original calls InitTime() + QPC + 2x x87 fild + x87 fdiv + 3 store-forwarding stalls per call.
inline std::int64_t g_clock_start = 0;
inline double g_inv_clock_freq = 0.0;
inline const std::uint32_t* g_clock_filter_flag = nullptr;
inline void* const* g_clock_filter_vt = nullptr;

inline double __cdecl fast_plat_float_time() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double t = static_cast<double>(now.QuadPart - g_clock_start) * g_inv_clock_freq;
    if (*g_clock_filter_flag == 0) return t;
    using FilterFn = double(__cdecl*)(double);
    auto* vt = *reinterpret_cast<FilterFn* const*>(g_clock_filter_vt);
    return vt[6](t);
}

inline bool write_bytes_protected(unsigned char* dst, const unsigned char* src, std::size_t n) {
    DWORD old_prot = 0;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old_prot)) return false;
    std::memcpy(dst, src, n);
    VirtualProtect(dst, n, old_prot, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

// --- Smooth C_BaseAnimating::GetAnimTimeInterval (client.dll+0x8F960) ---
// Vanilla bug: if m_flAnimTime == m_flOldAnimTime (dt <= 0), it returns constant 1.0f, freezing
// animation extrapolation for the full 100 ms AI think; if dt is tiny (< 0.05s, rapid double update),
// (curtime - anim_time)/dt slams into the 2.0f clamp in ~10 ms and freezes for the remaining ~90 ms,
// making NPCs briefly look like choppy low-refresh-rate / 8-bit stop-motion animation.
inline void* const* g_client_engine_ptr = nullptr;

inline float anim_time_interval_impl(float anim_time, float old_anim_time, float curtime) {
    if (!std::isfinite(anim_time) || !(anim_time > 0.0f)) return 1.0f;
    float dt = anim_time - old_anim_time;
    if (!(dt >= 0.05f && dt <= 0.25f)) dt = 0.1f;
    float factor = 1.0f + (curtime - anim_time) / dt;
    if (!std::isfinite(factor)) return 1.0f;
    if (factor > 2.0f) return 2.0f;
    if (factor < 0.0f) return 0.0f;
    return factor;
}

inline float __fastcall fast_get_anim_time_interval(const unsigned char* self) {
    const float anim_time = *reinterpret_cast<const float*>(self + 0x70);
    const float old_anim_time = *reinterpret_cast<const float*>(self + 0x6C);
    if (!(anim_time > 0.0f) || !g_client_engine_ptr) return 1.0f;
    void* engine = *g_client_engine_ptr;
    if (!engine) return 1.0f;
    using GetTimeFn = float(__thiscall*)(void*, int);
    auto* vt = *reinterpret_cast<GetTimeFn* const*>(engine);
    const float curtime = vt[0x9C / 4](engine, 0);
    return anim_time_interval_impl(anim_time, old_anim_time, curtime);
}

// --- Fast O(1) CCvar::FindVar (engine.dll+0x400E0) ---
// Vanilla linearly scans the 1,700+ node ConCommandBase linked list with __strcmpi on every lookup.
inline const unsigned char kAsciiLower[256] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
    0x40,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,
    0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x5B,0x5C,0x5D,0x5E,0x5F,
    0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,
    0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x7B,0x7C,0x7D,0x7E,0x7F,
    0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8A,0x8B,0x8C,0x8D,0x8E,0x8F,
    0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0x9B,0x9C,0x9D,0x9E,0x9F,
    0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF,
    0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,
    0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,
    0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,
    0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,
    0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF
};

inline int __cdecl fast_stricmp_lut(const char* a, const char* b) {
    const auto* pa = reinterpret_cast<const unsigned char*>(a);
    const auto* pb = reinterpret_cast<const unsigned char*>(b);
    for (;;) {
        unsigned char ca = *pa++;
        unsigned char cb = *pb++;
        if (ca != cb) {
            ca = kAsciiLower[ca];
            cb = kAsciiLower[cb];
            if (ca != cb) return ca < cb ? -1 : 1;
        }
        if (ca == 0) return 0;
    }
}

// vstdlib.dll's own comparison helpers return only 0 (equal) or -1 (any difference), never +1.
// Callers may have built ordered containers with that result before these replacements are
// installed, so it is kept exactly.
inline int __cdecl fast_q_strncasecmp(const char* a, const char* b, int n) {
    const auto* pa = reinterpret_cast<const unsigned char*>(a);
    const auto* pb = reinterpret_cast<const unsigned char*>(b);
    for (; n > 0; --n) {
        const unsigned char ca = *pa++;
        const unsigned char cb = *pb++;
        if (ca != cb && kAsciiLower[ca] != kAsciiLower[cb]) return -1;
        if (ca == 0) return 0;
    }
    return 0;
}

inline int __cdecl fast_q_strcasecmp(const char* a, const char* b) {
    return fast_q_strncasecmp(a, b, 99999); // the limit vstdlib.dll passes
}

inline char* __cdecl fast_strnlwr_lut(char* s, int /*count*/) {
    if (!s) return nullptr;
    auto* p = reinterpret_cast<unsigned char*>(s);
    while (*p) {
        *p = kAsciiLower[*p];
        ++p;
    }
    return s;
}

inline int __cdecl fast_q_strncmp(const char* a, const char* b, int count) {
    for (; count > 0; --count, ++a, ++b) {
        if (*a != *b) return -1;
        if (*a == '\0') return 0;
    }
    return 0;
}

struct CvarLookupCache {
    static constexpr std::size_t kCap = 4096; // power of 2; VTMB has ~1,716 cvars/cmds
    static constexpr std::size_t kMaxEntries = 3400;
    struct Slot {
        std::uint32_t hash;
        const char* name;
        unsigned char* node;
        unsigned char* previous; // the list node before `node` (nullptr: the head)
    };
    Slot slots[kCap] = {};
    void* cached_head = nullptr;
    bool overflowed = false;

    void clear() {
        std::memset(slots, 0, sizeof(slots));
        cached_head = nullptr;
        overflowed = false;
    }

    static __forceinline std::uint32_t hash_ci(const char* s) {
        std::uint32_t h = 2166136261u;
        for (const auto* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
            h ^= kAsciiLower[*p];
            h *= 16777619u;
        }
        return h ? h : 1u;
    }

    void rebuild(void* head) {
        std::memset(slots, 0, sizeof(slots));
        cached_head = head;
        overflowed = false;
        std::size_t count = 0;
        unsigned char* previous = nullptr;
        for (auto* cur = reinterpret_cast<unsigned char*>(head); cur; previous = cur, cur = *reinterpret_cast<unsigned char**>(cur + 8)) {
            auto* base = *reinterpret_cast<unsigned char**>(cur + 4);
            const char* name = base ? *reinterpret_cast<const char**>(base + 0x10) : nullptr;
            if (!name) continue;
            if (count >= kMaxEntries) {
                overflowed = true;
                return;
            }
            const std::uint32_t h = hash_ci(name);
            std::size_t idx = h & (kCap - 1);
            for (;;) {
                if (slots[idx].hash == 0) {
                    slots[idx] = {h, name, cur, previous};
                    ++count;
                    break;
                }
                if (slots[idx].hash == h && fast_stricmp_lut(slots[idx].name, name) == 0) {
                    // First match from head wins, matching linear scan semantics.
                    break;
                }
                idx = (idx + 1) & (kCap - 1);
            }
        }
    }

    // Still linked where it was when cached? CCvar::UnlinkVariables (engine.dll+0x40160) removes a
    // DLL's nodes and reverses the list, so after two calls (client, then server) the old head can be
    // back in place while cached nodes belong to an unloaded DLL. Every unlinked node loses its link,
    // and the node before it gets another one.
    static bool still_linked(const Slot& s, const void* head) {
        __try {
            return s.previous ? *reinterpret_cast<unsigned char* const*>(s.previous + 8) == s.node : s.node == head;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void* find(void* head, const char* name) {
        if (!head || !name) return nullptr;
        if (head != cached_head) rebuild(head);
        void* node = nullptr;
        if (lookup(head, name, node)) return node;
        rebuild(head); // the list changed behind the same head
        return lookup(head, name, node) ? node : nullptr;
    }

    // False when a cached entry that could match is stale; `node` is the result otherwise.
    bool lookup(void* head, const char* name, void*& node) {
        node = nullptr;
        if (overflowed) {
            for (auto* cur = reinterpret_cast<unsigned char*>(head); cur; cur = *reinterpret_cast<unsigned char**>(cur + 8)) {
                auto* base = *reinterpret_cast<unsigned char**>(cur + 4);
                const char* n = base ? *reinterpret_cast<const char**>(base + 0x10) : nullptr;
                if (n && fast_stricmp_lut(name, n) == 0) { node = cur; return true; }
            }
            return true;
        }
        const std::uint32_t h = hash_ci(name);
        std::size_t idx = h & (kCap - 1);
        for (;;) {
            const std::uint32_t sh = slots[idx].hash;
            if (sh == 0) return true;
            if (sh == h) {
                if (!still_linked(slots[idx], head)) return false; // before its name is read
                if (fast_stricmp_lut(slots[idx].name, name) == 0) { node = slots[idx].node; return true; }
            }
            idx = (idx + 1) & (kCap - 1);
        }
    }
};

inline CvarLookupCache g_cvar_cache;
inline void* const* g_cvar_list_head = nullptr;

inline void* __cdecl fast_cvar_find_var(const char* name) {
    if (!g_cvar_list_head) return nullptr;
    return g_cvar_cache.find(*g_cvar_list_head, name);
}

// --- Fast 64-bit CDeltaBitsWriter / CDeltaBitsReader (engine.dll+0x5F1E0, +0x5F360) ---
struct BfBuf {
    unsigned char* data; // +0x00
    int nDataBytes;      // +0x04
    int nDataBits;       // +0x08
    int iCurBit;         // +0x0C
    unsigned char bOverflow; // +0x10
};

struct DeltaBitsWriter {
    BfBuf* m_pBuf;   // +0x00
    int m_iLastProp; // +0x04
};

struct DeltaBitsReader {
    BfBuf* m_pBuf;           // +0x00
    unsigned char m_bFinished; // +0x04
    unsigned char pad[3];
    int m_iLastProp;         // +0x08
};

struct DeltaEncEntry {
    std::uint32_t code;
    std::uint8_t nbits;
};

struct DeltaEncTable {
    static constexpr int kMaxFastDelta = 120; // deltas 0..119 fit in <= 29 bits (incl. leading 1-bit)
    DeltaEncEntry entries[kMaxFastDelta];

    DeltaEncTable() {
        for (int d = 0; d < kMaxFastDelta; ++d) {
            std::uint32_t code = 1u; // leading 1 bit
            int pos = 1;
            int rem = d;
            int nbits = 1;
            for (;;) {
                const int maxv = (1 << nbits) - 1;
                const bool full = (rem >= maxv);
                const int chunk = full ? maxv : rem;
                code |= static_cast<std::uint32_t>(chunk) << pos;
                pos += nbits;
                rem -= chunk;
                ++nbits;
                if (rem == 0 && !full) break;
            }
            entries[d] = {code, static_cast<std::uint8_t>(pos)};
        }
    }
};

inline const DeltaEncTable g_delta_enc;

inline void bf_write_ubit_fallback(BfBuf* buf, std::uint32_t val, int nbits) {
    const int cur = buf->iCurBit;
    if (cur + nbits > buf->nDataBits) {
        buf->iCurBit = buf->nDataBits;
        buf->bOverflow = 1;
        return;
    }
    int bit = cur;
    for (int i = 0; i < nbits; ++i, ++bit) {
        unsigned char& byte = buf->data[bit >> 3];
        const unsigned char mask = static_cast<unsigned char>(1u << (bit & 7));
        if ((val >> i) & 1u) byte |= mask;
        else byte &= static_cast<unsigned char>(~mask);
    }
    buf->iCurBit = cur + nbits;
}

inline std::uint32_t bf_read_ubit_fallback(BfBuf* buf, int nbits) {
    const int cur = buf->iCurBit;
    if (cur + nbits > buf->nDataBits) {
        buf->iCurBit = buf->nDataBits;
        buf->bOverflow = 1;
        return 0;
    }
    std::uint32_t out = 0;
    int bit = cur;
    for (int i = 0; i < nbits; ++i, ++bit) {
        if ((buf->data[bit >> 3] >> (bit & 7)) & 1u) out |= (1u << i);
    }
    buf->iCurBit = cur + nbits;
    return out;
}

inline int __fastcall fast_delta_write_prop_index(DeltaBitsWriter* self, void*, int prop) {
    const int delta = (prop - self->m_iLastProp) - 1;
    self->m_iLastProp = prop;
    BfBuf* buf = self->m_pBuf;
    const int cur = buf->iCurBit;

    if (static_cast<unsigned int>(delta) < static_cast<unsigned int>(DeltaEncTable::kMaxFastDelta) &&
        !buf->bOverflow) {
        const DeltaEncEntry e = g_delta_enc.entries[delta];
        if (cur + e.nbits <= buf->nDataBits && (cur >> 3) + 8 <= buf->nDataBytes) {
            const int byte_off = cur >> 3;
            const int bit_off = cur & 7;
            std::uint64_t word = 0;
            std::memcpy(&word, buf->data + byte_off, 8);
            const std::uint64_t mask = ((1ULL << e.nbits) - 1ULL) << bit_off;
            word = (word & ~mask) | (static_cast<std::uint64_t>(e.code) << bit_off);
            std::memcpy(buf->data + byte_off, &word, 8);
            buf->iCurBit = cur + e.nbits;
            return e.nbits;
        }
    }

    // Exact vanilla fallback (large delta, near end of buffer, or overflow)
    if (cur + 1 > buf->nDataBits) {
        buf->bOverflow = 1;
    }
    if (!buf->bOverflow) {
        buf->data[cur >> 3] |= static_cast<unsigned char>(1u << (cur & 7));
        buf->iCurBit = cur + 1;
    }
    int total = 1;
    int rem = delta;
    int nbits = 1;
    for (;;) {
        const int maxv = (1 << (nbits & 31)) - 1;
        const bool full = (rem >= maxv);
        const int chunk = full ? maxv : rem;
        bf_write_ubit_fallback(buf, static_cast<std::uint32_t>(chunk), nbits);
        total += nbits;
        rem -= chunk;
        ++nbits;
        if (rem == 0 && !full) break;
    }
    return total;
}

inline std::uint32_t __fastcall fast_delta_read_next_prop_index(DeltaBitsReader* self, void*, int* out_prop) {
    BfBuf* buf = self->m_pBuf;
    const int cur = buf->iCurBit;

    if (!buf->bOverflow && (cur >> 3) + 8 <= buf->nDataBytes && cur + 36 <= buf->nDataBits) {
        std::uint64_t w = 0;
        std::memcpy(&w, buf->data + (cur >> 3), 8);
        w >>= (cur & 7);
        if ((w & 1ULL) == 0ULL) {
            buf->iCurBit = cur + 1;
            self->m_bFinished = 1;
            return 0;
        }
        // Ultra-hot path: delta == 0 (consecutive prop, bit0=1, bit1=0)
        if ((w & 3ULL) == 1ULL) {
            buf->iCurBit = cur + 2;
            const int p = self->m_iLastProp + 1;
            self->m_iLastProp = p;
            *out_prop = p;
            return 1;
        }
        // Register-only decode for nbits 1..7 (covers deltas 1..246, <= 29 bits total)
        std::uint64_t r = w >> 1;
        int prop = self->m_iLastProp + 1;
        int consumed = 1;
        for (int nbits = 1; nbits <= 7; ++nbits) {
            const std::uint32_t mask = (1u << nbits) - 1u;
            const std::uint32_t v = static_cast<std::uint32_t>(r) & mask;
            r >>= nbits;
            consumed += nbits;
            prop += static_cast<int>(v);
            if (v != mask) {
                buf->iCurBit = cur + consumed;
                self->m_iLastProp = prop;
                *out_prop = prop;
                return 1;
            }
        }
        // Rare delta >= 247: continue from cur + consumed via exact fallback
        buf->iCurBit = cur + consumed;
        for (int nbits = 8;; ++nbits) {
            const std::uint32_t mask = (1u << (nbits & 31)) - 1u;
            const std::uint32_t v = bf_read_ubit_fallback(buf, nbits);
            prop += static_cast<int>(v);
            if (v != mask) break;
        }
        self->m_iLastProp = prop;
        *out_prop = prop;
        return 1;
    }

    // Exact vanilla fallback near end of buffer or already overflowed
    if (cur + 1 > buf->nDataBits) {
        buf->bOverflow = 1;
    }
    if (!buf->bOverflow) {
        const unsigned char b = buf->data[cur >> 3];
        buf->iCurBit = cur + 1;
        if ((b >> (cur & 7)) & 1u) {
            int prop = self->m_iLastProp + 1;
            for (int nbits = 1;; ++nbits) {
                const std::uint32_t mask = (1u << (nbits & 31)) - 1u;
                const std::uint32_t v = bf_read_ubit_fallback(buf, nbits);
                prop += static_cast<int>(v);
                if (v != mask) break;
            }
            self->m_iLastProp = prop;
            *out_prop = prop;
            return 1;
        }
    }
    self->m_bFinished = 1;
    return 0;
}

// --- Fast 64-bit SendProp Float_Decode / Int_Compare / Float_Compare ---
// Replaces x87 fdiv + 4 stack store-forwarding stalls in Float_Decode (engine.dll+0x60820) and
// word-boundary bit loops in Int_Compare (+0x60500) & Float_Compare (+0x609E0) with 64-bit loads + LUT.
struct DecodeInfo {
    void* m_pRecvProp;      // +0x00 (m_ProxyFn at +0x1C)
    float m_FloatVal;       // +0x04
    std::uint32_t pad[5];   // +0x08..+0x1B
    void* m_pStruct;        // +0x1C
    void* m_pOut;           // +0x20
    const unsigned char* m_pProp; // +0x24 (nBits +0x0C, fLow +0x14, fHigh +0x18, flags +0x3C)
    BfBuf* m_pIn;           // +0x28
};

using ReadBitCoordFn = float(__thiscall*)(BfBuf*);
inline ReadBitCoordFn g_read_bit_coord = nullptr;  // engine.dll+0x6FC0
inline ReadBitCoordFn g_read_bit_normal = nullptr; // engine.dll+0x75B0

struct InvMaxUIntTable {
    double inv[33];
    InvMaxUIntTable() {
        inv[0] = 0.0;
        for (int b = 1; b <= 32; ++b) {
            const std::uint64_t denom = (1ULL << b) - 1ULL;
            inv[b] = 1.0 / static_cast<double>(denom);
        }
    }
};
inline const InvMaxUIntTable g_inv_max_uint;

inline std::uint32_t __forceinline bf_read_ubit_fast(BfBuf* buf, int nbits) {
    const int cur = buf->iCurBit;
    const int next = cur + nbits;
    if (next > buf->nDataBits) {
        buf->iCurBit = buf->nDataBits;
        buf->bOverflow = 1;
        return 0;
    }
    if ((cur >> 3) + 8 <= buf->nDataBytes) {
        std::uint64_t w = 0;
        std::memcpy(&w, buf->data + (cur >> 3), 8);
        w >>= (cur & 7);
        buf->iCurBit = next;
        const std::uint32_t mask = (nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1u);
        return static_cast<std::uint32_t>(w) & mask;
    }
    return bf_read_ubit_fallback(buf, nbits);
}

inline void __cdecl fast_float_decode(DecodeInfo* info) {
    const unsigned char* prop = info->m_pProp;
    const std::uint32_t flags = *reinterpret_cast<const std::uint32_t*>(prop + 0x3C);
    BfBuf* in = info->m_pIn;
    float val = 0.0f;
    if (flags & 2u) {
        val = g_read_bit_coord(in);
    } else if (flags & 4u) {
        const std::uint32_t raw = bf_read_ubit_fast(in, 32);
        std::memcpy(&val, &raw, 4);
    } else if (flags & 0x20u) {
        val = g_read_bit_normal(in);
    } else {
        const int nbits = *reinterpret_cast<const int*>(prop + 0x0C);
        const std::uint32_t raw = bf_read_ubit_fast(in, nbits);
        const float lo = *reinterpret_cast<const float*>(prop + 0x14);
        const float hi = *reinterpret_cast<const float*>(prop + 0x18);
        const int b = (nbits >= 1 && nbits <= 32) ? nbits : 1;
        const float t = static_cast<float>(static_cast<double>(raw) * g_inv_max_uint.inv[b]);
        val = lo + (hi - lo) * t;
    }
    info->m_FloatVal = val;
    if (info->m_pRecvProp) {
        using ProxyFn = void(__cdecl*)(DecodeInfo*, void*, void*);
        auto fn = *reinterpret_cast<ProxyFn*>(reinterpret_cast<unsigned char*>(info->m_pRecvProp) + 0x1C);
        fn(info, info->m_pStruct, info->m_pOut);
    }
}

inline int __cdecl fast_int_compare(const unsigned char* prop, BfBuf* b1, BfBuf* b2) {
    const int nbits = *reinterpret_cast<const int*>(prop + 0x0C);
    const std::uint32_t v1 = bf_read_ubit_fast(b1, nbits);
    const std::uint32_t v2 = bf_read_ubit_fast(b2, nbits);
    return v1 != v2 ? 1 : 0;
}

inline int __cdecl fast_float_compare(const unsigned char* prop, BfBuf* b1, BfBuf* b2) {
    const std::uint32_t flags = *reinterpret_cast<const std::uint32_t*>(prop + 0x3C);
    if (flags & 2u) {
        const float f1 = g_read_bit_coord(b1);
        const float f2 = g_read_bit_coord(b2);
        return !(f1 == f2) ? 1 : 0;
    }
    if (flags & 4u) {
        const std::uint32_t v1 = bf_read_ubit_fast(b1, 32);
        const std::uint32_t v2 = bf_read_ubit_fast(b2, 32);
        return v1 != v2 ? 1 : 0;
    }
    if (flags & 0x20u) {
        // As the native code: the 12 raw bits (sign and 11-bit fraction), not the decoded values.
        const std::uint32_t v1 = bf_read_ubit_fast(b1, 12);
        const std::uint32_t v2 = bf_read_ubit_fast(b2, 12);
        return v1 != v2 ? 1 : 0;
    }
    const int nbits = *reinterpret_cast<const int*>(prop + 0x0C);
    const std::uint32_t v1 = bf_read_ubit_fast(b1, nbits);
    const std::uint32_t v2 = bf_read_ubit_fast(b2, nbits);
    return v1 != v2 ? 1 : 0;
}

// Only caches are dropped. The pointers the replacements read stay: if a module was not reloaded its
// replacements are still in place and need them, and otherwise the next install sets them again.
inline void reset_on_restart() {
    g_monitor_cache.clear();
    g_cvar_cache.clear();
}

inline bool module_range_ok(HMODULE mod, std::size_t rva, std::size_t size) {
    if (!mod) return false;
    __try {
        const auto base = reinterpret_cast<const unsigned char*>(mod);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x1000) return false;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
        const std::size_t image_size = nt->OptionalHeader.SizeOfImage;
        return rva <= image_size && size <= image_size - rva;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool u32_at_equals(const unsigned char* at, std::uintptr_t expected) {
    std::uint32_t value = 0;
    std::memcpy(&value, at, 4);
    return value == static_cast<std::uint32_t>(expected);
}

inline void install_common() {
    // Fast SSE2 Plat_FloatTime (tier0.dll+0x38D0)
    if (enabled("fast_timer", "1") && perf_math::detect_cpu().sse2) {
        HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
        if (tier0 && module_range_ok(tier0, 0x38D0, 0x7B) && module_range_ok(tier0, 0x2B3F0, 4)) {
            auto* base = reinterpret_cast<unsigned char*>(tier0);
            unsigned char* fn = base + 0x38D0;
            const auto b = reinterpret_cast<std::uintptr_t>(base);
            static const unsigned char kPlatFloatTimeHead[16] = {
                0x83, 0xEC, 0x18, 0xE8, 0x98, 0x00, 0x00, 0x00,
                0x8D, 0x44, 0x24, 0x08, 0x50, 0xFF, 0x15, 0xD4
            };
            const bool operands_ok =
                std::memcmp(fn, kPlatFloatTimeHead, sizeof(kPlatFloatTimeHead)) == 0 &&
                fn[0x13] == 0x8B && fn[0x14] == 0x15 && u32_at_equals(fn + 0x15, b + 0x296E0) &&
                fn[0x36] == 0xA1 && u32_at_equals(fn + 0x37, b + 0x2B3F0) &&
                fn[0x41] == 0xDF && fn[0x42] == 0x2D && u32_at_equals(fn + 0x43, b + 0x296D8) &&
                fn[0x69] == 0x8B && fn[0x6A] == 0x15 && u32_at_equals(fn + 0x6B, b + 0x21EB4);
            if (operands_ok) {
                // Invoke once so tier0 InitTime() initializes g_PerformanceFrequency and g_ClockStart
                using FloatTimeFn = double(__cdecl*)();
                (void)reinterpret_cast<FloatTimeFn>(fn)();
                const std::int64_t freq = *reinterpret_cast<const std::int64_t*>(base + 0x296D8);
                const std::int64_t start = *reinterpret_cast<const std::int64_t*>(base + 0x296E0);
                if (freq > 0) {
                    g_clock_start = start;
                    g_inv_clock_freq = 1.0 / static_cast<double>(freq);
                    g_clock_filter_flag = reinterpret_cast<const std::uint32_t*>(base + 0x2B3F0);
                    g_clock_filter_vt = reinterpret_cast<void* const*>(base + 0x21EB4);
                    if (perf_math::write_jmp5(fn, reinterpret_cast<void*>(fast_plat_float_time))) {
                        host->log("Fast SSE2 Plat_FloatTime enabled (tier0.dll+0x38D0).");
                    }
                }
            }
        }
    }

    // Fast O(1) CCvar::FindVar (engine.dll+0x400E0)
    HMODULE engine = GetModuleHandleW(L"engine.dll");
    if (engine && !restart_startup::engine_known(engine)) {
        host->log("Fast CVar lookup and network codec disabled: unknown engine.dll build.");
        engine = nullptr;
    }
    if (engine && enabled("fast_cvar_lookup", "1") && module_range_ok(engine, 0x400E0, 24) &&
        module_range_ok(engine, 0xA3F45C, 4)) {
        auto* base = reinterpret_cast<unsigned char*>(engine);
        unsigned char* fn = base + 0x400E0;
        const auto b = reinterpret_cast<std::uintptr_t>(base);
        static const unsigned char kFindVarTail[17] = {
            0x85, 0xF6, 0x57, 0x74, 0x1F, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0x46, 0x04, 0x8B, 0x40, 0x10, 0x50, 0x57
        };
        if (fn[0] == 0x56 && fn[1] == 0x8B && fn[2] == 0x35 &&
            u32_at_equals(fn + 3, b + 0xA3F45C) &&
            std::memcmp(fn + 7, kFindVarTail, sizeof(kFindVarTail)) == 0) {
            g_cvar_list_head = reinterpret_cast<void* const*>(base + 0xA3F45C);
            g_cvar_cache.clear();
            if (perf_math::write_jmp5(fn, reinterpret_cast<void*>(fast_cvar_find_var))) {
                host->log("Fast O(1) CCvar::FindVar hash cache enabled (engine.dll+0x400E0).");
            }
        }
    }

    // Fast 64-bit CDeltaBitsWriter / CDeltaBitsReader (engine.dll+0x5F1E0, +0x5F360)
    if (engine && enabled("fast_delta_bits", "1") && module_range_ok(engine, 0x5F1E0, 0x200)) {
        auto* base = reinterpret_cast<unsigned char*>(engine);
        static const unsigned char kWritePropHead[20] = {
            0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x18, 0x53, 0x55, 0x56,
            0x8B, 0xF1, 0x8B, 0x6E, 0x04, 0x57, 0x8B, 0xF8, 0x89, 0x46
        };
        static const unsigned char kReadPropHead[20] = {
            0x83, 0xEC, 0x08, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x2E, 0x8B,
            0x45, 0x0C, 0x8B, 0x4D, 0x08, 0x8D, 0x50, 0x01, 0x3B, 0xD1
        };
        int hooked = 0;
        if (std::memcmp(base + 0x5F1E0, kWritePropHead, sizeof(kWritePropHead)) == 0 &&
            perf_math::write_jmp5(base + 0x5F1E0, reinterpret_cast<void*>(fast_delta_write_prop_index)))
            ++hooked;
        if (std::memcmp(base + 0x5F360, kReadPropHead, sizeof(kReadPropHead)) == 0 &&
            perf_math::write_jmp5(base + 0x5F360, reinterpret_cast<void*>(fast_delta_read_next_prop_index)))
            ++hooked;
        if (hooked > 0) {
            char msg[128];
            sprintf_s(msg, "Fast 64-bit DeltaBits prop codec enabled (%d/2 patched in engine.dll).", hooked);
            host->log(msg);
        }
    }

    // Fast 64-bit SendProp Float_Decode / Int_Compare / Float_Compare (engine.dll+0x60820, +0x60500, +0x609E0)
    if (engine && enabled("fast_prop_codec", "1") && module_range_ok(engine, 0x60500, 0x600)) {
        auto* base = reinterpret_cast<unsigned char*>(engine);
        static const unsigned char kIntCompareHead[20] = {
            0x8B, 0x44, 0x24, 0x04, 0x53, 0x55, 0x8B, 0x68, 0x0C, 0x8B,
            0x44, 0x24, 0x10, 0x8B, 0x48, 0x0C, 0x56, 0x8B, 0x70, 0x08
        };
        static const unsigned char kFloatDecodeHead[20] = {
            0x83, 0xEC, 0x08, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x8B, 0x4F,
            0x24, 0x8B, 0x51, 0x3C, 0xF6, 0xC2, 0x02, 0x8B, 0x47, 0x28
        };
        static const unsigned char kFloatCompareHead[20] = {
            0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x8B, 0x46, 0x3C,
            0xA8, 0x02, 0x57, 0x74, 0x32, 0x8B, 0x4C, 0x24, 0x18, 0xE8
        };
        // The two native readers the replacements call.
        static const unsigned char kReadBitCoordHead[16] = {0x83, 0xEC, 0x14, 0x53, 0x55, 0x8B, 0xC1, 0x8B, 0x50, 0x0C, 0x56, 0x57, 0x8B, 0x78, 0x08, 0x33};
        static const unsigned char kReadBitNormalHead[16] = {0x83, 0xEC, 0x08, 0x53, 0x55, 0x8B, 0xC1, 0x8B, 0x50, 0x0C, 0x56, 0x8B, 0x70, 0x08, 0x57, 0x8D};
        const bool readers_ok = std::memcmp(base + 0x6FC0, kReadBitCoordHead, sizeof(kReadBitCoordHead)) == 0 &&
                                std::memcmp(base + 0x75B0, kReadBitNormalHead, sizeof(kReadBitNormalHead)) == 0;
        if (readers_ok) {
            g_read_bit_coord = reinterpret_cast<ReadBitCoordFn>(base + 0x6FC0);
            g_read_bit_normal = reinterpret_cast<ReadBitCoordFn>(base + 0x75B0);
        } else {
            host->log("Fast SendProp Float_Decode/Float_Compare disabled: native bit readers differ.");
        }
        int hooked = 0;
        if (readers_ok && std::memcmp(base + 0x60820, kFloatDecodeHead, sizeof(kFloatDecodeHead)) == 0 &&
            perf_math::write_jmp5(base + 0x60820, reinterpret_cast<void*>(fast_float_decode)))
            ++hooked;
        if (std::memcmp(base + 0x60500, kIntCompareHead, sizeof(kIntCompareHead)) == 0 &&
            perf_math::write_jmp5(base + 0x60500, reinterpret_cast<void*>(fast_int_compare)))
            ++hooked;
        if (readers_ok && std::memcmp(base + 0x609E0, kFloatCompareHead, sizeof(kFloatCompareHead)) == 0 &&
            perf_math::write_jmp5(base + 0x609E0, reinterpret_cast<void*>(fast_float_compare)))
            ++hooked;
        if (hooked > 0) {
            char msg[128];
            sprintf_s(msg, "Fast 64-bit SendProp Float_Decode/Compare enabled (%d/3 patched in engine.dll).", hooked);
            host->log(msg);
        }
    }

    // Fast zero-assert vstdlib.dll Q_str* helpers. The debug _Q_* exports take (file, line, ...) first
    // and no shipped game module imports them, so they stay native.
    if (enabled("fast_vstdlib", "1")) {
        HMODULE vstd = GetModuleHandleW(L"vstdlib.dll");
        if (vstd) {
            struct VstdHook {
                const char* name;
                unsigned char prefix[6];
                std::size_t prefix_len;
                void* target;
            };
            static const VstdHook kVstdHooks[] = {
                {"Q_strncmp",     {0x53, 0x8B, 0x1D, 0x00, 0x00, 0x00}, 3, reinterpret_cast<void*>(fast_q_strncmp)},
                {"Q_strnlwr",     {0x8B, 0x44, 0x24, 0x08, 0x56, 0x8B}, 6, reinterpret_cast<void*>(fast_strnlwr_lut)},
                {"Q_strncasecmp", {0x53, 0x56, 0x8B, 0x35, 0x00, 0x00}, 4, reinterpret_cast<void*>(fast_q_strncasecmp)},
                {"Q_strcasecmp",  {0x53, 0x56, 0x8B, 0x74, 0x24, 0x0C}, 6, reinterpret_cast<void*>(fast_q_strcasecmp)},
                {"Q_strnicmp",    {0x53, 0x56, 0x8B, 0x74, 0x24, 0x0C}, 6, reinterpret_cast<void*>(fast_q_strncasecmp)},
            };
            int patched = 0;
            for (const auto& h : kVstdHooks) {
                auto* fn = reinterpret_cast<unsigned char*>(GetProcAddress(vstd, h.name));
                if (fn && std::memcmp(fn, h.prefix, h.prefix_len) == 0 && perf_math::write_jmp5(fn, h.target)) {
                    ++patched;
                }
            }
            if (patched > 0) {
                char msg[128];
                sprintf_s(msg, "Fast zero-assert vstdlib string helpers enabled (%d/%d patched).", patched, static_cast<int>(_countof(kVstdHooks)));
                host->log(msg);
            }
        }
    }
}

inline void install_screen(HMODULE client_mod) {
    // O(log N) RichText::GetCursorLine (GameUI.dll+0x29250) — independent of client.dll build check
    if (enabled("fast_richtext", "1")) {
        HMODULE gameui = GetModuleHandleW(L"GameUI.dll");
        if (gameui && module_range_ok(gameui, 0x29250, 63)) {
            unsigned char* fn = reinterpret_cast<unsigned char*>(gameui) + 0x29250;
            static const unsigned char kGetCursorLine[63] = {
                0x56, 0x8B, 0x71, 0x64, 0x33, 0xC0, 0x57, 0x85, 0xF6, 0x7E, 0x1F, 0x8B, 0x79, 0x70, 0x85, 0xC0,
                0x7C, 0x0D, 0x3B, 0x41, 0x64, 0x7D, 0x08, 0x8B, 0x51, 0x6C, 0x8B, 0x14, 0x82, 0xEB, 0x02, 0x33,
                0xD2, 0x3B, 0xFA, 0x7C, 0x05, 0x40, 0x3B, 0xC6, 0x7C, 0xE4, 0x8A, 0x51, 0x75, 0x84, 0xD2, 0x74,
                0x0B, 0x8B, 0x51, 0x70, 0x8B, 0x71, 0x58, 0x3B, 0xD6, 0x74, 0x01, 0x48, 0x5F, 0x5E, 0xC3
            };
            if (std::memcmp(fn, kGetCursorLine, sizeof(kGetCursorLine)) == 0 &&
                perf_math::write_jmp5(fn, reinterpret_cast<void*>(fast_richtext_get_cursor_line))) {
                host->log("Fast O(log N) RichText::GetCursorLine enabled (GameUI.dll+0x29250).");
            }
        }
    }

    if (!client_mod || !supported(client_mod)) return;
    const auto base = reinterpret_cast<unsigned char*>(client_mod);

    // 1. Fast SSE2 AngleMatrix & AngleVectors
    if (enabled("fast_angle_math", "1") && perf_math::detect_cpu().sse2) {
        static const unsigned char kAngleHead[28] = {
            0x83, 0xEC, 0x20, 0x8D, 0x4C, 0x24, 0x24, 0x8D, 0x44, 0x24, 0x00, 0x89, 0x4C, 0x24,
            0x14, 0x8B, 0x4C, 0x24, 0x24, 0x89, 0x44, 0x24, 0x18, 0xD9, 0x41, 0x04, 0xD8, 0x0D
        };
        int hooked = 0;
        if (std::memcmp(base + 0x1094E0, kAngleHead, sizeof(kAngleHead)) == 0 &&
            perf_math::write_jmp5(base + 0x1094E0, reinterpret_cast<void*>(fast_angle_matrix)))
            ++hooked;
        if (std::memcmp(base + 0x1090A0, kAngleHead, sizeof(kAngleHead)) == 0 &&
            perf_math::write_jmp5(base + 0x1090A0, reinterpret_cast<void*>(fast_angle_vectors)))
            ++hooked;
        if (hooked > 0) {
            char msg[128];
            sprintf_s(msg, "Fast SSE2 AngleMatrix/AngleVectors enabled (%d/2 patched in client.dll).", hooked);
            host->log(msg);
        }
    }

    // 2. Vtable-cached __RTDynamicCast in CViewRender::DrawHighEndMonitors (client.dll+0x191B6D)
    if (enabled("fast_monitor_cast", "1")) {
        // 10191B60: 53 68 <dst> 68 <src> 53 50 E8 <rel32> 8B F8 83 C4 14
        unsigned char* site = base + 0x191B60;
        if (site[0] == 0x53 && site[1] == 0x68 && site[6] == 0x68 &&
            site[11] == 0x53 && site[12] == 0x50 && site[13] == 0xE8 &&
            site[18] == 0x8B && site[19] == 0xF8 && site[20] == 0x83 && site[21] == 0xC4 && site[22] == 0x14) {
            void* target = nullptr;
            if (host->hook_call(host->context, site + 13, reinterpret_cast<void*>(cached_monitor_cast), &target) == BLSE_HOOK_OK) {
                orig_monitor_cast = reinterpret_cast<RtDynamicCastFn>(target);
                g_monitor_cache.clear();
                host->log("Fast monitor RTTI cache enabled (client.dll+0x191B6D).");
            }
        }
    }

    // 3. Smooth C_BaseAnimating::GetAnimTimeInterval (client.dll+0x8F960)
    if (enabled("npc_anim_time_fix", "1")) {
        static const unsigned char kAnimTimeHead[16] = {
            0x51, 0x56, 0x8B, 0xF1, 0xD9, 0x46, 0x70, 0xD8, 0x66, 0x6C, 0xD9, 0x5C, 0x24, 0x04, 0xD9, 0x05
        };
        if (std::memcmp(base + 0x8F960, kAnimTimeHead, sizeof(kAnimTimeHead)) == 0 &&
            base[0x8F987] == 0x8B && base[0x8F988] == 0x0D &&
            u32_at_equals(base + 0x8F989, reinterpret_cast<std::uintptr_t>(base) + 0x4A57D4)) {
            g_client_engine_ptr = reinterpret_cast<void* const*>(base + 0x4A57D4);
            if (perf_math::write_jmp5(base + 0x8F960, reinterpret_cast<void*>(fast_get_anim_time_interval))) {
                host->log("NPC animation timing continuity fix enabled (client.dll+0x8F960).");
            }
        }
    }
}

inline void install_world() {
    if (!server_profile_supported()) return;
    HMODULE vamp = GetModuleHandleW(L"vampire.dll");
    if (!vamp) return;
    const auto base = reinterpret_cast<unsigned char*>(vamp);

    // 3. Strip shipped debug g_ScopeTraceStack from hot trivial CBaseEntity ops in vampire.dll
    if (enabled("fast_entity_flags", "1")) {
        int patched = 0;
        constexpr std::uintptr_t kFlagsOffset = 0x434; // m_fFlags, as each native body reads or writes it

        // 3a. CBaseEntity::GetFlags (vampire.dll+0xB3700) -> mov eax, [ecx+0x434]; ret
        static const unsigned char kScopeEntHead[15] = {
            0x85, 0xC9, 0x74, 0x11, 0x8B, 0x91, 0x6C, 0x02, 0x00, 0x00, 0x85, 0xD2, 0x75, 0x0C, 0xBA
        };
        static const unsigned char kFastGetFlags[7] = {
            0x8B, 0x81, 0x34, 0x04, 0x00, 0x00, 0xC3
        };
        if (std::memcmp(base + 0xB3700, kScopeEntHead, sizeof(kScopeEntHead)) == 0 &&
            u32_at_equals(base + 0xB3700 + 0x6B, kFlagsOffset) && base[0xB3700 + 0x78] == 0xC3 &&
            write_bytes_protected(base + 0xB3700, kFastGetFlags, sizeof(kFastGetFlags)))
            ++patched;

        // 3b. CBaseEntity::AddFlag (vampire.dll+0xB3580) -> mov eax, [esp+4]; or [ecx+0x434], eax; ret 4
        static const unsigned char kFastAddFlag[13] = {
            0x8B, 0x44, 0x24, 0x04, 0x09, 0x81, 0x34, 0x04, 0x00, 0x00, 0xC2, 0x04, 0x00
        };
        if (std::memcmp(base + 0xB3580, kScopeEntHead, sizeof(kScopeEntHead)) == 0 &&
            u32_at_equals(base + 0xB3580 + 0x6F, kFlagsOffset) && u32_at_equals(base + 0xB3580 + 0x78, kFlagsOffset) &&
            base[0xB3580 + 131] == 0xC2 && base[0xB3580 + 132] == 0x04 && base[0xB3580 + 133] == 0x00 &&
            write_bytes_protected(base + 0xB3580, kFastAddFlag, sizeof(kFastAddFlag)))
            ++patched;

        // 3c. CBaseEntity::RemoveFlag (vampire.dll+0xB3640) -> mov eax, [esp+4]; not eax; and [ecx+0x434], eax; ret 4
        static const unsigned char kFastRemoveFlag[15] = {
            0x8B, 0x44, 0x24, 0x04, 0xF7, 0xD0, 0x21, 0x81, 0x34, 0x04, 0x00, 0x00, 0xC2, 0x04, 0x00
        };
        if (std::memcmp(base + 0xB3640, kScopeEntHead, sizeof(kScopeEntHead)) == 0 &&
            u32_at_equals(base + 0xB3640 + 0x70, kFlagsOffset) && u32_at_equals(base + 0xB3640 + 0x7A, kFlagsOffset) &&
            base[0xB3640 + 133] == 0xC2 && base[0xB3640 + 134] == 0x04 && base[0xB3640 + 135] == 0x00 &&
            write_bytes_protected(base + 0xB3640, kFastRemoveFlag, sizeof(kFastRemoveFlag)))
            ++patched;

        // 3d. CBaseEntity::Relink (vampire.dll+0xB3B00): empty body except g_ScopeTraceStack -> ret
        static const unsigned char kRelinkHead[15] = {
            0x85, 0xC9, 0x74, 0x11, 0x8B, 0x89, 0x6C, 0x02, 0x00, 0x00, 0x85, 0xC9, 0x75, 0x0C, 0xB9
        };
        static const unsigned char kRet[1] = {0xC3};
        if (std::memcmp(base + 0xB3B00, kRelinkHead, sizeof(kRelinkHead)) == 0 &&
            base[0xB3B00 + 0x6E] == 0xC3 &&
            write_bytes_protected(base + 0xB3B00, kRet, sizeof(kRet)))
            ++patched;

        if (patched > 0) {
            char msg[128];
            sprintf_s(msg, "Fast CBaseEntity flag/relink ops enabled (%d/4 patched in vampire.dll).", patched);
            host->log(msg);
        }
    }

    // 4. Fast SSE2 AngleVectors in vampire.dll (vampire.dll+0x139610)
    if (enabled("fast_angle_math", "1") && perf_math::detect_cpu().sse2) {
        static const unsigned char kAngleHead[28] = {
            0x83, 0xEC, 0x20, 0x8D, 0x4C, 0x24, 0x24, 0x8D, 0x44, 0x24, 0x00, 0x89, 0x4C, 0x24,
            0x14, 0x8B, 0x4C, 0x24, 0x24, 0x89, 0x44, 0x24, 0x18, 0xD9, 0x41, 0x04, 0xD8, 0x0D
        };
        if (std::memcmp(base + 0x139610, kAngleHead, sizeof(kAngleHead)) == 0 &&
            perf_math::write_jmp5(base + 0x139610, reinterpret_cast<void*>(fast_angle_vectors)))
            host->log("Fast SSE2 AngleVectors enabled (vampire.dll+0x139610).");
    }
}

} // namespace perf_runtime
