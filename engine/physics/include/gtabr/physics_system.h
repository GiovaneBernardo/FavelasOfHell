#pragma once

// Placeholder for the physics module (Jolt). Jolt is a PRIVATE dependency:
// public physics headers expose engine types only.

#include "gtabr/platform.h"

namespace gtabr {

class GTABR_API PhysicsSystem {
public:
    // Registers Jolt allocator/types. Call once before creating any physics world.
    static void Init();
    static void Shutdown();
};

} // namespace gtabr
