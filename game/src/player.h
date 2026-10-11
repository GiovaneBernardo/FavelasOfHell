#pragma once

// The player character and its camera (game module, hot reloadable).
//
//   WASD        move relative to the camera        Space   jump
//   Left Shift  run (the default is a walk)        C       crouch (hold; Player::crouchToggle makes it a toggle)
//   Mouse       look                               V       first person / third person (GTA style, over the shoulder)
//
// Jumping works like in GTA: Space starts the wind-up, the body leaves the ground at the exact moment the animation
// springs (the animation system reports it), the direction is locked while airborne, and landing keeps the momentum
// for a moment before the controls take over again.
//
// The character is whatever model the Player entity asks for through PrefabRequest. Its animation is driven by the
// engine's locomotion system: this code only says where the player wants to go (LocomotionControl) and handles the
// parts the engine does not own yet: collision against StaticBox entities, ground, jumping, and the camera.
// All state lives in components and resources, so it survives a reload of the module.

#include "foh/animation/components.h"
#include "foh/ecs.h"
#include "foh/input.h"
#include "foh/renderer/components.h"

namespace foh::game {

struct Player {
    // Speeds in m/s. The defaults match the authored Mixamo clips (walk 1.6, run 4.2), so the feet do not slide.
    f32 walkSpeed = 1.6f;
    f32 runSpeed = 4.2f;
    f32 crouchSpeed = 1.2f;
    f32 jumpSpeed = 4.8f;
    f32 gravity = 13.0f;
    f32 jumpFallbackDelay = 0.4f; // without a jump animation: seconds from Space to leaving the ground
    f32 landingMomentum = 0.8f;   // fraction of the jump speed that carries on after landing
    f32 landingRecovery = 0.4f;   // seconds until the controls fully take over after landing
    f32 radius = 0.35f;
    f32 standHeight = 1.8f;
    f32 crouchHeight = 1.25f;
    f32 stepHeight = 0.5f;       // ledges up to this high are climbed without jumping
    f32 maxSlope = 1.4f;         // terrain steeper than this (rise/run, about 54 degrees) cannot be walked up
    bool crouchToggle = false;
    // Runtime state
    f32 verticalVelocity = 0.0f;
    f32 jumpTimer = -1.0f;       // >= 0: waiting for the animation to spring (or the fallback delay)
    f32 landingTimer = 0.0f;     // > 0: recovering from a landing
    Vec3 lockedVelocity{0.0f};   // horizontal velocity while airborne (no steering in the air)
    Vec3 lockedFacing{0.0f};
    bool jumpPending = false;
    f32 crouch01 = 0.0f;         // 0 standing .. 1 crouched, smoothed
    bool grounded = true;
    bool crouching = false;
    Vec3 previousPosition{0.0f}; // last frame's position: terrain walls need the direction of the move
    bool hasPrevious = false;
};

// Camera settings (the camera entity: Transform, Camera, PlayerCamera).
struct PlayerCamera {
    f32 distance = 3.3f;     // third person: meters behind the shoulder pivot
    f32 shoulder = 0.55f;    // third person: sideways offset of the pivot (positive: right)
    f32 sensitivity = 0.0022f; // radians per pixel of mouse movement
    f32 fovThirdPerson = 1.0472f;
    f32 fovFirstPerson = 1.2217f;
    f32 followRate = 16.0f;  // third person: how fast the pivot catches up with the player (1/s)
};

// Axis aligned solid, taken from the entity's world transform (a unit cube scaled and placed). A stand-in for real
// collision until the physics module exists.
struct StaticBox {
    u32 flags = 0;
};

// Resources: where the player looks, and what the camera needs to know about the player.
struct PlayerLook {
    f32 yaw = 0.0f;   // radians about +Y; 0 looks down -Z (the fly camera's convention)
    f32 pitch = -0.15f;
    bool firstPerson = false;
};

struct PlayerView {
    Vec3 position{0.0f}; // feet
    Vec3 pivot{0.0f};    // smoothed third-person pivot
    f32 crouch01 = 0.0f;
    u64 entity = 0;      // Entity::id of the player
    bool valid = false;
    bool pivotValid = false;
    bool hidden = false; // the player's body is hidden (first person, or driving)
    bool inVehicle = false; // the player is driving: the vehicle camera takes over
};

void RegisterPlayer(App& app);

} // namespace foh::game

FOH_REFLECT(foh::game::Player, walkSpeed, runSpeed, crouchSpeed, jumpSpeed, gravity, jumpFallbackDelay, landingMomentum, landingRecovery,
            radius, standHeight, crouchHeight, stepHeight, maxSlope, crouchToggle, verticalVelocity, jumpTimer, landingTimer, lockedVelocity,
            lockedFacing, jumpPending, crouch01, grounded, crouching, previousPosition, hasPrevious);
FOH_REFLECT(foh::game::PlayerCamera, distance, shoulder, sensitivity, fovThirdPerson, fovFirstPerson, followRate);
FOH_REFLECT(foh::game::StaticBox, flags);
FOH_REFLECT(foh::game::PlayerLook, yaw, pitch, firstPerson);
FOH_REFLECT(foh::game::PlayerView, position, pivot, crouch01, entity, valid, pivotValid, hidden, inVehicle);
