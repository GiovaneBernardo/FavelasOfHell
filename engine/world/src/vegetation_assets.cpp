#include "vegetation_assets.h"

#include <algorithm>
#include <cmath>

#include "foh/world/noise.h"

namespace foh {

namespace {

// ---- small helpers -------------------------------------------------------------------------------------------------

struct Rng {
    u32 s;
    explicit Rng(u32 seed) : s(seed * 2654435761u + 0x9E3779B9u) {}
    u32 nextU() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    f32 next() { return static_cast<f32>(nextU() & 0xFFFFFFu) * (1.0f / 16777216.0f); }
    f32 range(f32 a, f32 b) { return a + (b - a) * next(); }
    Vec3 inSphere() {
        for (;;) {
            const Vec3 v(range(-1, 1), range(-1, 1), range(-1, 1));
            const f32 l = glm::dot(v, v);
            if (l <= 1.0f && l > 1e-4f) return v;
        }
    }
};

struct Rect {
    i32 x0, y0, x1, y1;
};

void SetPixel(VegImage& im, i32 x, i32 y, const Rect& r, Vec3 rgb) {
    if (x < r.x0 || x >= r.x1 || y < r.y0 || y >= r.y1) return;
    u8* p = &im.rgba[(size_t(y) * im.width + size_t(x)) * 4];
    p[0] = static_cast<u8>(std::clamp(rgb.r, 0.0f, 255.0f));
    p[1] = static_cast<u8>(std::clamp(rgb.g, 0.0f, 255.0f));
    p[2] = static_cast<u8>(std::clamp(rgb.b, 0.0f, 255.0f));
    p[3] = 255;
}

// A thick line with linearly changing width and colour, stamped as discs (the brush of every cut-out here).
void StampLine(VegImage& im, const Rect& r, Vec2 a, Vec2 b, f32 w0, f32 w1, Vec3 c0, Vec3 c1) {
    const f32 len = glm::length(b - a);
    const int steps = std::max(2, static_cast<int>(len * 1.5f));
    for (int i = 0; i <= steps; ++i) {
        const f32 t = f32(i) / f32(steps);
        const Vec2 p = a + (b - a) * t;
        const f32 rad = std::max(0.5f, 0.5f * (w0 + (w1 - w0) * t));
        const Vec3 col = c0 + (c1 - c0) * t;
        for (i32 y = i32(std::floor(p.y - rad)); y <= i32(std::ceil(p.y + rad)); ++y)
            for (i32 x = i32(std::floor(p.x - rad)); x <= i32(std::ceil(p.x + rad)); ++x) {
                const f32 dx = f32(x) + 0.5f - p.x, dy = f32(y) + 0.5f - p.y;
                if (dx * dx + dy * dy <= rad * rad) SetPixel(im, x, y, r, col);
            }
    }
}

// ---- foliage textures -------------------------------------------------------------------------------------------------
// Everything is painted at twice the final size and box-filtered down, so the alpha of every cut-out has anti-aliased
// edges: the alpha test (cutoff 0.5) then follows the bilinear-filtered edge instead of stair-stepping along texels.

// A leaf: pointed ellipse along `angle`, with a lighter midrib, fine side veins and a slightly darker rim. Lengths in
// pixels of the image being painted.
void PaintLeaf(VegImage& im, const Rect& r, Vec2 c, f32 angle, f32 len, f32 wid, Vec3 color, f32 vein) {
    const f32 ca = std::cos(angle), sa = std::sin(angle);
    const f32 R = len * 0.5f + 2.0f;
    for (i32 y = i32(c.y - R); y <= i32(c.y + R); ++y)
        for (i32 x = i32(c.x - R); x <= i32(c.x + R); ++x) {
            const f32 dx = f32(x) + 0.5f - c.x, dy = f32(y) + 0.5f - c.y;
            const f32 u = dx * ca + dy * sa, v = -dx * sa + dy * ca;
            const f32 t = u / len + 0.5f;
            if (t < 0.0f || t > 1.0f) continue;
            // Widest a third of the way along, a drawn-out tip, a rounded base.
            const f32 halfW = wid * 0.5f * std::pow(std::sin(3.14159265f * std::pow(t, 0.62f)), 0.8f);
            if (std::abs(v) > halfW) continue;
            const f32 across = std::abs(v) / std::max(halfW, 0.01f); // 0 on the midrib .. 1 at the rim
            f32 shade = 1.0f - 0.18f * across * across;
            shade *= 1.0f + 0.10f * (0.5f - t); // base a little darker than the tip
            if (std::abs(v) < vein) shade *= 1.18f; // midrib
            // Side veins: faint diagonal ridges.
            const f32 ridge = std::sin((t * 9.0f + across * 0.9f) * 6.2831853f);
            shade *= 1.0f + 0.035f * ridge * (1.0f - across);
            SetPixel(im, x, y, r, color * shade);
        }
}

void FillLeafCluster(VegImage& im, const Rect& r, u32 seed, Vec3 greenA, Vec3 greenB, Vec3 accent) {
    Rng rng(seed);
    const f32 K = f32(r.x1 - r.x0) / 256.0f; // the proportions below were tuned on a 256 px cluster
    const Vec2 center(f32(r.x0 + r.x1) * 0.5f, f32(r.y0 + r.y1) * 0.5f);
    const f32 R = f32(r.x1 - r.x0) * 0.46f;
    // Thin twigs first, so leaves cover most of them.
    for (int k = 0; k < 9; ++k) {
        const f32 a = rng.range(0.0f, 6.2831853f), L = rng.range(0.55f, 0.95f) * R;
        const Vec2 mid = center + Vec2(std::cos(a + 0.3f), std::sin(a + 0.3f)) * L * 0.55f;
        const Vec2 end = center + Vec2(std::cos(a), std::sin(a)) * L;
        StampLine(im, r, center, mid, 3.0f * K, 2.2f * K, Vec3(64, 48, 32), Vec3(62, 47, 31));
        StampLine(im, r, mid, end, 2.2f * K, 1.2f * K, Vec3(62, 47, 31), Vec3(56, 43, 29));
    }
    // Leaves from the middle outward: the inner ones are deep in the shade of the crown, the outer ones catch the light,
    // and painting in that order lets the bright ones overlap the dark.
    struct Spot {
        f32 rr, ang;
    };
    std::vector<Spot> spots;
    const int count = 1300;
    spots.reserve(count);
    for (int k = 0; k < count; ++k) spots.push_back({R * std::pow(rng.next(), 0.55f), rng.range(0.0f, 6.2831853f)});
    std::sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) { return a.rr < b.rr; });
    for (const Spot& sp : spots) {
        const Vec2 c = center + Vec2(std::cos(sp.ang), std::sin(sp.ang)) * sp.rr;
        // Leaves point mostly outward and droop: nothing like the random sticks of a flat scatter.
        const f32 dir = sp.ang + rng.range(-1.1f, 1.1f);
        Vec3 col = glm::mix(greenA, greenB, std::clamp(0.5f * rng.next() + 0.6f * (sp.rr / R) - 0.1f, 0.0f, 1.0f));
        if (rng.next() < 0.04f) col = accent;
        col *= 0.52f + 0.42f * (sp.rr / R);
        col *= 0.92f + 0.16f * noise::ValueNoise(c.x / (22.0f * K), c.y / (22.0f * K), seed); // slow patches of lighter / darker foliage
        PaintLeaf(im, r, c, dir, rng.range(9.0f, 15.0f) * K, rng.range(4.0f, 6.6f) * K, col, 0.5f * K);
    }
}

