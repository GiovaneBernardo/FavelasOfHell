#include <doctest/doctest.h>

#include <filesystem>

#include "foh/world/roads.h"
#include "foh/world/terrain.h"

using namespace foh;

namespace {
Road SlopeRoad() {
    Road r;
    r.name = "Test road";
    r.width = 7.0f;
    for (int i = 0; i < 5; ++i) r.points.push_back({-60.0f + 30.0f * f32(i), -150.0f + 8.0f * std::sin(f32(i))});
    return r;
}
} // namespace

TEST_CASE("The road spline passes through its points and is resampled evenly") {
    const Road r = SlopeRoad();
    const std::vector<Vec2> c = RoadCenterline(r, 1.0f);
    REQUIRE(c.size() > 100);
    CHECK(glm::length(c.front() - Vec2(r.points.front().x, r.points.front().z)) < 0.01f);
    CHECK(glm::length(c.back() - Vec2(r.points.back().x, r.points.back().z)) < 0.6f);
    for (size_t i = 1; i + 1 < c.size(); ++i) CHECK(glm::length(c[i] - c[i - 1]) == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("Carving makes the ground level across the road, within the grade along it, and is undoable") {
    Terrain t;
    t.setParams({});
    const Road r = SlopeRoad();
    const f32 before = t.height(0.0f, -150.0f);
    CarveRoad(t, r);
    const std::vector<Vec2> c = RoadCenterline(r, 2.0f);
    f32 previous = t.height(c[0].x, c[0].y);
    for (size_t i = 1; i + 1 < c.size(); ++i) {
        const f32 h = t.height(c[i].x, c[i].y);
        CHECK(std::abs(h - previous) <= r.maxGrade * 2.0f + 0.05f); // 2 m steps
        previous = h;
        // Level across: the asphalt and the sidewalk are at the height of the centre.
        const Vec2 d = glm::normalize(c[i + 1] - c[i]);
        const Vec2 side(-d.y, d.x);
        const f32 across = r.width * 0.5f + r.sidewalk - 0.5f;
        CHECK(std::abs(t.height(c[i].x + side.x * across, c[i].y + side.y * across) - h) < 0.35f);
        CHECK(std::abs(t.height(c[i].x - side.x * across, c[i].y - side.y * across) - h) < 0.35f);
    }
    CHECK(t.canUndo());
    t.undo();
    CHECK(t.height(0.0f, -150.0f) == doctest::Approx(before));
}

TEST_CASE("Road meshes have surface and sidewalk triangles that follow the ground") {
    Terrain t;
    t.setParams({});
    Road r = SlopeRoad();
    CarveRoad(t, r);
    const RoadMeshes m = BuildRoadMeshes(t, r);
    CHECK(m.surface.indices.size() > 300);
    CHECK(m.sidewalk.indices.size() > m.surface.indices.size());
    CHECK(m.surface.positions.size() == m.surface.normals.size());
    CHECK(m.surface.positions.size() == m.surface.uvs.size());
    CHECK(m.surface.positions.size() == m.surface.tangents.size());
    for (const Vec3& p : m.surface.positions) CHECK(p.y == doctest::Approx(t.height(p.x, p.z) + 0.04f).epsilon(0.01));
    // Surface normals face (nearly) up or (nearly) down, never sideways.
    for (const Vec3& n : m.surface.normals) CHECK(std::abs(n.y) > 0.8f);
    r.sidewalk = 0.0f;
    CHECK(BuildRoadMeshes(t, r).sidewalk.empty());
}

TEST_CASE("Roads save and load") {
    RoadNetwork net;
    net.roads.push_back(SlopeRoad());
    net.roads.back().sidewalk = 3.0f;
    net.roads.push_back(SlopeRoad());
    net.roads.back().name = "Second one";
    const auto file = std::filesystem::temp_directory_path() / "foh_roads_test.fohroads";
    REQUIRE(net.save(file));
    RoadNetwork loaded;
    REQUIRE(loaded.load(file));
    REQUIRE(loaded.roads.size() == 2);
    CHECK(loaded.roads[0].name == "Test road");
    CHECK(loaded.roads[0].sidewalk == doctest::Approx(3.0f));
    CHECK(loaded.roads[1].name == "Second one");
    CHECK(loaded.roads[1].points.size() == 5);
    CHECK(loaded.roads[1].points[2].x == doctest::Approx(0.0f));
    std::filesystem::remove(file);
}
