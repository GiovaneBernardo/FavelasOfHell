#include <doctest/doctest.h>

#include <cmath>

#include "foh/animation/animation.h"
#include "foh/animation/inertialization.h"
#include "foh/animation/pose.h"
#include "foh/animation/spring.h"
#include "foh/asset_import.h"

using namespace foh;

namespace {

constexpr f32 kPi = 3.14159265358979f;

ImportedSkeleton skeletonSource() {
    ImportedSkeleton sk;
    sk.key = "skeleton";
    sk.name = "walker";
    sk.jointNames = {"mixamorig:Hips", "mixamorig:LeftFoot", "mixamorig:RightFoot"};
    sk.parents = {-1, 0, 0};
    sk.restPosition = {Vec3(0, 1, 0), Vec3(0.1f, -0.9f, 0), Vec3(-0.1f, -0.9f, 0)};
    sk.restRotation = {Quat(1, 0, 0, 0), Quat(1, 0, 0, 0), Quat(1, 0, 0, 0)};
    sk.restScale = {Vec3(1), Vec3(1), Vec3(1)};
    sk.inverseBind.assign(3, Mat4(1.0f));
    sk.hasInverseBind.assign(3, 0);
    return sk;
}

// A one second gait cycle moving along `direction` (unit, XZ) at `speed`, turning at `yawRate`. Speed 0 and no sway: an idle.
ImportedClip gaitClip(const char* name, Vec2 direction, f32 speed, f32 yawRate = 0.0f) {
    ImportedClip clip;
    clip.name = name;
    clip.key = name;
    clip.skeleton = 0;
    clip.frameRate = 30.0f;
    clip.frameCount = 31;
    clip.tracks.resize(3);
    for (u32 f = 0; f < clip.frameCount; ++f) {
        const f32 t = static_cast<f32>(f) / 30.0f;
        const Quat yaw = glm::angleAxis(yawRate * t, Vec3(0, 1, 0));
        const Vec3 move = Vec3(direction.x, 0, direction.y) * (speed * t);
        const f32 sway = speed > 0.0f ? 0.02f * std::sin(2 * kPi * t) : 0.0f;
        clip.tracks[0].translation.push_back(Vec3(sway, 1 + 0.005f * std::cos(4 * kPi * t), 0) + move);
        clip.tracks[0].rotation.push_back(yaw);
        clip.tracks[1].translation.push_back(Vec3(0.1f, -0.9f + 0.1f * std::max(0.0f, std::sin(2 * kPi * t)), 0));
        clip.tracks[2].translation.push_back(Vec3(-0.1f, -0.9f + 0.1f * std::max(0.0f, -std::sin(2 * kPi * t)), 0));
        clip.tracks[1].rotation.push_back(Quat(1, 0, 0, 0));
        clip.tracks[2].rotation.push_back(Quat(1, 0, 0, 0));
    }
    return clip;
}

AssetId id(u64 n) { return AssetId{0xA11CE, n}; }

struct Rig {
    AnimationLibrary library;
    SkeletonAsset skeleton;
    AssetId skeletonId = id(1);

    Rig() {
        skeleton = CookSkeleton(skeletonSource());
        library.setSkeleton(skeletonId, skeleton);
    }
    AssetId add(u64 n, const ImportedClip& clip, const char* loop = "auto") {
        ImportSettings settings;
        settings.clipLoop = loop;
        library.setClip(id(n), CookClip(clip, skeletonSource(), settings));
        return id(n);
    }
};

} // namespace

TEST_CASE("sampling a cooked clip reproduces the source pose") {
    Rig rig;
    const ImportedClip source = gaitClip("walk", Vec2(0, 1), 1.6f);
    const AssetId clipId = rig.add(10, source);
    const ClipAsset& clip = *rig.library.clip(clipId);
    const ClipBinding* binding = rig.library.binding(rig.skeletonId, clipId);
    REQUIRE(binding);
    REQUIRE(binding->trackJoint.size() == clip.tracks.size());

    std::vector<JointPose> pose(3);
    for (u32 f = 0; f < 31; ++f) {
        SetRestPose(rig.skeleton, pose);
        SampleClip(clip, *binding, static_cast<f32>(f) / 30.0f, pose);
        // hips height and the feet are untouched by the root motion split
        CHECK(pose[0].position.y == doctest::Approx(source.tracks[0].translation[f].y).epsilon(0.002));
        CHECK(pose[1].position.y == doctest::Approx(source.tracks[1].translation[f].y).epsilon(0.002));
        CHECK(std::abs(pose[0].position.z) < 0.002f); // forward travel left the pose
    }
    // between samples it interpolates
    SetRestPose(rig.skeleton, pose);
    SampleClip(clip, *binding, 0.5f / 30.0f, pose);
    CHECK(pose[0].position.y == doctest::Approx(1.0f + 0.005f * std::cos(4 * kPi * 0.5f / 30.0f)).epsilon(0.003));
}

