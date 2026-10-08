#include <doctest/doctest.h>

#include <chrono>

#include "foh/ecs.h"
#include "foh/renderer/render_scene.h"

using namespace foh;

namespace {

struct Fixture {
    Fixture() : app({.workerThreads = 2}) { RegisterTransformPropagation(app); }
    World& world() { return app.world(); }
    void step() { app.update(0.016f); }
    Vec3 globalPos(Entity e) { return world().get<GlobalTransform>(e)->position(); }
    App app;
};

Transform at(Vec3 p) {
    Transform t;
    t.position = p;
    return t;
}

// Moves an entity the way a game system does through a Query: tracked automatically.
void moveBy(World& w, Entity e, Vec3 delta) {
    Transform* t = w.get<Transform>(e);
    t->position += delta;
    w.markModified<Transform>(e); // in-place write through get(): must be announced
}

} // namespace

TEST_CASE("every entity with a Transform gets a GlobalTransform equal to it") {
    Fixture f;
    Transform t = at(Vec3(1, 2, 3));
    t.rotation = glm::angleAxis(glm::half_pi<f32>(), Vec3(0, 1, 0));
    t.scale = Vec3(2, 2, 2);
    const Entity e = f.world().spawn(t);
    f.step();
    REQUIRE(f.world().has<GlobalTransform>(e));
    const Mat4 expect = t.matrix();
    const Mat4 got = f.world().get<GlobalTransform>(e)->matrix();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) CHECK(got[c][r] == doctest::Approx(expect[c][r]).epsilon(1e-5));
}

TEST_CASE("children follow their parent, through any depth, and update when the parent moves") {
    Fixture f;
    World& w = f.world();
    const Entity root = w.spawn(at(Vec3(10, 0, 0)));
    const Entity child = w.spawn(at(Vec3(0, 1, 0)));
    const Entity grand = w.spawn(at(Vec3(0, 0, 1)));
    w.setParent(child, root);
    w.setParent(grand, child);
    f.step();
    CHECK(f.globalPos(root) == Vec3(10, 0, 0));
    CHECK(f.globalPos(child) == Vec3(10, 1, 0));
    CHECK(f.globalPos(grand) == Vec3(10, 1, 1));

    moveBy(w, root, Vec3(5, 0, 0));
    f.step();
    CHECK(f.globalPos(child) == Vec3(15, 1, 0));
    CHECK(f.globalPos(grand) == Vec3(15, 1, 1));

    // Moving only the middle one moves its subtree but not its parent.
    moveBy(w, child, Vec3(0, 4, 0));
    f.step();
    CHECK(f.globalPos(root) == Vec3(15, 0, 0));
    CHECK(f.globalPos(child) == Vec3(15, 5, 0));
    CHECK(f.globalPos(grand) == Vec3(15, 5, 1));
}

