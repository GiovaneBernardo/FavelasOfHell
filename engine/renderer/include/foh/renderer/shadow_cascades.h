#pragma once

// Cascaded shadow map fitting for the sun. Pure CPU math (no NVRHI), so it is
// unit tested and safe to include from game modules.
//
// The view frustum is split along view depth; every slice gets an orthographic
// light-space projection fitted to the slice's bounding SPHERE (its size does not
// change when the camera rotates, so shadow edges do not shimmer) and snapped to
// whole shadow texels (so they do not crawl when the camera translates).
// Depth is reverse-Z like the main view: the nearest point to the light is 1.

#include "foh/math.h"
#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

inline constexpr u32 kShadowCascades = 4;

struct ShadowCascadeDesc {
    Mat4 view{1.0f}; // camera world -> view
    Mat4 proj{1.0f}; // camera reverse-Z projection (see MakePerspectiveReverseZ)
    f32 nearZ = 0.1f;
    f32 farZ = 4000.0f;
    Vec3 lightDirection{0.0f, -1.0f, 0.0f}; // direction the light travels
    f32 shadowDistance = 400.0f;            // shadows end here (clamped to farZ)
    u32 resolution = 2048;                  // texels per cascade side
    f32 splitLambda = 0.8f;                 // 0 = uniform splits, 1 = logarithmic
    f32 casterMargin = 150.0f;              // how far in front of a slice casters are still kept
};

struct ShadowCascades {
    Mat4 viewProj[kShadowCascades];   // world -> shadow clip space of each cascade
    f32 splitFar[kShadowCascades];    // view-space depth where each cascade ends
    f32 texelWorld[kShadowCascades];  // world-space size of one shadow texel
};

FOH_API ShadowCascades ComputeShadowCascades(const ShadowCascadeDesc& desc);

} // namespace foh