TEST_CASE("a looping clip wraps, a one-shot clamps") {
    Rig rig;
    ClipAsset loop = *rig.library.clip(rig.add(10, gaitClip("walk", Vec2(0, 1), 1.6f)));
    CHECK(WrapClipTime(loop, 1.25f) == doctest::Approx(0.25f).epsilon(1e-3));
    CHECK(WrapClipTime(loop, -0.25f) == doctest::Approx(0.75f).epsilon(1e-3));
    ClipAsset once = *rig.library.clip(rig.add(11, gaitClip("walk", Vec2(0, 1), 1.6f), "once"));
    CHECK(WrapClipTime(once, 1.25f) == doctest::Approx(1.0f).epsilon(1e-3));
}

TEST_CASE("blending is a normalised weighted sum with shortest-arc rotations") {
    std::vector<JointPose> a(1), b(1), acc(1), out(1);
    a[0].position = Vec3(0, 0, 0);
    b[0].position = Vec3(2, 0, 0);
    a[0].rotation = glm::angleAxis(0.2f, Vec3(0, 1, 0));
    b[0].rotation = -glm::angleAxis(0.6f, Vec3(0, 1, 0)); // the same rotation as +q: must not blend through the long way
    BlendInit(acc, a, 0.25f);
    BlendAdd(acc, b, 0.75f);
    BlendFinish(acc, 1.0f);
    CHECK(acc[0].position.x == doctest::Approx(1.5f));
    const f32 angle = 2.0f * std::atan2(acc[0].rotation.y, std::abs(acc[0].rotation.w));
    CHECK(angle == doctest::Approx(0.2f * 0.25f + 0.6f * 0.75f).epsilon(0.01));
    CHECK(glm::length(acc[0].rotation) == doctest::Approx(1.0f));
    BlendTwo(a, b, 0.5f, out);
    CHECK(out[0].position.x == doctest::Approx(1.0f));
}

TEST_CASE("springs are exact, stable for any time step and never overshoot") {
    f32 x = 1.0f, v = 0.0f;
    spring::step(x, v, 0.0f, 0.2f, 0.2f);
    CHECK(x == doctest::Approx(0.5f).epsilon(0.1)); // one half-life covers about half the way (starting at rest it lags a little)
    f32 xa = 1.0f, va = 0.0f, xb = 1.0f, vb = 0.0f;
    for (int i = 0; i < 100; ++i) spring::step(xa, va, 0.0f, 0.3f, 0.01f);
    spring::step(xb, vb, 0.0f, 0.3f, 1.0f);
    CHECK(xa == doctest::Approx(xb).epsilon(1e-4)); // independent of the step size
    CHECK(va == doctest::Approx(vb).epsilon(1e-3));
    f32 big = 1.0f, bigV = 0.0f;
    spring::step(big, bigV, 0.0f, 0.1f, 1000.0f);
    CHECK(std::isfinite(big));
    CHECK(std::abs(big) < 1e-6f);

    // the decaying offset starts where it was told and ends at zero
    CHECK(spring::decayValue(0.3f, -1.0f, 0.1f, 0.0f) == doctest::Approx(0.3f));
    CHECK(std::abs(spring::decayValue(0.3f, -1.0f, 0.1f, InertializeDuration(0.1f))) < 1e-4f);
    // a start velocity that would swing it across zero is limited
    const f32 v0 = spring::clampStartVelocity(0.3f, -50.0f, 0.1f);
    for (f32 t = 0.0f; t < 1.0f; t += 0.01f) CHECK(spring::decayValue(0.3f, v0, 0.1f, t) >= -1e-6f);
    CHECK(AngleDelta(3.0f, -3.0f) == doctest::Approx(2.0f * kPi - 6.0f));
}

