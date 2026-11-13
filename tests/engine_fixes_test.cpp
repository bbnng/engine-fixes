#include "../src/engine_fixes.cpp"
#include <cassert>
#include <vector>
#include <string>
#include <cstdio>
#include <limits>
extern "C" void __cdecl _CIacos();
char test_log[2300];
void BLSE_CALL test_logger(const char* message) { strcpy_s(test_log,message); }
int yaw_test_mode=0,yaw_reads=0,yaw_sets=0;
int BLSE_CALL yaw_get(void*,const char* name,char* buffer,size_t capacity,size_t* size) {
    assert(!std::strcmp(name,"debug_npc_move_yaw_snap"));
    if (yaw_test_mode==1) return 0;
    const char* value=yaw_reads++==0 && yaw_test_mode!=4?"1":yaw_test_mode==3?"1":"0";
    strcpy_s(buffer,capacity,value); *size=std::strlen(value); return 1;
}
int BLSE_CALL yaw_set(void*,const char* name,const char* value) {
    assert(!std::strcmp(name,"debug_npc_move_yaw_snap") && !std::strcmp(value,"0"));
    ++yaw_sets; return yaw_test_mode!=2;
}
void test_yaw_setting() {
    BLSE_HostAPI mock={}; mock.struct_size=sizeof(BLSE_HostAPI); mock.cvar_get=yaw_get; mock.cvar_set=yaw_set; mock.log=test_logger;
    const auto previous=host; host=&mock;
    for (yaw_test_mode=0;yaw_test_mode<5;++yaw_test_mode) {
        yaw_reads=yaw_sets=0; test_log[0]=0;
        assert(apply_move_yaw_blend()==(yaw_test_mode==0 || yaw_test_mode==4));
        assert(yaw_sets==(yaw_test_mode==1?0:1));
    }
    host=previous;
    std::puts("Move yaw CVar: native snap disabled/verified, already-disabled accepted; missing, failed-write and failed-readback cases rejected.");
}
int __fastcall mock_activity_sequence(void* self, void*, int activity, int sequence, int, int) {
    auto words = static_cast<int*>(self);
    words[1019] = activity; words[444] = sequence;
    auto values = static_cast<float*>(self);
    values[446] = values[406] = 0.0f;
    static_cast<unsigned char*>(self)[1628] = 0;
    return 456;
}
void test_walk_phase() {
    original_activity_sequence = reinterpret_cast<SetActivitySequence>(mock_activity_sequence);
    int words[6000] = {};
    auto values = reinterpret_cast<float*>(words);
    for (int mode = 0; mode < 14; ++mode) {
        for (auto& record : walk_phases) record = {};
        words[1019] = mode == 6 ? 19 : 9; words[444] = 86; words[5936] = mode == 4 ? 4 : 1;
        values[446] = 0.65f; values[406] = 0.60f;
        const int activity = words[1019];
        assert(transition_at(words, 1, 3, 1, 0, 10.0f) == 456);
        if (mode == 2) values[446] = 0.0054f; // internal idle advance, same simulation tick
        if (mode == 8) values[446] = 0.0054f; // same cycle advance, but a later tick must restart
        if (mode == 9) values[446] = std::numeric_limits<float>::quiet_NaN();
        if (mode == 10) values[446] = 1.0f;
        if (mode == 11) values[446] = -0.01f;
        if (mode == 12) words[5936] = 7;
        if (mode == 13) words[444] = 99;
        if (mode == 5) transition_at(words, 20, 99, 20, 0, 10.0f); // unrelated action
        if (mode == 7) static_cast<unsigned char*>(static_cast<void*>(words))[1628] = 1;
        transition_at(words, activity, mode == 3 ? 87 : 86, activity, 0, mode == 1 || mode == 8 ? 10.1f : 10.0f);
        const bool restored = mode == 0 || mode == 2 || mode == 6;
        assert(values[446] == (restored ? 0.65f : 0.0f));
        assert(values[406] == (restored ? 0.60f : 0.0f));
    }
    std::puts("Walk phase: 14 internal-advance, later-tick, script/death, sequence, event and invalid-cycle cases passed.");
}
int reset_test_mode = 0;
int __fastcall mock_idle_reset(void* self, void*, int sequence) {
    auto words = static_cast<int*>(self);
    if (reset_test_mode != 1) words[444] = sequence;
    if (reset_test_mode == 2) words[5939] = 999;
    if (reset_test_mode == 3) words[1020] = 9;
    return 123;
}
void test_idle_consistency() {
    original_idle_reset = reinterpret_cast<ResetSequence>(mock_idle_reset);
    int words[6000] = {};
    for (int mode = 0; mode < 7; ++mode) {
        words[444] = words[5939] = 1453;
        words[1019] = words[1020] = 1;
        reset_test_mode = mode < 4 ? mode : 0;
        if (mode == 4) words[1019] = 9;
        if (mode == 5) words[1020] = 9;
        if (mode == 6) words[5939] = 888;
        assert(reset_idle_consistently(words, nullptr, 1448) == 123);
        const int expected = mode == 0 ? 1448 : mode == 2 ? 999 : mode == 6 ? 888 : 1453;
        assert(words[5939] == expected);
    }
    std::puts("Idle consistency: 7 adoption and AI-intent guard cases passed.");
}

__declspec(naked) void eye_trampoline_test() {
    __asm { fld1 }
    __asm { ret }
}

__declspec(noinline) double invoke_eye_blend(double value) {
    double one, result, saved;
    const double sentinel = 7.25;
    __asm {
        fld sentinel
        fld value
        call eye_blend_clamp
        fstp one
        fstp result
        fstp saved
    }
    assert(one == 1.0 && saved == sentinel);
    return result;
}

