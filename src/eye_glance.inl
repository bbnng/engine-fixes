// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// NPC dialogue eye glances (0.9.0, reworked 0.9.2). Settings: [EyeGlances] in engine_fixes.ini.
//
// vdata/system/dispositiontable.txt gives dispositions an EyeTarget block (others inherit
// Neutral's). In dialogue an NPC glances every Min-Max Interval (5-8 s) at "Fidget Points": all -1
// means one glance at RandomInt(1, 9), a keypad direction +-20 degrees pitch/yaw from the head's eye
// direction (7/8/9 up, 1/2/3 down, 4/6 sideways, 5 straight ahead), 25 units out (0x1026b580). The
// glance moves the eye target by EyeTarget "Eye Turn Rate" per 0.1 s: 0.3 for Neutral and every
// disposition inheriting it, against 0.9 for the normal gaze, and only advances once the eyes
// arrive. So the eyes swept out over ~0.4-0.5 s and back just as slowly: the "eye roll".
// A few dispositions author their own
// glances: Apathy [7,8,9] at 0.2 with a 1-2 s hold (a deliberate bored eye roll), Confused [7,5,9],
// Disgust [0,2,3], Anger/PrinceSitting [0,2,0], Damaged [0,0,0].
//
// IDA (vampire.dll; disposition table object at +0x924980, records *(table+8) + 612 * index,
// index clamped like the game: < 0 or > *(table+20) -> *(table+4); NPC disposition index +0x64d4):
//   0x102c0010 glance stepper, thiscall(npc, int), retn 4. Only reader of the NPC's copied
//              interval (+0x6584 min, +0x6588 max, written by SetDisposition 0x102c0f70).
//   0x100ecc60 GetEyeFidgetPoint(index, step), retn 8: record+580.. list, all -1 -> RandomInt(1, 9).
//   0x100ece60 GetEyeHold(index, float* min, float* max), retn 0Ch: record+600 / +604.
//   0x100ecdf0 GetEyeTurnRate(index, glance), retn 8, st0: glance ? record+608 : record+572.
//   0x1026b580 glance point, thiscall(npc, type, Vector* out), retn 8; 0x10331da0 eye position and
//              direction, thiscall(npc, Vector* pos, Vector* dir).
// The hooks only change random glances, unless keep_authored_glances=0. The normal gaze is untouched.

struct GlanceSettings {
    bool allow = true;               // glances=0: NPCs keep looking at you, no random glances
    bool keep_authored = true;
    float turn_rate = 0.1f;          // 0 = game value
    float angle = 14.0f;             // degrees; 0 = game value (20)
    float interval_min = 8.0f, interval_max = 14.0f;  // 0 = game value
    float hold_min = 0.35f, hold_max = 0.7f;          // 0 = game value
    // Odds per direction: side (4 and 6 each), down (2), down diagonal (1 and 3 each),
    // up (8), up diagonal (7 and 9 each), straight ahead (5).
    float weights[6] = {4.0f, 1.0f, 0.5f, 0.25f, 0.25f, 0.0f};
};
GlanceSettings glance;
std::uintptr_t glance_server = 0;
unsigned glance_random_state = 0x9e3779b9u;
unsigned glance_count = 0;

using EyeTurnRateFn = double (__thiscall*)(void*, int, char);
using EyeFidgetPointFn = int (__thiscall*)(void*, int, int);
using EyeHoldFn = int (__thiscall*)(void*, int, float*, float*);
using GlanceStepFn = int (__thiscall*)(void*, int);
using GlancePointFn = int (__thiscall*)(void*, int, float*);
using EyePositionFn = char (__thiscall*)(void*, float*, float*);
EyeTurnRateFn original_eye_turn_rate;
EyeFidgetPointFn original_eye_fidget_point;
EyeHoldFn original_eye_hold;
GlanceStepFn original_glance_step;
GlancePointFn original_glance_point;
EyePositionFn game_eye_position;

// --- pure decisions (offline-tested) ---

