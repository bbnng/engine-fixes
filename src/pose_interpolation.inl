// Client-side NPC pose parameter interpolation (0.8.3).
//
// The server sends NPC pose parameters (for walkers: move_yaw, the legs' movement direction)
// once per AI think, about every 100 ms. This client build does not interpolate them: the
// value is used as-is until the next update, so the leg blend changes in visible 10 Hz steps
// (captured walking-0.8.2-2026-11-12-1641.log: pose 0 holds for ~0.1 s, then jumps up to 36
// degrees) while the body's position and facing move smoothly every frame.
//
// Inside the existing client pose-build hook (client+0x979b0), each changed networked value
// is blended linearly from what was shown to the new value over `npc_pose_interpolation_ms`,
// written only for that build, and restored afterwards, so networking and the server are
// untouched. A jump of more than half the range (the move_yaw +-180 wrap) snaps, as before.
// A pose the build itself writes is treated as client-driven and left alone for that NPC.
// Client entity: pose parameters at +0x5d4 (24 floats, 0..1), entity index at +0x44.

constexpr int pose_count = 24;
struct PoseBlend {
    const void* entity = nullptr;
    unsigned handle = 0;
    double seen = 0;
    unsigned client_driven = 0;
    float net[pose_count] = {}, from[pose_count] = {};
    double start[pose_count] = {};
};
// One slot per entity index (the engine allows 2048 entities): shared slots made two NPCs
// 512 indices apart reset each other every frame, so neither was blended.
PoseBlend pose_blends[2048];
bool pose_interpolation = false;
float pose_interpolation_ms = 100.0f;
unsigned pose_blend_frames = 0, pose_blend_snaps = 0, pose_client_driven = 0;

double pose_clock_ms() {
    static LARGE_INTEGER frequency{};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

float pose_blend_value(const PoseBlend& state, int i, double now, float duration) {
    const double t = duration > 0.0f ? (now - state.start[i]) / duration : 1.0;
    if (!(t < 1.0)) return state.net[i];
    if (!(t > 0.0)) return state.from[i];
    return state.from[i] + (state.net[i] - state.from[i]) * static_cast<float>(t);
}

// Pure core used by the hook and the offline tests. `poses` holds the networked values on
// entry; on return it holds the values to show for this build and `shown` gets a copy. The
// returned bit mask lists the parameters that differ from the networked value.
unsigned prepare_pose_blend(PoseBlend& state, const void* entity, unsigned handle, double now,
                            float duration, float* poses, float* shown) {
    const bool fresh = state.entity != entity || state.handle != handle ||
                       !(now >= state.seen) || now - state.seen > 500.0;
    if (fresh) {
        state = {};
        state.entity = entity;
        state.handle = handle;
        for (int i = 0; i < pose_count; ++i) { state.net[i] = state.from[i] = poses[i]; state.start[i] = now - duration; }
    }
    state.seen = now;
    unsigned mask = 0;
    for (int i = 0; i < pose_count; ++i) {
        const float value = poses[i];
        shown[i] = value;
        if (state.client_driven & (1u << i)) continue;
        if (!std::isfinite(value)) { state.net[i] = state.from[i] = value; state.start[i] = now - duration; continue; }
        if (value != state.net[i]) {
            const float current = pose_blend_value(state, i, now, duration);
            const bool snap = !std::isfinite(current) || std::fabs(value - current) > 0.5f;
            if (snap) ++pose_blend_snaps;
            state.from[i] = snap ? value : current;
            state.net[i] = value;
            state.start[i] = now;
        }
        const float display = pose_blend_value(state, i, now, duration);
        if (display != value && std::isfinite(display)) { poses[i] = shown[i] = display; mask |= 1u << i; }
    }
    return mask;
}

// After the build: restore the networked value, unless the build wrote its own.
void finish_pose_blend(PoseBlend& state, unsigned mask, float* poses, const float* shown) {
    for (int i = 0; i < pose_count; ++i) {
        if (!(mask & (1u << i))) continue;
        if (poses[i] == shown[i]) poses[i] = state.net[i];
        else { state.client_driven |= 1u << i; ++pose_client_driven; }
    }
}

// Returns the slot for an NPC's client entity, or nullptr when the entity is not an NPC.
PoseBlend* npc_pose_slot(const void* self, unsigned& handle) {
    if (!server_base) return nullptr;
    const auto c = static_cast<const unsigned char*>(self);
    const int index = *reinterpret_cast<const int*>(c + 0x44);
    if (index <= 1 || index >= 2048) return nullptr;
    const auto list = *reinterpret_cast<const unsigned char* const*>(server_base + 0x566458);
    if (!list) return nullptr;
    const auto entry = list + 4 + 12 * index;
    const auto s = *reinterpret_cast<const unsigned char* const*>(entry);
    if (!s) return nullptr;
    const char* cls = *reinterpret_cast<const char* const*>(s + 0x11c);
    if (!cls || std::strncmp(cls, "npc_", 4)) return nullptr;
    handle = (*reinterpret_cast<const unsigned*>(entry + 4) << 13) | static_cast<unsigned>(index);
    return &pose_blends[index];
}

struct PoseBlendScope {
    PoseBlend* state = nullptr;
    float* poses = nullptr;
    unsigned mask = 0;
    float shown[pose_count] = {};
};
void begin_pose_blend(void* self, PoseBlendScope& scope) {
    if (!pose_interpolation) return;
    __try {
        unsigned handle = 0;
        PoseBlend* state = npc_pose_slot(self, handle);
        if (!state) return;
        float* poses = reinterpret_cast<float*>(static_cast<unsigned char*>(self) + 0x5d4);
        scope.mask = prepare_pose_blend(*state, self, handle, pose_clock_ms(), pose_interpolation_ms, poses, scope.shown);
        scope.state = state;
        scope.poses = poses;
        if (scope.mask) ++pose_blend_frames;
    } __except (EXCEPTION_EXECUTE_HANDLER) { scope = {}; }
}
void end_pose_blend(PoseBlendScope& scope) {
    if (!scope.state || !scope.mask) return;
    __try { finish_pose_blend(*scope.state, scope.mask, scope.poses, scope.shown); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
