#pragma once

// Vehicles in the game (ADR 0015, hot reloadable).
//
//   F           get in the nearest vehicle / get out
//   W S         accelerate / brake, and reverse when stopped      A D   steer
//   Space       handbrake                                          R     turn an overturned vehicle back onto its wheels
//   Mouse       look around the car (the camera swings back behind it after a moment)
//   B           spawn the vehicle selected in the editor's Vehicles panel in front of the player
//
// A vehicle is an entity that asked for a .fvehicle (GameAssetRequest): the engine adds the model and VehicleSetup, this
// module adds VehicleBody and simulates it (vehicle_dynamics.h). The driver stays an entity (the Player, marked InVehicle)
// and is hidden until there is a seated pose. Props (.fprop) get their collider here as well, because the colliders are
// game components (StaticBox).
//
// All state is in components and resources, so it survives a reload of the module.

#include "foh/ecs.h"
#include "foh/input.h"
#include "vehicle_dynamics.h"

namespace foh::game {

// On the player while driving.
struct InVehicle {
    Entity vehicle{};
};

// Marks a prop whose collider entity exists.
struct PropCollider {
    u32 flags = 0;
};

// Chase camera state while driving.
struct VehicleCamera {
    f32 heading = 0.0f;    // radians about +Y, the direction the camera looks (smoothed behind the car)
    f32 yawOffset = 0.0f;  // the player's look-around, decays back to zero
    f32 pitchOffset = 0.0f;
    f32 idleTime = 0.0f;   // seconds since the mouse moved
    bool active = false;
};

void RegisterVehicles(App& app);

} // namespace foh::game

FOH_REFLECT(foh::game::InVehicle, vehicle);
FOH_REFLECT(foh::game::PropCollider, flags);
FOH_REFLECT(foh::game::VehicleCamera, heading, yawOffset, pitchOffset, idleTime, active);
