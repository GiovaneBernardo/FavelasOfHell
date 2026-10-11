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
#include <span>
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

// Texel formats of cooked textures (same values as foh::TexelFormat in the asset system).
enum class TextureFormat : u8 { Rgba8Unorm, Rgba8Srgb, Bc7Unorm, Bc7Srgb, Bc5Unorm, Bc4Unorm };

// A cooked texture on its way to the GPU: the whole mip chain, largest first, each mip exactly as the format
// stores it (BC blocks or RGBA8 rows). Block formats need a width and height that are multiples of 4.
struct TextureUpload {
    AssetId id;
    TextureFormat format = TextureFormat::Rgba8Unorm;
    u32 width = 0;
    u32 height = 0;
    std::vector<std::vector<u8>> mips;
};

struct TextureStats {
    u32 resident = 0;
    u32 queued = 0;
    u64 residentBytes = 0;
    u64 queuedBytes = 0;
};

// glTF metallic-roughness material. Texture references are asset ids; a texture that is not resident yet
// (or was never uploaded) shows as white / flat normal until it arrives.
struct MaterialParams {
    Vec4 baseColor{1.0f};
    Vec3 emissive{0.0f}; // reserved: the G-buffer has no emissive target yet
    f32 metallic = 0.0f;
    f32 roughness = 0.8f;
    f32 normalScale = 1.0f;
    f32 occlusionStrength = 1.0f;
    f32 alphaCutoff = 0.5f;
    bool alphaMask = false; // alpha-tested (also used for blended materials: there is no transparent pass yet)
    bool doubleSided = false;
    AssetId baseColorTexture, normalTexture, metalRoughTexture, occlusionTexture, emissiveTexture;
    // Foliage: double-sided leaves lit from both sides with the same (smooth, crown-shaped) normal instead of a flipped
    // one, with `translucency` letting light wrap round them and glow through when backlit.
    bool foliage = false;
    f32 translucency = 0.5f;
    // Terrain material (ADR 0012): when set, the single-material textures above are ignored and the surface blends four
    // ground layers in world space by slope, height and noise. Layer order: grass, soil, rock (triplanar), sand/shore.
    // A layer whose textures are not resident yet shows a plain colour.
    bool terrain = false;
    f32 terrainTileSize = 6.0f; // metres covered by one repeat of the layer textures
    AssetId terrainAlbedo[4], terrainNormal[4];
};

// CPU geometry of an imported mesh. All spans index the same vertices; normals and uvs
// may be empty (up vector / zero).
struct MeshGeometry {
    std::span<const Vec3> positions;
    std::span<const Vec3> normals;
    std::span<const Vec2> uvs;
    std::span<const Vec4> tangents; // xyz + bitangent sign; may be empty (normal maps then look flat-shaded)
    std::span<const u32> indices;
    // Skinning (empty for a static mesh, else one entry per vertex): four joint indices (one byte each, low byte
    // first) and four weights (unorm8, summing to 255) packed in a u32 each. Joints index the skeleton of the pose
    // that deforms the mesh (SkinnedMesh / SkinPalette).
    std::span<const u32> skinJoints;
    std::span<const u32> skinWeights;
};
// A draw range inside MeshGeometry::indices (one per submesh; MeshRenderer::submesh picks it).
struct MeshIndexRange {
    u32 firstIndex = 0;
    u32 indexCount = 0;
};

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
    Water = 1u << 2, // the sea (see WaterDesc); opt-in so scenes without it (previews) stay dry
    Probes = 1u << 3, // dynamic light probes around this viewport's camera (RenderSettings::probes); opt-in: each one costs captures
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

// Physical model behind the precomputed atmosphere (Bruneton & Neyret). Earth defaults; a change only takes effect
// through Renderer::generateAtmosphere, which rebuilds the lookup tables (and the cache file).
struct AtmosphereDesc {
    f32 bottomRadiusKm = 6360.0f; // planet radius (sea level)
    f32 topRadiusKm = 6420.0f;    // top of the atmosphere
    f32 rayleighScale = 1.0f;     // air density multiplier (blue sky strength)
    f32 mieScale = 1.0f;          // aerosol density multiplier (haze, humidity, pollution)
    f32 mieAnisotropy = 0.8f;     // Mie phase function asymmetry g, 0..0.99 (sun glow width)
    f32 ozoneScale = 1.0f;        // ozone absorption multiplier (deep blue twilight)
    Vec3 groundAlbedo{0.1f};      // bounce light of the terrain, part of the multiple scattering
    u32 scatteringOrders = 4;     // 1 = single scattering only, 4 is the reference default
};

