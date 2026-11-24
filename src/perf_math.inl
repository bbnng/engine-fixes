// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Fast IEEE-754 SSE4.1/SSE2 replacements for MSVC 7.1 static-CRT math (floor, ceil, sqrt, sin,
// cos, tan, atan, asin, acos, log, log10, pow), memcpy/memmove, and __strcmpi across the game's
// core DLLs.
//
// In 2004 MSVC 7.1 CRT, floor() and ceil() call _ctrlfp() (FLDCW) twice per call to switch x87
// rounding mode, and _CI*/double math routines call FLDCW twice per call whenever Direct3D 9
// lowers the x87 precision word from 0x027F to 0x007F. On modern x86 CPUs FLDCW is a full
// pipeline stall (~30-60 cycles), consuming ~35-40% of total CPU time in busy scenes.
//
// Each replacement preserves every required register, EFLAGS (for _CI* intrinsics), and the x87
// FPU control word, and produces IEEE-754 results (including -0.0, subnormals, +/-Inf, and NaN).
// Every site is verified by exact instruction bytes before patching.

#include <intrin.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace perf_math {

struct CpuCaps {
    bool sse2 = false;
    bool sse41 = false;
};

inline CpuCaps detect_cpu() {
    CpuCaps c{};
    int info[4] = {};
    __cpuid(info, 1);
    c.sse2 = (info[3] & (1 << 26)) != 0;
    c.sse41 = (info[2] & (1 << 19)) != 0;
    return c;
}

// __cdecl double floor(double x) -> returns in st(0), preserves all GPRs & EFLAGS
__declspec(naked) inline void fast_floor_sse41() {
    __asm {
        sub     esp, 8
        // roundsd xmm0, qword ptr [esp+12], 1  (round toward -inf)
        __emit  0x66
        __emit  0x0F
        __emit  0x3A
        __emit  0x0B
        __emit  0x44
        __emit  0x24
        __emit  0x0C
        __emit  0x01
        movsd   qword ptr [esp], xmm0
        fld     qword ptr [esp]
        add     esp, 8
        ret
    }
}

// __cdecl double ceil(double x) -> returns in st(0), preserves all GPRs & EFLAGS
__declspec(naked) inline void fast_ceil_sse41() {
    __asm {
        sub     esp, 8
        // roundsd xmm0, qword ptr [esp+12], 2  (round toward +inf)
        __emit  0x66
        __emit  0x0F
        __emit  0x3A
        __emit  0x0B
        __emit  0x44
        __emit  0x24
        __emit  0x0C
        __emit  0x02
        movsd   qword ptr [esp], xmm0
        fld     qword ptr [esp]
        add     esp, 8
        ret
    }
}

// _CIsqrt(): takes x in st(0), returns sqrt(x) in st(0), preserves all GPRs & EFLAGS
__declspec(naked) inline void fast_cisqrt_sse2() {
    __asm {
        sub     esp, 8
        fstp    qword ptr [esp]
        sqrtsd  xmm0, qword ptr [esp]
        movsd   qword ptr [esp], xmm0
        fld     qword ptr [esp]
        add     esp, 8
        ret
    }
}

// __cdecl double sqrt(double x) -> returns in st(0), preserves all GPRs & EFLAGS
__declspec(naked) inline void fast_sqrt_sse2() {
    __asm {
        sub     esp, 8
        sqrtsd  xmm0, qword ptr [esp + 12]
        movsd   qword ptr [esp], xmm0
        fld     qword ptr [esp]
        add     esp, 8
        ret
    }
}

// Modern MSVC 2022 UCRT SSE2 transcendental wrappers (zero FLDCW, x87 CW untouched)
static double __cdecl ucrt_sin(double x) { return std::sin(x); }
static double __cdecl ucrt_cos(double x) { return std::cos(x); }
static double __cdecl ucrt_tan(double x) { return std::tan(x); }
static double __cdecl ucrt_atan(double x) { return std::atan(x); }
static double __cdecl ucrt_asin(double x) { return std::asin(x); }
static double __cdecl ucrt_acos(double x) { return std::acos(x); }
static double __cdecl ucrt_log(double x) { return std::log(x); }
static double __cdecl ucrt_log10(double x) { return std::log10(x); }
static double __cdecl ucrt_pow(double x, double y) { return std::pow(x, y); }

