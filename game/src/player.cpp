#include "player.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "foh/log.h"
#include "foh/scene/prefab_request.h"
#include "foh/world/city.h"
#include "foh/world/surface.h"

namespace foh::game {

namespace {

constexpr f32 kMaxDt = 0.05f;

struct Box {
    Vec3 center, half;
};

f32 smoothTowards(f32 value, f32 target, f32 rate, f32 dt) { return value + (target - value) * (1.0f - std::exp(-rate * dt)); }

// Height of the highest thing under the point that can be stood on (a ledge no higher than a step above the feet).
f32 supportHeight(const std::vector<Box>& boxes, const Vec3& p, f32 feetY, f32 step) {
    f32 height = 0.0f;
    for (const Box& b : boxes) {
        const f32 top = b.center.y + b.half.y;
        if (top > feetY + step) continue;
        if (std::abs(p.x - b.center.x) > b.half.x || std::abs(p.z - b.center.z) > b.half.z) continue;
        height = std::max(height, top);
    }
    return height;
}

// Pushes a standing circle (feet position `p`, radius `r`, spanning feetY..headY vertically) out of the boxes it
// overlaps. Boxes whose top is within a step of the feet are walked onto instead. Returns the accumulated push direction.
Vec2 pushOut(Vec3& p, f32 r, f32 feetY, f32 headY, f32 step, const std::vector<Box>& boxes) {
    Vec2 normal(0.0f);
    for (int pass = 0; pass < 2; ++pass) {
        for (const Box& b : boxes) {
            const f32 top = b.center.y + b.half.y, bottom = b.center.y - b.half.y;
            if (top <= feetY + step || bottom >= headY) continue;
            const f32 cx = std::clamp(p.x, b.center.x - b.half.x, b.center.x + b.half.x);
            const f32 cz = std::clamp(p.z, b.center.z - b.half.z, b.center.z + b.half.z);
            const f32 dx = p.x - cx, dz = p.z - cz;
            const f32 d2 = dx * dx + dz * dz;
            if (d2 >= r * r) continue;
            Vec2 n;
            f32 push;
            if (d2 > 1e-8f) {
                const f32 d = std::sqrt(d2);
                n = Vec2(dx / d, dz / d);
                push = r - d;
            } else {
                // The center is inside the box: leave through the nearest face.
                const f32 ox = b.half.x + r - std::abs(p.x - b.center.x), oz = b.half.z + r - std::abs(p.z - b.center.z);
                if (ox < oz) {
                    n = Vec2(p.x < b.center.x ? -1.0f : 1.0f, 0.0f);
                    push = ox;
                } else {
                    n = Vec2(0.0f, p.z < b.center.z ? -1.0f : 1.0f);
                    push = oz;
                }
            }
            p.x += n.x * push;
            p.z += n.y * push;
            normal += n;
        }
    }
    return normal;
}

} // namespace

// ---- look ----------------------------------------------------------------------------------------

void PlayerLookSystem(Res<GameInput> input, ResMut<PlayerLook> look, Query<const PlayerCamera&> cameras) {
    if (!input->captured) return;
    f32 sensitivity = 0.0022f;
    cameras.each([&](const PlayerCamera& c) { sensitivity = c.sensitivity; });
    const InputState& in = input->state;
    if (in.wasPressed(Key::V)) look->firstPerson = !look->firstPerson;
    look->yaw -= in.mouseDeltaX * sensitivity;
    // First person may look straight up and down; third person keeps the camera above the ground.
    const f32 lo = look->firstPerson ? -1.45f : -1.1f, hi = look->firstPerson ? 1.45f : 0.5f;
    look->pitch = std::clamp(look->pitch - in.mouseDeltaY * sensitivity, lo, hi);
}

// ---- movement ------------------------------------------------------------------------------------

void PlayerMoveSystem(Res<Time> time, Res<GameInput> input, Res<PlayerLook> look, ResMut<PlayerView> view, Res<WorldSurface> surface, Local<std::vector<Box>> boxes,
                      Query<Entity, Transform&, Player&, LocomotionControl&> players,
                      Query<const LocomotionState&, With<Player>> animationState,
                      Query<const GlobalTransform&, const StaticBox&> solids) {
    const f32 dt = std::min(time->delta, kMaxDt);
    // What the animation system says about the player (when it has a model to animate): the instant the feet leave
    // the ground, and the smoothed velocity the body is really moving with.
    bool takeOff = false, haveState = false;
    Vec3 actualVelocity(0.0f);
    animationState.each([&](const LocomotionState& s) {
        haveState = true;
        takeOff = takeOff || s.takeOffNow;
        actualVelocity = s.velocity;
    });
    boxes->clear();
    solids.each([&](const GlobalTransform& g, const StaticBox&) {
        // Axis aligned bounds of the transformed unit cube.
        const Vec3 half(0.5f * (std::abs(g.row0.x) + std::abs(g.row0.y) + std::abs(g.row0.z)),
                        0.5f * (std::abs(g.row1.x) + std::abs(g.row1.y) + std::abs(g.row1.z)),
                        0.5f * (std::abs(g.row2.x) + std::abs(g.row2.y) + std::abs(g.row2.z)));
        boxes->push_back({g.position(), half});
    });

    const InputState& in = input->state;
    const bool live = input->captured;
    players.each([&](Entity entity, Transform& t, Player& p, LocomotionControl& control) {
        // ---- what the player asks for ----
        Vec2 wish(0.0f);
        if (live) {
            wish.x = (in.isDown(Key::D) ? 1.0f : 0.0f) - (in.isDown(Key::A) ? 1.0f : 0.0f);
            wish.y = (in.isDown(Key::W) ? 1.0f : 0.0f) - (in.isDown(Key::S) ? 1.0f : 0.0f);
            if (glm::dot(wish, wish) > 1.0f) wish = glm::normalize(wish);
            if (p.crouchToggle) {
                if (in.wasPressed(Key::C)) p.crouching = !p.crouching;
            } else {
                p.crouching = in.isDown(Key::C);
            }
        } else if (!p.crouchToggle) {
            p.crouching = false;
        }
        p.crouch01 = smoothTowards(p.crouch01, p.crouching ? 1.0f : 0.0f, 10.0f, dt);

        // Camera relative directions on the ground (the camera looks down -Z at yaw 0).
        const Vec3 forward(-std::sin(look->yaw), 0.0f, -std::cos(look->yaw));
        const Vec3 right(std::cos(look->yaw), 0.0f, -std::sin(look->yaw));
        // Walk by default, run while Shift is held (the speeds match the authored walk and run clips).
        const f32 maxSpeed = p.crouching ? p.crouchSpeed : (live && in.isDown(Key::LShift) ? p.runSpeed : p.walkSpeed);
        Vec3 wishVelocity = (forward * wish.y + right * wish.x) * maxSpeed;
        Vec3 velocity = wishVelocity;

        // ---- jumping ----
        // Space starts the wind-up. The direction is locked from that moment on; the body leaves the ground when the
        // animation springs (takeOffNow), or after a fallback delay when there is no jump animation.
        if (live && in.wasPressed(Key::Space) && p.grounded && !p.crouching && !p.jumpPending && p.landingTimer <= 0.0f) {
            control.jump = true;
            p.jumpPending = true;
            p.jumpTimer = p.jumpFallbackDelay;
            p.lockedVelocity = haveState ? actualVelocity : velocity;
            p.lockedFacing = look->firstPerson ? forward : (glm::length(p.lockedVelocity) > 0.3f ? glm::normalize(p.lockedVelocity) : Vec3(0.0f));
        }
        if (p.jumpPending) {
            p.jumpTimer -= dt;
            if (takeOff || p.jumpTimer < 0.0f) {
                p.verticalVelocity = p.jumpSpeed;
                p.grounded = false;
                p.jumpPending = false;
            }
        }
        const bool airborne = !p.grounded;
        if (airborne || p.jumpPending) velocity = p.lockedVelocity; // no steering in the air or during the wind-up
        if (p.landingTimer > 0.0f) {
            // Landing keeps the momentum, then the controls blend back in.
            p.landingTimer = std::max(0.0f, p.landingTimer - dt);
            const f32 t = 1.0f - p.landingTimer / std::max(p.landingRecovery, 1e-3f);
            const f32 s = t * t * (3.0f - 2.0f * t);
            velocity = p.lockedVelocity * p.landingMomentum * (1.0f - s) + wishVelocity * s;
        }

        // ---- collision and ground ----
        // The locomotion system (which ran before this) has already moved the entity horizontally by its smoothed
        // velocity; walls are resolved on top of that, and the wish velocity loses the part pointing into them so the
        // run animation does not play against a wall.
        const f32 height = p.standHeight + (p.crouchHeight - p.standHeight) * p.crouch01;
        Vec2 normal = pushOut(t.position, p.radius, t.position.y, t.position.y + height, p.stepHeight, *boxes);

        // Terrain: cliffs are walls (too steep to walk up: slide along the contour), tree trunks are solid.
        if (surface->terrain && p.hasPrevious) {
            const Vec2 prev(p.previousPosition.x, p.previousPosition.z);
            const Vec2 delta = Vec2(t.position.x, t.position.z) - prev;
            const f32 moved = glm::length(delta);
            if (moved > 1e-4f) {
                const f32 rise = surface->groundHeight(t.position.x, t.position.z) - surface->groundHeight(prev.x, prev.y);
                if (rise / moved > p.maxSlope && rise > 0.02f) {
                    const Vec3 n = surface->terrain->normal(t.position.x, t.position.z);
                    const Vec2 uphill = glm::length(Vec2(n.x, n.z)) > 1e-4f ? glm::normalize(Vec2(-n.x, -n.z)) : Vec2(0.0f);
                    const f32 into = glm::dot(delta, uphill);
                    if (into > 0.0f) {
                        const Vec2 slid = delta - uphill * into; // keep only the part along the contour
                        t.position.x = prev.x + slid.x;
                        t.position.z = prev.y + slid.y;
                        normal -= uphill;
                    }
                }
            }
        }
        if (surface->vegetation) {
            const Vec2 before(t.position.x, t.position.z);
            const Vec2 after = surface->vegetation->pushOutOfTrunks(before, p.radius);
            if (glm::dot(after - before, after - before) > 1e-10f) {
                t.position.x = after.x;
                t.position.z = after.y;
                normal += glm::normalize(after - before);
            }
        }
        if (surface->city) { // buildings are solid (ADR 0014)
            const Vec2 before(t.position.x, t.position.z);
            const Vec2 after = surface->city->pushOutOfBuildings(before, p.radius);
            if (glm::dot(after - before, after - before) > 1e-10f) {
                t.position.x = after.x;
                t.position.z = after.y;
                normal += glm::normalize(after - before);
            }
        }
        p.previousPosition = t.position;
        p.hasPrevious = true;
        if (glm::dot(normal, normal) > 1e-6f) {
            const Vec2 n = glm::normalize(normal);
            const f32 into = velocity.x * n.x + velocity.z * n.y;
            if (into < 0.0f) {
                velocity.x -= n.x * into;
                velocity.z -= n.y * into;
            }
        }
        f32 ground = supportHeight(*boxes, t.position, t.position.y, p.stepHeight);
        if (surface->terrain) ground = std::max(ground, surface->groundHeight(t.position.x, t.position.z)); // the terrain is the floor under everything
        if (p.grounded) {
            if (t.position.y - ground > p.stepHeight) p.grounded = false; // walked off a ledge
            else t.position.y = ground;
        }
        if (!p.grounded) {
            p.verticalVelocity -= p.gravity * dt;
            t.position.y += p.verticalVelocity * dt;
            if (t.position.y <= ground && p.verticalVelocity <= 0.0f) {
                t.position.y = ground;
                p.verticalVelocity = 0.0f;
                p.grounded = true;
                p.landingTimer = p.landingRecovery; // see above: momentum for a moment
            }
        }

        // ---- what the animation gets ----
        control.velocity = velocity;
        control.crouch = p.crouching;
        control.kinematic = true;
        control.grounded = p.grounded;
        control.expectedAirTime = 2.0f * p.jumpSpeed / std::max(p.gravity, 1.0f);
        // First person: the body follows the camera heading (strafing); third person: it faces where it runs. In the
        // air and while landing it keeps the heading it had.
        control.facing = look->firstPerson ? forward : Vec3(0.0f);
        if (p.jumpPending || airborne || p.landingTimer > 0.0f) control.facing = p.lockedFacing;

        view->position = t.position;
        view->crouch01 = p.crouch01;
        view->entity = entity.id;
        view->valid = true;
    });
}

// ---- camera --------------------------------------------------------------------------------------

void PlayerCameraSystem(Res<Time> time, Res<PlayerLook> look, ResMut<PlayerView> view, Res<WorldSurface> surface, Commands commands,
                        Query<Entity, With<SlotModelPart>, Without<Hidden>> shownParts,
                        Query<Entity, With<SlotModelPart>, With<Hidden>> hiddenParts,
                        Query<Transform&, Camera&, const PlayerCamera&> cameras) {
    if (!view->valid) return;
    const f32 dt = std::min(time->delta, kMaxDt);
    const Quat rotation = glm::angleAxis(look->yaw, Vec3(0, 1, 0)) * glm::angleAxis(look->pitch, Vec3(1, 0, 0));

    // First person hides the player's own body (the animation keeps running; the renderer just skips it).
    if (look->firstPerson != view->hidden) {
        view->hidden = look->firstPerson;
        if (view->entity != 0) {
            if (view->hidden) commands.entity(Entity{view->entity}).insert(Hidden{});
            else commands.entity(Entity{view->entity}).remove<Hidden>();
        }
    }

    // The parts of the model (static meshes of an unrigged model, skinned meshes) follow the same rule; checked every
    // frame so a model swapped while in first person comes up hidden too.
    if (look->firstPerson) shownParts.each([&](Entity e) { commands.entity(e).insert(Hidden{}); });
    else hiddenParts.each([&](Entity e) { commands.entity(e).remove<Hidden>(); });

    cameras.each([&](Transform& t, Camera& camera, const PlayerCamera& settings) {
        const f32 crouch = view->crouch01;
        if (look->firstPerson) {
            const f32 eye = 1.62f + (1.12f - 1.62f) * crouch;
            const Vec3 forwardOnGround(-std::sin(look->yaw), 0.0f, -std::cos(look->yaw));
            t.position = view->position + Vec3(0.0f, eye, 0.0f) + forwardOnGround * 0.14f;
            view->pivotValid = false;
            camera.fovY = smoothTowards(camera.fovY, settings.fovFirstPerson, 12.0f, dt);
        } else {
            // Over the shoulder (GTA 5): the pivot sits above and to the side of the player and trails it a little, so
            // running does not jitter the view; the camera orbits the pivot at a fixed distance.
            const f32 pivotHeight = 1.55f + (1.05f - 1.55f) * crouch;
            const Vec3 target = view->position + Vec3(0.0f, pivotHeight, 0.0f);
            if (!view->pivotValid) view->pivot = target;
            view->pivotValid = true;
            const f32 k = 1.0f - std::exp(-settings.followRate * dt);
            view->pivot += (target - view->pivot) * k;
            const Vec3 right = rotation * Vec3(1, 0, 0), back = rotation * Vec3(0, 0, 1);
            t.position = view->pivot + right * settings.shoulder + back * settings.distance;
            t.position.y = std::max(t.position.y, (surface->terrain ? surface->groundHeight(t.position.x, t.position.z) : 0.0f) + 0.25f); // not below the ground
            camera.fovY = smoothTowards(camera.fovY, settings.fovThirdPerson, 12.0f, dt);
        }
        t.rotation = rotation;
    });
}

void RegisterPlayer(App& app) {
    app.registerComponent<Player>();
    app.registerComponent<PlayerCamera>();
    app.registerComponent<StaticBox>();
    if (!app.world().resource<WorldSurface>()) app.insertResource(WorldSurface{}); // the host fills it where there is terrain
    if (!app.world().resource<PlayerLook>()) app.insertResource(PlayerLook{});
    if (!app.world().resource<PlayerView>()) app.insertResource(PlayerView{});
    if (!app.world().resource<GameInput>()) app.insertResource(GameInput{}); // hosts replace it every frame
    app.addSystem(Stage::Update, &PlayerLookSystem).name("PlayerLookSystem");
    app.addSystem(Stage::Update, &PlayerMoveSystem).name("PlayerMoveSystem");
    app.addSystem(Stage::Update, &PlayerCameraSystem).name("PlayerCameraSystem").after(&PlayerMoveSystem);
}

} // namespace foh::game
