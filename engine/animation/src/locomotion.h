#pragma once

// The locomotion systems (private to the animation module; registered by RegisterAnimation).

#include <array>
#include <vector>

#include "foh/animation/animation.h"

namespace foh {

// Pose buffers of one job thread, reused for every character it evaluates (no allocation per character or frame).
struct LocomotionScratch {
    std::array<JointPose, kMaxSkeletonJoints> target, previous, layer;
};

// Serial: makes sure every distinct LocomotionClips has its blend space built.
void prepareLocomotionSets(ResMut<AnimationLink> link, Query<const LocomotionClips&> clips);

// Parallel over characters: the whole pipeline from LocomotionControl to SkinPalette.
void updateLocomotion(Res<AnimationLink> link, Res<Time> time, Local<std::vector<LocomotionScratch>> scratch,
                      BuffersMut<JointHistory> histories, BuffersMut<SkinMatrix> palettes,
                      Query<Transform&, Animator&, SkinPalette&, const LocomotionClips&, LocomotionControl&, LocomotionState&> characters);

} // namespace foh
