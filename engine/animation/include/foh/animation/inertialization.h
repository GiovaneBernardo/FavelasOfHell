#pragma once

// Inertialization (Bollinger, GDC 2018): transitions without blending two animations.
//
// A cross-fade evaluates the old and the new animation for a while and mixes two moving poses, which smears feet
// and costs two full evaluations. Inertialization switches to the new animation at once and instead decays the
// DIFFERENCE between where the character was and where the new animation says it should be, together with the
// velocity of that difference, with a critically damped spring. The old momentum carries on and fades out, the new
// animation takes over underneath it, and only one animation is evaluated.
//
// Usage per character and frame (see locomotion.cpp):
//   1. evaluate the target pose (the animation the state machine wants now)
//   2. on a transition frame: InertializeStart(history, targetNow, targetOneFrameAgo, dt, halfLife), elapsed = 0
//   3. while elapsed < InertializeDuration(halfLife): InertializeApply(history, elapsed, halfLife, pose)
//   4. RecordHistory(history, finalPose, dt) so the next transition knows position and velocity
//
// Thread-safety: pure functions over caller memory.

#include <span>

#include "foh/animation/pose.h"
#include "foh/ecs/reflect.h"

namespace foh {

// Everything the character remembers between frames, per joint (plain data: lives in an engine buffer, saved with the
// world, rebuilt when missing). 100 bytes per joint.
struct JointHistory {
    Vec3 prevPos{0.0f};
    Quat prevRot{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 velPos{0.0f};  // units per second
    Vec3 velRot{0.0f};  // angular velocity as a rotation vector, radians per second
    Vec3 offPos{0.0f}, offPosVel{0.0f}; // offset at the start of the transition and its velocity
    Vec3 offRot{0.0f}, offRotVel{0.0f}; // same, as rotation vectors
};

// Seconds after which a transition's offset is below 1/65536 of its start and is dropped.
inline f32 InertializeDuration(f32 halfLife) { return 16.0f * halfLife; }

FOH_API void InertializeStart(std::span<JointHistory> history, std::span<const JointPose> targetNow,
                              std::span<const JointPose> targetPrevious, f32 dt, f32 halfLife);
// Adds the decayed offset to `pose` (the target pose of this frame).
FOH_API void InertializeApply(std::span<const JointHistory> history, f32 elapsed, f32 halfLife, std::span<JointPose> pose);
// Stores the final pose and its velocity. `valid` is false for the very first frame (no previous pose to difference).
FOH_API void RecordHistory(std::span<JointHistory> history, std::span<const JointPose> finalPose, f32 dt, bool valid);

// Rotation vector (axis * angle, shortest) of a quaternion and back. Exposed for tests and IK code.
FOH_API Vec3 QuatLog(Quat q);
FOH_API Quat QuatExp(const Vec3& rotationVector);

} // namespace foh

FOH_REFLECT(foh::JointHistory, prevPos, prevRot, velPos, velRot, offPos, offPosVel, offRot, offRotVel);
