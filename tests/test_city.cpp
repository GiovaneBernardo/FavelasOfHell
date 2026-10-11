#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "foh/world/city.h"
#include "foh/world/ground_patch.h"
#include "foh/world/terrain.h"

using namespace foh;

namespace {

template <class T>
void Put(std::ofstream& f, T v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

std::vector<Vec2> Square(f32 x0, f32 z0, f32 x1, f32 z1) { return {{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}}; }

f32 TriangleArea(const std::vector<Vec2>& v, const std::vector<u32>& t) {
    f32 a = 0.0f;
    for (size_t i = 0; i + 2 < t.size(); i += 3) {
        const Vec2 p = v[t[i]], q = v[t[i + 1]], r = v[t[i + 2]];
        a += std::abs((q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x)) * 0.5f;
    }
    return a;
}

// A small district: a crossing of two streets and one building, in the FOHC layout tools/build_district.py writes.
std::filesystem::path WriteDistrict(const char* name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream f(path, std::ios::binary);
    f.write("FOHC", 4);
    Put<u32>(f, 1);
    Put<f64>(f, -22.9935);
    Put<f64>(f, -43.2330);
    const Vec2 nodes[5] = {{-80, 0}, {80, 0}, {0, -80}, {0, 80}, {0, 0}};
    Put<u32>(f, 5);
    for (const Vec2& n : nodes) {
        Put<f32>(f, n.x);
        Put<f32>(f, n.y);
    }
    Put<u32>(f, 4);
    for (u32 i = 0; i < 4; ++i) { // four streets from the outer nodes to the centre
        Put<u32>(f, i);
        Put<u32>(f, 4);
        Put<u8>(f, u8(RoadClass::Residential));
        Put<u8>(f, 2);
        Put<u8>(f, kEdgeLit);
        Put<u8>(f, 0);
        Put<f32>(f, 7.0f);
        Put<f32>(f, 2.5f);
        Put<f32>(f, 2.5f);
        Put<u16>(f, 0);
        Put<u16>(f, 2);
        Put<f32>(f, nodes[i].x);
        Put<f32>(f, nodes[i].y);
        Put<f32>(f, 0.0f);
        Put<f32>(f, 0.0f);
    }
    Put<u32>(f, 1); // names
    Put<u16>(f, 4);
    f.write("Test", 4);
    Put<u32>(f, 1); // one building, a 20 x 14 block in the corner between two streets
    Put<u8>(f, u8(BuildingKind::Apartments));
    Put<u8>(f, 4);
    Put<u8>(f, 1);
    Put<u8>(f, 1);
    Put<f32>(f, 12.0f);
    Put<f32>(f, 3.0f);
    Put<u32>(f, 42);
    Put<u16>(f, 4);
    for (const Vec2& p : Square(14.0f, 14.0f, 34.0f, 28.0f)) {
        Put<f32>(f, p.x);
        Put<f32>(f, p.y);
    }
    Put<u32>(f, 0); // areas
    Put<u32>(f, 1); // points: a lamp
    Put<u8>(f, u8(PointKind::Lamp));
    Put<f32>(f, 10.0f);
    Put<f32>(f, 5.0f);
    Put<f32>(f, 0.0f);
    Put<u32>(f, 0);
    return path;
}

} // namespace

TEST_CASE("Triangulation covers a polygon with a courtyard exactly") {
    const std::vector<std::vector<Vec2>> rings = {Square(0, 0, 10, 10), {{3, 3}, {3, 7}, {7, 7}, {7, 3}}}; // hole wound the other way
    std::vector<Vec2> v;
    std::vector<u32> t;
    city::Triangulate(rings, v, t);
    CHECK(TriangleArea(v, t) == doctest::Approx(84.0f).epsilon(0.001));
    const std::vector<std::vector<Vec2>> lshape = {{{0, 0}, {10, 0}, {10, 4}, {4, 4}, {4, 10}, {0, 10}}};
    city::Triangulate(lshape, v, t);
    CHECK(TriangleArea(v, t) == doctest::Approx(64.0f).epsilon(0.001));
}

TEST_CASE("A circle is pushed out of a footprint, from outside and from inside, and left alone far away") {
    const std::vector<std::vector<Vec2>> rings = {Square(0, 0, 10, 10)};
    const Vec2 near = PushOutOfFootprint(rings, Vec2(-0.4f, 5.0f), 1.0f);
    CHECK(near.x == doctest::Approx(-1.0f));
    CHECK(near.y == doctest::Approx(5.0f));
    const Vec2 inside = PushOutOfFootprint(rings, Vec2(9.0f, 5.0f), 1.0f);
    CHECK(inside.x >= 11.0f - 1e-3f); // out through the nearest wall, a radius beyond it
    CHECK(PushOutOfFootprint(rings, Vec2(30.0f, 5.0f), 1.0f) == Vec2(30.0f, 5.0f));
}

TEST_CASE("The district file loads and the chunk generators produce sane geometry") {
    const std::filesystem::path path = WriteDistrict("foh_test_district.fohcity");
    CityData data;
    REQUIRE(data.load(path));
    CHECK(data.nodes.size() == 5);
    CHECK(data.edges.size() == 4);
    REQUIRE(data.buildings.size() == 1);
    CHECK(data.buildings[0].height == doctest::Approx(12.0f));
    CHECK(data.buildings[0].lo == Vec2(14.0f, 14.0f));
    CHECK(data.points.size() == 1);

    Terrain terrain;
    terrain.setParams({});
    const f32 ground = terrain.height(24.0f, 21.0f);
    // Put the building's ground floor on the real ground so the heights can be checked.
    data.buildings[0].baseZ = ground + 0.3f;
    CitySettings settings;
    const CityChunkReport full = BuildCityChunkReport(data, terrain, settings, 0, 0, 0);
    CHECK(full.finite);
    CHECK(full.indicesValid);
    CHECK(full.buildings == 1);
    CHECK(full.triangles > 100);
    CHECK(full.propInstances > 0);
    CHECK(full.maxY >= ground + 0.3f + 12.0f - 0.2f);  // the parapet reaches the building's height
    CHECK(full.maxY <= ground + 0.3f + 12.0f + 8.0f);  // plus the machinery on the roof, nothing taller

    // The far version is lighter and keeps the same silhouette.
    const CityChunkReport far = BuildCityChunkReport(data, terrain, settings, 0, 0, 1);
    CHECK(far.finite);
    CHECK(far.indicesValid);
    CHECK(far.triangles < full.triangles);
    CHECK(far.propInstances == 0);

    // A chunk with nothing in it builds nothing, and switching layers off is honoured.
    CHECK(BuildCityChunkReport(data, terrain, settings, 40, 40, 0).triangles == 0);
    settings.buildings = settings.roads = settings.props = false;
    CHECK(BuildCityChunkReport(data, terrain, settings, 0, 0, 0).triangles == 0);
    std::filesystem::remove(path);
}

TEST_CASE("A bridge builds a deck, parapets and a slab; a canal builds walls; a cycle path meets a street at its kerb") {
    const std::filesystem::path path = WriteDistrict("foh_test_bridge.fohcity");
    CityData data;
    REQUIRE(data.load(path));
    Terrain terrain;
    terrain.setParams({});
    CitySettings settings;
    const CityChunkReport plain = BuildCityChunkReport(data, terrain, settings, 0, 0, 0);
    // Flag the first street as a bridge and add a canal and a cycle path that ends at the crossing.
    data.edges[0].flags |= kEdgeBridge;
    CityEdge canal = data.edges[1];
    canal.cls = RoadClass::Canal;
    canal.flags = 0;
    canal.width = 12.0f;
    canal.pts = {Vec2(-60.0f, 40.0f), Vec2(60.0f, 40.0f)};
    data.edges.push_back(canal);
    CityEdge bike = data.edges[2];
    bike.cls = RoadClass::Cycleway;
    bike.flags = 0;
    bike.width = 2.5f;
    bike.sidewalkL = bike.sidewalkR = 0.0f;
    bike.pts = {Vec2(0.0f, 0.0f), Vec2(-40.0f, 30.0f)};
    bike.a = 4;
    bike.b = 0;
    data.edges.push_back(bike);
    const CityChunkReport with = BuildCityChunkReport(data, terrain, settings, 0, 0, 0);
    CHECK(with.finite);
    CHECK(with.indicesValid);
    CHECK(with.triangles > plain.triangles); // the canal's walls and the path add geometry
    // The slab hangs under the deck: the lowest vertex is below what the street alone reaches.
    CHECK(with.minY < plain.minY - 0.5f);
    std::filesystem::remove(path);
}

TEST_CASE("Buildings that share a wall do not draw it, and a lower neighbour leaves a party wall above it") {
    const std::filesystem::path path = WriteDistrict("foh_test_district2.fohcity");
    CityData data;
    REQUIRE(data.load(path));
    Terrain terrain;
    terrain.setParams({});
    const f32 ground = terrain.height(40.0f, 21.0f);
    CityBuilding a = data.buildings[0]; // x 14..34
    a.baseZ = ground + 0.3f;
    CitySettings settings;
    settings.roads = false;
    settings.props = false;
    auto build = [&](f32 gap, f32 heightA, f32 heightB) {
        CityBuilding x = a, y = a;
        x.height = heightA;
        y.height = heightB;
        y.rings[0] = Square(34.0f + gap, 14.0f, 54.0f + gap, 28.0f);
        y.lo = Vec2(34.0f + gap, 14.0f);
        y.hi = Vec2(54.0f + gap, 28.0f);
        y.osmId = 43;
        CityData d = data;
        d.buildings = {x, y};
        return BuildCityChunkReport(d, terrain, settings, 0, 0, 0).triangles;
    };
    const u32 apart = build(8.0f, 12.0f, 12.0f);
    const u32 touching = build(0.0f, 12.0f, 12.0f);
    const u32 stepped = build(0.0f, 12.0f, 9.0f);
    CHECK(touching < apart);    // the shared wall is drawn by nobody
    CHECK(stepped > touching);  // the taller one shows a wall above the lower one
    std::filesystem::remove(path);
}

TEST_CASE("The ground patch blends in with a weight and is bilinear") {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "foh_test_ground.fohp";
    {
        std::ofstream f(path, std::ios::binary);
        f.write("FOHP", 4);
        Put<u32>(f, 1);
        Put<u32>(f, 4);
        Put<u32>(f, 4);
        Put<f32>(f, 2.0f);   // cell
        Put<f32>(f, 100.0f); // origin x
        Put<f32>(f, 200.0f); // origin z
        Put<f32>(f, 0.0625f); // unit
        Put<f32>(f, 4.0f);   // surface cell
        Put<u32>(f, 2);
        Put<u32>(f, 2);
        for (int i = 0; i < 16; ++i) Put<i16>(f, i16(160)); // 10 m everywhere
        for (int i = 0; i < 16; ++i) Put<u8>(f, 255);
        for (int i = 0; i < 4; ++i) { // R paving, G sand, B lawn, A
            Put<u8>(f, 255);
            Put<u8>(f, 0);
            Put<u8>(f, 0);
            Put<u8>(f, 255);
        }
    }
    const auto patch = GroundPatch::load(path);
    REQUIRE(patch);
    CHECK(patch->height(104.0f, 204.0f) == doctest::Approx(10.0f));
    CHECK(patch->weight(104.0f, 204.0f) == doctest::Approx(1.0f));
    CHECK(patch->weight(50.0f, 204.0f) == 0.0f); // outside
    f32 s[3];
    patch->surfaceAt(102.0f, 202.0f, s);
    CHECK(s[0] == doctest::Approx(1.0f));
    CHECK(s[1] == doctest::Approx(0.0f));
    std::filesystem::remove(path);
    CHECK_FALSE(GroundPatch::load(std::filesystem::temp_directory_path() / "foh_missing.fohp"));
}