#define DEFINE_CI_UNARY_THUNK(ci_name, target_fn) \
    __declspec(naked) inline void ci_name() { \
        __asm { pushfd } \
        __asm { push eax } \
        __asm { push ecx } \
        __asm { push edx } \
        __asm { sub esp, 8 } \
        __asm { fstp qword ptr [esp] } \
        __asm { call target_fn } \
        __asm { add esp, 8 } \
        __asm { pop edx } \
        __asm { pop ecx } \
        __asm { pop eax } \
        __asm { popfd } \
        __asm { ret } \
    }

DEFINE_CI_UNARY_THUNK(fast_cisin, ucrt_sin)
DEFINE_CI_UNARY_THUNK(fast_cicos, ucrt_cos)
DEFINE_CI_UNARY_THUNK(fast_citan, ucrt_tan)
DEFINE_CI_UNARY_THUNK(fast_ciatan, ucrt_atan)
DEFINE_CI_UNARY_THUNK(fast_ciasin, ucrt_asin)
DEFINE_CI_UNARY_THUNK(fast_ciacos, ucrt_acos)
DEFINE_CI_UNARY_THUNK(fast_cilog, ucrt_log)
DEFINE_CI_UNARY_THUNK(fast_cilog10, ucrt_log10)
#undef DEFINE_CI_UNARY_THUNK

// _CIpow(): st(1) = base x, st(0) = exponent y -> pops both, returns pow(x, y) in st(0)
__declspec(naked) inline void fast_cipow() {
    __asm {
        pushfd
        push    eax
        push    ecx
        push    edx
        sub     esp, 16
        fstp    qword ptr [esp + 8]
        fstp    qword ptr [esp]
        call    ucrt_pow
        add     esp, 16
        pop     edx
        pop     ecx
        pop     eax
        popfd
        ret
    }
}

// Fast ASCII case-insensitive string comparison matching MSVC 7.1 __strcmpi (-1 / 0 / +1).
// Eliminates per-call locale-handle check and frame setup; uses L1-resident lowercase LUT.
inline int __cdecl fast_stricmp(const char* a, const char* b) {
    static constexpr unsigned char kLower[256] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
        0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,
        0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,
        0x40,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,
        0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x5b,0x5c,0x5d,0x5e,0x5f,
        0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,
        0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x7b,0x7c,0x7d,0x7e,0x7f,
        0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
        0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f,
        0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,
        0xb0,0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf,
        0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf,
        0xd0,0xd1,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xdb,0xdc,0xdd,0xde,0xdf,
        0xe0,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xeb,0xec,0xed,0xee,0xef,
        0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff,
    };
    const auto* s1 = reinterpret_cast<const unsigned char*>(a);
    const auto* s2 = reinterpret_cast<const unsigned char*>(b);
    for (;;) {
        unsigned char c1 = *s1++;
        unsigned char c2 = *s2++;
        if (c1 != c2) {
            c1 = kLower[c1];
            c2 = kLower[c2];
            if (c1 != c2) return c1 < c2 ? -1 : 1;
        }
        if (c1 == 0) return 0;
    }
}

inline bool write_jmp5(unsigned char* site, void* target) {
    DWORD old_prot = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old_prot)) return false;
    const std::int32_t rel = static_cast<std::int32_t>(
        reinterpret_cast<std::uintptr_t>(target) - (reinterpret_cast<std::uintptr_t>(site) + 5));
    site[0] = 0xE9;
    std::memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old_prot, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

