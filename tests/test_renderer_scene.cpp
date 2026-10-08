#include <doctest/doctest.h>

#include <cmath>

#include "foh/ecs.h"
#include "foh/renderer/renderer.h"

using namespace foh;

// The renderer never touches ECS storage: RenderExtractPlugin copies what it
// needs into a RenderScene. These tests drive that path through a real App with
// no GPU involved.

namespace {

struct Fixture {
    Fixture() : app({.workerThreads = 2}) {
        app.addPlugin(plugin);
        app.world().resource<RenderSceneLink>()->scene = &scene;
    }
    App app;
    RenderScene scene;
    RenderExtractPlugin plugin;
};

} // namespace

TEST_CASE("extraction copies meshes, cameras and the sun into the scene snapshot") {
    Fixture f;
    World& w = f.app.world();
    for (int i = 0; i < 100; ++i) {
        w.spawn(Transform{{static_cast<f32>(i), 0, 0}, Quat(1, 0, 0, 0), {1, 1, 1}},
                MeshRenderer{static_cast<u32>(BuiltinMesh::Sphere), {1, 0, 0, 1}});
    }
    const Entity cam = w.spawn(Transform{{0, 2, 5}, Quat(1, 0, 0, 0), {1, 1, 1}}, Camera{});
    w.spawn(Transform{{0, 0, 0}, glm::quatLookAt(Vec3(0, -1, 0), Vec3(0, 0, -1)), {1, 1, 1}},
            DirectionalLight{{1, 1, 1}, 2.0f});

    f.app.update(0.016f);

    // Nothing is visible to the renderer before publish().
    CHECK(f.scene.read().meshes.empty());
    f.scene.publish();

    const ExtractedScene& s = f.scene.read();
    CHECK(s.meshes.size() == 100);
    for (const ExtractedMesh& m : s.meshes) CHECK(m.mesh == static_cast<u32>(BuiltinMesh::Sphere));
    REQUIRE(s.cameras.size() == 1);
    CHECK(s.cameras[0].entity == cam.id);
    CHECK(s.cameras[0].world[3].y == doctest::Approx(2.0f));
    REQUIRE(s.sun.present);
    CHECK(s.sun.direction.y == doctest::Approx(-1.0f).epsilon(0.001));
    CHECK(s.sun.color.x == doctest::Approx(2.0f));
}

TEST_CASE("publish without a new extraction keeps the last snapshot") {
    Fixture f;
    f.app.world().spawn(Transform{}, MeshRenderer{});
    f.app.update(0.016f);
    f.scene.publish();
    REQUIRE(f.scene.read().meshes.size() == 1);

    f.scene.publish(); // no update in between (e.g. a frame the world did not run)
    CHECK(f.scene.read().meshes.size() == 1);
}

TEST_CASE("a world without a linked scene extracts nothing and does not crash") {
    App app({.workerThreads = 0});
    RenderExtractPlugin plugin;
    app.addPlugin(plugin);
    app.world().spawn(Transform{}, MeshRenderer{});
    app.update(0.016f); // RenderSceneLink::scene is null
    CHECK(app.world().entityCount() == 1);
}

TEST_CASE("reverse-Z projection maps near to 1 and far to 0") {
    const Mat4 p = MakePerspectiveReverseZ(1.0471976f, 16.0f / 9.0f, 0.1f, 1000.0f);
    auto ndcZ = [&](f32 viewZ) {
        const Vec4 c = p * Vec4(0, 0, viewZ, 1);
        return c.z / c.w;
    };
    CHECK(ndcZ(-0.1f) == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(ndcZ(-1000.0f) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(ndcZ(-1.0f) > ndcZ(-10.0f)); // nearer = larger depth
}

TEST_CASE("view params place the camera and look down its local -Z") {
    const Mat4 world = glm::translate(Mat4(1.0f), Vec3(3, 4, 5));
    const RenderViewParams v = MakeViewParams(world, 1.0f, 1.0f, 0.1f, 100.0f);
    CHECK(v.position.x == doctest::Approx(3.0f));
    // A point straight ahead of the camera ends up on the view-space -Z axis.
    const Vec4 p = v.view * Vec4(3, 4, 0, 1);
    CHECK(p.x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(p.z == doctest::Approx(-5.0f));
}
