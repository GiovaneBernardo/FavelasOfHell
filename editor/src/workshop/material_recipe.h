#pragma once

// Turns a downloaded PBR texture set (ambientCG, Poly Haven, ...) into what the engine's material
// needs: it finds which file is which map, and packs separate ambient-occlusion / roughness / metalness
// images into one glTF-style "ORM" texture (R = occlusion, G = roughness, B = metalness), because
// MaterialAsset reads roughness and metalness from a single texture.
//
// Thread-safety: pure functions, safe on a job thread.

#include <filesystem>
#include <string>
#include <vector>

namespace foh {

struct PbrFiles {
    std::filesystem::path color, normal, roughness, metalness, ao, arm, emissive;
    bool any() const { return !color.empty() || !normal.empty() || !arm.empty() || !roughness.empty() || !metalness.empty() || !ao.empty(); }
    bool needsPacking() const { return arm.empty() && (!roughness.empty() || !metalness.empty() || !ao.empty()); }
};

// Picks the maps out of a package's files by their names. DirectX-convention normal maps are ignored when
// an OpenGL one exists (the engine uses +Y up normals like glTF); height maps are not used yet.
PbrFiles ClassifyPbrFiles(const std::vector<std::filesystem::path>& imageFiles);

// Writes `out` (PNG). Missing channels default to "no occlusion" (white), fully rough (white) and
// non-metal (black). Maps of different sizes are resampled to the largest.
bool PackOrmTexture(const PbrFiles& files, const std::filesystem::path& out, std::string& error);

} // namespace foh
