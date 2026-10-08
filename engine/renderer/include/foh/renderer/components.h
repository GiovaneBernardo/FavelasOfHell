#pragma once

// Render-facing components. Plain data (the ECS requires trivially copyable
// components), no NVRHI, safe to include from game modules.
//
// NOTE: Transform is a flat local-to-world transform for now. When the ECS grows
// relationships (parent/child) it moves to its own scene module and gets a
// separate LocalTransform/GlobalTransform pair; the extraction below then reads
// the global one.

#include <string_view>

#include "foh/asset_id.h"
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

// Perspective camera looking down the entity's local -Z (Y up).
struct Camera {
    f32 fovY = 1.0471976f; // 60 degrees
    f32 nearZ = 0.01f;
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
FOH_REFLECT(foh::AssetId, hi, lo);
FOH_REFLECT(foh::MeshRenderer, mesh, color, asset, submesh, material);
FOH_REFLECT(foh::Camera, fovY, nearZ, farZ, active);
FOH_REFLECT(foh::DirectionalLight, color, intensity);
