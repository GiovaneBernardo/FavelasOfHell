#pragma once

// Helpers shared by the importers and the cook step. Private to the assets module.

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "foh/asset_import.h"

namespace foh::import_util {

// `base` if unused, else base#2, base#3...; an empty base becomes `fallback`. Registers the result.
std::string uniqueKey(std::set<std::string>& used, std::string base, const std::string& fallback);

// Replaces characters that are unsafe in node paths ('/').
std::string sanitizeName(std::string name);

// Smooth normals from triangles (area weighted); overwrites existing ones.
void generateNormals(ImportedMesh& mesh);
// Per-vertex tangents from UV derivatives (needs normals); handedness in w.
void generateTangents(ImportedMesh& mesh);

struct StbFree {
    void operator()(u8* p) const;
};

// Decoded pixels stay in the buffer the decoder allocated: a 4K image is 64 MB, and copying it into a vector
// would double the allocation traffic (and the page-fault storm that comes with it) for nothing.
struct DecodedImage {
    u32 width = 0;
    u32 height = 0;
    std::unique_ptr<u8, StbFree> pixels; // RGBA8, width * height * 4 bytes
};

// Decodes an encoded image (png/jpg/tga/bmp/...) to RGBA8. False with `error` on failure. Thread safe.
bool decodeImage(const u8* data, size_t size, DecodedImage& out, std::string& error);
// Checks that the header is a supported image without decoding it (importers use this to fail early).
bool probeImage(const u8* data, size_t size, std::string& error);

// Merges identical vertices (FBX gives one vertex per face corner).
void weldVertices(ImportedMesh& mesh);

// Four joint influences -> bytes: strongest first, weights normalised to sum to exactly 255 (largest remainder).
// Unused slots have weight 0. False (outputs zeroed) when no weight is positive.
bool packSkinInfluences(const u32 joints[4], const f32 weights[4], u8 outJoints[4], u8 outWeights[4]);

// Lower-case copy of the file extension including the dot.
std::string lowerExtension(const std::filesystem::path& p);

} // namespace foh::import_util
