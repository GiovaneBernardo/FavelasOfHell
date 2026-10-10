#include <doctest/doctest.h>

#include <cmath>

#include "foh/animation/animation.h"
#include "foh/animation/ik.h"
#include "foh/animation/pose.h"
#include "foh/asset_import.h"

using namespace foh;

namespace {

constexpr f32 kPi = 3.14159265358979f;

// A character with legs: Hips(0) > Spine(1), LeftUpLeg(2) > LeftLeg(3) > LeftFoot(4), RightUpLeg(5) > RightLeg(6) > RightFoot(7).
ImportedSkeleton legsSource() {
    ImportedSkeleton sk;
    sk.key = "skeleton";
    sk.name = "legs";
    sk.jointNames = {"mixamorig:Hips", "mixamorig:Spine", "mixamorig:LeftUpLeg", "mixamorig:LeftLeg", "mixamorig:LeftFoot",
                     "mixamorig:RightUpLeg", "mixamorig:RightLeg", "mixamorig:RightFoot"};
    sk.parents = {-1, 0, 0, 2, 3, 0, 5, 6};
    sk.restPosition = {Vec3(0, 1, 0),     Vec3(0, 0.2f, 0),     Vec3(0.1f, 0, 0),   Vec3(0, -0.45f, 0),
                       Vec3(0, -0.45f, 0), Vec3(-0.1f, 0, 0),   Vec3(0, -0.45f, 0), Vec3(0, -0.45f, 0)};
    sk.restRotation.assign(8, Quat(1, 0, 0, 0));
    sk.restScale.assign(8, Vec3(1));
    sk.inverseBind.assign(8, Mat4(1.0f));
    sk.hasInverseBind.assign(8, 0);
    return sk;
}

AssetId id(u64 n) { return AssetId{0xB0B, n}; }

struct Legs {
    AnimationLibrary library;
    SkeletonAsset skeleton;
    AssetId skeletonId = id(1);
    Legs() {
        skeleton = CookSkeleton(legsSource());
        library.setSkeleton(skeletonId, skeleton);
    }
    AssetId add(u64 n, const ImportedClip& clip, const char* loop = "auto") {
        ImportSettings settings;
        settings.clipLoop = loop;
        library.setClip(id(n), CookClip(clip, legsSource(), settings));
        return id(n);
    }
};

// A clip over the legs skeleton: `lift(t)` raises both feet by that many meters (a jump has both feet in the air).
template <class LiftFn>
ImportedClip liftClip(const char* name, f32 seconds, LiftFn lift, bool sway = true) {
    ImportedClip clip;
    clip.name = name;
    clip.key = name;
    clip.skeleton = 0;
    clip.frameRate = 30.0f;
    clip.frameCount = static_cast<u32>(seconds * 30.0f) + 1;
    clip.tracks.resize(8);
    for (u32 f = 0; f < clip.frameCount; ++f) {
        const f32 t = static_cast<f32>(f) / 30.0f;
        const f32 l = lift(t);
        clip.tracks[0].translation.push_back(Vec3(0, 1.0f + l + (sway ? 0.004f * std::sin(t * 2.0f) : 0.0f), 0));
        clip.tracks[0].rotation.push_back(Quat(1, 0, 0, 0));
        for (u32 j : {4u, 7u}) {
            // the foot rises relative to the hips by l (the hips rose by l as well, so the foot is lifted by 2l over rest)
            clip.tracks[j].translation.push_back(Vec3(j == 4 ? 0.0f : 0.0f, -0.45f - l + l * 2.0f, 0));
            clip.tracks[j].rotation.push_back(Quat(1, 0, 0, 0));
        }
    }
    return clip;
}

Vec3 modelPosition(const SkeletonAsset& skeleton, std::span<const JointPose> pose, u32 joint) {
    std::vector<SkinMatrix> m(skeleton.jointCount());
    ComputeModelPose(skeleton, pose, m);
    return Vec3(m[joint].row0.w, m[joint].row1.w, m[joint].row2.w);
}

} // namespace

