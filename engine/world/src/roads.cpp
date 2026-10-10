#include "foh/world/roads.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "foh/log.h"
#include "foh/world/noise.h"

namespace foh {

using noise::Smoothstep;

// ---- persistence ---------------------------------------------------------------------------------------------------

bool RoadNetwork::save(const std::filesystem::path& file) const {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::trunc);
    if (!f) return false;
    f << "foh-roads 1\n";
    for (const Road& r : roads) {
        f << "road " << (r.name.empty() ? "Road" : r.name) << "\n";
        f << "  width " << r.width << " sidewalk " << r.sidewalk << " curb " << r.curb << " grade " << r.maxGrade << "\n";
        for (const RoadPoint& p : r.points) f << "  p " << p.x << " " << p.z << "\n";
    }
    return bool(f);
}

bool RoadNetwork::load(const std::filesystem::path& file) {
    std::ifstream f(file);
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line) || line.rfind("foh-roads", 0) != 0) return false;
    std::vector<Road> loaded;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string word;
        in >> word;
        if (word == "road") {
            Road r;
            std::getline(in >> std::ws, r.name);
            loaded.push_back(std::move(r));
        } else if (word == "width" && !loaded.empty()) {
            Road& r = loaded.back();
            std::string key;
            in >> r.width;
            while (in >> key) {
                if (key == "sidewalk") in >> r.sidewalk;
                else if (key == "curb") in >> r.curb;
                else if (key == "grade") in >> r.maxGrade;
            }
        } else if (word == "p" && !loaded.empty()) {
            RoadPoint p;
            if (in >> p.x >> p.z) loaded.back().points.push_back(p);
        }
    }
    roads = std::move(loaded);
    ++revision;
    return true;
}

// ---- spline --------------------------------------------------------------------------------------------------------

std::vector<Vec2> RoadCenterline(const Road& road, f32 step) {
    const size_t n = road.points.size();
    std::vector<Vec2> pts;
    for (const RoadPoint& p : road.points) pts.emplace_back(p.x, p.z);
    if (n < 2) return pts;
    auto at = [&](i32 i) {
        if (i < 0) return pts[0] * 2.0f - pts[1];
        if (i >= i32(n)) return pts[n - 1] * 2.0f - pts[n - 2];
        return pts[size_t(i)];
    };
    // Dense polyline first (24 steps per span), then resampled at a uniform spacing.
    std::vector<Vec2> dense;
    for (i32 i = 0; i + 1 < i32(n); ++i) {
        const Vec2 p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
        for (int k = 0; k < 24; ++k) {
            const f32 t = f32(k) / 24.0f, t2 = t * t, t3 = t2 * t;
            dense.push_back(0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3));
        }
    }
    dense.push_back(pts[n - 1]);
    std::vector<Vec2> out;
    out.push_back(dense[0]);
    f32 carry = 0.0f;
    for (size_t i = 1; i < dense.size(); ++i) {
        Vec2 a = dense[i - 1];
        const Vec2 b = dense[i];
        f32 len = glm::length(b - a);
        while (carry + len >= step && len > 1e-6f) {
            const f32 need = step - carry;
            a += (b - a) * (need / len);
            out.push_back(a);
            len = glm::length(b - a);
            carry = 0.0f;
        }
        carry += len;
    }
    if (glm::length(out.back() - dense.back()) > step * 0.25f) out.push_back(dense.back());
    return out;
}

namespace {

// The road's heights along its centre: the terrain smoothed over ~25 m and limited to the grade (so it cuts through bumps
// and does not copy them). `centre` is spaced 1 m.
std::vector<f32> Profile(const Terrain& terrain, const Road& road, const std::vector<Vec2>& centre) {
    std::vector<f32> h(centre.size());
    for (size_t i = 0; i < centre.size(); ++i) h[i] = terrain.height(centre[i].x, centre[i].y);
    const i32 n = i32(h.size());
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<f32> s(h.size());
        for (i32 i = 0; i < n; ++i) {
            f32 sum = 0.0f;
            int cnt = 0;
            for (i32 k = -12; k <= 12; ++k) {
                const i32 j = i + k;
                if (j < 0 || j >= n) continue;
                sum += h[size_t(j)];
                ++cnt;
            }
            s[size_t(i)] = sum / f32(cnt);
        }
        h = s;
    }
    // Grade limit, both ways (the spacing is 1 m).
    const f32 g = std::max(road.maxGrade, 0.01f);
    for (i32 i = 1; i < n; ++i) h[size_t(i)] = std::clamp(h[size_t(i)], h[size_t(i - 1)] - g, h[size_t(i - 1)] + g);
    for (i32 i = n - 2; i >= 0; --i) h[size_t(i)] = std::clamp(h[size_t(i)], h[size_t(i + 1)] - g, h[size_t(i + 1)] + g);
    return h;
}

} // namespace

