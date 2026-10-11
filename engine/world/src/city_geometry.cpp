#include <algorithm>
#include <cmath>

#include "city_internal.h"

namespace foh::city {

namespace {

bool SegmentsCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    const f32 d1 = Cross(b - a, c - a), d2 = Cross(b - a, d - a), d3 = Cross(d - c, a - c), d4 = Cross(d - c, b - c);
    return ((d1 > 1e-6f && d2 < -1e-6f) || (d1 < -1e-6f && d2 > 1e-6f)) && ((d3 > 1e-6f && d4 < -1e-6f) || (d3 < -1e-6f && d4 > 1e-6f));
}

bool InTriangle(Vec2 p, Vec2 a, Vec2 b, Vec2 c, f32 sign) {
    const f32 d1 = Cross(b - a, p - a) * sign, d2 = Cross(c - b, p - b) * sign, d3 = Cross(a - c, p - c) * sign;
    return d1 > 1e-5f && d2 > 1e-5f && d3 > 1e-5f;
}

} // namespace

void Triangulate(const std::vector<std::vector<Vec2>>& rings, std::vector<Vec2>& verts, std::vector<u32>& tris) {
    verts.clear();
    tris.clear();
    if (rings.empty() || rings[0].size() < 3) return;
    std::vector<Vec2> poly = rings[0];
    // Bridge every hole into the outline: connect its rightmost vertex to the nearest outline vertex it can see.
    std::vector<std::vector<Vec2>> holes(rings.begin() + 1, rings.end());
    std::sort(holes.begin(), holes.end(), [](const auto& a, const auto& b) {
        f32 ma = -1e30f, mb = -1e30f;
        for (const Vec2& p : a) ma = std::max(ma, p.x);
        for (const Vec2& p : b) mb = std::max(mb, p.x);
        return ma > mb;
    });
    for (size_t hi = 0; hi < holes.size(); ++hi) {
        const auto& hole = holes[hi];
        if (hole.size() < 3) continue;
        size_t hv = 0;
        for (size_t i = 1; i < hole.size(); ++i)
            if (hole[i].x > hole[hv].x) hv = i;
        std::vector<std::pair<f32, size_t>> cand;
        for (size_t i = 0; i < poly.size(); ++i) cand.push_back({glm::length(poly[i] - hole[hv]), i});
        std::sort(cand.begin(), cand.end());
        size_t pick = cand[0].second;
        for (const auto& [dist, ci] : cand) {
            bool blocked = false;
            auto test = [&](const std::vector<Vec2>& r) {
                for (size_t i = 0; i < r.size() && !blocked; ++i) blocked = SegmentsCross(hole[hv], poly[ci], r[i], r[(i + 1) % r.size()]);
            };
            test(poly);
            for (size_t k = hi; k < holes.size() && !blocked; ++k) test(holes[k]);
            if (!blocked) {
                pick = ci;
                break;
            }
        }
        std::vector<Vec2> merged(poly.begin(), poly.begin() + std::ptrdiff_t(pick) + 1);
        for (size_t k = 0; k <= hole.size(); ++k) merged.push_back(hole[(hv + k) % hole.size()]);
        merged.push_back(poly[pick]);
        merged.insert(merged.end(), poly.begin() + std::ptrdiff_t(pick) + 1, poly.end());
        poly = std::move(merged);
    }
    verts = poly;
    const f32 sign = SignedArea(poly) >= 0.0f ? 1.0f : -1.0f;
    std::vector<u32> rest(poly.size());
    for (u32 i = 0; i < rest.size(); ++i) rest[i] = i;
    u32 guard = 0;
    while (rest.size() > 3 && guard++ < 100000) {
        bool clipped = false;
        const size_t n = rest.size();
        for (size_t i = 0; i < n && !clipped; ++i) {
            const u32 ia = rest[(i + n - 1) % n], ib = rest[i], ic = rest[(i + 1) % n];
            const Vec2 a = poly[ia], b = poly[ib], c = poly[ic];
            if (Cross(b - a, c - b) * sign <= 1e-7f) continue; // reflex or flat
            bool empty = true;
            for (size_t k = 0; k < n && empty; ++k) {
                const u32 id = rest[k];
                if (id == ia || id == ib || id == ic) continue;
                const Vec2 p = poly[id];
                if (p == a || p == b || p == c) continue;
                if (InTriangle(p, a, b, c, sign)) empty = false;
            }
            if (!empty) continue;
            tris.insert(tris.end(), {ia, ib, ic});
            rest.erase(rest.begin() + std::ptrdiff_t(i));
            clipped = true;
        }
        if (!clipped) { // degenerate input: drop the flattest vertex so the loop ends
            size_t best = 0;
            f32 bestArea = 1e30f;
            for (size_t i = 0; i < n; ++i) {
                const f32 ar = std::abs(Cross(poly[rest[i]] - poly[rest[(i + n - 1) % n]], poly[rest[(i + 1) % n]] - poly[rest[i]]));
                if (ar < bestArea) {
                    bestArea = ar;
                    best = i;
                }
            }
            rest.erase(rest.begin() + std::ptrdiff_t(best));
        }
    }
    if (rest.size() == 3) tris.insert(tris.end(), {rest[0], rest[1], rest[2]});
}

} // namespace foh::city
