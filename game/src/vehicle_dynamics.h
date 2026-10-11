#pragma once

// Vehicle dynamics (ADR 0015): one rigid body on spring-damper wheels. Pure math over plain data (no ECS access), so the
// handling can be tuned while the game runs (this lives in the hot-reloadable game module) and unit-tested.
//
// The model, per sub-step:
//   * the chassis is a rigid body (mass, box inertia, centre of mass) integrated semi-implicitly;
//   * every wheel samples the ground below its hub (terrain height or the top of a solid box), which gives the suspension
//     compression; the spring pushes along the ground normal with the static weight as preload, so the vehicle sits at
//     the ride height the artist modelled;
//   * tires turn the hub's velocity over the ground into a friction force: stiff while the tire grips, saturating on a
//     friction ellipse, so it holds on slopes, slides when overdriven and drifts with the handbrake;
//   * the engine (torque curve, rev limiter) drives the wheels through an automatic gearbox, brakes act on wheel spin;
//   * the body (a few circles along the collider box) is pushed out of buildings, trunks, solid boxes and cliffs.
//
// Conventions: vehicle space is +Z forward, +Y up, +X left. A positive steering angle turns left.
//
// Thread-safety: StepVehicle touches only the data passed in; run it for different vehicles on different threads.

#include <span>

#include "foh/ecs/reflect.h"
#include "foh/math.h"
#include "foh/renderer/components.h"
#include "foh/scene/game_asset_request.h"
#include "foh/world/surface.h"

namespace foh::game {

struct SolidBox {
    Vec3 center{0.0f}, half{0.5f};
};

// Runtime state of a vehicle. Reflected: a game module reload keeps the car moving.
struct VehicleBody {
    Vec3 velocity{0.0f};        // m/s, world, of the centre of mass
    Vec3 angularVelocity{0.0f}; // rad/s, world
    // Driver intent (written by the control system; a parked vehicle keeps the handbrake on).
    f32 throttle = 0.0f;        // 0..1
    f32 brake = 0.0f;           // 0..1
    f32 steerInput = 0.0f;      // -1 (right) .. 1 (left)
    f32 handbrake = 1.0f;       // 0..1
    bool reverse = false;       // the driver wants to go backwards (S while stopped)
    // Drivetrain.
    f32 steer = 0.0f;           // current steering angle of the wheels at the "centre", radians
    i32 gear = 1;               // -1 reverse, 1.. forward
    f32 rpm = 0.0f;
    f32 shiftTimer = 0.0f;      // > 0: shifting, no torque
    f32 sinceShift = 10.0f;     // seconds since the last gear change (the box does not hunt)
    // Wheels, by index of VehicleSetup::wheels.
    f32 wheelSpeed[kMaxVehicleWheels] = {};  // rad/s, positive when rolling forward
    f32 wheelAngle[kMaxVehicleWheels] = {};  // accumulated rotation, radians (the visual)
    f32 wheelSteer[kMaxVehicleWheels] = {};  // steering angle of this wheel (Ackermann), radians
    f32 suspension[kMaxVehicleWheels] = {};  // compression relative to rest, m (+ = wheel moved up into the body)
    f32 slip[kMaxVehicleWheels] = {};        // how hard the tire is sliding, 0 grip .. 1+ sliding (for smoke and sound)
    f32 load[kMaxVehicleWheels] = {};        // vertical force on the wheel, N
    // Summary.
    f32 speed = 0.0f;           // m/s, any direction
    f32 forwardSpeed = 0.0f;    // m/s along the vehicle's forward axis
    u32 groundedWheels = 0;
    f32 upsideDownTime = 0.0f;  // seconds on its roof or side
    bool fresh = true;          // not placed on the ground yet
    bool occupied = false;
    Entity driver{};
};

// Where the vehicle is: the ground and what it can hit. Built by the caller every frame.
struct VehicleWorld {
    const WorldSurface* surface = nullptr;
    std::span<const SolidBox> boxes;

    // Height of the ground at (x, z): the terrain, or the top of a solid box when it is not above `reachY`.
    f32 groundHeight(f32 x, f32 z, f32 reachY) const;
    Vec3 groundNormal(f32 x, f32 z) const;
};

// Puts the vehicle on the ground, still, at the ride height of the model.
void PlaceVehicle(const VehicleSetup& setup, VehicleBody& body, Transform& transform, const VehicleWorld& world);

// Advances the vehicle by dt seconds (internally in sub-steps of at most 10 ms).
void StepVehicle(const VehicleSetup& setup, VehicleBody& body, Transform& transform, const VehicleWorld& world, f32 dt);

// Top speed in the highest gear at the redline, ignoring drag (an upper bound): what the Forge shows next to the numbers.
f32 VehicleTopSpeedEstimate(const VehicleSetup& setup);

} // namespace foh::game

FOH_REFLECT(foh::game::VehicleBody, velocity, angularVelocity, throttle, brake, steerInput, handbrake, reverse, steer, gear, rpm, shiftTimer, sinceShift,
            wheelSpeed, wheelAngle, wheelSteer, suspension, slip, load, speed, forwardSpeed, groundedWheels, upsideDownTime, fresh, occupied, driver);
