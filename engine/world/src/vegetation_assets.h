#pragma once

// Procedural vegetation content: meshes and textures generated in code, so the world has trees and grass without any
// imported model. They are stand-ins with a deliberate interface (a trunk mesh + a leaf mesh per variant and LOD):
// authored tree models can replace them without touching the placement or streaming code.
//
// Trunks and leaves are separate meshes because they are separate materials: the trunk takes a PBR bark texture
// (albedo + normal map, real-world scale UVs), the leaves a generated alpha-tested atlas lit as smooth foliage.

#include <vector>

#include "foh/math.h"
#include "foh/types.h"

namespace foh {

struct VegMesh {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    std::vector<u32> indices;
    std::vector<Vec4> tangents; // xyz + bitangent sign; empty = the renderer makes one
};

struct VegImage {
    u32 width = 0, height = 0;
    std::vector<u8> rgba; // sRGB colour, straight (not premultiplied) alpha
};

inline constexpr u32 kTreeVariants = 4;
inline constexpr u32 kTreeLods = 2;

struct TreeMeshes {
    VegMesh trunk;  // trunk and branches: UVs in metres / 1.2 (one repeat of the bark texture), tangents present
    VegMesh leaves; // clumps of leaf cards with crown-shaped normals; UVs in the leaf atlas
    f32 trunkRadius = 0.3f; // at the base (scale 1): the collision radius
    f32 height = 12.0f;     // total height at scale 1
};

// A broadleaf tree, 11 to 18 m tall at scale 1 with the foot at the origin. LOD 0 is the close version (flared trunk,
// branches, dense leaves), LOD 1 a coarser silhouette with the same outline.
TreeMeshes BuildTree(u32 variant, u32 lod);
// A palm: leaning trunk and a crown of long fronds (UVs in the frond part of the atlas).
TreeMeshes BuildPalm(u32 lod);
// A tuft of grass: crossed, slightly bent cards about 0.8 m wide and 0.55 m tall.
VegMesh BuildGrassMesh();

// 512 x 512: two leafy twigs (alpha-cut) side by side on top, a palm frond (alpha-cut) in the bottom half.
VegImage BuildLeafAtlas();
// A seamless-enough bark stand-in used until the real bark textures are resident.
VegImage BuildBarkImage();
// Blades of grass fanning from the bottom centre, 256 x 256.
VegImage BuildGrassImage();

// Mip chain (largest first) of an alpha-tested image: colour is averaged weighted by alpha and the alpha of every
// mip is scaled so the same fraction of texels pass the cutoff, which keeps foliage from thinning out with distance.
// With cutoff <= 0 it is a plain box-filtered chain.
std::vector<std::vector<u8>> BuildFoliageMips(const VegImage& image, f32 cutoff, u32 maxLevels);

} // namespace foh
