#include "file_icons.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace foh {

namespace {

struct Row {
    const char* ext;
    FileCategory category;
    const char* label;
    ImU32 color;
    bool importable;
    bool imagePreview;
    bool modelPreview;
    bool materialPreview = false;
};

constexpr ImU32 kModel = IM_COL32(236, 140, 56, 255);
constexpr ImU32 kImage = IM_COL32(72, 190, 120, 255);
constexpr ImU32 kMaterial = IM_COL32(176, 112, 232, 255);
constexpr ImU32 kPrefab = IM_COL32(64, 156, 255, 255);
constexpr ImU32 kAudio = IM_COL32(232, 96, 140, 255);
constexpr ImU32 kScript = IM_COL32(232, 208, 72, 255);
constexpr ImU32 kShader = IM_COL32(72, 200, 210, 255);
constexpr ImU32 kText = IM_COL32(150, 158, 170, 255);
constexpr ImU32 kArchive = IM_COL32(200, 150, 90, 255);
constexpr ImU32 kGame = IM_COL32(255, 196, 64, 255);

constexpr Row kRows[] = {
    {"gltf", FileCategory::Model, "glTF Model", kModel, true, false, true},
    {"glb", FileCategory::Model, "glTF Binary Model", kModel, true, false, true},
    {"fbx", FileCategory::Model, "FBX Model", kModel, true, false, true},
    {"obj", FileCategory::Model, "OBJ Model", kModel, false, false, false},
    {"blend", FileCategory::Model, "Blender File", kModel, false, false, false},
    {"png", FileCategory::Image, "PNG Texture", kImage, true, true, false},
    {"jpg", FileCategory::Image, "JPEG Texture", kImage, true, true, false},
    {"jpeg", FileCategory::Image, "JPEG Texture", kImage, true, true, false},
    {"tga", FileCategory::Image, "TGA Texture", kImage, true, true, false},
    {"bmp", FileCategory::Image, "BMP Texture", kImage, true, true, false},
    {"gif", FileCategory::Image, "GIF Image", kImage, false, true, false},
    {"hdr", FileCategory::Image, "HDR Image", kImage, false, false, false},
    {"exr", FileCategory::Image, "EXR Image", kImage, false, false, false},
    {"fmat", FileCategory::Material, "Material", kMaterial, false, false, false, true},
    {"fvariant", FileCategory::Prefab, "Prefab Variant", kPrefab, false, false, false},
    {"fprefab", FileCategory::Prefab, "Prefab", kPrefab, false, false, false},
    {"fscene", FileCategory::Scene, "Scene", kPrefab, false, false, false},
    {"fvehicle", FileCategory::GameAsset, "Vehicle", kGame, false, false, false},
    {"fprop", FileCategory::GameAsset, "Prop", kGame, false, false, false},
    {"fchar", FileCategory::GameAsset, "Character", kGame, false, false, false},
    {"wav", FileCategory::Audio, "WAV Audio", kAudio, false, false, false},
    {"ogg", FileCategory::Audio, "OGG Audio", kAudio, false, false, false},
    {"mp3", FileCategory::Audio, "MP3 Audio", kAudio, false, false, false},
    {"flac", FileCategory::Audio, "FLAC Audio", kAudio, false, false, false},
    {"cpp", FileCategory::Script, "C++ Source", kScript, false, false, false},
    {"h", FileCategory::Script, "C++ Header", kScript, false, false, false},
    {"slang", FileCategory::Shader, "Slang Shader", kShader, false, false, false},
    {"hlsl", FileCategory::Shader, "HLSL Shader", kShader, false, false, false},
    {"glsl", FileCategory::Shader, "GLSL Shader", kShader, false, false, false},
    {"json", FileCategory::Text, "JSON", kText, false, false, false},
    {"txt", FileCategory::Text, "Text", kText, false, false, false},
    {"md", FileCategory::Text, "Markdown", kText, false, false, false},
    {"zip", FileCategory::Archive, "ZIP Archive", kArchive, false, false, false},
};

} // namespace

FileTypeInfo ClassifyExtension(std::string_view extension) {
    for (const Row& r : kRows) {
        if (extension == r.ext) {
            return {r.category, r.label, r.color, r.importable, r.imagePreview, r.modelPreview, r.materialPreview};
        }
    }
    FileTypeInfo other;
    other.category = FileCategory::Other;
    other.label = "File";
    other.color = IM_COL32(120, 126, 136, 255);
    return other;
}

FileTypeInfo FolderType() {
    FileTypeInfo f;
    f.category = FileCategory::Folder;
    f.label = "Folder";
    f.color = IM_COL32(232, 190, 96, 255);
    return f;
}

