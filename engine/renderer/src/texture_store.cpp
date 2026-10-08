#include "texture_store.h"

#include <algorithm>

#include "foh/log.h"

namespace foh {

namespace {

struct FormatInfo {
    nvrhi::Format format;
    u32 blockBytes; // bytes per 4x4 block (BC) or per texel (RGBA8)
    bool block;
};

FormatInfo infoOf(TextureFormat f) {
    switch (f) {
        case TextureFormat::Rgba8Unorm: return {nvrhi::Format::RGBA8_UNORM, 4, false};
        case TextureFormat::Rgba8Srgb: return {nvrhi::Format::SRGBA8_UNORM, 4, false};
        case TextureFormat::Bc7Unorm: return {nvrhi::Format::BC7_UNORM, 16, true};
        case TextureFormat::Bc7Srgb: return {nvrhi::Format::BC7_UNORM_SRGB, 16, true};
        case TextureFormat::Bc5Unorm: return {nvrhi::Format::BC5_UNORM, 16, true};
        case TextureFormat::Bc4Unorm: return {nvrhi::Format::BC4_UNORM, 8, true};
    }
    return {nvrhi::Format::UNKNOWN, 0, false};
}

u32 rowPitchOf(const FormatInfo& fi, u32 width) { return fi.block ? ((width + 3) / 4) * fi.blockBytes : width * fi.blockBytes; }
u32 rowsOf(const FormatInfo& fi, u32 height) { return fi.block ? (height + 3) / 4 : height; }

u64 totalBytes(const TextureUpload& up) {
    u64 n = 0;
    for (const auto& m : up.mips) n += m.size();
    return n;
}

} // namespace

bool TextureStore::init(nvrhi::IDevice* device, nvrhi::IBindingLayout* layout) {
    m_table = device->createDescriptorTable(layout);
    if (!m_table) return false;
    device->resizeDescriptorTable(m_table, kCapacity, false);

    // The two stand-ins, written into slots 0 and 1.
    nvrhi::CommandListParameters params;
    params.enableImmediateExecution = false;
    nvrhi::CommandListHandle cmd = device->createCommandList(params);
    cmd->open();
    auto makeSolid = [&](const u8 rgba[4], const char* name, u32 slot) {
        nvrhi::TextureDesc td;
        td.width = td.height = 1;
        td.format = nvrhi::Format::RGBA8_UNORM;
        td.initialState = nvrhi::ResourceStates::ShaderResource;
        td.keepInitialState = true;
        td.debugName = name;
        nvrhi::TextureHandle tex = device->createTexture(td);
        cmd->writeTexture(tex, 0, 0, rgba, 4);
        device->writeDescriptorTable(m_table, nvrhi::BindingSetItem::Texture_SRV(slot, tex));
        return tex;
    };
    const u8 white[4] = {255, 255, 255, 255};
    const u8 flat[4] = {128, 128, 255, 255};
    m_white = makeSolid(white, "WhiteTexture", kWhiteSlot);
    m_flatNormal = makeSolid(flat, "FlatNormalTexture", kFlatNormalSlot);
    cmd->close();
    device->executeCommandLists(&cmd, 1);
    return true;
}

bool TextureStore::enqueue(TextureUpload&& upload) {
    if (!upload.id.isValid() || upload.mips.empty() || upload.mips.size() > 16 || upload.width == 0 || upload.height == 0) return false;
    const FormatInfo fi = infoOf(upload.format);
    if (fi.format == nvrhi::Format::UNKNOWN) return false;
    if (fi.block && (upload.width % 4 != 0 || upload.height % 4 != 0)) return false; // mip 0 of a BC texture must be a multiple of 4
    // Every mip must have exactly the size its dimensions imply: the data comes from a file.
    u32 w = upload.width, h = upload.height;
    for (const auto& mip : upload.mips) {
        if (mip.size() != static_cast<size_t>(rowPitchOf(fi, w)) * rowsOf(fi, h)) return false;
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }

    const u64 bytes = totalBytes(upload);
    if (const auto it = m_queued.find(upload.id); it != m_queued.end()) {
        std::erase_if(m_queue, [&](const TextureUpload& q) { return q.id == upload.id; });
        m_queuedBytes -= it->second;
    }
    m_queued[upload.id] = bytes;
    m_queuedBytes += bytes;
    m_queue.push_back(std::move(upload));
    return true;
}

void TextureStore::release(const AssetId& id, u64 frame) {
    if (const auto q = m_queued.find(id); q != m_queued.end()) {
        std::erase_if(m_queue, [&](const TextureUpload& u) { return u.id == id; });
        m_queuedBytes -= q->second;
        m_queued.erase(q);
    }
    if (const auto it = m_resident.find(id); it != m_resident.end()) {
        m_residentBytes -= it->second.bytes;
        retireOld(std::move(it->second), frame);
        m_resident.erase(it);
    }
}

u32 TextureStore::allocateSlot() {
    if (!m_freeSlots.empty()) {
        const u32 slot = m_freeSlots.back();
        m_freeSlots.pop_back();
        return slot;
    }
    return m_nextSlot < kCapacity ? m_nextSlot++ : ~0u;
}

bool TextureStore::uploadNow(nvrhi::IDevice* device, nvrhi::ICommandList* cmd, TextureUpload& up, Resident& out) {
    const u32 slot = allocateSlot();
    if (slot == ~0u) {
        FOH_ERROR("texture table is full ({} textures): {:016x}{:016x} stays a stand-in", kCapacity, up.id.hi, up.id.lo);
        return false;
    }
    const FormatInfo fi = infoOf(up.format);
    nvrhi::TextureDesc td;
    td.width = up.width;
    td.height = up.height;
    td.mipLevels = static_cast<u32>(up.mips.size());
    td.format = fi.format;
    td.initialState = nvrhi::ResourceStates::ShaderResource; // the table is not tracked by command lists: always readable
    td.keepInitialState = true;
    td.debugName = "ImportedTexture";
    nvrhi::TextureHandle tex = device->createTexture(td);
    if (!tex) {
        m_freeSlots.push_back(slot);
        return false;
    }
    u32 w = up.width, h = up.height;
    for (u32 mip = 0; mip < up.mips.size(); ++mip) {
        cmd->writeTexture(tex, 0, mip, up.mips[mip].data(), rowPitchOf(fi, w));
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }
    device->writeDescriptorTable(m_table, nvrhi::BindingSetItem::Texture_SRV(slot, tex));
    out.texture = std::move(tex);
    out.slot = slot;
    out.bytes = totalBytes(up);
    return true;
}

void TextureStore::pump(nvrhi::IDevice* device, u64 frame, u64 byteBudget, std::vector<AssetId>& becameReady) {
    // Slots of replaced/released textures come back once no frame in flight can still sample them.
    std::erase_if(m_retired, [&](Retired& r) {
        if (r.frame + 4 > frame) return false;
        m_freeSlots.push_back(r.resident.slot);
        return true; // the texture handle dies here
    });
    if (m_queue.empty()) return;

    nvrhi::CommandListParameters params;
    params.enableImmediateExecution = false;
    nvrhi::CommandListHandle cmd = device->createCommandList(params);
    cmd->open();
    u64 spent = 0;
    bool any = false;
    while (!m_queue.empty() && (!any || spent < byteBudget)) {
        TextureUpload up = std::move(m_queue.front());
        m_queue.pop_front();
        if (const auto q = m_queued.find(up.id); q != m_queued.end()) {
            m_queuedBytes -= q->second;
            m_queued.erase(q);
        }
        Resident fresh;
        if (!uploadNow(device, cmd, up, fresh)) continue;
        spent += fresh.bytes;
        any = true;
        m_residentBytes += fresh.bytes;
        if (const auto it = m_resident.find(up.id); it != m_resident.end()) {
            m_residentBytes -= it->second.bytes;
            retireOld(std::move(it->second), frame);
            it->second = std::move(fresh);
        } else {
            m_resident.emplace(up.id, std::move(fresh));
        }
        becameReady.push_back(up.id);
    }
    cmd->close();
    device->executeCommandLists(&cmd, 1);
}

TextureStats TextureStore::stats() const {
    TextureStats s;
    s.resident = static_cast<u32>(m_resident.size());
    s.queued = static_cast<u32>(m_queue.size());
    s.residentBytes = m_residentBytes;
    s.queuedBytes = m_queuedBytes;
    return s;
}

} // namespace foh
