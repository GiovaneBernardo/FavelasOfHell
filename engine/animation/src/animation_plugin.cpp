#include "foh/animation/animation.h"

#include "locomotion.h"

namespace foh {

void RegisterAnimation(App& app) {
    app.registerComponent<Animator>();
    app.registerComponent<LocomotionClips>();
    app.registerComponent<LocomotionControl>();
    app.registerComponent<LocomotionState>();
    app.registerComponent<SkinPalette>();
    app.registerComponent<SkinnedMesh>();
    app.insertResource(AnimationLink{});
    app.addSystem(Stage::Update, &prepareLocomotionSets).name("prepareLocomotionSets");
    app.addSystem(Stage::Update, &updateLocomotion).name("updateLocomotion");
}

} // namespace foh
