#pragma once

// Helpers shared by the importers and the cook step. Private to the assets module.

#include <filesystem>
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

// Decodes an encoded image (png/jpg/tga/bmp/...) to RGBA8. False with `error` on failure.
bool decodeImage(const u8* data, size_t size, ImportedImage& out, std::string& error);

// Merges identical vertices (FBX gives one vertex per face corner).
void weldVertices(ImportedMesh& mesh);

// Lower-case copy of the file extension including the dot.
std::string lowerExtension(const std::filesystem::path& p);

} // namespace foh::import_util
