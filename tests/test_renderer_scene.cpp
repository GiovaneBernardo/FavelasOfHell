#include <doctest/doctest.h>

#include <algorithm>
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

// ---- imported meshes and materials: how MeshRenderer references reach the renderer's ids ----

TEST_CASE("extraction maps asset references to renderer mesh and material ids") {
    Fixture f;
    World& w = f.app.world();
    const AssetId mesh{1, 1}, material{2, 2}, unknownMesh{3, 3}, unknownMaterial{4, 4};

    MeshLookup meshes;
    meshes.set(mesh, {40, 41}); // two submeshes
    MaterialLookup materials;
    materials.set(material, 7);
    f.scene.meshLookup = &meshes;
    f.scene.materialLookup = &materials;

    MeshRenderer a;
    a.asset = mesh;
    a.submesh = 1;
    a.material = material;
    MeshRenderer b;
    b.asset = mesh;
    b.submesh = 0;
    b.material = unknownMaterial; // not uploaded yet: the default material
    MeshRenderer missing;
    missing.asset = unknownMesh;  // not uploaded yet: not drawn at all
    MeshRenderer builtin;         // no asset: the builtin cube with the default material
    w.spawn(Transform{}, a);
    w.spawn(Transform{}, b);
    w.spawn(Transform{}, missing);
    w.spawn(Transform{}, builtin);

    f.app.update(0.016f);
    f.scene.publish();
    const ExtractedScene& out = f.scene.read();
    REQUIRE(out.meshes.size() == 3);
    std::vector<std::pair<u32, u32>> got; // (mesh, material)
    for (const ExtractedMesh& m : out.meshes) got.push_back({m.mesh, m.material});
    std::sort(got.begin(), got.end());
    CHECK(got[0] == std::pair<u32, u32>{static_cast<u32>(BuiltinMesh::Cube), 0});
    CHECK(got[1] == std::pair<u32, u32>{40, 0});
    CHECK(got[2] == std::pair<u32, u32>{41, 7});
}

TEST_CASE("without lookups an asset reference is simply not drawn") {
    Fixture f; // scene.meshLookup stays null, like a scene created outside a renderer
    MeshRenderer m;
    m.asset = AssetId{9, 9};
    f.app.world().spawn(Transform{}, m);
    f.app.update(0.016f);
    f.scene.publish();
    CHECK(f.scene.read().meshes.empty());
}

TEST_CASE("camera and sun extraction use the world transform of their entities") {
    Fixture f;
    World& w = f.app.world();
    const Entity rig = w.spawn(Transform{{10, 0, 0}, Quat(1, 0, 0, 0), {2, 2, 2}}); // scaled parent: must not reach the view
    Transform local;
    local.position = Vec3(0, 3, 0);
    const Entity cam = w.spawn(local, Camera{});
    w.setParent(cam, rig);
    Transform sunT;
    sunT.rotation = glm::angleAxis(-glm::half_pi<f32>(), Vec3(1, 0, 0)); // local -Z points down (-Y) after this rotation
    w.spawn(sunT, DirectionalLight{});

    f.app.update(0.016f);
    f.scene.publish();
    const ExtractedScene& out = f.scene.read();
    REQUIRE(out.cameras.size() == 1);
    // Position: parent (10,0,0) + parent scale 2 * local (0,3,0) = (10,6,0); axes are unit length despite the parent scale.
    CHECK(out.cameras[0].world[3].x == doctest::Approx(10.0f));
    CHECK(out.cameras[0].world[3].y == doctest::Approx(6.0f));
    CHECK(glm::length(Vec3(out.cameras[0].world[0])) == doctest::Approx(1.0f));
    REQUIRE(out.sun.present);
    CHECK(out.sun.direction.y == doctest::Approx(-1.0f).epsilon(1e-4));
}

TEST_CASE("rotating the sun after the first frame changes the extracted direction") {
    Fixture f;
    World& w = f.app.world();
    // Other entities share nothing with the sun's table; the sun is alone in its archetype like in the game.
    w.spawn(Transform{{0, 0, 0}, Quat(1, 0, 0, 0), {1, 1, 1}}, MeshRenderer{});
    const Entity sun = w.spawn(Transform{{0, 0, 0}, glm::quatLookAt(Vec3(0, -1, 0), Vec3(0, 0, -1)), {1, 1, 1}},
                               DirectionalLight{});
    f.app.update(0.016f);
    f.scene.publish();
    CHECK(f.scene.read().sun.direction.y == doctest::Approx(-1.0f).epsilon(0.001));

    // The editor inspector/gizmo path: write in place, then announce.
    w.get<Transform>(sun)->rotation = glm::quatLookAt(Vec3(1, 0, 0), Vec3(0, 1, 0));
    w.markModified<Transform>(sun);
    f.app.update(0.016f);
    f.scene.publish();
    CHECK(f.scene.read().sun.direction.x == doctest::Approx(1.0f).epsilon(0.001));
}
