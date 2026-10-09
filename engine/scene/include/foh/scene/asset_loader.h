#pragma once

// Streams cooked assets from disk to the renderer without ever blocking the frame.
//
//   request(id)  ->  worker (low priority): read file, decompress, validate, convert to the renderer's form
//                ->  pump() on the main thread: hand the result to the renderer, a few milliseconds per frame at most
//                ->  renderer: textures upload under a byte budget, meshes append to the shared buffers
//
// Requesting a prefab (or variant) requests everything it needs: its meshes, its materials and, through the
// materials, their textures. Nothing is requested twice. Until an asset arrives, things that use it degrade
// gracefully: a mesh that is not there yet is not drawn, a material not there yet is the default material, a
// texture not there yet is a white / flat-normal stand-in.
//
// Thread-safety: main thread only. The workers touch only their own job object and read files by path.

#include <chrono>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "foh/animation/animation_library.h"
#include "foh/asset_db.h"
#include "foh/jobs.h"
#include "foh/platform.h"
#include "foh/renderer/renderer.h"

namespace foh {

struct AssetLoaderStats {
    u32 queued = 0;   // waiting for a worker
    u32 loading = 0;  // on a worker or waiting for pump()
    u32 loaded = 0;
    u32 failed = 0;
};

class FOH_API AssetLoader {
public:
    // All three must outlive the loader. `jobs` may be null: loads then run inline, one per pump().
    AssetLoader(AssetDb& db, Renderer& renderer, JobSystem* jobs);
    ~AssetLoader(); // waits for loads in flight
    AssetLoader(const AssetLoader&) = delete;
    AssetLoader& operator=(const AssetLoader&) = delete;

    void request(AssetId id);
    // Hands finished loads to the renderer (stops after `budgetMs`, the rest waits for the next frame) and
    // starts queued ones. Cheap when idle.
    void pump(f32 budgetMs = 3.0f);
    // A source was reimported: reloads what is loaded, releases what is gone.
    void assetChanged(AssetId id);

    // Where loaded skeletons and animation clips go (they are not renderer data). Without one they are not loaded.
    void setAnimationLibrary(AnimationLibrary* library) { m_animation = library; }

    // Textures larger than this are loaded without their largest mips (VRAM budget on the minimum spec).
    void setMaxTextureSize(u32 size) { m_maxTextureSize = size; }

    struct Job; // what a worker produces for one asset (internal)

    AssetLoaderStats stats() const;
    bool idle() const { return m_queue.empty() && m_inflight.empty(); }

private:
    enum class State : u8 { Queued, Loading, Loaded, Failed };
    struct Entry {
        State state = State::Queued;
        AssetKind kind = AssetKind::None;
        bool reloadAfter = false; // changed while a load was in flight: load again when it lands
    };

    void start(const AssetId& id, AssetKind kind);
    void apply(Job& job);
    void release(const AssetId& id, AssetKind kind);

    AssetDb& m_db;
    Renderer& m_renderer;
    JobSystem* m_jobs;
    AnimationLibrary* m_animation = nullptr;
    u32 m_maxTextureSize = 4096;
    std::unordered_map<AssetId, Entry, AssetIdHash> m_entries;
    std::deque<AssetId> m_queue;
    std::vector<std::unique_ptr<Job>> m_inflight;
};

} // namespace foh
