#pragma once

// GPU residency of imported textures: a bindless descriptor table plus the textures behind it.
//
// Uploads are queued and drained under a per-frame byte budget (a 4K texture is ~20 MB of BC7; a scene
// import can queue hundreds), so loading a model never stalls a frame. A texture only becomes visible
// when its whole mip chain is on the GPU; until then materials keep pointing at the stand-in slots.
//
// Slots are write-once: a slot's descriptor is written when the texture becomes resident and never
// changed afterwards. Replacing a texture (reimport) gets a NEW slot and retires the old one a few frames
// later, so frames still in flight keep sampling a valid texture. Materials learn the new slot through the
// `becameReady` list of pump() and rewrite themselves (a buffer write, which the GPU orders for us).
//
// Thread-safety: main thread only, between frames (the render jobs only read the descriptor table).

#include <deque>
#include <unordered_map>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "foh/renderer/renderer.h"

namespace foh {

class TextureStore {
public:
    static constexpr u32 kCapacity = 8192;     // bindless table size (descriptors)
    static constexpr u32 kWhiteSlot = 0;       // 1x1 white: unset color/data textures
    static constexpr u32 kFlatNormalSlot = 1;  // 1x1 (0.5, 0.5, 1): unset normal maps (and not-yet-resident ones)

    // `layout` is the bindless layout the table is created from.
    bool init(nvrhi::IDevice* device, nvrhi::IBindingLayout* layout);
    nvrhi::IDescriptorTable* table() const { return m_table; }

    // Takes the data; replaces a queued upload of the same asset. False if the upload is malformed.
    bool enqueue(TextureUpload&& upload);
    void release(const AssetId& id, u64 frame);
    bool resident(const AssetId& id) const { return m_resident.contains(id); }
    bool known(const AssetId& id) const { return m_resident.contains(id) || m_queued.contains(id); }
    // The slot a material should use for `id`: its own once resident, `fallback` before that.
    u32 slotOf(const AssetId& id, u32 fallback) const {
        const auto it = m_resident.find(id);
        return it == m_resident.end() ? fallback : it->second.slot;
    }

    // Uploads queued textures (at least one per call, then until `byteBudget` is spent), publishes them and
    // retires replaced/released ones. Appends the ids that became resident to `becameReady`.
    void pump(nvrhi::IDevice* device, u64 frame, u64 byteBudget, std::vector<AssetId>& becameReady);

    TextureStats stats() const;

private:
    struct Resident {
        nvrhi::TextureHandle texture;
        u32 slot = 0;
        u64 bytes = 0;
    };
    struct Retired {
        u64 frame;
        Resident resident;
    };

    bool uploadNow(nvrhi::IDevice* device, nvrhi::ICommandList* cmd, TextureUpload& upload, Resident& out);
    u32 allocateSlot();
    void retireOld(Resident&& r, u64 frame) { m_retired.push_back({frame, std::move(r)}); }

    nvrhi::DescriptorTableHandle m_table;
    nvrhi::TextureHandle m_white, m_flatNormal; // the stand-ins in slots 0 and 1
    std::unordered_map<AssetId, Resident, AssetIdHash> m_resident;
    std::unordered_map<AssetId, u64, AssetIdHash> m_queued; // asset -> queued bytes (for stats and replacement)
    std::deque<TextureUpload> m_queue;
    std::vector<Retired> m_retired;
    std::vector<u32> m_freeSlots;
    u32 m_nextSlot = 2;
    u64 m_residentBytes = 0;
    u64 m_queuedBytes = 0;
};

} // namespace foh