void DrawFolderIcon(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 color, bool hovered) {
    const float w = max.x - min.x, h = max.y - min.y;
    const float s = std::min(w, h);
    const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float fw = s * 0.82f, fh = s * 0.60f;
    const ImVec2 a(c.x - fw * 0.5f, c.y - fh * 0.5f + s * 0.04f), b(c.x + fw * 0.5f, c.y + fh * 0.5f + s * 0.04f);
    const float r = s * 0.06f;
    ImU32 back = IM_COL32((color >> IM_COL32_R_SHIFT & 255) * 0.72f, (color >> IM_COL32_G_SHIFT & 255) * 0.72f,
                          (color >> IM_COL32_B_SHIFT & 255) * 0.72f, 255);
    ImU32 front = hovered ? color : IM_COL32((color >> IM_COL32_R_SHIFT & 255) * 0.92f, (color >> IM_COL32_G_SHIFT & 255) * 0.92f,
                                             (color >> IM_COL32_B_SHIFT & 255) * 0.92f, 255);
    // Back plate with the tab, then the front plate slightly lower.
    dl->AddRectFilled(ImVec2(a.x, a.y - s * 0.08f), ImVec2(a.x + fw * 0.42f, a.y + s * 0.10f), back, r, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(a, b, back, r);
    dl->AddRectFilled(ImVec2(a.x, a.y + fh * 0.16f), b, front, r);
}

void DrawFileIcon(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 color, const std::string& extension) {
    const float w = max.x - min.x, h = max.y - min.y;
    const float s = std::min(w, h);
    const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float pw = s * 0.62f, ph = s * 0.80f;
    const ImVec2 a(c.x - pw * 0.5f, c.y - ph * 0.5f), b(c.x + pw * 0.5f, c.y + ph * 0.5f);
    const float fold = pw * 0.30f;
    const float r = s * 0.04f;
    const ImU32 page = IM_COL32(58, 62, 70, 255);
    const ImU32 edge = IM_COL32(94, 100, 112, 255);
    // Page with a clipped top-right corner.
    const ImVec2 pts[] = {a, ImVec2(b.x - fold, a.y), ImVec2(b.x, a.y + fold), b, ImVec2(a.x, b.y)};
    dl->AddConvexPolyFilled(pts, 5, page);
    dl->AddPolyline(pts, 5, edge, ImDrawFlags_Closed, 1.0f);
    dl->AddTriangleFilled(ImVec2(b.x - fold, a.y), ImVec2(b.x - fold, a.y + fold), ImVec2(b.x, a.y + fold), edge);
    // Colored band across the lower part carries the extension.
    const float bandH = ph * 0.30f;
    const ImVec2 bandA(a.x, b.y - bandH - ph * 0.10f), bandB(b.x, b.y - ph * 0.10f);
    dl->AddRectFilled(bandA, bandB, color, r);
    std::string label = extension;
    std::transform(label.begin(), label.end(), label.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (label.size() > 5) label.resize(5);
    if (!label.empty()) {
        ImFont* font = ImGui::GetFont();
        float fs = bandH * 0.72f;
        ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0.0f, label.c_str());
        if (ts.x > pw * 0.88f) {
            fs *= pw * 0.88f / ts.x;
            ts = font->CalcTextSizeA(fs, 1e9f, 0.0f, label.c_str());
        }
        dl->AddText(font, fs, ImVec2(c.x - ts.x * 0.5f, (bandA.y + bandB.y) * 0.5f - ts.y * 0.5f), IM_COL32(16, 18, 22, 255),
                    label.c_str());
    }
}

void DrawModelGlyph(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 color) {
    const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float s = std::min(max.x - min.x, max.y - min.y) * 0.32f;
    const ImVec2 top[] = {ImVec2(c.x, c.y - s), ImVec2(c.x + s * 0.87f, c.y - s * 0.5f), ImVec2(c.x, c.y), ImVec2(c.x - s * 0.87f, c.y - s * 0.5f)};
    const ImVec2 left[] = {ImVec2(c.x - s * 0.87f, c.y - s * 0.5f), ImVec2(c.x, c.y), ImVec2(c.x, c.y + s), ImVec2(c.x - s * 0.87f, c.y + s * 0.5f)};
    const ImVec2 right[] = {ImVec2(c.x, c.y), ImVec2(c.x + s * 0.87f, c.y - s * 0.5f), ImVec2(c.x + s * 0.87f, c.y + s * 0.5f), ImVec2(c.x, c.y + s)};
    auto shade = [&](float k) {
        return IM_COL32((color >> IM_COL32_R_SHIFT & 255) * k, (color >> IM_COL32_G_SHIFT & 255) * k, (color >> IM_COL32_B_SHIFT & 255) * k, 255);
    };
    ImDrawList* d = dl;
    d->AddConvexPolyFilled(top, 4, shade(1.0f));
    d->AddConvexPolyFilled(left, 4, shade(0.62f));
    d->AddConvexPolyFilled(right, 4, shade(0.80f));
}

std::string FormatBytes(unsigned long long bytes) {
    char buf[32];
    if (bytes < 1024) std::snprintf(buf, sizeof buf, "%llu B", bytes);
    else if (bytes < 1024ull * 1024) std::snprintf(buf, sizeof buf, "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

} // namespace foh
