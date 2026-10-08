#pragma once

// The renderer's public face: scenes, viewports and the frame loop. No NVRHI
// here (only forward declarations), so game modules may include it.
//
// Model:
//   Scene     the renderer-side copy of ONE ECS world's render data (RenderScene),
//             filled each frame by RenderExtractPlugin.
//   Viewport  an independent render target + camera + scene reference + its own
//             render graph. The editor's scene view, the game view and a debug
//             scene's view are three viewports; any number may exist. Several
//             viewports may show the same scene, and each can show a different one.
//   Frame     beginFrame -> (update UI, move cameras) -> renderViewports ->
//             renderWindow (UI / present) -> swapchain.present -> endFrame.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "foh/math.h"
#include "foh/platform.h"
#include "foh/renderer/render_scene.h"
#include "foh/types.h"

namespace nvrhi {
class ITexture;
}

namespace foh {

class GraphicsDevice;
class JobSystem;
class PipelineCache;
class RenderGraph;
class ShaderLibrary;
class Swapchain;
struct RgTexture;

struct SceneId {
    u32 value = 0;
    constexpr bool valid() const { return value != 0; }
    friend constexpr bool operator==(SceneId, SceneId) = default;
};
struct ViewportId {
    u32 value = 0;
    constexpr bool valid() const { return value != 0; }
    friend constexpr bool operator==(ViewportId, ViewportId) = default;
};

enum class ViewportFlags : u32 {
    None = 0,
    Sky = 1u << 0,  // procedural sky background (otherwise a flat clear color)
    Grid = 1u << 1, // editor ground grid
};
constexpr ViewportFlags operator|(ViewportFlags a, ViewportFlags b) {
    return static_cast<ViewportFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr bool HasFlag(ViewportFlags set, ViewportFlags f) { return (static_cast<u32>(set) & static_cast<u32>(f)) != 0; }

// Camera matrices. Projection is reverse-Z (near -> 1, far -> 0) with a 32-bit
// float depth buffer: the right precision distribution for a 1.5 km world.
struct RenderViewParams {
    Mat4 view{1.0f};
    Mat4 proj{1.0f};
    Vec3 position{0.0f};
    f32 nearZ = 0.1f;
    f32 farZ = 4000.0f;
};

FOH_API Mat4 MakePerspectiveReverseZ(f32 fovY, f32 aspect, f32 nearZ, f32 farZ);
// `cameraWorld` is the camera's world matrix (it looks down its local -Z).
FOH_API RenderViewParams MakeViewParams(const Mat4& cameraWorld, f32 fovY, f32 aspect, f32 nearZ, f32 farZ);

struct ViewportDesc {
    std::string name = "Viewport";
    u32 width = 1280;
    u32 height = 720;
    SceneId scene;
    // Entity::id of the camera to render with; 0 = the scene's first active camera.
    u64 cameraEntity = 0;
    ViewportFlags flags = ViewportFlags::Sky;
};

struct RendererDesc {
    JobSystem* jobs = nullptr; // not owned; recording and per-viewport work run on it
    bool validation =
#if defined(NDEBUG)
        false;
#else
        true;
#endif
    u32 framesInFlight = 2;
    // A render graph with at most this many passes is recorded into ONE command list; bigger
    // ones are split into groups recorded in parallel. 1 puts every pass in its own command
    // list (QA: exercises the cross-command-list barrier planning).
    u32 minPassesPerGroup = 4;
    std::filesystem::path shaderDirectory; // default: <exe dir>/shaders
};

// For the render graph debugger panel.
struct ViewportDebugInfo {
    struct Pass {
        const char* name;
        bool culled;
        u32 group;
    };
    std::string name;
    u32 width = 0, height = 0;
    u32 instances = 0;
    u32 groups = 0;
    u32 physicalTextures = 0;
    u32 transientTextures = 0;
    u32 barriers = 0;
    std::vector<Pass> passes;
};

// Thread-safety: main thread, except where noted. Internally the renderer runs
// viewport recording on the job system.
class FOH_API Renderer {
public:
    // Null (with the reason logged) when no GPU/API/shaders are available.
    static std::unique_ptr<Renderer> create(const RendererDesc& desc);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // ---- scenes ----
    SceneId createScene(const char* name);
    void destroyScene(SceneId id);
    RenderScene* scene(SceneId id);

    // ---- viewports ----
    ViewportId createViewport(const ViewportDesc& desc);
    void destroyViewport(ViewportId id);
    // Takes effect at the next frame (the target is recreated lazily).
    void setViewportSize(ViewportId id, u32 width, u32 height);
    void setViewportScene(ViewportId id, SceneId scene);
    void setViewportCameraEntity(ViewportId id, u64 entityId);
    void setViewportFlags(ViewportId id, ViewportFlags flags);
    // Render with these matrices instead of a scene camera (editor cameras).
    void setViewportCamera(ViewportId id, const RenderViewParams& params);
    void clearViewportCamera(ViewportId id);
    // Inactive viewports (hidden panels) cost nothing.
    void setViewportActive(ViewportId id, bool active);
    u32 viewportWidth(ViewportId id) const;
    u32 viewportHeight(ViewportId id) const;
    // The finished sRGB image. Always in the shader-resource state between frames.
    nvrhi::ITexture* viewportTexture(ViewportId id) const;
    // Use when the viewport's own camera drives aspect ratio, e.g. for UI layout.
    float viewportAspect(ViewportId id) const;
    bool viewportDebugInfo(ViewportId id, ViewportDebugInfo& out) const;

    // ---- picking ----
    // Asks which entity is under pixel (x, y) of the viewport's own image (origin
    // top-left). The viewport renders an entity-id pass on the NEXT frame only, then
    // copies that one texel back; the answer is available a few frames later through
    // takePickResult. Requests while one is pending are ignored. Cost on all other
    // frames: none.
    void requestPick(ViewportId id, u32 x, u32 y);
    // True exactly once per request when the answer is ready; `entityId` is
    // Entity::id of the picked mesh, or 0 if nothing was under the pixel.
    bool takePickResult(ViewportId id, u64& entityId);

    // ---- frame ----
    // Waits for the GPU if it is too far behind, hot reloads changed shaders,
    // publishes the extracted scenes and applies pending viewport resizes.
    void beginFrame();
    // Builds, compiles and records one render graph per active viewport (in
    // parallel on the job system), then submits them in viewport creation order.
    void renderViewports();

    // Renders into a window: imports the backbuffer into a fresh graph, lets
    // `build` add passes (UI, blits) and executes it. `build` runs on this thread.
    using WindowPassFn = void (*)(RenderGraph& graph, RgTexture backbuffer, void* user);
    void renderWindow(Swapchain& swapchain, WindowPassFn build, void* user);
    // Convenience pass for players without UI: copies a viewport onto the backbuffer.
    void addPresentPass(RenderGraph& graph, RgTexture backbuffer, ViewportId viewport);
    // Imports a viewport's image for sampling in a window graph (UI).
    RgTexture importViewportTexture(RenderGraph& graph, ViewportId viewport);

    void endFrame();

    // ---- services ----
    GraphicsDevice& device();
    ShaderLibrary& shaders();
    PipelineCache& pipelines();
    JobSystem* jobs() const;
    u64 frameIndex() const;

    struct Impl; // opaque (src/renderer_impl.h)

private:
    Renderer();
    std::unique_ptr<Impl> m_impl;
};

} // namespace foh
