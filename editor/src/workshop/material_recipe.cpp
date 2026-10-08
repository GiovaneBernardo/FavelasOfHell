#include "material_recipe.h"

#include <algorithm>
#include <cctype>
#include <set>

#include <stb_image.h>

#include "foh/asset_container.h" // ReadFileBytes
#include "foh/png.h"
#include "foh/types.h"
#include "foh/utf8_path.h"

namespace foh {

namespace {

namespace fs = std::filesystem;

std::vector<std::string> tokensOf(const fs::path& file) {
    std::string stem = PathToUtf8(file.stem());
    std::vector<std::string> tokens;
    std::string cur;
    for (unsigned char c : stem) {
        if (std::isalnum(c)) {
            cur += static_cast<char>(std::tolower(c));
        } else if (!cur.empty()) {
            tokens.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

bool isOneOf(const std::string& token, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (token == n) return true;
    return false;
}

struct Decoded {
    u32 w = 0, h = 0;
    std::vector<u8> rgba;
    bool valid() const { return w && h; }
};

Decoded decode(const fs::path& file) {
    Decoded d;
    std::vector<u8> bytes;
    if (file.empty() || !ReadFileBytes(file, bytes) || bytes.empty()) return d;
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp)) return d;
    if (w <= 0 || h <= 0 || static_cast<u64>(w) * static_cast<u64>(h) > (96ull << 20)) return d;
    u8* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (!px) return d;
    d.w = static_cast<u32>(w);
    d.h = static_cast<u32>(h);
    d.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return d;
}

} // namespace

PbrFiles ClassifyPbrFiles(const std::vector<fs::path>& files) {
    PbrFiles out;
    int normalScore = 0;
    for (const fs::path& f : files) {
        std::string ext = PathToUtf8(f.extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".jpg" && ext != ".jpeg" && ext != ".png" && ext != ".tga" && ext != ".bmp") continue;
        // The map type is the LAST word of the name ("Wood096_1K-JPG_Color"): earlier words may be part of the
        // asset's own name ("rough_concrete_diff").
        const std::vector<std::string> t = tokensOf(f);
        if (t.empty()) continue;
        const std::string& last = t.back();
        const std::string prev = t.size() > 1 ? t[t.size() - 2] : std::string();
        if (isOneOf(last, {"normaldx", "nordx"}) || (last == "dx" && isOneOf(prev, {"normal", "nor", "nrm"}))) continue;
        if (isOneOf(last, {"normalgl", "norgl"}) || (last == "gl" && isOneOf(prev, {"normal", "nor", "nrm"}))) {
            out.normal = f;
            normalScore = 2;
        } else if (isOneOf(last, {"normal", "nrm", "nor"})) {
            if (normalScore < 1) {
                out.normal = f;
                normalScore = 1;
            }
        } else if (isOneOf(last, {"arm", "orm"})) out.arm = f;
        else if (isOneOf(last, {"roughness", "rough"})) out.roughness = f;
        else if (isOneOf(last, {"metalness", "metallic", "metal"})) out.metalness = f;
        else if (isOneOf(last, {"ambientocclusion", "ao", "occlusion"})) out.ao = f;
        else if (isOneOf(last, {"emission", "emissive", "emit"})) out.emissive = f;
        else if (isOneOf(last, {"color", "col", "diff", "diffuse", "albedo", "basecolor"})) out.color = f;
    }
    return out;
}

bool PackOrmTexture(const PbrFiles& files, const fs::path& out, std::string& error) {
    const Decoded ao = decode(files.ao), rough = decode(files.roughness), metal = decode(files.metalness);
    u32 w = 0, h = 0;
    for (const Decoded* d : {&ao, &rough, &metal}) {
        w = std::max(w, d->w);
        h = std::max(h, d->h);
    }
    if (!w || !h) {
        error = "no occlusion/roughness/metalness map could be read";
        return false;
    }
    std::vector<u8> px(static_cast<size_t>(w) * h * 4);
    auto sample = [&](const Decoded& d, u32 x, u32 y, u8 fallback) -> u8 {
        if (!d.valid()) return fallback;
        const u32 sx = static_cast<u32>(static_cast<u64>(x) * d.w / w), sy = static_cast<u32>(static_cast<u64>(y) * d.h / h);
        return d.rgba[(static_cast<size_t>(sy) * d.w + sx) * 4]; // grayscale maps: red carries the value
    };
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u8* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = sample(ao, x, y, 255);
            p[1] = sample(rough, x, y, 255);
            p[2] = sample(metal, x, y, 0);
            p[3] = 255;
        }
    }
    if (!WritePng(out, px.data(), w, h, w * 4)) {
        error = "cannot write " + PathToUtf8(out);
        return false;
    }
    return true;
}

} // namespace foh
