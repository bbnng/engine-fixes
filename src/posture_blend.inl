// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Smooth posture transitions for sitting, huddling and sleeping NPCs.
//
// In client.dll, C_BaseAnimating::MaintainSequenceTransitions (0x10091110) crossfades between
// the old and new sequence using a cubic Hermite S-curve (3t^2 - 2t^3) whose duration comes from
// Studio_GetTransitionTime (client.dll+0x8de30, __cdecl(old_seqdesc, new_seqdesc, cycle)).
// Every sequence in VTMB's character models ships with fadein/fadeout = 0.200 s. For places and
// scripts that lack an authored sit-down or stand-up animation (bums entering/leaving ACT_HUDDLE
// or ACT_SLEEP, Arthur Kilpatrick standing up from Desk_idle01, or seated NPCs interrupted
// directly into standing/walking), 0.200 s snaps a 20-40 inch vertical posture change in a split
// second.
//
// Extending the Hermite fade duration only when transitioning directly between distinct static
// postures (Standing <-> Seated / Low / Lying) without an authored transition sequence makes
// sitting down and standing up smooth universally, while keeping normal walk/idle/combat blends
// and authored Into/Outof animations at their native 0.200 s timing.

enum class PostureClass { Standing, Seated, Low, Lying, Transition };

float posture_blend_seated = 0.75f;
float posture_blend_low = 0.85f;
float posture_blend_lying = 1.00f;
unsigned posture_blend_extended_count = 0;

unsigned char ascii_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}
bool ascii_equals_ci(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (ascii_lower(static_cast<unsigned char>(*a)) != ascii_lower(static_cast<unsigned char>(*b))) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}
bool ascii_contains_ci(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return false;
    for (const char* p = hay; *p; ++p) {
        const char* h = p;
        const char* n = needle;
        while (*h && *n && ascii_lower(static_cast<unsigned char>(*h)) == ascii_lower(static_cast<unsigned char>(*n))) {
            ++h; ++n;
        }
        if (!*n) return true;
    }
    return false;
}

PostureClass classify_sequence_posture(const char* name) {
    if (!name || !*name) return PostureClass::Standing;
    // Authored transition sequences always keep their native 0.20 s handoff.
    if (ascii_contains_ci(name, "into") ||
        ascii_contains_ci(name, "outof") ||
        ascii_contains_ci(name, "out_left") ||
        ascii_contains_ci(name, "out_right") ||
        ascii_contains_ci(name, "upfast") ||
        ascii_contains_ci(name, "getup") ||
        ascii_contains_ci(name, "trans_") ||
        ascii_equals_ci(name, "Nines_Sitting_Down")) {
        return PostureClass::Transition;
    }
    if ((ascii_contains_ci(name, "sitback_") || ascii_contains_ci(name, "sitforward_")) &&
        !ascii_contains_ci(name, "idle") && !ascii_contains_ci(name, "cards_")) {
        return PostureClass::Transition;
    }
    // Lying / sleeping poses.
    if (ascii_equals_ci(name, "sleep") ||
        ascii_contains_ci(name, "sleep_idle") ||
        ascii_contains_ci(name, "stance_lay_idle") ||
        ascii_contains_ci(name, "bed_left") ||
        ascii_contains_ci(name, "bed_right")) {
        return PostureClass::Lying;
    }
    // Ground-sitting / huddled poses (bums, Tin Can Bill).
    if (ascii_equals_ci(name, "huddle") || ascii_contains_ci(name, "huddle")) {
        return PostureClass::Low;
    }
    // Seated poses (chairs, desks, couches, booths, barstools).
    if (ascii_contains_ci(name, "desk_idle") ||
        ascii_contains_ci(name, "sitneutral_idle") ||
        ascii_contains_ci(name, "sitlow_idle") ||
        ascii_contains_ci(name, "couchtv_idle") ||
        ascii_contains_ci(name, "sit_barstool_idle") ||
        ascii_contains_ci(name, "sitback_idle") ||
        ascii_contains_ci(name, "sitforward_idle") ||
        ascii_contains_ci(name, "cards_sitforward") ||
        ascii_contains_ci(name, "stance_sitting_idle") ||
        ascii_contains_ci(name, "stance_princesitting_idle") ||
        ascii_contains_ci(name, "stance_chairdamaged_idle") ||
        ascii_contains_ci(name, "sitting_down_idle")) {
        return PostureClass::Seated;
    }
    return PostureClass::Standing;
}

float adjusted_posture_transition_time(const char* old_name, const char* new_name, float native_time) {
    if (!std::isfinite(native_time) || !(native_time > 0.0f)) return native_time;
    const PostureClass a = classify_sequence_posture(old_name);
    const PostureClass b = classify_sequence_posture(new_name);
    if (a == b || a == PostureClass::Transition || b == PostureClass::Transition) return native_time;
    float target = posture_blend_seated;
    if (a == PostureClass::Lying || b == PostureClass::Lying) target = posture_blend_lying;
    else if (a == PostureClass::Low || b == PostureClass::Low) target = posture_blend_low;
    return native_time < target ? target : native_time;
}

const char* seqdesc_name(const void* seqdesc) {
    if (!seqdesc) return nullptr;
    const auto base = static_cast<const unsigned char*>(seqdesc);
    const int offset = *reinterpret_cast<const int*>(base);
    if (offset <= 0 || offset > 0x100000) return nullptr;
    return reinterpret_cast<const char*>(base + offset);
}

using TransitionTimeFn = float (__cdecl*)(const void*, const void*, float);
TransitionTimeFn original_transition_time;

float __cdecl smooth_posture_transition_time(const void* old_seq, const void* new_seq, float cycle) {
    const float native_time = original_transition_time(old_seq, new_seq, cycle);
    if (!std::isfinite(native_time) || !(native_time > 0.0f) || !old_seq || !new_seq) return native_time;
    __try {
        const char* old_name = seqdesc_name(old_seq);
        const char* new_name = seqdesc_name(new_seq);
        const float adjusted = adjusted_posture_transition_time(old_name, new_name, native_time);
        if (adjusted > native_time) ++posture_blend_extended_count;
        return adjusted;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return native_time;
}

void install_posture_blend() {
    const auto module = GetModuleHandleW(L"client.dll");
    if (!module || !supported(module)) {
        host->log("NPC posture blend fix disabled: unsupported or unavailable client.dll.");
        return;
    }
    posture_blend_seated = setting_number("npc_posture_blend_seated", "0.75", 0.2f, 2.0f);
    posture_blend_low = setting_number("npc_posture_blend_low", "0.85", 0.2f, 2.0f);
    posture_blend_lying = setting_number("npc_posture_blend_lying", "1.00", 0.2f, 2.5f);
    const unsigned char expected[] = {0x83,0xec,0x08,0x56,0x8b,0x74,0x24,0x10,0x85,0xf6,0x57};
    void* trampoline = nullptr;
    if (host->hook_function(host->context, reinterpret_cast<unsigned char*>(module) + 0x8de30,
            expected, sizeof(expected), reinterpret_cast<void*>(smooth_posture_transition_time), &trampoline) != BLSE_HOOK_OK) {
        host->log("NPC posture blend fix disabled: byte check or hook conflict.");
        return;
    }
    original_transition_time = reinterpret_cast<TransitionTimeFn>(trampoline);
    char message[256];
    sprintf_s(message, "NPC posture blend fix enabled: standing <-> seated %.2fs, low/huddle %.2fs, lying/sleep %.2fs (client.dll+0x8DE30).",
              posture_blend_seated, posture_blend_low, posture_blend_lying);
    host->log(message);
}
