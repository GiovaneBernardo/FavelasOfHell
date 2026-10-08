#include "thumbnails.h"

#include <algorithm>
#include <cstring>

#include "foh/log.h"
#include "foh/renderer/renderer.h"
#include "foh/rhi/graphics_device.h"

namespace foh {

namespace {

constexpr u32 kPageSize = 2048;
constexpr u32 kSlotsPerRow = kPageSize / kThumbSize;
constexpr u32 kSlotsPerPage = kSlotsPerRow * kSlotsPerRow;
constexpr u32 kMaxPages = 8;           // 512 resident tiles, 128 MB at most
constexpr u32 kUploadsPerFrame = 6;    // each upload is 256 KB: keeps a fast scroll from hitching
constexpr u32 kMaxQueued = 512;        // requests are re-issued when still visible, so dropping is safe
constexpr u64 kStaleFrames = 3;        // a queued request unseen for this long is dropped

ImTextureID toImTexture(nvrhi::ITexture* tex) {
    return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(tex));
}

} // namespace

struct ThumbnailService::Task {
    JobCounter counter;
    u64 key = 0;
    ThumbLoader loader;
    ThumbPixels result;
    bool ok = false;

    static void run(void* arg) {
        auto* self = static_cast<Task*>(arg);
        self->ok = self->loader(self->result) && self->result.width > 0 && self->result.height > 0 &&
                   self->result.rgba.size() == static_cast<size_t>(self->result.width) * self->result.height * 4;
    }
};

ThumbnailService::ThumbnailService() = default;
ThumbnailService::~ThumbnailService() { shutdown(); }

void ThumbnailService::init(Renderer& renderer, JobSystem& jobs) {
    m_renderer = &renderer;
    m_jobs = &jobs;
    m_device = renderer.device().nvrhi();
    nvrhi::CommandListParameters params;
    params.enableImmediateExecution = false;
    m_uploadList = m_device->createCommandList(params);

    nvrhi::TextureDesc td;
    td.width = kThumbSize;
    td.height = kThumbSize;
    td.format = nvrhi::Format::RGBA8_UNORM;
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true;
    td.debugName = "Thumbnail staging";
    for (u32 i = 0; i < kUploadsPerFrame; ++i) m_staging.push_back(m_device->createTexture(td));
}

void ThumbnailService::shutdown() {
    if (!m_jobs) return;
    for (auto& t : m_inFlight) m_jobs->wait(t->counter); // jobs own pointers into their tasks
    m_inFlight.clear();
    m_queue.clear();
    m_uploads.clear();
    m_entries.clear();
    m_staging.clear();
    m_pages.clear();
    m_freeSlots.clear();
    m_uploadList = nullptr;
    m_jobs = nullptr;
    m_renderer = nullptr;
    m_device = nullptr;
}

void ThumbnailService::beginFrame() { ++m_frame; }

size_t ThumbnailService::residentCount() const {
    size_t n = 0;
    for (const auto& [k, e] : m_entries) n += e.uploaded ? 1 : 0;
    return n;
}

ThumbnailService::State ThumbnailService::lookup(u64 key, ThumbRect& out) {
    auto it = m_entries.find(key);
    if (it == m_entries.end()) return State::Unknown;
    Entry& e = it->second;
    e.lastUsedFrame = m_frame;
    if (e.state == State::Ready && e.uploaded) {
        const float inset = 0.5f / static_cast<float>(kPageSize); // half a texel: no bleeding from neighbours
        const float x = static_cast<float>((e.slot % kSlotsPerRow) * kThumbSize);
        const float y = static_cast<float>((e.slot / kSlotsPerRow) * kThumbSize);
        out.texture = toImTexture(m_pages[e.page].Get());
        out.uv0 = ImVec2(x / kPageSize + inset, y / kPageSize + inset);
        out.uv1 = ImVec2((x + static_cast<float>(e.width)) / kPageSize - inset,
                         (y + static_cast<float>(e.height)) / kPageSize - inset);
        out.aspect = static_cast<float>(e.width) / static_cast<float>(std::max(e.height, 1u));
        return State::Ready;
    }
    return e.state == State::Failed ? State::Failed : State::Pending;
}

void ThumbnailService::request(u64 key, ThumbLoader loader) {
    if (m_queue.size() >= kMaxQueued) return; // asked again next frame while still visible
    Entry e;
    e.lastUsedFrame = m_frame;
    m_entries.emplace(key, e);
    m_queue.emplace_back(key, std::move(loader));
}

void ThumbnailService::insert(u64 key, ThumbPixels pixels) {
    Entry& e = m_entries[key];
    e.state = State::Pending;
    e.lastUsedFrame = m_frame;
    m_uploads.push_back({key, std::move(pixels)});
}

void ThumbnailService::endFrame() {
    if (!m_jobs) return;
    collectTasks();
    startTasks();
    uploadReady();
}