namespace {
int kind = 11, cleaned = 0, calls = 0;
int __fastcall type(void*, void*) { return kind; }
unsigned int __fastcall think(void* self, void*) {
    ++calls;
    static_cast<std::uint32_t*>(self)[262] = 0;
    return 42;
}
int __fastcall destroy(void* self, void*) {
    assert(calls == 1); // cleanup must follow the engine's existing orphan handling
    ++cleaned;
    static_cast<std::uint32_t*>(self)[283] = 0;
    return 0;
}
void run(std::uint32_t handle, int class_id, bool live, bool instance, bool expect_cleanup) {
    std::uint32_t object[304] = {}, entries[6] = {};
    std::uintptr_t table[9] = {};
    table[8] = reinterpret_cast<std::uintptr_t>(type);
    *reinterpret_cast<std::uintptr_t**>(object + 3) = table;
    object[98] = handle;
    object[283] = instance ? 123 : 0;
    entries[5] = 2;
    entries[4] = live ? 123 : 0;
    auto entry_ptr = entries;
    client = reinterpret_cast<std::uintptr_t>(&entry_ptr) - 0x2b8e98;
    kind = class_id; cleaned = calls = 0;
    assert(particle_think(object, nullptr) == 42);
    assert(calls == 1 && cleaned == (expect_cleanup ? 1 : 0));
}
}
char __fastcall mock_run_move_solver(void*, void*, const float*, void*, float probe, char oscillation, float* out) {
    // Flip: the solver wants 148.7 degrees while the NPC last moved at 25 degrees.
    out[0]=static_cast<float>(std::cos(148.7/57.29577951308232)*0.9);
    out[1]=static_cast<float>(std::sin(148.7/57.29577951308232)*0.9);
    out[2]=0.25f;
    return probe==270.0f && oscillation==1 ? 1 : 0;
}
int __fastcall mock_apply_move_yaw(void* npc, void*, int argument) {
    return static_cast<int>(*reinterpret_cast<float*>(static_cast<unsigned char*>(npc)+0x63ec)*10.0f)+argument;
}
// Mock CAI_PlaneSolver::Solve: runs the (hooked) solver, copies the result to goal.dir like the
// game, then the "obstacle check" passes only for the solver's own direction (148.7 deg) unless
// mock_solve_lenient is set.
int mock_solve_calls = 0;
bool mock_solve_lenient = false, mock_solve_reject = false;
char __fastcall mock_solve(void* self, void*, float* goal, float, float* out) {
    ++mock_solve_calls;
    const auto run = reinterpret_cast<RunMoveSolverFn>(damp_run_move_solver);
    if (!run(self, goal, nullptr, 270.0f, 1, out)) return 0;
    goal[3] = out[0]; goal[4] = out[1]; goal[5] = out[2];
    const float yaw = static_cast<float>(std::atan2(out[1], out[0]) * 57.29577951308232);
    if (mock_solve_reject) return 0;
    return mock_solve_lenient || std::fabs(yaw - 148.7f) < 1.0f ? 1 : 0;
}
void test_steering() {
    int npc_a=0,npc_b=0;
    const float target[3]={10,20,0}, other[3]={10,21,0};
    float used=0;
    SteeringState last{&npc_a,5.0f,0.0f,{10,20,0}};
    assert(!damped_steering_yaw(SteeringState{},&npc_a,target,5.1f,360,140,used) && used==140);
    assert(damped_steering_yaw(last,&npc_a,target,5.1f,360,140,used) && std::fabs(used-36)<0.01f);
    assert(damped_steering_yaw(last,&npc_a,target,5.1f,360,-140,used) && std::fabs(used+36)<0.01f);
    assert(!damped_steering_yaw(last,&npc_a,target,5.1f,360,20,used) && used==20);
    assert(!damped_steering_yaw(last,&npc_a,other,5.1f,360,140,used) && used==140);
    assert(!damped_steering_yaw(last,&npc_b,target,5.1f,360,140,used) && used==140);
    assert(!damped_steering_yaw(last,&npc_a,target,5.35f,360,140,used) && used==140);
    assert(!damped_steering_yaw(last,&npc_a,target,4.9f,360,140,used) && used==140);
    assert(damped_steering_yaw(last,&npc_a,target,5.0f,360,140,used) && std::fabs(used-36)<0.01f);
    assert(damped_steering_yaw(last,&npc_a,target,5.2f,360,140,used) && std::fabs(used-72)<0.01f);
    SteeringState wrap{&npc_a,5.0f,170.0f,{10,20,0}};
    assert(!damped_steering_yaw(wrap,&npc_a,target,5.1f,360,-170,used) && used==-170);
    assert(damped_steering_yaw(wrap,&npc_a,target,5.1f,360,90,used) && std::fabs(used-134)<0.01f);
    assert(!damped_steering_yaw(last,&npc_a,target,5.1f,360,std::numeric_limits<float>::quiet_NaN(),used));
    // Recorded zig-zag headings (one per think): per-think change stays <= 36 deg.
    const float headings[]={-57,9,11.5f,25,148.7f,141,144,147,168,-72,-73,-75,-70,12,13,93,93,145,141,141,141,144,14,20,20};
    float yaw=headings[0], worst_raw=0, worst_used=0;
    for (int i=1;i<static_cast<int>(sizeof(headings)/sizeof(headings[0]));++i) {
        SteeringState s{&npc_a,i*0.1f,yaw,{10,20,0}};
        float next=0; damped_steering_yaw(s,&npc_a,target,(i+1)*0.1f,360,headings[i],next);
        worst_raw=std::fmax(worst_raw,std::fabs(wrap_degrees(headings[i]-headings[i-1])));
        worst_used=std::fmax(worst_used,std::fabs(wrap_degrees(next-yaw)));
        yaw=next;
    }
    assert(worst_raw>120 && worst_used<=36.01f);

    MoveYawState state{};
    bool changed=true;
    assert(smoothed_move_yaw(state,&npc_a,1.0f,360,10,changed)==10 && !changed);
    assert(std::fabs(smoothed_move_yaw(state,&npc_a,1.1f,360,-129,changed)+26)<0.01f && changed);
    assert(std::fabs(smoothed_move_yaw(state,&npc_a,1.1f,360,-129,changed)+26)<0.01f && changed);
    assert(std::fabs(smoothed_move_yaw(state,&npc_a,1.2f,360,-30,changed)+30)<0.01f && !changed);
    assert(std::fabs(smoothed_move_yaw(state,&npc_a,1.3f,360,175,changed)-(-66))<0.01f && changed); // -30 -> 175 goes the short way down through -180
    assert(smoothed_move_yaw(state,&npc_a,2.0f,360,90,changed)==90 && !changed);
    assert(smoothed_move_yaw(state,&npc_b,2.1f,360,-90,changed)==-90 && !changed);
    const float nan=std::numeric_limits<float>::quiet_NaN();
    assert(std::isnan(smoothed_move_yaw(state,&npc_b,2.2f,360,nan,changed)) && !changed && !state.npc);

    // Full hook paths with the game's thiscall ABI (callee pops 5 / 1 stack arguments).
    const auto old_base=server_base;
    unsigned char globals[32]={}; *reinterpret_cast<float*>(globals+12)=7.1f;
    unsigned char* globals_ptr=globals;
    server_base=reinterpret_cast<std::uintptr_t>(&globals_ptr)-0x70b228;
    unsigned char solver_object[128]={}, npc_object[0x6400]={};
    *reinterpret_cast<void**>(solver_object+4)=npc_object;
    *reinterpret_cast<int*>(solver_object+92)=2;
    float goal[20]={10,20,0}; reinterpret_cast<int*>(goal)[12]=1;
    steering_slot(npc_object)={npc_object,7.0f,25.0f,{10,20,0}};
    original_run_move_solver=reinterpret_cast<RunMoveSolverFn>(mock_run_move_solver);
    const auto run=reinterpret_cast<RunMoveSolverFn>(damp_run_move_solver);
    float out[3]={};
    assert(run(solver_object,goal,nullptr,270.0f,1,out)==1);
    const float used_yaw=static_cast<float>(std::atan2(out[1],out[0])*57.29577951308232);
    assert(std::fabs(used_yaw-61)<0.05f && std::fabs(std::sqrt(out[0]*out[0]+out[1]*out[1])-0.9f)<0.001f && out[2]==0.25f);
    reinterpret_cast<int*>(goal)[12]=2; // flying NPCs are left alone
    assert(run(solver_object,goal,nullptr,270.0f,1,out)==1 && std::fabs(std::atan2(out[1],out[0])*57.29577951308232-148.7)<0.05);
    reinterpret_cast<int*>(goal)[12]=1;
    assert(run(solver_object,goal,nullptr,100.0f,1,out)==0); // failed solve passes through untouched

    // Solve retry: a damped direction that fails the check is retried with the game's own.
    original_solve=reinterpret_cast<SolveFn>(mock_solve);
    const auto solve=reinterpret_cast<SolveFn>(checked_solve);
    steering_slot(npc_object)={npc_object,7.0f,25.0f,{10,20,0}};
    goal[3]=0.1f; goal[4]=0.2f; goal[5]=0.3f;
    mock_solve_calls=0; const unsigned retries_before=solve_retries, saved_before=solve_retry_saved;
    assert(solve(solver_object,goal,1.0f,out)==1 && mock_solve_calls==2);
    assert(std::fabs(std::atan2(out[1],out[0])*57.29577951308232-148.7)<0.05 && solve_retries==retries_before+1 && solve_retry_saved==saved_before+1);
    assert(!steering_bypass && !steering_changed);
    // The damped direction passes: one call, damping kept.
    mock_solve_lenient=true; mock_solve_calls=0;
    steering_slot(npc_object)={npc_object,7.0f,25.0f,{10,20,0}};
    assert(solve(solver_object,goal,1.0f,out)==1 && mock_solve_calls==1);
    assert(std::fabs(std::atan2(out[1],out[0])*57.29577951308232-61)<0.05);
    // Nothing damped (no history) and the check fails: no retry, the failure stands.
    mock_solve_lenient=false; mock_solve_calls=0;
    steering_slot(npc_object)={};
    assert(solve(solver_object,goal,1.0f,out)==1 && mock_solve_calls==1); // game direction passes
    // The game's own solve fails as well: no retry because nothing was changed.
    mock_solve_reject=true;
    assert(solve(solver_object,goal,1.0f,out)==0 && mock_solve_calls==2);
    mock_solve_reject=false;

    *reinterpret_cast<float*>(npc_object+0x63ec)=10.0f;
    move_yaw_slot(npc_object)={};
    original_apply_move_yaw=reinterpret_cast<ApplyMoveYawFn>(mock_apply_move_yaw);
    const auto apply=reinterpret_cast<ApplyMoveYawFn>(smooth_apply_move_yaw);
    assert(apply(npc_object,3)==103);
    *reinterpret_cast<float*>(globals+12)=7.2f;
    *reinterpret_cast<float*>(npc_object+0x63ec)=-129.0f;
    const int applied_result=apply(npc_object,3); // applied about -26 degrees (float time step)
    assert(applied_result==-257 || applied_result==-256);
    assert(*reinterpret_cast<float*>(npc_object+0x63ec)==-129.0f); // raw value restored
    server_base=old_base;
    for (auto& s:steering_states) s={};
    for (auto& s:move_yaw_states) s={};
    std::puts("NPC steering: 13 damping decisions, recorded 25-think zig-zag (max swing >120 -> <=36 deg/think), 8 move_yaw cases, both thiscall hook paths and the Solve retry (4 cases) passed.");
}
float mock_angles[3] = {0, -20.0f, 0};
const float* __fastcall mock_local_angles(const void*, void*) { return mock_angles; }
float mock_origin[3] = {0, 0, 0};
const float* __fastcall mock_abs_origin(const void*, void*) { return mock_origin; }
float move_facing_seen_yaw = 0;
int __fastcall mock_move_facing(void*, void*, const float* g) {
    move_facing_seen_yaw = static_cast<float>(std::atan2(g[4], g[3]) * 57.29577951308232);
    return 7;
}
void test_steering_body() {
    int npc_a=0;
    const float target[3]={10,20,0};
    float used=0;
    // Body turn rate below the configured rate limits the turn: 250 deg/s -> 25 deg per think.
    SteeringState s{&npc_a,5.0f,0.0f,{10,20,0}};
    s.turn_rate=250; s.body=0; s.faces_move=true;
    assert(damped_steering_yaw(s,&npc_a,target,5.1f,360,140,used,0) && std::fabs(used-25)<0.01f);
    // Legs within reach of the body (30 + 25 deg): unchanged.
    s.body=-20;
    assert(damped_steering_yaw(s,&npc_a,target,5.1f,360,140,used,30) && std::fabs(used-25)<0.01f);
    // Body 50 deg behind: direction pulled back to body + 55.
    s.body=-50;
    assert(damped_steering_yaw(s,&npc_a,target,5.1f,360,140,used,30) && std::fabs(used-5)<0.01f);
    // Facing target (faces_move false), already strafing (legs > 2 x 30) or max_leg 0: no clamp.
    s.faces_move=false;
    assert(damped_steering_yaw(s,&npc_a,target,5.1f,360,140,used,30) && std::fabs(used-25)<0.01f);
    s.faces_move=true; s.body=-90;
    assert(damped_steering_yaw(s,&npc_a,target,5.1f,360,140,used,30) && std::fabs(used-25)<0.01f);
    s.body=-50;
    assert(damped_steering_yaw(s,&npc_a,target,5.1f,360,140,used,0) && std::fabs(used-25)<0.01f);
    // New waypoint or detour: no rate limit, but the legs still stay within reach of the body.
    const float next_target[3]={30,20,0};
    s.body=-50;
    assert(damped_steering_yaw(s,&npc_a,next_target,5.1f,360,100,used,30) && std::fabs(used-5)<0.01f);
    s.faces_move=false;
    assert(!damped_steering_yaw(s,&npc_a,next_target,5.1f,360,140,used,30) && used==140);
    s.faces_move=true;
    // Unknown body rate: plain rate limit only.
    s.turn_rate=0;
    assert(damped_steering_yaw(s,&npc_a,target,5.1f,360,140,used,30) && std::fabs(used-36)<0.01f);

    // Recorded U-turn (solver heading per think) with a simulated body
    // turning at 250 deg/s toward the walking direction, as UpdateYaw does.
    const float uturn[]={-33,-9,24,26,65,63,63,155,155,155,155,155,155,-153,-153,-153};
    float worst_old=0, worst_new=0;
    for (int mode=0; mode<2; ++mode) {
        float dir=uturn[0], body=uturn[0];
        for (int i=1;i<static_cast<int>(sizeof(uturn)/sizeof(uturn[0]));++i) {
            // The 63 -> 155 jump comes with a new target.
            SteeringState st{&npc_a,i*0.1f,dir,{10,20,0}};
            const float* aim=i>=7 ? next_target : target;
            if (i>7) std::memcpy(st.target,next_target,sizeof(st.target));
            st.body=body; st.faces_move=true; st.turn_rate=mode ? 250.0f : 0.0f;
            damped_steering_yaw(st,&npc_a,aim,(i+1)*0.1f,360,uturn[i],dir,mode ? 30.0f : 0.0f);
            body=approach_degrees(dir,body,25.0f);
            float& worst=mode ? worst_new : worst_old;
            worst=std::fmax(worst,std::fabs(wrap_degrees(dir-body)));
        }
        assert(std::fabs(wrap_degrees(dir-uturn[15]))<1.0f); // still completes the turn
    }
    assert(worst_old>60.0f && worst_new<=30.01f);

    // MoveFacing hook (thiscall, callee pops the goal): body, rate and facing read after the original.
    const auto old_base=server_base;
    unsigned char globals[32]={}; *reinterpret_cast<float*>(globals+12)=9.0f;
    unsigned char* globals_ptr=globals;
    server_base=reinterpret_cast<std::uintptr_t>(&globals_ptr)-0x70b228;
    void* vtable[300]={}; vtable[884/4]=reinterpret_cast<void*>(mock_local_angles);
    vtable[880/4]=reinterpret_cast<void*>(mock_abs_origin);
    void* npc_object[64]={}; npc_object[0]=vtable;
    unsigned char motor[64]={};
    *reinterpret_cast<void**>(motor+4)=npc_object;
    *reinterpret_cast<float*>(motor+52)=30.0f;
    *reinterpret_cast<float*>(motor+56)=24.7f;
    float goal[20]={10,20,0, 0.8660254f,0.5f,0, 0.8660254f,0.5f,0};
    original_move_facing=reinterpret_cast<MoveFacingFn>(mock_move_facing);
    const auto facing=reinterpret_cast<MoveFacingFn>(record_move_facing);
    assert(facing(motor,goal)==7);
    const SteeringState& r=steering_slot(npc_object);
    assert(r.npc==npc_object && r.time==9.0f && std::fabs(r.yaw-30)<0.01f && r.body==-20.0f && r.turn_rate==240.0f && r.faces_move);
    *reinterpret_cast<float*>(motor+52)=80.0f; // facing target pulls the ideal yaw away
    facing(motor,goal); assert(!steering_slot(npc_object).faces_move);
    *reinterpret_cast<float*>(motor+52)=30.0f; motor[40]=1; // walking backwards
    facing(motor,goal); assert(!steering_slot(npc_object).faces_move);
    motor[40]=0; vtable[884/4]=nullptr; // a fault in the game call keeps the move record only
    facing(motor,goal); assert(steering_slot(npc_object).npc==npc_object && !steering_slot(npc_object).faces_move);

    // Pure leg clamp for MoveFacing.
    float leg=0;
    assert(clamp_leg_direction(150,-20,30,240,0.05f,leg) && std::fabs(leg-22)<0.01f);
    assert(clamp_leg_direction(150,-20,30,240,-1,leg) && std::fabs(leg-10)<0.01f);   // clock went back: no turn credit
    assert(clamp_leg_direction(150,-20,30,240,5,leg) && std::fabs(leg-34)<0.01f);    // long gap capped at one think
    assert(!clamp_leg_direction(20,-20,30,240,0.05f,leg) && leg==20);                // within reach
    assert(!clamp_leg_direction(150,-20,0,240,0.05f,leg) && leg==150);               // off
    assert(!clamp_leg_direction(150,-20,30,0,0.05f,leg) && leg==150);                // unknown body rate
    assert(!clamp_leg_direction(150,std::numeric_limits<float>::quiet_NaN(),30,240,0.05f,leg) && leg==150);
    assert(clamp_leg_direction(-170,170,30,240,0.05f,leg) == false);                 // 20 deg across the wrap

    // MoveFacing pre-clamp: the game sees the limited direction; the next record is the used one.
    vtable[884/4]=reinterpret_cast<void*>(mock_local_angles);
    *reinterpret_cast<float*>(motor+52)=30.0f;
    goal[3]=0.8660254f; goal[4]=0.5f;
    facing(motor,goal);
    assert(steering_slot(npc_object).faces_move && std::fabs(move_facing_seen_yaw-30)<0.01f);
    *reinterpret_cast<float*>(motor+44)=8.95f;                                      // last yaw update 50 ms ago
    goal[3]=static_cast<float>(std::cos(150/57.29577951308232)); goal[4]=static_cast<float>(std::sin(150/57.29577951308232));
    const unsigned clamps_before=leg_clamp_count;
    facing(motor,goal);
    assert(std::fabs(move_facing_seen_yaw-22)<0.05f && leg_clamp_count==clamps_before+1);
    assert(std::fabs(std::sqrt(goal[3]*goal[3]+goal[4]*goal[4])-1)<1e-4f && std::fabs(steering_slot(npc_object).yaw-22)<0.05f);
    // Facing target recorded last time: no clamp now.
    *reinterpret_cast<float*>(motor+52)=80.0f;
    facing(motor,goal);                                                            // records faces_move=0
    goal[3]=static_cast<float>(std::cos(150/57.29577951308232)); goal[4]=static_cast<float>(std::sin(150/57.29577951308232));
    facing(motor,goal);
    assert(std::fabs(move_facing_seen_yaw-150)<0.05f);
    // Flying NPCs untouched.
    *reinterpret_cast<float*>(motor+52)=30.0f; facing(motor,goal); assert(steering_slot(npc_object).faces_move); // ideal == facing again
    reinterpret_cast<int*>(goal)[12]=2;
    goal[3]=static_cast<float>(std::cos(150/57.29577951308232)); goal[4]=static_cast<float>(std::sin(150/57.29577951308232));
    facing(motor,goal); assert(std::fabs(move_facing_seen_yaw-150)<0.05f);
    reinterpret_cast<int*>(goal)[12]=0;

    // The avoidance solver ran this think -> no MoveFacing clamp.
    *reinterpret_cast<float*>(motor+52)=30.0f; goal[3]=0.8660254f; goal[4]=0.5f;
    facing(motor,goal); assert(steering_slot(npc_object).faces_move);
    clamp_guard(npc_object).solver_time=9.0f;
    goal[3]=static_cast<float>(std::cos(150/57.29577951308232)); goal[4]=static_cast<float>(std::sin(150/57.29577951308232));
    facing(motor,goal); assert(std::fabs(move_facing_seen_yaw-150)<0.05f);
    clamp_guard(npc_object).solver_time=-1;
    // A clamped step that did not move suspends the clamp for 2 s.
    *reinterpret_cast<float*>(motor+52)=150.0f; goal[6]=goal[3]; goal[7]=goal[4];
    facing(motor,goal); assert(steering_slot(npc_object).faces_move);              // ideal == facing (150)
    goal[9]=40.0f;                                                                // walking speed 40 u/s
    goal[3]=static_cast<float>(std::cos(-150/57.29577951308232)); goal[4]=static_cast<float>(std::sin(-150/57.29577951308232));
    facing(motor,goal); assert(std::fabs(move_facing_seen_yaw+62)<0.05f && steering_slot(npc_object).clamped); // -20 - (30 + 240 x 0.05)
    *reinterpret_cast<float*>(globals+12)=9.1f;                                   // 0.1 s later, still at the same spot
    const unsigned suspended_before=leg_clamp_suspended;
    goal[3]=static_cast<float>(std::cos(-150/57.29577951308232)); goal[4]=static_cast<float>(std::sin(-150/57.29577951308232));
    facing(motor,goal);
    assert(leg_clamp_suspended==suspended_before+1 && std::fabs(move_facing_seen_yaw+150)<0.05f);
    assert(clamp_guard(npc_object).suspended_until>11.0f);
    // Pure blocked-step check.
    SteeringState moved{&npc_a,1.0f,0,{0,0,0}}; moved.clamped=true; moved.has_origin=true;
    const float here[3]={0,0,0}, walked[3]={3,0,0};
    assert(clamped_step_blocked(moved,here,40,1.1f));             // expected 4, moved 0
    assert(!clamped_step_blocked(moved,walked,40,1.1f));          // moved 3 of 4
    assert(!clamped_step_blocked(moved,here,10,1.1f));            // expected 1: too small to judge
    assert(!clamped_step_blocked(moved,here,40,1.5f));            // stale
    moved.clamped=false; assert(!clamped_step_blocked(moved,here,40,1.1f));

    // Follow smoothing: half the remaining angle per think toward the same target.
    SteeringState f{&npc_a,5.0f,0.0f,{10,20,0}};
    assert(damped_steering_yaw(f,&npc_a,target,5.1f,360,40,used,0,0.5f) && std::fabs(used-20)<0.01f);
    assert(damped_steering_yaw(f,&npc_a,target,5.2f,360,40,used,0,0.5f) && std::fabs(used-30)<0.01f); // two thinks: 75 %
    assert(!damped_steering_yaw(f,&npc_a,next_target,5.1f,360,40,used,0,0.5f) && used==40);           // new target: free
    // Recorded flip-flop (solver choice per think). Wobble = total
    // angle travelled minus the net change: the back-and-forth part.
    const float flip[]={-9,-48,-3,-54,-105,-51,-105,-105,-45,-42};
    float wobble_old=0, wobble_new=0, end_old=0, end_new=0;
    for (int mode=0; mode<2; ++mode) {
        float dir=flip[0], travelled=0;
        for (int i=1;i<static_cast<int>(sizeof(flip)/sizeof(flip[0]));++i) {
            SteeringState st{&npc_a,i*0.1f,dir,{10,20,0}};
            float next=0; damped_steering_yaw(st,&npc_a,target,(i+1)*0.1f,250,flip[i],next,0,mode ? 0.4f : 1.0f);
            travelled+=std::fabs(wrap_degrees(next-dir));
            dir=next;
        }
        (mode ? wobble_new : wobble_old)=travelled-std::fabs(wrap_degrees(dir-flip[0]));
        (mode ? end_new : end_old)=dir;
    }
    assert(wobble_old>160 && wobble_new<0.5f*wobble_old && std::fabs(wrap_degrees(end_new-end_old))<10);
    std::printf("NPC steering: solver-ran skip, blocked-step suspension, 5 blocked checks, follow smoothing; flip-flop wobble %.0f -> %.0f deg.\n",
        wobble_old, wobble_new);
    server_base=old_base;
    for (auto& st:steering_states) st={};
    std::printf("NPC steering: body-rate limit, leg clamp and its gates, U-turn (legs %.0f -> %.0f deg) and MoveFacing hook passed; leg clamp: 8 decisions and MoveFacing pre-clamp passed.\n", worst_old, worst_new);
}
float overlay_seen_pose = -1;
int __fastcall mock_overlay_pose(void* self, void*, int, int, int) {
    overlay_seen_pose = *reinterpret_cast<float*>(static_cast<unsigned char*>(self) + 0x5d4);
    return 42;
}
void test_pose_blend() {
    int npc_a = 0, npc_b = 0;
    PoseBlend st{};
    float poses[pose_count], shown[pose_count];
    for (float& v : poses) v = 0.5f;
    auto approx = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    // First sight: shown as networked.
    assert(prepare_pose_blend(st, &npc_a, 7, 1000, 100, poses, shown) == 0 && poses[0] == 0.5f);
    // New network value: blend from the shown 0.5 over 100 ms.
    poses[0] = 0.6f;
    assert(prepare_pose_blend(st, &npc_a, 7, 1000, 100, poses, shown) == 1 && approx(poses[0], 0.5f));
    finish_pose_blend(st, 1, poses, shown);
    assert(poses[0] == 0.6f); // networked value restored after the build
    assert(prepare_pose_blend(st, &npc_a, 7, 1050, 100, poses, shown) == 1 && approx(poses[0], 0.55f));
    finish_pose_blend(st, 1, poses, shown);
    // Next update mid-blend continues from what is on screen (0.55), no jump.
    poses[0] = 0.7f;
    assert(prepare_pose_blend(st, &npc_a, 7, 1050, 100, poses, shown) == 1 && approx(poses[0], 0.55f));
    finish_pose_blend(st, 1, poses, shown);
    assert(prepare_pose_blend(st, &npc_a, 7, 1100, 100, poses, shown) == 1 && approx(poses[0], 0.625f));
    finish_pose_blend(st, 1, poses, shown);
    assert(prepare_pose_blend(st, &npc_a, 7, 1150, 100, poses, shown) == 0 && poses[0] == 0.7f);
    // move_yaw wrap (more than half the range): snaps like the game did.
    poses[0] = 0.05f; st.net[0] = 0.95f; st.from[0] = 0.95f; st.start[0] = 0;
    assert(prepare_pose_blend(st, &npc_a, 7, 1200, 100, poses, shown) == 0 && poses[0] == 0.05f);
    // A pose the build writes itself becomes client_entity-driven and is left alone afterwards.
    poses[1] = 0.8f;
    unsigned mask = prepare_pose_blend(st, &npc_a, 7, 1210, 100, poses, shown);
    assert(mask == 2 && approx(poses[1], 0.5f));
    poses[1] = 0.33f; // written by the build
    finish_pose_blend(st, mask, poses, shown);
    assert(poses[1] == 0.33f && (st.client_driven & 2));
    poses[1] = 0.9f;
    assert(prepare_pose_blend(st, &npc_a, 7, 1220, 100, poses, shown) == 0 && poses[1] == 0.9f);
    // Another entity, a reused slot (new handle) or a long gap: no blend from stale values.
    poses[0] = 0.2f;
    assert(prepare_pose_blend(st, &npc_b, 7, 1230, 100, poses, shown) == 0 && poses[0] == 0.2f && !st.client_driven);
    poses[0] = 0.4f;
    assert(prepare_pose_blend(st, &npc_b, 8, 1240, 100, poses, shown) == 0 && poses[0] == 0.4f);
    poses[0] = 0.45f;
    assert(prepare_pose_blend(st, &npc_b, 8, 1900, 100, poses, shown) == 0 && poses[0] == 0.45f);
    // NaN passes through untouched and does not poison later values.
    poses[0] = std::numeric_limits<float>::quiet_NaN();
    assert(prepare_pose_blend(st, &npc_b, 8, 1910, 100, poses, shown) == 0 && std::isnan(poses[0]));
    poses[0] = 0.5f;
    assert(prepare_pose_blend(st, &npc_b, 8, 1920, 100, poses, shown) == 0 && poses[0] == 0.5f);

    // Recorded pose 0 network values, one per 100 ms think, drawn at 60 fps.
    const float recorded[] = {0.499f, 0.593f, 0.693f, 0.618f, 0.542f, 0.499f, 0.456f, 0.499f};
    PoseBlend r{};
    float worst_raw = 0, worst_shown = 0, last = recorded[0];
    for (int frame = 0; frame < 8 * 6; ++frame) {
        float pose[pose_count]; for (float& v : pose) v = 0.5f;
        pose[0] = recorded[frame / 6];
        float out[pose_count];
        const unsigned m = prepare_pose_blend(r, &npc_a, 1, 2000 + frame * (100.0 / 6), 100, pose, out);
        worst_shown = std::fmax(worst_shown, std::fabs(pose[0] - last));
        last = pose[0];
        finish_pose_blend(r, m, pose, out);
    }
    for (int i = 1; i < 8; ++i) worst_raw = std::fmax(worst_raw, std::fabs(recorded[i] - recorded[i - 1]));
    assert(worst_raw >= 0.099f && worst_shown <= 0.0175f);

    // Full hook path: pose_build with a mock pose build, server entity list and NPC class.
    const auto old_base = server_base;
    unsigned char server_npc[0x200] = {};
    const char* npc_class = "npc_VHuman";
    *reinterpret_cast<const char**>(server_npc + 0x11c) = npc_class;
    unsigned char list[4 + 12 * 8] = {};
    *reinterpret_cast<void**>(list + 4 + 12 * 5) = server_npc;
    unsigned char* list_ptr = list;
    server_base = reinterpret_cast<std::uintptr_t>(&list_ptr) - 0x566458;
    unsigned char client_entity[0x700] = {};
    *reinterpret_cast<int*>(client_entity + 0x44) = 5;
    float* cp = reinterpret_cast<float*>(client_entity + 0x5d4);
    for (int i = 0; i < pose_count; ++i) cp[i] = 0.5f;
    original_pose_build = reinterpret_cast<PoseBuild>(mock_overlay_pose);
    pose_interpolation = true;
    const auto overlay = reinterpret_cast<PoseBuild>(pose_build);
    assert(overlay(client_entity, 1, 2, 3) == 42 && overlay_seen_pose == 0.5f);
    cp[0] = 0.6f; // new network value
    assert(overlay(client_entity, 1, 2, 3) == 42 && std::fabs(overlay_seen_pose - 0.5f) < 0.01f && cp[0] == 0.6f);
    *reinterpret_cast<const char**>(server_npc + 0x11c) = "prop_physics"; // not an NPC: untouched
    cp[0] = 0.9f;
    assert(overlay(client_entity, 1, 2, 3) == 42 && overlay_seen_pose == 0.9f && cp[0] == 0.9f);
    pose_interpolation = false;
    server_base = old_base;
    for (auto& b : pose_blends) b = {};
    std::printf("NPC pose interpolation: blend, restore, mid-blend update, wrap snap, client-driven, identity/stale/NaN, recorded sequence (step %.3f -> %.4f per frame) and hook path passed.\n", worst_raw, worst_shown);
}
double __fastcall mock_eye_turn_rate(void*, void*, int index, char glancing) {
    return glancing ? (index == 1 ? 0.2 : 0.3) : 0.9;
}
int __fastcall mock_eye_fidget_point(void*, void*, int index, int step) { return 100 + index * 10 + step; }
int __fastcall mock_eye_hold(void*, void*, int, float* min, float* max) { *min = 0.15f; *max = 0.25f; return 5; }
int mock_glance_step_calls = 0;
int __fastcall mock_glance_step(void*, void*, int argument) { ++mock_glance_step_calls; return argument + 1; }
int __fastcall mock_glance_point(void*, void*, int, float* out) {
    // Game point for a 20 degree yaw glance, 25 units out from the origin, eye direction +x.
    out[0] = static_cast<float>(std::cos(20 / 57.29577951308232) * 25); out[1] = static_cast<float>(std::sin(20 / 57.29577951308232) * 25); out[2] = 0;
    return 9;
}
char __fastcall mock_eye_position(void*, void*, float* pos, float* dir) {
    pos[0] = pos[1] = pos[2] = 0; dir[0] = 1; dir[1] = dir[2] = 0; return 1;
}
void test_eye_glance() {
    // Weighted direction pick.
    const float side_only[6] = {1, 0, 0, 0, 0, 0};
    assert(pick_glance_direction(side_only, 0.0f) == 4 && pick_glance_direction(side_only, 0.49f) == 4 && pick_glance_direction(side_only, 0.51f) == 6);
    const float none[6] = {};
    assert(pick_glance_direction(none, 0.3f) == 0);
    const float defaults[6] = {4, 1, 0.5f, 0.25f, 0.25f, 0};
    int counts[10] = {};
    for (int i = 0; i < 10000; ++i) ++counts[pick_glance_direction(defaults, (i + 0.5f) / 10000.0f)];
    assert(counts[5] == 0 && counts[0] == 0);
    assert(std::abs(counts[4] + counts[6] - 7442) < 5);   // side 8 / 10.75
    assert(std::abs(counts[7] + counts[8] + counts[9] - 698) < 5); // up 0.75 / 10.75
    assert(pick_glance_direction(defaults, std::numeric_limits<float>::quiet_NaN()) == 4);
    // Disposition records and which ones are shaped.
    unsigned char records[612 * 3] = {};
    *reinterpret_cast<int*>(records + 580) = -1;              // 0: random (Neutral)
    *reinterpret_cast<int*>(records + 612 + 580) = 7;         // 1: authored (Apathy-like)
    *reinterpret_cast<int*>(records + 1224 + 580) = -1;       // 2: random
    int table[6] = {0, 0, static_cast<int>(reinterpret_cast<std::uintptr_t>(records)), 0, 0, 2};
    assert(disposition_record(table, 1) == records + 612 && disposition_record(table, 9) == records && disposition_record(table, -1) == records);
    assert(glances_shaped(records, true) && !glances_shaped(records + 612, true) && glances_shaped(records + 612, false) && !glances_shaped(nullptr, false));
    // Angle scaling: 20 -> 14 degrees, distance kept.
    const float pos[3] = {0, 0, 0}, dir[3] = {2, 0, 0};
    float point[3] = {static_cast<float>(std::cos(20 / 57.29577951308232) * 25), static_cast<float>(std::sin(20 / 57.29577951308232) * 25), 0};
    assert(scale_glance_point(pos, dir, point, 0.7f));
    assert(std::fabs(std::atan2(point[1], point[0]) * 57.29577951308232 - 14) < 0.01 && std::fabs(std::hypot(point[0], point[1]) - 25) < 0.01);
    float ahead[3] = {25, 0, 0};
    assert(!scale_glance_point(pos, dir, ahead, 0.7f) && ahead[0] == 25);   // straight ahead: nothing to scale
    assert(!scale_glance_point(pos, dir, point, 1.0f));

    // Hooks through the game's thiscall ABI.
    const auto old_settings = glance;
    const auto old_server = glance_server;
    glance_server = reinterpret_cast<std::uintptr_t>(table) - 0x924980;
    glance = GlanceSettings{};
    original_eye_turn_rate = reinterpret_cast<EyeTurnRateFn>(mock_eye_turn_rate);
    original_eye_fidget_point = reinterpret_cast<EyeFidgetPointFn>(mock_eye_fidget_point);
    original_eye_hold = reinterpret_cast<EyeHoldFn>(mock_eye_hold);
    original_glance_step = reinterpret_cast<GlanceStepFn>(mock_glance_step);
    original_glance_point = reinterpret_cast<GlancePointFn>(mock_glance_point);
    game_eye_position = reinterpret_cast<EyePositionFn>(mock_eye_position);
    const auto rate = reinterpret_cast<EyeTurnRateFn>(shaped_eye_turn_rate);
    const auto fidget = reinterpret_cast<EyeFidgetPointFn>(shaped_eye_fidget_point);
    const auto hold = reinterpret_cast<EyeHoldFn>(shaped_eye_hold);
    const auto step = reinterpret_cast<GlanceStepFn>(shaped_glance_step);
    const auto glance_point = reinterpret_cast<GlancePointFn>(shaped_glance_point);
    double sum = 0;
    for (int i = 0; i < 1000; ++i) sum += rate(table, 0, 1);
    assert(std::fabs(sum - 100.0) < 0.01);                                  // random disposition: 0.3 -> 0.1, stack balanced
    assert(rate(table, 0, 0) == 0.9 && rate(table, 1, 1) == 0.2);            // normal gaze and authored glance untouched
    for (int i = 0; i < 200; ++i) { const int t = fidget(table, 0, 0); assert(t >= 1 && t <= 9 && t != 5); }
    assert(fidget(table, 0, 1) == -1 && fidget(table, 1, 0) == 110 && fidget(table, 1, 2) == 112); // one random glance; authored list kept
    float mn = 0, mx = 0;
    assert(hold(table, 0, &mn, &mx) == 5 && mn == 0.35f && mx == 0.7f);
    assert(hold(table, 1, &mn, &mx) == 5 && mn == 0.15f && mx == 0.25f);
    unsigned char npc[0x6600] = {};
    *reinterpret_cast<int*>(npc + 0x64d4) = 0;
    *reinterpret_cast<float*>(npc + 0x6584) = 5; *reinterpret_cast<float*>(npc + 0x6588) = 8;
    assert(step(npc, 41) == 42 && *reinterpret_cast<float*>(npc + 0x6584) == 8 && *reinterpret_cast<float*>(npc + 0x6588) == 14);
    *reinterpret_cast<int*>(npc + 0x64d4) = 1;                              // authored disposition: game interval kept
    *reinterpret_cast<float*>(npc + 0x6584) = 5; *reinterpret_cast<float*>(npc + 0x6588) = 8;
    assert(step(npc, 1) == 2 && *reinterpret_cast<float*>(npc + 0x6584) == 5);
    *reinterpret_cast<int*>(npc + 0x64d4) = 0;
    float out[3] = {};
    assert(glance_point(npc, 4, out) == 9 && std::fabs(std::atan2(out[1], out[0]) * 57.29577951308232 - 14) < 0.01);
    *reinterpret_cast<int*>(npc + 0x64d4) = 1;
    assert(glance_point(npc, 4, out) == 9 && std::fabs(std::atan2(out[1], out[0]) * 57.29577951308232 - 20) < 0.01);
    // keep_authored_glances=0 reshapes authored dispositions too.
    glance.keep_authored = false;
    assert(std::fabs(rate(table, 1, 1) - 0.1) < 1e-6 && fidget(table, 1, 1) == -1);
    // 0 = game value.
    glance = GlanceSettings{}; glance.turn_rate = 0; glance.hold_min = 0; glance.hold_max = 0;
    assert(rate(table, 0, 1) == 0.3 && hold(table, 0, &mn, &mx) == 5 && mn == 0.15f && mx == 0.25f);
    glance = old_settings;
    glance_server = old_server;
    std::puts("Eye glances: weighted directions (10000 picks), disposition records, angle scaling, and the five thiscall hooks (random vs authored dispositions, 0 = game value) passed.");
}
void test_release_state() {
    // Per-NPC state is cleared on map change (BLSE_MSG_LEVEL_SHUTDOWN).
    int npc = 0;
    walk_phases[3].entity = &npc; steering_states[4].npc = &npc; clamp_guards[5].npc = &npc;
    move_yaw_states[6].npc = &npc; pose_blends[2047].entity = &npc;
    reset_npc_state(nullptr, nullptr, 0, nullptr);
    assert(!walk_phases[3].entity && !steering_states[4].npc && !clamp_guards[5].npc && !move_yaw_states[6].npc && !pose_blends[2047].entity);
    // client.dll build check: a different module fails and the result is cached per module.
    const HMODULE self_module = GetModuleHandleW(nullptr);
    assert(!supported(self_module) && checked_module == self_module && !supported(self_module));
    assert(!supported(nullptr));
    // Pose slots: one per entity index up to the engine's 2048 limit.
    std::vector<unsigned char> server_npc(0x200);
    const char* npc_class = "npc_VHuman";
    *reinterpret_cast<const char**>(server_npc.data() + 0x11c) = npc_class;
    std::vector<unsigned char> list(4 + 12 * 2048);
    *reinterpret_cast<void**>(list.data() + 4 + 12 * 1600) = server_npc.data();
    *reinterpret_cast<void**>(list.data() + 4 + 12 * (1600 - 512)) = server_npc.data();
    unsigned char* list_ptr = list.data();
    const auto old_base = server_base;
    server_base = reinterpret_cast<std::uintptr_t>(&list_ptr) - 0x566458;
    unsigned char client_a[0x700] = {}, client_b[0x700] = {};
    *reinterpret_cast<int*>(client_a + 0x44) = 1600; *reinterpret_cast<int*>(client_b + 0x44) = 1600 - 512;
    unsigned handle = 0;
    assert(npc_pose_slot(client_a, handle) == &pose_blends[1600] && npc_pose_slot(client_b, handle) == &pose_blends[1088]);
    *reinterpret_cast<int*>(client_a + 0x44) = 2048;
    assert(!npc_pose_slot(client_a, handle));
    server_base = old_base;
    std::puts("Release state: map-change reset, cached build check, one pose slot per entity index passed.");
}
void test_plugin_load() {
    // An old BLSE whose host table ends before the hook functions: refused, with a log line.
    BLSE_HostAPI old_host = {};
    old_host.api_version = BLSE_API_VERSION; old_host.log = test_logger;
    old_host.struct_size = static_cast<std::uint32_t>(offsetof(BLSE_HostAPI, hook_function));
    test_log[0] = 0;
    assert(BLSE_PluginLoad(&old_host) == 0 && std::strstr(test_log, "too old"));
    // Wrong API version: refused silently.
    old_host.api_version = BLSE_API_VERSION + 1;
    assert(BLSE_PluginLoad(&old_host) == 0);
    std::puts("Plugin load: too-old host table and wrong API version refused.");
}
int ini_writes = 0;
int BLSE_CALL mock_ini_get(void*, const char*, const char* section, const char* key, char* buffer, size_t capacity, size_t* size) {
    struct Entry { const char* section; const char* key; const char* value; };
    static const Entry entries[] = {
        {"Effects", "orphan_particles", "0"}, {"NPCAnimation", "npc_steering_damping", "1"},
        {"Fixes", "npc_idle_pool", "0"},           // old INI layout still honoured
        {"NPCAnimation", "npc_steering_follow", "0.7"}, {"EyeGlances", "glances", "0"}, {"EyeGlances", "turn_rate", "2.0"},
    };
    for (const auto& e : entries)
        if (!std::strcmp(e.section, section) && !std::strcmp(e.key, key)) {
            *size = std::strlen(e.value);
            if (*size >= capacity) return 0;
            strcpy_s(buffer, capacity, e.value);
            return 1;
        }
    return 0;
}
int BLSE_CALL mock_ini_set(void*, const char*, const char*, const char*, const char*) { ++ini_writes; return 1; }
void test_settings_sections() {
    BLSE_HostAPI mock = {}; mock.ini_get = mock_ini_get; mock.ini_set = mock_ini_set;
    const auto old_host = host; host = &mock;
    assert(!enabled("orphan_particles") && enabled("npc_steering_damping") && !enabled("npc_idle_pool"));
    assert(enabled("same_tick_walk_phase") && !enabled("walking_trace", "0"));         // missing: defaults
    assert(std::fabs(setting_number("npc_steering_follow", "0.4", 0.05f, 1) - 0.7f) < 1e-6f);
    assert(setting_number("npc_steering_turn_rate", "360", 30, 3600) == 360.0f);         // trimmed from the INI: default
    assert(glance_number("glances", "1", 0, 1) == 0.0f && glance_number("turn_rate", "0.6", 0, 1) == 0.6f); // out of range: default
    assert(glance_number("enabled", "1", 0, 1) == 1.0f);
    assert(ini_writes == 0);                                                              // nothing written back
    load_glance_settings();
    assert(!glance.allow);
    // glances=0: the random-glance step returns "none"; authored lists are untouched.
    unsigned char records[612 * 2] = {};
    *reinterpret_cast<int*>(records + 580) = -1;
    *reinterpret_cast<int*>(records + 612 + 580) = 7;
    int table[6] = {0, 0, static_cast<int>(reinterpret_cast<std::uintptr_t>(records)), 0, 0, 1};
    original_eye_fidget_point = reinterpret_cast<EyeFidgetPointFn>(mock_eye_fidget_point);
    const auto fidget = reinterpret_cast<EyeFidgetPointFn>(shaped_eye_fidget_point);
    assert(fidget(table, 0, 0) == -1 && fidget(table, 1, 0) == 110);
    glance = GlanceSettings{};
    host = old_host;
    std::puts("Settings: themed sections, old [Fixes] fallback, defaults for trimmed keys, no write-back; glances=0 stops random glances.");
}
// client.dll check (1.0.1): the analyzed file, the same build edited in place by a patch, and files
// that differ too much or have another link time. Runs where the analyzed client.dll is at hand.
void test_client_layout() {
    // Headers alone, without the game's client.dll: the analyzed layout and what breaks it.
    std::vector<unsigned char> head(0x400);
    head[0] = 'M'; head[1] = 'Z'; head[0x3c] = 0x80;
    std::memcpy(head.data() + 0x80, "PE\0\0", 4);
    head[0x84] = 0x4c; head[0x85] = 0x01;
    head[0x86] = static_cast<unsigned char>(_countof(client_sections));
    std::memcpy(head.data() + 0x88, &client_timestamp, 4);
    head[0x94] = 0xe0;
    const std::size_t table = 0x98 + 0xe0;
    for (std::size_t i = 0; i < _countof(client_sections); ++i) {
        unsigned char* h = head.data() + table + 40 * i;
        const ClientSection& c = client_sections[i];
        std::memcpy(h, c.name, 8);
        std::memcpy(h + 8, &c.size, 4); std::memcpy(h + 12, &c.address, 4); std::memcpy(h + 16, &c.raw_size, 4); std::memcpy(h + 20, &c.raw_offset, 4);
    }
    assert(same_client_layout(head.data(), head.size()));
    assert(same_client_layout(head.data(), table + 40 * _countof(client_sections)));      // exactly the section table
    assert(!same_client_layout(head.data(), table + 40 * _countof(client_sections) - 1)); // the last section cut off
    assert(!same_client_layout(head.data(), 0x3f) && !same_client_layout(head.data(), 0x90));
    struct Change { std::size_t at; unsigned char value; };
    const Change changes[] = {{0, 'X'}, {0x80, 'X'}, {0x84, 0x64}, {0x86, 5}, {0x86, 3}, {0x88, 0x64}, {0x95, 0x10},
                              {table + 1, 'X'}, {table + 8, 0}, {table + 40 + 13, 0}, {table + 80 + 17, 0}, {table + 120 + 21, 0}};
    for (const auto& c : changes) {
        std::vector<unsigned char> copy = head;
        copy[c.at] = c.value;
        assert(!same_client_layout(copy.data(), copy.size()));
    }
    std::vector<unsigned char> distant = head;
    distant[0x3c] = distant[0x3d] = distant[0x3e] = distant[0x3f] = 0xff;                                 // e_lfanew far past the data
    assert(!same_client_layout(distant.data(), distant.size()));
    assert(!check_client_file(L"C:\\this-file-should-not-exist-engine-fixes.dll"));
    std::puts("client.dll layout: analyzed headers, broken and cut-off headers and a missing file passed.");
}