// Disposition record for `index`, clamped like the game's getters; nullptr if unreadable.
const unsigned char* disposition_record(const void* table, int index) {
    if (!table) return nullptr;
    const auto t = static_cast<const int*>(table);
    if (index < 0 || index > t[5]) index = t[1];
    if (index < 0 || index > t[5]) return nullptr;
    const auto records = reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(t[2]));
    return records ? records + 612 * index : nullptr;
}
// True when this disposition's glances are shaped by [EyeGlances]: random-glance dispositions
// always, authored ones (their own Fidget Points) only with keep_authored_glances=0.
bool glances_shaped(const unsigned char* record, bool keep_authored) {
    if (!record) return false;
    return !keep_authored || *reinterpret_cast<const int*>(record + 580) == -1;
}
// Keypad direction for a weighted random glance; r in [0, 1). Returns 0 when all weights are 0.
int pick_glance_direction(const float w[6], float r) {
    const int types[9] = {4, 6, 2, 1, 3, 8, 7, 9, 5};
    const int group[9] = {0, 0, 1, 2, 2, 3, 4, 4, 5};
    float total = 0;
    for (int i = 0; i < 9; ++i) total += std::isfinite(w[group[i]]) && w[group[i]] > 0 ? w[group[i]] : 0.0f;
    if (!(total > 0.0f)) return 0;
    float at = (std::isfinite(r) ? std::fmin(std::fmax(r, 0.0f), 0.999999f) : 0.0f) * total;
    int last_nonzero = 0;
    for (int i = 0; i < 9; ++i) {
        const float weight = std::isfinite(w[group[i]]) && w[group[i]] > 0 ? w[group[i]] : 0.0f;
        if (weight > 0.0f) last_nonzero = types[i];
        if (at < weight) return types[i];
        at -= weight;
    }
    return last_nonzero;
}
// Rotate the glance point toward the eye direction so its angle becomes `factor` times the game's.
bool scale_glance_point(const float pos[3], const float dir[3], float point[3], float factor) {
    if (!(factor > 0.0f) || factor == 1.0f) return false;
    float g[3] = {point[0] - pos[0], point[1] - pos[1], point[2] - pos[2]};
    const float gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!std::isfinite(gl) || !std::isfinite(dl) || gl < 1e-3f || dl < 1e-6f) return false;
    float a[3], b[3];
    for (int i = 0; i < 3; ++i) { a[i] = dir[i] / dl; b[i] = g[i] / gl; }
    const float c = std::fmax(-1.0f, std::fmin(1.0f, a[0] * b[0] + a[1] * b[1] + a[2] * b[2]));
    const float theta = std::acos(c);
    if (!(theta > 1e-4f) || theta > 3.0f) return false;
    const float s = std::sin(theta);
    const float wa = std::sin((1.0f - factor) * theta) / s, wb = std::sin(factor * theta) / s;
    for (int i = 0; i < 3; ++i) point[i] = pos[i] + (wa * a[i] + wb * b[i]) * gl;
    return true;
}
float glance_random() {
    glance_random_state ^= glance_random_state << 13;
    glance_random_state ^= glance_random_state >> 17;
    glance_random_state ^= glance_random_state << 5;
    return static_cast<float>(glance_random_state >> 8) / 16777216.0f;
}

// --- hooks ---

