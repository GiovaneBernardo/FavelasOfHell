// Streets of the district: junction corners computed once at load, then asphalt / sidewalk / marking meshes per chunk.
#include <algorithm>
#include <chrono>
#include <cmath>

#include "city_internal.h"
#include "foh/log.h"

namespace foh::city {

namespace {

constexpr f32 kAsphaltLift = 0.04f; // above the terrain: no z-fight with a coarse far LOD
constexpr f32 kCurb = 0.15f;
constexpr f32 kPathLift = 0.20f;    // paths lie over the sidewalk's top (0.19)
constexpr f32 kMarkLift = 0.012f;

bool IsRoad(RoadClass c) { return u8(c) <= u8(RoadClass::Pedestrian); }

f32 CornerRadius(RoadClass a, RoadClass b) {
    auto r = [](RoadClass c) {
        switch (c) {
        case RoadClass::Trunk:
        case RoadClass::Primary:
        case RoadClass::Secondary:
        case RoadClass::Tertiary: return 4.5f;
        case RoadClass::Residential:
        case RoadClass::Living: return 3.0f;
        default: return 1.5f;
        }
    };
    return std::min(r(a), r(b));
}

// Position and direction along an edge's polyline at arc length s.
struct Along {
    Vec2 pos, dir;
    u32 seg;
};

Along At(const CityEdge& e, const EdgeGeo& g, f32 s) {
    s = std::clamp(s, 0.0f, g.length);
    const auto it = std::upper_bound(g.cum.begin(), g.cum.end(), s);
    u32 seg = u32(std::clamp<i64>(i64(it - g.cum.begin()) - 1, 0, i64(e.pts.size()) - 2));
    const f32 len = std::max(g.cum[seg + 1] - g.cum[seg], 1e-4f);
    const f32 f = (s - g.cum[seg]) / len;
    const Vec2 d = (e.pts[seg + 1] - e.pts[seg]) / len;
    return {e.pts[seg] + (e.pts[seg + 1] - e.pts[seg]) * f, d, seg};
}

struct Incident {
    u32 edge;
    bool atA;
    Vec2 dir;
    f32 hw, swL, swR; // outward-left / right
    f32 angle;
    f32 trimL = 0.0f, trimR = 0.0f;
    RoadClass cls;
};

struct Corner {
    bool valid = false;
    Vec2 C, D, Ti, Tj;
    std::vector<Vec2> arc; // Ti .. Tj
    f32 k = 0.0f, ti = 0.0f, sj = 0.0f;
    Vec2 F, G;             // outer line points at the tangent distances
};

} // namespace

void BuildRoadIndex(CityIndex& index) {
    const CityData& d = *index.data;
    index.edgeGeo.assign(d.edges.size(), {});
    for (size_t ei = 0; ei < d.edges.size(); ++ei) {
        const CityEdge& e = d.edges[ei];
        EdgeGeo& g = index.edgeGeo[ei];
        g.cum.assign(e.pts.size(), 0.0f);
        g.lo = g.hi = e.pts[0];
        for (size_t i = 1; i < e.pts.size(); ++i) {
            g.cum[i] = g.cum[i - 1] + glm::length(e.pts[i] - e.pts[i - 1]);
            g.lo = glm::min(g.lo, e.pts[i]);
            g.hi = glm::max(g.hi, e.pts[i]);
        }
        g.length = g.cum.back();
        g.valid = g.length > 0.5f;
    }

    // Incident streets per node (roads only: paths never make junction polygons).
    std::vector<std::vector<Incident>> inc(d.nodes.size());
    for (size_t ei = 0; ei < d.edges.size(); ++ei) {
        const CityEdge& e = d.edges[ei];
        const EdgeGeo& g = index.edgeGeo[ei];
        if (!IsRoad(e.cls) || !g.valid) continue;
        for (int end = 0; end < 2; ++end) {
            const bool atA = end == 0;
            const u32 node = atA ? e.a : e.b;
            const f32 probe = std::min(5.0f, g.length * 0.5f);
            const Along far = At(e, g, atA ? probe : g.length - probe);
            const Vec2 dir = glm::normalize(far.pos - d.nodes[node]);
            Incident in;
            in.edge = u32(ei);
            in.atA = atA;
            in.dir = dir;
            in.hw = e.width * 0.5f;
            in.swL = atA ? e.sidewalkL : e.sidewalkR;
            in.swR = atA ? e.sidewalkR : e.sidewalkL;
            in.angle = std::atan2(dir.y, dir.x);
            in.cls = e.cls;
            if (e.cls == RoadClass::Pedestrian) in.swL = in.swR = 0.0f;
            inc[node].push_back(in);
        }
    }

    index.junctions.clear();
    for (u32 node = 0; node < inc.size(); ++node) {
        auto& list = inc[node];
        if (list.size() < 2) continue;
        std::sort(list.begin(), list.end(), [](const Incident& a, const Incident& b) { return a.angle < b.angle; });
        const Vec2 n0 = d.nodes[node];
        const size_t n = list.size();
        std::vector<Corner> corners(n); // corner between list[i] and list[(i + 1) % n]
        for (size_t i = 0; i < n; ++i) {
            const size_t j = (i + 1) % n;
            Incident& a = list[i];
            Incident& b = list[j];
            const f32 cr = Cross(a.dir, b.dir);
            Corner& c = corners[i];
            if (cr <= 0.12f) continue;
            const Vec2 ra = RightOf(a.dir), rb = RightOf(b.dir);
            auto solve = [&](f32 wa, f32 wb, Vec2& point, f32& t, f32& s) {
                const Vec2 A = ra * wa + rb * wb;
                t = -Cross(A, b.dir) / cr;
                s = -Cross(A, a.dir) / cr;
                point = n0 + a.dir * t + ra * wa;
            };
            f32 t, s;
            solve(a.hw, b.hw, c.C, t, s);
            if (t < -0.5f || s < -0.5f || t > 40.0f || s > 40.0f) continue;
            const EdgeGeo& ga = index.edgeGeo[a.edge];
            const EdgeGeo& gb = index.edgeGeo[b.edge];
            const f32 phi = std::acos(std::clamp(glm::dot(a.dir, b.dir), -1.0f, 1.0f));
            const f32 radius = CornerRadius(a.cls, b.cls);
            f32 k = radius / std::tan(phi * 0.5f);
            k = std::min(k, 0.42f * std::min(ga.length, gb.length) - std::max(t, s));
            k = std::max(k, 0.0f);
            c.valid = true;
            c.k = k;
            c.ti = t + k;
            c.sj = s + k;
            c.Ti = c.C + a.dir * k;
            c.Tj = c.C + b.dir * k;
            c.arc.clear();
            if (k > 0.05f) {
                const f32 rEff = k * std::tan(phi * 0.5f);
                const Vec2 bis = glm::normalize(a.dir + b.dir);
                const Vec2 O = c.C + bis * (rEff / std::sin(phi * 0.5f));
                const Vec2 va = c.Ti - O, vb = c.Tj - O;
                const f32 a0 = std::atan2(va.y, va.x);
                f32 da = std::atan2(vb.y, vb.x) - a0;
                while (da > 3.14159265f) da -= 6.2831853f;
                while (da < -3.14159265f) da += 6.2831853f;
                const int steps = std::max(3, int(std::ceil(std::abs(da) / 0.28f)));
                for (int q = 0; q <= steps; ++q) {
                    const f32 ang = a0 + da * f32(q) / f32(steps);
                    c.arc.push_back(O + Vec2(std::cos(ang), std::sin(ang)) * rEff);
                }
            } else {
                c.arc = {c.Ti, c.Tj};
            }
            f32 t2, s2;
            solve(a.hw + a.swR, b.hw + b.swL, c.D, t2, s2);
            c.F = c.Ti + ra * a.swR;
            c.G = c.Tj - rb * b.swL;
            a.trimR = std::max(a.trimR, c.ti);
            b.trimL = std::max(b.trimL, c.sj);
        }

        // Cut the streets back and store the trims.
        for (Incident& in : list) {
            EdgeGeo& g = index.edgeGeo[in.edge];
            const f32 cap = 0.45f * g.length;
            const f32 tl = std::min(in.trimL, cap), tr = std::min(in.trimR, cap);
            const f32 ta = std::max(tl, tr);
            // outward-left / right map back to the edge's own sides at end B
            if (in.atA) {
                g.tA = ta;
                g.lA = tl;
                g.rA = tr;
            } else {
                g.tB = ta;
                g.lB = tr;
                g.rB = tl;
            }
        }

        Junction j;
        j.node = node;
        j.center = n0;
        j.curb = kCurb;
        std::vector<Vec2> poly;
        bool allPaved = true;
        f32 maxSw = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            const Incident& in = list[i];
            const EdgeGeo& g = index.edgeGeo[in.edge];
            const f32 t = in.atA ? g.tA : g.tB;
            const Vec2 r = RightOf(in.dir);
            poly.push_back(n0 + in.dir * t - r * in.hw);
            poly.push_back(n0 + in.dir * t + r * in.hw);
            if (in.cls != RoadClass::Pedestrian) allPaved = false;
            maxSw = std::max(maxSw, std::max(in.swL, in.swR));
            const Corner& c = corners[i];
            if (!c.valid) continue;
            poly.push_back(c.Ti);
            for (const Vec2& p : c.arc) poly.push_back(p);
            poly.push_back(c.Tj);
            // The sidewalk patch beyond the curb arc.
            const Incident& nb = list[(i + 1) % n];
            if (in.swR > 0.01f || nb.swL > 0.01f) {
                std::vector<Vec2> patch = c.arc;
                patch.push_back(c.G);
                patch.push_back(c.D);
                patch.push_back(c.F);
                j.sidewalks.push_back(std::move(patch));
            }
            for (size_t q = 0; q + 1 < c.arc.size(); ++q) j.curbs.push_back({c.arc[q], c.arc[q + 1]});
        }
        j.asphalt = std::move(poly);
        j.pavedOnly = allPaved;
        j.sidewalk = maxSw;
        j.promenade = maxSw > 6.5f;
        index.junctions.push_back(std::move(j));
    }