TEST_CASE("a crouch lowers the hips and folds the knees while the feet stay where the animation put them") {
    Legs legs;
    std::vector<JointPose> pose(8);
    SetRestPose(legs.skeleton, pose);
    // put the left foot a step forward and lifted, the right one flat: a walking pose
    pose[4].position = Vec3(0, -0.40f, 0.0f);
    pose[3].position = Vec3(0, -0.45f, 0.0f);
    pose[2].rotation = glm::angleAxis(-0.35f, Vec3(1, 0, 0)); // thigh swung forward

    const RigInfo rig = FindRig(legs.skeleton);
    REQUIRE(rig.hasLegs());
    CHECK(rig.spine[0] == 1);

    const Vec3 footL = modelPosition(legs.skeleton, pose, 4), footR = modelPosition(legs.skeleton, pose, 7);
    const Vec3 hips = modelPosition(legs.skeleton, pose, 0);
    std::vector<JointPose> crouched = pose;
    ApplyCrouch(legs.skeleton, rig, crouched, 1.0f);

    CHECK(glm::length(modelPosition(legs.skeleton, crouched, 4) - footL) < 0.01f);
    CHECK(glm::length(modelPosition(legs.skeleton, crouched, 7) - footR) < 0.01f);
    const Vec3 hipsNow = modelPosition(legs.skeleton, crouched, 0);
    CHECK(hipsNow.y == doctest::Approx(hips.y - 0.42f).epsilon(0.02));
    CHECK(hipsNow.z < hips.z - 0.05f); // moved back
    // the right knee bends forward (+Z is the character's front), clearly in front of the line hip -> foot
    const Vec3 kneeR = modelPosition(legs.skeleton, crouched, 6);
    CHECK(kneeR.z > footR.z + 0.15f);
    // leg bones keep their length (no stretching)
    const f32 thigh = glm::length(modelPosition(legs.skeleton, crouched, 6) - modelPosition(legs.skeleton, crouched, 5));
    CHECK(thigh == doctest::Approx(0.45f).epsilon(0.01));

    // amount 0 changes nothing, and a half crouch is in between
    std::vector<JointPose> none = pose;
    ApplyCrouch(legs.skeleton, rig, none, 0.0f);
    CHECK(glm::length(modelPosition(legs.skeleton, none, 0) - hips) < 1e-5f);
    std::vector<JointPose> half = pose;
    ApplyCrouch(legs.skeleton, rig, half, 0.5f);
    CHECK(modelPosition(legs.skeleton, half, 0).y == doctest::Approx(hips.y - 0.21f).epsilon(0.05));
    CHECK(glm::length(modelPosition(legs.skeleton, half, 4) - footL) < 0.01f);
}

TEST_CASE("two-bone IK reaches a target, clamps an unreachable one and keeps the foot orientation") {
    Legs legs;
    std::vector<JointPose> pose(8);
    SetRestPose(legs.skeleton, pose);
    const Quat footBefore = glm::angleAxis(0.3f, Vec3(0, 1, 0));
    pose[4].rotation = footBefore;
    const Vec3 target(0.1f, 0.35f, 0.25f);
    REQUIRE(SolveTwoBoneLeg(legs.skeleton, pose, 2, 3, 4, target, Vec3(0, 0, 1)));
    CHECK(glm::length(modelPosition(legs.skeleton, pose, 4) - target) < 0.005f);

    std::vector<JointPose> far(8);
    SetRestPose(legs.skeleton, far);
    SolveTwoBoneLeg(legs.skeleton, far, 2, 3, 4, Vec3(0.1f, -3.0f, 0.0f), Vec3(0, 0, 1));
    const Vec3 hip = modelPosition(legs.skeleton, far, 2), foot = modelPosition(legs.skeleton, far, 4);
    CHECK(glm::length(foot - hip) == doctest::Approx(0.9f).epsilon(0.01)); // fully stretched toward it, not beyond
    CHECK(SolveTwoBoneLeg(legs.skeleton, far, 0, 3, 4, target, Vec3(0, 0, 1)) == false); // not a chain
}

