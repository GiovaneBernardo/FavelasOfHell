#pragma once

// Animation as an ECS plugin (ADR 0009).
//
//   RegisterAnimation(app)           registers the animation components and the systems below
//   app.insertResource(AnimationLink{&library})   connects a world to the engine's AnimationLibrary
//
// Systems (Stage::Update, in this order):
//   prepareLocomotionSets   serial: builds the blend space of every distinct LocomotionClips (needs the library mutable)
//   updateLocomotion        parallel over characters: smoothing, state machine, pose sampling and blending,
//                           inertialized transitions, heading/position, skinning matrices -> SkinPalette
// Transform propagation and render extraction (PostUpdate) then pick the result up.
//
// Thread-safety: the systems run on the job system; the library is read-only while they run.

#include "foh/animation/animation_library.h"
#include "foh/animation/components.h"
#include "foh/ecs.h"
#include "foh/platform.h"

namespace foh {

// Resource tying a world to the library of loaded animation assets. Holds a pointer into engine-owned memory
// (allowed, like RenderSceneLink); null until attached, in which case characters keep their last pose.
struct AnimationLink {
    AnimationLibrary* library = nullptr;
};

FOH_API void RegisterAnimation(App& app);

class AnimationPlugin final : public Plugin {
public:
    void build(App& app) override { RegisterAnimation(app); }
};

} // namespace foh
