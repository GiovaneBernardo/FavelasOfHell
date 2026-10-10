// Locomotion: from "what gameplay wants" (LocomotionControl) to "what the GPU skins" (SkinPalette), per character.
//
//   smooth velocity -> pick mode (move / turn in place / one-shot) -> advance the gait phase with stride matching
//   -> sample + blend the clips of the blend space (phase synchronised) -> inertialize mode changes
//   -> heading, position -> skinning matrices
//
// Why it looks right (ADR 0009): clips are blended on one shared gait phase so feet land together, the playback rate
// follows the speed so the feet do not slide, mode changes are inertialized instead of cross-faded, the body heading
// is a critically damped spring, and turn-in-place clips drive the heading with their own root motion.
#include "locomotion.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "foh/animation/spring.h"
#include "foh/jobs.h"

namespace foh {

namespace {

constexpr f32 kMaxDt = 0.05f;          // a long frame must not fling the spring state
constexpr f32 kMinTurnInterrupt = 0.2f; // seconds a turn runs before the player's input may cut it
constexpr f32 kInterruptSpeed = 0.6f;   // desired speed that cancels a turn in place
constexpr f32 kMinRate = 0.5f, kMaxRate = 1.5f; // stride-matching playback rate limits

f32 headingOf(const Quat& rotation) {
    const Vec3 forward = rotation * Vec3(0.0f, 0.0f, 1.0f);
    return std::atan2(forward.x, forward.z);
}

Vec2 toLocal(const Vec3& worldVelocity, f32 yaw) {
    const f32 c = std::cos(yaw), s = std::sin(yaw);
    return Vec2(worldVelocity.x * c - worldVelocity.z * s, worldVelocity.x * s + worldVelocity.z * c);
}

f32 wrap01(f32 x) { return x - std::floor(x); }

// Everything the pose of one frame depends on, so the previous frame's target can be rebuilt for a transition.
struct PoseRequest {
    LocomotionMode mode = LocomotionMode::Move;
    Vec2 localVelocity{0.0f};
    f32 phase = 0.0f;
    f32 idleTime = 0.0f;
    f32 modeTime = 0.0f;
};

struct Character {
    const AnimationLibrary& library;
    const SkeletonAsset& skeleton;
    AssetId skeletonId;
    const MoveSet* moves;
    const LocomotionState& state; // only oneShot is read
};

// Samples `clipId` over `layer` (start from rest) and accumulates it into `out`.
void accumulateClip(const Character& c, const AssetId& clipId, f32 time, f32 weight, bool first, std::span<JointPose> out,
                    std::span<JointPose> layer) {
    const ClipAsset* clip = c.library.clip(clipId);
    const ClipBinding* binding = c.library.binding(c.skeletonId, clipId);
    if (!clip || !binding || weight <= 0.0f) return;
    SetRestPose(c.skeleton, layer);
    SampleClip(*clip, *binding, time, layer);
    if (first) BlendInit(out, layer, weight);
    else BlendAdd(out, layer, weight);
}

void evaluatePose(const Character& c, const PoseRequest& req, std::span<JointPose> out, std::span<JointPose> layer) {
    const size_t n = c.skeleton.jointCount();
    out = out.first(n);
    layer = layer.first(n);

    if (req.mode != LocomotionMode::Move) {
        const ClipAsset* clip = c.library.clip(c.state.oneShot);
        const ClipBinding* binding = c.library.binding(c.skeletonId, c.state.oneShot);
        SetRestPose(c.skeleton, out);
        if (clip && binding) SampleClip(*clip, *binding, std::min(req.modeTime, std::max(clip->duration() - 1e-4f, 0.0f)), out);
        return;
    }

    SetRestPose(c.skeleton, out);
    if (!c.moves) return;
    const MoveWeights w = EvaluateMoveSet(*c.moves, req.localVelocity);
    bool first = true;
    f32 total = 0.0f;
    if (w.idleWeight > 0.0f && c.moves->idle.isValid()) {
        accumulateClip(c, c.moves->idle, req.idleTime, w.idleWeight, first, out, layer);
        first = false;
        total += w.idleWeight;
    }
    for (u32 i = 0; i < w.count; ++i) {
        const MoveEntry& e = c.moves->entries[w.items[i].entry];
        // Every clip is read at the same point of the gait: phase 0 is "left foot comes down" in all of them.
        accumulateClip(c, e.clip, wrap01(req.phase + e.syncOffset) * e.duration, w.items[i].weight, first, out, layer);
        first = false;
        total += w.items[i].weight;
    }
    if (!first) BlendFinish(out, total);
    else SetRestPose(c.skeleton, out);
}

} // namespace

void prepareLocomotionSets(ResMut<AnimationLink> link, Query<const LocomotionClips&> clips) {
    AnimationLibrary* library = link->library;
    if (!library) return;
    LocomotionClips last{};
    bool haveLast = false;
    clips.each([&](const LocomotionClips& c) {
        // Most characters share one set; skip the hash lookup when the previous entity had the same clips.
        if (haveLast && std::memcmp(&last, &c, sizeof(c)) == 0) return;
        library->prepareMoveSet(c, false);
        library->prepareMoveSet(c, true);
        last = c;
        haveLast = true;
    });
}

void updateLocomotion(Res<AnimationLink> link, Res<Time> time, Local<std::vector<LocomotionScratch>> scratch,
                      BuffersMut<JointHistory> histories, BuffersMut<SkinMatrix> palettes,
                      Query<Transform&, Animator&, SkinPalette&, const LocomotionClips&, LocomotionControl&, LocomotionState&> characters) {
    const AnimationLibrary* library = link->library;
    if (!library) return;
    if (scratch->empty()) scratch->resize(kMaxJobThreads);
    const f32 dt = std::clamp(time->delta, 0.0f, kMaxDt);
    LocomotionScratch* buffers = scratch->data();

    characters.parEach([&](Transform& transform, Animator& animator, SkinPalette& palette, const LocomotionClips& clips,
                           LocomotionControl& control, LocomotionState& state) {
        if (!animator.enabled) return;
        const SkeletonAsset* skeleton = library->skeleton(animator.skeleton);
        if (!skeleton) return;
        const u32 joints = skeleton->jointCount();
        if (joints == 0) return;
        LocomotionScratch& s = buffers[JobSystem::currentThreadSlot()];

        // ---- buffers: created on first use, rebuilt when the skeleton changed ----
        if (animator.jointCount != joints || !animator.history) {
            if (animator.history) histories.release(animator.history);
            animator.history = histories.create();
            histories.resize(animator.history, joints);
            animator.jointCount = joints;
            animator.historyValid = false;
        }
        if (!palette.matrices) palette.matrices = palettes.create();
        if (palettes.size(palette.matrices) != joints) palettes.resize(palette.matrices, joints);
        const std::span<JointHistory> history = histories.get(animator.history);
        const std::span<SkinMatrix> out = palettes.get(palette.matrices);
        if (history.size() != joints || out.size() != joints) return;

        // Stance: crouching needs a crouch idle; a turn or one-shot in progress finishes first.
        const MoveSet* crouchSet = control.crouch ? library->moveSet(clips, true) : nullptr;
        const u32 wantedStance = crouchSet && crouchSet->idle.isValid() ? 1u : 0u;
        const MoveSet* moves = library->moveSet(clips, state.stance != 0);
        Character character{*library, *skeleton, animator.skeleton, moves, state};
        const f32 step = dt * animator.playbackSpeed;

        // ---- inputs ----
        f32 yaw = headingOf(transform.rotation);
        Vec3 desired = control.velocity;
        desired.y = 0.0f;
        const f32 desiredSpeed = glm::length(desired);
        if (!state.initialized) {
            state.velocity = desired;
            state.initialized = true;
        }
        // First-order smoothing of the velocity: exact for any dt, never overshoots.
        const f32 blend = 1.0f - std::exp(-spring::decayRate(control.velocityHalfLife) * dt);
        state.velocity += (desired - state.velocity) * blend;
        state.velocity.y = 0.0f;
        const f32 speed = glm::length(state.velocity);

        Vec3 face = control.facing;
        face.y = 0.0f;
        bool hasTarget = false;
        f32 targetYaw = yaw;
        if (glm::length(face) > 0.01f) {
            targetYaw = std::atan2(face.x, face.z);
            hasTarget = true;
        } else if (speed > 0.15f) {
            targetYaw = std::atan2(state.velocity.x, state.velocity.z);
            hasTarget = true;
        }

        // ---- mode ----
        bool transitioned = false;
        auto startClip = [&](LocomotionMode mode, const AssetId& clip, f32 duration) {
            state.mode = static_cast<u32>(mode);
            state.oneShot = clip;
            state.oneShotDuration = duration;
            state.oneShotYaw = yaw;
            state.modeTime = 0.0f;
            transitioned = true;
        };
        auto backToMove = [&] {
            state.mode = static_cast<u32>(LocomotionMode::Move);
            state.modeTime = 0.0f;
            transitioned = true;
        };
        LocomotionMode mode = static_cast<LocomotionMode>(state.mode);
        if (mode == LocomotionMode::Move) {
            if (control.jump && moves && moves->jump.isValid()) {
                startClip(LocomotionMode::OneShot, moves->jump, moves->jumpDuration);
                control.jump = false;
            } else if (hasTarget && moves && speed < 0.3f && desiredSpeed < 0.1f) {
                const f32 delta = AngleDelta(yaw, targetYaw);
                if (std::abs(delta) > control.turnInPlaceDegrees * math::kPi / 180.0f) {
                    if (const TurnEntry* turn = PickTurn(*moves, delta)) startClip(LocomotionMode::Turn, turn->clip, turn->duration);
                }
            }
        } else {
            state.modeTime += step;
            const bool finished = state.modeTime >= state.oneShotDuration;
            const bool interrupted = mode == LocomotionMode::Turn && state.modeTime > kMinTurnInterrupt && desiredSpeed > kInterruptSpeed;
            if (finished || interrupted) backToMove();
        }
        mode = static_cast<LocomotionMode>(state.mode);
        if (control.jump && mode != LocomotionMode::Move) control.jump = false; // one at a time
        if (mode == LocomotionMode::Move && state.stance != wantedStance) {
            state.stance = wantedStance; // inertialized like any other change of animation
            moves = library->moveSet(clips, state.stance != 0);
            character.moves = moves;
            transitioned = true;
        }

        // ---- heading and position ----
        const ClipAsset* oneShotClip = mode == LocomotionMode::Turn ? library->clip(state.oneShot) : nullptr;
        if (oneShotClip && !oneShotClip->rootCurve.empty()) {
            // The turn clip owns the heading: its root rotation, exactly as authored.
            yaw = state.oneShotYaw + SampleRootCurve(*oneShotClip, state.modeTime).y;
            state.yawVelocity = 0.0f;
        } else if (hasTarget && mode != LocomotionMode::OneShot) {
            f32 offset = -AngleDelta(yaw, targetYaw); // position relative to the goal
            // Standing still, small corrections are slow (a turn clip takes the big ones); moving, the body follows quickly.
            spring::step(offset, state.yawVelocity, 0.0f, speed > 0.3f ? control.turnHalfLife : control.turnHalfLife * 2.0f, dt);
            yaw = targetYaw + offset;
        }
        transform.rotation = glm::angleAxis(yaw, Vec3(0.0f, 1.0f, 0.0f));
        if (control.kinematic) transform.position += state.velocity * dt;

        // ---- gait: stride matching on one shared phase ----
        const Vec2 local = toLocal(state.velocity, yaw);
        if (mode == LocomotionMode::Move && moves) {
            const MoveWeights w = EvaluateMoveSet(*moves, local);
            if (w.moveWeight > 1e-3f && w.blendedDuration > 1e-3f) {
                // Playing faster or slower than authored so that the feet cover exactly the ground the body does.
                const f32 rate = w.blendedSpeed > 0.1f ? std::clamp(speed / w.blendedSpeed, kMinRate, kMaxRate) : 1.0f;
                state.phase = wrap01(state.phase + step * rate / w.blendedDuration);
            }
            state.idleTime += step;
            if (moves->idleDuration > 0.0f) state.idleTime = std::fmod(state.idleTime, moves->idleDuration);
        }

        // ---- pose ----
        const std::span<JointPose> target(s.target.data(), joints);
        PoseRequest now;
        now.mode = mode;
        now.localVelocity = local;
        now.phase = state.phase;
        now.idleTime = state.idleTime;
        now.modeTime = state.modeTime;
        evaluatePose(character, now, target, s.layer);

        const f32 halfLife = animator.transitionHalfLife;
        if (transitioned && animator.historyValid) {
            // The same animation one frame earlier, to measure its velocity next to the character's own.
            PoseRequest before = now;
            before.modeTime = std::max(0.0f, now.modeTime - step);
            evaluatePose(character, before, std::span<JointPose>(s.previous.data(), joints), s.layer);
            InertializeStart(history, target, std::span<const JointPose>(s.previous.data(), joints), dt, halfLife);
            animator.inertialElapsed = 0.0f;
        }
        if (animator.inertialElapsed < InertializeDuration(halfLife)) {
            InertializeApply(history, animator.inertialElapsed, halfLife, target);
            animator.inertialElapsed += dt;
        }
        RecordHistory(history, target, dt, animator.historyValid);
        animator.historyValid = true;

        ComputeSkinPalette(*skeleton, target, out);
    });
}

} // namespace foh