TEST_CASE("the airborne interval of a jump clip is found from the feet") {
    // standing for 0.5 s, feet off the ground from 0.5 to 0.9 s, standing again until 1.8 s
    auto lift = [](f32 t) { return t > 0.5f && t < 0.9f ? 0.2f : 0.0f; };
    Legs legs;
    const ClipAsset& clip = *(legs.add(10, liftClip("jump", 1.8f, lift), "once"), legs.library.clip(id(10)));
    REQUIRE(clip.hasAir);
    CHECK(clip.airStart == doctest::Approx(0.5f).epsilon(0.1));
    CHECK(clip.airEnd == doctest::Approx(0.9f).epsilon(0.1));

    // a clip whose feet never leave the ground has no air interval
    legs.add(11, liftClip("idle", 1.8f, [](f32) { return 0.0f; }), "once");
    CHECK_FALSE(legs.library.clip(id(11))->hasAir);
}

namespace {

struct JumpWorld {
    App app{{.workerThreads = 0}};
    Legs legs;
    LocomotionClips clips;
    Entity character{};

    JumpWorld() {
        RegisterAnimation(app);
        app.insertResource(AnimationLink{&legs.library});
        clips.idleClip = legs.add(20, liftClip("idle", 2.0f, [](f32) { return 0.0f; }), "loop");
        clips.jumpClip = legs.add(21, liftClip("jump", 1.8f, [](f32 t) { return t > 0.5f && t < 0.9f ? 0.2f : 0.0f; }), "once");
        Animator animator;
        animator.skeleton = legs.skeletonId;
        character = app.world().spawn(Transform{}, animator, SkinPalette{}, clips, LocomotionControl{}, LocomotionState{});
    }
    void step(f32 dt = 1.0f / 60.0f) { app.update(dt); }
    LocomotionControl& control() { return *app.world().get<LocomotionControl>(character); }
    const LocomotionState& state() { return *app.world().get<LocomotionState>(character); }
};

} // namespace

TEST_CASE("a jump follows the clip's landmarks: wind-up, the take-off event, an air pose stretched over the air time, a landing") {
    JumpWorld w;
    for (int i = 0; i < 20; ++i) w.step();
    CHECK(w.state().mode == static_cast<u32>(LocomotionMode::Move));

    w.control().jump = true;
    bool tookOff = false;
    int takeOffFrame = -1;
    for (int i = 0; i < 120 && !tookOff; ++i) {
        w.step();
        if (w.state().takeOffNow) {
            tookOff = true;
            takeOffFrame = i;
        }
    }
    REQUIRE(tookOff);
    CHECK(w.state().mode == static_cast<u32>(LocomotionMode::Jump));
    CHECK(w.state().jumpPhase == static_cast<u32>(JumpPhase::Air));
    // the 0.5 s wind-up of the clip is played faster so the body leaves the ground quickly (the target is 0.2 s)
    CHECK(takeOffFrame < 16);
    CHECK_FALSE(w.control().jump); // the request was consumed

    // gameplay: leaves the ground now, lands after the expected air time
    w.control().grounded = false;
    w.control().expectedAirTime = 0.6f;
    f32 lastClipTime = w.state().jumpClipTime;
    int airFrames = 0;
    while (w.state().jumpPhase == static_cast<u32>(JumpPhase::Air) && airFrames < 200) {
        w.step();
        ++airFrames;
        if (w.state().jumpPhase == static_cast<u32>(JumpPhase::Air)) {
            CHECK(w.state().jumpClipTime >= lastClipTime - 1e-4f); // the air pose only moves forward
            lastClipTime = w.state().jumpClipTime;
        }
        if (airFrames == 30) w.control().grounded = true; // lands 0.5 s after take-off, a little early
    }
    CHECK(w.state().jumpPhase == static_cast<u32>(JumpPhase::Land));
    CHECK(airFrames >= 29);
    CHECK(airFrames < 40);

    int landFrames = 0;
    while (w.state().mode == static_cast<u32>(LocomotionMode::Jump) && landFrames < 200) {
        w.step();
        ++landFrames;
    }
    CHECK(w.state().mode == static_cast<u32>(LocomotionMode::Move)); // back to locomotion
    CHECK(landFrames < 120);
}

