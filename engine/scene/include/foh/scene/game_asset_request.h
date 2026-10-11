#pragma once

// Game assets in a world (ADR 0015). Game code asks for a vehicle, prop or character by the path of its definition file;
// the scene plugin turns the request into an entity:
//
//     world.spawn(Transform{...}, MakeGameAssetRequest("imported/Car/Car.fvehicle"));
//
//   vehicle    PrefabSlot (the model) + VehicleSetup (the flattened .fvehicle, with the wheel mesh entities found)
//   prop       PrefabSlot + PropSetup (collision shape; the game module turns it into colliders)
//   character  PrefabSlot (the rigged model when there is one) + CharacterSetup
//
// The components are plain data, so they are saved with the world and survive game module reloads. What a vehicle DOES
// (driving it) is game code: it reads VehicleSetup. A request waits until its model is imported and instantiated.
//
// Thread-safety: the resolving system is exclusive (main thread).

#include <string_view>

#include "foh/ecs.h"
#include "foh/ecs/reflect.h"
#include "foh/game_asset.h"
#include "foh/platform.h"

namespace foh {

struct GameAssetRequest {
    char path[128] = {}; // definition file, relative to the asset root ("imported/Car/Car.fvehicle")
};

inline GameAssetRequest MakeGameAssetRequest(std::string_view path) {
    GameAssetRequest r;
    const size_t n = path.size() < sizeof(r.path) - 1 ? path.size() : sizeof(r.path) - 1;
    for (size_t i = 0; i < n; ++i) r.path[i] = path[i];
    return r;
}

inline constexpr u32 kMaxWheelNodes = 3; // a wheel may be a tire mesh plus a rim mesh (plus a brake disc...)

// A mesh entity that belongs to a wheel: the simulation poses it every frame from its rest transform and the chain above it.
struct WheelNode {
    Entity entity{};                          // null: unused slot
    Vec4 parentRow0{1.0f, 0.0f, 0.0f, 0.0f}; // node parent space -> vehicle space, 3x4 rows
    Vec4 parentRow1{0.0f, 1.0f, 0.0f, 0.0f};
    Vec4 parentRow2{0.0f, 0.0f, 1.0f, 0.0f};
    Vec3 restPosition{0.0f};                  // the node local transform at rest
    Quat restRotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 restScale{1.0f};
};

// One wheel as the simulation needs it.
struct WheelSetup {
    Vec3 position{0.0f};     // wheel centre at rest, vehicle space
    f32 radius = 0.33f;
    f32 width = 0.22f;
    bool steered = false;
    bool driven = false;
    bool handbrake = false;
    WheelNode nodes[kMaxWheelNodes] = {};
};

// The vehicle definition as a component (see VehicleDef for the meaning of the fields).
struct VehicleSetup {
    f32 mass = 1400.0f;
    Vec3 centerOfMass{0.0f, 0.5f, 0.0f};
    Vec3 colliderCenter{0.0f, 0.75f, 0.0f};
    Vec3 colliderHalf{0.9f, 0.7f, 2.2f};
    f32 dragCoefficient = 0.32f;
    f32 frontalArea = 2.2f;
    f32 downforce = 0.0f;
    f32 frontFrequency = 1.6f;
    f32 rearFrequency = 1.7f;
    f32 dampingRatio = 0.6f;
    f32 travelUp = 0.12f;
    f32 travelDown = 0.14f;
    f32 antiRoll = 0.35f;
    f32 frontGrip = 1.1f;
    f32 rearGrip = 1.15f;
    f32 rollingResistance = 0.012f;
    f32 tractionControl = 0.6f;
    f32 maxTorque = 320.0f;
    f32 idleRpm = 900.0f;
    f32 peakRpm = 4200.0f;
    f32 redlineRpm = 6500.0f;
    f32 gears[kMaxVehicleGears] = {};
    u32 gearCount = 0;
    f32 reverseGear = 3.4f;
    f32 finalDrive = 3.7f;
    f32 drivelineEfficiency = 0.88f;
    f32 brakeTorque = 2600.0f;
    f32 brakeFrontBias = 0.62f;
    f32 handbrakeTorque = 3200.0f;
    f32 maxSteerRadians = 0.59f;
    f32 highSpeedSteer = 0.3f;
    f32 steerSpeed = 3.2f;
    Vec3 driverSeat{0.4f, 0.95f, 0.1f};
    Vec3 exitOffset{1.6f, 0.0f, 0.0f};
    f32 cameraDistance = 6.5f;
    f32 cameraHeight = 2.1f;
    f32 cameraFov = 1.0f;
    u32 wheelCount = 0;
    u32 frontAxleCount = 2;
    WheelSetup wheels[kMaxVehicleWheels] = {};
};

struct PropSetup {
    u32 body = 0;  // PropBody
    u32 shape = 0; // PropShape
    Vec3 center{0.0f};
    Vec3 halfExtents{0.5f};
    f32 mass = 20.0f;
    f32 friction = 0.6f;
    f32 restitution = 0.1f;
    bool breakable = false;
    f32 health = 100.0f;
    char surface[24] = {};
};

struct CharacterSetup {
    u32 role = 0; // CharacterRole
    f32 height = 1.8f;
    f32 radius = 0.35f;
};

// Resource: the definition the game's "spawn a vehicle" key creates. The editor's Vehicles panel sets it. Plain text
// so it survives reloads and saves.
struct SpawnSelection {
    char vehicle[128] = {};
};

// Registers the components, the SpawnSelection resource and the resolving system. Called by RegisterPrefabRequests.
FOH_API void RegisterGameAssets(App& app);

// The runtime form of a definition (also used by tests and the editor's preview).
FOH_API VehicleSetup MakeVehicleSetup(const VehicleDef& def);
FOH_API PropSetup MakePropSetup(const PropDef& def);
FOH_API CharacterSetup MakeCharacterSetup(const CharacterDef& def);

} // namespace foh

FOH_REFLECT(foh::GameAssetRequest, path);
FOH_REFLECT(foh::WheelNode, entity, parentRow0, parentRow1, parentRow2, restPosition, restRotation, restScale);
FOH_REFLECT(foh::WheelSetup, position, radius, width, steered, driven, handbrake, nodes);
// More fields than FOH_REFLECT takes in one list: described in two halves.
template <>
struct ::foh::Reflect<foh::VehicleSetup> {
    static void describe(::foh::SchemaBuilder<foh::VehicleSetup>& b) {
        FOH_FOR_EACH_FIELD(foh::VehicleSetup, mass, centerOfMass, colliderCenter, colliderHalf, dragCoefficient, frontalArea, downforce, frontFrequency,
                           rearFrequency, dampingRatio, travelUp, travelDown, antiRoll, frontGrip, rearGrip, rollingResistance, tractionControl, maxTorque,
                           idleRpm, peakRpm, redlineRpm, gears, gearCount)
        FOH_FOR_EACH_FIELD(foh::VehicleSetup, reverseGear, finalDrive, drivelineEfficiency, brakeTorque, brakeFrontBias, handbrakeTorque, maxSteerRadians,
                           highSpeedSteer, steerSpeed, driverSeat, exitOffset, cameraDistance, cameraHeight, cameraFov, wheelCount, frontAxleCount, wheels)
    }
};
FOH_REFLECT(foh::PropSetup, body, shape, center, halfExtents, mass, friction, restitution, breakable, health, surface);
FOH_REFLECT(foh::CharacterSetup, role, height, radius);
FOH_REFLECT(foh::SpawnSelection, vehicle);