void test_client_check() {
    const wchar_t* analyzed = L"Game Files\\Vampire\\cl_dlls\\client.dll";
    FILE* f = nullptr;
    if (_wfopen_s(&f, analyzed, L"rb") || !f) { std::puts("client.dll check: the analyzed client.dll is not in Game Files, skipped."); return; }
    std::vector<unsigned char> data;
    unsigned char buffer[65536];
    for (std::size_t got; (got = std::fread(buffer, 1, sizeof(buffer), f)) > 0;) data.insert(data.end(), buffer, buffer + got);
    std::fclose(f);
    assert(check_client_file(analyzed));
    wchar_t dir[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, dir);
    const std::wstring copy = std::wstring(dir, n) + L"engine-fixes-client-test.dll";
    auto check_copy = [&](const std::vector<unsigned char>& bytes) {
        FILE* out = nullptr;
        assert(!_wfopen_s(&out, copy.c_str(), L"wb") && out);
        assert(std::fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size());
        std::fclose(out);
        const bool ok = check_client_file(copy.c_str());
        DeleteFileW(copy.c_str());
        return ok;
    };
    std::vector<unsigned char> edited = data;
    edited[0x18539c] ^= 0xff;                                   // where Unofficial Patch 12.0 changed code
    edited[0x2d64c4] ^= 0xff;                                   // and a name in .data
    assert(check_copy(edited));
    // 838 blocks: up to 83 (838 / 10, rounded down) may differ.
    std::vector<unsigned char> limit = data;
    for (std::size_t i = 0; i < 83; ++i) limit[0x1000 + i * 4096] ^= 0xff; // 83: the most allowed
    assert(check_copy(limit));
    std::vector<unsigned char> heavy = limit;
    heavy[0x1000 + 83 * 4096] ^= 0xff;                                    // 84: one too many
    assert(!check_copy(heavy));
    std::vector<unsigned char> longer = edited;
    longer.insert(longer.end(), 4096, 0);                                   // a block appended: it counts as changed
    assert(check_copy(longer));
    std::vector<unsigned char> shorter = edited;
    shorter.resize(shorter.size() - 300);                                   // the last block shortened
    assert(check_copy(shorter));
    std::vector<unsigned char> relinked = edited;
    const std::uint32_t pe = le32(relinked.data() + 0x3c);
    relinked[pe + 8] ^= 1;                                      // another link time
    assert(!check_copy(relinked));
    std::vector<unsigned char> grown = edited;
    const std::size_t table = pe + 24 + (grown[pe + 20] | grown[pe + 21] << 8);
    grown[table + 8] += 0x10;                                   // .text VirtualSize grown
    assert(!check_copy(grown));
    std::puts("client.dll check: analyzed file, in-place edit, 83 and 84 changed blocks, longer and shorter files, another link time, changed sections passed.");
}

