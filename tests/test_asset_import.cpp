#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <fstream>

#include "foh/asset_container.h"
#include "foh/asset_db.h"
#include "foh/asset_import.h"
#include "foh/asset_types.h"
#include "foh/png.h"
#include "foh/utf8_path.h"
#include "asset_test_util.h"

using namespace foh;
using namespace foh::testutil;
namespace fs = std::filesystem;

namespace {

AssetDbDesc dbFor(const TempDir& t) { return {t.path / "assets", t.path / "cache"}; }

// Writes the model and its texture straight into the source root.
void writeModel(const TempDir& t, const std::string& rel, const std::string& gltf) {
    writeText(t.path / "assets" / rel, gltf);
    writeTexture((t.path / "assets" / rel).parent_path() / "albedo.png", 8, 8, 200, 100, 50);
}

} // namespace

// ---- container -----------------------------------------------------------------------------

TEST_CASE("XXH64 matches the reference values") {
    CHECK(HashBytes("", 0) == 0xEF46DB3751D8E999ull);
    CHECK(HashBytes("a", 1) == 0xD24EC4F1A98C6E5Bull);
    // Long input exercises the 32-byte lane loop; the value only has to be stable and sensitive.
    std::vector<u8> data(1000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<u8>(i * 7);
    const u64 h = HashBytes(data);
    data[500] ^= 1;
    CHECK(HashBytes(data) != h);
}

TEST_CASE("AssetId text round trip and deterministic derivation") {
    const AssetId a = GenerateAssetId();
    AssetId parsed;
    REQUIRE(ParseAssetId(ToString(a), parsed));
    CHECK(parsed == a);
    CHECK_FALSE(ParseAssetId("not-an-id", parsed));
    CHECK(DeriveAssetId(a, "mesh:Wall") == DeriveAssetId(a, "mesh:Wall"));
    CHECK_FALSE(DeriveAssetId(a, "mesh:Wall") == DeriveAssetId(a, "mesh:Door"));
    CHECK_FALSE(DeriveAssetId(a, "mesh:Wall") == DeriveAssetId(GenerateAssetId(), "mesh:Wall"));
}

TEST_CASE("container round trip, compression and tamper detection") {
    const AssetId id = GenerateAssetId();
    std::vector<u8> compressible(4096, 7), random(300);
    for (size_t i = 0; i < random.size(); ++i) random[i] = static_cast<u8>((i * 2654435761u) >> 13);

    ContainerWriter w(FourCC('T', 'E', 'S', 'T'), 3, id);
    w.addChunk(FourCC('A', 'A', 'A', 'A'), compressible, ChunkCodec::Zstd);
    w.addChunk(FourCC('B', 'B', 'B', 'B'), compressible, ChunkCodec::Lz4);
    w.addChunk(FourCC('C', 'C', 'C', 'C'), random);
    w.addChunk(FourCC('E', 'E', 'E', 'E'), std::vector<u8>{});
    const std::vector<u8> file = w.finish();

    auto reader = ContainerReader::Open(file);
    REQUIRE(reader);
    CHECK(reader->typeId() == FourCC('T', 'E', 'S', 'T'));
    CHECK(reader->typeVersion() == 3);
    CHECK(reader->id() == id);
    CHECK(reader->findChunk(FourCC('A', 'A', 'A', 'A'))->codec == ChunkCodec::Zstd);
    CHECK(reader->findChunk(FourCC('A', 'A', 'A', 'A'))->storedSize < 100);
    CHECK(reader->findChunk(FourCC('C', 'C', 'C', 'C'))->codec == ChunkCodec::None);
    CHECK(reader->findChunk(FourCC('C', 'C', 'C', 'C'))->offset % 64 == 0);

    std::vector<u8> out;
    CHECK(reader->readChunk(FourCC('A', 'A', 'A', 'A'), out));
    CHECK(out == compressible);
    CHECK(reader->readChunk(FourCC('B', 'B', 'B', 'B'), out));
    CHECK(out == compressible);
    CHECK(reader->readChunk(FourCC('C', 'C', 'C', 'C'), out));
    CHECK(out == random);
    CHECK(reader->readChunk(FourCC('E', 'E', 'E', 'E'), out));
    CHECK(out.empty());
    CHECK_FALSE(reader->readChunk(FourCC('Z', 'Z', 'Z', 'Z'), out));

    // Bit flip in a stored chunk: the per-chunk hash must catch it.
    {
        std::vector<u8> bad = file;
        bad[reader->findChunk(FourCC('C', 'C', 'C', 'C'))->offset + 10] ^= 0x40;
        auto r = ContainerReader::Open(bad);
        REQUIRE(r);
        CHECK_FALSE(r->readChunk(FourCC('C', 'C', 'C', 'C'), out));
    }
    // Structural damage is rejected on open instead of crashing later.
    std::string error;
    {
        std::vector<u8> bad = file;
        bad.resize(bad.size() - 10);
        CHECK_FALSE(ContainerReader::Open(bad, &error));
    }
    {
        std::vector<u8> bad = file;
        bad[0] = 'X';
        CHECK_FALSE(ContainerReader::Open(bad, &error));
    }
    {
        std::vector<u8> bad = file;
        bad[40] ^= 0xFF; // table offset
        CHECK_FALSE(ContainerReader::Open(bad, &error));
    }
    CHECK_FALSE(ContainerReader::Open(std::vector<u8>(10), &error));
}

// ---- asset types ---------------------------------------------------------------------------

TEST_CASE("mesh, material, texture, prefab and variant round trip") {
    const AssetId id = GenerateAssetId();
    {
        MeshAsset m;
        m.name = "Wall";
        m.streams = {{VertexSemantic::Position, VertexFormat::Float3, 0}, {VertexSemantic::Uv0, VertexFormat::Float2, 12}};
        m.vertexStride = 20;
        m.vertexData.assign(3 * 20, 9);
        m.indices = {0, 1, 2, 2, 1, 0};
        m.subMeshes = {{0, 3, 0, Vec3(0), Vec3(1)}, {3, 3, 1, Vec3(-1), Vec3(2)}};
        m.materialSlotCount = 2;
        m.boundsMax = Vec3(2);
        auto reader = ContainerReader::Open(SerializeMesh(m, id));
        REQUIRE(reader);
        MeshAsset out;
        REQUIRE(DeserializeMesh(*reader, out));
        CHECK(out.name == "Wall");
        CHECK(out.vertexCount() == 3);
        CHECK(out.indices == m.indices);
        CHECK(out.subMeshes.size() == 2);
        CHECK(out.subMeshes[1].materialSlot == 1);
        CHECK(out.findStream(VertexSemantic::Uv0)->offset == 12);
        CHECK(out.findStream(VertexSemantic::Tangent) == nullptr);

        // An index past the vertex count is a corrupt file, not something to draw.
        m.indices[0] = 99;
        auto bad = ContainerReader::Open(SerializeMesh(m, id));
        REQUIRE(bad);
        std::string error;
        CHECK_FALSE(DeserializeMesh(*bad, out, &error));
        CHECK_FALSE(error.empty());
    }
    {
        MaterialAsset m;
        m.name = "Paint";
        m.baseColor = Vec4(0.1f, 0.2f, 0.3f, 0.4f);
        m.metallic = 0.5f;
        m.alphaMode = AlphaMode::Mask;
        m.doubleSided = true;
        m.baseColorTexture = {GenerateAssetId(), 1};
        auto reader = ContainerReader::Open(SerializeMaterial(m, id));
        REQUIRE(reader);
        MaterialAsset out;
        REQUIRE(DeserializeMaterial(*reader, out));
        CHECK(out.baseColor == m.baseColor);
        CHECK(out.alphaMode == AlphaMode::Mask);
        CHECK(out.doubleSided);
        CHECK(out.baseColorTexture.texture == m.baseColorTexture.texture);
        CHECK(out.baseColorTexture.uvSet == 1);
        CHECK_FALSE(out.normalTexture.isValid());
        const auto deps = ReadDependencies(*reader);
        REQUIRE(deps.size() == 1);
        CHECK(deps[0] == m.baseColorTexture.texture);
    }
    {
        std::vector<u8> pixels(4 * 4 * 4, 128);
        TextureAsset t;
        t.name = "Tex";
        t.role = TextureRole::Normal;
        t.format = TexelFormat::Rgba8Unorm;
        t.width = t.height = 4;
        t.mips = {{4, 4, pixels}, {2, 2, std::vector<u8>(16, 1)}, {1, 1, std::vector<u8>(4, 2)}};
        auto reader = ContainerReader::Open(SerializeTexture(t, id));
        REQUIRE(reader);
        TextureAsset out;
        REQUIRE(DeserializeTexture(*reader, out));
        CHECK(out.mips.size() == 3);
        CHECK(out.role == TextureRole::Normal);
        CHECK(out.mips[1].width == 2);
        REQUIRE(DeserializeTexture(*reader, out, 1)); // streaming: skip the top mip
        CHECK(out.width == 2);
        CHECK(out.mips.size() == 2);
        CHECK(out.mips[0].data == std::vector<u8>(16, 1));
    }
    {
        PrefabAsset p;
        p.name = "House";
        p.nodes.push_back({"Root", -1, Vec3(1, 2, 3), Quat(1, 0, 0, 0), Vec3(1), {}, {}});
        p.nodes.push_back({"Wall", 0, Vec3(0), Quat(1, 0, 0, 0), Vec3(2), GenerateAssetId(), {GenerateAssetId(), AssetId{}}});
        auto reader = ContainerReader::Open(SerializePrefab(p, id));
        REQUIRE(reader);
        PrefabAsset out;
        REQUIRE(DeserializePrefab(*reader, out));
        REQUIRE(out.nodes.size() == 2);
        CHECK(out.nodes[1].mesh == p.nodes[1].mesh);
        CHECK(out.nodes[1].materials.size() == 2);
        CHECK(out.nodePath(1) == "Root/Wall");
        CHECK(out.findNode("Root/Wall") == 1);
        CHECK(out.findNode("Root/Nope") == -1);
        CHECK(ReadDependencies(*reader).size() == 2); // mesh + the one valid material
    }
    {
        PrefabVariantAsset v;
        v.name = "RedHouse";
        v.base = GenerateAssetId();
        PrefabOverride o;
        o.nodePath = "Root/Wall";
        o.flags = PrefabOverride::SetMaterial;
        o.material = GenerateAssetId();
        v.overrides.push_back(o);
        auto reader = ContainerReader::Open(SerializePrefabVariant(v, id));
        REQUIRE(reader);
        PrefabVariantAsset out;
        REQUIRE(DeserializePrefabVariant(*reader, out));
        CHECK(out.base == v.base);
        REQUIRE(out.overrides.size() == 1);
        CHECK(out.overrides[0].material == o.material);
    }
}

TEST_CASE("prefab variants apply overrides, removals and report orphans") {
    PrefabAsset base;
    base.name = "Base";
    const AssetId mat0 = GenerateAssetId(), mat1 = GenerateAssetId();
    base.nodes.push_back({"Root", -1, Vec3(0), Quat(1, 0, 0, 0), Vec3(1), {}, {}});
    base.nodes.push_back({"A", 0, Vec3(0), Quat(1, 0, 0, 0), Vec3(1), GenerateAssetId(), {mat0}});
    base.nodes.push_back({"A1", 1, Vec3(0), Quat(1, 0, 0, 0), Vec3(1), {}, {}});
    base.nodes.push_back({"B", 0, Vec3(0), Quat(1, 0, 0, 0), Vec3(1), GenerateAssetId(), {mat0}});

    PrefabVariantAsset v;
    v.name = "Variant";
    PrefabOverride swap;
    swap.nodePath = "Root/B";
    swap.flags = PrefabOverride::SetMaterial | PrefabOverride::SetTransform;
    swap.material = mat1;
    swap.position = Vec3(4, 5, 6);
    PrefabOverride remove;
    remove.nodePath = "Root/A";
    remove.flags = PrefabOverride::Remove;
    PrefabOverride gone;
    gone.nodePath = "Root/Deleted";
    gone.flags = PrefabOverride::SetTransform;
    PrefabOverride badSlot;
    badSlot.nodePath = "Root/B";
    badSlot.flags = PrefabOverride::SetMaterial;
    badSlot.materialSlot = 5;
    v.overrides = {swap, remove, gone, badSlot};

    const VariantApplyResult r = ApplyVariant(base, v);
    REQUIRE(r.prefab.nodes.size() == 2); // A and its child A1 are gone
    CHECK(r.prefab.nodes[1].name == "B");
    CHECK(r.prefab.nodes[1].parent == 0);
    CHECK(r.prefab.nodes[1].position == Vec3(4, 5, 6));
    CHECK(r.prefab.nodes[1].materials[0] == mat1);
    REQUIRE(r.orphaned.size() == 2);
    CHECK(r.orphaned[0] == "Root/Deleted");
    CHECK(r.orphaned[1] == "Root/B[slot 5]");
    CHECK(base.nodes.size() == 4); // the base is untouched
}

// ---- textures ------------------------------------------------------------------------------

TEST_CASE("mip chains and color-space aware filtering") {
    std::vector<u8> bw(4 * 4 * 4);
    for (size_t i = 0; i < 16; ++i) {
        const u8 v = (i % 2 == 0) ? 0 : 255; // alternating black/white columns
        bw[i * 4] = bw[i * 4 + 1] = bw[i * 4 + 2] = v;
        bw[i * 4 + 3] = 255;
    }
    auto srgb = BuildMipChain(bw.data(), 4, 4, true);
    auto linear = BuildMipChain(bw.data(), 4, 4, false);
    REQUIRE(srgb.size() == 3);
    CHECK(srgb[1].width == 2);
    CHECK(srgb[2].width == 1);
    CHECK(srgb[2].data.size() == 4);
    // Half black, half white: 50% light output is sRGB 188, not 128.
    CHECK(std::abs(int(srgb[1].data[0]) - 188) <= 1);
    CHECK(std::abs(int(linear[1].data[0]) - 128) <= 1);
    CHECK(srgb[1].data[3] == 255);

    auto odd = BuildMipChain(bw.data(), 4, 1, false); // non-square: 4x1 -> 2x1 -> 1x1
    REQUIRE(odd.size() == 3);
    CHECK(odd[1].height == 1);
}

TEST_CASE("texture role guessing from file names") {
    CHECK(GuessTextureRole("brick_albedo.png") == TextureRole::Color);
    CHECK(GuessTextureRole("brick_n.png") == TextureRole::Normal);
    CHECK(GuessTextureRole("Brick_Normal.png") == TextureRole::Normal);
    CHECK(GuessTextureRole("brick_roughness.png") == TextureRole::Data);
    CHECK(GuessTextureRole("brick_ORM.png") == TextureRole::Data);
}

// ---- importing -----------------------------------------------------------------------------

TEST_CASE("glTF import creates meshes, materials, textures per role and a prefab") {
    TempDir t;
    writeModel(t, "models/quad.gltf", makeGltf());
    AssetDb db(dbFor(t));
    const ImportResult r = db.importFile("models/quad.gltf");
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK_FALSE(r.upToDate);
    CHECK(r.prefab.isValid());

    // 1 mesh, 2 materials, one texture per (image, role) = color + normal, 1 prefab.
    CHECK(db.assetsOfKind(AssetKind::Mesh).size() == 1);
    CHECK(db.assetsOfKind(AssetKind::Material).size() == 2);
    CHECK(db.assetsOfKind(AssetKind::Texture).size() == 2);
    CHECK(db.assetsOfKind(AssetKind::Prefab).size() == 1);
    CHECK(r.assets.size() == 6);
    CHECK(r.changed.size() == 6);

    MeshAsset mesh;
    std::string error;
    REQUIRE(db.load(db.assetsOfKind(AssetKind::Mesh)[0], mesh, &error));
    CHECK(mesh.name == "Quad");
    CHECK(mesh.subMeshes.size() == 2);
    CHECK(mesh.materialSlotCount == 2);
    CHECK(mesh.vertexCount() == 8); // two primitives of 4 vertices
    CHECK(mesh.indices.size() == 6);
    CHECK(mesh.indices[3] == 4); // the second primitive's indices were offset by the first one's vertices
    CHECK(mesh.boundsMin == Vec3(0, 0, 0));
    CHECK(mesh.boundsMax == Vec3(1, 1, 0));
    REQUIRE(mesh.findStream(VertexSemantic::Tangent));
    const auto* v = reinterpret_cast<const CookedVertex*>(mesh.vertexData.data());
    CHECK(v[2].uv0 == Vec2(1, 0));
    CHECK(v[0].normal == Vec3(0, 0, 1));
    CHECK(std::abs(glm::length(Vec3(v[0].tangent)) - 1.0f) < 1e-4f); // generated from the UVs

    // Materials keep their factors and point at texture assets of the right role.
    AssetId red, blue;
    for (const AssetId& id : db.assetsOfKind(AssetKind::Material)) {
        MaterialAsset m;
        REQUIRE(db.load(id, m, &error));
        (m.name == "Red" ? red : blue) = id;
    }
    MaterialAsset redMat, blueMat;
    REQUIRE(db.load(red, redMat));
    REQUIRE(db.load(blue, blueMat));
    CHECK(redMat.baseColor == Vec4(1, 0, 0, 1));
    CHECK(redMat.metallic == doctest::Approx(0.25f));
    CHECK(redMat.roughness == doctest::Approx(0.75f));
    CHECK(blueMat.doubleSided);
    CHECK(blueMat.normalScale == doctest::Approx(2.0f));
    REQUIRE(redMat.baseColorTexture.isValid());
    REQUIRE(blueMat.normalTexture.isValid());
    CHECK_FALSE(redMat.baseColorTexture.texture == blueMat.normalTexture.texture);

    TextureAsset color, normal;
    REQUIRE(db.load(redMat.baseColorTexture.texture, color, 0, &error));
    REQUIRE(db.load(blueMat.normalTexture.texture, normal, 0, &error));
    CHECK(color.format == TexelFormat::Bc7Srgb); // 8x8 is a multiple of 4: block compressed
    CHECK(normal.format == TexelFormat::Bc5Unorm);
    CHECK(color.role == TextureRole::Color);
    CHECK(normal.role == TextureRole::Normal);
    CHECK(color.mips.size() == 4); // 8x8 down to 1x1
    CHECK(color.mips[0].data.size() == 4 * 16); // 2x2 blocks of 16 bytes

    // The prefab: Root > Child (mesh, rotated), Child#2 (scaled).
    PrefabAsset prefab;
    REQUIRE(db.load(r.prefab, prefab, &error));
    REQUIRE(prefab.nodes.size() == 3);
    CHECK(prefab.nodePath(1) == "Root/Child");
    CHECK(prefab.nodePath(2) == "Root/Child#2");
    CHECK(prefab.nodes[0].position == Vec3(1, 2, 3));
    CHECK(prefab.nodes[2].scale == Vec3(2));
    CHECK(prefab.nodes[1].mesh.isValid());
    CHECK_FALSE(prefab.nodes[2].mesh.isValid());
    CHECK(prefab.nodes[1].materials == std::vector<AssetId>{red, blue});
    CHECK(std::abs(prefab.nodes[1].rotation.y - 0.7071068f) < 1e-5f);

    // The dependency graph: prefab -> mesh + materials -> textures.
    const AssetId meshId = db.assetsOfKind(AssetKind::Mesh)[0];
    CHECK(db.dependencies(r.prefab).size() == 3);
    CHECK(db.dependents(meshId) == std::vector<AssetId>{r.prefab});
    const auto texDependents = db.dependents(redMat.baseColorTexture.texture);
    REQUIRE(texDependents.size() == 1);
    CHECK(texDependents[0] == red);

    // A meta file sits next to the source.
    CHECK(fs::exists(t.path / "assets/models/quad.gltf.meta"));
}

TEST_CASE("reimporting keeps GUIDs, skips unchanged work and rewrites only what changed") {
    TempDir t;
    writeModel(t, "models/quad.gltf", makeGltf());
    AssetDb db(dbFor(t));
    const ImportResult first = db.importFile("models/quad.gltf");
    REQUIRE(first.ok);

    const ImportResult again = db.importFile("models/quad.gltf");
    CHECK(again.ok);
    CHECK(again.upToDate);
    CHECK(again.changed.empty());
    CHECK(again.assets == first.assets);

    // Only the root node is renamed: the prefab changes, meshes/materials/textures are untouched.
    writeText(t.path / "assets/models/quad.gltf", makeGltf("Quad", "House"));
    const auto changed = db.reimportChanged();
    REQUIRE(changed.size() == 1);
    REQUIRE(changed[0].ok);
    CHECK(changed[0].assets == first.assets);
    REQUIRE(changed[0].changed.size() == 1);
    CHECK(changed[0].changed[0] == first.prefab);
    CHECK(db.reimportChanged().empty()); // nothing left to do

    // A fresh database finds everything again from the meta files, with the same ids.
    AssetDb reopened(dbFor(t));
    reopened.scan();
    CHECK(reopened.assetsOfKind(AssetKind::Mesh) == db.assetsOfKind(AssetKind::Mesh));
    const ImportResult third = reopened.importFile("models/quad.gltf");
    CHECK(third.upToDate);
    CHECK(third.assets == first.assets);
    PrefabAsset prefab;
    REQUIRE(reopened.load(first.prefab, prefab));
    CHECK(prefab.nodes[0].name == "House");

    // Losing the whole cache is harmless: the next import rebuilds it under the same ids.
    fs::remove_all(t.path / "cache");
    const ImportResult rebuilt = reopened.importFile("models/quad.gltf");
    REQUIRE(rebuilt.ok);
    CHECK_FALSE(rebuilt.upToDate);
    CHECK(rebuilt.assets == first.assets);
    MeshAsset mesh;
    CHECK(reopened.load(reopened.assetsOfKind(AssetKind::Mesh)[0], mesh));
}

TEST_CASE("a renamed mesh is tombstoned and can be remapped to keep its identity") {
    TempDir t;
    writeModel(t, "quad.gltf", makeGltf("Quad"));
    AssetDb db(dbFor(t));
    const ImportResult first = db.importFile("quad.gltf");
    REQUIRE(first.ok);
    const AssetId oldMesh = db.assetsOfKind(AssetKind::Mesh)[0];

    writeText(t.path / "assets/quad.gltf", makeGltf("Quad_renamed"));
    const ImportResult second = db.importFile("quad.gltf");
    REQUIRE(second.ok);
    const auto meshes = db.assetsOfKind(AssetKind::Mesh);
    REQUIRE(meshes.size() == 1);
    CHECK_FALSE(meshes[0] == oldMesh); // a new key is a new identity...
    const SourceRecord* record = db.findSource("quad.gltf");
    REQUIRE(record);
    const auto tombstone = std::find_if(record->subAssets.begin(), record->subAssets.end(),
                                        [](const SubAssetRecord& s) { return s.tombstone; });
    REQUIRE(tombstone != record->subAssets.end()); // ...and the old one is remembered
    CHECK(tombstone->guid == oldMesh);
    CHECK(db.find(oldMesh) == nullptr);

    // The artist says "that is the same mesh": references to the old id work again.
    const ImportResult remapped = db.remapSubAsset("quad.gltf", "Quad", "Quad_renamed");
    INFO(remapped.error);
    REQUIRE(remapped.ok);
    CHECK(db.assetsOfKind(AssetKind::Mesh) == std::vector<AssetId>{oldMesh});
    MeshAsset mesh;
    CHECK(db.load(oldMesh, mesh));
    PrefabAsset prefab;
    REQUIRE(db.load(remapped.prefab, prefab));
    CHECK(prefab.nodes[1].mesh == oldMesh);
    const SourceRecord* after = db.findSource("quad.gltf");
    CHECK(std::none_of(after->subAssets.begin(), after->subAssets.end(), [](const SubAssetRecord& s) { return s.tombstone; }));
}

TEST_CASE("import settings: no prefab for kit pieces, texture size cap, no mips") {
    TempDir t;
    writeModel(t, "kit.gltf", makeGltf());
    AssetDb db(dbFor(t));
    REQUIRE(db.importFile("kit.gltf").ok);
    CHECK(db.assetsOfKind(AssetKind::Prefab).size() == 1);

    ImportSettings s;
    s.generatePrefab = false;
    s.maxTextureSize = 4;
    s.generateMips = false;
    const ImportResult r = db.setImportSettings("kit.gltf", s);
    REQUIRE(r.ok);
    CHECK_FALSE(r.prefab.isValid());
    CHECK(db.assetsOfKind(AssetKind::Prefab).empty());
    CHECK(db.assetsOfKind(AssetKind::Mesh).size() == 1);

    TextureAsset tex;
    REQUIRE(db.load(db.assetsOfKind(AssetKind::Texture)[0], tex));
    CHECK(tex.width == 4); // the 8x8 source was reduced
    CHECK(tex.mips.size() == 1);

    // The settings are persisted in the meta file.
    AssetDb reopened(dbFor(t));
    reopened.scan();
    REQUIRE(reopened.findSource("kit.gltf"));
    CHECK(reopened.findSource("kit.gltf")->settings == s);
}

TEST_CASE("files dropped from outside the project are copied in with what they reference") {
    TempDir t;       // the project
    TempDir outside; // e.g. the Downloads folder
    writeText(outside.path / "downloads/house/scene.gltf", makeGltf());
    writeTexture(outside.path / "downloads/house/albedo.png", 8, 8, 10, 20, 30);

    AssetDb db(dbFor(t));
    const ImportResult r = db.importFile(outside.path / "downloads/house/scene.gltf");
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK(r.sourcePath == "imported/scene/scene.gltf");
    CHECK(fs::exists(t.path / "assets/imported/scene/scene.gltf"));
    CHECK(fs::exists(t.path / "assets/imported/scene/albedo.png"));
    CHECK(fs::exists(t.path / "assets/imported/scene/scene.gltf.meta"));
    CHECK(db.assetsOfKind(AssetKind::Mesh).size() == 1);

    // Dropping it again is a reimport of the same source, not a duplicate.
    const ImportResult again = db.importFile(outside.path / "downloads/house/scene.gltf");
    REQUIRE(again.ok);
    CHECK(again.source == r.source);
    CHECK(again.assets == r.assets);
    CHECK(db.sources().size() == 1);
}

TEST_CASE("standalone textures take their role from the file name or the setting") {
    TempDir t;
    writeTexture(t.path / "assets/bricks_n.png", 16, 16, 128, 128, 255);
    writeTexture(t.path / "assets/bricks_albedo.png", 16, 16, 128, 64, 32);
    AssetDb db(dbFor(t));
    const ImportResult n = db.importFile("bricks_n.png");
    const ImportResult c = db.importFile("bricks_albedo.png");
    REQUIRE(n.ok);
    REQUIRE(c.ok);
    CHECK_FALSE(n.prefab.isValid());
    TextureAsset tn, tc;
    REQUIRE(db.load(n.assets[0], tn));
    REQUIRE(db.load(c.assets[0], tc));
    CHECK(tn.role == TextureRole::Normal);
    CHECK(tn.format == TexelFormat::Bc5Unorm);
    CHECK(tc.role == TextureRole::Color);
    CHECK(tc.format == TexelFormat::Bc7Srgb);
    CHECK(tc.mips.size() == 5);

    ImportSettings s;
    s.textureRole = "data";
    const ImportResult forced = db.setImportSettings("bricks_albedo.png", s);
    REQUIRE(forced.ok);
    CHECK(forced.assets == c.assets); // the role change did not change the identity
    REQUIRE(db.load(c.assets[0], tc));
    CHECK(tc.role == TextureRole::Data);
}

TEST_CASE("broken sources fail cleanly and leave the database untouched") {
    TempDir t;
    writeText(t.path / "assets/bad.gltf", "{ this is not json");
    writeText(t.path / "assets/bad.png", "definitely not a png");
    writeText(t.path / "assets/notes.txt", "hello");
    AssetDb db(dbFor(t));
    CHECK_FALSE(db.importFile("bad.gltf").ok);
    CHECK_FALSE(db.importFile("bad.png").ok);
    CHECK_FALSE(db.importFile("notes.txt").ok);
    CHECK_FALSE(db.importFile("missing.gltf").ok);
    CHECK(db.sources().empty());
    CHECK_FALSE(fs::exists(t.path / "assets/bad.gltf.meta"));

    // A glTF whose buffer file is missing: error, not a crash.
    std::string json = makeGltf();
    const size_t at = json.find("data:application/octet-stream;base64,");
    json.replace(at, json.find('"', at) - at, "missing.bin");
    writeText(t.path / "assets/nobin.gltf", json);
    CHECK_FALSE(db.importFile("nobin.gltf").ok);
}

TEST_CASE("prefab variants are saved, indexed, found again and resolved") {
    TempDir t;
    writeModel(t, "quad.gltf", makeGltf());
    AssetDb db(dbFor(t));
    const ImportResult r = db.importFile("quad.gltf");
    REQUIRE(r.ok);
    const AssetId blue = [&] {
        for (const AssetId& id : db.assetsOfKind(AssetKind::Material)) {
            MaterialAsset m;
            db.load(id, m);
            if (m.name == "Blue") return id;
        }
        return AssetId{};
    }();

    PrefabVariantAsset v;
    v.name = "BlueQuad";
    v.base = r.prefab;
    PrefabOverride o;
    o.nodePath = "Root/Child";
    o.flags = PrefabOverride::SetMaterial;
    o.materialSlot = 0;
    o.material = blue;
    v.overrides.push_back(o);
    const AssetId variantId = db.saveVariant("variants/blue_quad", v);
    REQUIRE(variantId.isValid());
    CHECK(db.saveVariant("variants/blue_quad", v) == variantId); // same file, same identity
    CHECK(db.dependents(r.prefab) == std::vector<AssetId>{variantId});

    VariantApplyResult resolved;
    std::string error;
    REQUIRE(db.loadResolvedPrefab(variantId, resolved, &error));
    CHECK(resolved.orphaned.empty());
    REQUIRE(resolved.prefab.nodes.size() == 3);
    CHECK(resolved.prefab.nodes[1].materials[0] == blue);

    AssetDb reopened(dbFor(t));
    reopened.scan();
    CHECK(reopened.find(variantId));
    VariantApplyResult again;
    REQUIRE(reopened.loadResolvedPrefab(variantId, again, &error));
    CHECK(again.prefab.nodes[1].materials[0] == blue);
}

TEST_CASE("FBX import (ASCII) converts units and builds a prefab") {
    TempDir t;
    // One triangle, translated by 100 cm, authored in centimeters (the FBX default unit).
    writeText(t.path / "assets/tri.fbx", R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",1
	}
}
Objects:  {
	Geometry: 1000, "Geometry::Tri", "Mesh" {
		Vertices: *9 {
			a: 0,0,0,100,0,0,0,100,0
		}
		PolygonVertexIndex: *3 {
			a: 0,1,-3
		}
		GeometryVersion: 124
	}
	Model: 2000, "Model::TriNode", "Mesh" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",100,0,0
		}
	}
}
Connections:  {
	C: "OO",2000,0
	C: "OO",1000,2000
}
)");
    AssetDb db(dbFor(t));
    const ImportResult r = db.importFile("tri.fbx");
    INFO(r.error);
    REQUIRE(r.ok);
    REQUIRE(db.assetsOfKind(AssetKind::Mesh).size() == 1);
    REQUIRE(r.prefab.isValid());

    MeshAsset mesh;
    REQUIRE(db.load(db.assetsOfKind(AssetKind::Mesh)[0], mesh));
    CHECK(mesh.vertexCount() == 3);
    CHECK(mesh.indices.size() == 3);
    CHECK(mesh.boundsMax.x == doctest::Approx(1.0f).epsilon(0.01)); // 100 cm -> 1 m
    const auto* v = reinterpret_cast<const CookedVertex*>(mesh.vertexData.data());
    CHECK(std::abs(v[0].normal.z) > 0.99f); // generated normals

    PrefabAsset prefab;
    REQUIRE(db.load(r.prefab, prefab));
    REQUIRE(prefab.nodes.size() == 1);
    CHECK(prefab.nodes[0].name == "TriNode");
    CHECK(prefab.nodes[0].position.x == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(prefab.nodes[0].mesh.isValid());
}
