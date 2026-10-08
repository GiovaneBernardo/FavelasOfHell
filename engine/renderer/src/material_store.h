#pragma once

// The GPU material table: one GpuMaterial per imported material in a structured buffer, plus the default
// material at index 0 (what builtin meshes and not-yet-uploaded materials use).
//
// Materials store their textures by asset id and resolve them to bindless slots whenever the material or one
// of its textures changes, so a texture that arrives later (or is replaced by a reimport) reaches every
// material that uses it without anybody tracking dependents.
//
// Thread-safety: main thread between frames; classOf() may be called from render jobs while nothing mutates.

#include <unordered_map>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "foh/renderer/render_scene.h"
#include "gpu_structs.h"
#include "texture_store.h"

namespace foh {

// How a material is drawn: which pipeline variant. Instances are batched per class so the cheap path
// (opaque, single-sided: depth passes without a pixel shader) stays cheap for everything that can use it.
enum class DrawClass : u32 { Opaque = 0, Masked = 1, OpaqueDoubleSided = 2, MaskedDoubleSided = 3, Count = 4 };

inline constexpr u32 kDrawClassCount = static_cast<u32>(DrawClass::Count);
inline constexpr bool IsMasked(DrawClass c) { return (static_cast<u32>(c) & 1u) != 0; }
inline constexpr bool IsDoubleSided(DrawClass c) { return (static_cast<u32>(c) & 2u) != 0; }

class MaterialStore {
public:
    void init(nvrhi::IDevice* device);

    nvrhi::IBuffer* buffer() const { return m_buffer; }
    const MaterialLookup& lookup() const { return m_lookup; }
    u32 count() const { return static_cast<u32>(m_cpu.size()); }
    DrawClass classOf(u32 index) const { return index < m_classes.size() ? m_classes[index] : DrawClass::Opaque; }

    // Creates or updates; the entry resolves its texture slots against `textures` now.
    void set(const AssetId& id, const MaterialParams& params, const TextureStore& textures);
    void remove(const AssetId& id);
    bool contains(const AssetId& id) const { return m_lookup.contains(id); }
    // Textures became resident or were replaced: rewrite the materials that reference them.
    void texturesChanged(const std::vector<AssetId>& ids, const TextureStore& textures);
    // Writes the changed entries to the GPU (growing the buffer if needed).
    void upload(nvrhi::IDevice* device);

private:
    struct Entry {
        AssetId id;
        MaterialParams params;
        bool used = false;
    };
    void resolve(u32 index, const TextureStore& textures);
    u32 allocate();

    nvrhi::BufferHandle m_buffer;
    u32 m_capacity = 0;
    std::vector<GpuMaterial> m_cpu;   // index == GPU index
    std::vector<Entry> m_entries;     // same indexing; entry 0 is the default material
    std::vector<DrawClass> m_classes;
    std::vector<u32> m_free;
    MaterialLookup m_lookup;
    bool m_dirty = true;
};

} // namespace foh
