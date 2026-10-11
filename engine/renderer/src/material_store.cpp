#include "material_store.h"

#include <algorithm>

namespace foh {

namespace {

constexpr u32 kInitialCapacity = 256;

}

void MaterialStore::init(nvrhi::IDevice* device) {
    m_cpu.clear();
    m_entries.clear();
    m_classes.clear();
    // Index 0: the default material, used by builtin meshes and for anything not uploaded yet.
    MaterialParams def;
    def.roughness = 0.8f;
    Entry e;
    e.params = def;
    e.used = true;
    m_entries.push_back(e);
    m_cpu.push_back({});
    m_classes.push_back(DrawClass::Opaque);
    GpuMaterial& g = m_cpu[0];
    g.baseColor = def.baseColor;
    g.alphaCutoff = def.alphaCutoff;
    g.metallic = def.metallic;
    g.roughness = def.roughness;
    g.normalScale = 1.0f;
    g.occlusionStrength = 1.0f;
    g.baseColorTex = g.metalRoughTex = g.occlusionTex = g.emissiveTex = TextureStore::kWhiteSlot;
    g.normalTex = TextureStore::kFlatNormalSlot;
    (void)device; // the buffer is created lazily by upload()
    m_dirty = true;
}

void MaterialStore::resolve(u32 index, const TextureStore& textures) {
    const MaterialParams& p = m_entries[index].params;
    GpuMaterial& g = m_cpu[index];
    g = {};
    g.baseColor = p.baseColor;
    g.emissive = p.emissive;
    g.alphaCutoff = p.alphaCutoff;
    g.metallic = p.metallic;
    g.roughness = p.roughness;
    g.normalScale = p.normalScale;
    g.occlusionStrength = p.occlusionStrength;
    g.baseColorTex = textures.slotOf(p.baseColorTexture, TextureStore::kWhiteSlot);
    g.normalTex = textures.slotOf(p.normalTexture, TextureStore::kFlatNormalSlot);
    g.metalRoughTex = textures.slotOf(p.metalRoughTexture, TextureStore::kWhiteSlot);
    // Without a dedicated occlusion texture the occlusion slot equals the metal/roughness one: the shader then reads
    // both from a single sample. With neither it is white (no occlusion).
    g.occlusionTex = p.occlusionTexture.isValid() ? textures.slotOf(p.occlusionTexture, TextureStore::kWhiteSlot) : g.metalRoughTex;
    if (!p.occlusionTexture.isValid()) g.occlusionStrength = 0.0f; // nothing to apply: ao = 1
    g.emissiveTex = textures.slotOf(p.emissiveTexture, TextureStore::kWhiteSlot);
    // The normal map flag is on only once the texture is resident: a flat stand-in would otherwise be sampled for nothing.
    const bool hasNormalMap = p.normalTexture.isValid() && textures.resident(p.normalTexture);
    g.flags = (p.alphaMask ? kMaterialFlagAlphaMask : 0u) | (p.doubleSided ? kMaterialFlagDoubleSided : 0u) |
              (hasNormalMap ? kMaterialFlagNormalMap : 0u);

    if (p.terrain) {
        g.flags = kMaterialFlagTerrain;
        u32* albedo[4] = {&g.baseColorTex, &g.metalRoughTex, &g.occlusionTex, &g.emissiveTex};
        u32* normal[4] = {&g.normalTex, &g.extra0, &g.extra1, &g.extra2};
        for (u32 l = 0; l < 4; ++l) {
            *albedo[l] = textures.slotOf(p.terrainAlbedo[l], TextureStore::kWhiteSlot);
            *normal[l] = textures.slotOf(p.terrainNormal[l], TextureStore::kFlatNormalSlot);
            if (p.terrainAlbedo[l].isValid() && textures.resident(p.terrainAlbedo[l])) g.flags |= kMaterialFlagLayerAlbedo0 << l;
            if (p.terrainNormal[l].isValid() && textures.resident(p.terrainNormal[l])) g.flags |= kMaterialFlagLayerNormal0 << l;
        }
        g.terrainTile = p.terrainTileSize;
        g.occlusionStrength = 0.0f;
        if (p.terrainMask.isValid() && textures.resident(p.terrainMask)) {
            g.flags |= kMaterialFlagTerrainMask;
            g.pad1 = textures.slotOf(p.terrainMask, TextureStore::kWhiteSlot);
            g.baseColor = p.terrainMaskRect;
        }
        m_classes[index] = DrawClass::Opaque;
        m_dirty = true;
        return;
    }

    if (p.foliage) {
        g.flags |= kMaterialFlagFoliage;
        g.translucency = p.translucency;
    }
    const bool masked = p.alphaMask;
    m_classes[index] = masked ? (p.doubleSided ? DrawClass::MaskedDoubleSided : DrawClass::Masked)
                              : (p.doubleSided ? DrawClass::OpaqueDoubleSided : DrawClass::Opaque);
    m_dirty = true;
}

u32 MaterialStore::allocate() {
    if (!m_free.empty()) {
        const u32 i = m_free.back();
        m_free.pop_back();
        return i;
    }
    m_entries.emplace_back();
    m_cpu.emplace_back();
    m_classes.push_back(DrawClass::Opaque);
    return static_cast<u32>(m_entries.size() - 1);
}

void MaterialStore::set(const AssetId& id, const MaterialParams& params, const TextureStore& textures) {
    if (!id.isValid()) return;
    u32 index = m_lookup.contains(id) ? m_lookup.find(id) : allocate();
    m_entries[index].id = id;
    m_entries[index].params = params;
    m_entries[index].used = true;
    m_lookup.set(id, index);
    resolve(index, textures);
}

void MaterialStore::remove(const AssetId& id) {
    if (!m_lookup.contains(id)) return;
    const u32 index = m_lookup.find(id);
    m_lookup.erase(id);
    m_entries[index] = {};
    m_cpu[index] = m_cpu[0]; // a stale instance still draws something sane for the frames it lives on
    m_classes[index] = DrawClass::Opaque;
    m_free.push_back(index);
    m_dirty = true;
}

void MaterialStore::texturesChanged(const std::vector<AssetId>& ids, const TextureStore& textures) {
    if (ids.empty()) return;
    for (u32 i = 1; i < m_entries.size(); ++i) {
        if (!m_entries[i].used) continue;
        const MaterialParams& p = m_entries[i].params;
        for (const AssetId& t : ids) {
            bool layer = false;
            if (p.terrain)
                for (u32 l = 0; l < 4; ++l) layer = layer || t == p.terrainAlbedo[l] || t == p.terrainNormal[l];
            if (p.terrain && t == p.terrainMask) layer = true;
            if (layer || t == p.baseColorTexture || t == p.normalTexture || t == p.metalRoughTexture || t == p.occlusionTexture ||
                t == p.emissiveTexture) {
                resolve(i, textures);
                break;
            }
        }
    }
}

void MaterialStore::upload(nvrhi::IDevice* device) {
    if (!m_dirty && m_buffer) return;
    const u32 needed = static_cast<u32>(m_cpu.size());
    if (!m_buffer || needed > m_capacity) {
        m_capacity = std::max(kInitialCapacity, m_capacity);
        while (m_capacity < needed) m_capacity *= 2;
        nvrhi::BufferDesc bd;
        bd.byteSize = static_cast<u64>(m_capacity) * sizeof(GpuMaterial);
        bd.structStride = sizeof(GpuMaterial);
        bd.initialState = nvrhi::ResourceStates::ShaderResource;
        bd.keepInitialState = true;
        bd.debugName = "Materials";
        m_buffer = device->createBuffer(bd);
    }
    nvrhi::CommandListParameters params;
    params.enableImmediateExecution = false;
    nvrhi::CommandListHandle cmd = device->createCommandList(params);
    cmd->open();
    cmd->writeBuffer(m_buffer, m_cpu.data(), m_cpu.size() * sizeof(GpuMaterial));
    cmd->close();
    device->executeCommandLists(&cmd, 1);
    m_dirty = false;
}

} // namespace foh
