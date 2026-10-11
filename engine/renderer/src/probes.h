#pragma once

// Dynamic light probes (docs/adr/0013-indirect-light-probes.md): a window of probes that follows the viewport's camera
// and is refreshed a few at a time, so the diffuse ambient light comes from what is actually around (canopy, walls,
// sunlit ground) instead of the sky model alone, at a cost that does not grow with the scene.
//
// Layout: a square window of kProbeGridDim x kProbeGridDim cells (RenderSettings::probeSpacing metres each), two layers
// of probes per cell, at fixed heights above the ground (so on a steep hillside the probes follow the slope). A probe
// owns the texel (cell mod window) of the grid textures: the window slides over the world while a probe that stays in
// it keeps its texel and its data. Each probe is captured by drawing the scene from its position into 6 faces of
// kProbeFace^2 texels, lighting them (shaders/probe_light.slang) and projecting the result onto first-order spherical
// harmonics (shaders/probe_sh.slang). The CPU decides which probes to refresh each frame: new cells first, nearest to
// the camera first, then the oldest.
//
// Cost (1080p, RTX 2060-class, estimate): per refreshed probe 6 culled scene draws of 32x32 pixels + a 6144-pixel
// lighting pass + one SH reduction; at the default 2 probes per frame about 0.2-0.4 ms, mostly draw submission.
//
// Thread-safety: main thread only (update() runs while the viewport graphs are built).

#include <algorithm>
#include <cmath>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "foh/math.h"
#include "foh/types.h"

namespace foh {

inline constexpr u32 kProbeGridDim = 24;      // cells per side of the window
inline constexpr u32 kProbeLayers = 2;        // probes per cell, stacked above the ground
inline constexpr u32 kProbeFace = 32;         // texels per capture face side (probe.slangh)
inline constexpr u32 kProbeFaces = 6;
inline constexpr u32 kProbeMaxPerFrame = 8;   // refreshes per frame, at most (probe.slangh)
inline constexpr f32 kProbeLayerHeight[kProbeLayers] = {1.5f, 6.0f};
inline constexpr f32 kProbeNear = 0.2f;
inline constexpr f32 kProbeFar = 70.0f;  // what is farther is sky: the captures are about the canopy and walls around the probe
inline constexpr u32 kProbeMinAgeFrames = 120; // a probe is not refreshed again sooner than this
inline constexpr u32 kEnvBaseSize = 128;       // texels per side of the sky environment image (kEnvSize in sky_probe.slang)

// CPU mirror of ProbeConstants in shaders/common/probe.slangh.
struct ProbeConstants {
    Mat4 invViewProj[kProbeMaxPerFrame * kProbeFaces];
    Vec4 origin[kProbeMaxPerFrame];
    u32 slot[kProbeMaxPerFrame][4];
    u32 count[4];
};
static_assert(sizeof(ProbeConstants) == 48 * 64 + 8 * 16 + 8 * 16 + 16, "ProbeConstants must match probe.slangh");

struct ProbeJob {
    u32 x = 0, z = 0, layer = 0; // texel in the grid textures (z = array slice)
    Vec3 position{0.0f};
    bool refresh = false;        // the texel already holds a capture of this very place
};

// The six face views of a probe at `position` (view and reverse-Z projection with a 90 degree field of view).
void ProbeFaceViews(const Vec3& position, Mat4 view[kProbeFaces], Mat4& proj);

// `ground(x, z)` is the terrain height; the grid is only meaningful where there is terrain.
class ProbeGrid {
public:
    // Moves the window to `camera` and picks up to `maxJobs` probes to capture this frame (appended to `jobs`).
    // A changed `spacing` throws every probe away.
    template <class GroundFn>
    void update(const Vec3& camera, f32 spacing, u64 frame, u32 maxJobs, GroundFn&& ground, std::vector<ProbeJob>& jobs);

    // World xz of the centre of the window (for the shader's fade-out at its border).
    Vec2 center() const { return m_center; }
    f32 spacing() const { return m_spacing; }
    // Forget everything captured (scene change, the textures were recreated).
    void reset();

