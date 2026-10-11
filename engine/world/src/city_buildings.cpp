// Buildings of the district: one generated mesh per footprint (ADR 0014).
//
// A building is its footprint (outer ring, courtyards), a ground floor level (baseZ, from the district file: the ground is levelled
// to it on hillsides) and a height. From these:
//   - the walls, one quad per wall section: a ground floor of shop fronts / entrances and the storeys above, textured with one
//     repeat per bay, so windows line up with the wall's length; every wall is split where a taller neighbour shares it (the
//     party wall), and the part that shows above the neighbour is a blank wall
//   - a foundation from the ground floor down to the lowest ground in front of the wall, so slopes never open a gap
//   - a roof: a flat slab with a parapet and its machinery (stair and lift housings, water tank, antennas), or a hipped tile
//     roof on small rectangular houses
#include <algorithm>
#include <chrono>
#include <cmath>

#include "city_internal.h"
#include "foh/world/noise.h"

namespace foh::city {

namespace {

constexpr f32 kParapet = 0.9f;
constexpr f32 kGroundH = 3.6f;

struct Interval {
    f32 t0, t1, top; // along the wall (metres), height of the neighbour's roof
};

struct Obb {
    Vec2 centre{0.0f}, axis{1, 0}; // axis: the long direction
    f32 halfLong = 1.0f, halfShort = 1.0f;
    f32 area() const { return 4.0f * halfLong * halfShort; }
};

Obb MinBox(const std::vector<Vec2>& ring) {
    Obb best;
    f32 bestArea = 1e30f;
    for (size_t i = 0; i < ring.size(); ++i) {
        const Vec2 d = glm::normalize(ring[(i + 1) % ring.size()] - ring[i]);
        const Vec2 n = RightOf(d);
        f32 lo0 = 1e30f, hi0 = -1e30f, lo1 = 1e30f, hi1 = -1e30f;
        for (const Vec2& p : ring) {
            const f32 a = glm::dot(p, d), b = glm::dot(p, n);
            lo0 = std::min(lo0, a);
            hi0 = std::max(hi0, a);
            lo1 = std::min(lo1, b);
            hi1 = std::max(hi1, b);
        }
        const f32 area = (hi0 - lo0) * (hi1 - lo1);
        if (area < bestArea) {
            bestArea = area;
            const bool alongFirst = (hi0 - lo0) >= (hi1 - lo1);
            best.axis = alongFirst ? d : n;
            best.halfLong = 0.5f * std::max(hi0 - lo0, hi1 - lo1);
            best.halfShort = 0.5f * std::min(hi0 - lo0, hi1 - lo1);
            best.centre = d * (0.5f * (lo0 + hi0)) + n * (0.5f * (lo1 + hi1));
        }
    }
    return best;
}

struct Gen {
    const CityIndex& index;
    const Terrain& terrain;
    const CitySettings& settings;
    ChunkParts& out;
    MeshData& mesh(u32 slot) {
        for (auto& [s, m] : out.parts)
            if (s == slot) return m;
        out.parts.push_back({slot, {}});
        return out.parts.back().second;
    }
    f32 ground(Vec2 p) const { return terrain.height(p.x, p.y); }
};

f32 Rnd(const CityBuilding& b, u32 settingsSeed, u32 salt) { return noise::Rand01(i32(b.osmId & 0x7FFFFFFF), i32(salt), settingsSeed * 2654435761u + 17u); }

// One wall quad between two plan points. u runs toward the viewer's right (opposite to the ring direction).
void Wall(MeshData& m, Vec2 a, Vec2 b, f32 y0, f32 y1, f32 uA, f32 uB, f32 v0, f32 v1, Vec3 nrm) {
    const Vec3 tdir = uB >= uA ? glm::normalize(Vec3(b.x - a.x, 0.0f, b.y - a.y)) : glm::normalize(Vec3(a.x - b.x, 0.0f, a.y - b.y)); // direction of increasing u
    Vec3 up(0, 1, 0);
    const f32 sign = glm::dot(glm::cross(nrm, tdir), up) >= 0.0f ? 1.0f : -1.0f;
    const Vec4 tan(tdir, sign);
    const u32 q0 = m.vert(Vec3(a.x, y0, a.y), nrm, Vec2(uA, v0), tan), q1 = m.vert(Vec3(b.x, y0, b.y), nrm, Vec2(uB, v0), tan);
    const u32 q2 = m.vert(Vec3(b.x, y1, b.y), nrm, Vec2(uB, v1), tan), q3 = m.vert(Vec3(a.x, y1, a.y), nrm, Vec2(uA, v1), tan);
    m.quad(q0, q1, q2, q3, nrm);
}

// A closed box (no bottom) for roof machinery, UVs in metres.
void Box(MeshData& m, Vec2 centre, Vec2 axis, f32 hx, f32 hz, f32 y0, f32 y1) {
    const Vec2 ax = axis, az = RightOf(axis);
    const Vec2 c[4] = {centre - ax * hx - az * hz, centre + ax * hx - az * hz, centre + ax * hx + az * hz, centre - ax * hx + az * hz};
    for (int i = 0; i < 4; ++i) {
        const Vec2 a = c[i], b = c[(i + 1) % 4];
        const Vec2 d = glm::normalize(b - a);
        Wall(m, a, b, y0, y1, 0.0f, glm::length(b - a) / 4.0f, y0 / 4.0f, y1 / 4.0f, Vec3(d.y, 0.0f, -d.x));
    }
    const Vec3 up(0, 1, 0);
    u32 v[4];
    for (int i = 0; i < 4; ++i) v[i] = m.vert(Vec3(c[i].x, y1, c[i].y), up, c[i] / 4.0f);
    m.quad(v[0], v[1], v[2], v[3], up);
}

void Cap(MeshData& m, const std::vector<std::vector<Vec2>>& rings, f32 y, f32 uvScale) {
    std::vector<Vec2> verts;
    std::vector<u32> tris;
    Triangulate(rings, verts, tris);
    if (tris.empty()) return;
    const Vec3 up(0, 1, 0);
    const u32 base = u32(m.p.size());
    for (const Vec2& p : verts) m.vert(Vec3(p.x, y, p.y), up, p * uvScale, Vec4(1, 0, 0, 1));
    for (size_t i = 0; i + 2 < tris.size(); i += 3) m.tri(base + tris[i], base + tris[i + 1], base + tris[i + 2], up);
}

void HipRoof(MeshData& m, const Obb& box, f32 eaveY, f32 overhang) {
    const f32 hl = box.halfLong + overhang, hs = box.halfShort + overhang;
    const f32 rise = hs * std::tan(0.46f); // about 26 degrees
    const f32 ridgeHalf = std::max(hl - hs, 0.3f);
    const Vec2 ax = box.axis, az = RightOf(box.axis);
    auto P = [&](f32 l, f32 s) { return box.centre + ax * l + az * s; };
    const Vec2 c0 = P(-hl, -hs), c1 = P(hl, -hs), c2 = P(hl, hs), c3 = P(-hl, hs), r0 = P(-ridgeHalf, 0), r1 = P(ridgeHalf, 0);
    auto face = [&](std::vector<std::pair<Vec2, f32>> pts) {
        std::vector<u32> ids;
        Vec3 n(0.0f);
        const Vec3 p0(pts[0].first.x, pts[0].second, pts[0].first.y), p1(pts[1].first.x, pts[1].second, pts[1].first.y), p2(pts[2].first.x, pts[2].second, pts[2].first.y);
        n = glm::normalize(glm::cross(p1 - p0, p2 - p0));
        if (n.y < 0.0f) n = -n;
        const Vec3 tn = glm::normalize(glm::cross(Vec3(0, 1, 0), n)) ;
        for (auto& [p, y] : pts) ids.push_back(m.vert(Vec3(p.x, y, p.y), n, Vec2(p.x, p.y) * 0.9f + Vec2(y * 0.35f, 0.0f), Vec4(glm::length(tn) > 0.1f ? tn : Vec3(1, 0, 0), 1.0f)));
        for (size_t i = 1; i + 1 < ids.size(); ++i) m.tri(ids[0], ids[i], ids[i + 1], n);
    };
    const f32 top = eaveY + rise, eave = eaveY - 0.1f;
    face({{c0, eave}, {c1, eave}, {r1, top}, {r0, top}});
    face({{c2, eave}, {c3, eave}, {r0, top}, {r1, top}});
    face({{c3, eave}, {c0, eave}, {r0, top}});
    face({{c1, eave}, {c2, eave}, {r1, top}});
}

// The far version: one quad per wall and a flat roof, a few materials, no detail.
void GenFar(Gen& g, u32 bi) {
    const CityBuilding& b = g.index.data->buildings[bi];
    const std::vector<Vec2>& outer = b.rings[0];
    const u32 seed = g.settings.seed;
    const f32 H = std::max(b.height * g.settings.heightScale, 2.2f);
    if (b.kind == BuildingKind::Roof) return;
    const f32 baseY = b.baseZ - 0.6f, topY = b.baseZ + H;
    const u32 palette = u32(Rnd(b, seed, 1) * f32(kPalettes)) % kPalettes;
    const f32 sr = Rnd(b, seed, 2);
    const u32 style = sr < 0.40f ? 1u : (sr < 0.75f ? 0u : 2u);
    const u32 slot = kFacade0 + style * kPalettes + palette;
    const f32 floors = std::max(1.0f, std::round(H / 3.0f));
    for (size_t i = 0; i < outer.size(); ++i) {
        const Vec2 a = outer[i], bpt = outer[(i + 1) % outer.size()];
        const f32 L = glm::length(bpt - a);
        if (L < 0.5f) continue;
        const Vec2 d = (bpt - a) / L;
        const f32 bays = std::max(1.0f, std::round(L / 3.6f));
        Wall(g.mesh(slot), a, bpt, baseY, topY, bays, 0.0f, 0.0f, floors, Vec3(d.y, 0.0f, -d.x));
    }
    Cap(g.mesh(kRoofSlab), b.rings, topY, 0.25f);
}

void GenBuilding(Gen& g, u32 bi) {
    const CityBuilding& b = g.index.data->buildings[bi];
    const std::vector<Vec2>& outer = b.rings[0];
    const u32 seed = g.settings.seed;
    f32 H = b.height * g.settings.heightScale;
    if (H < 2.2f) H = 2.2f;
    const f32 baseY = b.baseZ;
    const f32 topY = baseY + H;

    // ---- the look ----
    const bool house = b.kind == BuildingKind::House || (b.kind == BuildingKind::Apartments && H < 7.5f && glm::length(b.hi - b.lo) < 22.0f);
    const bool canopy = b.kind == BuildingKind::Roof;
    const u32 palette = u32(Rnd(b, seed, 1) * f32(kPalettes)) % kPalettes;
    const f32 sr = Rnd(b, seed, 2);
    u32 style = sr < 0.40f ? 1u : (sr < 0.75f ? 0u : 2u);
    if (b.kind == BuildingKind::Commercial || b.kind == BuildingKind::Hotel) style = sr < 0.5f ? 2u : 0u;
    if (house) style = sr < 0.7f ? 1u : 0u;
    const u32 facadeSlot = kFacade0 + style * kPalettes + palette, blankSlot = kBlank0 + palette;
    const f32 footArea = std::abs(SignedArea(outer));
    const bool flatRoof = !house || footArea < 40.0f || Rnd(b, seed, 3) > 0.62f;

    // ---- storeys ----
    const f32 parapet = flatRoof ? kParapet : 0.0f;
    const f32 usable = std::max(H - parapet, 2.4f);
    u32 upperN = 0;
    f32 groundH = usable, upperH = 3.0f;
    if (!house && !canopy && usable >= kGroundH + 2.6f) {
        groundH = kGroundH;
        upperN = std::max(1u, u32(std::round((usable - kGroundH) / 3.0f)));
        upperH = (usable - kGroundH) / f32(upperN);
    } else if (house && usable > 3.6f) {
        const u32 n = std::max(1u, u32(std::round(usable / 3.0f)));
        groundH = usable / f32(n);
        upperN = n - 1;
        upperH = groundH;
    }
    const bool shopGround = !house && !canopy && upperN > 0;

    // ---- neighbours: intervals of each wall that touch a neighbour's wall ----
    auto neighbours = [&](Vec2 a, Vec2 bpt, Vec2 dir, std::vector<Interval>& iv) {
        const Vec2 lo = glm::min(a, bpt) - Vec2(1.0f), hi = glm::max(a, bpt) + Vec2(1.0f);
        const f32 L = glm::length(bpt - a);
        for (i32 gz = CityIndex::GridOf(lo.y); gz <= CityIndex::GridOf(hi.y); ++gz)
            for (i32 gx = CityIndex::GridOf(lo.x); gx <= CityIndex::GridOf(hi.x); ++gx) {
                const auto it = g.index.buildingGrid.find(CityIndex::GridKey(gx, gz));
                if (it == g.index.buildingGrid.end()) continue;
                for (const u32 ni : it->second) {
                    if (ni == bi) continue;
                    const CityBuilding& nb = g.index.data->buildings[ni];
                    if (nb.kind == BuildingKind::Roof || nb.hi.x < lo.x || nb.lo.x > hi.x || nb.hi.y < lo.y || nb.lo.y > hi.y) continue;
                    const f32 nTop = nb.baseZ + nb.height * g.settings.heightScale;
                    const Vec2 nrm = Vec2(dir.y, -dir.x);
                    for (const auto& ring : nb.rings)
                        for (size_t k = 0; k < ring.size(); ++k) {
                            const Vec2 p = ring[k], q = ring[(k + 1) % ring.size()];
                            const f32 ql = glm::length(q - p);
                            if (ql < 0.3f) continue;
                            const Vec2 qd = (q - p) / ql;
                            if (std::abs(Cross(dir, qd)) > 0.10f || glm::dot(dir, qd) > 0.0f) continue; // parallel and facing us
                            const f32 dist = glm::dot(p - a, nrm);
                            if (std::abs(dist) > 0.7f) continue;
                            f32 t0 = glm::dot(q - a, dir), t1 = glm::dot(p - a, dir);
                            if (t0 > t1) std::swap(t0, t1);
                            t0 = std::max(t0, 0.0f);
                            t1 = std::min(t1, L);
                            if (t1 - t0 > 0.25f) iv.push_back({t0, t1, nTop});
                        }
                }
            }
    };

    f32 gMin = 1e9f;
    auto wallsOf = [&](const std::vector<Vec2>& ring) {
        for (size_t i = 0; i < ring.size(); ++i) {
            const Vec2 a = ring[i], bpt = ring[(i + 1) % ring.size()];
            const f32 L = glm::length(bpt - a);
            if (L < 0.25f) continue;
            const Vec2 dir = (bpt - a) / L;
            const Vec3 nrm(dir.y, 0.0f, -dir.x);
            const Vec2 out2(dir.y, -dir.x);

            std::vector<Interval> iv;
            neighbours(a, bpt, dir, iv);
            std::vector<f32> cuts = {0.0f, L};
            for (const Interval& in : iv) {
                cuts.push_back(in.t0);
                cuts.push_back(in.t1);
            }
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end(), [](f32 x, f32 y) { return y - x < 0.2f; }), cuts.end());

            // The look of the whole wall: windows or blank, ground floor kind.
            const Vec2 mid = (a + bpt) * 0.5f;
            const bool street = g.index.distanceToStreet(mid + out2 * 4.0f, 14.0f) < 9.0f;
            const f32 wr = noise::Rand01(i32(b.osmId & 0x7FFFFFFF) + i32(i), 5, seed + 3);
            const bool blankWall = !street && wr < 0.20f;
            const i32 bays = std::max(1, i32(std::round(L / 3.6f)));
            auto uOf = [&](f32 t) { return (L - t) / L * f32(bays); };

            // Ground height just outside the wall, for the foundation.
            const Vec2 outMid = mid + out2 * 0.25f;
            const f32 gMid = g.ground(outMid), gA = g.ground(a + out2 * 0.25f), gB = g.ground(bpt + out2 * 0.25f);
            gMin = std::min(gMin, std::min(gMid, std::min(gA, gB)));

            for (size_t c = 0; c + 1 < cuts.size(); ++c) {
                const f32 t0 = cuts[c], t1 = cuts[c + 1];
                if (t1 - t0 < 0.2f) continue;
                f32 hidden = -1e9f;
                for (const Interval& in : iv)
                    if (in.t0 <= t0 + 0.15f && in.t1 >= t1 - 0.15f) hidden = std::max(hidden, in.top);
                if (hidden >= topY - 0.05f) continue; // the neighbour covers it
                const Vec2 pa = a + dir * t0, pb = a + dir * t1;
                const f32 ua = uOf(t0), ub = uOf(t1);
                if (hidden > baseY + 0.3f) { // a party wall that shows above the lower neighbour
                    Wall(g.mesh(blankSlot), pa, pb, hidden, topY, ua, ub, (hidden - baseY) / 3.0f, (topY - baseY) / 3.0f, nrm);
                    continue;
                }
                // foundation
                {
                    const f32 ya = std::min(g.ground(pa + out2 * 0.25f), baseY) - 0.15f, yb = std::min(g.ground(pb + out2 * 0.25f), baseY) - 0.15f;
                    if (ya < baseY - 0.02f || yb < baseY - 0.02f) {
                        MeshData& m = g.mesh(kFoundation);
                        const Vec3 tdir = glm::normalize(Vec3(pa.x - pb.x, 0.0f, pa.y - pb.y));
                        const Vec4 tan(tdir, glm::dot(glm::cross(nrm, tdir), Vec3(0, 1, 0)) >= 0.0f ? 1.0f : -1.0f);
                        const u32 q0 = m.vert(Vec3(pa.x, ya, pa.y), nrm, Vec2((t0) / 2.0f, ya / 2.0f), tan), q1 = m.vert(Vec3(pb.x, yb, pb.y), nrm, Vec2(t1 / 2.0f, yb / 2.0f), tan);
                        const u32 q2 = m.vert(Vec3(pb.x, baseY, pb.y), nrm, Vec2(t1 / 2.0f, baseY / 2.0f), tan), q3 = m.vert(Vec3(pa.x, baseY, pa.y), nrm, Vec2(t0 / 2.0f, baseY / 2.0f), tan);
                        m.quad(q0, q1, q2, q3, nrm);
                    }
                }
                const bool blank = blankWall || (t1 - t0) < 1.0f || canopy;
                if (canopy) continue;
                if (blank) {
                    Wall(g.mesh(blankSlot), pa, pb, baseY, topY, ua, ub, 0.0f, H / 3.0f, nrm);
                    continue;
                }
                // ground floor
                f32 y = baseY;
                if (shopGround && street) {
                    // per bay: shop fronts on commercial buildings, one entrance and windows on residential ones
                    const i32 k0 = i32(std::floor(std::min(ua, ub))), k1 = i32(std::ceil(std::max(ua, ub)));
                    for (i32 k = k0; k < k1; ++k) {
                        // the part of this bay that lies in the segment
                        const f32 bu0 = std::max(f32(k), std::min(ua, ub)), bu1 = std::min(f32(k + 1), std::max(ua, ub));
                        if (bu1 - bu0 < 0.05f) continue;
                        const f32 s0 = (ua - bu0) / (ua - ub), s1 = (ua - bu1) / (ua - ub);
                        const Vec2 qa = pa + (pb - pa) * s0, qb = pa + (pb - pa) * s1;
                        u32 variant;
                        const f32 rr = noise::Rand01(i32(b.osmId & 0x7FFFFFFF), k + i32(i) * 31, seed + 9);
                        if (b.kind == BuildingKind::Commercial) variant = rr < 0.35f ? 0u : (rr < 0.8f ? 1u : 3u);
                        else if (k == bays / 2 && bu1 - bu0 > 0.9f) variant = 2u;
                        else variant = rr < 0.5f ? 3u : 100u; // 100: a residential window bay
                        if (variant == 100u) Wall(g.mesh(facadeSlot), qa, qb, baseY, baseY + groundH, f32(bu0 - f32(k)) , f32(bu1 - f32(k)), 0.0f, groundH / 3.0f, nrm);
                        else Wall(g.mesh(kShop0 + variant), qa, qb, baseY, baseY + groundH, bu0 - f32(k), bu1 - f32(k), 0.0f, groundH / kGroundH, nrm);
                    }
                    y = baseY + groundH;
                } else if (shopGround) {
                    Wall(g.mesh(facadeSlot), pa, pb, baseY, baseY + groundH, ua, ub, 0.0f, groundH / 3.0f, nrm);
                    y = baseY + groundH;
                }
                // the storeys: one repeat per storey, so v counts storeys
                const f32 yTop = topY - parapet;
                if (shopGround) {
                    if (upperN > 0) Wall(g.mesh(facadeSlot), pa, pb, y, y + upperH * f32(upperN), ua, ub, 0.0f, f32(upperN), nrm);
                } else {
                    const f32 floors = std::max(1.0f, std::round((yTop - baseY) / upperH));
                    Wall(g.mesh(facadeSlot), pa, pb, baseY, yTop, ua, ub, 0.0f, floors * ((yTop - baseY) / (floors * upperH)), nrm);
                }
                // the parapet band
                if (parapet > 0.0f) Wall(g.mesh(blankSlot), pa, pb, yTop, topY, ua, ub, 0.0f, parapet / 3.0f, nrm);
                else if (shopGround && upperN > 0 && y + upperH * f32(upperN) < topY - 0.05f) Wall(g.mesh(blankSlot), pa, pb, y + upperH * f32(upperN), topY, ua, ub, 0.0f, 0.3f, nrm);
            }
        }
    };
    wallsOf(outer);
    for (size_t r = 1; r < b.rings.size(); ++r) wallsOf(b.rings[r]);