// Volumetric clouds: one horizontal layer, ray marched at half resolution and lit by the sun through the
// atmosphere tables (so they redden at dusk and darken at night). Applies to every viewport with the Sky flag from
// the next frame. Only visible sky pixels cost anything; `quality` trades ray march steps for noise.
//
// The kind of sky comes from a weather preset (coverage, cloud type, storminess, thickness). The renderer eases from
// the current weather to the preset over `transitionSeconds`, and with `autoCycle` picks a new preset every
// `cycleSeconds`, so the sky changes character over time on its own.
enum class CloudPreset : u8 {
    Clear,         // a few small fair-weather cumulus
    Fair,          // scattered cumulus
    Broken,        // broken cumulus, big gaps
    Stratocumulus, // a low, lumpy, nearly closed deck
    Overcast,      // flat grey stratus
    Storm,         // dark, tall cumulonimbus
};
inline constexpr u32 kCloudPresetCount = 6;

struct CloudDesc {
    bool enabled = true;
    CloudPreset preset = CloudPreset::Fair; // the target weather (with autoCycle: where the cycle starts)
    bool autoCycle = true;
    f32 cycleSeconds = 240.0f;      // time between automatic weather changes
    f32 transitionSeconds = 60.0f;  // how long a change takes
    f32 coverageBias = 0.0f;        // added to the preset's coverage, -0.5..0.5
    f32 density = 1.0f;             // multiplier of the extinction: higher = darker, more opaque clouds
    f32 baseAltitude = 900.0f;      // m above sea level
    f32 thickness = 2600.0f;        // m (stratus uses the bottom fifth, cumulonimbus most of it)
    f32 windSpeed = 10.0f;          // m/s
    f32 windDirection = 40.0f;      // degrees in the ground plane (0 = +X, 90 = +Z)
    f32 cloudScale = 1.0f;          // feature size multiplier
    f32 detail = 0.6f;              // strength of the small-scale erosion, 0..1
    f32 ambient = 1.0f;             // multiplier of the sky-light part of the shading
    u32 quality = 1;                // 0 low, 1 medium, 2 high (ray march steps)
};

// The weather the sky is showing right now (between presets while a transition runs).
struct CloudWeather {
    f32 coverage = 0.0f; // 0 clear .. 1 overcast
    f32 type = 1.0f;     // 0 flat stratus .. 1 heaped cumulus
    f32 storm = 0.0f;    // 0 calm .. 1 cumulonimbus towers, dark undersides
    f32 density = 1.0f;
    CloudPreset target = CloudPreset::Fair;
};

// Water: one sea at `seaLevel` that extends to the horizon (a camera-centred clipmap, drawn with a pixel-shader-side
// depth test so terrain simply covers it: the shoreline is wherever the ground crosses the sea level). The waves
// are a Tessendorf FFT simulation in three cascades (wind sea + swell, JONSWAP spectra) with persistent foam, run
// once per frame for every viewport; the low quality tier replaces it with six analytic Gerstner waves.
// Applies to every viewport from the next frame.
struct WaterDesc {
    bool enabled = true;
    // 0 low: Gerstner waves, no screen-space reflections. 1 medium: FFT 128^2 x 3, 12-step SSR. 2 high: FFT 256^2 x 3, 24-step SSR.
    u32 quality = 2;
    f32 seaLevel = 0.0f;         // m (tide: change it live)
    // Wind sea
    f32 windDirection = 40.0f;   // degrees in the ground plane (0 = +X, 90 = +Z); waves travel toward it
    f32 waveHeight = 0.9f;       // significant wave height of the wind sea (m)
    f32 wavePeriod = 6.5f;       // peak period (s); the wavelength follows from it (about 66 m at 6.5 s)
    f32 spread = 1.0f;           // directional narrowness: 1 = natural, higher = more aligned waves
    // Swell: long, regular waves from far away
    f32 swellHeight = 1.0f;      // m (0 = none)
    f32 swellPeriod = 11.0f;     // s
    f32 swellDirection = 20.0f;  // degrees
    f32 choppiness = 0.9f;       // sharpness of the crests (horizontal displacement), 0..1.5
    f32 tileSize = 512.0f;       // m: the largest cascade repeats at this distance
    // Look
    Vec3 absorption{0.40f, 0.065f, 0.030f}; // 1/m per channel: red is gone within metres, blue lasts
    Vec3 bodyColor{0.045f, 0.20f, 0.19f};   // colour of the light the water scatters back (albedo)
    f32 turbidity = 0.18f;       // scattering density (1/m): higher = murkier, greener near shore
    f32 refraction = 0.05f;      // how much the surface bends the view of what is below
    f32 sandBrightness = 0.55f;  // floor colour where no terrain is drawn below the water
    f32 foam = 1.0f;             // whitecaps
    f32 foamThreshold = 0.0f;    // wave folding needed before foam appears (0 = only the sharpest crests)
    f32 foamPersistence = 3.0f;  // seconds foam takes to fade
    f32 shoreFoam = 1.0f;        // breaker zone and swash line at the shore
    f32 shoalingDepth = 6.0f;    // waves lose height below this depth (m)
    bool reflections = true;     // screen-space reflections (the sky probe is always used)
};

