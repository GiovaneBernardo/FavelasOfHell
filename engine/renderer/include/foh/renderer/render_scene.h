#pragma once

#include <memory>
#include <vector>

#include "foh/ecs.h"
#include "foh/jobs.h"
#include "foh/math.h"
#include "foh/platform.h"
#include "foh/renderer/components.h"
#include "foh/types.h"

namespace foh {

// ---- extracted data: what the renderer knows about a scene ---------------------------

struct ExtractedMesh {
    Mat4 world;
    Vec4 color;
    u32 mesh;
    u32 pad[3];
};

struct ExtractedCamera {
    u64 entity; // Entity::id of the owner
    Mat4 world;
    f32 fovY, nearZ, farZ;
    bool active;
};

struct ExtractedSun {
    bool present = false;
    Vec3 direction{0.4f, -1.0f, 0.3f}; // direction the light travels
    Vec3 color{1.0f};                  // already multiplied by intensity
};

struct ExtractedScene {
    std::vector<ExtractedMesh> meshes;
    std::vector<ExtractedCamera> cameras;
    ExtractedSun sun;
    u64 frame = 0;
};

// The renderer's copy of one ECS World's render data. The ECS never exposes its
// storage to the renderer: a plugin (RenderExtractPlugin) copies what is needed
// into this object once per frame, and the renderer only ever reads the
// published snapshot. That keeps the two sides decoupled and makes a dedicated
// render thread possible later (the double buffer below is the seam).
//
// Thread-safety: write() is for the extraction system (inside App::update), read()
// for the renderer after publish(). publish() runs on the main thread between
// the two, never concurrently with either.
class FOH_API RenderScene {
public:
    ExtractedScene& write() {
        m_dirty = true;
        return m_buffers[m_writeIndex];
    }
    const ExtractedScene& read() const { return m_buffers[m_writeIndex ^ 1]; }
    // Makes the last write() visible to read(). Without a new write() since the
    // previous publish() it does nothing, so a frame without a world update keeps
    // showing the last extracted state instead of an older one.
    void publish() {
        if (!m_dirty) return;
        m_writeIndex ^= 1;
        m_dirty = false;
    }

    // Per-job-thread staging used by the parallel extraction.
    std::vector<ExtractedMesh>& staging(u32 threadSlot) { return m_staging[threadSlot]; }

private:
    ExtractedScene m_buffers[2];
    u32 m_writeIndex = 0;
    bool m_dirty = false;
    std::vector<ExtractedMesh> m_staging[kMaxJobThreads];
};

// Resource that ties an ECS world to its RenderScene. Holds a pointer into
// engine-owned memory (the Renderer owns the scene), which is allowed.
struct RenderSceneLink {
    RenderScene* scene = nullptr;
};

// Registers the render components, the link resource (empty until a scene is
// attached) and the PostUpdate extraction system. Call it (or add the plugin) for
// every App whose world should be renderable; it belongs to the engine, not to a
// game module.
FOH_API void RegisterRenderExtract(App& app);

class RenderExtractPlugin final : public Plugin {
public:
    void build(App& app) override { RegisterRenderExtract(app); }
};

} // namespace foh
