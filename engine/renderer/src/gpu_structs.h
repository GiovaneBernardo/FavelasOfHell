#pragma once

// CPU mirrors of structs shared with shaders. Keep in sync with
// shaders/common/view.slangh; the static_asserts catch layout drift.

#include <cstddef>

#include "foh/math.h"
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
};
static_assert(sizeof(ViewConstants) == 4 * 64 + 5 * 16, "ViewConstants must match view.slangh");
static_assert(offsetof(ViewConstants, cameraPos) == 256, "ViewConstants layout drifted from view.slangh");
static_assert(offsetof(ViewConstants, params) == 320, "ViewConstants layout drifted from view.slangh");

// Per-instance vertex stream: rows 0..2 of the world matrix and the base color.
struct GpuInstance {
    Vec4 row0, row1, row2;
    Vec4 color;
};
static_assert(sizeof(GpuInstance) == 64, "instance stride is baked into the input layout");

struct MeshVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};
static_assert(sizeof(MeshVertex) == 32, "vertex stride is baked into the input layout");

} // namespace foh