void ThumbnailService::startTasks() {
    // Keep a couple of jobs in flight at most: decoding is bulk work that must not starve the frame's own jobs.
    const size_t limit = std::max<size_t>(1, m_jobs->workerCount() / 2);
    while (m_inFlight.size() < limit && !m_queue.empty()) {
        auto [key, loader] = std::move(m_queue.front());
        m_queue.pop_front();
        auto it = m_entries.find(key);
        if (it == m_entries.end()) continue;
        if (m_frame - it->second.lastUsedFrame > kStaleFrames) {
            m_entries.erase(it); // scrolled away before it started: a later get() re-requests it
            continue;
        }
        auto task = std::make_unique<Task>();
        task->key = key;
        task->loader = std::move(loader);
        Task* raw = task.get();
        m_jobs->submit(&Task::run, raw, raw->counter, JobPriority::Low);
        m_inFlight.push_back(std::move(task));
    }
}

void ThumbnailService::collectTasks() {
    for (size_t i = 0; i < m_inFlight.size();) {
        Task& t = *m_inFlight[i];
        if (t.counter.pending.load(std::memory_order_acquire) != 0) {
            ++i;
            continue;
        }
        auto it = m_entries.find(t.key);
        if (it != m_entries.end()) {
            if (t.ok) m_uploads.push_back({t.key, std::move(t.result)});
            else it->second.state = State::Failed;
        }
        m_inFlight.erase(m_inFlight.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void ThumbnailService::addPage() {
    nvrhi::TextureDesc td;
    td.width = kPageSize;
    td.height = kPageSize;
    td.format = nvrhi::Format::RGBA8_UNORM;
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true;
    td.debugName = "Thumbnail atlas";
    m_pages.push_back(m_device->createTexture(td));
    std::vector<u32> free;
    for (u32 i = kSlotsPerPage; i-- > 0;) free.push_back(i); // pop_back hands out slot 0 first
    m_freeSlots.push_back(std::move(free));
}

bool ThumbnailService::allocateSlot(u64 forKey, u32& page, u32& slot) {
    for (u32 p = 0; p < m_freeSlots.size(); ++p) {
        if (!m_freeSlots[p].empty()) {
            page = p;
            slot = m_freeSlots[p].back();
            m_freeSlots[p].pop_back();
            return true;
        }
    }
    if (m_pages.size() < kMaxPages) {
        addPage();
        return allocateSlot(forKey, page, slot);
    }
    // Evict the least recently drawn tile, but never one drawn this or the previous frame.
    u64 victimKey = 0;
    u64 oldest = ~u64{0};
    for (const auto& [k, e] : m_entries) {
        if (k == forKey || !e.uploaded || e.lastUsedFrame + 1 >= m_frame) continue;
        if (e.lastUsedFrame < oldest) {
            oldest = e.lastUsedFrame;
            victimKey = k;
        }
    }
    if (oldest == ~u64{0}) return false;
    const Entry victim = m_entries[victimKey];
    page = victim.page;
    slot = victim.slot;
    m_entries.erase(victimKey);
    return true;
}

void ThumbnailService::uploadReady() {
    if (m_uploads.empty()) return;
    bool opened = false;
    std::vector<u8> padded(static_cast<size_t>(kThumbSize) * kThumbSize * 4);
    for (u32 n = 0; n < kUploadsPerFrame && !m_uploads.empty(); ++n) {
        Upload& up = m_uploads.front();
        auto it = m_entries.find(up.key);
        if (it == m_entries.end()) { // evicted or cleared while waiting
            m_uploads.pop_front();
            continue;
        }
        u32 page = 0, slot = 0;
        if (it->second.uploaded) { // re-insert of a resident key: reuse its slot
            page = it->second.page;
            slot = it->second.slot;
        } else if (!allocateSlot(up.key, page, slot)) {
            break; // atlas full of tiles in use: try again next frame
        }
        it = m_entries.find(up.key); // allocateSlot may have erased other entries

        const u32 w = std::min(up.pixels.width, kThumbSize);
        const u32 h = std::min(up.pixels.height, kThumbSize);
        std::memset(padded.data(), 0, padded.size());
        for (u32 y = 0; y < h; ++y) {
            std::memcpy(padded.data() + static_cast<size_t>(y) * kThumbSize * 4,
                        up.pixels.rgba.data() + static_cast<size_t>(y) * up.pixels.width * 4, static_cast<size_t>(w) * 4);
        }
        if (!opened) {
            m_uploadList->open();
            opened = true;
        }
        nvrhi::ITexture* staging = m_staging[n].Get();
        m_uploadList->writeTexture(staging, 0, 0, padded.data(), static_cast<size_t>(kThumbSize) * 4);
        const u32 x = (slot % kSlotsPerRow) * kThumbSize;
        const u32 y = (slot / kSlotsPerRow) * kThumbSize;
        nvrhi::TextureSlice dst;
        dst.setOrigin(x, y, 0).setWidth(kThumbSize).setHeight(kThumbSize);
        nvrhi::TextureSlice src;
        src.setWidth(kThumbSize).setHeight(kThumbSize);
        m_uploadList->copyTexture(m_pages[page].Get(), dst, staging, src);

        Entry& e = it->second;
        e.state = State::Ready;
        e.page = page;
        e.slot = slot;
        e.width = w;
        e.height = h;
        e.uploaded = true;
        m_uploads.pop_front();
    }
    if (opened) {
        m_uploadList->close();
        nvrhi::ICommandList* list = m_uploadList.Get();
        m_device->executeCommandLists(&list, 1);
    }
}

} // namespace foh