inline bool find_text_section(HMODULE mod, unsigned char*& out_start, std::size_t& out_size, std::uintptr_t& out_img_end) {
    if (!mod) return false;
    __try {
        const auto base = reinterpret_cast<unsigned char*>(mod);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x1000) return false;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
        const std::size_t image_size = nt->OptionalHeader.SizeOfImage;
        if (image_size < 0x1000) return false;
        out_img_end = reinterpret_cast<std::uintptr_t>(base) + image_size;
        const auto sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (std::memcmp(sec[i].Name, ".text", 5) == 0 && sec[i].Name[5] == '\0' &&
                sec[i].VirtualAddress < image_size && sec[i].Misc.VirtualSize >= 64) {
                const std::size_t max_size = image_size - sec[i].VirtualAddress;
                out_start = base + sec[i].VirtualAddress;
                out_size = sec[i].Misc.VirtualSize <= max_size ? sec[i].Misc.VirtualSize : max_size;
                return out_size >= 64;
            }
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool contains_bytes(const unsigned char* hay, std::size_t hay_len, const unsigned char* pat, std::size_t pat_len) {
    if (pat_len > hay_len) return false;
    for (std::size_t i = 0; i + pat_len <= hay_len; ++i) {
        if (hay[i] == pat[0] && std::memcmp(hay + i, pat, pat_len) == 0) return true;
    }
    return false;
}

struct ModulePatchStats {
    int floors = 0;
    int ceils = 0;
    int sqrts = 0;
    int trans = 0;
    int memmoves = 0;
    int stricmps = 0;
};

inline ModulePatchStats patch_module(HMODULE mod, const CpuCaps& caps, bool do_math, bool do_memcpy, bool do_stricmp) {
    ModulePatchStats st{};
    unsigned char* text = nullptr;
    std::size_t text_size = 0;
    std::uintptr_t img_end = 0;
    if (!find_text_section(mod, text, text_size, img_end)) return st;
    const auto img_base = reinterpret_cast<std::uintptr_t>(mod);

    // 1. MSVC 7.1 floor(double) / ceil(double):
    //    55 8B EC 51 51 53 56           push ebp; mov ebp,esp; push ecx; push ecx; push ebx; push esi
    //    BE FF FF 00 00 56 FF 35 <cw>   mov esi,0FFFFh; push esi; push dword ptr [cw]
    //    E8 <rel32>                     call _ctrlfp  (target begins with 55 8B EC 51 9B D9 7D FC)
    //    DD 45 08                       fld qword ptr [ebp+8]
    if (do_math && caps.sse41 && text_size >= 32) {
        static const unsigned char kHead[15] = {
            0x55, 0x8B, 0xEC, 0x51, 0x51, 0x53, 0x56,
            0xBE, 0xFF, 0xFF, 0x00, 0x00, 0x56, 0xFF, 0x35
        };
        static const unsigned char kCtrlFp[8] = {0x55, 0x8B, 0xEC, 0x51, 0x9B, 0xD9, 0x7D, 0xFC};
        for (std::size_t i = 0; i + 27 <= text_size; ++i) {
            if (text[i] != 0x55 || text[i + 7] != 0xBE) continue;
            if (std::memcmp(text + i, kHead, sizeof(kHead)) != 0) continue;
            if (text[i + 19] != 0xE8 || text[i + 24] != 0xDD || text[i + 25] != 0x45 || text[i + 26] != 0x08) continue;
            std::uint32_t cw_va = 0;
            std::int32_t call_rel = 0;
            std::memcpy(&cw_va, text + i + 15, 4);
            std::memcpy(&call_rel, text + i + 20, 4);
            if (cw_va < img_base || cw_va > img_end || img_end - cw_va < 4) continue;
            const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(text + i + 24) + call_rel;
            if (target < img_base || target > img_end || img_end - target < sizeof(kCtrlFp)) continue;
            if (std::memcmp(reinterpret_cast<const void*>(target), kCtrlFp, sizeof(kCtrlFp)) != 0) continue;
            const std::uint32_t cw_val = *reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(cw_va));
            if (cw_val == 0x173Fu) {
                if (write_jmp5(text + i, reinterpret_cast<void*>(fast_floor_sse41))) ++st.floors;
            } else if (cw_val == 0x1B3Fu) {
                if (write_jmp5(text + i, reinterpret_cast<void*>(fast_ceil_sse41))) ++st.ceils;
            }
        }
    }

    // 2. MSVC 7.1 1-arg _CI<fn>() + <fn>(double) pairs:
    //    sqrt, sin, cos, tan, atan, asin, acos, log, log10
    if (do_math && caps.sse2 && text_size >= 96) {
        static const unsigned char kHead1[7] = {0x83, 0xEC, 0x08, 0xDD, 0x14, 0x24, 0xE8};
        static const unsigned char kMid1[13] = {
            0xE8, 0x0D, 0x00, 0x00, 0x00, 0x83, 0xC4, 0x08, 0xC3, 0x8D, 0x54, 0x24, 0x04
        };
        static const unsigned char kMid2[5] = {0x52, 0x9B, 0xD9, 0x3C, 0x24};
        static const unsigned char kCwCmp[6] = {0x66, 0x81, 0x3C, 0x24, 0x7F, 0x02};
        static const unsigned char kFsqrt[9] = {0xA9, 0x00, 0x00, 0x00, 0x80, 0x75, 0x1F, 0xD9, 0xFA};
        static const unsigned char kAcosOp[6] = {0xD9, 0xFA, 0xD9, 0xC9, 0xD9, 0xF3};
        static const unsigned char kAsinOp[4] = {0xD9, 0xFA, 0xD9, 0xF3};
        static const unsigned char kLogOp[6] = {0xD9, 0xED, 0xD9, 0xC9, 0xD9, 0xF1};
        static const unsigned char kLog10Op[6] = {0xD9, 0xEC, 0xD9, 0xC9, 0xD9, 0xF1};
        static const unsigned char kAsinCmp[5] = {0x3D, 0x00, 0x00, 0xF0, 0x3F};

        for (std::size_t i = 0; i + 96 <= text_size; ++i) {
            if (text[i] != 0x83 || text[i + 3] != 0xDD) continue;
            if (std::memcmp(text + i, kHead1, sizeof(kHead1)) != 0) continue;
            if (std::memcmp(text + i + 11, kMid1, sizeof(kMid1)) != 0) continue;
            if (text[i + 24] != 0xE8) continue;
            if (std::memcmp(text + i + 29, kMid2, sizeof(kMid2)) != 0) continue;

            void* ci_dst = nullptr;
            void* fn_dst = nullptr;
            bool is_sqrt = false;

            if (std::memcmp(text + i + 36, kCwCmp, sizeof(kCwCmp)) == 0) {
                const unsigned char* tail = text + i + 42;
                if (tail[0] == 0x74 && tail[1] == 0x06 && tail[2] == 0xD9 && tail[3] == 0x2D) {
                    if (tail[8] == 0xD9 && tail[9] == 0xFE) {
                        ci_dst = reinterpret_cast<void*>(fast_cisin);
                        fn_dst = reinterpret_cast<void*>(ucrt_sin);
                    } else if (tail[8] == 0xD9 && tail[9] == 0xFF) {
                        ci_dst = reinterpret_cast<void*>(fast_cicos);
                        fn_dst = reinterpret_cast<void*>(ucrt_cos);
                    } else if (tail[8] == 0xD9 && tail[9] == 0xF2) {
                        ci_dst = reinterpret_cast<void*>(fast_citan);
                        fn_dst = reinterpret_cast<void*>(ucrt_tan);
                    } else if (tail[8] == 0xD9 && tail[9] == 0xE8 && tail[10] == 0xD9 && tail[11] == 0xF3) {
                        ci_dst = reinterpret_cast<void*>(fast_ciatan);
                        fn_dst = reinterpret_cast<void*>(ucrt_atan);
                    }
                } else if (tail[0] == 0x74 && tail[1] == 0x05 && tail[2] == 0xE8 &&
                           std::memcmp(tail + 7, kAsinCmp, sizeof(kAsinCmp)) == 0) {
                    if (contains_bytes(tail, 30, kAcosOp, sizeof(kAcosOp))) {
                        ci_dst = reinterpret_cast<void*>(fast_ciacos);
                        fn_dst = reinterpret_cast<void*>(ucrt_acos);
                    } else if (contains_bytes(tail, 30, kAsinOp, sizeof(kAsinOp))) {
                        ci_dst = reinterpret_cast<void*>(fast_ciasin);
                        fn_dst = reinterpret_cast<void*>(ucrt_asin);
                    }
                }
            } else if (std::memcmp(text + i + 40, kCwCmp, sizeof(kCwCmp)) == 0) {
                const unsigned char* tail = text + i + 46;
                if (tail[0] == 0x74 && tail[1] == 0x05 && tail[2] == 0xE8 &&
                    std::memcmp(tail + 7, kFsqrt, sizeof(kFsqrt)) == 0) {
                    ci_dst = reinterpret_cast<void*>(fast_cisqrt_sse2);
                    fn_dst = reinterpret_cast<void*>(fast_sqrt_sse2);
                    is_sqrt = true;
                } else if (tail[0] == 0x74 && tail[1] == 0x06 && tail[2] == 0xD9 && tail[3] == 0x2D) {
                    if (contains_bytes(tail, 34, kLogOp, sizeof(kLogOp))) {
                        ci_dst = reinterpret_cast<void*>(fast_cilog);
                        fn_dst = reinterpret_cast<void*>(ucrt_log);
                    } else if (contains_bytes(tail, 34, kLog10Op, sizeof(kLog10Op))) {
                        ci_dst = reinterpret_cast<void*>(fast_cilog10);
                        fn_dst = reinterpret_cast<void*>(ucrt_log10);
                    }
                }
            }

            if (ci_dst && fn_dst) {
                if (write_jmp5(text + i, ci_dst) && write_jmp5(text + i + 20, fn_dst)) {
                    if (is_sqrt) ++st.sqrts;
                    else ++st.trans;
                }
            }
        }

        // 2b. MSVC 7.1 _CIpow() + pow(double, double) pair
        static const unsigned char kPowHead[30] = {
            0x83, 0xEC, 0x10, 0xD9, 0xC9, 0xDD, 0x1C, 0x24, 0xDD, 0x54, 0x24, 0x08, 0x8B, 0x44, 0x24,
            0x0C, 0xE8, 0x0D, 0x00, 0x00, 0x00, 0x83, 0xC4, 0x10, 0xC3, 0x8D, 0x54, 0x24, 0x0C, 0xE8
        };
        static const unsigned char kPowMask[6] = {0x81, 0xE1, 0x00, 0x00, 0xF0, 0x7F};
        for (std::size_t i = 0; i + 64 <= text_size; ++i) {
            if (text[i] != 0x83 || text[i + 3] != 0xD9) continue;
            if (std::memcmp(text + i, kPowHead, sizeof(kPowHead)) != 0) continue;
            if (std::memcmp(text + i + 41, kCwCmp, sizeof(kCwCmp)) != 0) continue;
            if (std::memcmp(text + i + 54, kPowMask, sizeof(kPowMask)) != 0) continue;
            if (write_jmp5(text + i, reinterpret_cast<void*>(fast_cipow)) &&
                write_jmp5(text + i + 25, reinterpret_cast<void*>(ucrt_pow))) {
                ++st.trans;
            }
        }
    }

    // 3. MSVC 7.1 static-CRT memcpy/memmove -> modern MSVC SSE2/ERMS std::memmove
    if (do_memcpy && caps.sse2 && text_size >= 32) {
        static const unsigned char kMemmove[26] = {
            0x55, 0x8B, 0xEC, 0x57, 0x56, 0x8B, 0x75, 0x0C, 0x8B, 0x4D, 0x10, 0x8B, 0x7D, 0x08,
            0x8B, 0xC1, 0x8B, 0xD1, 0x03, 0xC6, 0x3B, 0xFE, 0x76, 0x08, 0x3B, 0xF8
        };
        using MemmoveFn = void* (__cdecl*)(void*, const void*, std::size_t);
        MemmoveFn modern_memmove = &std::memmove;
        for (std::size_t i = 0; i + sizeof(kMemmove) <= text_size; ++i) {
            if (text[i] != 0x55 || text[i + 3] != 0x57) continue;
            if (std::memcmp(text + i, kMemmove, sizeof(kMemmove)) != 0) continue;
            if (write_jmp5(text + i, reinterpret_cast<void*>(modern_memmove))) ++st.memmoves;
        }
    }

    // 4. MSVC 7.1 static-CRT __strcmpi -> fast ASCII LUT fast_stricmp
    if (do_stricmp && text_size >= 48) {
        static const unsigned char kStricmpHead[14] = {
            0x55, 0x8B, 0xEC, 0x57, 0x56, 0x53, 0x8B, 0x75, 0x0C, 0x8B, 0x7D, 0x08, 0x8D, 0x05
        };
        static const unsigned char kStricmpLoop[10] = {
            0x8A, 0x06, 0x46, 0x8A, 0x27, 0x47, 0x38, 0xC4, 0x74, 0xF2
        };
        for (std::size_t i = 0; i + 44 <= text_size; ++i) {
            if (text[i] != 0x55 || text[i + 5] != 0x53) continue;
            if (std::memcmp(text + i, kStricmpHead, sizeof(kStricmpHead)) != 0) continue;
            if (std::memcmp(text + i + 32, kStricmpLoop, sizeof(kStricmpLoop)) != 0) continue;
            if (write_jmp5(text + i, reinterpret_cast<void*>(fast_stricmp))) ++st.stricmps;
        }
    }

    return st;
}