void FillFrondRegion(VegImage& im) {
    // Bottom band (full width, half the height of the image): rachis along the middle, pairs of long thin leaflets
    // sweeping forward and down.
    const f32 K = f32(im.width) / 512.0f;
    const Rect r{0, i32(im.height) / 2, i32(im.width), i32(im.height)};
    Rng rng(31);
    const f32 midY = f32(r.y0) + f32(r.y1 - r.y0) * 0.5f;
    StampLine(im, r, Vec2(2 * K, midY), Vec2(508 * K, midY - 6.0f * K), 7.0f * K, 2.5f * K, Vec3(70, 92, 40), Vec3(60, 96, 40));
    for (int k = 0; k < 70; ++k) {
        const f32 t = f32(k) / 69.0f;
        const f32 x = (20.0f + t * 470.0f) * K;
        const f32 y = midY - 6.0f * t * K;
        const f32 len = (24.0f + 100.0f * std::pow(std::sin(3.14159265f * (0.08f + 0.9f * t)), 0.8f)) * rng.range(0.9f, 1.05f) * K;
        for (int side = -1; side <= 1; side += 2) {
            const Vec2 d = glm::normalize(Vec2(0.62f, f32(side) * 0.78f));
            const Vec2 a(x, y), mid = a + d * len * 0.55f + Vec2(0.0f, f32(side) * 4.0f * K), b = a + d * len + Vec2(0.0f, f32(side) * 14.0f * K);
            const Vec3 c0(38, 88, 28), c1 = glm::mix(Vec3(62, 112, 36), Vec3(88, 126, 44), rng.next());
            StampLine(im, r, a, mid, 6.0f * K, 5.0f * K, c0, glm::mix(c0, c1, 0.6f));
            StampLine(im, r, mid, b, 5.0f * K, 1.0f * K, glm::mix(c0, c1, 0.6f), c1);
        }
    }
}

