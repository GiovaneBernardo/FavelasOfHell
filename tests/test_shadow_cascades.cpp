#include <cmath>

#include <doctest/doctest.h>

#include "foh/renderer/renderer.h"
#include "foh/renderer/shadow_cascades.h"

using namespace foh;

namespace {

ShadowCascadeDesc makeDesc(const Mat4& cameraWorld, const Vec3& light) {
    const RenderViewParams v = MakeViewParams(cameraWorld, 1.0471976f, 16.0f / 9.0f, 0.1f, 4000.0f);
    ShadowCascadeDesc d;
    d.view = v.view;
    d.proj = v.proj;
    d.nearZ = v.nearZ;
    d.farZ = v.farZ;
    d.lightDirection = light;
    return d;
}

} // namespace

TEST_CASE("Cascade splits grow and stop at the shadow distance") {
    const ShadowCascades c = ComputeShadowCascades(makeDesc(Mat4(1.0f), Vec3(0.4f, -1.0f, 0.3f)));
    for (u32 i = 1; i < kShadowCascades; ++i) CHECK(c.splitFar[i] > c.splitFar[i - 1]);
    CHECK(c.splitFar[0] > 0.1f);
    CHECK(c.splitFar[kShadowCascades - 1] == doctest::Approx(400.0f));
}

TEST_CASE("Every cascade contains the frustum slice it covers, with reverse-Z depth in [0,1]") {
    const Mat4 camera = glm::translate(Mat4(1.0f), Vec3(10.0f, 5.0f, 30.0f)) *
                        glm::rotate(Mat4(1.0f), 0.7f, Vec3(0.0f, 1.0f, 0.0f));
    ShadowCascadeDesc d = makeDesc(camera, Vec3(0.4f, -1.0f, 0.3f));
    const ShadowCascades c = ComputeShadowCascades(d);
    const Mat4 invViewProj = glm::inverse(d.proj * d.view);

    f32 sliceNear = d.nearZ;
    for (u32 i = 0; i < kShadowCascades; ++i) {
        // Probe the center line of the slice's near and far ends.
        for (f32 depth : {sliceNear + 0.01f, c.splitFar[i] - 0.01f}) {
            const f32 t = (depth - d.nearZ) / (d.farZ - d.nearZ);
            const Vec4 n = invViewProj * Vec4(0, 0, 1, 1);
            const Vec4 f = invViewProj * Vec4(0, 0, 0, 1);
            const Vec3 p = Vec3(n) / n.w + (Vec3(f) / f.w - Vec3(n) / n.w) * t;
            const Vec4 clip = c.viewProj[i] * Vec4(p, 1.0f);
            CHECK(std::abs(clip.x) <= 1.0f);
            CHECK(std::abs(clip.y) <= 1.0f);
            CHECK(clip.z >= 0.0f);
            CHECK(clip.z <= 1.0f);
        }
        sliceNear = c.splitFar[i];
    }
}

TEST_CASE("Points nearer to the light get larger depth (reverse-Z)") {
    const ShadowCascades c = ComputeShadowCascades(makeDesc(Mat4(1.0f), Vec3(0.0f, -1.0f, 0.0f)));
    const Vec4 low = c.viewProj[0] * Vec4(0.0f, 0.0f, -5.0f, 1.0f);
    const Vec4 high = c.viewProj[0] * Vec4(0.0f, 2.0f, -5.0f, 1.0f);
    CHECK(high.z > low.z);
}

TEST_CASE("Cascades are stable under camera translation (texel snapping)") {
    // Moving the camera by less than a texel must not move the shadow map.
    const Vec3 light(0.4f, -1.0f, 0.3f);
    ShadowCascadeDesc d0 = makeDesc(Mat4(1.0f), light);
    const ShadowCascades a = ComputeShadowCascades(d0);
    const f32 shift = a.texelWorld[0] * 0.25f;
    ShadowCascadeDesc d1 = makeDesc(glm::translate(Mat4(1.0f), Vec3(shift, 0.0f, 0.0f)), light);
    const ShadowCascades b = ComputeShadowCascades(d1);

    // A fixed world point moves by a WHOLE number of texels (no sub-texel crawling).
    const Vec3 world(3.0f, 1.0f, -2.0f);
    const Vec4 pa = a.viewProj[0] * Vec4(world, 1.0f);
    const Vec4 pb = b.viewProj[0] * Vec4(world, 1.0f);
    const f32 texelNdc = 2.0f / 2048.0f;
    const f32 texels = (pa.x - pb.x) / texelNdc;
    CHECK(std::abs(texels - std::round(texels)) < 0.01f);
    CHECK(std::abs(texels) <= 1.01f);
    CHECK(a.texelWorld[0] == doctest::Approx(b.texelWorld[0]));
}
