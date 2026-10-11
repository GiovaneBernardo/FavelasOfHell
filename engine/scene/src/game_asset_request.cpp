#include "foh/scene/game_asset_request.h"

#include <cstring>
#include <string>
#include <vector>

#include "foh/log.h"
#include "foh/renderer/components.h"
#include "foh/scene/asset_scene_bridge.h"
#include "foh/scene/prefab_request.h"

namespace foh {

namespace {

void copyText(char* dst, size_t capacity, const std::string& src) {
    const size_t n = std::min(src.size(), capacity - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = 0;
}

// The child of `parent` called `name` (names are compared at the width a Name component keeps).
Entity findChildNamed(const World& world, Entity parent, std::string_view name) {
    const Name wanted = Name::Make(name);
    Entity found{};
    world.forEachChild(parent, [&](Entity child) {
        if (!found.isNull()) return;
        if (const Name* n = world.get<Name>(child); n && std::strncmp(n->text, wanted.text, sizeof(n->text)) == 0) found = child;
    });
    return found;
}

// "Car/Body/Wheel FL" walked down from the vehicle entity; the entities on the way (top first, the node last) go to `chain`.
bool findByPath(const World& world, Entity root, const std::string& path, std::vector<Entity>& chain) {
    chain.clear();
    Entity current = root;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t end = path.find('/', pos);
        if (end == std::string::npos) end = path.size();
        current = findChildNamed(world, current, std::string_view(path).substr(pos, end - pos));
        if (current.isNull()) return false;
        chain.push_back(current);
        pos = end + 1;
    }
    return !chain.empty();
}

Mat4 toMatrix(const Transform& t) { return t.matrix(); }

void rowsOf(const Mat4& m, Vec4& r0, Vec4& r1, Vec4& r2) {
    r0 = Vec4(m[0][0], m[1][0], m[2][0], m[3][0]);
    r1 = Vec4(m[0][1], m[1][1], m[2][1], m[3][1]);
    r2 = Vec4(m[0][2], m[1][2], m[2][2], m[3][2]);
}

// Finds the wheel mesh entities of a freshly instantiated model and records their rest transforms.
void bindWheels(World& world, Entity vehicle, const VehicleDef& def) {
    VehicleSetup* setup = world.get<VehicleSetup>(vehicle);
    if (!setup) return;
    std::vector<Entity> chain;
    for (u32 i = 0; i < setup->wheelCount && i < def.wheels.size(); ++i) {
        u32 slot = 0;
        for (const std::string& path : SplitWheelNodes(def.wheels[i].node)) {
            if (slot >= kMaxWheelNodes) break;
            if (!findByPath(world, vehicle, path, chain)) {
                FOH_WARN("vehicle '{}': wheel '{}' names the node '{}' which the model does not have", def.name, def.wheels[i].name, path);
                continue;
            }
            Mat4 parentToVehicle(1.0f);
            for (size_t k = 0; k + 1 < chain.size(); ++k) {
                const Transform* t = world.get<Transform>(chain[k]);
                if (t) parentToVehicle = parentToVehicle * toMatrix(*t);
            }
            const Transform* node = world.get<Transform>(chain.back());
            if (!node) continue;
            WheelNode& n = setup->wheels[i].nodes[slot++];
            n.entity = chain.back();
            rowsOf(parentToVehicle, n.parentRow0, n.parentRow1, n.parentRow2);
            n.restPosition = node->position;
            n.restRotation = node->rotation;
            n.restScale = node->scale;
        }
    }
}

void setSlot(World& world, Entity e, const std::string& model) {
    if (PrefabSlot* slot = world.get<PrefabSlot>(e)) {
        if (std::strncmp(slot->sourcePath, model.c_str(), sizeof(slot->sourcePath)) != 0) copyText(slot->sourcePath, sizeof(slot->sourcePath), model);
    } else {
        world.insert(e, MakePrefabSlot(model));
    }
}

bool modelShown(World& world, Entity e, const std::string& model) {
    const PrefabSlot* slot = world.get<PrefabSlot>(e);
    return slot && std::strncmp(slot->applied, model.c_str(), sizeof(slot->applied)) == 0 && slot->applied[0];
}

// Exclusive: adds components and swaps models.
void resolveGameAssets(World& world, Res<SceneBridgeLink> link, Query<Entity, const GameAssetRequest&> requests) {
    AssetSceneBridge* bridge = link->bridge;
    if (!bridge) return;
    struct Pending {
        Entity entity;
        std::string path;
    };
    std::vector<Pending> pending;
    requests.each([&](Entity e, const GameAssetRequest& r) { pending.push_back({e, r.path}); });
    for (const Pending& p : pending) {
        std::string text, error;
        const GameAssetKind kind = GameAssetKindFromPath(p.path);
        if (!bridge->readAssetText(p.path, text)) {
            FOH_ERROR("game asset '{}': cannot read the file", p.path);
            world.remove<GameAssetRequest>(p.entity);
            continue;
        }
        switch (kind) {
        case GameAssetKind::Vehicle: {
            VehicleDef def;
            if (!ParseVehicleDef(text, def, &error)) {
                FOH_ERROR("vehicle '{}': {}", p.path, error);
                world.remove<GameAssetRequest>(p.entity);
                break;
            }
            for (const std::string& problem : ValidateVehicleDef(def)) FOH_WARN("vehicle '{}': {}", p.path, problem);
            if (!world.has<VehicleSetup>(p.entity)) world.insert(p.entity, MakeVehicleSetup(def));
            setSlot(world, p.entity, def.model);
            if (!modelShown(world, p.entity, def.model)) break; // the model is not imported / instantiated yet: wait
            bindWheels(world, p.entity, def);
            world.remove<GameAssetRequest>(p.entity);
            break;
        }
        case GameAssetKind::Prop: {
            PropDef def;
            if (!ParsePropDef(text, def, &error)) {
                FOH_ERROR("prop '{}': {}", p.path, error);
            } else {
                world.insert(p.entity, MakePropSetup(def));
                setSlot(world, p.entity, def.model);
            }
            world.remove<GameAssetRequest>(p.entity);
            break;
        }
        case GameAssetKind::Character: {
            CharacterDef def;
            if (!ParseCharacterDef(text, def, &error)) {
                FOH_ERROR("character '{}': {}", p.path, error);
            } else {
                world.insert(p.entity, MakeCharacterSetup(def));
                setSlot(world, p.entity, def.riggedModel.empty() ? def.model : def.riggedModel);
            }
            world.remove<GameAssetRequest>(p.entity);
            break;
        }
        default:
            FOH_ERROR("game asset '{}': unknown kind", p.path);
            world.remove<GameAssetRequest>(p.entity);
            break;
        }
    }
}

} // namespace

VehicleSetup MakeVehicleSetup(const VehicleDef& d) {
    VehicleSetup s;
    s.mass = d.mass;
    s.centerOfMass = d.centerOfMass;
    s.colliderCenter = d.colliderCenter;
    s.colliderHalf = d.colliderHalf;
    s.dragCoefficient = d.dragCoefficient;
    s.frontalArea = d.frontalArea;
    s.downforce = d.downforce;
    s.frontFrequency = d.frontFrequency;
    s.rearFrequency = d.rearFrequency;
    s.dampingRatio = d.dampingRatio;
    s.travelUp = d.travelUp;
    s.travelDown = d.travelDown;
    s.antiRoll = d.antiRoll;
    s.frontGrip = d.frontGrip;
    s.rearGrip = d.rearGrip;
    s.rollingResistance = d.rollingResistance;
    s.tractionControl = d.tractionControl;
    s.maxTorque = d.maxTorque;
    s.idleRpm = d.idleRpm;
    s.peakRpm = d.peakRpm;
    s.redlineRpm = d.redlineRpm;
    s.gearCount = static_cast<u32>(std::min<size_t>(d.gears.size(), kMaxVehicleGears));
    for (u32 i = 0; i < s.gearCount; ++i) s.gears[i] = d.gears[i];
    s.reverseGear = d.reverseGear;
    s.finalDrive = d.finalDrive;
    s.drivelineEfficiency = d.drivelineEfficiency;
    s.brakeTorque = d.brakeTorque;
    s.brakeFrontBias = d.brakeFrontBias;
    s.handbrakeTorque = d.handbrakeTorque;
    s.maxSteerRadians = glm::radians(d.maxSteerDegrees);
    s.highSpeedSteer = d.highSpeedSteer;
    s.steerSpeed = d.steerSpeed;
    s.driverSeat = d.driverSeat;
    s.exitOffset = d.exitOffset;
    s.cameraDistance = d.cameraDistance;
    s.cameraHeight = d.cameraHeight;
    s.cameraFov = d.cameraFov;
    s.wheelCount = static_cast<u32>(std::min<size_t>(d.wheels.size(), kMaxVehicleWheels));
    s.frontAxleCount = d.frontAxleCount();
    for (u32 i = 0; i < s.wheelCount; ++i) {
        WheelSetup& w = s.wheels[i];
        w.position = d.wheels[i].position;
        w.radius = d.wheels[i].radius;
        w.width = d.wheels[i].width;
        w.steered = d.wheels[i].steered;
        w.driven = d.wheels[i].driven;
        w.handbrake = d.wheels[i].handbrake;
    }
    return s;
}

PropSetup MakePropSetup(const PropDef& d) {
    PropSetup s;
    s.body = static_cast<u32>(d.body);
    s.shape = static_cast<u32>(d.shape);
    s.center = d.center;
    s.halfExtents = d.halfExtents;
    s.mass = d.mass;
    s.friction = d.friction;
    s.restitution = d.restitution;
    s.breakable = d.breakable;
    s.health = d.health;
    copyText(s.surface, sizeof(s.surface), d.surface);
    return s;
}

CharacterSetup MakeCharacterSetup(const CharacterDef& d) {
    CharacterSetup s;
    s.role = static_cast<u32>(d.role);
    s.height = d.height;
    s.radius = d.radius;
    return s;
}

void RegisterGameAssets(App& app) {
    app.registerComponent<GameAssetRequest>();
    app.registerComponent<VehicleSetup>();
    app.registerComponent<PropSetup>();
    app.registerComponent<CharacterSetup>();
    if (!app.world().resource<SpawnSelection>()) app.insertResource(SpawnSelection{});
    app.addSystem(Stage::PreUpdate, &resolveGameAssets).name("resolveGameAssets");
}

} // namespace foh