// 2x2 box filter that weights the colour by alpha (no dark fringes where a leaf meets the empty background) and keeps the
// averaged alpha: the soft edge the supersampling is for.
VegImage Downsample2(const VegImage& hi) {
    VegImage lo;
    lo.width = hi.width / 2;
    lo.height = hi.height / 2;
    lo.rgba.assign(size_t(lo.width) * lo.height * 4, 0);
    for (u32 y = 0; y < lo.height; ++y)
        for (u32 x = 0; x < lo.width; ++x) {
            f32 rgb[3] = {0, 0, 0}, a = 0.0f;
            for (u32 dy = 0; dy < 2; ++dy)
                for (u32 dx = 0; dx < 2; ++dx) {
                    const u8* p = &hi.rgba[(size_t(y * 2 + dy) * hi.width + (x * 2 + dx)) * 4];
                    const f32 wa = f32(p[3]);
                    for (int c = 0; c < 3; ++c) rgb[c] += f32(p[c]) * wa;
                    a += wa;
                }
            u8* o = &lo.rgba[(size_t(y) * lo.width + x) * 4];
            for (int c = 0; c < 3; ++c) o[c] = static_cast<u8>(a > 0.0f ? rgb[c] / a : 0.0f);
            o[3] = static_cast<u8>(a * 0.25f);
        }
    return lo;
}

} // namespace

VegImage BuildLeafAtlas() {
    VegImage hi;
    hi.width = 2048; // painted at 2x of the 1024 px atlas
    hi.height = 2048;
    hi.rgba.assign(size_t(2048) * 2048 * 4, 0);
    FillLeafCluster(hi, {0, 0, 1024, 1024}, 11, Vec3(24, 58, 20), Vec3(56, 98, 30), Vec3(96, 112, 34));
    FillLeafCluster(hi, {1024, 0, 2048, 1024}, 12, Vec3(32, 66, 22), Vec3(70, 102, 30), Vec3(122, 118, 36)); // a warmer, yellower tree
    FillFrondRegion(hi);
    return Downsample2(hi);
}