// 1.0.1: an engine restart drops the cached build check and waits for client.dll again, once.
int restart_subscribes = 0;
std::uint64_t BLSE_CALL mock_subscribe(void*, const char* topic, BLSE_MessageFn, void*) {
    ++restart_subscribes;
    return std::strcmp(topic, BLSE_MSG_GAME_FRAME) == 0 ? 77 : 0;
}
void BLSE_CALL mock_log(const char*) {}
void test_restart() {
    BLSE_HostAPI mock = {};
    mock.subscribe = mock_subscribe;
    mock.log = mock_log;
    const auto old_host = host; host = &mock;
    const auto old_startup = startup_subscription;
    int npc = 0;
    walk_phases[1].entity = &npc;
    checked_module = GetModuleHandleW(nullptr); checked_result = true; pose_interpolation = true;
    startup_subscription = 0;                        // installed before: waiting again
    rearm_after_restart(nullptr, nullptr, 0, nullptr);
    assert(!checked_module && !checked_result && !pose_interpolation && !walk_phases[1].entity);
    assert(startup_subscription == 77 && restart_subscribes == 1);
    rearm_after_restart(nullptr, nullptr, 0, nullptr); // still waiting: no second subscription
    assert(startup_subscription == 77 && restart_subscribes == 1);
    startup_subscription = old_startup;
    host = old_host;
    std::puts("Engine restart: build check dropped, per-NPC state cleared, one wait for client.dll passed.");
}