TEST_CASE("running on cuts the landing short, and a fall off a ledge plays the air pose") {
    JumpWorld w;
    for (int i = 0; i < 10; ++i) w.step();
    // walked off a ledge: no jump request, the ground is simply gone
    w.control().grounded = false;
    w.step();
    CHECK(w.state().mode == static_cast<u32>(LocomotionMode::Jump));
    CHECK(w.state().jumpPhase == static_cast<u32>(JumpPhase::Air));
    CHECK_FALSE(w.state().takeOffNow); // nothing to spring from
    for (int i = 0; i < 20; ++i) w.step();
    w.control().grounded = true;
    w.control().velocity = Vec3(0, 0, 4.0f); // the player keeps running
    int frames = 0;
    while (w.state().mode == static_cast<u32>(LocomotionMode::Jump) && frames < 200) {
        w.step();
        ++frames;
    }
    CHECK(w.state().mode == static_cast<u32>(LocomotionMode::Move));
    CHECK(frames < 40); // much shorter than the clip's own landing
}

TEST_CASE("without gameplay physics a jump still plays through in the expected time") {
    JumpWorld w;
    for (int i = 0; i < 5; ++i) w.step();
    w.control().jump = true;
    w.control().expectedAirTime = 0.5f;
    int frames = 0;
    bool left = false;
    while (frames < 400) {
        w.step();
        ++frames;
        left = left || w.state().mode == static_cast<u32>(LocomotionMode::Jump);
        if (left && w.state().mode == static_cast<u32>(LocomotionMode::Move)) break;
    }
    CHECK(left);
    CHECK(w.state().mode == static_cast<u32>(LocomotionMode::Move));
    CHECK(frames < 300);
}

TEST_CASE("a procedural crouch eases in and out of the pose, and a preview clip replaces locomotion") {
    JumpWorld w;
    for (int i = 0; i < 10; ++i) w.step();
    CHECK(w.state().crouch01 == doctest::Approx(0.0f).epsilon(0.01));
    w.control().crouch = true;
    for (int i = 0; i < 60; ++i) w.step();
    CHECK(w.state().crouch01 > 0.95f);
    w.control().crouch = false;
    for (int i = 0; i < 90; ++i) w.step();
    CHECK(w.state().crouch01 < 0.02f);

    w.control().previewClip = w.clips.jumpClip;
    w.control().previewPlay = false;
    w.control().previewTime = 0.7f;
    w.step();
    CHECK(w.state().mode == static_cast<u32>(LocomotionMode::Preview));
    w.control().previewPlay = true;
    const f32 before = w.control().previewTime;
    w.step();
    CHECK(w.control().previewTime > before);
    w.control().previewClip = AssetId{};
    w.step();
    CHECK(w.state().mode == static_cast<u32>(LocomotionMode::Move));
}

