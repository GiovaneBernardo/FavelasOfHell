#pragma once

// Job-thread loaders that turn files into thumbnails (see thumbnails.h). Everything here is pure:
// no shared state, so any number may run at once.

#include <filesystem>
#include <unordered_map>

#include "foh/asset_id.h"
#include "thumbnails.h"

namespace foh {

// Decodes a PNG/JPG/TGA/BMP/... file and box-filters it down to fit kThumbSize.
bool LoadImageThumbnail(const std::filesystem::path& file, ThumbPixels& out);
// Same for an encoded image already in memory (downloaded previews).
bool DecodeImageThumbnail(const u8* data, size_t size, ThumbPixels& out);

// What the model rasterizer needs, resolved on the main thread (the asset database itself is not thread safe).
struct ModelThumbSource {
    std::filesystem::path prefabFile;
    // Cooked file of every mesh and material the prefab can reference.
    std::unordered_map<AssetId, std::filesystem::path, AssetIdHash> files;
};

// Software-rasterizes the cooked prefab (all its meshes, node transforms applied) with simple
// lighting. Runs entirely on the CPU so it needs no GPU context and cannot disturb the renderer.
bool RenderModelThumbnail(const ModelThumbSource& source, ThumbPixels& out);

} // namespace foh