    // ---- roof ----
    const Vec3 up(0, 1, 0);
    if (canopy) { // a roof on posts: slab, edges and corner pillars
        Cap(g.mesh(kRoofSlab), b.rings, topY, 0.25f);
        Cap(g.mesh(kRoofSlab), b.rings, topY - 0.25f, 0.25f);
        for (size_t i = 0; i < outer.size(); ++i) {
            const Vec2 a = outer[i], bpt = outer[(i + 1) % outer.size()];
            const Vec2 d = glm::normalize(bpt - a);
            Wall(g.mesh(kRoofSlab), a, bpt, topY - 0.25f, topY, 0.0f, glm::length(bpt - a) / 4.0f, 0.0f, 0.06f, Vec3(d.y, 0.0f, -d.x));
            if (i % 2 == 0 || outer.size() <= 4) Box(g.mesh(kFoundation), a - d * 0.0f, d, 0.12f, 0.12f, g.ground(a), topY - 0.25f);
        }
        return;
    }
    const Obb box = MinBox(outer);
    if (!flatRoof && outer.size() <= 8 && footArea / std::max(box.area(), 1.0f) > 0.86f && box.halfShort > 2.0f) {
        HipRoof(g.mesh(kRoofTile), box, topY, 0.35f);
        // gaps under the overhang are hidden by the eave; no cap needed
    } else {
        const f32 roofY = topY - (parapet > 0.0f ? 0.75f : 0.1f);
        Cap(g.mesh(kRoofSlab), b.rings, roofY, 0.25f);
        if (parapet > 0.0f) { // the inner face of the parapet
            for (const auto& ring : b.rings)
                for (size_t i = 0; i < ring.size(); ++i) {
                    const Vec2 a = ring[i], bpt = ring[(i + 1) % ring.size()];
                    const f32 L = glm::length(bpt - a);
                    if (L < 0.3f) continue;
                    const Vec2 d = (bpt - a) / L;
                    Wall(g.mesh(kRoofSlab), bpt, a, roofY, topY, 0.0f, L / 4.0f, 0.0f, 0.2f, Vec3(-d.y, 0.0f, d.x));
                }
            // machinery
            if (footArea > 90.0f) {
                const u32 nBoxes = H > 14.0f ? 2u : 1u;
                for (u32 k = 0; k < nBoxes; ++k) {
                    for (int attempt = 0; attempt < 6; ++attempt) {
                        const f32 rx = Rnd(b, seed, 20 + k * 7 + u32(attempt)) - 0.5f, rz = Rnd(b, seed, 21 + k * 7 + u32(attempt)) - 0.5f;
                        const Vec2 p = box.centre + box.axis * (rx * box.halfLong * 1.3f) + RightOf(box.axis) * (rz * box.halfShort * 1.3f);
                        const f32 hx = 1.1f + 1.2f * Rnd(b, seed, 30 + k), hz = 1.0f + 1.0f * Rnd(b, seed, 31 + k);
                        const Vec2 ax = box.axis, az = RightOf(box.axis);
                        bool ok = true;
                        for (const Vec2 c : {p + ax * hx + az * hz, p - ax * hx + az * hz, p + ax * hx - az * hz, p - ax * hx - az * hz}) ok = ok && InsideRings(b.rings, c);
                        if (!ok) continue;
                        const f32 bh = 2.2f + 0.8f * Rnd(b, seed, 32 + k);
                        Box(g.mesh(kRoofSlab), p, ax, hx, hz, roofY, roofY + bh);
                        if (H > 24.0f && k == 0) Box(g.mesh(kFoundation), p, ax, 0.05f, 0.05f, roofY + bh, roofY + bh + 5.0f); // antenna
                        break;
                    }
                }
            } else if (footArea > 25.0f) { // a water tank
                const Vec2 p = box.centre + box.axis * (box.halfLong * 0.3f * (Rnd(b, seed, 40) - 0.5f));
                if (InsideRings(b.rings, p)) Box(g.mesh(kFoundation), p, box.axis, 0.6f, 0.6f, roofY, roofY + 1.2f);
            }
        }
    }
    (void)up;
}

} // namespace

void BuildBuildingChunk(const CityIndex& index, const Terrain& terrain, const CitySettings& settings, i32 cx, i32 cz, u32 lod, ChunkParts& out) {
    const auto it = index.buildingsByChunk.find(ChunkKey(cx, cz));
    if (it == index.buildingsByChunk.end()) return;
    Gen g{index, terrain, settings, out};
    for (const u32 bi : it->second) {
        if (lod > 0) GenFar(g, bi);
        else GenBuilding(g, bi);
        ++out.buildings;
    }
}

} // namespace foh::city
