// Procedural content of the district: textures (facades, shop fronts, paving, asphalt, markings) and prop meshes, generated in
// code so the city looks like a city without any imported asset (ADR 0014). They are stand-ins with a deliberate interface:
// every texture is one repeat of a real-world size (a facade texture is one storey of one bay), so an artist-made set of the
// same sizes replaces them without touching the generators.
#include <algorithm>
#include <array>
#include <cmath>

#include "city_internal.h"
#include "foh/log.h"
#include "foh/world/noise.h"

namespace foh::city {

namespace {

constexpr f32 kBayW = 3.6f, kStoreyH = 3.0f, kShopH = 3.6f;

// ---- small helpers -------------------------------------------------------------------------------------------------

f32 Sat(f32 v) { return std::clamp(v, 0.0f, 1.0f); }
f32 Mixf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
Vec3 MixC(Vec3 a, Vec3 b, f32 t) { return a + (b - a) * t; }
f32 Step(f32 a, f32 b, f32 x) {
    const f32 t = Sat((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}
bool In(f32 x, f32 y, f32 x0, f32 y0, f32 x1, f32 y1) { return x >= x0 && x < x1 && y >= y0 && y < y1; }

// Value noise that repeats every (px, py) cells, so the textures tile.
f32 PNoise(f32 x, f32 y, int px, int py, u32 seed) {
    const f32 fx = std::floor(x), fy = std::floor(y);
    const int ix = int(fx), iy = int(fy);
    const f32 tx = noise::Fade(x - fx), ty = noise::Fade(y - fy);
    auto v = [&](int dx, int dy) {
        const int a = ((ix + dx) % px + px) % px, b = ((iy + dy) % py + py) % py;
        return noise::Rand01(a, b, seed);
    };
    const f32 a = Mixf(v(0, 0), v(1, 0), tx), b = Mixf(v(0, 1), v(1, 1), tx);
    return Mixf(a, b, ty); // 0..1
}
f32 PFbm(f32 u, f32 v, int period, u32 seed, int oct = 4) { // u, v in [0, 1): one repeat of the texture
    f32 sum = 0.0f, amp = 0.5f, norm = 0.0f;
    int per = period;
    for (int o = 0; o < oct; ++o) {
        sum += amp * PNoise(u * f32(per), v * f32(per), per, per, seed + u32(o) * 31u);
        norm += amp;
        per *= 2;
        amp *= 0.5f;
    }
    return sum / norm;
}

struct Texel {
    Vec3 c{0.5f};      // sRGB albedo 0..1
    f32 h = 0.0f;      // relief in metres (normal map)
    f32 rough = 0.85f; // metal/roughness texture
    f32 metal = 0.0f;
    f32 alpha = 1.0f;
};

struct Maps {
    u32 w = 0, h = 0;
    std::vector<u8> albedo, normal, mr;
};

// Evaluates `f(u, v)` (0..1, v up) for every pixel; builds albedo, a normal map from the relief (`reliefScale` metres per
// unit of h per texture size) and the metal/roughness map.
template <class F>
Maps Bake(u32 size, f32 widthM, f32 heightM, F&& f, bool wantNormal = true, bool wantMr = true) {
    Maps m;
    m.w = m.h = size;
    std::vector<Texel> t(size_t(size) * size);
    for (u32 j = 0; j < size; ++j)
        for (u32 i = 0; i < size; ++i) t[size_t(j) * size + i] = f((f32(i) + 0.5f) / f32(size), (f32(j) + 0.5f) / f32(size));
    m.albedo.resize(t.size() * 4);
    if (wantNormal) m.normal.resize(t.size() * 4);
    if (wantMr) m.mr.resize(t.size() * 4);
    auto q = [](f32 v) { return u8(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    for (u32 j = 0; j < size; ++j)
        for (u32 i = 0; i < size; ++i) {
            const Texel& x = t[size_t(j) * size + i];
            u8* a = &m.albedo[(size_t(j) * size + i) * 4];
            a[0] = q(x.c.r);
            a[1] = q(x.c.g);
            a[2] = q(x.c.b);
            a[3] = q(x.alpha);
            if (wantNormal) {
                auto at = [&](int di, int dj) { return t[size_t((int(j) + dj + int(size)) % int(size)) * size + size_t((int(i) + di + int(size)) % int(size))].h; };
                const f32 dx = (at(1, 0) - at(-1, 0)) / (2.0f * widthM / f32(size)), dy = (at(0, 1) - at(0, -1)) / (2.0f * heightM / f32(size));
                Vec3 n = glm::normalize(Vec3(-dx, -dy, 1.0f));
                u8* o = &m.normal[(size_t(j) * size + i) * 4];
                o[0] = q(n.x * 0.5f + 0.5f);
                o[1] = q(n.y * 0.5f + 0.5f);
                o[2] = q(n.z * 0.5f + 0.5f);
                o[3] = 255;
            }
            if (wantMr) {
                u8* o = &m.mr[(size_t(j) * size + i) * 4];
                o[0] = 255;
                o[1] = q(x.rough);
                o[2] = q(x.metal);
                o[3] = 255;
            }
        }
    return m;
}

std::vector<std::vector<u8>> Mips(const std::vector<u8>& base, u32 size, bool alphaWeighted = false) {
    std::vector<std::vector<u8>> out;
    out.push_back(base);
    u32 w = size;
    while (w > 1) {
        const u32 nw = w / 2;
        const std::vector<u8>& s = out.back();
        std::vector<u8> d(size_t(nw) * nw * 4);
        for (u32 y = 0; y < nw; ++y)
            for (u32 x = 0; x < nw; ++x)
                for (u32 ch = 0; ch < 4; ++ch) {
                    u32 sum = 0;
                    for (u32 dy = 0; dy < 2; ++dy)
                        for (u32 dx = 0; dx < 2; ++dx) sum += s[(size_t(y * 2 + dy) * w + (x * 2 + dx)) * 4 + ch];
                    d[(size_t(y) * nw + x) * 4 + ch] = u8((sum + 2) / 4);
                }
        out.push_back(std::move(d));
        w = nw;
    }
    (void)alphaWeighted;
    return out;
}

void Upload(Renderer& r, u32 id, const Maps& m, const std::vector<u8>& data, TextureFormat fmt) {
    TextureUpload up;
    up.id = TextureId(id);
    up.format = fmt;
    up.width = m.w;
    up.height = m.h;
    up.mips = Mips(data, m.w);
    r.uploadTexture(std::move(up));
}

// ---- texture ids ---------------------------------------------------------------------------------------------------

enum Tex : u32 {
    tAsphaltC, tAsphaltN, tSidewalkC, tSidewalkN, tPromC, tPromN, tCycleC, tCycleN, tMarkC, tMarkYC,
    tFacadeC,                         // + style * kPalettes + palette
    tFacadeN = tFacadeC + 15,         // + style
    tFacadeMR = tFacadeN + 3,         // + style
    tShopC = tFacadeMR + 3,           // + variant
    tShopN = tShopC + 4,
    tShopMR = tShopN + 4,
    tBlankC = tShopMR + 4,            // + palette
    tBlankN = tBlankC + 5,
    tRoofTileC, tRoofTileN, tSlabC, tSlabN, tFoundC, tFoundN,
    tGutterC, tGutterN, tManholeC, tDrainC,
    tCount
};

const Vec3 kPalette[kPalettes] = {Vec3(0.91f, 0.89f, 0.82f), Vec3(0.90f, 0.82f, 0.58f), Vec3(0.80f, 0.55f, 0.43f), Vec3(0.67f, 0.72f, 0.77f), Vec3(0.75f, 0.65f, 0.53f)};

// Wall paint with soft patches and rain streaks, the base of every facade.
Vec3 Wall(Vec3 base, f32 x, f32 y, f32 widthM, u32 seed) {
    const f32 u = x / widthM, v = y / kStoreyH;
    const f32 patch = PFbm(u, v, 3, seed, 4);
    const f32 streak = PNoise(u * 16.0f, v * 2.0f, 16, 2, seed + 9);
    f32 k = 0.90f + 0.16f * patch - 0.06f * Step(0.55f, 0.9f, streak) * (1.0f - v * 0.3f);
    k *= 0.97f + 0.06f * noise::Rand01(int(u * 256.0f), int(v * 256.0f), seed + 3);
    return base * k;
}

// ---- facade styles: x metres across the bay (0..3.6), y metres up the storey (0..3) --------------------------------------

Texel Style0(f32 x, f32 y, Vec3 pal, u32 seed) { // modern apartments: sliding windows, glass balcony railings
    Texel t;
    t.c = Wall(pal, x, y, kBayW, seed);
    t.rough = 0.9f;
    if (y < 0.22f) { // the slab's edge
        t.c = pal * 0.80f * (0.95f + 0.1f * PFbm(x / kBayW, y, 4, seed + 1));
        t.h = 0.05f;
    }
    if (In(x, y, 0.25f, 0.22f, 3.35f, 1.12f)) { // balcony railing: pale glass between posts
        const f32 post = std::abs(std::fmod(x - 0.25f + 0.0001f, 0.775f));
        t.c = MixC(t.c, Vec3(0.62f, 0.72f, 0.74f), 0.55f);
        if (post < 0.04f) t.c = Vec3(0.28f, 0.29f, 0.30f);
        t.h = 0.08f;
        t.rough = 0.2f;
        t.metal = post < 0.04f ? 0.8f : 0.0f;
    }
    if (In(x, y, 0.25f, 1.08f, 3.35f, 1.16f)) { // handrail
        t.c = Vec3(0.20f, 0.21f, 0.22f);
        t.h = 0.1f;
        t.metal = 0.7f;
        t.rough = 0.35f;
    }
    if (In(x, y, 0.85f, 1.25f, 2.75f, 2.65f)) { // window: frame, glass, mullion
        const bool frame = !In(x, y, 0.92f, 1.32f, 2.68f, 2.58f) || std::abs(x - 1.8f) < 0.03f;
        if (frame) {
            t.c = Vec3(0.86f, 0.86f, 0.85f);
            t.h = -0.03f;
            t.rough = 0.4f;
            t.metal = 0.5f;
        } else {
            const f32 g = Sat((y - 1.32f) / 1.26f);
            Vec3 glass = MixC(Vec3(0.17f, 0.23f, 0.29f), Vec3(0.44f, 0.56f, 0.65f), g * 0.8f);
            const f32 refl = Step(0.0f, 0.12f, 0.5f - std::abs(x * 0.6f + y * 0.45f - 1.9f));
            glass = MixC(glass, Vec3(0.72f, 0.80f, 0.86f), refl * 0.35f);
            t.c = glass;
            t.h = -0.12f;
            t.rough = 0.08f;
        }
    }
    if (In(x, y, 0.80f, 1.17f, 2.80f, 1.25f)) { // sill
        t.c = pal * 1.05f;
        t.h = 0.05f;
    }
    return t;
}

Texel Style1(f32 x, f32 y, Vec3 pal, u32 seed) { // older stucco buildings: louvered shutters
    Texel t;
    t.c = Wall(pal * 0.98f, x, y, kBayW, seed);
    t.rough = 0.92f;
    if (y < 0.10f) {
        t.c = pal * 0.9f;
        t.h = 0.04f;
    }
    static const Vec3 shutter[3] = {Vec3(0.18f, 0.32f, 0.22f), Vec3(0.42f, 0.28f, 0.17f), Vec3(0.90f, 0.90f, 0.86f)};
    const Vec3 sh = shutter[seed % 3];
    for (int k = 0; k < 2; ++k) {
        const f32 x0 = 0.42f + 1.8f * f32(k), x1 = x0 + 0.96f;
        if (In(x, y, x0 - 0.08f, 0.76f, x1 + 0.08f, 2.30f)) {
            const bool inner = In(x, y, x0, 0.88f, x1, 2.18f);
            if (!inner) {
                t.c = pal * 1.06f; // surround
                t.h = 0.04f;
            } else {
                const f32 slat = std::fmod(y - 0.88f, 0.075f);
                const f32 s = Step(0.0f, 0.02f, slat) * (1.0f - Step(0.05f, 0.075f, slat));
                t.c = sh * (0.78f + 0.3f * s);
                t.h = -0.05f + 0.03f * s;
                t.rough = 0.6f;
            }
        }
    }
    if (In(x, y, 0.30f, 0.70f, 3.30f, 0.78f)) { // ledge below the windows
        t.c = pal * 1.08f;
        t.h = 0.06f;
    }
    return t;
}

Texel Style2(f32 x, f32 y, Vec3 pal, u32 seed) { // tiled facades with ribbon windows and air conditioners
    Texel t;
    const f32 cell = 0.10f;
    const int ti = int(std::floor(x / cell)), tj = int(std::floor(y / cell));
    const f32 tv = noise::Rand01(ti, tj, seed + 5);
    t.c = Wall(pal * (0.88f + 0.12f * tv), x, y, kBayW, seed);
    const f32 gx = std::fmod(x, cell), gy = std::fmod(y, cell);
    if (gx < 0.008f || gy < 0.008f) t.c *= 0.78f;
    t.rough = 0.45f;
    t.h = (gx < 0.008f || gy < 0.008f) ? -0.004f : 0.0f;
    if (In(x, y, 0.15f, 0.95f, 3.45f, 2.35f)) { // ribbon window with mullions
        const bool mull = std::abs(std::fmod(x - 0.15f, 1.1f)) < 0.04f || In(x, y, 0.15f, 0.95f, 3.45f, 1.02f) || In(x, y, 0.15f, 2.28f, 3.45f, 2.35f);
        if (mull) {
            t.c = Vec3(0.30f, 0.30f, 0.31f);
            t.metal = 0.7f;
            t.rough = 0.35f;
            t.h = -0.02f;
        } else {
            const f32 g = Sat((y - 1.02f) / 1.26f);
            t.c = MixC(Vec3(0.16f, 0.22f, 0.27f), Vec3(0.42f, 0.54f, 0.62f), g * 0.7f);
            t.h = -0.10f;
            t.rough = 0.08f;
            t.metal = 0.0f;
        }
    }
    if (In(x, y, 0.35f, 0.25f, 1.15f, 0.80f)) { // air conditioner
        const bool vent = In(x, y, 0.45f, 0.35f, 1.05f, 0.70f) && std::fmod(y - 0.35f, 0.06f) < 0.03f;
        t.c = vent ? Vec3(0.30f, 0.30f, 0.32f) : Vec3(0.80f, 0.80f, 0.78f);
        t.h = 0.12f;
        t.rough = 0.5f;
    }
    return t;
}

Texel Shop(u32 variant, f32 x, f32 y, u32 seed) { // ground floor: 3.6 m wide, 3.6 m high
    Texel t;
    const Vec3 wall = Vec3(0.78f, 0.74f, 0.67f) * (0.88f + 0.2f * PFbm(x / kBayW, y / kShopH, 3, seed, 4));
    t.c = wall;
    t.rough = 0.88f;
    if (y < 0.9f) t.c *= 0.86f; // a dirtier base
    static const Vec3 sign[4] = {Vec3(0.75f, 0.15f, 0.12f), Vec3(0.10f, 0.32f, 0.62f), Vec3(0.88f, 0.70f, 0.12f), Vec3(0.14f, 0.45f, 0.26f)};
    const Vec3 sg = sign[(seed / 3) % 4];
    switch (variant) {
    case 0: // rolling shutter
        if (In(x, y, 0.25f, 0.0f, 3.35f, 2.75f)) {
            const f32 slat = std::fmod(y, 0.07f);
            const f32 s = Step(0.0f, 0.02f, slat);
            t.c = Vec3(0.55f, 0.56f, 0.57f) * (0.65f + 0.3f * s) * (0.9f + 0.2f * PFbm(x / kBayW, y / kShopH, 5, seed + 2));
            t.h = -0.02f + 0.02f * s;
            t.metal = 0.7f;
            t.rough = 0.5f;
        }
        break;
    case 1: // glass storefront and door
        if (In(x, y, 0.2f, 0.25f, 3.4f, 2.75f)) {
            const bool mull = std::abs(std::fmod(x - 0.2f, 1.07f)) < 0.04f || y < 0.32f || y > 2.68f;
            if (mull) {
                t.c = Vec3(0.20f, 0.20f, 0.21f);
                t.metal = 0.7f;
                t.rough = 0.4f;
                t.h = 0.02f;
            } else {
                const f32 g = Sat(y / 2.7f);
                t.c = MixC(Vec3(0.20f, 0.24f, 0.25f), Vec3(0.50f, 0.58f, 0.62f), g * 0.6f);
                if (In(x, y, 2.35f, 0.32f, 3.3f, 2.5f)) t.c *= 0.55f; // the door, darker
                t.h = -0.08f;
                t.rough = 0.08f;
            }
        }
        break;
    case 2: // residential entrance: stone base, portal, brown door
        if (y < 1.1f) {
            const f32 st = noise::Rand01(int(x / 0.4f), int(y / 0.25f), seed + 11);
            t.c = Vec3(0.45f, 0.43f, 0.40f) * (0.8f + 0.3f * st);
            t.h = 0.02f * st;
        }
        if (In(x, y, 1.25f, 0.0f, 2.35f, 2.7f)) {
            const bool inner = In(x, y, 1.35f, 0.0f, 2.25f, 2.6f);
            t.c = inner ? (y > 1.4f && In(x, y, 1.5f, 1.5f, 2.1f, 2.45f) ? Vec3(0.22f, 0.30f, 0.34f) : Vec3(0.30f, 0.20f, 0.13f)) : Vec3(0.70f, 0.68f, 0.64f);
            t.h = inner ? -0.08f : 0.03f;
            t.rough = inner ? 0.3f : 0.85f;
        }
        break;
    default: // garage door
        if (In(x, y, 0.5f, 0.0f, 3.1f, 2.5f)) {
            const f32 panel = std::fmod(y, 0.62f);
            t.c = Vec3(0.78f, 0.78f, 0.76f) * (panel < 0.02f ? 0.55f : 1.0f);
            t.h = panel < 0.02f ? -0.015f : 0.0f;
            t.metal = 0.5f;
            t.rough = 0.45f;
        }
        break;
    }
    if (variant < 2 && y > 2.78f && y < 3.45f && x > 0.2f && x < 3.4f) { // sign band
        t.c = sg * (0.9f + 0.1f * PFbm(x / kBayW, y / kShopH, 6, seed + 4));
        t.h = 0.05f;
        t.rough = 0.5f;
        if (std::fmod(x, 0.42f) < 0.22f && y > 3.0f && y < 3.22f) t.c = Vec3(0.95f); // illegible letters: invented shop names only
    }
    if (y > 3.5f) t.c *= 0.9f;
    return t;
}

Texel Blank(f32 x, f32 y, Vec3 pal, u32 seed) {
    Texel t;
    t.c = Wall(pal * 0.96f, x, y, kBayW, seed + 40);
    const f32 crack = PNoise(x / kBayW * 24.0f, y / kStoreyH * 3.0f, 24, 3, seed + 12);
    t.c *= 1.0f - 0.06f * Step(0.7f, 0.95f, crack);
    t.rough = 0.92f;
    return t;
}

// ---- roads ---------------------------------------------------------------------------------------------------------

// Voronoi cells that repeat every `period` cells: the distance to the nearest and second nearest feature point and the nearest cell's id.
struct Cell {
    f32 d1, d2;
    int ix, iy;
};
Cell Voronoi(f32 x, f32 y, int period, u32 seed) {
    const int cx = int(std::floor(x)), cy = int(std::floor(y));
    Cell c{9.0f, 9.0f, 0, 0};
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int nx = cx + dx, ny = cy + dy;
            const int wx = ((nx % period) + period) % period, wy = ((ny % period) + period) % period;
            const f32 fx = f32(nx) + 0.15f + 0.7f * noise::Rand01(wx, wy, seed), fy = f32(ny) + 0.15f + 0.7f * noise::Rand01(wx, wy, seed + 1);
            const f32 d = std::sqrt((fx - x) * (fx - x) + (fy - y) * (fy - y));
            if (d < c.d1) {
                c.d2 = c.d1;
                c.d1 = d;
                c.ix = wx;
                c.iy = wy;
            } else if (d < c.d2) {
                c.d2 = d;
            }
        }
    return c;
}

Texel Asphalt(f32 u, f32 v) { // 5 m: worn road with patched repairs, seams, cracks and oil
    Texel t;
    const f32 grain = noise::Rand01(int(u * 512.0f), int(v * 512.0f), 71);
    const f32 big = PFbm(u, v, 4, 72, 5);
    f32 s = 0.15f + 0.055f * big + 0.04f * grain;
    // Repaired patches: rectangles (about 1.2 x 2 m) a little darker or lighter than the road, with a seam of tar around them.
    const f32 gx = u * 4.0f, gy = v * 2.5f;
    const int ci = int(std::floor(gx)), cj = int(std::floor(gy));
    const f32 patchR = noise::Rand01(ci, cj, 76);
    f32 seam = 0.0f;
    if (patchR < 0.20f) {
        const f32 fx = gx - std::floor(gx), fy = gy - std::floor(gy);
        const f32 edge = std::min(std::min(fx, 1.0f - fx) * 1.25f, std::min(fy, 1.0f - fy) * 2.0f); // metres to the patch border
        s *= patchR < 0.10f ? 0.78f : 1.22f;
        seam = 1.0f - Step(0.0f, 0.035f, std::abs(edge - 0.10f) + 0.0f);
    }
    // Cracks: the ridges of a coarse cell pattern, only where a second noise allows them.
    const Cell cr = Voronoi(u * 5.0f, v * 5.0f, 5, 74);
    const f32 ridge = 1.0f - Step(0.0f, 0.028f, cr.d2 - cr.d1);
    const f32 crack = ridge * Step(0.60f, 0.72f, PFbm(u, v, 3, 75, 3));
    // Oil and rubber: soft dark blobs.
    const f32 oil = Step(0.64f, 0.78f, PFbm(u, v, 6, 77, 3));
    t.c = Vec3(s, s, s * 1.03f);
    if (noise::Rand01(int(u * 512.0f), int(v * 512.0f), 73) > 0.975f) t.c += Vec3(0.10f); // aggregate
    t.c *= 1.0f - 0.55f * Sat(seam) - 0.60f * crack - 0.35f * oil;
    t.rough = 0.90f - 0.12f * big - 0.45f * oil;
    t.h = 0.004f * (grain - 0.5f) - 0.012f * crack - 0.006f * seam;
    return t;
}

Texel Sidewalk(f32 u, f32 v) { // 2 m: small limestone setts (calcada) with dark joints, a few basalt stones, soot and wear
    Texel t;
    constexpr int kCells = 30;
    const Cell c = Voronoi(u * f32(kCells), v * f32(kCells), kCells, 81);
    const f32 pick = noise::Rand01(c.ix, c.iy, 83);
    f32 tone = 0.60f + 0.20f * noise::Rand01(c.ix, c.iy, 84);
    if (pick > 0.93f) tone = 0.17f + 0.08f * noise::Rand01(c.ix, c.iy, 85); // basalt
    else if (pick < 0.07f) tone = 0.42f + 0.1f * noise::Rand01(c.ix, c.iy, 86);
    const f32 mott = PFbm(u, v, 4, 82, 5);
    const f32 soot = Step(0.55f, 0.85f, PFbm(u, v, 5, 87, 4));
    f32 k = tone * (0.88f + 0.22f * mott) * (1.0f - 0.22f * soot);
    const f32 joint = 1.0f - Step(0.04f, 0.16f, c.d2 - c.d1); // 1 on the joint between two stones
    k *= 1.0f - 0.60f * joint;
    t.c = Vec3(k, k * 0.98f, k * 0.93f);
    t.rough = 0.86f - 0.1f * (1.0f - joint) * Step(0.4f, 0.8f, tone);
    t.h = -0.007f * joint + 0.003f * (1.0f - Step(0.0f, 0.5f, c.d1));
    return t;
}

Texel Gutter(f32 u, f32 v) { // 1 m: the concrete strip between the asphalt and the kerb, with dirt and the odd leaf
    Texel t;
    const f32 mott = PFbm(u, v, 4, 141, 5);
    const f32 dirt = Step(0.50f, 0.85f, PFbm(u * 2.0f, v, 4, 142, 3));
    f32 s = 0.46f + 0.10f * mott;
    s *= 1.0f - 0.40f * dirt;
    if (std::fmod(v * 2.0f, 1.0f) < 0.015f) s *= 0.6f; // a joint every half metre
    t.c = Vec3(s, s * 0.97f, s * 0.92f);
    t.rough = 0.88f;
    t.h = std::fmod(v * 2.0f, 1.0f) < 0.015f ? -0.008f : 0.0f;
    return t;
}

Texel Manhole(f32 u, f32 v) { // 0.75 m round lid with a ribbed pattern; alpha cuts the circle out of a square
    Texel t;
    const f32 x = (u - 0.5f) * 2.0f, y = (v - 0.5f) * 2.0f, r = std::sqrt(x * x + y * y);
    t.alpha = r < 0.96f ? 1.0f : 0.0f;
    const f32 rim = Step(0.80f, 0.86f, r);
    const f32 rib = (std::fmod((x + y) * 5.0f + 100.0f, 1.0f) < 0.45f) ? 1.0f : 0.8f;
    const f32 s = (0.16f + 0.05f * noise::Rand01(int(u * 64.0f), int(v * 64.0f), 151)) * (rim > 0.5f ? 1.35f : rib);
    t.c = Vec3(s * 1.0f, s * 0.97f, s * 0.92f);
    t.rough = 0.55f;
    return t;
}

Texel Drain(f32 u, f32 v) { // a storm drain grate, 1 m by 0.4 m (u along the kerb)
    Texel t;
    const f32 x = u, y = v;
    const bool frame = x < 0.05f || x > 0.95f || y < 0.10f || y > 0.90f;
    const bool bar = std::fmod(x * 14.0f, 1.0f) < 0.45f;
    t.alpha = 1.0f;
    const f32 s = frame ? 0.30f : (bar ? 0.26f : 0.03f);
    t.c = Vec3(s, s, s * 1.02f);
    t.rough = 0.5f;
    return t;
}

Texel Promenade(f32 u, f32 v) { // 4 m across, 4 m along: black and white waves, as on the real promenade
    Texel t;
    const f32 across = u * 4.0f, along = v * 4.0f;
    const f32 phase = 6.2831853f * (across / 1.0f) + 1.15f * std::sin(6.2831853f * along / 2.0f);
    const f32 wave = std::sin(phase);
    const bool light = wave > 0.0f;
    const f32 stone = 0.88f + 0.12f * noise::Rand01(int(u * 128.0f), int(v * 128.0f), 91);
    t.c = light ? Vec3(0.80f, 0.78f, 0.72f) * stone : Vec3(0.09f, 0.09f, 0.10f) * stone;
    // small cobbles: 8 cm joints
    const f32 cu = std::fmod(across / 0.08f, 1.0f), cv = std::fmod(along / 0.08f, 1.0f);
    if (cu < 0.1f || cv < 0.1f) {
        t.c *= 0.78f;
        t.h = -0.004f;
    }
    t.rough = 0.7f;
    return t;
}

Texel Cycle(f32 u, f32 v) { // 4 m, red asphalt
    Texel t = Asphalt(u, v);
    t.c = Vec3(0.50f, 0.23f, 0.18f) * (0.78f + 0.95f * t.c.r);
    return t;
}

Texel Marking(f32, f32 v, bool yellow, u32 seed) { // v in 0..1: paint for v < 0.38, bare asphalt elsewhere
    Texel t;
    t.c = yellow ? Vec3(0.86f, 0.70f, 0.10f) : Vec3(0.88f, 0.88f, 0.85f);
    const f32 wear = noise::Rand01(int(v * 64.0f), int(v * 64.0f * 7.0f), seed);
    t.alpha = v < 0.38f ? (wear > 0.93f ? 0.3f : 1.0f) : 0.0f;
    t.rough = 0.7f;
    return t;
}

Texel RoofTile(f32 u, f32 v) { // 1.2 m
    Texel t;
    const f32 col = std::fmod(u * 6.0f, 1.0f), row = std::fmod(v * 12.0f, 1.0f);
    const f32 shade = 0.80f + 0.30f * noise::Rand01(int(u * 6.0f), int(v * 12.0f), 101);
    const f32 curve = std::sin(col * 3.14159265f);
    t.c = Vec3(0.66f, 0.30f, 0.17f) * shade * (0.72f + 0.35f * curve) * (0.85f + 0.15f * row);
    t.c = MixC(t.c, Vec3(0.30f, 0.34f, 0.18f), 0.25f * Step(0.6f, 0.9f, PFbm(u, v, 4, 102)));
    t.h = 0.03f * curve + 0.02f * row;
    t.rough = 0.8f;
    return t;
}

Texel Slab(f32 u, f32 v) { // 4 m roof slab: concrete with tar patches
    Texel t;
    const f32 m = PFbm(u, v, 3, 111, 5);
    f32 s = 0.38f + 0.07f * m;
    t.c = Vec3(s, s, s * 0.97f);
    const f32 tar = Step(0.62f, 0.7f, PFbm(u, v, 3, 112, 3));
    t.c = MixC(t.c, Vec3(0.12f, 0.12f, 0.13f), tar * 0.32f);
    t.rough = 0.85f;
    t.h = 0.004f * noise::Rand01(int(u * 256.0f), int(v * 256.0f), 113);
    return t;
}

Texel Foundation(f32 u, f32 v) { // 2 m: formwork concrete
    Texel t;
    const f32 m = PFbm(u, v, 4, 121, 4);
    const f32 s = 0.36f + 0.1f * m;
    t.c = Vec3(s, s, s * 0.98f);
    if (std::fmod(v * 4.0f, 1.0f) < 0.02f) {
        t.c *= 0.7f;
        t.h = -0.01f;
    }
    t.rough = 0.9f;
    return t;
}

// ---- props: simple solids ----------------------------------------------------------------------------------------

struct Solid {
    MeshData m;
    void box(Vec3 c, Vec3 h) {
        for (int a = 0; a < 3; ++a)
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                const int b = (a + 1) % 3, d = (a + 2) % 3;
                Vec3 nr(0.0f);
                nr[a] = f32(sgn);
                const f32 sb[4] = {-1, 1, 1, -1}, sd[4] = {-1, -1, 1, 1};
                u32 q[4];
                for (int k = 0; k < 4; ++k) {
                    Vec3 p = c;
                    p[a] += f32(sgn) * h[a];
                    p[b] += sb[k] * h[b];
                    p[d] += sd[k] * h[d];
                    q[k] = m.vert(p, nr, Vec2(sb[k] * 0.5f + 0.5f, sd[k] * 0.5f + 0.5f));
                }
                m.quad(q[0], q[1], q[2], q[3], nr);
            }
    }
    void cylinder(Vec3 base, f32 r, f32 h, int seg = 10, bool cap = true) {
        std::vector<u32> lo, hi;
        for (int i = 0; i <= seg; ++i) {
            const f32 a = 6.2831853f * f32(i) / f32(seg);
            const Vec3 n(std::cos(a), 0, std::sin(a));
            lo.push_back(m.vert(base + n * r, n, Vec2(f32(i) / f32(seg), 0)));
            hi.push_back(m.vert(base + n * r + Vec3(0, h, 0), n, Vec2(f32(i) / f32(seg), 1)));
        }
        for (int i = 0; i < seg; ++i) {
            const Vec3 mid = (m.p[lo[size_t(i)]] + m.p[lo[size_t(i) + 1]]) * 0.5f - base;
            m.quad(lo[size_t(i)], lo[size_t(i) + 1], hi[size_t(i) + 1], hi[size_t(i)], Vec3(mid.x, 0, mid.z));
        }
        if (cap) {
            const u32 c = m.vert(base + Vec3(0, h, 0), Vec3(0, 1, 0), Vec2(0.5f));
            for (int i = 0; i < seg; ++i) {
                const f32 a0 = 6.2831853f * f32(i) / f32(seg), a1 = 6.2831853f * f32(i + 1) / f32(seg);
                const u32 p0 = m.vert(base + Vec3(std::cos(a0) * r, h, std::sin(a0) * r), Vec3(0, 1, 0), Vec2(0.5f)), p1 = m.vert(base + Vec3(std::cos(a1) * r, h, std::sin(a1) * r), Vec3(0, 1, 0), Vec2(0.5f));
                m.tri(c, p0, p1, Vec3(0, 1, 0));
            }
        }
    }
    void dome(Vec3 centre, f32 r, f32 h, int seg = 12, int rings = 3) { // umbrella canopy: a shallow cone-dome, open below
        std::vector<std::vector<u32>> rowsV;
        for (int rg = 0; rg <= rings; ++rg) {
            const f32 t = f32(rg) / f32(rings);
            const f32 rad = r * (1.0f - t), y = h * (1.0f - (1.0f - t) * (1.0f - t));
            std::vector<u32> row;
            for (int i = 0; i <= seg; ++i) {
                const f32 a = 6.2831853f * f32(i) / f32(seg);
                const Vec3 p = centre + Vec3(std::cos(a) * rad, y, std::sin(a) * rad);
                const Vec3 n = glm::normalize(Vec3(std::cos(a) * h, r * 0.5f * (1.0f + t), std::sin(a) * h));
                row.push_back(m.vert(p, n, Vec2(f32(i) / f32(seg), t)));
            }
            rowsV.push_back(row);
        }
        for (int rg = 0; rg < rings; ++rg)
            for (int i = 0; i < seg; ++i) {
                const Vec3 n = m.n[rowsV[size_t(rg)][size_t(i)]];
                m.tri(rowsV[size_t(rg)][size_t(i)], rowsV[size_t(rg)][size_t(i) + 1], rowsV[size_t(rg) + 1][size_t(i) + 1], n);
                m.tri(rowsV[size_t(rg)][size_t(i)], rowsV[size_t(rg) + 1][size_t(i) + 1], rowsV[size_t(rg) + 1][size_t(i)], n);
            }
    }
};

void UploadMesh(Renderer& r, Prop p, const Solid& s) {
    const MeshIndexRange whole{0, u32(s.m.idx.size())};
    if (!r.uploadMeshAsset(PropMesh(p), MeshGeometry{s.m.p, s.m.n, s.m.uv, s.m.t, s.m.idx}, std::span(&whole, 1))) FOH_ERROR("city: prop mesh {} was rejected ({} vertices, {} indices)", u32(p), s.m.p.size(), s.m.idx.size());
}

// ---- cars: lofted bodies (a ring of six points per station along the length), dark glass and trim, wheels ----------------------

struct Station {
    f32 x, w, yb, yt; // along the car, half width, bottom, top
};

// Smooth-shaded loft through the stations (+x is the front); both ends are capped.
void Loft(Solid& s, const std::vector<Station>& st, f32 tumble = 0.88f) {
    const size_t n = st.size();
    std::vector<std::array<Vec3, 6>> rings(n);
    for (size_t i = 0; i < n; ++i) {
        const Station& q = st[i];
        const f32 ym = q.yb + (q.yt - q.yb) * 0.62f;
        rings[i] = {Vec3(q.x, q.yb, -q.w), Vec3(q.x, q.yb, q.w), Vec3(q.x, ym, q.w), Vec3(q.x, q.yt, q.w * tumble), Vec3(q.x, q.yt, -q.w * tumble), Vec3(q.x, ym, -q.w)};
    }
    std::vector<std::array<u32, 6>> ids(n);
    for (size_t i = 0; i < n; ++i) {
        Vec3 centre(0.0f);
        for (const Vec3& p : rings[i]) centre += p / 6.0f;
        for (int j = 0; j < 6; ++j) {
            const Vec3 tx = rings[std::min(i + 1, n - 1)][size_t(j)] - rings[i > 0 ? i - 1 : 0][size_t(j)];
            const Vec3 tr = rings[i][size_t((j + 1) % 6)] - rings[i][size_t((j + 5) % 6)];
            Vec3 nr = glm::cross(tr, tx);
            nr = glm::length(nr) > 1e-6f ? glm::normalize(nr) : Vec3(0, 1, 0);
            if (glm::dot(nr, rings[i][size_t(j)] - centre) < 0.0f) nr = -nr;
            ids[i][size_t(j)] = s.m.vert(rings[i][size_t(j)], nr, Vec2(rings[i][size_t(j)].x * 0.4f, f32(j) * 0.25f));
        }
    }
    for (size_t i = 0; i + 1 < n; ++i)
        for (int j = 0; j < 6; ++j) {
            const int k = (j + 1) % 6;
            const Vec3 mid = (rings[i][size_t(j)] + rings[i][size_t(k)] + rings[i + 1][size_t(j)] + rings[i + 1][size_t(k)]) * 0.25f;
            const Vec3 centre = (rings[i][0] + rings[i][3] + rings[i + 1][0] + rings[i + 1][3]) * 0.25f;
            s.m.quad(ids[i][size_t(j)], ids[i][size_t(k)], ids[i + 1][size_t(k)], ids[i + 1][size_t(j)], mid - centre);
        }
    for (int end = 0; end < 2; ++end) {
        const size_t i = end ? n - 1 : 0;
        const Vec3 nr(end ? 1.0f : -1.0f, 0.0f, 0.0f);
        Vec3 centre(0.0f);
        for (const Vec3& p : rings[i]) centre += p / 6.0f;
        const u32 c = s.m.vert(centre, nr, Vec2(0.5f));
        std::array<u32, 6> cap;
        for (int j = 0; j < 6; ++j) cap[size_t(j)] = s.m.vert(rings[i][size_t(j)], nr, Vec2(0.5f));
        for (int j = 0; j < 6; ++j) s.m.tri(c, cap[size_t(j)], cap[size_t((j + 1) % 6)], nr);
    }
}

// A wheel with its axis along z.
void WheelZ(Solid& s, Vec3 centre, f32 r, f32 width, int seg = 14) {
    std::vector<u32> a, b;
    for (int i = 0; i <= seg; ++i) {
        const f32 ang = 6.2831853f * f32(i) / f32(seg);
        const Vec3 n(std::cos(ang), std::sin(ang), 0.0f);
        a.push_back(s.m.vert(centre + n * r + Vec3(0, 0, -width * 0.5f), n, Vec2(f32(i) / f32(seg), 0)));
        b.push_back(s.m.vert(centre + n * r + Vec3(0, 0, width * 0.5f), n, Vec2(f32(i) / f32(seg), 1)));
    }
    for (int i = 0; i < seg; ++i) {
        const Vec3 mid((std::cos(6.2831853f * (f32(i) + 0.5f) / f32(seg))), std::sin(6.2831853f * (f32(i) + 0.5f) / f32(seg)), 0.0f);
        s.m.quad(a[size_t(i)], a[size_t(i) + 1], b[size_t(i) + 1], b[size_t(i)], mid);
    }
    for (int side = -1; side <= 1; side += 2) {
        const Vec3 nr(0, 0, f32(side));
        const u32 c = s.m.vert(centre + Vec3(0, 0, side * width * 0.5f), nr, Vec2(0.5f));
        std::vector<u32> rim;
        for (int i = 0; i <= seg; ++i) {
            const f32 ang = 6.2831853f * f32(i) / f32(seg);
            rim.push_back(s.m.vert(centre + Vec3(std::cos(ang) * r, std::sin(ang) * r, side * width * 0.5f), nr, Vec2(0.5f)));
        }
        for (int i = 0; i < seg; ++i) s.m.tri(c, rim[size_t(i)], rim[size_t(i) + 1], nr);
    }
}

struct CarSpec {
    std::vector<Station> body, cabin;
    f32 wheelbase, wheelR, track, halfLen, belt;
};

CarSpec CarVariant(int v) {
    CarSpec c;
    if (v == 0) { // hatchback
        c.body = {{-1.95f, 0.70f, 0.40f, 0.80f}, {-1.88f, 0.80f, 0.30f, 0.92f}, {-1.20f, 0.84f, 0.28f, 0.95f}, {-0.70f, 0.85f, 0.28f, 0.92f}, {0.90f, 0.85f, 0.28f, 0.90f}, {1.55f, 0.84f, 0.28f, 0.82f}, {1.88f, 0.78f, 0.30f, 0.68f}, {1.95f, 0.66f, 0.38f, 0.60f}};
        c.cabin = {{-1.30f, 0.72f, 0.92f, 0.98f}, {-1.12f, 0.73f, 0.92f, 1.40f}, {-0.95f, 0.70f, 0.92f, 1.47f}, {0.30f, 0.70f, 0.92f, 1.47f}, {0.58f, 0.73f, 0.92f, 1.40f}, {1.00f, 0.74f, 0.92f, 0.95f}};
        c.wheelbase = 2.45f; c.wheelR = 0.31f; c.track = 0.78f; c.halfLen = 1.95f; c.belt = 0.92f;
    } else if (v == 1) { // sedan
        c.body = {{-2.25f, 0.72f, 0.42f, 0.78f}, {-2.18f, 0.82f, 0.30f, 0.90f}, {-1.70f, 0.86f, 0.28f, 0.96f}, {-1.00f, 0.87f, 0.28f, 0.94f}, {1.00f, 0.87f, 0.28f, 0.92f}, {1.75f, 0.86f, 0.28f, 0.84f}, {2.18f, 0.80f, 0.30f, 0.70f}, {2.25f, 0.68f, 0.38f, 0.62f}};
        c.cabin = {{-1.35f, 0.74f, 0.93f, 0.97f}, {-0.95f, 0.74f, 0.93f, 1.38f}, {-0.55f, 0.70f, 0.93f, 1.45f}, {0.45f, 0.70f, 0.93f, 1.45f}, {0.85f, 0.74f, 0.93f, 1.38f}, {1.20f, 0.75f, 0.93f, 0.95f}};
        c.wheelbase = 2.70f; c.wheelR = 0.32f; c.track = 0.80f; c.halfLen = 2.25f; c.belt = 0.93f;
    } else { // SUV
        c.body = {{-2.20f, 0.76f, 0.45f, 0.95f}, {-2.12f, 0.86f, 0.36f, 1.05f}, {-1.60f, 0.88f, 0.34f, 1.08f}, {-0.50f, 0.89f, 0.34f, 1.08f}, {1.00f, 0.89f, 0.34f, 1.04f}, {1.80f, 0.87f, 0.34f, 0.98f}, {2.15f, 0.82f, 0.36f, 0.82f}, {2.22f, 0.70f, 0.44f, 0.74f}};
        c.cabin = {{-1.95f, 0.78f, 1.06f, 1.12f}, {-1.90f, 0.78f, 1.06f, 1.62f}, {-1.70f, 0.74f, 1.06f, 1.72f}, {0.50f, 0.74f, 1.06f, 1.72f}, {0.95f, 0.78f, 1.06f, 1.55f}, {1.30f, 0.80f, 1.06f, 1.08f}};
        c.wheelbase = 2.75f; c.wheelR = 0.38f; c.track = 0.82f; c.halfLen = 2.22f; c.belt = 1.06f;
    }
    return c;
}

void BuildCar(Renderer& r, int v, Prop bodyProp, Prop glassProp, Prop wheelProp) {
    const CarSpec c = CarVariant(v);
    Solid body;
    Loft(body, c.body);
    // The roof: a slim paint plate over the cabin (the glass is the loft below it).
    const f32 roofX0 = c.cabin[2].x - 0.02f, roofX1 = c.cabin[3].x + 0.02f, roofY = c.cabin[2].yt;
    body.box(Vec3((roofX0 + roofX1) * 0.5f, roofY - 0.02f, 0.0f), Vec3((roofX1 - roofX0) * 0.5f, 0.045f, c.cabin[2].w * 0.88f));
    Solid glass;
    Loft(glass, c.cabin, 0.9f);
    // Pillars in paint colour are left out on purpose: the loft reads as one dark greenhouse; the trim goes in the dark mesh.
    glass.box(Vec3(c.halfLen - 0.04f, 0.50f, 0.0f), Vec3(0.07f, 0.12f, 0.62f));  // front bumper
    glass.box(Vec3(-c.halfLen + 0.04f, 0.46f, 0.0f), Vec3(0.07f, 0.12f, 0.62f)); // rear bumper
    glass.box(Vec3(c.halfLen - 0.02f, 0.72f, 0.0f), Vec3(0.04f, 0.07f, 0.42f));  // grille
    for (f32 sz : {-1.0f, 1.0f}) {
        glass.box(Vec3(c.halfLen - 0.10f, 0.78f, sz * 0.60f), Vec3(0.06f, 0.05f, 0.14f));  // headlight
        glass.box(Vec3(-c.halfLen + 0.07f, 0.80f, sz * 0.62f), Vec3(0.05f, 0.05f, 0.14f)); // tail light
    }
    UploadMesh(r, bodyProp, body);
    UploadMesh(r, glassProp, glass);
    Solid wheels;
    for (f32 sx : {-0.5f, 0.5f})
        for (f32 sz : {-1.0f, 1.0f}) WheelZ(wheels, Vec3(sx * c.wheelbase, c.wheelR, sz * c.track), c.wheelR, 0.22f);
    UploadMesh(r, wheelProp, wheels);
}

} // namespace

void UploadCityContent(Renderer& r) {
    // ---- textures ----
    auto roadTex = [&](u32 colorId, u32 normalId, u32 size, f32 metres, auto&& f) {
        const Maps m = Bake(size, metres, metres, f);
        Upload(r, colorId, m, m.albedo, TextureFormat::Rgba8Srgb);
        if (normalId != ~0u) Upload(r, normalId, m, m.normal, TextureFormat::Rgba8Unorm);
    };
    roadTex(tAsphaltC, tAsphaltN, 512, 5.0f, Asphalt);
    roadTex(tSidewalkC, tSidewalkN, 512, 2.0f, Sidewalk);
    roadTex(tPromC, tPromN, 512, 4.0f, Promenade);
    roadTex(tCycleC, tCycleN, 256, 4.0f, Cycle);
    {
        const Maps m = Bake(64, 1.0f, 9.0f, [](f32 u, f32 v) { return Marking(u, v, false, 131); }, false, false);
        Upload(r, tMarkC, m, m.albedo, TextureFormat::Rgba8Srgb);
        const Maps y = Bake(64, 1.0f, 9.0f, [](f32 u, f32 v) { return Marking(u, v, true, 132); }, false, false);
        Upload(r, tMarkYC, y, y.albedo, TextureFormat::Rgba8Srgb);
    }
    for (u32 style = 0; style < kFacadeStyles; ++style) {
        for (u32 pal = 0; pal < kPalettes; ++pal) {
            const u32 seed = 200 + style * 17 + pal * 5;
            const Maps m = Bake(256, kBayW, kStoreyH, [&](f32 u, f32 v) {
                const f32 x = u * kBayW, y = v * kStoreyH;
                return style == 0 ? Style0(x, y, kPalette[pal], seed) : (style == 1 ? Style1(x, y, kPalette[pal], seed) : Style2(x, y, kPalette[pal], seed));
            }, pal == 0, pal == 0);
            Upload(r, tFacadeC + style * kPalettes + pal, m, m.albedo, TextureFormat::Rgba8Srgb);
            if (pal == 0) {
                Upload(r, tFacadeN + style, m, m.normal, TextureFormat::Rgba8Unorm);
                Upload(r, tFacadeMR + style, m, m.mr, TextureFormat::Rgba8Unorm);
            }
        }
    }
    for (u32 v = 0; v < kShopVariants; ++v) {
        const Maps m = Bake(256, kBayW, kShopH, [&](f32 u, f32 w) { return Shop(v, u * kBayW, w * kShopH, 300 + v); });
        Upload(r, tShopC + v, m, m.albedo, TextureFormat::Rgba8Srgb);
        Upload(r, tShopN + v, m, m.normal, TextureFormat::Rgba8Unorm);
        Upload(r, tShopMR + v, m, m.mr, TextureFormat::Rgba8Unorm);
    }
    for (u32 pal = 0; pal < kPalettes; ++pal) {
        const Maps m = Bake(256, kBayW, kStoreyH, [&](f32 u, f32 v) { return Blank(u * kBayW, v * kStoreyH, kPalette[pal], 400 + pal); }, false, false);
        Upload(r, tBlankC + pal, m, m.albedo, TextureFormat::Rgba8Srgb);
    }
    roadTex(tRoofTileC, tRoofTileN, 256, 1.2f, RoofTile);
    roadTex(tSlabC, tSlabN, 256, 4.0f, Slab);
    roadTex(tFoundC, tFoundN, 256, 2.0f, Foundation);
    roadTex(tGutterC, tGutterN, 128, 1.0f, Gutter);
    {
        const Maps m = Bake(128, 0.75f, 0.75f, Manhole, false, false);
        Upload(r, tManholeC, m, m.albedo, TextureFormat::Rgba8Srgb);
        const Maps d = Bake(128, 1.0f, 0.4f, Drain, false, false);
        Upload(r, tDrainC, d, d.albedo, TextureFormat::Rgba8Srgb);
    }

    // ---- materials ----
    auto mat = [&](u32 slot, u32 color, u32 normal, f32 rough, f32 metal = 0.0f, u32 mr = ~0u) {
        MaterialParams p;
        if (color != ~0u) p.baseColorTexture = TextureId(color);
        if (normal != ~0u) p.normalTexture = TextureId(normal);
        if (mr != ~0u) {
            p.metalRoughTexture = TextureId(mr);
            p.roughness = 1.0f;
            p.metallic = 1.0f;
        } else {
            p.roughness = rough;
            p.metallic = metal;
        }
        p.doubleSided = false;
        r.setMaterial(MaterialId(slot), p);
    };
    mat(kAsphalt, tAsphaltC, tAsphaltN, 0.9f);
    mat(kSidewalk, tSidewalkC, tSidewalkN, 0.88f);
    mat(kPromenade, tPromC, tPromN, 0.7f);
    mat(kCycle, tCycleC, tCycleN, 0.85f);
    {
        MaterialParams p;
        p.baseColorTexture = TextureId(tMarkC);
        p.alphaMask = true;
        p.alphaCutoff = 0.5f;
        p.doubleSided = true;
        p.roughness = 0.7f;
        r.setMaterial(MaterialId(kMarking), p);
        p.baseColorTexture = TextureId(tMarkYC);
        r.setMaterial(MaterialId(kMarkingY), p);
    }
    for (u32 s = 0; s < kFacadeStyles; ++s)
        for (u32 pal = 0; pal < kPalettes; ++pal) mat(kFacade0 + s * kPalettes + pal, tFacadeC + s * kPalettes + pal, tFacadeN + s, 1.0f, 1.0f, tFacadeMR + s);
    for (u32 v = 0; v < kShopVariants; ++v) mat(kShop0 + v, tShopC + v, tShopN + v, 1.0f, 1.0f, tShopMR + v);
    for (u32 pal = 0; pal < kPalettes; ++pal) mat(kBlank0 + pal, tBlankC + pal, ~0u, 0.92f);
    mat(kRoofTile, tRoofTileC, tRoofTileN, 0.8f);
    mat(kRoofSlab, tSlabC, tSlabN, 0.85f);
    mat(kFoundation, tFoundC, tFoundN, 0.9f);
    mat(kGutter, tGutterC, tGutterN, 0.88f);
    for (const auto& [slot, tex] : {std::pair<u32, u32>{kManhole, tManholeC}, std::pair<u32, u32>{kDrain, tDrainC}}) {
        MaterialParams p;
        p.baseColorTexture = TextureId(tex);
        p.alphaMask = true;
        p.alphaCutoff = 0.5f;
        p.doubleSided = true;
        p.roughness = 0.55f;
        p.metallic = 0.3f;
        r.setMaterial(MaterialId(slot), p);
    }
    auto flat = [&](u32 slot, Vec3 c, f32 rough, f32 metal) {
        MaterialParams p;
        p.baseColor = Vec4(c, 1.0f);
        p.roughness = rough;
        p.metallic = metal;
        p.doubleSided = true;
        r.setMaterial(MaterialId(slot), p);
    };
    flat(kMetal, Vec3(0.20f, 0.21f, 0.22f), 0.45f, 0.8f);
    flat(kLampHead, Vec3(0.75f, 0.75f, 0.70f), 0.4f, 0.3f);
    flat(kPaintWhite, Vec3(0.90f, 0.90f, 0.88f), 0.6f, 0.0f);
    flat(kPaintRed, Vec3(0.62f, 0.12f, 0.10f), 0.55f, 0.0f);
    flat(kGlassDark, Vec3(0.06f, 0.09f, 0.11f), 0.1f, 0.0f);
    flat(kWood, Vec3(0.40f, 0.26f, 0.15f), 0.75f, 0.0f);
    flat(kPlastic, Vec3(0.12f, 0.30f, 0.16f), 0.6f, 0.0f);
    flat(kWire, Vec3(0.03f, 0.03f, 0.035f), 0.5f, 0.2f);
    flat(kCarPaint, Vec3(1.0f, 1.0f, 1.0f), 0.32f, 0.18f);
    flat(kRubber, Vec3(0.025f, 0.025f, 0.03f), 0.85f, 0.0f);
    flat(kPaintBlue, Vec3(0.05f, 0.20f, 0.52f), 0.5f, 0.0f);

    // ---- prop meshes ----
    { // street lamp: a tapered pole with an arm over the street (the head is a separate mesh so it can be lit differently)
        Solid pole;
        pole.cylinder(Vec3(0, 0, 0), 0.09f, 7.2f, 8);
        pole.box(Vec3(0.0f, 7.15f, 0.0f), Vec3(0.12f, 0.05f, 0.12f));
        pole.box(Vec3(1.1f, 7.3f, 0.0f), Vec3(1.1f, 0.04f, 0.04f));
        UploadMesh(r, Prop::LampPole, pole);
        Solid head;
        head.box(Vec3(2.0f, 7.2f, 0.0f), Vec3(0.45f, 0.09f, 0.17f));
        UploadMesh(r, Prop::LampHead, head);
    }
    {
        Solid pole;
        pole.cylinder(Vec3(0, 0, 0), 0.06f, 3.4f, 8);
        pole.box(Vec3(0.0f, 3.3f, 0.0f), Vec3(0.2f, 0.04f, 0.04f));
        UploadMesh(r, Prop::TrafficPole, pole);
        Solid head;
        head.box(Vec3(0.0f, 3.0f, 0.0f), Vec3(0.17f, 0.45f, 0.14f));
        UploadMesh(r, Prop::TrafficHead, head);
    }
    {
        Solid frame;
        for (f32 sx : {-1.4f, 1.4f}) frame.box(Vec3(sx, 1.2f, 0.0f), Vec3(0.04f, 1.2f, 0.04f));
        frame.box(Vec3(0.0f, 1.2f, -0.55f), Vec3(1.4f, 1.0f, 0.02f));
        frame.box(Vec3(0.0f, 0.5f, -0.35f), Vec3(1.0f, 0.03f, 0.2f)); // seat
        UploadMesh(r, Prop::ShelterFrame, frame);
        Solid roof;
        roof.box(Vec3(0.0f, 2.45f, -0.1f), Vec3(1.6f, 0.05f, 0.75f));
        UploadMesh(r, Prop::ShelterRoof, roof);
    }
    {
        Solid bench;
        for (int k = 0; k < 4; ++k) bench.box(Vec3(0, 0.45f, -0.18f + 0.12f * f32(k)), Vec3(0.9f, 0.02f, 0.05f));
        bench.box(Vec3(0, 0.85f, -0.24f), Vec3(0.9f, 0.18f, 0.02f));
        UploadMesh(r, Prop::Bench, bench);
        Solid bin;
        bin.cylinder(Vec3(0, 0, 0), 0.22f, 0.85f, 10);
        UploadMesh(r, Prop::Bin, bin);
    }
    { // lifeguard post: stilts, a cabin with a deck and a roof
        Solid tower;
        for (f32 sx : {-1.1f, 1.1f})
            for (f32 sz : {-1.1f, 1.1f}) tower.box(Vec3(sx, 1.4f, sz), Vec3(0.12f, 1.4f, 0.12f));
        tower.box(Vec3(0, 2.85f, 0), Vec3(1.5f, 0.07f, 1.5f));
        tower.box(Vec3(0, 4.1f, -0.2f), Vec3(1.05f, 1.2f, 1.05f));
        UploadMesh(r, Prop::PostoTower, tower);
        Solid roof;
        roof.box(Vec3(0, 5.4f, -0.2f), Vec3(1.5f, 0.08f, 1.4f));
        UploadMesh(r, Prop::PostoRoof, roof);
    }
    {
        Solid pole;
        pole.cylinder(Vec3(0, 0, 0), 0.025f, 2.3f, 6);
        UploadMesh(r, Prop::UmbrellaPole, pole);
        Solid canopy;
        canopy.dome(Vec3(0, 1.95f, 0), 1.15f, 0.38f);
        UploadMesh(r, Prop::UmbrellaCanopy, canopy);
    }
    {
        Solid body;
        body.box(Vec3(0, 1.15f, 0), Vec3(1.6f, 1.15f, 1.1f));
        UploadMesh(r, Prop::KioskBody, body);
        Solid roof;
        roof.box(Vec3(0, 2.4f, 0), Vec3(1.9f, 0.1f, 1.4f));
        UploadMesh(r, Prop::KioskRoof, roof);
    }
    {
        Solid post;
        post.cylinder(Vec3(0, 0, 0), 0.05f, 2.6f, 6);
        UploadMesh(r, Prop::NetPost, post);
        Solid band;
        band.box(Vec3(0, 0, 0), Vec3(4.0f, 0.04f, 0.012f));
        UploadMesh(r, Prop::NetBand, band);
    }
    BuildCar(r, 0, Prop::CarHatchBody, Prop::CarHatchGlass, Prop::CarHatchWheels);
    BuildCar(r, 1, Prop::CarSedanBody, Prop::CarSedanGlass, Prop::CarSedanWheels);
    BuildCar(r, 2, Prop::CarSuvBody, Prop::CarSuvGlass, Prop::CarSuvWheels);
    { // fire hydrant
        Solid h;
        h.cylinder(Vec3(0, 0, 0), 0.13f, 0.66f, 10);
        h.cylinder(Vec3(0, 0.62f, 0), 0.165f, 0.12f, 10);
        h.box(Vec3(0.17f, 0.42f, 0), Vec3(0.09f, 0.055f, 0.055f));
        h.box(Vec3(-0.17f, 0.42f, 0), Vec3(0.09f, 0.055f, 0.055f));
        h.box(Vec3(0, 0.37f, 0.17f), Vec3(0.06f, 0.06f, 0.09f));
        UploadMesh(r, Prop::Hydrant, h);
    }
    { // street sign pole with a name plate (two blue plates, one each way) and the two kinds of round sign
        Solid post;
        post.cylinder(Vec3(0, 0, 0), 0.035f, 2.6f, 8);
        UploadMesh(r, Prop::SignPost, post);
        Solid plate;
        plate.box(Vec3(0, 2.42f, 0), Vec3(0.46f, 0.10f, 0.012f));
        plate.box(Vec3(0, 2.42f, 0), Vec3(0.012f, 0.10f, 0.46f));
        UploadMesh(r, Prop::SignPlate, plate);
        Solid red;
        WheelZ(red, Vec3(0, 2.25f, -0.008f), 0.31f, 0.012f, 16);
        UploadMesh(r, Prop::SignDiscRed, red);
        Solid blue;
        WheelZ(blue, Vec3(0, 2.25f, 0.004f), 0.255f, 0.014f, 16);
        UploadMesh(r, Prop::SignDiscBlue, blue);
    }
    {
        Solid b;
        b.cylinder(Vec3(0, 0, 0), 0.075f, 0.85f, 10);
        UploadMesh(r, Prop::Bollard, b);
    }
    { // an illuminated advertising panel on a post (the street-furniture panels of a Brazilian city), two faces
        Solid post;
        post.box(Vec3(0, 1.1f, 0), Vec3(0.55f, 1.1f, 0.10f));
        post.box(Vec3(0, 2.25f, 0), Vec3(0.60f, 0.05f, 0.14f));
        UploadMesh(r, Prop::AdPanel, post);
        Solid face;
        face.box(Vec3(0, 1.15f, 0.105f), Vec3(0.48f, 0.82f, 0.008f));
        face.box(Vec3(0, 1.15f, -0.105f), Vec3(0.48f, 0.82f, 0.008f));
        UploadMesh(r, Prop::AdFace, face);
    }
    { // utility pole: a concrete pole with a cross arm and insulators (the wires are chunk geometry)
        Solid pole;
        pole.cylinder(Vec3(0, 0, 0), 0.13f, 9.4f, 8);
        pole.box(Vec3(0, 8.4f, 0), Vec3(0.08f, 0.07f, 0.9f));
        for (f32 z : {-0.7f, 0.0f, 0.7f}) pole.box(Vec3(0, 8.55f, z), Vec3(0.03f, 0.08f, 0.03f));
        pole.box(Vec3(0.28f, 6.4f, 0), Vec3(0.22f, 0.45f, 0.18f)); // a transformer can
        UploadMesh(r, Prop::UtilityPole, pole);
    }
}

void ReleaseCityContent(Renderer& r) {
    for (u32 i = 0; i < tCount; ++i) r.releaseTexture(TextureId(i));
    for (u32 s = 0; s < kSlotCount; ++s) r.releaseMaterial(MaterialId(s));
    for (u32 p = 0; p < u32(Prop::Count); ++p) r.releaseMeshAsset(PropMesh(Prop(p)));
}

} // namespace foh::city