    // The four SH coefficient textures (Texture2DArray RGBA16F, kProbeGridDim^2 x kProbeLayers). Created on demand.
    bool ensureTextures(nvrhi::IDevice* device);
    nvrhi::ITexture* texture(u32 i) const { return m_sh[i]; }
    // True once after creation: the textures hold garbage and must be cleared by a pass before first use.
    bool takeNeedsClear() {
        const bool v = m_needsClear;
        m_needsClear = false;
        return v;
    }
    void release();

private:
    struct Slot {
        i32 cellX = 0, cellZ = 0;
        u64 frame = 0;
        bool captured = false;
    };
    static i32 wrapToWindow(i32 index, i32 minCell) {
        const i32 dim = static_cast<i32>(kProbeGridDim);
        i32 m = (index - minCell) % dim;
        if (m < 0) m += dim;
        return minCell + m;
    }

    std::vector<Slot> m_slots; // [layer][z][x]
    f32 m_spacing = 0.0f;
    Vec2 m_center{0.0f};
    nvrhi::TextureHandle m_sh[4];
    bool m_needsClear = false;
};

template <class GroundFn>
void ProbeGrid::update(const Vec3& camera, f32 spacing, u64 frame, u32 maxJobs, GroundFn&& ground, std::vector<ProbeJob>& jobs) {
    if (m_slots.empty() || spacing != m_spacing) {
        m_slots.assign(static_cast<size_t>(kProbeGridDim) * kProbeGridDim * kProbeLayers, Slot{});
        m_spacing = spacing;
    }
    const i32 dim = static_cast<i32>(kProbeGridDim);
    const i32 minX = static_cast<i32>(std::floor(camera.x / spacing)) - dim / 2;
    const i32 minZ = static_cast<i32>(std::floor(camera.z / spacing)) - dim / 2;
    m_center = Vec2((static_cast<f32>(minX) + 0.5f * dim) * spacing, (static_cast<f32>(minZ) + 0.5f * dim) * spacing);

    struct Candidate {
        u32 index;
        f32 score; // lower = sooner
    };
    Candidate best[kProbeMaxPerFrame];
    u32 bestCount = 0;
    maxJobs = std::min(maxJobs, kProbeMaxPerFrame);
    if (maxJobs == 0) return; // a frame without captures: the window still moved
    auto consider = [&](u32 index, f32 score) {
        if (bestCount < maxJobs) {
            best[bestCount++] = {index, score};
        } else {
            u32 worst = 0;
            for (u32 i = 1; i < bestCount; ++i)
                if (best[i].score > best[worst].score) worst = i;
            if (score >= best[worst].score) return;
            best[worst] = {index, score};
        }
    };
    for (u32 layer = 0; layer < kProbeLayers; ++layer) {
        for (u32 z = 0; z < kProbeGridDim; ++z) {
            for (u32 x = 0; x < kProbeGridDim; ++x) {
                const u32 index = (layer * kProbeGridDim + z) * kProbeGridDim + x;
                const Slot& s = m_slots[index];
                const i32 cx = wrapToWindow(static_cast<i32>(x), minX);
                const i32 cz = wrapToWindow(static_cast<i32>(z), minZ);
                const f32 dx = (static_cast<f32>(cx) + 0.5f) * spacing - camera.x;
                const f32 dz = (static_cast<f32>(cz) + 0.5f) * spacing - camera.z;
                const f32 dist2 = dx * dx + dz * dz;
                const bool stale = !s.captured || s.cellX != cx || s.cellZ != cz;
                if (stale) {
                    consider(index, dist2); // new cells: nearest first
                } else if (frame >= s.frame + kProbeMinAgeFrames) {
                    // Old ones after every new one: by age, oldest first.
                    consider(index, 1.0e12f - static_cast<f32>(frame - s.frame));
                }
            }
        }
    }
    for (u32 i = 0; i < bestCount; ++i) {
        const u32 index = best[i].index;
        const u32 x = index % kProbeGridDim;
        const u32 z = (index / kProbeGridDim) % kProbeGridDim;
        const u32 layer = index / (kProbeGridDim * kProbeGridDim);
        Slot& s = m_slots[index];
        const i32 cx = wrapToWindow(static_cast<i32>(x), minX);
        const i32 cz = wrapToWindow(static_cast<i32>(z), minZ);
        ProbeJob job;
        job.x = x;
        job.z = z;
        job.layer = layer;
        job.refresh = s.captured && s.cellX == cx && s.cellZ == cz;
        const f32 px = (static_cast<f32>(cx) + 0.5f) * spacing, pz = (static_cast<f32>(cz) + 0.5f) * spacing;
        job.position = Vec3(px, ground(px, pz) + kProbeLayerHeight[layer], pz);
        s.cellX = cx;
        s.cellZ = cz;
        s.frame = frame;
        s.captured = true;
        jobs.push_back(job);
    }
}

} // namespace foh
