#pragma once

// Render-facing components. Plain data (the ECS requires trivially copyable
// components), no NVRHI, safe to include from game modules.
//
// Transforms: `Transform` is LOCAL (relative to the parent entity; for an entity without a parent it
// is the world transform). `GlobalTransform` is the world-space result, computed every frame by the
// propagation system (see RegisterTransformPropagation) before rendering extracts it; the renderer,
// picking and the editor gizmo read that one. Write Transform, read GlobalTransform. (Both will move
// to their own scene module together with the rest of the transform code.)

#include <string_view>

#include "foh/asset_id.h"
#include "foh/ecs/buffer.h"
#include "foh/ecs/reflect.h"
#include "foh/math.h"
#include "foh/types.h"

namespace foh {

// Human readable label shown by the editor (hierarchy, inspector). Fixed capacity
// because components are plain data.
struct Name {
    char text[48] = {};
    static Name Make(std::string_view s) {
        Name n;
        const size_t len = s.size() < sizeof(n.text) - 1 ? s.size() : sizeof(n.text) - 1;
        for (size_t i = 0; i < len; ++i) n.text[i] = s[i];
        return n;
    }
};

struct Transform {
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};

    Mat4 matrix() const {
        return glm::translate(Mat4(1.0f), position) * glm::mat4_cast(rotation) * glm::scale(Mat4(1.0f), scale);
    }

    // Translation, rotation and scale of an affine matrix (shear is dropped).
    static Transform FromMatrix(const Mat4& m) {
        Transform t;
        t.position = Vec3(m[3]);
        Vec3 cols[3] = {Vec3(m[0]), Vec3(m[1]), Vec3(m[2])};
        Vec3 s(glm::length(cols[0]), glm::length(cols[1]), glm::length(cols[2]));
        if (glm::determinant(Mat3(cols[0], cols[1], cols[2])) < 0.0f) s.x = -s.x; // mirrored
        for (int i = 0; i < 3; ++i)
            if (s[i] != 0.0f) cols[i] /= s[i];
        t.scale = s;
        t.rotation = glm::normalize(glm::quat_cast(Mat3(cols[0], cols[1], cols[2])));
        return t;
    }
};

// World-space transform as a 3x4 matrix (rows, the layout the GPU instance stream uses), plus the frame
// it was last computed in (propagation bookkeeping). Computed from Transform and the parent chain; an
// entity with a Transform gets one automatically.
struct GlobalTransform {
    Vec4 row0{1.0f, 0.0f, 0.0f, 0.0f};
    Vec4 row1{0.0f, 1.0f, 0.0f, 0.0f};
    Vec4 row2{0.0f, 0.0f, 1.0f, 0.0f};
    u32 stamp = 0;

    Mat4 matrix() const {
        return Mat4(Vec4(row0.x, row1.x, row2.x, 0.0f), Vec4(row0.y, row1.y, row2.y, 0.0f), Vec4(row0.z, row1.z, row2.z, 0.0f),
                    Vec4(row0.w, row1.w, row2.w, 1.0f));
    }
    Vec3 position() const { return Vec3(row0.w, row1.w, row2.w); }
    // World-space basis vectors (columns of the 3x3 part), not normalized.
    Vec3 axisX() const { return Vec3(row0.x, row1.x, row2.x); }
    Vec3 axisY() const { return Vec3(row0.y, row1.y, row2.y); }
    Vec3 axisZ() const { return Vec3(row0.z, row1.z, row2.z); }

    static GlobalTransform FromMatrix(const Mat4& m) {
        GlobalTransform g;
        g.row0 = Vec4(m[0][0], m[1][0], m[2][0], m[3][0]);
        g.row1 = Vec4(m[0][1], m[1][1], m[2][1], m[3][1]);
        g.row2 = Vec4(m[0][2], m[1][2], m[2][2], m[3][2]);
        return g;
    }
};

// Built-in primitive meshes, until the mesh asset pipeline exists.
enum class BuiltinMesh : u32 { Cube, Plane, Sphere, Count };

// Draws one mesh. An imported mesh is referenced by asset id (stable across sessions and
// saves) plus the submesh to draw; `mesh` is only used while `asset` is invalid.
// `material` is the material asset the color came from (the editor keeps `color` in sync
// when that material is reimported); textures are not sampled by the renderer yet.
struct MeshRenderer {
    u32 mesh = static_cast<u32>(BuiltinMesh::Cube);
    Vec4 color{0.8f, 0.8f, 0.8f, 1.0f};
    AssetId asset;
    u32 submesh = 0;
    AssetId material;
};

// ---- skinning ----------------------------------------------------------------------------------
// Animation (engine/animation) produces the skinning matrices, rendering consumes them. Both sides only share these
// plain components, so neither needs the other's headers.

// One skinning matrix: jointModelPose * inverseBind, as 3 rows (the layout GlobalTransform and the instance stream use).
// It maps a vertex of the mesh's bind pose to the posed vertex in the character's model space.
struct SkinMatrix {
    Vec4 row0{1.0f, 0.0f, 0.0f, 0.0f};
    Vec4 row1{0.0f, 1.0f, 0.0f, 0.0f};
    Vec4 row2{0.0f, 0.0f, 1.0f, 0.0f};
};

// The pose of a character, one SkinMatrix per skeleton joint, refilled every frame by the animation system. Lives on
// the character's root entity; the engine owns and frees the buffer. An empty buffer means "no pose yet".
struct SkinPalette {
    BufferHandle<SkinMatrix> matrices;
};

// Marks a MeshRenderer whose mesh is skinned (its asset has a skeleton). The skinned entity is a child of the entity
// that holds the SkinPalette; it is drawn only once that palette exists, placed by the root's GlobalTransform.
struct SkinnedMesh {
    u32 flags = 0; // reserved
};

// Not drawn (and, for a character, not skinned): put it on a mesh entity or on the root of a skinned character
// (first person hides the player's own body this way). Costs nothing for entities without it.
struct Hidden {
    u32 flags = 0; // reserved
};

// Perspective camera looking down the entity's local -Z (Y up).
struct Camera {
    f32 fovY = 1.0471976f; // 60 degrees
    f32 nearZ = 0.0001f;
    f32 farZ = 15000.0f;
    bool active = true;
};

// Sunlight: shines along the entity's local -Z.
struct DirectionalLight {
    Vec3 color{1.0f, 0.96f, 0.88f};
    f32 intensity = 3.0f;
};

} // namespace foh

// Reflection: lets the world serialize these and migrate them across layout changes.
FOH_REFLECT(foh::Name, text);
FOH_REFLECT(foh::Transform, position, rotation, scale);
FOH_REFLECT(foh::GlobalTransform, row0, row1, row2, stamp);
FOH_REFLECT(foh::AssetId, hi, lo);
FOH_REFLECT(foh::MeshRenderer, mesh, color, asset, submesh, material);
FOH_REFLECT(foh::SkinMatrix, row0, row1, row2);
FOH_REFLECT(foh::SkinPalette, matrices);
FOH_REFLECT(foh::SkinnedMesh, flags);
FOH_REFLECT(foh::Hidden, flags);
FOH_REFLECT(foh::Camera, fovY, nearZ, farZ, active);
FOH_REFLECT(foh::DirectionalLight, color, intensity);