TEST_CASE("parent rotation and scale are inherited") {
    Fixture f;
    World& w = f.world();
    Transform p = at(Vec3(0));
    p.rotation = glm::angleAxis(glm::half_pi<f32>(), Vec3(0, 1, 0)); // local +X becomes world -Z
    p.scale = Vec3(2.0f);
    const Entity parent = w.spawn(p);
    const Entity child = w.spawn(at(Vec3(1, 0, 0)));
    w.setParent(child, parent);
    f.step();
    const Vec3 got = f.globalPos(child);
    CHECK(got.x == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(got.z == doctest::Approx(-2.0f).epsilon(1e-5)); // scaled by 2, rotated
    const Mat4 m = w.get<GlobalTransform>(child)->matrix();
    CHECK(glm::length(Vec3(m[0])) == doctest::Approx(2.0f).epsilon(1e-5)); // child inherits scale 2
}

TEST_CASE("changing both a parent and its child in one frame ends up consistent in either visiting order") {
    for (int round = 0; round < 6; ++round) {
        Fixture f;
        World& w = f.world();
        std::vector<Entity> roots, kids;
        for (int i = 0; i < 20; ++i) {
            roots.push_back(w.spawn(at(Vec3(static_cast<f32>(i), 0, 0))));
            kids.push_back(w.spawn(at(Vec3(0, 1, 0))));
            w.setParent(kids.back(), roots.back());
        }
        f.step();
        // Move a different mix of parents and children every round; tables are visited in storage order.
        for (int i = 0; i < 20; ++i) {
            if ((i + round) % 2 == 0) moveBy(w, roots[i], Vec3(0, 0, 100));
            if ((i + round) % 3 == 0) moveBy(w, kids[i], Vec3(0, 10, 0));
        }
        f.step();
        for (int i = 0; i < 20; ++i) {
            const Vec3 expectedRoot(static_cast<f32>(i), 0, (i + round) % 2 == 0 ? 100.0f : 0.0f);
            const Vec3 expectedKid = expectedRoot + Vec3(0, (i + round) % 3 == 0 ? 11.0f : 1.0f, 0);
            CHECK(f.globalPos(roots[i]) == expectedRoot);
            CHECK(f.globalPos(kids[i]) == expectedKid);
        }
    }
}

TEST_CASE("reparenting and unparenting recompute the world transform") {
    Fixture f;
    World& w = f.world();
    const Entity a = w.spawn(at(Vec3(100, 0, 0)));
    const Entity b = w.spawn(at(Vec3(0, 100, 0)));
    const Entity c = w.spawn(at(Vec3(1, 1, 1)));
    w.setParent(c, a);
    f.step();
    CHECK(f.globalPos(c) == Vec3(101, 1, 1));
    w.setParent(c, b);
    f.step();
    CHECK(f.globalPos(c) == Vec3(1, 101, 1));
    w.clearParent(c);
    f.step();
    CHECK(f.globalPos(c) == Vec3(1, 1, 1));
}

TEST_CASE("a parent without a Transform is transparent for the hierarchy view") {
    Fixture f;
    World& w = f.world();
    const Entity group = w.spawn(); // pure grouping entity: no Transform
    const Entity child = w.spawn(at(Vec3(3, 0, 0)));
    w.setParent(child, group);
    f.step();
    CHECK(f.globalPos(child) == Vec3(3, 0, 0));
}

TEST_CASE("static entities are not touched while others move") {
    Fixture f;
    World& w = f.world();
    // A static "city" and one mover. Archetypes differ (the mover has an extra component) so the
    // static table stays unflagged.
    std::vector<Entity> city;
    for (int i = 0; i < 2000; ++i) city.push_back(w.spawn(at(Vec3(static_cast<f32>(i), 0, 0)), Name::Make("static")));
    const Entity mover = w.spawn(at(Vec3(0)));
    f.step();
    const u32 stampBefore = w.get<GlobalTransform>(city[5])->stamp;
    for (int i = 0; i < 5; ++i) {
        moveBy(w, mover, Vec3(1, 0, 0));
        f.step();
    }
    CHECK(w.get<GlobalTransform>(city[5])->stamp == stampBefore); // never recomputed
    CHECK(w.get<GlobalTransform>(mover)->stamp > stampBefore);
    CHECK(f.globalPos(mover) == Vec3(5, 0, 0));
}

TEST_CASE("propagation cost scales with what changes, not with the world") {
    Fixture f;
    World& w = f.world();
    constexpr int kStatic = 100000, kMovers = 500;
    for (int i = 0; i < kStatic; ++i) w.spawn(at(Vec3(static_cast<f32>(i % 1000), 0, static_cast<f32>(i / 1000))), Name::Make("s"));
    std::vector<Entity> movers;
    for (int i = 0; i < kMovers; ++i) {
        const Entity root = w.spawn(at(Vec3(0, 0, static_cast<f32>(i))));
        for (int k = 0; k < 4; ++k) w.setParent(w.spawn(at(Vec3(0, 1, 0))), root);
        movers.push_back(root);
    }
    f.step(); // first frame computes everything

    using Clock = std::chrono::steady_clock;
    // Idle frames: nothing changed, so the whole frame should be about the scheduler overhead.
    auto t0 = Clock::now();
    for (int i = 0; i < 20; ++i) f.step();
    const double idleMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / 20.0;

    t0 = Clock::now();
    for (int i = 0; i < 20; ++i) {
        for (const Entity& m : movers) moveBy(w, m, Vec3(0.01f, 0, 0));
        f.step();
    }
    const double activeMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / 20.0;
    MESSAGE("100k static + " << kMovers << " movers (x4 children): idle frame " << idleMs << " ms, moving frame " << activeMs << " ms");
    CHECK(idleMs < 5.0); // 100k entities that did not change must not cost milliseconds
    CHECK(f.globalPos(movers[0]).x == doctest::Approx(0.2f).epsilon(1e-3));
}