    // Chunk lists.
    index.edgesByChunk.clear();
    index.junctionsByChunk.clear();
    for (size_t ei = 0; ei < d.edges.size(); ++ei) {
        const CityEdge& e = d.edges[ei];
        const f32 margin = e.width * 0.5f + std::max(e.sidewalkL, e.sidewalkR) + 2.0f;
        for (size_t i = 0; i + 1 < e.pts.size(); ++i) {
            const Vec2 lo = glm::min(e.pts[i], e.pts[i + 1]) - Vec2(margin), hi = glm::max(e.pts[i], e.pts[i + 1]) + Vec2(margin);
            for (i32 cz = ChunkOf(lo.y); cz <= ChunkOf(hi.y); ++cz)
                for (i32 cx = ChunkOf(lo.x); cx <= ChunkOf(hi.x); ++cx) {
                    auto& v = index.edgesByChunk[ChunkKey(cx, cz)];
                    if (v.empty() || v.back() != u32(ei)) {
                        if (std::find(v.begin(), v.end(), u32(ei)) == v.end()) v.push_back(u32(ei));
                    }
                }
        }
    }
    for (size_t ji = 0; ji < index.junctions.size(); ++ji) {
        const Junction& j = index.junctions[ji];
        Vec2 lo = j.asphalt[0], hi = j.asphalt[0];
        for (const Vec2& p : j.asphalt) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        index.junctionsByChunk[ChunkKey(ChunkOf(j.center.x), ChunkOf(j.center.y))].push_back(u32(ji));
    }
    // Street centre samples every 6 m for "how far is the nearest street" queries.
    index.roadGrid.clear();
    for (size_t ei = 0; ei < d.edges.size(); ++ei) {
        const CityEdge& e = d.edges[ei];
        if (!IsRoad(e.cls) && e.cls != RoadClass::Cycleway) continue;
        const EdgeGeo& g = index.edgeGeo[ei];
        const f32 reach = e.width * 0.5f + std::max(e.sidewalkL, e.sidewalkR) + 3.0f;
        for (f32 s = 0.0f; s <= g.length + 3.0f; s += 6.0f) {
            const Vec2 p = At(e, g, s).pos;
            index.roadGrid[CityIndex::GridKey(CityIndex::GridOf(p.x), CityIndex::GridOf(p.y))].push_back({p, reach, u32(ei)});
        }
    }
}

