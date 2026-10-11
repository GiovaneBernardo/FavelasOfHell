#pragma once

// Game asset definitions (ADR 0015): what turns an imported model into something the game can use.
//
// A model from Sketchfab is just meshes. The Asset Forge (editor) adds the part only a human knows (where the wheels are,
// how stiff the springs are, what is solid, which bones are the arms) and stores it next to the model as a small text
// file, one per game asset:
//
//   assets/imported/Car/Car.gltf           the model (the prefab editor's .fvariant next to it fixes scale and orientation)
//   assets/imported/Car/Car.fvehicle       a vehicle   (wheels, suspension, engine, seats, camera)
//   assets/imported/Prop/Barrel.fprop      a prop      (collision shape, mass, surface)
//   assets/imported/Hero/Hero.fchar        a character (humanoid bone map, rig recipe, role)
//
// The files are text on purpose: they are diffed and merged in git like the .meta files, and a handler can read them
// without the asset database. The layout is `key value` lines grouped in `[section]` blocks; unknown keys are ignored and
// missing ones keep the defaults below, so files survive added fields in both directions.
//
// Conventions of a vehicle: model space is the vehicle space after the prefab variant is applied: +Z forward, +Y up,
// +X to the vehicle's left (glTF's convention), origin on the ground under the middle of the vehicle.
//
// Thread-safety: plain data; the Parse/Write functions are pure.

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "foh/math.h"
#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

enum class GameAssetKind : u8 { None, Vehicle, Prop, Character };

FOH_API const char* GameAssetKindName(GameAssetKind kind);              // "vehicle", "prop", "character"
FOH_API const char* GameAssetExtension(GameAssetKind kind);             // ".fvehicle", ".fprop", ".fchar"
FOH_API GameAssetKind GameAssetKindFromPath(std::string_view path);     // by extension, case insensitive

inline constexpr u32 kMaxVehicleWheels = 8;
inline constexpr u32 kMaxVehicleGears = 8;

// ---- vehicle -------------------------------------------------------------------------------------

struct WheelDef {
    std::string name;      // "Front Left": shown in the editor
    // Prefab node paths of the meshes of the wheel ("Car/Body/Tire_FL|Car/Body/Rim_FL": a tire and a rim turn together);
    // empty: the wheel has no mesh of its own and is not drawn separately.
    std::string node;
    Vec3 position{0.0f};   // wheel centre with the vehicle at rest, vehicle space
    f32 radius = 0.33f;
    f32 width = 0.22f;
    bool steered = false;
    bool driven = false;
    bool handbrake = false;
};

struct VehicleDef {
    std::string name;
    std::string model; // source path of the model, relative to the asset root ("imported/Car/Car.gltf")

    // Body.
    f32 mass = 1400.0f;                 // kg
    Vec3 centerOfMass{0.0f, 0.5f, 0.0f}; // vehicle space; low is stable, high rolls over
    Vec3 colliderCenter{0.0f, 0.75f, 0.0f}; // the box that hits buildings and trees
    Vec3 colliderHalf{0.9f, 0.7f, 2.2f};
    f32 dragCoefficient = 0.32f;        // Cd
    f32 frontalArea = 2.2f;             // m^2
    f32 downforce = 0.0f;               // N per (m/s)^2 pressing the car to the ground

    // Suspension (one set per axle: the frontmost wheels are the front axle, the rest the rear).
    f32 frontFrequency = 1.6f;          // Hz of the sprung mass; softer is ~1.0, a sports car ~2.2
    f32 rearFrequency = 1.7f;
    f32 dampingRatio = 0.6f;            // 1 = critically damped
    f32 travelUp = 0.12f;               // m the wheel can move up from rest
    f32 travelDown = 0.14f;             // m it can drop below rest
    f32 antiRoll = 0.35f;               // 0..1: how much a roll bar couples the two wheels of an axle

    // Tires.
    f32 frontGrip = 1.1f;               // friction coefficient on dry asphalt
    f32 rearGrip = 1.15f;
    f32 rollingResistance = 0.012f;
    f32 tractionControl = 0.6f;         // 0 off (burnouts, wheelspin) .. 1 strong: cuts the engine torque of a spinning wheel

    // Engine and transmission (automatic).
    f32 maxTorque = 320.0f;             // Nm at the peak
    f32 idleRpm = 900.0f;
    f32 peakRpm = 4200.0f;
    f32 redlineRpm = 6500.0f;
    std::vector<f32> gears{3.55f, 2.2f, 1.5f, 1.12f, 0.9f, 0.74f};
    f32 reverseGear = 3.4f;
    f32 finalDrive = 3.7f;
    f32 drivelineEfficiency = 0.88f;