void CarveRoad(Terrain& terrain, const Road& road) {
    const std::vector<Vec2> centre = RoadCenterline(road, 1.0f);
    if (centre.size() < 2) return;
    const std::vector<f32> profile = Profile(terrain, road, centre);
    const f32 core = road.width * 0.5f + road.sidewalk + 0.6f, shoulder = 5.0f, reach = core + shoulder;

    Vec2 lo = centre[0], hi = centre[0];
    for (const Vec2& p : centre) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    lo -= Vec2(reach + 1.0f);
    hi += Vec2(reach + 1.0f);

    // A coarse grid of the centre points, so the nearest one is found among a handful.
    constexpr f32 kCell = 8.0f;
    std::unordered_map<u64, std::vector<u32>> grid;
    auto cellOf = [&](f32 x, f32 z) { return (u64(static_cast<u32>(i32(std::floor(x / kCell)))) << 32) | u64(static_cast<u32>(i32(std::floor(z / kCell)))); };
    for (u32 i = 0; i < centre.size(); ++i) grid[cellOf(centre[i].x, centre[i].y)].push_back(i);
    const i32 span = i32(std::ceil(reach / kCell));

    terrain.editRect(lo, hi, [&](f32 x, f32 z, f32 current) {
        f32 best = reach * reach;
        i32 bi = -1;
        const i32 cx = i32(std::floor(x / kCell)), cz = i32(std::floor(z / kCell));
        for (i32 dz = -span; dz <= span; ++dz)
            for (i32 dx = -span; dx <= span; ++dx) {
                const auto it = grid.find((u64(static_cast<u32>(cx + dx)) << 32) | u64(static_cast<u32>(cz + dz)));
                if (it == grid.end()) continue;
                for (const u32 i : it->second) {
                    const f32 ddx = x - centre[i].x, ddz = z - centre[i].y;
                    const f32 d2 = ddx * ddx + ddz * ddz;
                    if (d2 < best) {
                        best = d2;
                        bi = i32(i);
                    }
                }
            }
        if (bi < 0) return current;
        const f32 d = std::sqrt(best);
        const f32 w = d <= core ? 1.0f : 1.0f - Smoothstep(core, reach, d);
        return current + (profile[size_t(bi)] - current) * w;
    });
}

std::vector<VegetationClearing> RoadClearings(const Road& road) {
    std::vector<VegetationClearing> out;
    const std::vector<Vec2> centre = RoadCenterline(road, 6.0f);
    const f32 radius = road.width * 0.5f + road.sidewalk + 1.5f;
    for (const Vec2& p : centre) out.push_back({p.x, p.y, radius, 4.0f});
    return out;
}

// ---- meshes --------------------------------------------------------------------------------------------------------

namespace {

// A strip between two polylines of cross-section columns along the road: `columns[c][i]` is column c at centre point i.
// Normals are averaged along and across the strip, tangents follow +u (across the road).
void AddStrip(RoadMesh& m, const std::vector<std::vector<Vec3>>& columns, const std::vector<f32>& across, const std::vector<f32>& along, f32 uMetres, f32 vMetres) {
    const size_t cols = columns.size();
    const size_t rows = columns[0].size();
    const u32 base = static_cast<u32>(m.positions.size());
    for (size_t i = 0; i < rows; ++i)
        for (size_t c = 0; c < cols; ++c) {
            m.positions.push_back(columns[c][i]);
            m.uvs.push_back({across[c] / uMetres, along[i] / vMetres});
        }
    m.normals.resize(m.positions.size(), Vec3(0.0f));
    m.tangents.resize(m.positions.size(), Vec4(1, 0, 0, 1));
    auto idx = [&](size_t i, size_t c) { return base + u32(i * cols + c); };
    for (size_t i = 0; i + 1 < rows; ++i)
        for (size_t c = 0; c + 1 < cols; ++c) {
            const u32 a = idx(i, c), b = idx(i, c + 1), d = idx(i + 1, c), e = idx(i + 1, c + 1);
            // Counter-clockwise seen from above when the road runs "down" the strip and columns go left to right.
            for (u32 v : {a, d, b, b, d, e}) m.indices.push_back(v);
            for (u32 tri = 0; tri < 2; ++tri) {
                const u32 i0 = tri == 0 ? a : b, i1 = tri == 0 ? d : d, i2 = tri == 0 ? b : e;
                const Vec3 n = glm::cross(m.positions[i1] - m.positions[i0], m.positions[i2] - m.positions[i0]);
                m.normals[i0] += n;
                m.normals[i1] += n;
                m.normals[i2] += n;
            }
        }
    for (size_t k = base; k < m.normals.size(); ++k) m.normals[k] = glm::length(m.normals[k]) > 1e-8f ? glm::normalize(m.normals[k]) : Vec3(0, 1, 0);
    // Tangent: the direction of increasing u.
    for (size_t i = 0; i < rows; ++i)
        for (size_t c = 0; c < cols; ++c) {
            const size_t c0 = c > 0 ? c - 1 : c, c1 = c + 1 < cols ? c + 1 : c;
            const Vec3 t = columns[c1][i] - columns[c0][i];
            m.tangents[idx(i, c)] = Vec4(glm::length(t) > 1e-6f ? glm::normalize(t) : Vec3(1, 0, 0), 1.0f);
        }
}

} // namespace

