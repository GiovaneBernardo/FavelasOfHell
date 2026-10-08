#pragma once

// File type classification and the procedural icons of the content browser (no icon font or image
// files needed: a folder, or a document page with the extension printed on it).

#include <string>
#include <string_view>

#include <imgui.h>

namespace foh {

enum class FileCategory : unsigned char { Folder, Model, Image, Material, Prefab, Audio, Script, Shader, Scene, Text, Archive, Other };

struct FileTypeInfo {
    FileCategory category = FileCategory::Other;
    const char* label = "File"; // "glTF Model"
    ImU32 color = 0;            // accent used for the tile's bottom bar and the icon page
    bool importable = false;    // can go through the asset importer
    bool imagePreview = false;  // a thumbnail can be decoded directly from the file
    bool modelPreview = false;  // a thumbnail can be rasterized from its imported prefab
};

// `extension` is lowercase without the dot ("png"). Unknown extensions get a neutral "File" type.
FileTypeInfo ClassifyExtension(std::string_view extension);
FileTypeInfo FolderType();

// Draws into [min, max] (a square area). `extension` is printed on the page, upper-cased.
void DrawFolderIcon(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 color, bool hovered);
void DrawFileIcon(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 color, const std::string& extension);
// Small 3D-cube glyph for models that have no preview yet.
void DrawModelGlyph(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 color);

// "1.4 MB"
std::string FormatBytes(unsigned long long bytes);

} // namespace foh
