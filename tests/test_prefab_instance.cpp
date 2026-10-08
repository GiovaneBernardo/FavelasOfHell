#include <doctest/doctest.h>

#include "foh/ecs.h"
#include "foh/ecs/inspect.h"
#include "foh/ecs/snapshot.h"
#include "foh/scene/prefab_instance.h"

using namespace foh;

namespace {

PrefabNode node(const char* name, i32 parent, Vec3 pos, AssetId mesh = {}, std::vector<AssetId> materials = {}) {
    PrefabNode n;
    n.name = name;
    n.parent = parent;
    n.position = pos;
    n.mesh = mesh;
    n.materials = std::move(materials);
    return n;
}

struct Fixture {
    Fixture() : app({.workerThreads = 0}) {
        World& w = app.world();
        w.component<Name>();
        w.component<Transform>();
        w.component<MeshRenderer>();
    }
    App app;
};

const AssetId kBody{1, 1}, kLamp{2, 2}, kWall{3, 3}, kGlass{4, 4};

} // namespace

TEST_CASE("instantiating a prefab builds the hierarchy with composed world transforms") {
    Fixture f;
    World& w = f.app.world();

    PrefabAsset prefab;
    prefab.name = "House";
    prefab.nodes.push_back(node("Root", -1, Vec3(1, 0, 0)));
    prefab.nodes[0].scale = Vec3(2.0f);
    prefab.nodes.push_back(node("Door", 0, Vec3(0, 1, 0), kBody, {kWall, kGlass}));
    prefab.nodes.push_back(node("Lamp", 1, Vec3(0, 0, 3), kLamp, {kWall}));
    prefab.nodes[1].rotation = glm::angleAxis(glm::half_pi<f32>(), Vec3(0, 1, 0)); // 90 degrees around Y

    // Body has two submeshes (slots 0 and 1), the lamp one.
    const MeshInfo body{{0, 1}}, lamp{{0}};
    auto info = [&](const AssetId& id) -> const MeshInfo* { return id == kBody ? &body : id == kLamp ? &lamp : nullptr; };
    auto color = [](const AssetId& id) { return id == kWall ? Vec4(1, 0, 0, 1) : id == kGlass ? Vec4(0, 0, 1, 1) : Vec4(0.5f); };

    Transform at;
    at.position = Vec3(10, 0, 0);
    const PrefabInstanceResult r = InstantiatePrefab(w, prefab, info, color, at);

    // root + 3 nodes + 1 extra submesh entity
    REQUIRE(r.entities.size() == 5);
    CHECK(w.get<Name>(r.root)->text == std::string("House"));
    CHECK(w.get<Transform>(r.root)->position == Vec3(10, 0, 0));

    // Entities are in node order: root, Root, Door, Door.sub1 (spawned with its node), Lamp.
    const Entity rootNode = r.entities[1], door = r.entities[2], doorSub = r.entities[3], lampE = r.entities[4];
    CHECK(w.parentOf(rootNode) == r.root);
    CHECK(w.parentOf(door) == rootNode);
    CHECK(w.parentOf(doorSub) == door);
    CHECK(w.parentOf(lampE) == door);

    // World transforms: Root = at + (1,0,0)*1 -> (11,0,0), scale 2. Door = Root + scale * (0,1,0) -> (11,2,0).
    CHECK(w.get<Transform>(rootNode)->position == Vec3(11, 0, 0));
    CHECK(w.get<Transform>(rootNode)->scale == Vec3(2));
    CHECK(w.get<Transform>(door)->position == Vec3(11, 2, 0));
    // Lamp: Door's rotation (90 deg about Y) turns local +Z into world +X, scaled by 2: (11,2,0) + (6,0,0).
    const Vec3 lampPos = w.get<Transform>(lampE)->position;
    CHECK(lampPos.x == doctest::Approx(17.0f).epsilon(1e-4));
    CHECK(lampPos.y == doctest::Approx(2.0f));
    CHECK(lampPos.z == doctest::Approx(0.0f).epsilon(1e-4));

    // Mesh renderers: first submesh on the node, extra submeshes on children; colors from materials.
    const MeshRenderer* a = w.get<MeshRenderer>(door);
    REQUIRE(a);
    CHECK(a->asset == kBody);
    CHECK(a->submesh == 0);
    CHECK(a->material == kWall);
    CHECK(a->color == Vec4(1, 0, 0, 1));
    const MeshRenderer* b = w.get<MeshRenderer>(doorSub);
    REQUIRE(b);
    CHECK(b->submesh == 1);
    CHECK(b->material == kGlass);
    CHECK(b->color == Vec4(0, 0, 1, 1));
    CHECK(w.get<MeshRenderer>(rootNode) == nullptr);
    CHECK(w.get<MeshRenderer>(lampE)->asset == kLamp);
}

TEST_CASE("a node whose mesh is unknown is created without a renderer") {
    Fixture f;
    World& w = f.app.world();
    PrefabAsset prefab;
    prefab.name = "Broken";
    prefab.nodes.push_back(node("Missing", -1, Vec3(0), kBody, {kWall}));
    auto none = [](const AssetId&) -> const MeshInfo* { return nullptr; };
    auto color = [](const AssetId&) { return Vec4(1); };
    const PrefabInstanceResult r = InstantiatePrefab(w, prefab, none, color, Transform{});
    REQUIRE(r.entities.size() == 2);
    CHECK(w.get<MeshRenderer>(r.entities[1]) == nullptr);
}

TEST_CASE("MeshRenderer asset references survive a world snapshot") {
    // The renderer-local mesh id is not stable across sessions; the asset id + submesh are.
    Fixture f;
    World& w = f.app.world();
    MeshRenderer mr;
    mr.asset = kBody;
    mr.submesh = 3;
    mr.material = kWall;
    w.spawn(Transform{}, mr);
    const std::vector<std::byte> blob = saveSnapshot(w);

    Fixture g;
    World& loaded = g.app.world();
    const SnapshotReport report = loadSnapshot(loaded, blob);
    REQUIRE(report.ok);
    const ComponentId id = loaded.component<MeshRenderer>();
    std::vector<Entity> found;
    findEntities(loaded, std::span(&id, 1), {}, found);
    REQUIRE(found.size() == 1);
    const MeshRenderer* got = loaded.get<MeshRenderer>(found[0]);
    REQUIRE(got);
    CHECK(got->asset == kBody);
    CHECK(got->submesh == 3);
    CHECK(got->material == kWall);
}