RoadMeshes BuildRoadMeshes(const Terrain& terrain, const Road& road) {
    RoadMeshes out;
    const std::vector<Vec2> centre = RoadCenterline(road, 1.0f);
    if (centre.size() < 2) return out;
    const size_t n = centre.size();
    std::vector<f32> along(n, 0.0f);
    for (size_t i = 1; i < n; ++i) along[i] = along[i - 1] + glm::length(centre[i] - centre[i - 1]);
    std::vector<Vec2> right(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec2 a = centre[i > 0 ? i - 1 : i], b = centre[i + 1 < n ? i + 1 : i];
        const Vec2 t = glm::normalize(b - a);
        right[i] = Vec2(-t.y, t.x);
    }
    auto ground = [&](size_t i, f32 offset) {
        const Vec2 p = centre[i] + right[i] * offset;
        return Vec3(p.x, terrain.height(p.x, p.y), p.y);
    };

    // Asphalt: five columns across, each on the terrain (which CarveRoad has made level across the road).
    const f32 hw = road.width * 0.5f;
    const std::vector<f32> acrossS = {-hw, -0.5f * hw, 0.0f, 0.5f * hw, hw};
    std::vector<std::vector<Vec3>> cols(acrossS.size(), std::vector<Vec3>(n));
    for (size_t c = 0; c < acrossS.size(); ++c)
        for (size_t i = 0; i < n; ++i) {
            cols[c][i] = ground(i, acrossS[c]);
            cols[c][i].y += 0.04f;
        }
    std::vector<f32> acrossU;
    for (const f32 a : acrossS) acrossU.push_back(a + hw);
    AddStrip(out.surface, cols, acrossU, along, 4.0f, 4.0f);

    if (road.sidewalk > 0.01f) {
        for (int side = -1; side <= 1; side += 2) {
            const f32 s = f32(side);
            // Curb face, pavement top, then a ramp back into the ground: three strips, each with its own normals.
            std::vector<Vec3> edge(n), edgeTop(n), outerTop(n), outerGround(n);
            const f32 outer = hw + road.sidewalk;
            for (size_t i = 0; i < n; ++i) {
                const Vec3 e = ground(i, s * hw);
                edge[i] = Vec3(e.x, e.y + 0.04f, e.z);
                edgeTop[i] = Vec3(e.x, e.y + 0.04f + road.curb, e.z);
                const Vec3 o = ground(i, s * outer);
                outerTop[i] = Vec3(o.x, e.y + 0.04f + road.curb, o.z); // level with the curb: the carving made it so
                const Vec3 g = ground(i, s * (outer + 0.45f));
                outerGround[i] = Vec3(g.x, g.y - 0.02f, g.z);
            }
            // Column order must run left to right as seen from above, so flip for the other side.
            auto strip = [&](const std::vector<Vec3>& from, const std::vector<Vec3>& to, f32 widthMetres) {
                std::vector<std::vector<Vec3>> c2 = side > 0 ? std::vector<std::vector<Vec3>>{from, to} : std::vector<std::vector<Vec3>>{to, from};
                AddStrip(out.sidewalk, c2, {0.0f, widthMetres}, along, 2.0f, 2.0f);
            };
            strip(edge, edgeTop, road.curb);
            strip(edgeTop, outerTop, road.sidewalk);
            strip(outerTop, outerGround, 0.45f);
        }
    }
    return out;
}

} // namespace foh
