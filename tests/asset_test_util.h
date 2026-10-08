#pragma once

// Helpers shared by the asset tests: a temp directory, generated glTF/PNG fixtures.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "foh/asset_container.h"
#include "foh/png.h"

#include <doctest/doctest.h>

namespace foh::testutil {

namespace fs = std::filesystem;

struct TempDir {
    TempDir() {
        path = fs::temp_directory_path() / ("foh_asset_test_" + ToString(GenerateAssetId()).substr(0, 12));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};

inline std::string base64(const std::vector<u8>& data) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        const u32 n = (data[i] << 16) | (i + 1 < data.size() ? data[i + 1] << 8 : 0) | (i + 2 < data.size() ? data[i + 2] : 0);
        out += t[(n >> 18) & 63];
        out += t[(n >> 12) & 63];
        out += i + 1 < data.size() ? t[(n >> 6) & 63] : '=';
        out += i + 2 < data.size() ? t[n & 63] : '=';
    }
    return out;
}

inline void writeText(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

inline void writeTexture(const fs::path& p, u32 w, u32 h, u8 r, u8 g, u8 b) {
    std::vector<u8> pixels(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = 255;
    }
    fs::create_directories(p.parent_path());
    REQUIRE(WritePng(p, pixels.data(), w, h, w * 4));
}

// A quad split into two one-triangle primitives with different materials. Both materials
// use albedo.png (one as base color, one as normal map). Nodes: Root > {Child, Child} (the
// duplicate name must be disambiguated), the first child carries the mesh.
inline std::string makeGltf(const std::string& meshName = "Quad", const std::string& rootName = "Root") {
    std::vector<u8> buf;
    auto putFloats = [&](std::initializer_list<f32> v) {
        for (f32 f : v) {
            u8 b[4];
            std::memcpy(b, &f, 4);
            buf.insert(buf.end(), b, b + 4);
        }
    };
    auto putU16 = [&](std::initializer_list<u16> v) {
        for (u16 s : v) {
            buf.push_back(static_cast<u8>(s & 255));
            buf.push_back(static_cast<u8>(s >> 8));
        }
    };
    putFloats({0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0});     // positions   [0, 48)
    putFloats({0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1});     // normals     [48, 96)
    putFloats({0, 1, 1, 1, 1, 0, 0, 0});                 // uvs         [96, 128)
    putU16({0, 1, 2, 0});                                // indices A   [128, 136) (3 used)
    putU16({0, 2, 3, 0});                                // indices B   [136, 144)

    std::string json = R"({
 "asset": {"version": "2.0"},
 "scene": 0,
 "scenes": [{"nodes": [0]}],
 "nodes": [
   {"name": "%ROOT%", "translation": [1, 2, 3], "children": [1, 2]},
   {"name": "Child", "mesh": 0, "rotation": [0, 0.7071068, 0, 0.7071068]},
   {"name": "Child", "scale": [2, 2, 2]}
 ],
 "meshes": [{"name": "%MESH%", "primitives": [
   {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0},
   {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 4, "material": 1}
 ]}],
 "materials": [
   {"name": "Red", "pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1], "metallicFactor": 0.25, "roughnessFactor": 0.75,
                                              "baseColorTexture": {"index": 0}}},
   {"name": "Blue", "normalTexture": {"index": 0, "scale": 2.0}, "doubleSided": true,
    "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 1, 1]}}
 ],
 "textures": [{"source": 0}],
 "images": [{"uri": "albedo.png"}],
 "accessors": [
   {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
   {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
   {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
   {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"},
   {"bufferView": 4, "componentType": 5123, "count": 3, "type": "SCALAR"}
 ],
 "bufferViews": [
   {"buffer": 0, "byteOffset": 0, "byteLength": 48},
   {"buffer": 0, "byteOffset": 48, "byteLength": 48},
   {"buffer": 0, "byteOffset": 96, "byteLength": 32},
   {"buffer": 0, "byteOffset": 128, "byteLength": 6},
   {"buffer": 0, "byteOffset": 136, "byteLength": 6}
 ],
 "buffers": [{"byteLength": 144, "uri": "data:application/octet-stream;base64,%BUFFER%"}]
})";
    auto replace = [&](const std::string& what, const std::string& with) { json.replace(json.find(what), what.size(), with); };
    replace("%ROOT%", rootName);
    replace("%MESH%", meshName);
    replace("%BUFFER%", base64(buf));
    return json;
}


} // namespace foh::testutil
