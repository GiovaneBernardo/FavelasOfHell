#pragma once

// Small deterministic noise used by the terrain generator and the vegetation placement. Integer hashing only, so a
// seed gives the same world on every platform and compiler (stable seeds, ADR 0012).

#include <algorithm>
#include <cmath>

#include "foh/types.h"

namespace foh::noise {

inline u32 Hash(i32 x, i32 y, u32 seed) {
    u32 h = static_cast<u32>(x) * 0x8da6b343u ^ static_cast<u32>(y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}

// Uniform in [0, 1).
inline f32 Rand01(i32 x, i32 y, u32 seed) { return static_cast<f32>(Hash(x, y, seed) & 0xFFFFFFu) * (1.0f / 16777216.0f); }

inline f32 Fade(f32 t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

// Value noise in [-1, 1].
inline f32 ValueNoise(f32 x, f32 y, u32 seed) {
    const f32 fx = std::floor(x), fy = std::floor(y);
    const i32 ix = static_cast<i32>(fx), iy = static_cast<i32>(fy);
    const f32 tx = Fade(x - fx), ty = Fade(y - fy);
    auto v = [&](i32 dx, i32 dy) { return static_cast<f32>(Hash(ix + dx, iy + dy, seed) & 0xFFFFFFu) * (2.0f / 16777215.0f) - 1.0f; };
    const f32 a = v(0, 0) + (v(1, 0) - v(0, 0)) * tx;
    const f32 b = v(0, 1) + (v(1, 1) - v(0, 1)) * tx;
    return a + (b - a) * ty;
}

inline f32 Fbm(f32 x, f32 y, u32 seed, int octaves) {
    f32 sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * ValueNoise(x, y, seed + static_cast<u32>(o) * 101u);
        norm += amp;
        x = x * 2.03f + 17.1f;
        y = y * 2.03f - 9.3f;
        amp *= 0.5f;
    }
    return sum / norm;
}

// Ridged noise in [0, 1]: sharp crests, used for rock.
inline f32 Ridged(f32 x, f32 y, u32 seed, int octaves) {
    f32 sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        const f32 n = 1.0f - std::abs(ValueNoise(x, y, seed + static_cast<u32>(o) * 57u));
        sum += amp * n * n;
        norm += amp;
        x = x * 2.1f + 5.7f;
        y = y * 2.1f + 3.1f;
        amp *= 0.5f;
    }
    return sum / norm;
}

inline f32 Smoothstep(f32 a, f32 b, f32 x) {
    const f32 t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline f32 Mix(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

} // namespace foh::noise
