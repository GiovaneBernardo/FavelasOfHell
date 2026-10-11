// Street furniture and beach props of the district (ADR 0014): lamps, traffic lights, bus shelters, benches, bins, lifeguard
// posts, kiosks, volleyball nets and beach umbrellas. Props are instances of a few shared meshes, so a chunk's props cost a draw
// per kind. Placement is deterministic (a hash of the position and the seed) and owned by the chunk that contains the spot.
#include <algorithm>
#include <cmath>

#include "city_internal.h"
#include "foh/world/noise.h"

namespace foh::city {

namespace {

Vec3 Hue(f32 h) { // saturated advertising colours
    static const Vec3 k[7] = {Vec3(0.85f, 0.12f, 0.10f), Vec3(0.95f, 0.75f, 0.10f), Vec3(0.10f, 0.35f, 0.80f), Vec3(0.10f, 0.65f, 0.60f), Vec3(0.95f, 0.45f, 0.10f), Vec3(0.80f, 0.20f, 0.55f), Vec3(0.92f, 0.92f, 0.90f)};
    return k[u32(h * 7.0f) % 7u];
}

u32 Tint(f32 r, f32 g, f32 b) {
    auto q = [](f32 v) { return u32(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return q(r) | (q(g) << 8) | (q(b) << 16) | 0xFF000000u;
}

struct Ctx {
    const CityIndex& index;
    const Terrain& terrain;
    const CitySettings& settings;
    i32 cx, cz;
    ChunkParts& out;

    bool mine(Vec2 p) const { return ChunkOf(p.x) == cx && ChunkOf(p.y) == cz; }
    f32 ground(Vec2 p) const { return terrain.height(p.x, p.y); }

    void put(Prop prop, u32 slot, Vec3 pos, f32 yaw, f32 scale = 1.0f, u32 tint = 0xFFFFFFFFu) {
        for (PropBatch& b : out.props)
            if (b.prop == prop && b.slot == slot) {
                b.instances.push_back(MakeStaticInstance(pos, yaw, scale, tint));
                return;
            }
        out.props.push_back({prop, slot, {MakeStaticInstance(pos, yaw, scale, tint)}});
        (void)terrain;
    }
    MeshData& mesh(u32 slot) {
        for (auto& [sl, m] : out.parts)
            if (sl == slot) return m;
        out.parts.push_back({slot, {}});
        return out.parts.back().second;
    }
    // A sagging wire between two points: crossed thin ribbons so it reads from every side.
    void wire(Vec3 a, Vec3 b, f32 sag) {
        MeshData& m = mesh(kWire);
        const Vec3 span = b - a;
        const f32 len = std::max(glm::length(Vec3(span.x, 0.0f, span.z)), 0.1f);
        const Vec3 dir = Vec3(span.x, 0.0f, span.z) / len;
        const Vec3 side = Vec3(-dir.z, 0.0f, dir.x);
        constexpr int kSeg = 6;
        u32 prevV[2] = {~0u, ~0u}, prevH[2] = {~0u, ~0u};
        for (int i = 0; i <= kSeg; ++i) {
            const f32 t = f32(i) / f32(kSeg);
            const Vec3 p = a + span * t - Vec3(0.0f, sag * 4.0f * t * (1.0f - t), 0.0f);
            const u32 v0 = m.vert(p + Vec3(0, 0.018f, 0), side, Vec2(t, 0)), v1 = m.vert(p - Vec3(0, 0.018f, 0), side, Vec2(t, 1));
            const u32 h0 = m.vert(p + side * 0.018f, Vec3(0, 1, 0), Vec2(t, 0)), h1 = m.vert(p - side * 0.018f, Vec3(0, 1, 0), Vec2(t, 1));
            if (i > 0) {
                m.quad(prevV[0], v0, v1, prevV[1], side);
                m.quad(prevH[0], h0, h1, prevH[1], Vec3(0, 1, 0));
            }
            prevV[0] = v0;
            prevV[1] = v1;
            prevH[0] = h0;
            prevH[1] = h1;
        }
    }
    // Position and unit direction along an edge.
    void at(const CityEdge& e, const EdgeGeo& g, f32 s, Vec2& pos, Vec2& dir) const {
        const auto it = std::upper_bound(g.cum.begin(), g.cum.end(), s);
        const size_t seg = size_t(std::clamp<i64>(i64(it - g.cum.begin()) - 1, 0, i64(e.pts.size()) - 2));
        const f32 len = std::max(g.cum[seg + 1] - g.cum[seg], 1e-3f);
        dir = (e.pts[seg + 1] - e.pts[seg]) / len;
        pos = e.pts[seg] + dir * (s - g.cum[seg]);
    }
    // Direction from `p` toward the nearest street centre (for orienting things toward the street).
    bool towardStreet(Vec2 p, Vec2& dir, f32& dist) const {
        f32 best = 40.0f;
        Vec2 bp(0.0f);
        const i32 gx = CityIndex::GridOf(p.x), gz = CityIndex::GridOf(p.y);
        for (i32 dz = -1; dz <= 1; ++dz)
            for (i32 dx = -1; dx <= 1; ++dx) {
                const auto it = index.roadGrid.find(CityIndex::GridKey(gx + dx, gz + dz));
                if (it == index.roadGrid.end()) continue;
                for (const auto& s : it->second) {
                    const f32 d = glm::length(s.p - p);
                    if (d < best) {
                        best = d;
                        bp = s.p;
                    }
                }
            }
        if (best >= 40.0f) return false;
        dist = best;
        dir = glm::normalize(bp - p);
        return true;
    }
};

f32 YawForX(Vec2 d) { return std::atan2(-d.y, d.x); } // local +x onto d
f32 YawForZ(Vec2 d) { return std::atan2(d.x, d.y); }  // local +z onto d

constexpr f32 kSidewalkTop = 0.19f;

void Points(Ctx& c) {
    const CityData& d = *c.index.data;
    const auto it = c.index.pointsByChunk.find(ChunkKey(c.cx, c.cz));
    if (it == c.index.pointsByChunk.end()) return;
    for (const u32 pi : it->second) {
        const CityPoint& p = d.points[pi];
        Vec2 toward;
        f32 dist = 0.0f;
        const bool street = c.towardStreet(p.p, toward, dist);
        const f32 y = c.ground(p.p) + (street && dist < 14.0f ? kSidewalkTop : 0.0f);
        const Vec3 pos(p.p.x, y, p.p.y);
        switch (p.kind) {
        case PointKind::Lamp:
            if (!street) toward = Vec2(0, 1);
            c.put(Prop::LampPole, kMetal, pos, YawForX(toward));
            c.put(Prop::LampHead, kLampHead, pos, YawForX(toward));
            break;
        case PointKind::Signals:
            if (!street) break;
            c.put(Prop::TrafficPole, kMetal, pos, YawForX(toward));
            c.put(Prop::TrafficHead, kGlassDark, pos, YawForX(toward));
            break;
        case PointKind::BusStop: {
            if (!street) break;
            c.put(Prop::ShelterFrame, kMetal, pos, YawForZ(toward));
            c.put(Prop::ShelterRoof, kPaintWhite, pos, YawForZ(toward));
            const f32 h1 = noise::Rand01(i32(pi), 41, c.settings.seed), h2 = noise::Rand01(i32(pi), 42, c.settings.seed);
            const Vec2 along(-toward.y, toward.x);
            if (h1 < 0.8f) { // an ad panel beside the shelter, facing the street
                const Vec2 q = p.p + along * 2.4f;
                const Vec3 col = Hue(h2);
                c.put(Prop::AdPanel, kMetal, Vec3(q.x, c.ground(q) + kSidewalkTop, q.y), YawForZ(toward));
                c.put(Prop::AdFace, kPaintWhite, Vec3(q.x, c.ground(q) + kSidewalkTop, q.y), YawForZ(toward), 1.0f, Tint(col.r, col.g, col.b));
            }
            if (h1 > 0.78f) { // a newsstand
                const Vec2 q = p.p - along * 4.0f - toward * 0.8f;
                static const Vec3 kCol[3] = {Vec3(0.20f, 0.45f, 0.30f), Vec3(0.75f, 0.30f, 0.20f), Vec3(0.25f, 0.40f, 0.65f)};
                const Vec3 col = kCol[u32(h2 * 3.0f) % 3u];
                c.put(Prop::KioskBody, kPaintWhite, Vec3(q.x, c.ground(q) + kSidewalkTop, q.y), YawForZ(toward), 0.8f, Tint(col.r, col.g, col.b));
                c.put(Prop::KioskRoof, kPaintRed, Vec3(q.x, c.ground(q) + kSidewalkTop, q.y), YawForZ(toward), 0.8f);
            }
            break;
        }
        case PointKind::Bench:
            c.put(Prop::Bench, kWood, pos, YawForZ(street ? toward : Vec2(0, 1)));
            break;
        case PointKind::Waste: c.put(Prop::Bin, kPlastic, pos, 0.0f); break;
        case PointKind::Lifeguard: c.put(Prop::PostoTower, kPaintWhite, pos, YawForZ(Vec2(0, 1))); c.put(Prop::PostoRoof, kPaintRed, pos, YawForZ(Vec2(0, 1))); break;
        default: break;
        }
    }
}

// Things placed at intervals along a street: lamps, benches and bins; on the beach front also postos, kiosks and nets.
void AlongStreets(Ctx& c) {
    const CityData& d = *c.index.data;
    const auto it = c.index.edgesByChunk.find(ChunkKey(c.cx, c.cz));
    if (it == c.index.edgesByChunk.end()) return;
    const u32 seed = c.settings.seed;
    for (const u32 ei : it->second) {
        const CityEdge& e = d.edges[ei];
        const EdgeGeo& g = c.index.edgeGeo[ei];
        if (!g.valid) continue;
        const bool road = u8(e.cls) <= u8(RoadClass::Living);
        const bool lit = (e.flags & kEdgeLit) || u8(e.cls) <= u8(RoadClass::Tertiary);
        const f32 hw = e.width * 0.5f;
        // Lamps: every 30 m, alternating sides, on streets with a sidewalk. The survey's own lamps near one suppress it.
        if (road && lit && std::max(e.sidewalkL, e.sidewalkR) >= 1.5f) {
            int k = 0;
            for (f32 s = g.tA + 6.0f + f32(ei % 7); s < g.length - g.tB - 6.0f; s += 30.0f, ++k) {
                Vec2 pos, dir;
                c.at(e, g, s, pos, dir);
                const int side = (k % 2 == 0) ? 1 : -1;
                const f32 w = side > 0 ? e.sidewalkR : e.sidewalkL;
                if (w < 1.5f) continue;
                const Vec2 right = RightOf(dir);
                const Vec2 p = pos + right * (f32(side) * (hw + 0.55f));
                if (!c.mine(p)) continue;
                const Vec3 pos3(p.x, c.ground(p) + kSidewalkTop, p.y);
                const f32 yaw = YawForX(-right * f32(side));
                c.put(Prop::LampPole, kMetal, pos3, yaw);
                c.put(Prop::LampHead, kLampHead, pos3, yaw);
            }
        }
        // Hydrants, round signs and the odd ad panel on the sidewalk side, deterministic by street.
        if (road && std::max(e.sidewalkL, e.sidewalkR) >= 1.5f && !(e.flags & kEdgeBridge)) {
            int k = 0;
            for (f32 s = g.tA + 10.0f + f32(ei % 11); s < g.length - g.tB - 10.0f; s += 38.0f, ++k) {
                const f32 h = noise::Rand01(i32(ei), k, seed + 301);
                Vec2 pos, dir;
                c.at(e, g, s, pos, dir);
                const int side = (noise::Rand01(i32(ei), k, seed + 302) < 0.5f) ? 1 : -1;
                const f32 w = side > 0 ? e.sidewalkR : e.sidewalkL;
                if (w < 1.5f) continue;
                const Vec2 right = RightOf(dir);
                const Vec2 p = pos + right * (f32(side) * (hw + 0.45f));
                if (!c.mine(p)) continue;
                const Vec3 pos3(p.x, c.ground(p) + kSidewalkTop, p.y);
                if (h < 0.30f) {
                    c.put(Prop::Hydrant, kPaintRed, pos3, 0.0f);
                } else if (h < 0.55f) { // a round sign facing the traffic of that side
                    const Vec2 face = side > 0 ? -dir : dir;
                    c.put(Prop::SignPost, kMetal, pos3, 0.0f);
                    c.put(Prop::SignDiscRed, kPaintRed, pos3, YawForZ(face));
                    c.put(Prop::SignDiscBlue, kPaintBlue, pos3, YawForZ(face));
                } else if (h < 0.63f && u8(e.cls) <= u8(RoadClass::Tertiary)) { // an ad panel facing the street
                    const Vec3 col = Hue(noise::Rand01(i32(ei), k, seed + 303));
                    c.put(Prop::AdPanel, kMetal, pos3, YawForZ(-right * f32(side)));
                    c.put(Prop::AdFace, kPaintWhite, pos3, YawForZ(-right * f32(side)), 1.0f, Tint(col.r, col.g, col.b));
                }
            }
        }
        // Overhead cables: concrete poles every ~36 m along one side of the quieter streets, three wires between them.
        if (u8(e.cls) >= u8(RoadClass::Tertiary) && u8(e.cls) <= u8(RoadClass::Living) && !(e.flags & kEdgePromenade)) {
            const int side = (ei % 2 == 0) ? 1 : -1;
            const f32 w = side > 0 ? e.sidewalkR : e.sidewalkL;
            if (w >= 1.5f) {
                std::vector<Vec3> tops;
                std::vector<Vec2> poles;
                for (f32 s = g.tA + 9.0f; s < g.length - g.tB - 6.0f; s += 36.0f) {
                    Vec2 pos, dir;
                    c.at(e, g, s, pos, dir);
                    poles.push_back(pos + RightOf(dir) * (f32(side) * (hw + 0.4f)));
                }
                for (size_t k = 0; k < poles.size(); ++k) {
                    const Vec2 p = poles[k];
                    if (!c.mine(p)) continue;
                    const f32 y = c.ground(p) + kSidewalkTop;
                    c.put(Prop::UtilityPole, kPaintWhite, Vec3(p.x, y, p.y), 0.0f, 1.0f, Tint(0.62f, 0.62f, 0.60f));
                    if (k + 1 >= poles.size()) continue;
                    const Vec2 q = poles[k + 1];
                    const Vec2 sp = glm::normalize(q - p), perp = RightOf(sp);
                    const f32 yq = c.ground(q) + kSidewalkTop;
                    for (const f32 off : {-0.7f, 0.0f, 0.7f}) c.wire(Vec3(p.x + perp.x * off, y + 8.55f, p.y + perp.y * off), Vec3(q.x + perp.x * off, yq + 8.55f, q.y + perp.y * off), 0.45f);
                }
            }
        }
        // Benches and bins along the promenade; postos, kiosks and nets on the sand beyond it.
        if (e.flags & kEdgePromenade) {
            const int seaSide = e.sidewalkR > 6.5f ? 1 : -1;
            const f32 sw = seaSide > 0 ? e.sidewalkR : e.sidewalkL;
            int k = 0;
            for (f32 s = g.tA + 10.0f; s < g.length - g.tB - 10.0f; s += 22.0f, ++k) {
                Vec2 pos, dir;
                c.at(e, g, s, pos, dir);
                const Vec2 right = RightOf(dir), sea = right * f32(seaSide);
                const Vec2 edge = pos + sea * (hw + sw - 0.9f);
                if (c.mine(edge)) {
                    const Vec3 b(edge.x, c.ground(edge) + kSidewalkTop, edge.y);
                    if (k % 2 == 0) c.put(Prop::Bench, kWood, b, YawForZ(sea));
                    else c.put(Prop::Bin, kPlastic, b + Vec3(sea.x * 0.0f, 0, sea.y * 0.0f), 0.0f);
                }
            }
            // Lifeguard posts every ~420 m, kiosks every ~170 m, nets in between.
            const f32 phase = noise::Rand01(i32(ei), 3, seed) * 60.0f;
            for (f32 s = g.tA + 40.0f + phase; s < g.length - g.tB - 20.0f; s += 70.0f) {
                const i32 idx = i32(std::floor((s + f32(ei) * 13.0f) / 70.0f));
                Vec2 pos, dir;
                c.at(e, g, s, pos, dir);
                const Vec2 right = RightOf(dir), sea = right * f32(seaSide);
                if (idx % 6 == 0) {
                    const Vec2 p = pos + sea * (hw + sw + 14.0f);
                    if (!c.mine(p)) continue;
                    const f32 y = c.ground(p);
                    if (y < 1.2f) continue;
                    c.put(Prop::PostoTower, kPaintWhite, Vec3(p.x, y, p.y), YawForZ(sea), 1.0f, Tint(0.95f, 0.95f, 0.92f));
                    c.put(Prop::PostoRoof, kPaintRed, Vec3(p.x, y, p.y), YawForZ(sea));
                } else if (idx % 6 == 3) {
                    const Vec2 p = pos + sea * (hw + sw + 4.5f);
                    if (!c.mine(p)) continue;
                    const f32 y = c.ground(p);
                    if (y < 1.2f) continue;
                    static const Vec3 kCol[4] = {Vec3(0.95f, 0.88f, 0.55f), Vec3(0.92f, 0.92f, 0.90f), Vec3(0.60f, 0.78f, 0.88f), Vec3(0.90f, 0.62f, 0.45f)};
                    const Vec3 col = kCol[u32(noise::Rand01(idx, i32(ei), seed + 5) * 4.0f) % 4u];
                    c.put(Prop::KioskBody, kPaintWhite, Vec3(p.x, y, p.y), YawForZ(sea), 1.0f, Tint(col.r, col.g, col.b));
                    c.put(Prop::KioskRoof, kPaintRed, Vec3(p.x, y, p.y), YawForZ(sea));
                } else if (idx % 6 == 1 || idx % 6 == 4) { // a volleyball court: two posts and two tapes
                    const Vec2 p = pos + sea * (hw + sw + 22.0f + 6.0f * noise::Rand01(idx, 4, seed));
                    if (!c.mine(p)) continue;
                    const f32 y = c.ground(p);
                    if (y < 1.4f) continue;
                    const Vec2 along = dir;
                    const Vec2 a = p - along * 4.0f, b = p + along * 4.0f;
                    c.put(Prop::NetPost, kMetal, Vec3(a.x, c.ground(a), a.y), 0.0f);
                    c.put(Prop::NetPost, kMetal, Vec3(b.x, c.ground(b), b.y), 0.0f);
                    c.put(Prop::NetBand, kPaintWhite, Vec3(p.x, y + 2.35f, p.y), YawForX(along));
                    c.put(Prop::NetBand, kPaintWhite, Vec3(p.x, y + 1.45f, p.y), YawForX(along));
                }
            }
        }
    }
}

// Parked cars along the kerb of streets with room for a parked row, in Rio's fleet colours (a few yellow taxis).
void ParkedCars(Ctx& c) {
    const CityData& d = *c.index.data;
    const auto it = c.index.edgesByChunk.find(ChunkKey(c.cx, c.cz));
    if (it == c.index.edgesByChunk.end()) return;
    const u32 seed = c.settings.seed;
    struct Paint {
        Vec3 c;
        f32 w;
    };
    static const Paint kPaint[] = {{Vec3(0.86f, 0.86f, 0.84f), 26}, {Vec3(0.60f, 0.61f, 0.63f), 20}, {Vec3(0.05f, 0.05f, 0.06f), 17}, {Vec3(0.28f, 0.29f, 0.31f), 12}, {Vec3(0.55f, 0.06f, 0.06f), 6},
                                   {Vec3(0.09f, 0.16f, 0.42f), 6}, {Vec3(0.68f, 0.60f, 0.47f), 5}, {Vec3(0.05f, 0.22f, 0.13f), 3}, {Vec3(0.92f, 0.70f, 0.04f), 5}};
    f32 total = 0.0f;
    for (const Paint& p : kPaint) total += p.w;
    const Prop bodies[3] = {Prop::CarHatchBody, Prop::CarSedanBody, Prop::CarSuvBody};
    const Prop glasses[3] = {Prop::CarHatchGlass, Prop::CarSedanGlass, Prop::CarSuvGlass};
    const Prop wheels[3] = {Prop::CarHatchWheels, Prop::CarSedanWheels, Prop::CarSuvWheels};
    // Zebra crossings keep clear.
    std::vector<Vec2> crossings;
    {
        const i32 x0 = ChunkOf(f32(c.cx) * kChunk - 20.0f), z0 = ChunkOf(f32(c.cz) * kChunk - 20.0f);
        for (i32 cz = z0; cz <= c.cz + 1; ++cz)
            for (i32 cx = x0; cx <= c.cx + 1; ++cx) {
                const auto pit = c.index.pointsByChunk.find(ChunkKey(cx, cz));
                if (pit == c.index.pointsByChunk.end()) continue;
                for (const u32 pi : pit->second)
                    if (d.points[pi].kind == PointKind::Crossing || d.points[pi].kind == PointKind::BusStop) crossings.push_back(d.points[pi].p);
            }
    }
    for (const u32 ei : it->second) {
        const CityEdge& e = d.edges[ei];
        const EdgeGeo& g = c.index.edgeGeo[ei];
        if (!g.valid || u8(e.cls) < u8(RoadClass::Secondary) || u8(e.cls) > u8(RoadClass::Living) || (e.flags & (kEdgeBridge | kEdgePromenade))) continue;
        const bool oneway = (e.flags & kEdgeOneway) != 0;
        const f32 hw = e.width * 0.5f;
        const int lanes = std::max<int>(e.lanes, oneway ? 1 : 2);
        const f32 room = e.width - f32(lanes) * 3.0f; // what is left of the carriageway for parked rows
        if (room < 1.95f) continue;
        const bool both = room >= 4.4f && !oneway;
        for (int side = -1; side <= 1; side += 2) {
            if (!both && side != ((ei % 2) ? 1 : -1)) continue;
            const f32 sw = side > 0 ? e.sidewalkR : e.sidewalkL;
            if (sw < 0.3f && u8(e.cls) < u8(RoadClass::Residential)) continue;
            int k = 0;
            for (f32 s = g.tA + 8.5f; s < g.length - g.tB - 8.5f; ++k) {
                const f32 jitter = noise::Rand01(i32(ei) * 7 + side, k, seed + 211);
                const f32 step = 5.4f + jitter * 1.2f;
                const f32 s0 = s;
                s += step;
                if (noise::Rand01(i32(ei) * 3 + side, k, seed + 212) < 0.14f) continue; // an empty space
                Vec2 pos, dir;
                c.at(e, g, s0, pos, dir);
                const Vec2 right = RightOf(dir);
                const Vec2 p = pos + right * (f32(side) * (hw - 1.02f));
                if (!c.mine(p)) continue;
                bool blocked = false;
                for (const Vec2& q : crossings)
                    if (glm::length(q - p) < 7.0f) blocked = true;
                if (blocked) continue;
                // not on a steep street: the cars cannot pitch
                const f32 yA = c.ground(p - dir * 2.2f), yB = c.ground(p + dir * 2.2f);
                if (std::abs(yB - yA) > 0.30f) continue;
                f32 pick = noise::Rand01(i32(ei) * 5 + side, k, seed + 213) * total;
                Vec3 col = kPaint[0].c;
                for (const Paint& pt : kPaint) {
                    if (pick < pt.w) {
                        col = pt.c;
                        break;
                    }
                    pick -= pt.w;
                }
                const u32 variant = u32(noise::Rand01(i32(ei) * 11 + side, k, seed + 214) * 3.0f) % 3u;
                const bool facing = oneway || side > 0; // right-hand traffic: the right kerb faces along the street
                const Vec2 face = facing ? dir : -dir;
                const f32 yaw = YawForX(face) + (noise::Rand01(i32(ei), k * 3 + side, seed + 215) - 0.5f) * 0.05f;
                const Vec3 pos3(p.x, (yA + yB) * 0.5f + 0.05f, p.y);
                c.put(bodies[variant], kCarPaint, pos3, yaw, 1.0f, Tint(col.r, col.g, col.b));
                c.put(glasses[variant], kGlassDark, pos3, yaw);
                c.put(wheels[variant], kRubber, pos3, yaw);
            }
        }
    }
}

// A pole with the street-name plates at one corner of each junction.
void JunctionSigns(Ctx& c) {
    const auto it = c.index.junctionsByChunk.find(ChunkKey(c.cx, c.cz));
    if (it == c.index.junctionsByChunk.end()) return;
    for (const u32 ji : it->second) {
        const Junction& j = c.index.junctions[ji];
        if (j.pavedOnly || j.sidewalks.empty()) continue;
        const std::vector<Vec2>& patch = j.sidewalks[size_t(noise::Rand01(i32(ji), 5, c.settings.seed) * f32(j.sidewalks.size())) % j.sidewalks.size()];
        if (patch.size() < 4) continue;
        const size_t arcN = patch.size() - 3;
        const Vec2 corner = patch[arcN + 1], arcMid = patch[arcN / 2];
        const Vec2 p = corner + glm::normalize(arcMid - corner) * 0.7f;
        if (!c.mine(p)) continue;
        const Vec3 pos3(p.x, c.ground(p) + kSidewalkTop, p.y);
        const f32 yaw = YawForX(glm::normalize(arcMid - corner));
        c.put(Prop::SignPost, kMetal, pos3, 0.0f);
        c.put(Prop::SignPlate, kPaintBlue, pos3, yaw);
    }
}

// Beach umbrellas: clusters on the sand between the promenade and the water.
void Umbrellas(Ctx& c) {
    const CityData& d = *c.index.data;
    const u32 seed = c.settings.seed;
    const f32 x0 = f32(c.cx) * kChunk, z0 = f32(c.cz) * kChunk;
    static const Vec3 kCol[7] = {Vec3(0.85f, 0.15f, 0.12f), Vec3(0.15f, 0.35f, 0.80f), Vec3(0.95f, 0.78f, 0.12f), Vec3(0.15f, 0.62f, 0.30f), Vec3(0.95f, 0.95f, 0.92f), Vec3(0.95f, 0.50f, 0.12f), Vec3(0.85f, 0.25f, 0.55f)};
    for (const u32 ai : c.index.beachAreas) {
        const CityArea& a = d.areas[ai];
        Vec2 lo = a.rings[0][0], hi = a.rings[0][0];
        for (const Vec2& p : a.rings[0]) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        if (hi.x < x0 || lo.x > x0 + kChunk || hi.y < z0 || lo.y > z0 + kChunk) continue;
        if (hi.x - lo.x < 150.0f) continue; // pocket beaches stay bare
        constexpr f32 step = 5.5f;
        for (f32 z = std::floor(std::max(lo.y, z0) / step) * step; z < std::min(hi.y, z0 + kChunk); z += step)
            for (f32 x = std::floor(std::max(lo.x, x0) / step) * step; x < std::min(hi.x, x0 + kChunk); x += step) {
                const i32 ix = i32(std::floor(x / step)), iz = i32(std::floor(z / step));
                const f32 cluster = noise::Fbm(x / 60.0f, z / 60.0f, seed + 71, 2) * 0.5f + 0.5f;
                const f32 r = noise::Rand01(ix, iz, seed + 72);
                if (r > 0.22f * cluster * cluster * 3.0f) continue;
                const Vec2 p(x + step * noise::Rand01(ix, iz, seed + 73) * 0.8f, z + step * noise::Rand01(ix, iz, seed + 74) * 0.8f);
                if (!c.mine(p) || !InsideRings(a.rings, p)) continue;
                const f32 y = c.ground(p);
                if (y < 1.5f || y > 3.2f) continue; // above the waves, below the promenade
                Vec2 toward;
                f32 dist;
                if (c.towardStreet(p, toward, dist) && dist < 18.0f) continue;
                const Vec3 col = kCol[u32(noise::Rand01(ix, iz, seed + 75) * 7.0f) % 7u];
                const f32 sc = 0.9f + 0.25f * noise::Rand01(ix, iz, seed + 76);
                c.put(Prop::UmbrellaPole, kPaintWhite, Vec3(p.x, y - 0.1f, p.y), noise::Rand01(ix, iz, seed + 77) * 6.28f, sc);
                c.put(Prop::UmbrellaCanopy, kPaintWhite, Vec3(p.x, y - 0.1f, p.y), noise::Rand01(ix, iz, seed + 77) * 6.28f, sc, Tint(col.r, col.g, col.b));
            }
    }
}

} // namespace

void BuildPropChunk(const CityIndex& index, const Terrain& terrain, const CitySettings& settings, i32 cx, i32 cz, ChunkParts& out) {
    Ctx c{index, terrain, settings, cx, cz, out};
    Points(c);
    AlongStreets(c);
    ParkedCars(c);
    JunctionSigns(c);
    Umbrellas(c);
}

} // namespace foh::city
