#pragma once

// Placeholder for the physics module (Jolt). Jolt is a PRIVATE dependency:
// public physics headers expose engine types only.

#include "foh/platform.h"

namespace foh {

class FOH_API PhysicsSystem {
public:
    // Registers Jolt allocator/types. Call once before creating any physics world.
    static void Init();
    static void Shutdown();
};

} // namespace foh