// The sea floor / terrain height used by the water for shoaling, the breaker zone and the colour of shallow water.
// Heights in metres, row-major, `width` x `height` samples covering [origin, origin + size) in the ground plane.
// The real terrain will provide the same data; until then any height field works (see the editor's water test scene).
struct WaterBedDesc {
    Vec2 origin{0.0f};
    Vec2 size{1.0f};
    u32 width = 0;
    u32 height = 0;
    std::span<const f32> heights;
};

struct AtmosphereStatus {
    enum class Phase : u8 {
        Missing,    // no tables yet: the sky falls back to a plain gradient
        Generating, // compute passes submitted, waiting for the GPU
        Saving,     // tables usable, cache file being written in the background
        Ready,
    };
    Phase phase = Phase::Missing;
    bool ready = false;     // the sky and lighting use the tables
    bool fromCache = false; // the tables in use were loaded from the cache file
    f32 lastGenerateMs = 0.0f; // wall time of the last generation, from request until the GPU was done
    u64 lutBytes = 0;          // GPU memory of the three tables
    u64 cacheBytes = 0;        // size of the cache file on disk (0: none)
    std::string cachePath;
    std::string message; // what happened last (loaded / generated / error)
};

// Display transform from HDR to the output (shaders/tonemap.slang mirrors the order).
enum class Tonemapper : u8 {
    Linear,           // clamp only: for debugging the scene values
    Reinhard,         // c / (1 + c) per channel: soft, desaturates highlights, never reaches white
    ReinhardExtended, // Reinhard on the luminance with a white point
    Uncharted2,       // Hable's filmic curve
    Aces,             // Narkowicz ACES fit: contrasty, the classic filmic look
    PbrNeutral,       // Khronos PBR Neutral: faithful colors below ~0.8, only compresses highlights
};
inline constexpr u32 kTonemapperCount = 6;

// Quality and pass switches, applied to every viewport from the next frame (editor: Render Graph panel).
struct RenderSettings {
    bool shadows = true;           // ShadowCascades pass (off: the pass only clears a 1x1 stand-in)
    u32 shadowCascadeSize = 2048;  // texels per cascade side (four cascades, one 2x2 atlas)
    f32 shadowDistance = 250.0f;   // meters from the camera; farther surfaces are unshadowed
    bool atmosphere = true;        // sky, sun color and ambient from the atmosphere tables (off: gradient sky, constant ambient)
    bool aerialPerspective = true; // distance haze on lit pixels (needs `atmosphere`)
    f32 exposure = 1.0f;           // linear multiplier before tonemapping (used when autoExposure is off)
    bool dither = true;            // sub-LSB noise on the output against 8-bit banding

    Tonemapper tonemapper = Tonemapper::Aces;
    f32 whitePoint = 4.0f;         // scene luminance that maps to white (ReinhardExtended, Uncharted2)

    // Auto exposure: luminance histogram -> average between two percentiles -> adapts toward middle grey (per viewport).
    bool autoExposure = true;
    f32 exposureCompensation = 0.0f; // EV added to the automatic exposure
    f32 exposureMinLuminance = -8.0f; // log2 of the darkest / brightest scene luminance the histogram resolves
    f32 exposureMaxLuminance = 8.0f;
    f32 adaptBrighten = 3.0f;      // adaptation speed (1/s) when the scene gets brighter (exposure drops)
    f32 adaptDarken = 1.0f;        // ... when it gets darker (exposure rises): the eye is slower here
    f32 meterCenterWeight = 0.5f;  // 0 = every pixel counts the same, 1 = weight falls to zero at the corners

    // Bloom (Call of Duty: Advanced Warfare): blurred downsample chain, tent-filtered upsample with accumulation.
    bool bloom = true;
    f32 bloomStrength = 0.06f;     // blend between the scene and the bloom chain (energy conserving)
    u32 bloomLevels = 6;           // downsample steps (1..8); more = a wider glow

    // Ambient occlusion (GTAO, Jimenez et al. 2016): darkens the ambient and sky-reflected light in creases and under
    // overhangs. Screen space: it only knows what the depth buffer shows.
    bool gtao = true;
    f32 gtaoRadius = 1.5f;         // metres of the search around each pixel
    f32 gtaoStrength = 1.2f;       // exponent applied to the visibility (higher = darker)

