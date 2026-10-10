#include <doctest/doctest.h>

#include <filesystem>

#include "foh/world/terrain.h"
#include "foh/world/terrain_renderer.h"

using namespace foh;

TEST_CASE("Terrain is deterministic per seed and flat at the floor far from the land") {
    Terrain a, b, c;
    TerrainGenParams p;
    a.setParams(p);
    b.setParams(p);
    p.seed = 99;
    c.setParams(p);
    CHECK(a.height(40.0f, 30.0f) == b.height(40.0f, 30.0f));
    CHECK(a.height(40.0f, 30.0f) != c.height(40.0f, 30.0f));

    // Infinite: far away in every direction the ground is exactly the floor.
    for (const Vec2 q : {Vec2(30000.0f, 0.0f), Vec2(-30000.0f, 500.0f), Vec2(0.0f, -30000.0f), Vec2(0.0f, 30000.0f), Vec2(5e6f, -5e6f)})
        CHECK(a.height(q.x, q.y) == doctest::Approx(-200.0f));
    // The land stands well above the sea inland, and the sea bed is below it offshore.
    CHECK(a.height(0.0f, -200.0f) > 20.0f);
    CHECK(a.height(0.0f, 800.0f) < -100.0f);
    // The sea bed falls smoothly: no step bigger than the slope allows between samples 1 m apart.
    for (f32 z = 150.0f; z < 1500.0f; z += 7.0f) CHECK(std::abs(a.height(0.0f, z + 1.0f) - a.height(0.0f, z)) < 1.0f);
}

TEST_CASE("Brushes edit the sculpt layer only, and undo/redo restore it") {
    Terrain t;
    t.setParams({});
    const f32 x = 32.0f, z = 32.0f; // a corner shared by four sculpt tiles
    const f32 base = t.height(x, z);
    t.beginStroke();
    BrushStamp s;
    s.mode = BrushMode::Raise;
    s.center = Vec2(x, z);
    s.radius = 12.0f;
    s.strength = 1.0f;
    s.dt = 0.1f;
    t.applyBrush(s);
    t.applyBrush(s);
    t.endStroke();
    CHECK(t.height(x, z) > base + 1.0f);
    CHECK(t.baseHeight(x, z) == doctest::Approx(base)); // the base is untouched
    CHECK(t.sculptTileCount() >= 4);
    t.undo();
    CHECK(t.height(x, z) == doctest::Approx(base));
    CHECK(t.sculptTileCount() == 0);
    t.redo();
    CHECK(t.height(x, z) > base + 1.0f);
    t.clearSculpt();
    CHECK(t.height(x, z) == doctest::Approx(base));
    t.undo();
    CHECK(t.height(x, z) > base + 1.0f);
}

TEST_CASE("A brush reports what it changed, and a lone stamp is its own undo step") {
    Terrain t;
    t.setParams({});
    u64 cursor = t.dirtyCursor();
    CHECK(t.dirtyRectsSince(cursor).empty());
    BrushStamp s;
    s.center = Vec2(-100.0f, -60.0f);
    s.radius = 4.0f;
    s.dt = 0.1f;
    t.applyBrush(s);
    const auto rects = t.dirtyRectsSince(cursor);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0].minX < -100.0f);
    CHECK(rects[0].maxX > -100.0f);
    CHECK(t.canUndo());
    CHECK(t.dirtyRectsSince(cursor).empty());
}

TEST_CASE("Smooth and flatten brushes move the surface toward their target") {
    Terrain t;
    t.setParams({});
    const f32 x = 10.0f, z = -150.0f;
    BrushStamp s;
    s.mode = BrushMode::Flatten;
    s.center = Vec2(x, z);
    s.radius = 10.0f;
    s.flattenHeight = t.height(x, z) + 5.0f;
    s.dt = 0.5f;
    s.strength = 1.0f;
    for (int i = 0; i < 6; ++i) t.applyBrush(s);
    CHECK(t.height(x, z) == doctest::Approx(s.flattenHeight).epsilon(0.02));
}

TEST_CASE("Neighbouring nodes agree on their shared border, and levels nest") {
    Terrain t;
    t.setParams({});
    TerrainTileMesh left, right;
    BuildTerrainNodeMesh(t, {0, 0, 0}, left);
    BuildTerrainNodeMesh(t, {0, 1, 0}, right);
    const u32 n = kTerrainTileCells + 1;
    for (u32 j = 0; j < n; ++j) {
        const u32 l = j * n + kTerrainTileCells, r = j * n;
        CHECK(left.positions[l].y == doctest::Approx(right.positions[r].y));
        CHECK(left.normals[l].x == doctest::Approx(right.normals[r].x));
        CHECK(left.normals[l].y == doctest::Approx(right.normals[r].y));
        CHECK(left.normals[l].z == doctest::Approx(right.normals[r].z));
    }
    // 64 x 64 quads plus a skirt on every side.
    CHECK(left.indices.size() == size_t(kTerrainTileCells) * kTerrainTileCells * 6 + 4 * size_t(kTerrainTileCells) * 6);

    // A coarse node's vertices lie on the same surface as the terrain function.
    TerrainTileMesh coarse;
    const TerrainNodeKey key{3, -1, 0};
    BuildTerrainNodeMesh(t, key, coarse);
    const Vec2 o = key.origin();
    for (u32 k : {0u, 17u, 64u}) {
        const Vec3 p = coarse.positions[32 * n + k];
        CHECK(p.y == doctest::Approx(t.height(o.x + p.x, o.y + p.z)));
    }
}

TEST_CASE("Raycast hits the surface from above and misses underground starts") {
    Terrain t;
    t.setParams({});
    const Vec3 from(0.0f, 900.0f, -100.0f);
    Vec3 hit;
    REQUIRE(t.raycast(from, Vec3(0, -1, 0), 2000.0f, hit));
    CHECK(hit.y == doctest::Approx(t.height(0.0f, -100.0f)).epsilon(0.01));
    CHECK_FALSE(t.raycast(Vec3(0.0f, -300.0f, 0.0f), Vec3(0, -1, 0), 2000.0f, hit));
    // A long grazing ray far from the land hits the floor.
    REQUIRE(t.raycast(Vec3(20000.0f, 50.0f, 0.0f), glm::normalize(Vec3(1.0f, -0.05f, 0.0f)), 40000.0f, hit));
    CHECK(hit.y == doctest::Approx(-200.0f).epsilon(0.01));
}

TEST_CASE("Terrain save/load keeps the sculpt layer and the generator parameters") {
    Terrain t;
    TerrainGenParams p;
    p.seed = 7;
    p.coastZ = 30.0f;
    t.setParams(p);
    BrushStamp s;
    s.center = Vec2(5.0f, -70.0f);
    s.radius = 15.0f;
    s.dt = 0.2f;
    t.applyBrush(s);
    const f32 expected = t.height(5.0f, -70.0f);

    const auto file = std::filesystem::temp_directory_path() / "foh_terrain_test.fohterrain";
    REQUIRE(t.save(file));
    Terrain u;
    REQUIRE(u.load(file));
    CHECK(u.params().seed == 7);
    CHECK(u.height(5.0f, -70.0f) == doctest::Approx(expected));
    std::filesystem::remove(file);
}
