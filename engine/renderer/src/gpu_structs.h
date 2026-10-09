#pragma once

// CPU mirrors of structs shared with shaders. Keep in sync with
// shaders/common/view.slangh; the static_asserts catch layout drift.

#include <cstddef>

#include "foh/math.h"
#include "foh/renderer/shadow_cascades.h"
#include "foh/types.h"

namespace foh {

struct ViewConstants {
    Mat4 view;
    Mat4 proj;
    Mat4 viewProj;
    Mat4 invViewProj;
    Vec4 cameraPos;
    Vec4 sunDirection;
    Vec4 sunColor;
    Vec4 viewport;
    Vec4 params;
    Mat4 cascadeViewProj[kShadowCascades];
    Vec4 cascadeSplits;
    Vec4 cascadeTexel;
    Vec4 shadowParams;
    Vec4 atmosphere0, atmosphere1, atmosphere2, atmosphere3;
};
static_assert(kShadowCascades == 4, "view.slangh declares four cascades");
static_assert(sizeof(ViewConstants) == 4 * 64 + 5 * 16 + 4 * 64 + 3 * 16 + 4 * 16, "ViewConstants must match view.slangh");
static_assert(offsetof(ViewConstants, cascadeViewProj) == 336, "ViewConstants layout drifted from view.slangh");
static_assert(offsetof(ViewConstants, shadowParams) == 624, "ViewConstants layout drifted from view.slangh");
static_assert(offsetof(ViewConstants, atmosphere0) == 640, "ViewConstants layout drifted from view.slangh");
static_assert(offsetof(ViewConstants, cameraPos) == 256, "ViewConstants layout drifted from view.slangh");
static_assert(offsetof(ViewConstants, params) == 320, "ViewConstants layout drifted from view.slangh");

// Per-instance vertex stream: rows 0..2 of the world matrix, tint color, entity id, material index.
struct GpuInstance {
    Vec4 row0, row1, row2;
    Vec4 color;             // multiplies the material's base color
    u32 entityLo, entityHi; // Entity::id, read by the picking pass
    u32 material;           // index into the material buffer (0 = default material)
    u32 pad;
};
static_assert(sizeof(GpuInstance) == 80, "instance stride is baked into the input layout");

// Four signed-normalized bytes: tangent xyz and the bitangent sign (+1/-1) in w.
struct PackedTangent {
    i8 x, y, z, w;
};
inline PackedTangent PackTangent(const Vec4& t) {
    auto q = [](f32 v) { return static_cast<i8>(glm::clamp(v, -1.0f, 1.0f) * 127.0f + (v >= 0.0f ? 0.5f : -0.5f)); };
    return {q(t.x), q(t.y), q(t.z), t.w < 0.0f ? static_cast<i8>(-127) : static_cast<i8>(127)};
}

// 36 bytes: normals stay full precision (they feed the G-buffer octahedral encode), tangents are
// packed, which is enough for a tangent frame and costs 4 bytes per vertex instead of 16.
struct MeshVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    PackedTangent tangent;
};
static_assert(sizeof(MeshVertex) == 36, "vertex stride is baked into the input layout");

// One entry of the material buffer. MIRRORED by GpuMaterial in shaders/common/material.slangh.
// Texture indices are slots of the bindless texture table; unset textures point at the white/flat-normal stand-ins.
struct GpuMaterial {
    Vec4 baseColor;
    Vec3 emissive;
    f32 alphaCutoff;
    f32 metallic, roughness, normalScale, occlusionStrength;
    u32 baseColorTex, normalTex, metalRoughTex, occlusionTex;
    u32 emissiveTex, flags, pad0, pad1;
};
static_assert(sizeof(GpuMaterial) == 80, "material stride is baked into material.slangh");
inline constexpr u32 kMaterialFlagAlphaMask = 1u << 0;
inline constexpr u32 kMaterialFlagDoubleSided = 1u << 1;
inline constexpr u32 kMaterialFlagNormalMap = 1u << 2;

} // namespace foh
