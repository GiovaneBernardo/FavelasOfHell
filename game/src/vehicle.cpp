#include "vehicle.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "foh/log.h"
#include "foh/world/city.h"
#include "player.h"

namespace foh::game {

namespace {

constexpr f32 kMaxDt = 0.05f;

Mat4 fromRows(const Vec4& r0, const Vec4& r1, const Vec4& r2) {
    return Mat4(Vec4(r0.x, r1.x, r2.x, 0.0f), Vec4(r0.y, r1.y, r2.y, 0.0f), Vec4(r0.z, r1.z, r2.z, 0.0f), Vec4(r0.w, r1.w, r2.w, 1.0f));
}

f32 angleDifference(f32 target, f32 current) {
    f32 d = std::fmod(target - current, 2.0f * math::kPi);
    if (d > math::kPi) d -= 2.0f * math::kPi;
    if (d < -math::kPi) d += 2.0f * math::kPi;
    return d;
}

// Yaw of the player's look convention (forward = (-sin, 0, -cos)) for a world direction.
f32 lookYawOf(const Vec3& direction) { return std::atan2(-direction.x, -direction.z); }

void gatherBoxes(std::vector<SolidBox>& boxes, Query<const GlobalTransform&, const StaticBox&>& solids) {
    boxes.clear();
    solids.each([&](const GlobalTransform& g, const StaticBox&) {
        const Vec3 half(0.5f * (std::abs(g.row0.x) + std::abs(g.row0.y) + std::abs(g.row0.z)), 0.5f * (std::abs(g.row1.x) + std::abs(g.row1.y) + std::abs(g.row1.z)),
                        0.5f * (std::abs(g.row2.x) + std::abs(g.row2.y) + std::abs(g.row2.z)));
        boxes.push_back({g.position(), half});
    });
}

} // namespace

// ---- setup ---------------------------------------------------------------------------------------------------------

void VehicleAttachSystem(Commands commands, Query<Entity, With<VehicleSetup>, Without<VehicleBody>> fresh) {
    fresh.each([&](Entity e) { commands.entity(e).insert(VehicleBody{}); });
}

// Props that ask for collision get an invisible box child. The player and the vehicles collide with StaticBox entities.
void PropColliderSystem(Commands commands, Query<Entity, const PropSetup&, Without<PropCollider>> props) {
    props.each([&](Entity e, const PropSetup& p) {
        commands.entity(e).insert(PropCollider{});
        if (p.body == static_cast<u32>(PropBody::Decoration)) return;
        Vec3 half = p.halfExtents;
        if (p.shape == static_cast<u32>(PropShape::Sphere)) half = Vec3(p.halfExtents.x);
        if (p.shape == static_cast<u32>(PropShape::Capsule)) half = Vec3(p.halfExtents.x, p.halfExtents.y + p.halfExtents.x, p.halfExtents.x);
        Transform t;
        t.position = p.center;
        t.scale = half * 2.0f;
        commands.spawn().insert(t).insert(Name::Make("Collider")).insert(StaticBox{}).setParent(e);
    });
}

// ---- getting in and out ----------------------------------------------------------------------------------------------

// Exclusive: moves the player between worlds of components (insert/remove InVehicle, Hidden, ...).
void VehicleEnterExitSystem(World& world, Res<GameInput> input, ResMut<PlayerLook> look, ResMut<PlayerView> view, ResMut<VehicleCamera> camera,
                            Res<WorldSurface> surface, Query<Entity, With<Player>> players,
                            Query<Entity, const VehicleSetup&, const Transform&> vehicles) {
    Entity player{};
    players.each([&](Entity e) { player = e; });
    if (player.isNull()) return;
    // The vehicle vanished under the driver (despawned from the editor): step out where it was.
    if (const InVehicle* seat = world.get<InVehicle>(player); seat && !world.isAlive(seat->vehicle)) {
        world.remove<InVehicle>(player);
        view->inVehicle = false;
        camera->active = false;
    }
    if (!input->captured || !input->state.wasPressed(Key::F)) return;
    const Transform* pt = world.get<Transform>(player);
    if (!pt) return;

    if (const InVehicle* seat = world.get<InVehicle>(player)) {
        // ---- get out ----
        const Entity vehicle = seat->vehicle;
        const VehicleSetup* setup = world.get<VehicleSetup>(vehicle);
        const Transform* vt = world.get<Transform>(vehicle);
        VehicleBody* body = world.get<VehicleBody>(vehicle);
        Transform out = *pt;
        if (setup && vt) {
            const Mat3 R = glm::mat3_cast(vt->rotation);
            out.position = vt->position + R * Vec3(setup->exitOffset.x, 0.0f, setup->exitOffset.z);
            if (surface->city) {
                const Vec2 pushed = surface->city->pushOutOfBuildings(Vec2(out.position.x, out.position.z), 0.4f);
                out.position.x = pushed.x;
                out.position.z = pushed.y;
            }
            out.position.y = std::max(surface->groundHeight(out.position.x, out.position.z), vt->position.y - 0.2f);
            if (!surface->terrain) out.position.y = vt->position.y;
            const Vec3 forward = R * Vec3(0.0f, 0.0f, 1.0f);
            look->yaw = lookYawOf(forward);
            out.rotation = glm::angleAxis(std::atan2(forward.x, forward.z), Vec3(0.0f, 1.0f, 0.0f));
        }
        if (body) {
            body->occupied = false;
            body->driver = Entity{};
        }
        *world.get<Transform>(player) = out;
        world.markModified<Transform>(player);
        if (Player* p = world.get<Player>(player)) {
            p->hasPrevious = false;
            p->grounded = true;
            p->verticalVelocity = 0.0f;
            p->jumpPending = false;
            p->landingTimer = 0.0f;
        }
        world.remove<InVehicle>(player);
        view->inVehicle = false;
        camera->active = false;
        return;
    }

    // ---- get in: the nearest vehicle within reach of its body ----
    Entity best{};
    f32 bestDistance = 1e9f;
    vehicles.each([&](Entity e, const VehicleSetup& s, const Transform& t) {
        const VehicleBody* body = world.get<VehicleBody>(e);
        if (!body || body->occupied) return;
        const Vec3 local = glm::conjugate(t.rotation) * (pt->position - t.position);
        const f32 dx = std::max(std::abs(local.x - s.colliderCenter.x) - s.colliderHalf.x, 0.0f);
        const f32 dz = std::max(std::abs(local.z - s.colliderCenter.z) - s.colliderHalf.z, 0.0f);
        const f32 distance = std::sqrt(dx * dx + dz * dz);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = e;
        }
    });
    if (best.isNull() || bestDistance > 2.8f) return;
    VehicleBody* body = world.get<VehicleBody>(best);
    const Transform* vt = world.get<Transform>(best);
    body->occupied = true;
    body->driver = player;
    body->handbrake = 0.0f;
    camera->heading = lookYawOf(glm::mat3_cast(vt->rotation) * Vec3(0, 0, 1));
    camera->yawOffset = camera->pitchOffset = 0.0f;
    camera->idleTime = 0.0f;
    camera->active = true;
    view->inVehicle = true;
    world.insert(player, InVehicle{best});
}

