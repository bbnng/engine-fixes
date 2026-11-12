// NPC local-avoidance damping and move_yaw smoothing.
//
// vampire.dll (server profile vtmb-x86-...-c546f4de-...):
//   0x1030a0d0 CAI_PlaneSolver::RunMoveSolver(goal, trace, probe, oscillation, out) thiscall, retn 14h.
//              Re-solves the walking direction from scratch every time it runs. Only 10-20 %
//              of the old direction/velocity is blended in, so in crowds the result can swing
//              100-200 degrees between AI thinks.
//   0x102e19e0 CAI_Motor::MoveFacing(goal) thiscall, retn 4. Receives the final ground move
//              direction (goal+12) and stores move_yaw = move dir - body yaw at NPC+0x63ec.
//   0x102bf310 NPC: SetPoseParameter("move_yaw", NPC+0x63ec), once per NPC think.
// Goal layout (AILocalMoveGoal_t): target +0, dir +12, navType +48. Solver: NPC at +4,
// obstacle suggestion count at +92. gpGlobals->curtime = *(server+0x70b228) + 12.
// goal.facing is at +24. CAI_Motor: NPC +4, face-backwards flag (byte) +40,
// ideal yaw +52, yaw speed +56. UpdateYaw (0x102e1e20) turns the body (NPC vtable +884,
// GetLocalAngles, y at +4) at (int)yaw_speed * 10 degrees per second; motor +44 is the time of
// the last yaw update.
// goal.facing is not the walking direction while avoiding, so "faces_move" only means: no facing target pulls the
// ideal yaw away from goal.facing, and the NPC is not set to walk backwards. The leg clamp is also
// applied in MoveFacing, before the game turns the body and walks, so direction changes that do
// not come from the avoidance solver (new detour waypoints, path corners) are covered too.

struct SteeringState {
    const void* npc = nullptr;
    float time = 0, yaw = 0, target[3] = {};
    // Body yaw after the last MoveFacing, the body's turn rate (deg/s, 0 = unknown) and
    // whether the NPC turns its body toward its walking direction (no facing target, not backwards).
    float body = 0, turn_rate = 0;
    bool faces_move = false;
    // Where the NPC stood when this record was made, and whether the MoveFacing clamp
    // changed the step that followed (to notice a clamped step that did not move).
    float origin[3] = {};
    bool has_origin = false, clamped = false;
};
// A MoveFacing clamp in a think where the avoidance solver already chose a checked direction can
// push the step into the obstacle, so the NPC stands still for one think. The MoveFacing clamp
// skips any think in which the solver ran for that NPC, and a clamped step that does not move
// suspends it for that NPC for 2 s.
struct ClampGuard {
    const void* npc = nullptr;
    float solver_time = -1, suspended_until = -1;
};
struct MoveYawState {
    const void* npc = nullptr;
    float time = 0, value = 0;
};
SteeringState steering_states[256];
ClampGuard clamp_guards[256];
MoveYawState move_yaw_states[256];
float steering_turn_rate = 360.0f, move_yaw_rate = 360.0f, steering_max_leg_angle = 30.0f, steering_follow = 0.4f;
unsigned steering_damped_count = 0, move_yaw_smoothed_count = 0, leg_clamp_count = 0, leg_clamp_skipped = 0, leg_clamp_suspended = 0;
// A damped direction can fail the obstacle
// check in CAI_PlaneSolver::Solve (0x1030aea0, thiscall(goal, probe, out), retn 0Ch). Solve then
// fails, and the NPC either stands still for a think or the navigator snaps to another route.
// Solve is hooked: if it fails after the damping changed the direction, it runs again with the
// game's own solver result, so damping never costs a step.
unsigned solve_retries = 0, solve_retry_saved = 0;
thread_local bool steering_bypass = false, steering_changed = false;

float wrap_degrees(float angle) {
    if (!std::isfinite(angle)) return 0.0f;
    angle = std::fmod(angle, 360.0f);
    if (angle > 180.0f) angle -= 360.0f;
    if (angle < -180.0f) angle += 360.0f;
    return angle;
}
// Move `current` toward `target` (both degrees) by at most `max_step`, along the short way.
float approach_degrees(float target, float current, float max_step) {
    const float diff = wrap_degrees(target - current);
    if (diff > max_step) return wrap_degrees(current + max_step);
    if (diff < -max_step) return wrap_degrees(current - max_step);
    return wrap_degrees(target);
}
float server_time() {
    const auto globals = *reinterpret_cast<const unsigned char**>(server_base + 0x70b228);
    return *reinterpret_cast<const float*>(globals + 12);
}
SteeringState& steering_slot(const void* npc) {
    return steering_states[(reinterpret_cast<std::uintptr_t>(npc) >> 4) % 256];
}
ClampGuard& clamp_guard(const void* npc) {
    auto& guard = clamp_guards[(reinterpret_cast<std::uintptr_t>(npc) >> 4) % 256];
    if (guard.npc != npc) guard = {npc};
    return guard;
}
MoveYawState& move_yaw_slot(const void* npc) {
    return move_yaw_states[(reinterpret_cast<std::uintptr_t>(npc) >> 4) % 256];
}