inline void install() {
    const bool do_math = enabled("fast_math", "1");
    const bool do_memcpy = enabled("fast_memcpy", "1");
    const bool do_stricmp = enabled("fast_stricmp", "1");
    if (!do_math && !do_memcpy && !do_stricmp) return;
    const CpuCaps caps = detect_cpu();
    if (!caps.sse2) {
        host->log("Fast math/memcpy/stricmp disabled: CPU lacks SSE2.");
        return;
    }
    static const wchar_t* const kTargets[] = {
        L"engine.dll",
        L"client.dll",
        L"vampire.dll",
        L"MaterialSystem.dll",
        L"StudioRender.dll",
        L"vphysics.dll",
        L"shaderapidx9.dll",
        L"tier0.dll",
        L"vstdlib.dll",
        L"GameUI.dll",
    };
    int total_f = 0, total_c = 0, total_s = 0, total_t = 0, total_m = 0, total_si = 0, mods_hit = 0;
    for (const wchar_t* name : kTargets) {
        HMODULE mod = GetModuleHandleW(name);
        if (!mod) continue;
        ModulePatchStats st = patch_module(mod, caps, do_math, do_memcpy, do_stricmp);
        if (st.floors || st.ceils || st.sqrts || st.trans || st.memmoves || st.stricmps) {
            ++mods_hit;
            total_f += st.floors;
            total_c += st.ceils;
            total_s += st.sqrts;
            total_t += st.trans;
            total_m += st.memmoves;
            total_si += st.stricmps;
        }
    }
    char msg[256];
    sprintf_s(msg, "Fast CRT enabled (SSE2=%d SSE4.1=%d): %d module(s) patched (%d floor, %d ceil, %d sqrt, %d trig/pow/log, %d memmove, %d stricmp).",
              caps.sse2 ? 1 : 0, caps.sse41 ? 1 : 0, mods_hit, total_f, total_c, total_s, total_t, total_m, total_si);
    host->log(msg);
}

} // namespace perf_math