VegImage BuildBarkImage() {
    VegImage im;
    im.width = 128;
    im.height = 128;
    im.rgba.assign(size_t(128) * 128 * 4, 255);
    for (i32 y = 0; y < 128; ++y)
        for (i32 x = 0; x < 128; ++x) {
            auto n = [&](f32 fx, f32 fy, u32 seed) {
                const f32 a = noise::ValueNoise(f32(x) * fx, f32(y) * fy, seed), b = noise::ValueNoise(f32(x - 128) * fx, f32(y) * fy, seed);
                return a + (b - a) * (f32(x) / 128.0f);
            };
            const f32 streak = 0.5f + 0.5f * n(0.1f, 0.025f, 21);
            f32 shade = 0.45f + 0.7f * streak;
            if (0.5f + 0.5f * n(0.3f, 0.07f, 22) > 0.78f) shade *= 0.55f;
            u8* p = &im.rgba[(size_t(y) * 128 + size_t(x)) * 4];
            p[0] = static_cast<u8>(std::clamp(86.0f * shade, 0.0f, 255.0f));
            p[1] = static_cast<u8>(std::clamp(68.0f * shade, 0.0f, 255.0f));
            p[2] = static_cast<u8>(std::clamp(52.0f * shade, 0.0f, 255.0f));
        }
    return im;
}

// A tuft: ~340 blades fanning out of the bottom middle, each a tapered curved ribbon, dark at the root (the ground
// shades it) and yellow-green at the tip, with a few dry straw-coloured ones. 512 px, painted at 1024.
VegImage BuildGrassImage() {
    VegImage hi;
    hi.width = 1024;
    hi.height = 1024;
    hi.rgba.assign(size_t(1024) * 1024 * 4, 0);
    const Rect r{0, 0, 1024, 1024};
    Rng rng(7);
    struct Blade {
        f32 baseX, lean, len, w, bright, curl;
        bool dry;
        f32 mixTip;
    };
    std::vector<Blade> blades;
    for (int k = 0; k < 340; ++k) {
        Blade b;
        b.baseX = 512.0f + rng.range(-250.0f, 250.0f);
        const f32 edge = std::abs(b.baseX - 512.0f) / 270.0f;
        b.lean = rng.range(-1.0f, 1.0f) * (0.30f + edge * 0.9f) + (b.baseX - 512.0f) / 900.0f; // the outer blades splay
        b.len = rng.range(380.0f, 990.0f) * (1.0f - 0.3f * edge * edge);
        b.w = rng.range(9.0f, 17.0f);
        b.bright = rng.range(0.8f, 1.2f);
        b.curl = rng.range(0.5f, 1.4f);
        b.dry = rng.next() < 0.07f;
        b.mixTip = rng.next();
        blades.push_back(b);
    }
    // Short blades first (they are in the back), long ones over them.
    std::sort(blades.begin(), blades.end(), [](const Blade& a, const Blade& b) { return a.len < b.len; });
    for (const Blade& b : blades) {
        const Vec3 dark = Vec3(20, 46, 13) * b.bright;
        const Vec3 mid = glm::mix(Vec3(30, 70, 18), Vec3(46, 86, 24), b.mixTip) * b.bright;
        const Vec3 tip = b.dry ? Vec3(150, 128, 62) * b.bright : glm::mix(Vec3(66, 106, 30), Vec3(106, 130, 42), b.mixTip) * b.bright;
        auto colorAt = [&](f32 t) {
            const Vec3 c = t < 0.35f ? glm::mix(dark, b.dry ? tip * 0.8f : mid, t / 0.35f) : glm::mix(b.dry ? tip * 0.8f : mid, tip, (t - 0.35f) / 0.65f);
            return c;
        };
        Vec2 prev(b.baseX, 1023.5f);
        const int segs = 22;
        for (int s = 1; s <= segs; ++s) {
            const f32 t = f32(s) / f32(segs), t0 = f32(s - 1) / f32(segs);
            // A blade rises straight, then bends over toward its lean: a parabola with a small wobble.
            const Vec2 p(b.baseX + b.lean * b.len * std::pow(t, 1.0f + b.curl) * 0.75f + std::sin(t * 3.0f + b.baseX) * 3.0f, 1023.5f - b.len * t * (1.0f - 0.12f * t * b.lean * b.lean));
            StampLine(hi, r, prev, p, b.w * std::pow(1.0f - t0, 0.7f), b.w * std::pow(1.0f - t, 0.7f), colorAt(t0), colorAt(t));
            prev = p;
        }
    }
    return Downsample2(hi);
}

