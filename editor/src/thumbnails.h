#pragma once

// Thumbnails for the content browser and the assets workshop.
//
// Decoding/rasterizing runs on the job system; the finished RGBA tiles are packed into a few
// large GPU atlas textures, so a screen full of thumbnails is ONE texture (one draw batch, one
// graph resource) instead of hundreds. Requests are driven by what is visible: a tile that
// scrolls out of view before its job started is dropped, and the atlas evicts the least
// recently drawn tiles when it is full. Uploads per frame are capped so scrolling never hitches.
//
// Thread-safety: main thread only (loaders run on job threads but only touch their own data).

#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <nvrhi/nvrhi.h>

#include "foh/jobs.h"
#include "foh/types.h"

namespace foh {

class Renderer;

// A decoded thumbnail: RGBA8, at most kThumbSize in each dimension, tightly packed.
struct ThumbPixels {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;
};

inline constexpr u32 kThumbSize = 256;

struct ThumbRect {
    ImTextureID texture = ImTextureID_Invalid;
    ImVec2 uv0{0, 0};
    ImVec2 uv1{1, 1};
    float aspect = 1.0f; // width / height of the content
};

// Runs on a job thread. Returns false when the file cannot produce a thumbnail.
using ThumbLoader = std::function<bool(ThumbPixels& out)>;

class ThumbnailService {
public:
    ThumbnailService();
    ~ThumbnailService();

    void init(Renderer& renderer, JobSystem& jobs);
    void shutdown();

    // Call once per frame before any get(): advances the frame counter that drives eviction.
    void beginFrame();
    // Call once per frame after the UI is built (before the renderer draws): collects finished jobs,
    // starts new ones for what was requested this frame and uploads up to a few tiles to the GPU.
    void endFrame();

    enum class State : u8 { Unknown, Pending, Ready, Failed };

    // One hash lookup. Marks the tile as drawn this frame (keeps it resident). The key must change when
    // the content does (include the file's modification time). Unknown means: call request().
    State lookup(u64 key, ThumbRect& out);
    // Enqueues a load for an Unknown key. Taking the loader here (not in lookup) lets callers build it
    // lazily: gathering a model's cooked files is far more work than a hash lookup.
    void request(u64 key, ThumbLoader loader);
    // Hands in pixels produced elsewhere (e.g. downloaded and decoded by the caller).
    void insert(u64 key, ThumbPixels pixels);

    // Number of tiles currently resident, for the stats panel.
    size_t residentCount() const;
    size_t pendingCount() const { return m_queue.size() + m_inFlight.size(); }

private:
    struct Entry {
        State state = State::Pending;
        u32 page = 0, slot = 0;
        u32 width = 0, height = 0;
        u64 lastUsedFrame = 0;
        bool uploaded = false;
    };
    struct Task;
    struct Upload {
        u64 key;
        ThumbPixels pixels;
    };

    bool allocateSlot(u64 forKey, u32& page, u32& slot);
    void addPage();
    void startTasks();
    void collectTasks();
    void uploadReady();

    Renderer* m_renderer = nullptr;
    JobSystem* m_jobs = nullptr;
    nvrhi::IDevice* m_device = nullptr;
    nvrhi::CommandListHandle m_uploadList;
    std::vector<nvrhi::TextureHandle> m_pages;
    std::vector<nvrhi::TextureHandle> m_staging;
    std::vector<std::vector<u32>> m_freeSlots; // per page

    std::unordered_map<u64, Entry> m_entries;
    std::deque<std::pair<u64, ThumbLoader>> m_queue; // not started yet, newest requests first
    std::vector<std::unique_ptr<Task>> m_inFlight;
    std::deque<Upload> m_uploads;
    u64 m_frame = 0;
};

} // namespace foh