int main() {
    test_client_layout();
    test_client_check();
    test_restart();
    test_release_state();
    test_plugin_load();
    test_steering();
    test_steering_body();
    test_pose_blend();
    test_eye_glance();
    test_settings_sections();
    test_yaw_setting();
    std::uint32_t model_header[110] = {};
    model_header[0] = 0x54534449; model_header[1] = 2531; model_header[2] = 0x05bbf60e;
    model_header[68] = 1;
    strcpy_s(reinterpret_cast<char*>(model_header)+12,sizeof(model_header)-12,"character/shared/male/pc_idles.mdl");
    assert(exclude_player_idle(model_header,1,1));
    assert(!exclude_player_idle(model_header,1,0));
    assert(!exclude_player_idle(model_header,9,1));
    model_header[2] = 0x716e5125;
    strcpy_s(reinterpret_cast<char*>(model_header)+12,sizeof(model_header)-12,"character/shared/female/pc_idles.mdl");
    assert(exclude_player_idle(model_header,1,1));
    model_header[101] = 1; assert(!exclude_player_idle(model_header,1,1));
    model_header[101] = 0; model_header[2] = 0; assert(!exclude_player_idle(model_header,1,1));
    assert(!exclude_player_idle(nullptr,1,1));
    std::puts("NPC idle pool: 7 known models, player, activity and modified-resource guard cases passed.");
    test_walk_phase();
    test_idle_consistency();
    original_think = reinterpret_cast<Think>(think);
    cleanup = reinterpret_cast<Cleanup>(destroy);
    run(0xffffffff, 11, false, true, true);
    run((2 << 13) | 1, 11, false, true, true);
    run((3 << 13) | 1, 11, true, true, true); // reused slot: wrong serial
    run((2 << 13) | 1, 11, true, true, false);
    run(0xffffffff, 10, false, true, false); // other network class
    run(0xffffffff, 11, false, false, false);
    std::puts("Particle cleanup: 6 cases passed.");
    original_eye_blend = reinterpret_cast<void*>(eye_trampoline_test);
    const double factors[] = {-5.0, -0.01, 0.0, 0.25, 0.5, 1.0, 1.01, 5.0, 100.0};
    for (double factor : factors) {
        const double actual = invoke_eye_blend(factor);
        const double expected = factor < 0 ? 0 : factor > 1 ? 1 : factor;
        assert(actual == expected);
        const double target = (1 - actual) * 10 + actual * 20;
        assert(target >= 10 && target <= 20);
    }
    assert(std::isnan(invoke_eye_blend(std::numeric_limits<double>::quiet_NaN())));
    for (int i = 0; i < 10000; ++i) assert(invoke_eye_blend(5.0) == 1.0);
    std::puts("Eye interpolation: endpoints, stale timing, normal blends, NaN and x87 stack checks passed.");
}

