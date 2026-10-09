#include "water_test_scene.h"

#include <algorithm>
#include <cmath>

namespace foh {

namespace {

f32 Smoothstep(f32 a, f32 b, f32 x) {
    const f32 t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The shoreline z as a function of x: waves in the coast and a cove around x = 0.
f32 ShoreZ(f32 x) {
    return -90.0f + 70.0f * std::sin(x / 210.0f) + 35.0f * std::sin(x / 83.0f + 1.3f) - 120.0f * std::exp(-(x * x) / (2.0f * 160.0f * 160.0f));
}

f32 BaseHeight(f32 x, f32 z) {
    const f32 d = ShoreZ(x) - z; // > 0 on land
    if (d >= 0.0f) {
        const f32 hills = 140.0f * Smoothstep(60.0f, 520.0f, d) * (0.65f + 0.35f * std::sin(x / 130.0f) * std::cos(d / 170.0f));
        return 0.035f * d + hills;
    }
    const f32 s = -d;
    const f32 u = s / 300.0f;
    return -(0.035f * s + 38.0f * (1.0f - std::exp(-u * u)));
}

// A gaussian bump that reaches `peak` metres above the sea at its centre, whatever the sea floor was there.
f32 Bump(f32 x, f32 z, f32 cx, f32 cz, f32 radius, f32 peak) {
    const f32 r = std::hypot(x - cx, z - cz);
    const f32 amplitude = peak - BaseHeight(cx, cz);
    return amplitude * std::exp(-(r / radius) * (r / radius));
}

} // namespace

f32 WaterTestTerrain::height(f32 x, f32 z) {
    return BaseHeight(x, z) + Bump(x, z, 260.0f, 420.0f, 50.0f, 20.0f)    // rocky islet
           + Bump(x, z, -300.0f, 250.0f, 80.0f, 1.6f);                    // sandbar a little above the waves
}

WaterTestMesh BuildWaterTestMesh(f32 cell) {
    const u32 n = static_cast<u32>(2.0f * WaterTestTerrain::kHalfSize / cell) + 1;
    WaterTestMesh m;
    m.positions.reserve(static_cast<size_t>(n) * n);
    m.normals.reserve(static_cast<size_t>(n) * n);
    m.uvs.reserve(static_cast<size_t>(n) * n);
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            const f32 x = -WaterTestTerrain::kHalfSize + static_cast<f32>(i) * cell;
            const f32 z = -WaterTestTerrain::kHalfSize + static_cast<f32>(j) * cell;
            const f32 hx = WaterTestTerrain::height(x + cell, z) - WaterTestTerrain::height(x - cell, z);
            const f32 hz = WaterTestTerrain::height(x, z + cell) - WaterTestTerrain::height(x, z - cell);
            m.positions.push_back({x, WaterTestTerrain::height(x, z), z});
            m.normals.push_back(glm::normalize(Vec3(-hx, 2.0f * cell, -hz)));
            m.uvs.push_back({x * 0.05f, z * 0.05f});
        }
    }
    m.indices.reserve(static_cast<size_t>(n - 1) * (n - 1) * 6);
    for (u32 j = 0; j + 1 < n; ++j) {
        for (u32 i = 0; i + 1 < n; ++i) {
            const u32 i00 = j * n + i, i10 = i00 + 1, i01 = i00 + n, i11 = i01 + 1;
            // Counter-clockwise seen from above, like every mesh the renderer draws.
            for (u32 v : {i00, i01, i10, i10, i01, i11}) m.indices.push_back(v);
        }
    }
    return m;
}

std::vector<f32> BuildWaterTestBed(u32 size) {
    std::vector<f32> heights(static_cast<size_t>(size) * size);
    const f32 extent = 2.0f * WaterTestTerrain::kHalfSize;
    for (u32 j = 0; j < size; ++j) {
        for (u32 i = 0; i < size; ++i) {
            const f32 x = -WaterTestTerrain::kHalfSize + (static_cast<f32>(i) + 0.5f) / static_cast<f32>(size) * extent;
            const f32 z = -WaterTestTerrain::kHalfSize + (static_cast<f32>(j) + 0.5f) / static_cast<f32>(size) * extent;
            heights[static_cast<size_t>(j) * size + i] = WaterTestTerrain::height(x, z);
        }
    }
    return heights;
}

} // namespace foh