std::vector<std::vector<u8>> BuildFoliageMips(const VegImage& image, f32 cutoff, u32 maxLevels) {
    std::vector<std::vector<u8>> mips;
    mips.push_back(image.rgba);
    auto coverageOf = [&](const std::vector<u8>& px, f32 scale) {
        size_t pass = 0, total = px.size() / 4;
        for (size_t i = 0; i < total; ++i) pass += (f32(px[i * 4 + 3]) * scale / 255.0f) > cutoff;
        return total ? f32(pass) / f32(total) : 0.0f;
    };
    const f32 target = cutoff > 0.0f ? coverageOf(mips[0], 1.0f) : 0.0f;
    u32 w = image.width, h = image.height;
    while (mips.size() < maxLevels && (w > 1 || h > 1)) {
        const u32 nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        const std::vector<u8>& src = mips.back();
        std::vector<u8> dst(size_t(nw) * nh * 4);
        for (u32 y = 0; y < nh; ++y)
            for (u32 x = 0; x < nw; ++x) {
                f32 rgb[3] = {0, 0, 0}, plain[3] = {0, 0, 0}, alpha = 0.0f, weight = 0.0f;
                for (u32 dy = 0; dy < 2; ++dy)
                    for (u32 dx = 0; dx < 2; ++dx) {
                        const u8* p = &src[(size_t(std::min(h - 1, y * 2 + dy)) * w + std::min(w - 1, x * 2 + dx)) * 4];
                        const f32 wa = cutoff > 0.0f ? f32(p[3]) : 255.0f; // colour weighted by alpha for cut-outs
                        for (int c = 0; c < 3; ++c) {
                            rgb[c] += f32(p[c]) * wa;
                            plain[c] += f32(p[c]);
                        }
                        weight += wa;
                        alpha += f32(p[3]);
                    }
                u8* o = &dst[(size_t(y) * nw + x) * 4];
                for (int c = 0; c < 3; ++c) o[c] = static_cast<u8>(weight > 0.0f ? rgb[c] / weight : plain[c] * 0.25f);
                o[3] = static_cast<u8>(alpha * 0.25f);
            }
        if (cutoff > 0.0f) {
            // Keep the share of texels that pass the alpha test.
            f32 lo = 1.0f, hi = 6.0f;
            for (int it = 0; it < 14; ++it) {
                const f32 mid = 0.5f * (lo + hi);
                (coverageOf(dst, mid) < target ? lo : hi) = mid;
            }
            const f32 scale = 0.5f * (lo + hi);
            for (size_t i = 0; i < size_t(nw) * nh; ++i) dst[i * 4 + 3] = static_cast<u8>(std::min(255.0f, f32(dst[i * 4 + 3]) * scale));
        }
        mips.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
    return mips;
}

// ---- meshes --------------------------------------------------------------------------------------------------------

namespace {

void AddQuad(VegMesh& m, const Vec3 p[4], const Vec3 n[4], const Vec2 uv[4], f32 ao) {
    const u32 base = static_cast<u32>(m.positions.size());
    for (int i = 0; i < 4; ++i) {
        m.positions.push_back(p[i]);
        m.normals.push_back(n[i]);
        m.uvs.push_back(uv[i]);
        m.tangents.push_back(Vec4(1.0f, 0.0f, 0.0f, ao * 2.0f - 1.0f)); // w: baked occlusion (see opaque.slang)
    }
    for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
}

// A tapered tube from `a` to `b` (curving by `bend` at the far end) with a flared foot (radius * (1 + flare * exp(-8 t))).
// UVs repeat the bark every 1.2 m (u around the circumference in whole repeats, v along the length); tangents follow u.
void AddTube(VegMesh& m, Vec3 a, Vec3 b, f32 r0, f32 r1, f32 flare, u32 sides, u32 segments, Vec3 bend = Vec3(0.0f)) {
    const Vec3 axis = glm::normalize(b - a);
    const Vec3 ref = std::abs(axis.y) > 0.9f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
    const Vec3 right = glm::normalize(glm::cross(axis, ref)), up = glm::cross(right, axis);
    const u32 start = static_cast<u32>(m.positions.size());
    const f32 repeats = std::max(1.0f, std::round(6.2831853f * r0 / 1.2f));
    f32 travelled = 0.0f;
    Vec3 prev = a;
    for (u32 s = 0; s <= segments; ++s) {
        const f32 t = f32(s) / f32(segments);
        const Vec3 c = a + (b - a) * t + bend * (t * t);
        travelled += glm::length(c - prev);
        prev = c;
        const f32 r = (r0 + (r1 - r0) * t) * (1.0f + flare * std::exp(-8.0f * t));
        for (u32 k = 0; k <= sides; ++k) {
            const f32 ang = f32(k) / f32(sides) * 6.2831853f;
            const Vec3 radial = right * std::cos(ang) + up * std::sin(ang);
            m.positions.push_back(c + radial * r);
            m.normals.push_back(radial);
            m.uvs.push_back({f32(k) / f32(sides) * repeats, travelled / 1.2f});
            m.tangents.push_back(Vec4(glm::normalize(-right * std::sin(ang) + up * std::cos(ang)), 1.0f));
        }
    }
    const u32 stride = sides + 1;
    for (u32 s = 0; s < segments; ++s)
        for (u32 k = 0; k < sides; ++k) {
            const u32 i0 = start + s * stride + k, i1 = i0 + 1, i2 = i0 + stride, i3 = i2 + 1;
            // Counter-clockwise seen from outside (the pipelines use frontCounterClockwise + back-face culling).
            for (u32 i : {i0, i2, i1, i1, i2, i3}) m.indices.push_back(i);
        }
}

// A card (quad) at `center`, oriented by `q`, size x size; normals shaded as part of a ball around `crown`.
void AddLeafCard(VegMesh& m, Vec3 center, Quat q, f32 size, Vec3 crown, f32 uMin, f32 uMax, f32 vMin, f32 vMax, f32 ao) {
    const f32 h = size * 0.5f;
    const Vec3 local[4] = {{-h, -h, 0}, {h, -h, 0}, {h, h, 0}, {-h, h, 0}};
    const Vec2 uv[4] = {{uMin, vMax}, {uMax, vMax}, {uMax, vMin}, {uMin, vMin}};
    const Vec3 cardN = q * Vec3(0, 0, 1);
    Vec3 p[4], n[4];
    for (int i = 0; i < 4; ++i) {
        p[i] = center + q * local[i];
        // Soft, volumetric lighting: mostly the direction away from the crown centre, a little of the card's own facing.
        const Vec3 out = glm::normalize(p[i] - crown);
        const Vec3 facing = glm::dot(cardN, out) >= 0.0f ? cardN : -cardN;
        n[i] = glm::normalize(out * 0.85f + facing * 0.15f + Vec3(0, 0.2f, 0));
    }
    AddQuad(m, p, n, uv, ao);
}

void AddLeafClusters(VegMesh& m, Rng& rng, Vec3 crown, f32 rx, f32 ry, f32 floorY, int count, f32 sizeMin, f32 sizeMax) {
    for (int i = 0; i < count; ++i) {
        const Vec3 d = glm::normalize(rng.inSphere());
        const f32 shell = rng.range(0.30f, 1.0f); // how far out in the crown: 1 = the surface
        Vec3 p = crown + Vec3(d.x * rx, d.y * ry, d.z * rx) * shell;
        p.y = std::max(p.y, floorY);
        const f32 size = rng.range(sizeMin, sizeMax);
        // Cards face outward with a lot of scatter, so neighbours catch the light at different angles: a crown of
        // randomly standing quads reads as cardboard, a crown of leaf masses turned every which way reads as foliage.
        const Vec3 out = glm::normalize(p - crown + Vec3(0.0f, 0.001f, 0.0f));
        const Vec3 facing = glm::normalize(out * 0.55f + rng.inSphere() * 0.75f + Vec3(0.0f, 0.25f, 0.0f));
        const Vec3 spin = glm::cross(Vec3(0, 0, 1), facing) + Vec3(1e-6f, 0.0f, 0.0f);
        const Quat tilt = glm::angleAxis(std::acos(std::clamp(facing.z, -1.0f, 1.0f)), glm::normalize(spin));
        const Quat q = glm::angleAxis(rng.range(0.0f, 6.2831853f), facing) * tilt;
        // Occlusion baked per card: dark deep inside the crown and on its underside, open at the top and rim.
        const f32 height = std::clamp((p.y - crown.y) / std::max(ry, 0.01f) * 0.5f + 0.5f, 0.0f, 1.0f);
        const f32 ao = std::clamp((0.22f + 0.78f * std::pow(shell, 1.4f)) * (0.72f + 0.28f * height), 0.0f, 1.0f);
        const bool second = rng.next() < 0.5f; // which of the two twigs of the atlas
        const f32 u0 = second ? 0.51f : 0.01f, u1 = second ? 0.99f : 0.49f;
        AddLeafCard(m, p, q, size, crown, u0, u1, 0.01f, 0.49f, ao);
        // A second card across the first (rotated about the card's own up axis) so the clump has depth from any side.
        AddLeafCard(m, p, q * glm::angleAxis(rng.range(1.0f, 2.1f), Vec3(0, 1, 0)), size, crown, u0, u1, 0.01f, 0.49f, ao);
    }
}

} // namespace

TreeMeshes BuildTree(u32 variant, u32 lod) {
    TreeMeshes t;
    Rng rng(1234 + variant * 77); // the same seed for both LODs: the silhouettes match
    const f32 height = 11.0f + 2.2f * f32(variant) + rng.range(0.0f, 1.5f);
    const f32 trunkH = height * 0.60f;
    const f32 r0 = 0.30f + 0.035f * f32(variant);
    const Vec3 lean(rng.range(-0.9f, 0.9f), 0.0f, rng.range(-0.9f, 0.9f));
    t.trunkRadius = r0;
    t.height = height;
    AddTube(t.trunk, Vec3(0), Vec3(0, trunkH, 0), r0, 0.13f, 0.55f, lod == 0 ? 10u : 6u, lod == 0 ? 7u : 3u, lean);

    const Vec3 top = Vec3(0, trunkH, 0) + lean;
    const Vec3 crown = top + Vec3(0, height * 0.17f, 0);
    const f32 rx = 0.30f * height, ry = 0.21f * height;
    if (lod == 0) {
        // Branches from the upper trunk toward the leaf masses.
        for (int b = 0; b < 6; ++b) {
            const f32 along = rng.range(0.55f, 0.95f);
            const Vec3 from = Vec3(0, trunkH * along, 0) + lean * (along * along);
            const f32 ang = rng.range(0.0f, 6.2831853f);
            const Vec3 to = crown + Vec3(std::cos(ang) * rx * rng.range(0.4f, 0.8f), rng.range(-0.2f, 0.5f) * ry, std::sin(ang) * rx * rng.range(0.4f, 0.8f));
            AddTube(t.trunk, from, to, 0.11f, 0.03f, 0.0f, 5, 3, Vec3(0, -0.5f, 0));
        }
    }
    Rng leafRng(99 + variant * 31);
    AddLeafClusters(t.leaves, leafRng, crown, rx, ry, trunkH * 0.80f, lod == 0 ? 120 : 34, lod == 0 ? 1.9f : 3.4f, lod == 0 ? 3.1f : 5.2f);
    return t;
}

TreeMeshes BuildPalm(u32 lod) {
    TreeMeshes t;
    Rng rng(4242);
    const f32 height = 9.5f;
    const Vec3 lean(1.6f, 0.0f, 0.5f);
    t.trunkRadius = 0.24f;
    t.height = height + 2.0f;
    AddTube(t.trunk, Vec3(0), Vec3(0, height, 0), 0.24f, 0.15f, 0.35f, lod == 0 ? 8u : 5u, lod == 0 ? 8u : 3u, lean);
    const Vec3 top = Vec3(0, height, 0) + lean;
    const int fronds = lod == 0 ? 14 : 8;
    const int segments = lod == 0 ? 4 : 2;
    for (int f = 0; f < fronds; ++f) {
        const f32 yaw = (f32(f) + rng.range(-0.25f, 0.25f)) / f32(fronds) * 6.2831853f;
        const Vec3 dir(std::cos(yaw), 0.0f, std::sin(yaw));
        const Vec3 side(-dir.z, 0.0f, dir.x);
        const f32 L = rng.range(3.6f, 4.8f), arc = rng.range(1.2f, 2.0f), droop = rng.range(1.6f, 3.0f), halfW = 1.25f;
        const u32 start = static_cast<u32>(t.leaves.positions.size());
        for (int s = 0; s <= segments; ++s) {
            const f32 u = f32(s) / f32(segments);
            const Vec3 c = top + dir * (L * u) + Vec3(0, arc * std::sin(3.14159265f * u * 0.75f) - droop * u * u, 0);
            const f32 w = halfW * (0.55f + 0.45f * std::sin(3.14159265f * std::min(1.0f, u * 1.15f)));
            for (int e = 0; e < 2; ++e) {
                t.leaves.positions.push_back(c + side * (e == 0 ? -w : w));
                t.leaves.normals.push_back(glm::normalize(Vec3(0, 1, 0) + dir * 0.2f));
                t.leaves.uvs.push_back({0.01f + 0.98f * u, e == 0 ? 0.51f : 0.99f});
                t.leaves.tangents.push_back(Vec4(1.0f, 0.0f, 0.0f, 1.0f));
            }
        }
        for (int s = 0; s < segments; ++s) {
            const u32 a = start + u32(s) * 2;
            for (u32 i : {a, a + 2, a + 1, a + 1, a + 2, a + 3}) t.leaves.indices.push_back(i);
        }
    }
    return t;
}

VegMesh BuildGrassMesh() {
    VegMesh m;
    const f32 w = 0.85f, h = 0.58f;
    for (int k = 0; k < 3; ++k) {
        const f32 a = f32(k) * 1.0471976f;
        const Vec3 dir(std::cos(a), 0.0f, std::sin(a)), out(-dir.z, 0.0f, dir.x);
        // Three rows, the tips leaning outward a little so the silhouette is not a stiff card.
        const f32 rows[3] = {0.0f, 0.5f, 1.0f}, lean[3] = {0.0f, 0.03f, 0.12f};
        const u32 start = static_cast<u32>(m.positions.size());
        for (int r = 0; r < 3; ++r)
            for (int e = 0; e < 2; ++e) {
                const f32 side = e == 0 ? -0.5f : 0.5f;
                m.positions.push_back(dir * (side * w * (1.0f - 0.15f * rows[r])) + out * lean[r] + Vec3(0, rows[r] * h, 0));
                m.normals.push_back(glm::normalize(Vec3(0, 1, 0) + out * 0.15f));
                m.uvs.push_back({e == 0 ? 0.0f : 1.0f, 1.0f - rows[r]});
            }
        for (int r = 0; r < 2; ++r) {
            const u32 q = start + u32(r) * 2;
            for (u32 i : {q, q + 1, q + 2, q + 1, q + 3, q + 2}) m.indices.push_back(i);
        }
    }
    return m;
}

} // namespace foh