    // Brakes and steering.
    f32 brakeTorque = 2600.0f;          // Nm per wheel
    f32 brakeFrontBias = 0.62f;         // share of the braking on the front axle
    f32 handbrakeTorque = 3200.0f;
    f32 maxSteerDegrees = 34.0f;
    f32 highSpeedSteer = 0.3f;          // fraction of the steering left at 40 m/s
    f32 steerSpeed = 3.2f;              // 1/s: how fast the wheels follow the input

    // People.
    Vec3 driverSeat{0.4f, 0.95f, 0.1f}; // where the driver sits
    Vec3 exitOffset{1.6f, 0.0f, 0.0f};  // where the driver steps out, relative to the vehicle on the ground
    f32 cameraDistance = 6.5f;
    f32 cameraHeight = 2.1f;
    f32 cameraFov = 1.0f;               // radians

    std::vector<WheelDef> wheels;

    // Wheels on the front axle: those within a tenth of the wheelbase of the frontmost.
    u32 frontAxleCount() const {
        if (wheels.empty()) return 0;
        f32 minZ = wheels[0].position.z, maxZ = minZ;
        for (const WheelDef& w : wheels) {
            minZ = w.position.z < minZ ? w.position.z : minZ;
            maxZ = w.position.z > maxZ ? w.position.z : maxZ;
        }
        const f32 tolerance = 0.1f * (maxZ - minZ);
        u32 n = 0;
        for (const WheelDef& w : wheels) n += w.position.z >= maxZ - tolerance ? 1u : 0u;
        return n;
    }
};

// The paths of WheelDef::node, in order.
FOH_API std::vector<std::string> SplitWheelNodes(std::string_view nodes);

// ---- prop ----------------------------------------------------------------------------------------

enum class PropBody : u8 { Static, Dynamic, Decoration }; // Decoration: no collision at all
enum class PropShape : u8 { Box, Sphere, Capsule };

struct PropDef {
    std::string name;
    std::string model;
    PropBody body = PropBody::Static;
    PropShape shape = PropShape::Box;
    Vec3 center{0.0f, 0.5f, 0.0f}; // model space
    Vec3 halfExtents{0.5f};        // Box: half sizes; Sphere: x is the radius; Capsule: x radius, y half length of the axis
    f32 mass = 20.0f;              // Dynamic only
    f32 friction = 0.6f;
    f32 restitution = 0.1f;
    std::string surface = "concrete"; // footsteps / impact sounds / particles later
    bool breakable = false;
    f32 health = 100.0f;
};

// ---- character -----------------------------------------------------------------------------------

enum class CharacterRole : u8 { Player, Npc };

// The humanoid the animation library speaks: Mixamo's bone names (the importer strips the "mixamorig:" prefix).
inline constexpr u32 kHumanoidJointCount = 25;
FOH_API const char* HumanoidJointName(u32 index);
FOH_API i32 HumanoidJointIndex(std::string_view name); // -1 when it is not one of the 25

struct CharacterDef {
    std::string name;
    std::string model;        // the model the artist imported
    std::string riggedModel;  // the model the auto-rig produced (empty: the source is used as is)
    CharacterRole role = CharacterRole::Npc;
    f32 height = 1.8f;        // m: the character is scaled to this
    f32 radius = 0.35f;
    // Auto-rig recipe: joint positions in model space (the 25 humanoid joints), so a rig can be redone after tweaking.
    bool hasLandmarks = false;
    std::array<Vec3, kHumanoidJointCount> landmarks{};
    bool normalizeTPose = true; // an A-pose (arms hanging) is lifted to a T-pose first: the animation clips expect one
    f32 skinSmoothness = 0.5f;  // 0 rigid .. 1 very soft weights
};

// ---- text format ---------------------------------------------------------------------------------

FOH_API std::string WriteVehicleDef(const VehicleDef& def);
FOH_API bool ParseVehicleDef(std::string_view text, VehicleDef& out, std::string* error = nullptr);
FOH_API std::string WritePropDef(const PropDef& def);
FOH_API bool ParsePropDef(std::string_view text, PropDef& out, std::string* error = nullptr);
FOH_API std::string WriteCharacterDef(const CharacterDef& def);
FOH_API bool ParseCharacterDef(std::string_view text, CharacterDef& out, std::string* error = nullptr);

// Sanity problems an artist should fix (empty: fine). Used by the Forge and before a vehicle is spawned.
FOH_API std::vector<std::string> ValidateVehicleDef(const VehicleDef& def);

} // namespace foh