// ---- driving ---------------------------------------------------------------------------------------------------------

void VehicleControlSystem(Res<Time> time, Res<GameInput> input, Query<Entity, VehicleBody&> vehicles) {
    const f32 dt = std::min(time->delta, kMaxDt);
    const InputState& in = input->state;
    vehicles.each([&](Entity, VehicleBody& b) {
        if (!b.occupied) { // parked: handbrake on, nothing pressed
            b.throttle = b.brake = b.steerInput = 0.0f;
            b.handbrake = 1.0f;
            b.reverse = false;
            return;
        }
        const bool live = input->captured;
        const bool w = live && in.isDown(Key::W), s = live && in.isDown(Key::S);
        const f32 steer = (live && in.isDown(Key::A) ? 1.0f : 0.0f) - (live && in.isDown(Key::D) ? 1.0f : 0.0f);
        // W goes forward (or brakes when rolling backwards); S brakes when rolling forward and reverses once nearly stopped.
        if (w && !s) {
            b.reverse = false;
            b.throttle = b.forwardSpeed < -1.5f ? 0.0f : 1.0f;
            b.brake = b.forwardSpeed < -1.5f ? 1.0f : 0.0f;
        } else if (s && !w) {
            if (b.forwardSpeed > 1.5f) {
                b.throttle = 0.0f;
                b.brake = 1.0f;
            } else {
                b.reverse = true;
                b.throttle = 1.0f;
                b.brake = 0.0f;
            }
        } else {
            b.throttle = 0.0f;
            b.brake = 0.0f;
            if (b.speed < 0.5f) b.reverse = false;
        }
        b.handbrake = live && in.isDown(Key::Space) ? 1.0f : 0.0f;
        // Steering input eases in and out so keys feel like a wheel.
        const f32 rate = steer == 0.0f ? 6.0f : 4.0f;
        b.steerInput += (steer - b.steerInput) * (1.0f - std::exp(-rate * dt));
        // Turn it back onto its wheels.
        if (live && in.wasPressed(Key::R) && b.upsideDownTime > 0.5f) b.fresh = true;
    });
}