// Pure decision used by the hook and the offline tests. Returns the direction (degrees) the
// NPC may take now: the solver's choice, limited to `rate` degrees per second away from the
// direction it actually moved last, while it keeps walking to the same target. Stale or
// missing history (> 0.3 s) or invalid input leave the solver's choice alone,
// so a blocked NPC always regains the full solver result within a few thinks.
// The turn rate never exceeds the NPC's own body turn rate, and while the NPC walks
// roughly forward (legs within 2 x max_leg of the body) the direction stays within max_leg of
// where the body will face after this think, so the legs show a curve, not a sidestep.
// `follow` (0..1) is the share of the remaining angle toward the solver's choice taken per 0.1 s
// toward the same target, so a solver flip-flopping between two gaps averages out instead of
// swinging the body back and forth. 1 = off.
bool damped_steering_yaw(const SteeringState& last, const void* npc, const float target[3],
                         float now, float rate, float solved, float& result, float max_leg = 0.0f,
                         float follow = 1.0f) {
    result = solved;
    if (!npc || last.npc != npc || !std::isfinite(solved) || !std::isfinite(now) || !(rate > 0.0f)) return false;
    const bool same_target = last.target[0] == target[0] && last.target[1] == target[1] && last.target[2] == target[2];
    const float age = now - last.time;
    if (!(age >= 0.0f) || age > 0.3f) return false;
    const float body_rate = std::isfinite(last.turn_rate) && last.turn_rate > 0.0f ? last.turn_rate : 0.0f;
    if (body_rate > 0.0f && body_rate < rate) rate = body_rate;
    // Several move steps can run inside one think; never allow less than one think's turn.
    // The rate limit applies only toward the same target; a new waypoint or detour may turn
    // freely, but (below) still only as fast as the body follows.
    if (same_target) {
        const float span = age < 0.1f ? 0.1f : age;
        float goal = solved;
        if (follow > 0.0f && follow < 1.0f) {
            const float share = 1.0f - std::pow(1.0f - follow, span / 0.1f);
            goal = wrap_degrees(last.yaw + wrap_degrees(solved - last.yaw) * share);
        }
        result = approach_degrees(goal, last.yaw, rate * span);
    }
    if (max_leg > 0.0f && last.faces_move && body_rate > 0.0f && std::isfinite(last.body) &&
        std::fabs(wrap_degrees(last.yaw - last.body)) <= 2.0f * max_leg) {
        // This think's MoveFacing turns the body toward the new direction by up to body_rate * dt.
        const float reach = max_leg + body_rate * (age < 0.15f ? age : 0.15f);
        result = approach_degrees(result, last.body, reach);
    }
    return result != wrap_degrees(solved);
}

// Pure decision for MoveFacing: keep the walking direction within max_leg of where the body will
// face after this turn step (body + turn_rate * dt). Returns true when the direction changed.
bool clamp_leg_direction(float wanted, float body, float max_leg, float turn_rate, float dt, float& result) {
    result = wanted;
    if (!(max_leg > 0.0f) || !(turn_rate > 0.0f) || !std::isfinite(wanted) || !std::isfinite(body) || !std::isfinite(turn_rate)) return false;
    if (!(dt >= 0.0f)) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    result = approach_degrees(wanted, body, max_leg + turn_rate * dt);
    return result != wrap_degrees(wanted);
}

// Pure decision for the move_yaw pose value (degrees, -180..180).
float smoothed_move_yaw(MoveYawState& state, const void* npc, float now, float rate, float raw, bool& changed) {
    changed = false;
    if (!std::isfinite(raw) || !std::isfinite(now) || !(rate > 0.0f)) { state = {}; return raw; }
    float applied = raw;
    const float age = now - state.time;
    if (state.npc == npc && age >= 0.0f && age <= 0.5f) {
        applied = approach_degrees(raw, state.value, rate * age);
        changed = applied != wrap_degrees(raw);
    }
    state = {npc, now, applied};
    return applied;
}