    // Screen-space sun shadows over a short distance, for what the shadow cascades are too coarse to resolve.
    bool contactShadows = true;
    f32 contactShadowLength = 0.8f; // metres toward the sun that are searched

    // Sun light that gets through the first layer of leaves above a surface (a shaded forest floor is green and
    // dappled, not black). 0 = off; scales the light, which is tinted by the leaf colour.
    f32 canopyTransmission = 0.6f;

    // Sky image-based lighting: the sky and clouds around the camera are reflected by every surface, rough ones blurrier.
    bool ibl = true;
    f32 iblIntensity = 1.0f;

    // Light probes (ViewportFlags::Probes): ambient light measured from the scene around the camera. They need the
    // terrain height field (Renderer::setWaterBed) to sit at a fixed height above the ground.
    bool probes = true;
    f32 probeSpacing = 4.0f;       // metres between probes (the window is 24 probes wide)
    u32 probesPerFrame = 1;        // probes refreshed per refresh frame (1..8): more = the lighting follows changes faster
    u32 probeInterval = 2;         // a refresh happens every this many frames (1 = every frame)
    f32 probeIntensity = 1.0f;     // multiplier of the probes' contribution

    bool gpuTimers = false;        // measure the GPU time of every pass of ONE viewport (setGpuTimerViewport; ViewportDebugInfo::gpu, a few frames late)
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
    // Imported textures are uploaded in the background: at most this many bytes of texture data per frame.
    u64 textureUploadBudget = 32ull << 20;
    // Initial values of RenderSettings::shadowCascadeSize / shadowDistance.
    u32 shadowCascadeSize = 2048;
    f32 shadowDistance = 250.0f; // meters from the camera; farther surfaces are unshadowed
    std::filesystem::path shaderDirectory; // default: <exe dir>/shaders
    // Generated data that is expensive to rebuild (the atmosphere tables). Default: <exe dir>/cache.
    std::filesystem::path cacheDirectory;
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
    struct Timing {
        const char* name;
        float ms;
    };
    std::vector<Timing> gpu; // RenderSettings::gpuTimers: GPU time per pass of a recent frame
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

    // ---- imported meshes ----
    // Uploads (or, for an asset that is already uploaded, replaces: hot reload) the geometry of
    // an imported mesh. MeshRenderer components that reference `asset` start drawing it with the
    // next extraction. False if the geometry is inconsistent (nothing changes then).
    bool uploadMeshAsset(const AssetId& asset, const MeshGeometry& geometry, std::span<const MeshIndexRange> submeshes);
    void releaseMeshAsset(const AssetId& asset);
    bool hasMeshAsset(const AssetId& asset) const;

    // ---- imported textures and materials ----
    // Queues a texture for upload (replaces an earlier one with the same id: hot reload). It becomes visible
    // once fully resident, a few frames later for big textures. False if the data is malformed.
    bool uploadTexture(TextureUpload&& upload);
    void releaseTexture(const AssetId& id);
    bool hasTexture(const AssetId& id) const; // resident (usable), not merely queued
    // Creates or updates a material. MeshRenderer::material = `id` then draws with it.
    void setMaterial(const AssetId& id, const MaterialParams& params);
    void releaseMaterial(const AssetId& id);
    bool hasMaterial(const AssetId& id) const;
    TextureStats textureStats() const;

    // ---- settings ----
    void setRenderSettings(const RenderSettings& settings);
    const RenderSettings& renderSettings() const;

    // ---- atmosphere ----
    // The sky, the sun's color and the ambient light come from lookup tables precomputed on the GPU. They are
    // loaded from the cache file at start-up and generated (then cached) once if there is none. This rebuilds
    // them with new parameters: the compute passes are submitted at the start of the next frame, the tables
    // switch over without stalling, and the cache file is rewritten in the background. Ignored while a
    // generation is still running (check atmosphereStatus().phase).
    void generateAtmosphere(const AtmosphereDesc& desc);
    // The parameters the tables in use were generated with.
    const AtmosphereDesc& atmosphereDesc() const;
    AtmosphereStatus atmosphereStatus() const;

    // ---- clouds ----
    void setClouds(const CloudDesc& desc);
    const CloudDesc& clouds() const;
    CloudWeather cloudWeather() const;

    // ---- water ----
    void setWater(const WaterDesc& desc);
    const WaterDesc& water() const;
    // False (nothing changes) if the sample count does not match width * height.
    bool setWaterBed(const WaterBedDesc& bed);
    void clearWaterBed();

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
    // The viewport whose passes get GPU timers when RenderSettings::gpuTimers is on (NVRHI has few timer queries).
    void setGpuTimerViewport(ViewportId id);
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