const void* glance_table() { return reinterpret_cast<const void*>(glance_server + 0x924980); }
bool npc_glances_shaped(const void* npc) {
    __try {
        const int index = *reinterpret_cast<const int*>(static_cast<const unsigned char*>(npc) + 0x64d4);
        return glances_shaped(disposition_record(glance_table(), index), glance.keep_authored);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool index_glances_shaped(const void* table, int index) {
    __try { return glances_shaped(disposition_record(table, index), glance.keep_authored); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

double __fastcall shaped_eye_turn_rate(void* table, void*, int index, char glancing) {
    const double rate = original_eye_turn_rate(table, index, glancing);
    if (!glancing || !(glance.turn_rate > 0.0f) || !index_glances_shaped(table, index)) return rate;
    return glance.turn_rate;
}
int __fastcall shaped_eye_fidget_point(void* table, void*, int index, int step) {
    if (!index_glances_shaped(table, index)) return original_eye_fidget_point(table, index, step);
    if (step != 0 || !glance.allow) return -1;  // one glance per sequence (game's random mode); -1 = none
    const int type = pick_glance_direction(glance.weights, glance_random());
    if (!type) return -1;  // every direction has weight 0 ("never look that way"): no glance
    ++glance_count;
    return type;
}
int __fastcall shaped_eye_hold(void* table, void*, int index, float* min, float* max) {
    const int result = original_eye_hold(table, index, min, max);
    if (!index_glances_shaped(table, index)) return result;
    __try {
        if (min && glance.hold_min > 0.0f) *min = glance.hold_min;
        if (max && glance.hold_max > 0.0f) *max = glance.hold_max;
        if (min && max && *max < *min) *max = *min;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}
int __fastcall shaped_glance_step(void* npc, void*, int argument) {
    if ((glance.interval_min > 0.0f || glance.interval_max > 0.0f) && npc_glances_shaped(npc)) {
        __try {
            auto fields = reinterpret_cast<float*>(static_cast<unsigned char*>(npc) + 0x6584);
            if (glance.interval_min > 0.0f) fields[0] = glance.interval_min;
            if (glance.interval_max > 0.0f) fields[1] = glance.interval_max;
            if (fields[1] < fields[0]) fields[1] = fields[0];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return original_glance_step(npc, argument);
}
int __fastcall shaped_glance_point(void* npc, void*, int type, float* out) {
    const int result = original_glance_point(npc, type, out);
    if (!(glance.angle > 0.0f) || glance.angle == 20.0f || !npc_glances_shaped(npc)) return result;
    __try {
        float pos[3] = {}, dir[3] = {};
        if (game_eye_position(npc, pos, dir)) scale_glance_point(pos, dir, out, glance.angle / 20.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}

// --- settings and install ---

// [EyeGlances] keys; "enabled" is read from that section only (other sections have no such key).
float glance_number(const char* key, const char* fallback, float low, float high) {
    if (std::strcmp(key, "enabled")) return setting_number(key, fallback, low, high);
    char setting[32] = {};
    size_t size = 0;
    if (!host->ini_get(host->context, "engine_fixes", "EyeGlances", key, setting, sizeof(setting), &size) || size >= sizeof(setting))
        strcpy_s(setting, fallback);
    char* end = nullptr;
    const double value = std::strtod(setting, &end);
    if (end == setting || !std::isfinite(value) || value < low || value > high) return static_cast<float>(std::strtod(fallback, nullptr));
    return static_cast<float>(value);
}
void load_glance_settings() {
    glance.allow = glance_number("glances", "1", 0, 1) != 0.0f;
    glance.keep_authored = glance_number("keep_authored_glances", "1", 0, 1) != 0.0f;
    glance.turn_rate = glance_number("turn_rate", "0.1", 0, 1);
    glance.angle = glance_number("angle", "14", 0, 45);
    glance.interval_min = glance_number("interval_min", "8", 0, 120);
    glance.interval_max = glance_number("interval_max", "14", 0, 120);
    glance.hold_min = glance_number("hold_min", "0.35", 0, 10);
    glance.hold_max = glance_number("hold_max", "0.7", 0, 10);
    if (glance.hold_max > 0.0f && glance.hold_min > glance.hold_max) glance.hold_max = glance.hold_min;
    glance.weights[0] = glance_number("weight_side", "4", 0, 100);
    glance.weights[1] = glance_number("weight_down", "1", 0, 100);
    glance.weights[2] = glance_number("weight_down_diagonal", "0.5", 0, 100);
    glance.weights[3] = glance_number("weight_up", "0.25", 0, 100);
    glance.weights[4] = glance_number("weight_up_diagonal", "0.25", 0, 100);
    glance.weights[5] = glance_number("weight_ahead", "0", 0, 100);
}

void install_eye_glance() {
    if (glance_number("enabled", "1", 0, 1) == 0.0f) return;
    if (!server_profile_supported()) {
        host->log("NPC eye glances disabled: unsupported server profile.");
        return;
    }
    const auto module = reinterpret_cast<unsigned char*>(GetModuleHandleW(L"vampire.dll"));
    if (!module) return;
    // 1.0.1: the eye position helper the glance point calls is checked too (sub esp,3Ch; push ebx;
    // push esi; mov esi,ecx; push edi; test esi,esi; je; mov ecx,[esi+26Ch]; test ecx,ecx), since a
    // patch may have edited vampire.dll in place.
    const unsigned char eye_position_code[] = {0x83,0xec,0x3c,0x53,0x56,0x8b,0xf1,0x57,0x85,0xf6,
                                               0x74,0x11,0x8b,0x8e,0x6c,0x02,0x00,0x00,0x85,0xc9};
    if (std::memcmp(module + 0x331da0, eye_position_code, sizeof(eye_position_code)) != 0) {
        host->log("NPC eye glances disabled: eye position helper bytes differ.");
        return;
    }
    load_glance_settings();
    glance_server = reinterpret_cast<std::uintptr_t>(module);
    game_eye_position = reinterpret_cast<EyePositionFn>(module + 0x331da0);
    glance_random_state ^= static_cast<unsigned>(GetTickCount64());
    if (!glance_random_state) glance_random_state = 0x9e3779b9u;
    struct Hook { unsigned rva; unsigned char bytes[8]; unsigned size; void* detour; void** original; };
    const Hook hooks[] = {
        {0xecdf0, {0x8b,0x44,0x24,0x04,0x85,0xc0}, 6, reinterpret_cast<void*>(shaped_eye_turn_rate), reinterpret_cast<void**>(&original_eye_turn_rate)},
        {0xecc60, {0x8b,0x54,0x24,0x04,0x85,0xd2}, 6, reinterpret_cast<void*>(shaped_eye_fidget_point), reinterpret_cast<void**>(&original_eye_fidget_point)},
        {0xece60, {0x8b,0x54,0x24,0x04,0x85,0xd2}, 6, reinterpret_cast<void*>(shaped_eye_hold), reinterpret_cast<void**>(&original_eye_hold)},
        {0x2c0010, {0x83,0xec,0x0c,0x53,0x55}, 5, reinterpret_cast<void*>(shaped_glance_step), reinterpret_cast<void**>(&original_glance_step)},
        {0x26b580, {0x83,0xec,0x54,0x8d,0x44,0x24,0x0c}, 7, reinterpret_cast<void*>(shaped_glance_point), reinterpret_cast<void**>(&original_glance_point)},
    };
    int installed = 0;
    for (const auto& h : hooks) {
        void* trampoline = nullptr;
        if (host->hook_function(host->context, module + h.rva, h.bytes, h.size, h.detour, &trampoline) != BLSE_HOOK_OK) {
            for (int i = installed - 1; i >= 0; --i) host->unhook(host->context, module + hooks[i].rva);
            host->log("NPC eye glances disabled: byte check or hook conflict; earlier glance hooks rolled back.");
            return;
        }
        *h.original = trampoline;
        ++installed;
    }
    char message[512];
    sprintf_s(message, "NPC eye glances enabled: glances %s, turn rate %.2f, angle %.0f deg, every %.0f-%.0f s, hold %.2f-%.2f s, "
        "odds side %.2f down %.2f down-diagonal %.2f up %.2f up-diagonal %.2f ahead %.2f, authored glances %s.",
        glance.allow ? "on" : "off", glance.turn_rate, glance.angle, glance.interval_min, glance.interval_max, glance.hold_min, glance.hold_max,
        glance.weights[0], glance.weights[1], glance.weights[2], glance.weights[3], glance.weights[4], glance.weights[5],
        glance.keep_authored ? "kept" : "reshaped");
    host->log(message);
}
