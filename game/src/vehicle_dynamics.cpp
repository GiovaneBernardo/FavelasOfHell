#include "vehicle_dynamics.h"

#include <algorithm>
#include <cmath>

#include "foh/world/city.h"

namespace foh::game {

namespace {

constexpr f32 kGravity = 9.81f;
constexpr f32 kAirDensity = 1.2f;
constexpr f32 kMaxSubstep = 0.01f;
constexpr f32 kTwoPi = 6.2831853f;

f32 sign(f32 v) { return v < 0.0f ? -1.0f : 1.0f; }

// Per-vehicle numbers that do not change inside a step.
struct Derived {
    bool front[kMaxVehicleWheels] = {};
    i32 partner[kMaxVehicleWheels] = {}; // the wheel on the other side of the same axle (-1: none)
    f32 staticLoad[kMaxVehicleWheels] = {};
    f32 springRate[kMaxVehicleWheels] = {};
    f32 damping[kMaxVehicleWheels] = {};
    f32 wheelInertia[kMaxVehicleWheels] = {};
    f32 wheelbase = 1.0f, halfTrack = 0.7f;
    u32 driven = 0;
    Vec3 inertia{1.0f}; // body-space principal moments
};

Derived derive(const VehicleSetup& s) {
    Derived d;
    f32 minZ = s.wheels[0].position.z, maxZ = minZ, minX = s.wheels[0].position.x, maxX = minX;
    for (u32 i = 0; i < s.wheelCount; ++i) {
        minZ = std::min(minZ, s.wheels[i].position.z);
        maxZ = std::max(maxZ, s.wheels[i].position.z);
        minX = std::min(minX, s.wheels[i].position.x);
        maxX = std::max(maxX, s.wheels[i].position.x);
    }
    d.wheelbase = std::max(maxZ - minZ, 0.5f);
    d.halfTrack = std::max(0.5f * (maxX - minX), 0.3f);
    const f32 tolerance = 0.1f * d.wheelbase;
    u32 frontCount = 0, rearCount = 0;
    for (u32 i = 0; i < s.wheelCount; ++i) {
        d.front[i] = s.wheels[i].position.z >= maxZ - tolerance;
        (d.front[i] ? frontCount : rearCount)++;
        if (s.wheels[i].driven) ++d.driven;
    }
    // Weight distribution from where the centre of mass sits between the axles.
    const f32 frontShare = rearCount == 0 ? 1.0f : std::clamp((s.centerOfMass.z - minZ) / d.wheelbase, 0.2f, 0.8f);
    for (u32 i = 0; i < s.wheelCount; ++i) {
        const f32 share = d.front[i] ? frontShare / static_cast<f32>(std::max(frontCount, 1u)) : (1.0f - frontShare) / static_cast<f32>(std::max(rearCount, 1u));
        d.staticLoad[i] = s.mass * kGravity * share;
        const f32 sprung = s.mass * share;
        const f32 omega = kTwoPi * (d.front[i] ? s.frontFrequency : s.rearFrequency);
        d.springRate[i] = sprung * omega * omega;
        d.damping[i] = 2.0f * s.dampingRatio * std::sqrt(d.springRate[i] * sprung);
        const f32 r = s.wheels[i].radius;
        d.wheelInertia[i] = std::max(0.4f, 22.0f * r * r); // a ~22 kg wheel and tire
        d.partner[i] = -1;
        for (u32 j = 0; j < s.wheelCount; ++j)
            if (j != i && std::abs(s.wheels[j].position.z - s.wheels[i].position.z) < 0.15f * d.wheelbase &&
                s.wheels[j].position.x * s.wheels[i].position.x < 0.0f)
                d.partner[i] = static_cast<i32>(j);
    }
    const Vec3 dim = s.colliderHalf * 2.0f;
    d.inertia = Vec3(dim.y * dim.y + dim.z * dim.z, dim.x * dim.x + dim.z * dim.z, dim.x * dim.x + dim.y * dim.y) * (s.mass / 12.0f) * 0.9f;
    return d;
}

f32 engineTorque(const VehicleSetup& s, f32 rpm) {
    if (rpm >= s.redlineRpm) return 0.0f; // the rev limiter
    const f32 span = std::max(std::max(s.peakRpm - s.idleRpm, s.redlineRpm - s.peakRpm), 1.0f);
    const f32 x = (rpm - s.peakRpm) / span;
    return s.maxTorque * std::clamp(1.0f - 0.75f * x * x, 0.2f, 1.0f);
}

f32 gearRatio(const VehicleSetup& s, i32 gear) {
    if (gear < 0) return s.reverseGear * s.finalDrive;
    const u32 i = static_cast<u32>(std::clamp(gear, 1, static_cast<i32>(std::max(s.gearCount, 1u)))) - 1;
    return (s.gearCount ? s.gears[i] : 3.0f) * s.finalDrive;
}

// ---- obstacles ---------------------------------------------------------------------------------------------------

struct Contact {
    Vec2 normal{0.0f};
    f32 depth = 0.0f;
};

// Pushes a vertical circle out of everything solid. Returns the strongest contact (depth 0: none).
Contact collideCircle(const VehicleWorld& w, Vec2 centre, f32 radius, f32 bottom, f32 top, f32 clearance) {
    Contact best;
    auto consider = [&](Vec2 corrected) {
        const Vec2 d = corrected - centre;
        const f32 depth = glm::length(d);
        if (depth > best.depth && depth > 1e-5f) best = {d / depth, depth};
    };
    if (w.surface) {
        if (w.surface->city) consider(w.surface->city->pushOutOfBuildings(centre, radius));
        if (w.surface->vegetation) consider(w.surface->vegetation->pushOutOfTrunks(centre, radius));
        if (w.surface->terrain) {
            // A slope too steep to drive up is a wall: the ground under the bumper is higher than the underside plus clearance.
            const f32 h = w.surface->terrain->height(centre.x, centre.y);
            const f32 excess = h - (bottom + clearance);
            if (excess > 0.0f) {
                const Vec3 n = w.surface->terrain->normal(centre.x, centre.y);
                const Vec2 downhill(n.x, n.z);
                const f32 len = glm::length(downhill);
                if (len > 0.2f) best = (std::min(excess, 0.6f) > best.depth) ? Contact{downhill / len, std::min(excess, 0.6f) * 0.5f} : best;
            }
        }
    }
    for (const SolidBox& b : w.boxes) {
        const f32 boxTop = b.center.y + b.half.y, boxBottom = b.center.y - b.half.y;
        if (boxTop <= bottom + clearance || boxBottom >= top) continue; // low enough to drive over, or above the roof
        const f32 cx = std::clamp(centre.x, b.center.x - b.half.x, b.center.x + b.half.x);
        const f32 cz = std::clamp(centre.y, b.center.z - b.half.z, b.center.z + b.half.z);
        const Vec2 d = centre - Vec2(cx, cz);
        const f32 d2 = glm::dot(d, d);
        if (d2 >= radius * radius) continue;
        Vec2 n;
        f32 depth;
        if (d2 > 1e-8f) {
            const f32 dist = std::sqrt(d2);
            n = d / dist;
            depth = radius - dist;
        } else {
            const f32 ox = b.half.x + radius - std::abs(centre.x - b.center.x), oz = b.half.z + radius - std::abs(centre.y - b.center.z);
            if (ox < oz) {
                n = Vec2(centre.x < b.center.x ? -1.0f : 1.0f, 0.0f);
                depth = ox;
            } else {
                n = Vec2(0.0f, centre.y < b.center.z ? -1.0f : 1.0f);
                depth = oz;
            }
        }
        if (depth > best.depth) best = {n, depth};
    }
    return best;
}

void resolveBody(const VehicleSetup& s, VehicleBody& body, Transform& t, const VehicleWorld& world, const Derived& d) {
    const Mat3 R = glm::mat3_cast(t.rotation);
    const f32 radius = std::max(s.colliderHalf.x, 0.3f);
    const f32 reach = std::max(s.colliderHalf.z - radius, 0.0f);
    const int circles = reach <= 0.05f ? 1 : static_cast<int>(std::ceil(2.0f * reach / (radius * 1.3f))) + 1;
    const Vec3 up = R[1];
    for (int pass = 0; pass < 2; ++pass) {
        for (int c = 0; c < circles; ++c) {
            const f32 z = circles == 1 ? 0.0f : -reach + 2.0f * reach * static_cast<f32>(c) / static_cast<f32>(circles - 1);
            const Vec3 local(s.colliderCenter.x, s.colliderCenter.y, s.colliderCenter.z + z);
            const Vec3 p = t.position + R * local;
            const f32 bottom = p.y - s.colliderHalf.y * std::max(up.y, 0.3f), top = p.y + s.colliderHalf.y;
            const Contact hit = collideCircle(world, Vec2(p.x, p.z), radius, bottom, top, 0.32f);
            if (hit.depth <= 0.0f) continue;

            const Vec3 n(hit.normal.x, 0.0f, hit.normal.y);
            t.position += n * (hit.depth * 0.85f);
            const Vec3 comW = t.position + R * s.centerOfMass;
            // The impulse acts at the height of the centre of mass: a bumper hit shoves and turns the car, it does not tip it over
            // its nose (the tires and suspension would hold it down in reality).
            Vec3 arm = p - comW;
            arm.y = 0.0f;
            const Vec3 vContact = body.velocity + glm::cross(body.angularVelocity, arm);
            const f32 vn = glm::dot(vContact, n);
            if (vn >= 0.0f) continue;
            // Impulse along the normal with the body's inertia (world space).
            const Mat3 invI = R * Mat3(1.0f / d.inertia.x, 0, 0, 0, 1.0f / d.inertia.y, 0, 0, 0, 1.0f / d.inertia.z) * glm::transpose(R);
            const Vec3 rn = glm::cross(arm, n);
            const f32 denom = 1.0f / s.mass + glm::dot(glm::cross(invI * rn, arm), n);
            const f32 restitution = std::abs(vn) > 2.0f ? 0.18f : 0.0f;
            const f32 j = -(1.0f + restitution) * vn / denom;
            body.velocity += n * (j / s.mass);
            body.angularVelocity += invI * glm::cross(arm, n * j);
            // Scraping along the wall costs speed.
            const Vec3 tangent = body.velocity - n * glm::dot(body.velocity, n);
            body.velocity -= tangent * 0.04f;
        }
    }
}

} // namespace

// ---- world -------------------------------------------------------------------------------------------------------

f32 VehicleWorld::groundHeight(f32 x, f32 z, f32 reachY) const {
    f32 h = surface ? surface->groundHeight(x, z) : 0.0f;
    for (const SolidBox& b : boxes) {
        const f32 top = b.center.y + b.half.y;
        if (top > reachY || top <= h) continue;
        if (std::abs(x - b.center.x) > b.half.x || std::abs(z - b.center.z) > b.half.z) continue;
        h = top;
    }
    return h;
}

Vec3 VehicleWorld::groundNormal(f32 x, f32 z) const {
    if (surface && surface->terrain) return surface->terrain->normal(x, z);
    return Vec3(0.0f, 1.0f, 0.0f);
}

// ---- the step ----------------------------------------------------------------------------------------------------

void PlaceVehicle(const VehicleSetup& s, VehicleBody& body, Transform& t, const VehicleWorld& world) {
    // Rest height: the lowest wheel's centre is `radius` above the ground. Level the car on its own yaw.
    const f32 yaw = std::atan2(2.0f * (t.rotation.w * t.rotation.y + t.rotation.x * t.rotation.z), 1.0f - 2.0f * (t.rotation.y * t.rotation.y + t.rotation.x * t.rotation.x));
    t.rotation = glm::angleAxis(yaw, Vec3(0.0f, 1.0f, 0.0f));
    const Mat3 R = glm::mat3_cast(t.rotation);
    f32 lift = -1e9f;
    for (u32 i = 0; i < s.wheelCount; ++i) {
        const Vec3 hub = R * s.wheels[i].position;
        const f32 ground = world.groundHeight(t.position.x + hub.x, t.position.z + hub.z, t.position.y + 50.0f);
        lift = std::max(lift, ground + s.wheels[i].radius - hub.y);
    }
    t.position.y = lift + 0.02f;
    body.velocity = Vec3(0.0f);
    body.angularVelocity = Vec3(0.0f);
    body.fresh = false;
}

void StepVehicle(const VehicleSetup& s, VehicleBody& body, Transform& t, const VehicleWorld& world, f32 dt) {
    if (s.wheelCount < 3 || s.mass < 1.0f || dt <= 0.0f) return;
    if (body.fresh) PlaceVehicle(s, body, t, world);
    const Derived d = derive(s);

    // ---- driver intent and the gearbox (once per frame) ----
    if (body.reverse && body.gear > 0) body.gear = -1;
    if (!body.reverse && body.gear < 0) body.gear = 1;
    body.shiftTimer = std::max(0.0f, body.shiftTimer - dt);
    f32 drivenSpeed = 0.0f;
    if (d.driven > 0) {
        for (u32 i = 0; i < s.wheelCount; ++i)
            if (s.wheels[i].driven) drivenSpeed += body.wheelSpeed[i];
        drivenSpeed /= static_cast<f32>(d.driven);
    }
    {
        const f32 wheelRpm = std::abs(drivenSpeed) * gearRatio(s, body.gear) * 60.0f / kTwoPi;
        const f32 revving = s.idleRpm + body.throttle * 0.35f * (s.redlineRpm - s.idleRpm); // the engine runs ahead of the wheels while the clutch slips
        body.rpm = std::clamp(std::max(wheelRpm, revving), s.idleRpm, s.redlineRpm * 1.02f);
        // The gearbox shifts by the speed over the ground, not by wheel spin (a spinning wheel must not cause an upshift), and
        // never twice within half a second.
        body.sinceShift += dt;
        if (body.gear > 0 && body.shiftTimer <= 0.0f && body.sinceShift > 0.5f) {
            f32 radius = 0.0f;
            u32 n = 0;
            for (u32 i = 0; i < s.wheelCount; ++i)
                if (s.wheels[i].driven) {
                    radius += s.wheels[i].radius;
                    ++n;
                }
            radius = n ? radius / static_cast<f32>(n) : 0.33f;
            const f32 groundRpm = std::abs(body.forwardSpeed) / std::max(radius, 0.05f) * gearRatio(s, body.gear) * 60.0f / kTwoPi;
            if (groundRpm > 0.86f * s.redlineRpm && body.gear < static_cast<i32>(s.gearCount) && body.throttle > 0.2f) {
                ++body.gear;
                body.shiftTimer = 0.2f;
                body.sinceShift = 0.0f;
            } else if (body.gear > 1 && groundRpm < 0.30f * s.redlineRpm) {
                const f32 after = groundRpm * gearRatio(s, body.gear - 1) / gearRatio(s, body.gear);
                if (after < 0.78f * s.redlineRpm) {
                    --body.gear;
                    body.shiftTimer = 0.15f;
                    body.sinceShift = 0.0f;
                }
            }
        }
    }
    f32 wheelTorqueTotal = 0.0f;
    if (d.driven > 0 && body.shiftTimer <= 0.0f) {
        const f32 ratio = gearRatio(s, body.gear);
        const f32 direction = body.gear < 0 ? -1.0f : 1.0f;
        f32 torque;
        if (body.throttle > 0.02f) torque = engineTorque(s, body.rpm) * body.throttle;
        else torque = std::abs(drivenSpeed) > 0.5f ? -0.14f * s.maxTorque * (body.rpm / s.redlineRpm) * sign(drivenSpeed * direction) : 0.0f; // engine braking
        wheelTorqueTotal = torque * ratio * s.drivelineEfficiency * direction;
    }

    // Steering: speed sensitive, with Ackermann geometry (the wheel on the inside of the turn turns more).
    const f32 speedFactor = 1.0f + (s.highSpeedSteer - 1.0f) * std::clamp(body.speed / 40.0f, 0.0f, 1.0f);
    const f32 steerTarget = body.steerInput * s.maxSteerRadians * speedFactor;
    body.steer += (steerTarget - body.steer) * (1.0f - std::exp(-s.steerSpeed * dt));
    for (u32 i = 0; i < s.wheelCount; ++i) {
        body.wheelSteer[i] = 0.0f;
        if (!s.wheels[i].steered || std::abs(body.steer) < 1e-4f) continue;
        const f32 turnRadius = d.wheelbase / std::tan(std::abs(body.steer));
        const f32 inside = s.wheels[i].position.x * sign(body.steer) > 0.0f ? -1.0f : 1.0f;
        body.wheelSteer[i] = std::atan(d.wheelbase / std::max(turnRadius + inside * std::abs(s.wheels[i].position.x), 0.1f)) * sign(body.steer);
    }

    // ---- sub-steps ----
    const int steps = std::clamp(static_cast<int>(std::ceil(dt / kMaxSubstep)), 1, 12);
    const f32 h = dt / static_cast<f32>(steps);
    const Mat3 inverseInertiaBody(1.0f / d.inertia.x, 0, 0, 0, 1.0f / d.inertia.y, 0, 0, 0, 1.0f / d.inertia.z);

    for (int step = 0; step < steps; ++step) {
        const Mat3 R = glm::mat3_cast(t.rotation);
        const Vec3 up = R[1];
        const Vec3 comW = t.position + R * s.centerOfMass;
        Vec3 force(0.0f, -kGravity * s.mass, 0.0f);
        Vec3 torque(0.0f);
        f32 compression[kMaxVehicleWheels] = {};
        f32 rawCompression[kMaxVehicleWheels] = {};
        bool contact[kMaxVehicleWheels] = {};
        Vec3 groundPoint[kMaxVehicleWheels];
        Vec3 groundNormal[kMaxVehicleWheels];

        // Suspension geometry first (the anti-roll bar needs both sides of an axle).
        for (u32 i = 0; i < s.wheelCount; ++i) {
            const WheelSetup& w = s.wheels[i];
            const Vec3 hubRest = t.position + R * w.position;
            const f32 gy = world.groundHeight(hubRest.x, hubRest.z, hubRest.y);
            const f32 distance = (hubRest.y - gy) / std::max(up.y, 0.25f);
            rawCompression[i] = w.radius - distance;
            contact[i] = rawCompression[i] > -s.travelDown;
            compression[i] = std::clamp(rawCompression[i], -s.travelDown, s.travelUp);
            groundPoint[i] = Vec3(hubRest.x, gy, hubRest.z);
            groundNormal[i] = world.groundNormal(hubRest.x, hubRest.z);
        }

        u32 grounded = 0;
        for (u32 i = 0; i < s.wheelCount; ++i) {
            const WheelSetup& w = s.wheels[i];
            const Vec3 hubRest = t.position + R * w.position;
            const Vec3 hubVelocity = body.velocity + glm::cross(body.angularVelocity, hubRest - comW);
            const f32 compressionRate = -glm::dot(hubVelocity, up);

            f32 normalForce = 0.0f;
            if (contact[i]) {
                normalForce = d.staticLoad[i] + d.springRate[i] * compression[i] + d.damping[i] * compressionRate;
                if (d.partner[i] >= 0) normalForce += s.antiRoll * d.springRate[i] * (compression[i] - compression[static_cast<u32>(d.partner[i])]);
                if (rawCompression[i] > s.travelUp) { // the bump stop
                    const f32 mw = s.mass / static_cast<f32>(s.wheelCount);
                    const f32 kb = mw * 2500.0f;
                    normalForce += kb * (rawCompression[i] - s.travelUp) + 2.0f * 0.7f * std::sqrt(kb * mw) * std::max(compressionRate, 0.0f);
                }
                normalForce = std::max(normalForce, 0.0f);
            }
            body.suspension[i] = compression[i];
            body.load[i] = normalForce;

            // Torques on the wheel: engine, brakes.
            const f32 brakeShare = d.front[i] ? 2.0f * s.brakeFrontBias : 2.0f * (1.0f - s.brakeFrontBias);
            f32 brakeTorque = s.brakeTorque * body.brake * brakeShare;
            if (w.handbrake) brakeTorque += s.handbrakeTorque * body.handbrake;
            const f32 driveTorque = w.driven && d.driven ? wheelTorqueTotal / static_cast<f32>(d.driven) : 0.0f;
            const f32 inertia = d.wheelInertia[i];
            f32 omega = body.wheelSpeed[i];
            const f32 brakeSign = std::clamp(omega / 0.5f, -1.0f, 1.0f);
            const f32 netTorqueFree = driveTorque - brakeSign * brakeTorque;

            if (normalForce <= 0.0f) {
                // In the air: the wheel spins freely (the brakes still stop it).
                f32 next = omega + netTorqueFree * h / inertia;
                if (omega * next < 0.0f && brakeTorque > std::abs(driveTorque)) next = 0.0f;
                omega = next;
                body.slip[i] = 0.0f;
            } else {
                ++grounded;
                const Vec3 P = groundPoint[i];
                const Vec3 gn = groundNormal[i];
                const Vec3 vContact = body.velocity + glm::cross(body.angularVelocity, P - comW);
                const f32 a = body.wheelSteer[i];
                const Vec3 fwdWheel = R * Vec3(std::sin(a), 0.0f, std::cos(a));
                Vec3 fwd = fwdWheel - gn * glm::dot(fwdWheel, gn);
                fwd = glm::length(fwd) > 1e-4f ? glm::normalize(fwd) : R[2];
                const Vec3 lat = glm::normalize(glm::cross(gn, fwd)); // left
                const f32 vLong = glm::dot(vContact, fwd), vLat = glm::dot(vContact, lat);
                const f32 mu = d.front[i] ? s.frontGrip : s.rearGrip;
                const f32 maxForce = std::max(mu * normalForce, 1.0f);
                const f32 r = w.radius;

                // Traction control: a driven wheel that spins well past the ground speed gets less engine torque. Off while the
                // handbrake is on (that is how you drift).
                f32 netTorque = netTorqueFree;
                if (w.driven && s.tractionControl > 0.0f && body.handbrake < 0.1f && driveTorque * omega > 0.0f) {
                    const f32 slipRatio = (std::abs(omega) * r - std::abs(vLong)) / std::max(std::abs(vLong), 2.5f);
                    if (slipRatio > 0.2f) netTorque = driveTorque * std::clamp(1.0f - s.tractionControl * (slipRatio - 0.2f) * 2.5f, 0.1f, 1.0f) - brakeSign * brakeTorque;
                }

                // Longitudinal: the tire pulls the wheel's ground speed toward the vehicle's; implicit in the wheel spin so it
                // stays stable however stiff the tire is.
                const f32 slipRef = std::max(0.6f, 0.1f * std::abs(vLong));
                const f32 Kx = maxForce / slipRef;
                f32 omegaNext = (inertia * omega + h * netTorque + h * Kx * r * vLong) / (inertia + h * Kx * r * r);
                if (omega * omegaNext < 0.0f && brakeTorque > std::abs(driveTorque)) omegaNext = 0.0f; // the brakes hold a stopped wheel
                f32 Fx = Kx * (omegaNext * r - vLong);
                if (std::abs(Fx) > maxForce) {
                    Fx = sign(Fx) * maxForce;
                    omegaNext = omega + h * (netTorque - Fx * r) / inertia;
                }
                // Lateral: stiff while gripping, saturating when sliding; the handbrake takes most of the rear's side grip (drifts).
                const f32 lateralGrip = (w.handbrake && body.handbrake > 0.1f) ? 1.0f - 0.55f * std::min(body.handbrake, 1.0f) : 1.0f;
                const f32 maxLateral = maxForce * lateralGrip;
                const f32 latRef = std::max(0.35f, 0.14f * std::abs(vLong));
                f32 Fy = std::clamp(-maxLateral / latRef * vLat, -maxLateral, maxLateral);
                // Friction ellipse: the tire has one budget of grip to share between pulling and cornering.
                const f32 ex = Fx / maxForce, ey = Fy / std::max(maxLateral, 1.0f);
                const f32 used = std::sqrt(ex * ex + ey * ey);
                if (used > 1.0f) {
                    Fx /= used;
                    Fy /= used;
                }
                body.slip[i] = used;
                const f32 rolling = -s.rollingResistance * normalForce * std::tanh(vLong / 0.3f);

                const Vec3 tireForce = gn * normalForce + fwd * (Fx + rolling) + lat * Fy;
                force += tireForce;
                torque += glm::cross(P - comW, tireForce);
                omega = omegaNext;
            }
            body.wheelSpeed[i] = omega;
            body.wheelAngle[i] = std::fmod(body.wheelAngle[i] + omega * h, kTwoPi);
        }
        body.groundedWheels = grounded;

        // Air: drag and downforce.
        const f32 speed = glm::length(body.velocity);
        force -= body.velocity * (0.5f * kAirDensity * s.dragCoefficient * s.frontalArea * speed);
        force -= up * (s.downforce * speed * speed);

        // Integrate the chassis.
        body.velocity += force * (h / s.mass);
        const Mat3 inverseInertia = R * inverseInertiaBody * glm::transpose(R);
        body.angularVelocity += inverseInertia * torque * h;
        body.angularVelocity *= std::exp(-0.15f * h);
        if (glm::length(body.velocity) > 120.0f) body.velocity = glm::normalize(body.velocity) * 120.0f;
        if (glm::length(body.angularVelocity) > 14.0f) body.angularVelocity = glm::normalize(body.angularVelocity) * 14.0f;

        const Vec3 newCom = comW + body.velocity * h;
        const Vec3 w = body.angularVelocity;
        const Quat spin = Quat(0.0f, w.x, w.y, w.z) * t.rotation;
        t.rotation = glm::normalize(Quat(t.rotation.w + 0.5f * h * spin.w, t.rotation.x + 0.5f * h * spin.x, t.rotation.y + 0.5f * h * spin.y,
                                         t.rotation.z + 0.5f * h * spin.z));
        t.position = newCom - glm::mat3_cast(t.rotation) * s.centerOfMass;

        resolveBody(s, body, t, world, d);

        // Parked on a slope that tires can hold: stay put (a viscous tire model would creep forever).
        const Vec3 upNow = glm::mat3_cast(t.rotation)[1];
        if (grounded >= 3 && upNow.y > 0.85f && body.throttle < 0.05f && (body.brake > 0.1f || body.handbrake > 0.5f) && glm::length(body.velocity) < 0.25f) {
            body.velocity *= 0.6f;
            body.angularVelocity *= 0.6f;
            if (glm::length(body.velocity) < 0.06f) {
                body.velocity = Vec3(0.0f);
                body.angularVelocity = Vec3(0.0f);
                for (u32 i = 0; i < s.wheelCount; ++i) body.wheelSpeed[i] = 0.0f;
            }
        }
    }

    const Mat3 R = glm::mat3_cast(t.rotation);
    body.speed = glm::length(body.velocity);
    body.forwardSpeed = glm::dot(body.velocity, R[2]);
    body.upsideDownTime = R[1].y < 0.25f ? body.upsideDownTime + dt : 0.0f;
}

f32 VehicleTopSpeedEstimate(const VehicleSetup& s) {
    if (s.wheelCount == 0 || s.gearCount == 0) return 0.0f;
    f32 radius = 0.0f;
    u32 n = 0;
    for (u32 i = 0; i < s.wheelCount; ++i)
        if (s.wheels[i].driven) {
            radius += s.wheels[i].radius;
            ++n;
        }
    radius = n ? radius / static_cast<f32>(n) : s.wheels[0].radius;
    const f32 ratio = s.gears[s.gearCount - 1] * s.finalDrive;
    return s.redlineRpm * kTwoPi / 60.0f / std::max(ratio, 0.1f) * radius;
}

} // namespace foh::game
