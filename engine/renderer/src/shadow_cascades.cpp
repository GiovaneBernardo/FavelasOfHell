#include "foh/renderer/shadow_cascades.h"

#include <algorithm>
#include <cmath>

namespace foh {

namespace {

// World-space corners of the camera frustum slice between view depths d0 and d1.
// Reverse-Z: NDC z = 1 is the near plane, 0 the far plane.
void sliceCorners(const Mat4& invViewProj, f32 nearZ, f32 farZ, f32 d0, f32 d1, Vec3 out[8]) {
    auto unproject = [&](f32 x, f32 y, f32 z) {
        const Vec4 p = invViewProj * Vec4(x, y, z, 1.0f);
        return Vec3(p) / p.w;
    };
    const f32 t0 = (d0 - nearZ) / (farZ - nearZ);
    const f32 t1 = (d1 - nearZ) / (farZ - nearZ);
    u32 i = 0;
    for (f32 y : {-1.0f, 1.0f}) {
        for (f32 x : {-1.0f, 1.0f}) {
            // View depth is linear along a ray from the eye, so lerping the edge is exact.
            const Vec3 n = unproject(x, y, 1.0f);
            const Vec3 f = unproject(x, y, 0.0f);
            out[i++] = n + (f - n) * t0;
            out[i++] = n + (f - n) * t1;
        }
    }
}

} // namespace

ShadowCascades ComputeShadowCascades(const ShadowCascadeDesc& d) {
    ShadowCascades out{};
    const f32 farZ = std::max(std::min(d.farZ, d.shadowDistance), d.nearZ + 0.01f);
    const Mat4 invViewProj = glm::inverse(d.proj * d.view);
    const Vec3 dir = glm::normalize(d.lightDirection);
    const Vec3 up = std::abs(dir.y) > 0.99f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);
    const Mat4 lightView = glm::lookAtRH(Vec3(0.0f), dir, up); // rotation only
    const f32 resolution = static_cast<f32>(std::max(d.resolution, 1u));

    f32 sliceNear = d.nearZ;
    for (u32 c = 0; c < kShadowCascades; ++c) {
        const f32 ratio = static_cast<f32>(c + 1) / static_cast<f32>(kShadowCascades);
        const f32 logSplit = d.nearZ * std::pow(farZ / d.nearZ, ratio);
        const f32 uniSplit = d.nearZ + (farZ - d.nearZ) * ratio;
        const f32 sliceFar = d.splitLambda * logSplit + (1.0f - d.splitLambda) * uniSplit;

        Vec3 corners[8];
        sliceCorners(invViewProj, d.nearZ, d.farZ, sliceNear, sliceFar, corners);
        Vec3 center(0.0f);
        for (const Vec3& p : corners) center += p;
        center /= 8.0f;
        f32 radius = 0.0f;
        for (const Vec3& p : corners) radius = std::max(radius, glm::length(p - center));
        // Quantized so tiny radius changes (float noise) do not resize the cascade.
        radius = std::ceil(radius * 16.0f) / 16.0f;

        const f32 texel = 2.0f * radius / resolution;
        Vec3 c0 = Vec3(lightView * Vec4(center, 1.0f));
        c0.x = std::floor(c0.x / texel) * texel;
        c0.y = std::floor(c0.y / texel) * texel;

        // Light looks down -Z in its view space: distance along the light = -z.
        const f32 nearD = -c0.z - radius - d.casterMargin;
        const f32 farD = -c0.z + radius;
        Mat4 proj = glm::orthoRH_ZO(c0.x - radius, c0.x + radius, c0.y - radius, c0.y + radius, nearD, farD);
        Mat4 flip(1.0f); // reverse-Z: depth' = 1 - depth
        flip[2][2] = -1.0f;
        flip[3][2] = 1.0f;

        out.viewProj[c] = flip * proj * lightView;
        out.splitFar[c] = sliceFar;
        out.texelWorld[c] = texel;
        sliceNear = sliceFar;
    }
    return out;
}

} // namespace foh