TEST_CASE("inertialization keeps position and velocity continuous across a pose jump, then converges") {
    std::vector<JointHistory> h(1);
    std::vector<JointPose> prev(1), prevPrev(1), targetNow(1), targetBefore(1);
    const f32 dt = 1.0f / 60.0f;
    // the character was moving at 1 m/s along x, now the new animation says it should be at x = 5, standing still
    prevPrev[0].position = Vec3(0.0f, 0, 0);
    prev[0].position = Vec3(dt, 0, 0);
    RecordHistory(h, prevPrev, dt, false);
    RecordHistory(h, prev, dt, true);
    CHECK(h[0].velPos.x == doctest::Approx(1.0f));
    targetBefore[0].position = Vec3(5.0f, 0, 0);
    targetNow[0].position = Vec3(5.0f, 0, 0);

    InertializeStart(h, targetNow, targetBefore, dt, 0.1f);
    std::vector<JointPose> out = targetNow;
    InertializeApply(h, 0.0f, 0.1f, out);
    CHECK(out[0].position.x == doctest::Approx(prev[0].position.x).epsilon(1e-4)); // no pop on the first frame

    f32 last = out[0].position.x;
    f32 elapsed = 0.0f;
    bool monotone = true;
    for (int i = 0; i < 200; ++i) {
        elapsed += dt;
        out = targetNow;
        InertializeApply(h, elapsed, 0.1f, out);
        if (out[0].position.x < last - 1e-5f) monotone = false;
        last = out[0].position.x;
    }
    CHECK(monotone);                                            // never swings back
    CHECK(last == doctest::Approx(5.0f).epsilon(1e-4));         // fully on the new animation
    CHECK(last <= 5.0f + 1e-4f);                                // and did not overshoot it

    // rotations: the same through the rotation vector
    std::vector<JointHistory> r(1);
    std::vector<JointPose> a(1), b(1), c(1);
    a[0].rotation = glm::angleAxis(1.0f, Vec3(0, 1, 0));
    b[0].rotation = glm::angleAxis(-1.0f, Vec3(0, 1, 0));
    RecordHistory(r, a, dt, false);
    InertializeStart(r, b, b, dt, 0.1f);
    c = b;
    InertializeApply(r, 0.0f, 0.1f, c);
    CHECK(glm::abs(glm::dot(c[0].rotation, a[0].rotation)) == doctest::Approx(1.0f).epsilon(1e-4));
    c = b;
    InertializeApply(r, InertializeDuration(0.1f), 0.1f, c);
    CHECK(glm::abs(glm::dot(c[0].rotation, b[0].rotation)) == doctest::Approx(1.0f).epsilon(1e-4));

    CHECK(glm::length(QuatLog(QuatExp(Vec3(0.3f, -0.2f, 0.5f))) - Vec3(0.3f, -0.2f, 0.5f)) < 1e-5f);
}