TEST_CASE("idle breaks play now and then while standing still, never while moving, and give way to input") {
    App app({.workerThreads = 0});
    Legs legs;
    RegisterAnimation(app);
    app.insertResource(AnimationLink{&legs.library});
    LocomotionClips clips;
    clips.idleClip = legs.add(30, liftClip("idle", 2.0f, [](f32) { return 0.0f; }), "loop");
    clips.idleBreak0Clip = legs.add(31, liftClip("break", 2.5f, [](f32 t) { return 0.02f * std::sin(t * 3.0f); }, false), "once");
    clips.walkClip = legs.add(32, liftClip("walk", 1.0f, [](f32) { return 0.0f; }), "loop");
    Animator animator;
    animator.skeleton = legs.skeletonId;
    const Entity e = app.world().spawn(Transform{}, animator, SkinPalette{}, clips, LocomotionControl{}, LocomotionState{});
    auto state = [&]() -> const LocomotionState& { return *app.world().get<LocomotionState>(e); };

    bool sawBreak = false;
    bool resumed = false;
    for (int i = 0; i < 60 * 40 && !resumed; ++i) { // up to 40 seconds standing
        app.update(1.0f / 60.0f);
        const bool inBreak = state().mode == static_cast<u32>(LocomotionMode::IdleBreak);
        if (inBreak) sawBreak = true;
        if (sawBreak && !inBreak) resumed = true;
    }
    CHECK(sawBreak);
    CHECK(resumed);
    CHECK(state().mode == static_cast<u32>(LocomotionMode::Move));

    // walking resets the timer: no break while moving
    app.world().get<LocomotionControl>(e)->velocity = Vec3(0, 0, 1.6f);
    bool brokeWhileMoving = false;
    for (int i = 0; i < 60 * 30; ++i) {
        app.update(1.0f / 60.0f);
        brokeWhileMoving = brokeWhileMoving || state().mode == static_cast<u32>(LocomotionMode::IdleBreak);
    }
    CHECK_FALSE(brokeWhileMoving);

    // a break in progress is cut by input
    app.world().get<LocomotionControl>(e)->velocity = Vec3(0.0f);
    bool inBreak = false;
    for (int i = 0; i < 60 * 40 && !inBreak; ++i) {
        app.update(1.0f / 60.0f);
        inBreak = state().mode == static_cast<u32>(LocomotionMode::IdleBreak);
    }
    REQUIRE(inBreak);
    for (int i = 0; i < 12; ++i) app.update(1.0f / 60.0f); // past the minimum time
    app.world().get<LocomotionControl>(e)->velocity = Vec3(0, 0, 1.6f);
    for (int i = 0; i < 6; ++i) app.update(1.0f / 60.0f);
    CHECK(state().mode == static_cast<u32>(LocomotionMode::Move));
}

TEST_CASE("clip segments: parsing, cutting a take, and closing a loop at the best matching frame") {
    std::vector<std::string> errors;
    const auto segments = ParseClipSegments("idle (mocap)=14.0-22.5:loop; break 1 = 4-9.25 ;bad;x=5-2;=1-2", &errors);
    REQUIRE(segments.size() == 2);
    CHECK(segments[0].name == "idle (mocap)");
    CHECK(segments[0].start == doctest::Approx(14.0f));
    CHECK(segments[0].end == doctest::Approx(22.5f));
    CHECK(segments[0].loop);
    CHECK(segments[1].name == "break 1");
    CHECK_FALSE(segments[1].loop);
    CHECK(errors.size() == 3);

    // a 10 s take of a 2 second cycle (so frames t and t+2 are identical), cut from 1.0 to 5.3 s: the loop end moves to a
    // whole cycle (5.0 s or 5.0 +- k * 2 within 0.4 s)
    ImportedClip take = liftClip("take", 10.0f, [](f32 t) { return 0.1f * std::sin(kPi * t); }, false);
    ImportSettings settings;
    settings.clipSegments = "cycle=1.0-5.3:loop;plain=2.0-4.0";
    const std::vector<ImportedClip> cut = SliceClips({take}, settings);
    REQUIRE(cut.size() == 2);
    CHECK(cut[0].name == "cycle");
    CHECK(cut[0].loop);
    // 1.0 -> a whole number of cycles later: 3.0 or 5.0; the candidate window around 5.3 s only holds 5.0
    CHECK(static_cast<f32>(cut[0].frameCount - 1) / 30.0f == doctest::Approx(4.0f).epsilon(0.02));
    CHECK(cut[1].frameCount == 61);
    CHECK_FALSE(cut[1].loop);
    // the cut keeps the data: first frame of the segment is frame 30 of the take
    CHECK(cut[1].tracks[0].translation.front() == take.tracks[0].translation[60]);

    // no segments: the take is returned untouched
    ImportSettings none;
    CHECK(SliceClips({take}, none).size() == 1);
}
