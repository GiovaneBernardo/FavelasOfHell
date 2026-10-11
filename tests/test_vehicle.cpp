#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "foh/game_asset.h"
#include "foh/scene/game_asset_request.h"
#include "vehicle_dynamics.h"

using namespace foh;
using namespace foh::game;

namespace {

VehicleSetup sedan() {
    VehicleDef d;
    d.name = "Sedan";
    d.model = "x.gltf";
    const f32 x = 0.8f, zf = 1.4f, zr = -1.3f, r = 0.33f;
    for (int i = 0; i < 4; ++i) {
        WheelDef w;
        w.position = Vec3(i % 2 ? -x : x, r, i < 2 ? zf : zr);
        w.radius = r;
        w.steered = i < 2;
        w.driven = i >= 2;
        w.handbrake = i >= 2;
        d.wheels.push_back(w);
    }
    d.centerOfMass = Vec3(0.0f, 0.55f, 0.0f);
    d.colliderHalf = Vec3(0.9f, 0.7f, 2.2f);
    d.colliderCenter = Vec3(0.0f, 0.75f, 0.0f);
    return MakeVehicleSetup(d);
}

struct Rig {
    VehicleSetup setup = sedan();
    VehicleBody body;
    Transform t;
    std::vector<SolidBox> boxes;
    f32 time = 0.0f;
    Rig() { t.position = Vec3(0.0f, 0.0f, 0.0f); }
    void run(f32 seconds, f32 dt = 1.0f / 60.0f) {
        const VehicleWorld world{nullptr, boxes};
        for (f32 elapsed = 0.0f; elapsed < seconds; elapsed += dt) {
            StepVehicle(setup, body, t, world, dt);
            time += dt;
        }
    }
    Vec3 up() const { return glm::mat3_cast(t.rotation)[1]; }
    Vec3 forward() const { return glm::mat3_cast(t.rotation)[2]; }
};

} // namespace

TEST_CASE("a parked vehicle settles on its wheels at the modelled ride height") {
    Rig r;
    r.t.position = Vec3(0.0f, 0.0f, 0.0f);
    r.body.handbrake = 1.0f;
    r.run(3.0f);
    CHECK(r.body.groundedWheels == 4);
    CHECK(r.body.speed < 0.05f);
    // At rest the wheels sit at their modelled height: the origin is on the ground.
    CHECK(r.t.position.y == doctest::Approx(0.0f).epsilon(0.1).scale(0.1));
    CHECK(std::abs(r.t.position.y) < 0.06f);
    for (u32 i = 0; i < 4; ++i) CHECK(std::abs(r.body.suspension[i]) < 0.05f); // suspension near rest
    CHECK(r.up().y > 0.999f);
    // Static load: the four springs carry the weight.
    f32 total = 0.0f;
    for (u32 i = 0; i < 4; ++i) total += r.body.load[i];
    CHECK(total == doctest::Approx(r.setup.mass * 9.81f).epsilon(0.05));
}

TEST_CASE("full throttle accelerates straight, shifts up and reaches a plausible speed") {
    Rig r;
    r.body.handbrake = 0.0f;
    r.body.throttle = 1.0f;
    r.run(10.0f);
    CHECK(r.body.forwardSpeed > 22.0f); // > 80 km/h in 10 s
    CHECK(r.body.forwardSpeed < VehicleTopSpeedEstimate(r.setup) + 1.0f);
    CHECK(r.body.gear > 2);
    CHECK(std::abs(r.t.position.x) < 0.5f); // no sideways drift
    CHECK(r.t.position.z > 50.0f);
    CHECK(r.up().y > 0.99f);
    CHECK(r.body.rpm <= r.setup.redlineRpm * 1.03f);
}

TEST_CASE("brakes stop the vehicle in a sensible distance") {
    Rig r;
    r.body.handbrake = 0.0f;
    r.body.throttle = 1.0f;
    r.run(6.0f);
    const f32 v0 = r.body.forwardSpeed;
    REQUIRE(v0 > 15.0f);
    const f32 z0 = r.t.position.z;
    r.body.throttle = 0.0f;
    r.body.brake = 1.0f;
    r.run(6.0f);
    CHECK(r.body.speed < 0.2f);
    const f32 distance = r.t.position.z - z0;
    CHECK(distance < v0 * v0 / (2.0f * 5.0f)); // better than 5 m/s^2
    CHECK(distance > v0 * v0 / (2.0f * 12.0f)); // and not magic (12 m/s^2 is racing slicks)
}

TEST_CASE("steering turns the vehicle and does not flip it") {
    Rig r;
    r.body.handbrake = 0.0f;
    r.body.throttle = 0.6f;
    r.run(5.0f);
    const f32 yaw0 = std::atan2(r.forward().x, r.forward().z);
    r.body.steerInput = 1.0f; // left
    r.run(3.0f);
    const f32 yaw1 = std::atan2(r.forward().x, r.forward().z);
    CHECK(yaw1 - yaw0 > 0.4f);   // turned left (+X)
    CHECK(r.t.position.x > 2.0f); // ... and the path bent that way
    CHECK(r.up().y > 0.9f);      // wheels still down
    CHECK(r.body.groundedWheels >= 3);
}

TEST_CASE("the handbrake breaks the rear loose") {
    Rig r;
    r.body.handbrake = 0.0f;
    r.body.throttle = 0.7f;
    r.run(5.0f);
    r.body.throttle = 0.0f;
    r.body.steerInput = 0.6f;
    r.body.handbrake = 1.0f;
    r.run(0.6f);
    f32 rearSlip = 0.0f;
    for (u32 i = 2; i < 4; ++i) rearSlip = std::max(rearSlip, r.body.slip[i]);
    CHECK(rearSlip > 0.95f); // sliding
    // The car is not facing where it travels any more.
    const Vec3 v = glm::normalize(Vec3(r.body.velocity.x, 0.0f, r.body.velocity.z));
    const Vec3 f = glm::normalize(Vec3(r.forward().x, 0.0f, r.forward().z));
    CHECK(glm::dot(v, f) < 0.99f);
}

TEST_CASE("a wall stops the vehicle and costs it its speed") {
    Rig r;
    r.boxes.push_back({Vec3(0.0f, 1.5f, 40.0f), Vec3(10.0f, 1.5f, 1.0f)}); // a wall across the road
    r.body.handbrake = 0.0f;
    r.body.throttle = 1.0f;
    r.run(7.0f);
    CHECK(r.t.position.z < 39.0f - 2.2f + 1.0f); // never through it: the nose (2.2 m ahead of the origin) stays in front
    CHECK(r.body.forwardSpeed < 6.0f);
    CHECK(r.up().y > 0.9f);
}

TEST_CASE("a vehicle drives onto a low ramp (a kerb) instead of stopping at it") {
    Rig r;
    r.boxes.push_back({Vec3(0.0f, 0.075f, 30.0f), Vec3(10.0f, 0.075f, 5.0f)}); // 15 cm step
    r.body.handbrake = 0.0f;
    r.body.throttle = 0.8f;
    r.run(8.0f);
    CHECK(r.t.position.z > 36.0f);
}

TEST_CASE("setups from definitions carry the numbers across") {
    const VehicleSetup s = sedan();
    CHECK(s.wheelCount == 4);
    CHECK(s.frontAxleCount == 2);
    CHECK(s.gearCount == 6);
    CHECK(s.maxSteerRadians == doctest::Approx(34.0f * 3.14159265f / 180.0f).epsilon(0.01));
    CHECK(VehicleTopSpeedEstimate(s) > 40.0f);
    CHECK(VehicleTopSpeedEstimate(s) < 90.0f);
}