TEST_CASE("the blend space is measured from the clips and blends by direction and speed") {
    Rig rig;
    LocomotionClips clips;
    clips.idleClip = rig.add(20, gaitClip("idle", Vec2(0, 0), 0.0f));
    clips.walkClip = rig.add(21, gaitClip("walk", Vec2(0, 1), 1.6f));
    clips.runClip = rig.add(22, gaitClip("run", Vec2(0, 1), 4.2f));
    clips.walkLeftClip = rig.add(23, gaitClip("walkLeft", Vec2(1, 0), 1.7f));   // +X is the character's left
    clips.walkRightClip = rig.add(24, gaitClip("walkRight", Vec2(-1, 0), 1.7f));
    const MoveSet* set = rig.library.prepareMoveSet(clips);
    REQUIRE(set);
    CHECK(set->entries.size() == 4);
    REQUIRE(set->rings.size() == 2); // walk-ish and run
    CHECK(set->rings[0].entries.size() == 3);
    CHECK(set->rings[1].entries.size() == 1);
    CHECK(set->rings[0].speed == doctest::Approx(1.667f).epsilon(0.02));

    auto weightOf = [&](const MoveWeights& w, const AssetId& clip) {
        f32 sum = 0.0f;
        for (u32 i = 0; i < w.count; ++i)
            if (set->entries[w.items[i].entry].clip == clip) sum += w.items[i].weight;
        return sum;
    };
    auto total = [](const MoveWeights& w) { return w.idleWeight + w.moveWeight; };

    MoveWeights w = EvaluateMoveSet(*set, Vec2(0, 0));
    CHECK(w.idleWeight == doctest::Approx(1.0f));
    w = EvaluateMoveSet(*set, Vec2(0, 0.8f)); // half the walk speed: half idle
    CHECK(w.idleWeight == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(total(w) == doctest::Approx(1.0f));
    w = EvaluateMoveSet(*set, Vec2(0, 1.667f));
    CHECK(weightOf(w, clips.walkClip) == doctest::Approx(1.0f).epsilon(0.02));
    w = EvaluateMoveSet(*set, Vec2(0, 2.9f)); // between the walk and run rings
    CHECK(weightOf(w, clips.walkClip) + weightOf(w, clips.runClip) == doctest::Approx(1.0f));
    CHECK(weightOf(w, clips.runClip) > 0.3f);
    CHECK(weightOf(w, clips.runClip) < 0.7f);
    w = EvaluateMoveSet(*set, Vec2(5.0f, 0.0f) * 0.4f + Vec2(0, 0.0f)); // 2 m/s pure left: strafe left, no forward
    CHECK(weightOf(w, clips.walkLeftClip) > 0.6f);
    const f32 c = 1.667f * 0.7071f;
    w = EvaluateMoveSet(*set, Vec2(c, c)); // forward-left diagonal: half and half
    CHECK(weightOf(w, clips.walkClip) == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(weightOf(w, clips.walkLeftClip) == doctest::Approx(0.5f).epsilon(0.05));
    // backwards, where no clip exists: the nearer strafe wins instead of a left/right average
    w = EvaluateMoveSet(*set, Vec2(0.3f, -1.6f));
    CHECK(weightOf(w, clips.walkLeftClip) > 0.85f);
    CHECK(total(w) == doctest::Approx(1.0f));
    // weights vary continuously around the circle
    f32 previous = -1.0f;
    f32 largestStep = 0.0f;
    for (int i = 0; i <= 720; ++i) {
        const f32 a = 2.0f * kPi * static_cast<f32>(i) / 720.0f;
        w = EvaluateMoveSet(*set, Vec2(std::sin(a), std::cos(a)) * 1.667f);
        const f32 now = weightOf(w, clips.walkClip);
        if (previous >= 0.0f) largestStep = std::max(largestStep, std::abs(now - previous));
        previous = now;
        CHECK(total(w) == doctest::Approx(1.0f).epsilon(1e-4));
    }
    CHECK(largestStep < 0.12f);
}

TEST_CASE("turn clips are picked by the heading change they produce") {
    Rig rig;
    LocomotionClips clips;
    clips.turnLeft90Clip = rig.add(30, gaitClip("l90", Vec2(0, 0), 0.0f, kPi / 2), "once");
    clips.turnLeftClip = rig.add(31, gaitClip("l180", Vec2(0, 0), 0.0f, kPi), "once");
    clips.turnRight90Clip = rig.add(32, gaitClip("r90", Vec2(0, 0), 0.0f, -kPi / 2), "once");
    const MoveSet* set = rig.library.prepareMoveSet(clips);
    REQUIRE(set);
    REQUIRE(set->turns.size() == 3);
    CHECK(PickTurn(*set, 1.4f)->clip == clips.turnLeft90Clip);
    CHECK(PickTurn(*set, 2.9f)->clip == clips.turnLeftClip);
    CHECK(PickTurn(*set, -1.6f)->clip == clips.turnRight90Clip);
}

namespace {

struct World3 {
    App app{{.workerThreads = 2}};
    Rig rig;
    LocomotionClips clips;
    Entity character{};

    World3() {
        RegisterAnimation(app);
        app.insertResource(AnimationLink{&rig.library});
        clips.idleClip = rig.add(20, gaitClip("idle", Vec2(0, 0), 0.0f));
        clips.walkClip = rig.add(21, gaitClip("walk", Vec2(0, 1), 1.6f));
        clips.runClip = rig.add(22, gaitClip("run", Vec2(0, 1), 4.2f));
        clips.turnLeft90Clip = rig.add(30, gaitClip("l90", Vec2(0, 0), 0.0f, kPi / 2), "once");
        clips.turnRight90Clip = rig.add(32, gaitClip("r90", Vec2(0, 0), 0.0f, -kPi / 2), "once");
        Animator animator;
        animator.skeleton = rig.skeletonId;
        character = app.world().spawn(Transform{}, animator, SkinPalette{}, clips, LocomotionControl{}, LocomotionState{});
    }
    void run(int frames, f32 dt = 1.0f / 60.0f) {
        for (int i = 0; i < frames; ++i) app.update(dt);
    }
    LocomotionControl& control() { return *app.world().get<LocomotionControl>(character); }
    const Transform& transform() { return *app.world().get<Transform>(character); }
    const LocomotionState& state() { return *app.world().get<LocomotionState>(character); }
};

f32 headingOf(const Transform& t) {
    const Vec3 f = t.rotation * Vec3(0, 0, 1);
    return std::atan2(f.x, f.z);
}

} // namespace

TEST_CASE("a character walks: smoothed velocity moves it, stride matching sets the gait, the palette is filled") {
    World3 w;
    w.control().velocity = Vec3(0, 0, 1.6f);
    w.run(180); // three seconds
    const f32 z = w.transform().position.z;
    CHECK(z > 4.2f);
    CHECK(z < 4.8f); // 1.6 * 3 minus the acceleration lag
    CHECK(w.state().velocity.z == doctest::Approx(1.6f).epsilon(0.01));
    CHECK(w.state().phase != 0.0f);
    CHECK(std::abs(headingOf(w.transform())) < 0.05f);

    const Animator& animator = *w.app.world().get<Animator>(w.character);
    const SkinPalette& palette = *w.app.world().get<SkinPalette>(w.character);
    REQUIRE(palette.matrices);
    BufferPool& pool = w.app.world().buffers();
    CHECK(pool.size(palette.matrices.raw) == 3);
    CHECK(animator.historyValid);
    const auto* m = static_cast<const SkinMatrix*>(pool.data(palette.matrices.raw, sizeof(SkinMatrix)));
    for (int j = 0; j < 3; ++j) {
        // palette = pose * inverse bind: close to identity for a pose near the bind pose, and always finite
        CHECK(std::isfinite(m[j].row0.w));
        CHECK(std::abs(m[j].row1.w) < 0.1f);
        CHECK(m[j].row0.x == doctest::Approx(1.0f).epsilon(0.05));
    }
}

TEST_CASE("the gait phase advances faster when the character moves faster than the clip was authored") {
    World3 slow, fast;
    slow.control().velocity = Vec3(0, 0, 1.6f);
    fast.control().velocity = Vec3(0, 0, 6.0f); // beyond the fastest clip (4.2 m/s): the playback speeds up 6 / 4.2 to match
    slow.run(120);
    fast.run(120);
    // two seconds at rate 1 is exactly two cycles (phase wraps back to ~0); the faster one is somewhere else
    CHECK(std::abs(slow.state().phase - std::round(slow.state().phase)) < 0.1f);
    CHECK(std::abs(fast.state().phase - slow.state().phase) > 0.05f);
}

TEST_CASE("standing still facing away plays a turn clip, the heading follows its root motion, and it ends facing the goal") {
    World3 w;
    w.run(10);
    w.control().facing = Vec3(1, 0, 0); // 90 degrees toward +X: the character's left
    bool turning = false;
    for (int i = 0; i < 240; ++i) {
        w.run(1);
        turning = turning || w.state().mode == static_cast<u32>(LocomotionMode::Turn);
    }
    CHECK(turning);
    CHECK(w.state().mode == static_cast<u32>(LocomotionMode::Move));
    CHECK(headingOf(w.transform()) == doctest::Approx(kPi / 2).epsilon(0.03));
    CHECK(w.transform().position.x == doctest::Approx(0.0f).epsilon(0.01)); // it turned on the spot
}

TEST_CASE("a character without loaded clips keeps working (rest pose, no crash)") {
    App app({.workerThreads = 0});
    Rig rig;
    RegisterAnimation(app);
    app.insertResource(AnimationLink{&rig.library});
    Animator animator;
    animator.skeleton = rig.skeletonId;
    const Entity e = app.world().spawn(Transform{}, animator, SkinPalette{}, LocomotionClips{}, LocomotionControl{}, LocomotionState{});
    for (int i = 0; i < 5; ++i) app.update(0.016f);
    const SkinPalette* palette = app.world().get<SkinPalette>(e);
    REQUIRE(palette);
    CHECK(palette->matrices);
}