void VehicleDynamicsSystem(Res<Time> time, Res<WorldSurface> surface, Local<std::vector<SolidBox>> boxes,
                           Query<const GlobalTransform&, const StaticBox&> solids, Query<Transform&, const VehicleSetup&, VehicleBody&> vehicles) {
    const f32 dt = std::min(time->delta, kMaxDt);
    gatherBoxes(*boxes, solids);
    const VehicleWorld world{surface.get(), std::span<const SolidBox>(boxes->data(), boxes->size())};
    vehicles.each([&](Transform& t, const VehicleSetup& setup, VehicleBody& body) {
        if (body.fresh) { // a new vehicle, or one being righted: stand it on its wheels where it is
            const f32 yaw = std::atan2(glm::mat3_cast(t.rotation)[2].x, glm::mat3_cast(t.rotation)[2].z);
            t.rotation = glm::angleAxis(yaw, Vec3(0.0f, 1.0f, 0.0f));
            t.position.y += 0.8f;
            PlaceVehicle(setup, body, t, world);
            return;
        }
        StepVehicle(setup, body, t, world, dt);
    });
}

// Exclusive: poses the wheel meshes (steering, spin, suspension) and seats the driver.
void VehicleVisualSystem(World& world, Query<Entity, const VehicleSetup&, const VehicleBody&> vehicles) {
    struct Item {
        Entity vehicle;
    };
    std::vector<Item> items;
    vehicles.each([&](Entity e, const VehicleSetup&, const VehicleBody&) { items.push_back({e}); });
    for (const Item& item : items) {
        const VehicleSetup* setup = world.get<VehicleSetup>(item.vehicle);
        const VehicleBody* body = world.get<VehicleBody>(item.vehicle);
        const Transform* vt = world.get<Transform>(item.vehicle);
        if (!setup || !body || !vt) continue;
        for (u32 i = 0; i < setup->wheelCount; ++i) {
            const WheelSetup& w = setup->wheels[i];
            const Vec3 c = w.position;
            const Mat4 pose = glm::translate(Mat4(1.0f), c + Vec3(0.0f, body->suspension[i], 0.0f)) *
                              glm::rotate(Mat4(1.0f), body->wheelSteer[i], Vec3(0.0f, 1.0f, 0.0f)) *
                              glm::rotate(Mat4(1.0f), body->wheelAngle[i], Vec3(1.0f, 0.0f, 0.0f)) * glm::translate(Mat4(1.0f), -c);
            for (const WheelNode& n : w.nodes) {
                if (n.entity.isNull() || !world.isAlive(n.entity)) continue;
                Transform* node = world.get<Transform>(n.entity);
                if (!node) continue;
                const Mat4 parent = fromRows(n.parentRow0, n.parentRow1, n.parentRow2);
                const Mat4 rest = Transform{n.restPosition, n.restRotation, n.restScale}.matrix();
                *node = Transform::FromMatrix(glm::inverse(parent) * pose * parent * rest);
                world.markModified<Transform>(n.entity);
            }
        }
        if (body->occupied && !body->driver.isNull() && world.isAlive(body->driver)) {
            if (Transform* d = world.get<Transform>(body->driver)) {
                d->position = vt->position + glm::mat3_cast(vt->rotation) * setup->driverSeat;
                d->rotation = vt->rotation;
                world.markModified<Transform>(body->driver);
            }
        }
    }
}

