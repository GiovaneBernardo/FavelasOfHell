#include "import_util.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO // we read files ourselves: unicode paths, and bytes may come from a .glb
#include <stb_image.h>

#include "foh/asset_container.h"

namespace foh::import_util {

std::string uniqueKey(std::set<std::string>& used, std::string base, const std::string& fallback) {
    if (base.empty()) base = fallback;
    std::string key = base;
    for (u32 n = 2; used.contains(key); ++n) key = base + "#" + std::to_string(n);
    used.insert(key);
    return key;
}

std::string sanitizeName(std::string name) {
    std::replace(name.begin(), name.end(), '/', '_');
    return name;
}

std::string lowerExtension(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

void generateNormals(ImportedMesh& mesh) {
    for (ImportedVertex& v : mesh.vertices) v.normal = Vec3(0.0f);
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        ImportedVertex& a = mesh.vertices[mesh.indices[i]];
        ImportedVertex& b = mesh.vertices[mesh.indices[i + 1]];
        ImportedVertex& c = mesh.vertices[mesh.indices[i + 2]];
        // Unnormalized cross product = twice the area: bigger triangles weigh more.
        const Vec3 n = glm::cross(b.position - a.position, c.position - a.position);
        a.normal += n;
        b.normal += n;
        c.normal += n;
    }
    for (ImportedVertex& v : mesh.vertices) {
        const f32 len = glm::length(v.normal);
        v.normal = len > 1e-12f ? v.normal / len : Vec3(0.0f, 1.0f, 0.0f);
    }
}

void generateTangents(ImportedMesh& mesh) {
    std::vector<Vec3> tan(mesh.vertices.size(), Vec3(0.0f)), bitan(mesh.vertices.size(), Vec3(0.0f));
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const u32 i0 = mesh.indices[i], i1 = mesh.indices[i + 1], i2 = mesh.indices[i + 2];
        const ImportedVertex &a = mesh.vertices[i0], &b = mesh.vertices[i1], &c = mesh.vertices[i2];
        const Vec3 e1 = b.position - a.position, e2 = c.position - a.position;
        const Vec2 d1 = b.uv0 - a.uv0, d2 = c.uv0 - a.uv0;
        const f32 det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-20f) continue;
        const f32 inv = 1.0f / det;
        const Vec3 t = (e1 * d2.y - e2 * d1.y) * inv;
        const Vec3 bt = (e2 * d1.x - e1 * d2.x) * inv;
        for (u32 idx : {i0, i1, i2}) {
            tan[idx] += t;
            bitan[idx] += bt;
        }
    }
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        ImportedVertex& v = mesh.vertices[i];
        // Gram-Schmidt against the normal; fall back to any perpendicular axis for degenerate UVs.
        Vec3 t = tan[i] - v.normal * glm::dot(v.normal, tan[i]);
        if (glm::length(t) < 1e-8f) {
            const Vec3 axis = std::abs(v.normal.x) < 0.9f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
            t = axis - v.normal * glm::dot(v.normal, axis);
        }
        t = glm::normalize(t);
        const f32 handed = glm::dot(glm::cross(v.normal, t), bitan[i]) < 0.0f ? -1.0f : 1.0f;
        v.tangent = Vec4(t, handed);
    }
}

void StbFree::operator()(u8* p) const { stbi_image_free(p); }

bool decodeImage(const u8* data, size_t size, DecodedImage& out, std::string& error) {
    if (size > static_cast<size_t>(INT32_MAX)) {
        error = "image too large";
        return false;
    }
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
    if (!pixels) {
        error = std::string("cannot decode image: ") + stbi_failure_reason();
        return false;
    }
    out.width = static_cast<u32>(w);
    out.height = static_cast<u32>(h);
    out.pixels.reset(pixels);
    return true;
}

bool probeImage(const u8* data, size_t size, std::string& error) {
    int w = 0, h = 0, channels = 0;
    if (size > static_cast<size_t>(INT32_MAX) || !stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &channels)) {
        error = std::string("unsupported or corrupt image: ") + stbi_failure_reason();
        return false;
    }
    if (w <= 0 || h <= 0 || w > (1 << 15) || h > (1 << 15)) {
        error = "image size out of range (" + std::to_string(w) + "x" + std::to_string(h) + ")";
        return false;
    }
    return true;
}

void weldVertices(ImportedMesh& mesh) {
    struct Hash {
        size_t operator()(const ImportedVertex& v) const { return static_cast<size_t>(HashBytes(&v, sizeof(v))); }
    };
    struct Eq {
        bool operator()(const ImportedVertex& a, const ImportedVertex& b) const {
            return std::memcmp(&a, &b, sizeof(a)) == 0;
        }
    };
    std::unordered_map<ImportedVertex, u32, Hash, Eq> seen;
    std::vector<ImportedVertex> unique;
    seen.reserve(mesh.vertices.size());
    for (u32& index : mesh.indices) {
        const ImportedVertex& v = mesh.vertices[index];
        const auto [it, inserted] = seen.try_emplace(v, static_cast<u32>(unique.size()));
        if (inserted) unique.push_back(v);
        index = it->second;
    }
    mesh.vertices = std::move(unique);
}

} // namespace foh::import_util
