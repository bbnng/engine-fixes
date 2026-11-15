/* Engine Fixes: GPLv3-only; see LICENSE and NOTICE for attribution
 * and the Bloodlines interoperability permission. */
namespace knox_eyes {
#include "knox_eyes_bytes.inc"
const std::uint32_t rules_jump_targets[] = {802380,802385,802424,802278,802297,802316,802332};
using Raw = std::uint64_t;
using Rules = Raw (__thiscall*)(void*, float*);
Rules original = nullptr;
std::uintptr_t client = 0;
DWORD thread = 0;
float scale = 0.3f;
bool applied_logged = false;

bool model_is_knox(const char* name) {
    const char expected[] = "character/npc/unique/santa_monica/knox/knox.mdl";
    for (size_t i = 0; i < sizeof(expected); ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c == '\\') c = '/';
        if (c != static_cast<unsigned char>(expected[i])) return false;
    }
    return true;
}
// No model/global pointers are retained between draws. Resolve the controller's
// mapped global index each time; different models can share this controller.
bool prepare(void* self, int indices[2], float values[2]) {
    __try {
        auto* hdr = *reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(self) + 0x6ac);
        if (!hdr || std::memcmp(hdr, "IDST", 4) || !model_is_knox(reinterpret_cast<char*>(hdr + 12))) return false;
        // Bloodlines has a 128-byte model name, not later Source's 64 bytes.
        const int length = *reinterpret_cast<int*>(hdr + 0x8c);
        const int count = *reinterpret_cast<int*>(hdr + 0x160);
        const int offset = *reinterpret_cast<int*>(hdr + 0x164);
        if (length < 0x170 || length > 128*1024*1024 || count < 2 || count > 128 || offset < 0x170 ||
            offset > length || count > (length - offset) / 20) return false;
        for (int i = 0; i < 2; ++i) {
            auto* def = hdr + offset + 20 * i;
            const char* wanted = i ? "left_lid_raiser" : "right_lid_raiser";
            const int relative = *reinterpret_cast<int*>(def + 4);
            const int position = offset + 20 * i;
            if (relative < 0 || relative > length - position - static_cast<int>(std::strlen(wanted)) - 1 ||
                std::memcmp(def + relative, wanted, std::strlen(wanted) + 1) ||
                *reinterpret_cast<float*>(def + 12) != 0.0f || *reinterpret_cast<float*>(def + 16) != 1.0f) return false;
            indices[i] = *reinterpret_cast<int*>(def + 8);
            if (indices[i] < 0 || indices[i] >= 128) return false;
            values[i] = reinterpret_cast<float*>(client + 0x4a4a90)[indices[i]];
            if (!std::isfinite(values[i]) || values[i] < 0.0f || values[i] > 1.0f) return false;
        }
        return indices[0] != indices[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
Raw __fastcall draw_rules(void* self, void*, float* out) {
    const DWORD error = GetLastError();
    int indices[2] = {};
    float saved[2] = {};
    const bool apply = GetCurrentThreadId() == thread && prepare(self, indices, saved);
    float* controls = reinterpret_cast<float*>(client + 0x4a4a90);
    if (apply) for (int i = 0; i < 2; ++i) controls[indices[i]] = saved[i] * scale;
    if (apply && !applied_logged && host && host->log) {
        char line[192];
        sprintf_s(line, "Knox relaxed eyes applied: raisers %.3f/%.3f -> %.3f/%.3f (scale %.3f).",
            saved[0], saved[1], saved[0]*scale, saved[1]*scale, scale);
        host->log(line);
        applied_logged = true;
    }
    SetLastError(error);
    Raw result = 0;
    // Restore shared controller scratch even if native rendering unwinds via SEH.
    __try { result = original(self, out); }
    __finally {
        if (apply) for (int i = 0; i < 2; ++i) controls[indices[i]] = saved[i];
    }
    return result;
}
bool body_matches(std::uintptr_t base) {
    unsigned char expected[sizeof(knox_rules_bytes)];
    std::memcpy(expected, knox_rules_bytes, sizeof(expected));
    for (unsigned off : knox_rules_relocations) {
        std::uint32_t operand;
        std::memcpy(&operand, expected + off, 4);
        operand += static_cast<std::uint32_t>(base - 0x10000000);
        std::memcpy(expected + off, &operand, 4);
    }
    if (std::memcmp(reinterpret_cast<void*>(base + 0xc3cd0), expected, sizeof(expected))) return false;
    for (unsigned i = 0; i < 7; ++i)
        if (*reinterpret_cast<std::uint32_t*>(base + 0xc3ee8 + 4*i) != base + rules_jump_targets[i]) return false;
    return true;
}
void install() {
    if (!enabled("knox_relaxed_eyes")) return;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"client.dll"));
    if (!supported(reinterpret_cast<HMODULE>(base)) || !body_matches(base)) {
        host->log("Knox relaxed eyes disabled: client layout differs or hook conflict."); return;
    }
    scale = setting_number("knox_lid_raiser_scale", "0.3", 0.0f, 1.0f);
    void* trampoline = nullptr;
    if (host->hook_function(host->context, reinterpret_cast<void*>(base + 0xc3cd0),
        knox_rules_bytes, 6, reinterpret_cast<void*>(draw_rules), &trampoline) != BLSE_HOOK_OK) {
        host->log("Knox relaxed eyes disabled: hook refused."); return;
    }
    original = reinterpret_cast<Rules>(trampoline);
    client = base;
    thread = GetCurrentThreadId();
    applied_logged = false;
    host->log("Knox relaxed eyes enabled: only his two eyelid raisers are reduced; no save changes.");
}
} // namespace knox_eyes