using MoveFacingFn = int (__thiscall*)(void*, const float*);
using LocalAnglesFn = const float* (__thiscall*)(const void*);
using RunMoveSolverFn = char (__thiscall*)(void*, const float*, void*, float, char, float*);
using SolveFn = char (__thiscall*)(void*, float*, float, float*);
SolveFn original_solve;
using ApplyMoveYawFn = int (__thiscall*)(void*, int);
MoveFacingFn original_move_facing;
RunMoveSolverFn original_run_move_solver;
ApplyMoveYawFn original_apply_move_yaw;

// Pure check: did the last clamped step barely move? `speed` is the goal's walking speed (u/s).
bool clamped_step_blocked(const SteeringState& last, const float origin[3], float speed, float now) {
    if (!last.clamped || !last.has_origin || !std::isfinite(speed) || !(speed > 0.0f)) return false;
    const float age = now - last.time;
    if (!(age > 0.0f) || age > 0.3f) return false;
    const float expected = speed * age;
    if (expected < 2.0f) return false;
    const float dx = origin[0] - last.origin[0], dy = origin[1] - last.origin[1];
    const float moved = std::sqrt(dx * dx + dy * dy);
    return std::isfinite(moved) && moved < 0.25f * expected;
}

// Body state is read after the original ran, when it has turned the body for this move.
void record_facing_state(const void* motor, const float* goal, bool clamped = false) {
    __try {
        const auto m = static_cast<const unsigned char*>(motor);
        const void* npc = *reinterpret_cast<void* const*>(m + 4);
        const float x = goal[3], y = goal[4];
        if (!npc || !std::isfinite(x) || !std::isfinite(y) || (x == 0.0f && y == 0.0f)) return;
        auto& state = steering_slot(npc);
        state = {};
        state.npc = npc;
        state.time = server_time();
        state.yaw = static_cast<float>(std::atan2(y, x) * 57.29577951308232);
        state.clamped = clamped;
        std::memcpy(state.target, goal, sizeof(state.target));
        const float speed = *reinterpret_cast<const float*>(m + 56);
        if (std::isfinite(speed) && speed > 0.0f && speed < 1000.0f)
            state.turn_rate = static_cast<float>(static_cast<int>(speed)) * 10.0f;
        const float fx = goal[6], fy = goal[7], ideal = *reinterpret_cast<const float*>(m + 52);
        const auto vtable = *reinterpret_cast<void* const* const*>(npc);
        if (const float* origin = reinterpret_cast<LocalAnglesFn>(vtable[880 / 4])(npc)) {
            std::memcpy(state.origin, origin, sizeof(state.origin));
            state.has_origin = std::isfinite(origin[0]) && std::isfinite(origin[1]);
        }
        const float* angles = reinterpret_cast<LocalAnglesFn>(vtable[884 / 4])(npc);
        if (!angles || !std::isfinite(angles[1])) return;
        state.body = wrap_degrees(angles[1]);
        // No facing target pulls the ideal yaw away from the goal's facing direction, and the NPC
        // is not set to walk backwards: its body turns toward where it is going.
        if (std::isfinite(fx) && std::isfinite(fy) && (fx != 0.0f || fy != 0.0f) && std::isfinite(ideal) && !m[40]) {
            const float facing = static_cast<float>(std::atan2(fy, fx) * 57.29577951308232);
            state.faces_move = std::fabs(wrap_degrees(ideal - facing)) < 3.0f;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Before the original: limit how far the walking direction may be from the body this step.
// Needs fresh history (< 0.3 s) saying the NPC has no facing target; flying NPCs are skipped.
// Returns true when it changed the direction.
bool clamp_move_facing(const void* motor, float* goal) {
    if (!(steering_max_leg_angle > 0.0f)) return false;
    __try {
        const auto m = static_cast<const unsigned char*>(motor);
        const void* npc = *reinterpret_cast<void* const*>(m + 4);
        if (!npc || *reinterpret_cast<const int*>(reinterpret_cast<const unsigned char*>(goal) + 48) == 2) return false;
        const auto& last = steering_slot(npc);
        const float now = server_time();
        const float age = now - last.time;
        if (last.npc != npc || !last.faces_move || !(last.turn_rate > 0.0f) || !(age >= 0.0f) || age > 0.3f) return false;
        auto& guard = clamp_guard(npc);
        const auto vtable = *reinterpret_cast<void* const* const*>(npc);
        if (const float* origin = reinterpret_cast<LocalAnglesFn>(vtable[880 / 4])(npc)) {
            const float speed = *reinterpret_cast<const float*>(reinterpret_cast<const unsigned char*>(goal) + 36);
            if (clamped_step_blocked(last, origin, speed, now)) {
                guard.suspended_until = now + 2.0f;
                ++leg_clamp_suspended;
            }
        }
        // The avoidance solver ran this think: its direction was checked against obstacles.
        if (std::fabs(now - guard.solver_time) < 0.001f || now < guard.suspended_until) { ++leg_clamp_skipped; return false; }
        const float x = goal[3], y = goal[4];
        const float length = std::sqrt(x * x + y * y);
        if (!std::isfinite(length) || length < 1e-4f) return false;
        const float* angles = reinterpret_cast<LocalAnglesFn>(vtable[884 / 4])(npc);
        if (!angles) return false;
        const float body = wrap_degrees(angles[1]);
        const float wanted = static_cast<float>(std::atan2(y, x) * 57.29577951308232);
        float allowed = wanted;
        if (!clamp_leg_direction(wanted, body, steering_max_leg_angle, last.turn_rate,
                now - *reinterpret_cast<const float*>(m + 44), allowed)) return false;
        const double radians = allowed / 57.29577951308232;
        goal[3] = static_cast<float>(std::cos(radians) * length);
        goal[4] = static_cast<float>(std::sin(radians) * length);
        ++leg_clamp_count;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
int __fastcall record_move_facing(void* motor, void*, const float* goal) {
    const bool clamped = clamp_move_facing(motor, const_cast<float*>(goal));
    const int result = original_move_facing(motor, goal);
    record_facing_state(motor, goal, clamped);
    return result;
}

char __fastcall damp_run_move_solver(void* solver, void*, const float* goal, void* trace, float probe,
                                     char oscillation, float* out) {
    const char solved = original_run_move_solver(solver, goal, trace, probe, oscillation, out);
    __try {
        const void* npc = *reinterpret_cast<void* const*>(static_cast<const unsigned char*>(solver) + 4);
        if (npc) clamp_guard(npc).solver_time = server_time();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!solved || !out || steering_bypass) return solved;
    __try {
        const auto bytes = static_cast<const unsigned char*>(solver);
        const void* npc = *reinterpret_cast<void* const*>(bytes + 4);
        if (!npc || *reinterpret_cast<const int*>(reinterpret_cast<const unsigned char*>(goal) + 48) == 2) return solved;
        const float length = std::sqrt(out[0] * out[0] + out[1] * out[1]);
        if (!std::isfinite(length) || length < 1e-4f) return solved;
        const float wanted = static_cast<float>(std::atan2(out[1], out[0]) * 57.29577951308232);
        const auto& last = steering_slot(npc);
        float allowed = wanted;
        const float now = server_time();
        if (!damped_steering_yaw(last, npc, goal, now, steering_turn_rate, wanted, allowed, steering_max_leg_angle, steering_follow)) return solved;
        const double radians = allowed / 57.29577951308232;
        out[0] = static_cast<float>(std::cos(radians) * length);
        out[1] = static_cast<float>(std::sin(radians) * length);
        steering_changed = true;
        if (std::fabs(wrap_degrees(allowed - wanted)) < 2.0f) return solved; // small smoothing: not counted
        ++steering_damped_count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return solved;
}

// Solve copies its result into goal.dir (+12) before checking it, so the retry restores it first.
char __fastcall checked_solve(void* solver, void*, float* goal, float probe, float* out) {
    float dir[3] = {};
    bool saved = false;
    __try { std::memcpy(dir, goal + 3, sizeof(dir)); saved = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    steering_changed = false;
    const char first = original_solve(solver, goal, probe, out);
    if (first || !steering_changed || !saved) return first;
    __try { std::memcpy(goal + 3, dir, sizeof(dir)); } __except (EXCEPTION_EXECUTE_HANDLER) { return first; }
    steering_bypass = true;
    const char second = original_solve(solver, goal, probe, out);
    steering_bypass = false;
    steering_changed = false;
    ++solve_retries;
    if (second) ++solve_retry_saved;
    return second;
}

int __fastcall smooth_apply_move_yaw(void* npc, void*, int argument) {
    auto field = reinterpret_cast<float*>(static_cast<unsigned char*>(npc) + 0x63ec);
    const float raw = *field;
    bool changed = false;
    float now = 0;
    __try { now = server_time(); } __except (EXCEPTION_EXECUTE_HANDLER) { return original_apply_move_yaw(npc, argument); }
    const float applied = smoothed_move_yaw(move_yaw_slot(npc), npc, now, move_yaw_rate, raw, changed);
    if (!changed) return original_apply_move_yaw(npc, argument);
    // The field is only read here; MoveFacing writes a fresh raw value each move.
    *field = applied;
    const int result = original_apply_move_yaw(npc, argument);
    *field = raw;
    ++move_yaw_smoothed_count;
    return result;
}



void install_steering_damping() {
    if (!server_profile_supported()) { host->log("NPC steering damping disabled: unsupported server profile."); return; }
    const auto module = GetModuleHandleW(L"vampire.dll");
    if (!module) return;
    server_base = reinterpret_cast<std::uintptr_t>(module);
    steering_turn_rate = setting_number("npc_steering_turn_rate", "360", 30.0f, 3600.0f);
    steering_max_leg_angle = setting_number("npc_steering_max_leg_angle", "30", 0.0f, 180.0f);
    steering_follow = setting_number("npc_steering_follow", "0.4", 0.05f, 1.0f);
    // Solve first: without the retry safety net, damping is not installed at all.
    const unsigned char solve_bytes[] = {0x83,0xec,0x68,0x55,0x56};
    void* solve = nullptr;
    if (host->hook_function(host->context, reinterpret_cast<void*>(server_base + 0x30aea0), solve_bytes, sizeof(solve_bytes),
            reinterpret_cast<void*>(checked_solve), &solve) != BLSE_HOOK_OK) {
        host->log("NPC steering damping disabled: Solve byte check or hook conflict.");
        return;
    }
    original_solve = reinterpret_cast<SolveFn>(solve);
    const unsigned char facing_bytes[] = {0x83,0xec,0x28,0x56,0x8b,0xf1};
    void* facing = nullptr;
    if (host->hook_function(host->context, reinterpret_cast<void*>(server_base + 0x2e19e0), facing_bytes, sizeof(facing_bytes),
            reinterpret_cast<void*>(record_move_facing), &facing) != BLSE_HOOK_OK) {
        host->unhook(host->context, reinterpret_cast<void*>(server_base + 0x30aea0));
        original_solve = nullptr;
        host->log("NPC steering damping disabled: MoveFacing byte check or hook conflict; Solve hook rolled back.");
        return;
    }
    original_move_facing = reinterpret_cast<MoveFacingFn>(facing);
    const unsigned char solver_bytes[] = {0x81,0xec,0x6c,0x02,0x00,0x00};
    void* solver = nullptr;
    if (host->hook_function(host->context, reinterpret_cast<void*>(server_base + 0x30a0d0), solver_bytes, sizeof(solver_bytes),
            reinterpret_cast<void*>(damp_run_move_solver), &solver) != BLSE_HOOK_OK) {
        host->unhook(host->context, reinterpret_cast<void*>(server_base + 0x2e19e0));
        host->unhook(host->context, reinterpret_cast<void*>(server_base + 0x30aea0));
        original_move_facing = nullptr;
        original_solve = nullptr;
        host->log("NPC steering damping disabled: avoidance solver byte check or hook conflict; MoveFacing and Solve hooks rolled back.");
        return;
    }
    original_run_move_solver = reinterpret_cast<RunMoveSolverFn>(solver);
    char message[512];
    sprintf_s(message, "NPC steering damping enabled: direction turns at most %.0f deg/s or the NPC's body turn rate; legs kept within %.0f deg of the body (0 = off); follow %.2f per think; failed checks retried with the game's direction.", steering_turn_rate, steering_max_leg_angle, steering_follow);
    host->log(message);
}

void install_move_yaw_smoothing() {
    if (!server_profile_supported()) { host->log("NPC move_yaw smoothing disabled: unsupported server profile."); return; }
    const auto module = GetModuleHandleW(L"vampire.dll");
    if (!module) return;
    server_base = reinterpret_cast<std::uintptr_t>(module);
    move_yaw_rate = setting_number("npc_move_yaw_rate", "360", 30.0f, 3600.0f);
    const unsigned char bytes[] = {0xd9,0x81,0xec,0x63,0x00,0x00};
    void* original_apply = nullptr;
    if (host->hook_function(host->context, reinterpret_cast<void*>(server_base + 0x2bf310), bytes, sizeof(bytes),
            reinterpret_cast<void*>(smooth_apply_move_yaw), &original_apply) != BLSE_HOOK_OK) {
        host->log("NPC move_yaw smoothing disabled: byte check or hook conflict.");
        return;
    }
    original_apply_move_yaw = reinterpret_cast<ApplyMoveYawFn>(original_apply);
    char message[192];
    sprintf_s(message, "NPC move_yaw smoothing enabled: leg direction changes at most %.0f deg/s.", move_yaw_rate);
    host->log(message);
}