// ---- chunk meshes --------------------------------------------------------------------------------------------------

namespace {

struct Sample {
    Vec2 pos, right;
    f32 s;
};

// Samples of an edge between arc lengths s0 and s1: every vertex and at least every 3.5 m, tangents mitred at vertices.
void Sampling(const CityEdge& e, const EdgeGeo& g, f32 s0, f32 s1, std::vector<Sample>& out) {
    out.clear();
    if (s1 - s0 < 0.2f) return;
    std::vector<f32> ss;
    ss.push_back(s0);
    for (size_t i = 1; i + 1 < e.pts.size(); ++i)
        if (g.cum[i] > s0 + 0.3f && g.cum[i] < s1 - 0.3f) ss.push_back(g.cum[i]);
    ss.push_back(s1);
    std::vector<f32> dense;
    for (size_t i = 0; i + 1 < ss.size(); ++i) {
        dense.push_back(ss[i]);
        const int extra = int(std::floor((ss[i + 1] - ss[i]) / 3.5f));
        for (int k = 1; k <= extra; ++k) {
            const f32 s = ss[i] + (ss[i + 1] - ss[i]) * f32(k) / f32(extra + 1);
            dense.push_back(s);
        }
    }
    dense.push_back(ss.back());
    for (f32 s : dense) {
        const Along a = At(e, g, s);
        Vec2 dir = a.dir;
        f32 miter = 1.0f;
        // At a polyline vertex: average the two segment directions.
        for (size_t i = 1; i + 1 < e.pts.size(); ++i)
            if (std::abs(g.cum[i] - s) < 0.02f) {
                const Vec2 d0 = glm::normalize(e.pts[i] - e.pts[i - 1]), d1 = glm::normalize(e.pts[i + 1] - e.pts[i]);
                const Vec2 avg = d0 + d1;
                if (glm::length(avg) > 0.1f) {
                    dir = glm::normalize(avg);
                    miter = std::min(1.0f / std::max(glm::dot(dir, d0), 0.5f), 2.0f);
                }
                break;
            }
        out.push_back({a.pos, RightOf(dir) * miter, s});
    }
}

struct Ctx {
    const CityIndex& index;
    const Terrain& terrain;
    i32 cx, cz;
    ChunkParts& out;
    bool far = false;
    MeshData& mesh(u32 slot) {
        for (auto& [s, m] : out.parts)
            if (s == slot) return m;
        out.parts.push_back({slot, {}});
        return out.parts.back().second;
    }
    f32 ground(Vec2 p) const { return terrain.height(p.x, p.y); }
    Vec3 normal(Vec2 p) const { return terrain.normal(p.x, p.y); }
    bool owns(Vec2 a, Vec2 b) const {
        const Vec2 m = (a + b) * 0.5f;
        return ChunkOf(m.x) == cx && ChunkOf(m.y) == cz;
    }
};

// A ribbon across the street: columns at `offsets` (+ right), heights from `y(sample, column)`, normals from `n`.
template <class YFn, class NFn>
void Ribbon(Ctx& c, MeshData& m, const std::vector<Sample>& ss, const std::vector<f32>& offsets, f32 uPerMetre, f32 vPerMetre, f32 uBase, YFn&& y, NFn&& nrm, const Vec3& facing, f32 sBase = 0.0f) {
    if (ss.size() < 2) return;
    const size_t cols = offsets.size();
    std::vector<u32> prev(cols, ~0u);
    bool havePrev = false;
    for (size_t k = 0; k + 1 < ss.size(); ++k) {
        const bool own = c.owns(ss[k].pos, ss[k + 1].pos);
        if (!own) {
            havePrev = false;
            continue;
        }
        if (!havePrev) {
            for (size_t col = 0; col < cols; ++col) {
                const Vec2 p = ss[k].pos + ss[k].right * offsets[col];
                const Vec3 pos(p.x, y(ss[k], col), p.y);
                const Vec3 dirAlong = glm::normalize(Vec3(-ss[k].right.y, 0.0f, ss[k].right.x));
                (void)dirAlong;
                prev[col] = m.vert(pos, nrm(ss[k], col, p), Vec2(uBase + offsets[col] * uPerMetre, (ss[k].s + sBase) * vPerMetre), Vec4(glm::normalize(Vec3(ss[k].right.x, 0.0f, ss[k].right.y)), 1.0f));
            }
        }
        std::vector<u32> next(cols);
        for (size_t col = 0; col < cols; ++col) {
            const Vec2 p = ss[k + 1].pos + ss[k + 1].right * offsets[col];
            const Vec3 pos(p.x, y(ss[k + 1], col), p.y);
            next[col] = m.vert(pos, nrm(ss[k + 1], col, p), Vec2(uBase + offsets[col] * uPerMetre, (ss[k + 1].s + sBase) * vPerMetre), Vec4(glm::normalize(Vec3(ss[k + 1].right.x, 0.0f, ss[k + 1].right.y)), 1.0f));
        }
        for (size_t col = 0; col + 1 < cols; ++col) m.quad(prev[col], prev[col + 1], next[col + 1], next[col], facing);
        prev = next;
        havePrev = true;
    }
}

void AddJunction(Ctx& c, const Junction& j) {
    const Vec3 up(0, 1, 0);
    const u32 slot = j.pavedOnly ? kSidewalk : kAsphalt;
    MeshData& m = c.mesh(slot);
    auto vtx = [&](MeshData& mm, Vec2 p, f32 y, Vec3 nrm, f32 uvScale) { return mm.vert(Vec3(p.x, y, p.y), nrm, Vec2(p.x, p.y) * uvScale, Vec4(1, 0, 0, 1)); };
    const f32 uv = j.pavedOnly ? 1.0f / 2.0f : 1.0f / 5.0f;
    const f32 lift = j.pavedOnly ? kPathLift : kAsphaltLift;
    const u32 centre = vtx(m, j.center, c.ground(j.center) + lift, c.normal(j.center), uv);
    std::vector<u32> ring;
    for (const Vec2& p : j.asphalt) ring.push_back(vtx(m, p, c.ground(p) + lift, c.normal(p), uv));
    for (size_t i = 0; i < ring.size(); ++i) {
        const size_t k = (i + 1) % ring.size();
        if (glm::length(j.asphalt[i] - j.asphalt[k]) < 1e-3f) continue;
        m.tri(centre, ring[i], ring[k], up);
    }
    if (j.pavedOnly || c.far) return;
    // Sidewalk corners: the top of the patch and the curb along the arc.
    MeshData& sw = c.mesh(j.promenade ? kPromenade : kSidewalk);
    for (const auto& patch : j.sidewalks) {
        if (patch.size() < 4) continue;
        const size_t arcN = patch.size() - 3;
        f32 yRef = 0.0f;
        for (size_t q = 0; q < arcN; ++q) yRef += c.ground(patch[q]);
        yRef = yRef / f32(arcN) + kAsphaltLift + j.curb;
        std::vector<u32> v;
        for (size_t q = 0; q < patch.size(); ++q) {
            const f32 y = q < arcN ? c.ground(patch[q]) + kAsphaltLift + j.curb : yRef;
            v.push_back(vtx(sw, patch[q], y, up, 0.5f));
        }
        // Fan from the outer corner D (the patch is convex).
        const size_t hub = arcN + 1;
        for (size_t q = 0; q < v.size(); ++q) {
            const size_t k = (q + 1) % v.size();
            if (q == hub || k == hub) continue;
            sw.tri(v[hub], v[q], v[k], up);
        }
        // Curb faces on the arc.
        for (size_t q = 0; q + 1 < arcN; ++q) {
            const Vec2 a = patch[q], b = patch[q + 1];
            const Vec2 dir = glm::normalize(b - a);
            Vec3 nrm(dir.y, 0.0f, -dir.x); // toward the road: away from the patch
            Vec2 centroid(0.0f);
            for (const Vec2& pp : patch) centroid += pp;
            centroid /= f32(patch.size());
            if (glm::dot(Vec2(nrm.x, nrm.z), (a + b) * 0.5f - centroid) < 0.0f) nrm = -nrm;
            const f32 ya = c.ground(a) + kAsphaltLift, yb = c.ground(b) + kAsphaltLift;
            const u32 q0 = sw.vert(Vec3(a.x, ya, a.y), nrm, Vec2(0, 0), Vec4(1, 0, 0, 1)), q1 = sw.vert(Vec3(b.x, yb, b.y), nrm, Vec2(0.3f, 0), Vec4(1, 0, 0, 1));
            const u32 q2 = sw.vert(Vec3(b.x, yb + j.curb, b.y), nrm, Vec2(0.3f, 0.15f), Vec4(1, 0, 0, 1)), q3 = sw.vert(Vec3(a.x, ya + j.curb, a.y), nrm, Vec2(0, 0.15f), Vec4(1, 0, 0, 1));
            sw.quad(q0, q1, q2, q3, nrm);
        }
    }
}

void AddEdge(Ctx& c, u32 ei) {
    const CityEdge& e = c.index.data->edges[ei];
    const EdgeGeo& g = c.index.edgeGeo[ei];
    if (!g.valid) return;
    const f32 hw = e.width * 0.5f;
    const Vec3 up(0, 1, 0);
    std::vector<Sample> ss;

    auto groundY = [&](const Sample& s, f32 o) { return c.ground(s.pos + s.right * o); };

    if (IsRoad(e.cls) && e.cls != RoadClass::Pedestrian) {
        // Asphalt.
        Sampling(e, g, g.tA, g.length - g.tB, ss);
        std::vector<f32> cols = {-hw, 0.0f, hw};
        Ribbon(c, c.mesh(kAsphalt), ss, cols, 1.0f / 5.0f, 1.0f / 5.0f, 0.0f, [&](const Sample& s, size_t col) { return groundY(s, cols[col]) + kAsphaltLift; },
               [&](const Sample&, size_t, Vec2 p) { return c.normal(p); }, up);

        if (c.far) return;
        // Markings.
        auto line = [&](f32 offset, f32 width, bool dashed, f32 phase, bool yellow = false) {
            std::vector<f32> oc = {offset - width * 0.5f, offset + width * 0.5f};
            Ribbon(c, c.mesh(yellow ? kMarkingY : kMarking), ss, oc, 1.0f, dashed ? 1.0f / 9.0f : 0.0f, 0.0f, [&](const Sample& s, size_t col) { return groundY(s, oc[col]) + kAsphaltLift + kMarkLift; },
                   [&](const Sample&, size_t, Vec2 p) { return c.normal(p); }, up, phase);
        };
        const bool oneway = (e.flags & kEdgeOneway) != 0;
        if (u8(e.cls) <= u8(RoadClass::Secondary)) {
            if (oneway) {
                const int lanes = std::max<int>(e.lanes, 1);
                const f32 lw = e.width / f32(lanes);
                for (int k = 1; k < lanes; ++k) line(-hw + lw * f32(k), 0.13f, true, 0.0f);
            } else {
                line(-0.12f, 0.11f, false, 0.0f, true);
                line(0.12f, 0.11f, false, 0.0f, true);
            }
            line(-hw + 0.25f, 0.12f, false, 0.0f);
            line(hw - 0.25f, 0.12f, false, 0.0f);
        } else if (e.cls == RoadClass::Tertiary && !oneway) {
            line(0.0f, 0.12f, true, 0.0f, true);
        }

        // Sidewalks, one per side. Side +1 is the right.
        for (int side = -1; side <= 1; side += 2) {
            const f32 w = side > 0 ? e.sidewalkR : e.sidewalkL;
            if (w < 0.05f) continue;
            const f32 sg = f32(side);
            const f32 s0 = side > 0 ? g.rA : g.lA, s1 = g.length - (side > 0 ? g.rB : g.lB);
            Sampling(e, g, s0, s1, ss);
            if (ss.size() < 2) continue;
            const u32 slot = w > 6.5f ? kPromenade : kSidewalk;
            MeshData& m = c.mesh(slot);
            auto edgeY = [&](const Sample& s) { return groundY(s, sg * hw) + kAsphaltLift; };
            // Curb face.
            {
                std::vector<f32> oc = {sg * hw, sg * hw};
                Ribbon(c, m, ss, oc, 0.0f, 1.0f / 2.0f, 0.0f, [&](const Sample& s, size_t col) { return edgeY(s) + (col ? kCurb : 0.0f); }, [&](const Sample& s, size_t, Vec2) { return Vec3(-s.right.x * sg, 0.0f, -s.right.y * sg); },
                       Vec3(-sg * ss.back().right.x, 0.0f, -sg * ss.back().right.y));
            }
            // Top.
            {
                std::vector<f32> oc = {sg * hw, sg * (hw + w)};
                Ribbon(c, m, ss, oc, 1.0f / 2.0f, 1.0f / 2.0f, 0.0f, [&](const Sample& s, size_t) { return edgeY(s) + kCurb; }, [&](const Sample&, size_t, Vec2) { return up; }, up);
            }
            // Outer face down to the ground behind.
            {
                std::vector<f32> oc = {sg * (hw + w), sg * (hw + w)};
                Ribbon(c, m, ss, oc, 0.0f, 1.0f / 2.0f, 0.0f,
                       [&](const Sample& s, size_t col) { return col == 0 ? edgeY(s) + kCurb : std::min(groundY(s, sg * (hw + w + 0.3f)) - 0.12f, edgeY(s) + kCurb - 0.05f); },
                       [&](const Sample& s, size_t, Vec2) { return Vec3(s.right.x * sg, 0.0f, s.right.y * sg); }, Vec3(ss.back().right.x * sg, 0.0f, ss.back().right.y * sg));
            }
        }
        return;
    }

    if (c.far) return;
    // Pedestrian streets, cycle paths, footpaths and steps: one ribbon laid over the ground.
    const f32 lift = e.cls == RoadClass::Pedestrian ? kAsphaltLift + 0.02f : (e.cls == RoadClass::Cycleway ? kPathLift + 0.005f : kPathLift);
    const u32 slot = e.cls == RoadClass::Cycleway ? kCycle : (e.cls == RoadClass::Pedestrian && e.width > 4.5f && (e.flags & kEdgePromenade) ? kPromenade : kSidewalk);
    const f32 s0 = e.cls == RoadClass::Pedestrian ? g.tA : 0.0f, s1 = g.length - (e.cls == RoadClass::Pedestrian ? g.tB : 0.0f);
    Sampling(e, g, s0, s1, ss);
    std::vector<f32> cols = {-hw, 0.0f, hw};
    Ribbon(c, c.mesh(slot), ss, cols, 1.0f / 2.0f, 1.0f / 2.0f, 0.0f, [&](const Sample& s, size_t col) { return groundY(s, cols[col]) + lift; }, [&](const Sample&, size_t, Vec2 p) { return c.normal(p); }, up);
}

void AddCrossings(Ctx& c) {
    if (c.far) return;
    const CityData& d = *c.index.data;
    const auto it = c.index.pointsByChunk.find(ChunkKey(c.cx, c.cz));
    if (it == c.index.pointsByChunk.end()) return;
    MeshData& m = c.mesh(kMarking);
    const Vec3 up(0, 1, 0);
    for (const u32 pi : it->second) {
        const CityPoint& pt = d.points[pi];
        if (pt.kind != PointKind::Crossing) continue;
        // Nearest street among those wide enough to need a zebra.
        const i32 gx = CityIndex::GridOf(pt.p.x), gz = CityIndex::GridOf(pt.p.y);
        f32 best = 5.0f;
        u32 bestEdge = ~0u;
        for (i32 dz = -1; dz <= 1; ++dz)
            for (i32 dx = -1; dx <= 1; ++dx) {
                const auto g = c.index.roadGrid.find(CityIndex::GridKey(gx + dx, gz + dz));
                if (g == c.index.roadGrid.end()) continue;
                for (const auto& s : g->second) {
                    const CityEdge& e = d.edges[s.edge];
                    if (u8(e.cls) > u8(RoadClass::Residential) || e.width < 5.0f) continue;
                    const f32 dist = glm::length(s.p - pt.p);
                    if (dist < best) {
                        best = dist;
                        bestEdge = s.edge;
                    }
                }
            }
        if (bestEdge == ~0u) continue;
        const CityEdge& e = d.edges[bestEdge];
        const EdgeGeo& g = c.index.edgeGeo[bestEdge];
        // Nearest point on the polyline.
        f32 bestD = 1e9f, bestS = 0.0f;
        Vec2 dir(1, 0);
        for (size_t i = 0; i + 1 < e.pts.size(); ++i) {
            const Vec2 a = e.pts[i], b = e.pts[i + 1], ab = b - a;
            const f32 len2 = std::max(glm::dot(ab, ab), 1e-6f);
            const f32 t = std::clamp(glm::dot(pt.p - a, ab) / len2, 0.0f, 1.0f);
            const f32 dist = glm::length(pt.p - (a + ab * t));
            if (dist < bestD) {
                bestD = dist;
                bestS = g.cum[i] + std::sqrt(len2) * t;
                dir = glm::normalize(ab);
            }
        }
        if (bestD > 4.0f || bestS < g.tA + 2.5f || bestS > g.length - g.tB - 2.5f) continue;
        const Along at = At(e, g, bestS);
        const Vec2 right = RightOf(at.dir);
        const f32 hw = e.width * 0.5f;
        for (f32 o = -hw + 0.5f; o <= hw - 0.4f; o += 0.8f) {
            const Vec2 centre = at.pos + right * o;
            const Vec2 a = centre - at.dir * 1.6f, b = centre + at.dir * 1.6f;
            const f32 w = 0.2f;
            const Vec2 corners[4] = {a - right * w, a + right * w, b + right * w, b - right * w};
            u32 v[4];
            for (int q = 0; q < 4; ++q) v[q] = m.vert(Vec3(corners[q].x, c.ground(corners[q]) + kAsphaltLift + kMarkLift * 1.5f, corners[q].y), up, Vec2(q == 1 || q == 2 ? 1.0f : 0.0f, 0.15f), Vec4(1, 0, 0, 1));
            m.quad(v[0], v[1], v[2], v[3], up);
        }
    }
}

} // namespace

void BuildRoadChunk(const CityIndex& index, const Terrain& terrain, i32 cx, i32 cz, u32 lod, ChunkParts& out) {
    Ctx c{index, terrain, cx, cz, out};
    c.far = lod > 0;
    if (const auto it = index.edgesByChunk.find(ChunkKey(cx, cz)); it != index.edgesByChunk.end())
        for (const u32 ei : it->second) AddEdge(c, ei);
    if (const auto it = index.junctionsByChunk.find(ChunkKey(cx, cz)); it != index.junctionsByChunk.end())
        for (const u32 ji : it->second) AddJunction(c, index.junctions[ji]);
    AddCrossings(c);
}

} // namespace foh::city