// Chase camera: behind the car, swinging after it, with the mouse to look around.
void VehicleCameraSystem(Res<Time> time, Res<GameInput> input, ResMut<VehicleCamera> cam, Res<PlayerView> view, Res<WorldSurface> surface,
                         Query<const Transform&, const VehicleSetup&, const VehicleBody&> vehicles, Query<Transform&, Camera&, const PlayerCamera&> cameras) {
    if (!view->inVehicle || !cam->active) return;
    const f32 dt = std::min(time->delta, kMaxDt);
    const Transform* vt = nullptr;
    const VehicleSetup* vs = nullptr;
    const VehicleBody* vb = nullptr;
    vehicles.each([&](const Transform& t, const VehicleSetup& s, const VehicleBody& b) {
        if (!b.occupied) return;
        vt = &t;
        vs = &s;
        vb = &b;
    });
    if (!vt) return;

    // Where the car points, with a pull toward where it travels when it is sliding.
    const Vec3 carForward = glm::mat3_cast(vt->rotation) * Vec3(0, 0, 1);
    f32 targetHeading = lookYawOf(carForward);
    if (vb->speed > 4.0f && vb->forwardSpeed > 0.0f) {
        const f32 travel = lookYawOf(glm::normalize(Vec3(vb->velocity.x, 0.0f, vb->velocity.z)));
        targetHeading += angleDifference(travel, targetHeading) * 0.4f;
    }
    cam->heading += angleDifference(targetHeading, cam->heading) * (1.0f - std::exp(-3.5f * dt));

    const InputState& in = input->state;
    const bool moved = input->captured && (std::abs(in.mouseDeltaX) + std::abs(in.mouseDeltaY) > 0.5f);
    f32 sensitivity = 0.0022f;
    cameras.each([&](Transform&, Camera&, const PlayerCamera& c) { sensitivity = c.sensitivity; });
    if (moved) {
        cam->yawOffset -= in.mouseDeltaX * sensitivity;
        cam->pitchOffset = std::clamp(cam->pitchOffset - in.mouseDeltaY * sensitivity, -0.5f, 0.9f);
        cam->idleTime = 0.0f;
    } else {
        cam->idleTime += dt;
        if (cam->idleTime > 1.5f) {
            const f32 k = std::exp(-2.5f * dt);
            cam->yawOffset *= k;
            cam->pitchOffset *= k;
        }
    }

    cameras.each([&](Transform& t, Camera& camera, const PlayerCamera&) {
        const f32 yaw = cam->heading + cam->yawOffset;
        const Vec3 forward(-std::sin(yaw), 0.0f, -std::cos(yaw)); // where the camera looks, on the ground
        const f32 distance = vs->cameraDistance + std::min(vb->speed * 0.04f, 2.0f);
        const Vec3 pivot = vt->position + Vec3(0.0f, 1.1f, 0.0f);
        const f32 elevation = std::atan2(vs->cameraHeight - 1.1f, vs->cameraDistance) + cam->pitchOffset;
        t.position = pivot - forward * (distance * std::cos(elevation)) + Vec3(0.0f, distance * std::sin(elevation), 0.0f);
        const f32 ground = (surface->terrain ? surface->groundHeight(t.position.x, t.position.z) : vt->position.y - 0.5f) + 0.35f;
        t.position.y = std::max(t.position.y, ground);
        const Vec3 target = pivot + glm::mat3_cast(vt->rotation) * Vec3(0, 0, 1) * std::min(vb->speed * 0.08f, 2.5f);
        t.rotation = glm::quatLookAt(glm::normalize(target - t.position), Vec3(0, 1, 0));
        const f32 fov = vs->cameraFov + std::min(vb->speed / 60.0f, 0.5f) * 0.3f;
        camera.fovY += (fov - camera.fovY) * (1.0f - std::exp(-6.0f * dt));
    });
}

// The vehicle the B key spawns until another one is selected (the editor's Game Assets window, or the Forge's test drive).
constexpr const char* kDefaultVehicle = "vehicles/placeholder_sedan/placeholder_sedan.fvehicle";

// B: a vehicle appears in front of the player.
void VehicleSpawnSystem(Res<GameInput> input, Res<PlayerLook> look, Res<PlayerView> view, Res<SpawnSelection> selection, Res<WorldSurface> surface,
                        Commands commands) {
    if (!input->captured || !input->state.wasPressed(Key::B) || !view->valid || view->inVehicle) return;
    const Vec3 forward(-std::sin(look->yaw), 0.0f, -std::cos(look->yaw));
    Transform t;
    t.position = view->position + forward * 7.0f;
    t.position.y = (surface->terrain ? surface->groundHeight(t.position.x, t.position.z) : view->position.y) + 0.3f;
    t.rotation = glm::angleAxis(std::atan2(forward.x, forward.z), Vec3(0.0f, 1.0f, 0.0f)); // the vehicle's +Z along the view
    commands.spawn().insert(t).insert(Name::Make("Vehicle")).insert(MakeGameAssetRequest(selection->vehicle[0] ? selection->vehicle : kDefaultVehicle));
}

void RegisterVehicles(App& app) {
    app.registerComponent<VehicleBody>();
    app.registerComponent<InVehicle>();
    app.registerComponent<PropCollider>();
    if (!app.world().resource<VehicleCamera>()) app.insertResource(VehicleCamera{});
    app.addSystem(Stage::Update, &VehicleAttachSystem).name("VehicleAttachSystem");
    app.addSystem(Stage::Update, &PropColliderSystem).name("PropColliderSystem");
    app.addSystem(Stage::Update, &VehicleEnterExitSystem).name("VehicleEnterExitSystem");
    app.addSystem(Stage::Update, &VehicleSpawnSystem).name("VehicleSpawnSystem");
    app.addSystem(Stage::Update, &VehicleControlSystem).name("VehicleControlSystem");
    app.addSystem(Stage::Update, &VehicleDynamicsSystem).name("VehicleDynamicsSystem").after(&VehicleControlSystem);
    app.addSystem(Stage::Update, &VehicleVisualSystem).name("VehicleVisualSystem").after(&VehicleDynamicsSystem);
    app.addSystem(Stage::Update, &VehicleCameraSystem).name("VehicleCameraSystem").after(&VehicleVisualSystem);
}

} // namespace foh::game
