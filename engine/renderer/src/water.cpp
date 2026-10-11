#include "water.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>

#include "foh/log.h"

namespace foh {

namespace {

constexpr f32 kPiF = 3.14159265f;
constexpr f32 kGravity = 9.81f;
constexpr f64 kLoopSeconds = 200.0; // the simulated field repeats exactly after this long (frequencies are snapped to it)
constexpr f32 kRingCell[3] = {1.5f, 1.0f, 0.5f}; // level-0 cell size (m) per quality tier

// CPU mirror of WaterSimConstants in shaders/water_sim.slang.
struct SimConstants {
    Vec4 dims, time, lengths, bandLow, bandHigh, sea, swell, foam;
};
static_assert(sizeof(SimConstants) == 8 * 16, "SimConstants must match water_sim.slang");

WaterDesc Sanitize(WaterDesc d) {
    if (const char* e = std::getenv("FOH_WATER_FOAM")) d.foamThreshold = static_cast<f32>(std::atof(e)); // QA-TEMP
    if (const char* e = std::getenv("FOH_WATER_HEIGHT")) d.waveHeight = static_cast<f32>(std::atof(e)); // QA-TEMP
    if (const char* e = std::getenv("FOH_WATER_SWELL")) d.swellHeight = static_cast<f32>(std::atof(e)); // QA-TEMP
    if (const char* e = std::getenv("FOH_WATER_CHOP")) d.choppiness = static_cast<f32>(std::atof(e)); // QA-TEMP
    if (std::getenv("FOH_WATER_NOSSR")) d.reflections = false; // QA-TEMP
    if (const char* e = std::getenv("FOH_WATER_QUALITY")) d.quality = static_cast<u32>(std::atoi(e)); // QA-TEMP
    d.quality = std::min(d.quality, 2u);
    d.seaLevel = std::clamp(d.seaLevel, -50.0f, 50.0f);
    d.waveHeight = std::clamp(d.waveHeight, 0.0f, 8.0f);
    d.wavePeriod = std::clamp(d.wavePeriod, 2.0f, 16.0f);
    d.spread = std::clamp(d.spread, 0.25f, 8.0f);
    d.swellHeight = std::clamp(d.swellHeight, 0.0f, 6.0f);
    d.swellPeriod = std::clamp(d.swellPeriod, 5.0f, 20.0f);
    d.choppiness = std::clamp(d.choppiness, 0.0f, 1.5f);
    d.tileSize = std::clamp(d.tileSize, 128.0f, 2048.0f);
    d.absorption = glm::clamp(d.absorption, Vec3(0.0f), Vec3(5.0f));
    d.bodyColor = glm::clamp(d.bodyColor, Vec3(0.0f), Vec3(1.0f));
    d.turbidity = std::clamp(d.turbidity, 0.001f, 2.0f);
    d.refraction = std::clamp(d.refraction, 0.0f, 0.3f);
    d.sandBrightness = std::clamp(d.sandBrightness, 0.0f, 1.5f);
    d.foam = std::clamp(d.foam, 0.0f, 4.0f);
    d.foamThreshold = std::clamp(d.foamThreshold, -0.5f, 1.0f);
    d.foamPersistence = std::clamp(d.foamPersistence, 0.2f, 20.0f);
    d.shoreFoam = std::clamp(d.shoreFoam, 0.0f, 4.0f);
    d.shoalingDepth = std::clamp(d.shoalingDepth, 0.5f, 50.0f);
    return d;
}

bool SameWaves(const WaterDesc& a, const WaterDesc& b) {
    return a.quality == b.quality && a.windDirection == b.windDirection && a.waveHeight == b.waveHeight &&
           a.wavePeriod == b.wavePeriod && a.spread == b.spread && a.swellHeight == b.swellHeight &&
           a.swellPeriod == b.swellPeriod && a.swellDirection == b.swellDirection && a.tileSize == b.tileSize;
}

f32 Radians(f32 degrees) { return degrees * kPiF / 180.0f; }

nvrhi::BindingLayoutHandle MakeLayout(nvrhi::IDevice* dev, std::initializer_list<u32> srvSlots, std::initializer_list<u32> uavSlots) {
    nvrhi::BindingLayoutDesc ld;
    ld.visibility = nvrhi::ShaderType::Compute;
    ld.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
    for (u32 s : srvSlots) ld.addItem(nvrhi::BindingLayoutItem::Texture_SRV(s));
    for (u32 s : uavSlots) ld.addItem(nvrhi::BindingLayoutItem::Texture_UAV(s));
    return dev->createBindingLayout(ld);
}

nvrhi::TextureHandle MakeArray(nvrhi::IDevice* dev, nvrhi::Format format, u32 size, u32 layers, u32 mips, const char* name) {
    nvrhi::TextureDesc td;
    td.dimension = nvrhi::TextureDimension::Texture2DArray;
    td.width = size;
    td.height = size;
    td.arraySize = layers;
    td.mipLevels = mips;
    td.format = format;
    td.isUAV = true;
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true; // every command list assumes (and restores) the shader-resource state
    td.debugName = name;
    return dev->createTexture(td);
}

nvrhi::TextureSubresourceSet Mip(u32 mip, u32 layers) { return nvrhi::TextureSubresourceSet(mip, 1, 0, layers); }

} // namespace

std::vector<u16> BuildWaterGridIndices(bool ring) {
    constexpr u32 n = kWaterGridCells, stride = n + 1;
    std::vector<u16> out;
    out.reserve(n * n * 6);
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            if (ring && x >= kWaterHoleLo && x < kWaterHoleHi && y >= kWaterHoleLo && y < kWaterHoleHi) continue;
            const u16 i00 = static_cast<u16>(y * stride + x), i10 = static_cast<u16>(i00 + 1);
            const u16 i01 = static_cast<u16>(i00 + stride), i11 = static_cast<u16>(i01 + 1);
            // Seen from above (+Y), x to the right and z toward the viewer: counter-clockwise, like the mesh pipelines.
            out.insert(out.end(), {i00, i01, i10, i10, i01, i11});
        }
    }
    return out;
}

u32 WaterRingLevels(f32 cell, f32 farZ) {
    f32 extent = 0.5f * static_cast<f32>(kWaterGridCells) * cell;
    u32 levels = 1;
    while (extent < farZ && levels < 14) {
        extent *= 2.0f;
        ++levels;
    }
    return std::max(levels, 3u);
}

// ---- lifetime ------------------------------------------------------------------------------------------------------

bool Water::init(nvrhi::IDevice* device, ShaderLibrary* shaders, PipelineCache* pipelines) {
    m_device = device;
    m_shaders = shaders;
    m_pipelines = pipelines;
    m_desc = Sanitize(WaterDesc{});

    nvrhi::CommandListParameters cp;
    cp.enableImmediateExecution = false;
    m_cmd = device->createCommandList(cp);

    // Clipmap index lists (the vertices come from SV_VertexID).
    {
        const std::vector<u16> ring = BuildWaterGridIndices(true), center = BuildWaterGridIndices(false);
        m_ringIndexCount = static_cast<u32>(ring.size());
        m_centerIndexCount = static_cast<u32>(center.size());
        auto make = [&](const std::vector<u16>& idx, const char* name) {
            nvrhi::BufferDesc bd;
            bd.byteSize = idx.size() * sizeof(u16);
            bd.isIndexBuffer = true;
            bd.initialState = nvrhi::ResourceStates::IndexBuffer;
            bd.keepInitialState = true;
            bd.debugName = name;
            return device->createBuffer(bd);
        };
        m_ringIb = make(ring, "Water ring indices");
        m_centerIb = make(center, "Water center indices");
        m_cmd->open();
        m_cmd->writeBuffer(m_ringIb, ring.data(), ring.size() * sizeof(u16));
        m_cmd->writeBuffer(m_centerIb, center.data(), center.size() * sizeof(u16));
        m_cmd->close();
        device->executeCommandList(m_cmd);
    }

    nvrhi::SamplerDesc sd;
    sd.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap).setMaxAnisotropy(8.0f);
    m_waveSampler = device->createSampler(sd);

    nvrhi::BindingLayoutDesc sl;
    sl.visibility = nvrhi::ShaderType::All;
    sl.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
    sl.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1));
    for (u32 t = 0; t < 9; ++t) sl.addItem(nvrhi::BindingLayoutItem::Texture_SRV(t));
    sl.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    sl.addItem(nvrhi::BindingLayoutItem::Sampler(1));
    m_surfaceLayout = device->createBindingLayout(sl);

    nvrhi::BufferDesc cb;
    cb.byteSize = sizeof(SimConstants);
    cb.isConstantBuffer = true;
    cb.isVolatile = true;
    cb.maxVersions = 8;
    cb.debugName = "Water simulation constants";
    m_simCb = device->createBuffer(cb);

    // Slots match shaders/water_sim.slang: every stage has its own.
    m_lSpectrum = MakeLayout(device, {}, {0});
    m_lEvolve = MakeLayout(device, {1}, {1});
    m_lFft = MakeLayout(device, {2}, {2});
    m_lFinalize = MakeLayout(device, {3, 4}, {3, 4});
    m_lMip = MakeLayout(device, {5, 6}, {5, 6});

    uploadBed(); // creates the 1x1 stand-in
    m_gridReady = createSimResources();
    return m_gridReady;
}

void Water::shutdown() {
    releaseSimResources();
    m_ringIb = nullptr;
    m_centerIb = nullptr;
    m_waveSampler = nullptr;
    m_surfaceLayout = nullptr;
    m_simCb = nullptr;
    m_lSpectrum = m_lEvolve = m_lFft = m_lFinalize = m_lMip = nullptr;
    m_cmd = nullptr;
    m_bed = nullptr;
    m_gridReady = false;
}

void Water::releaseSimResources() {
    m_sSpectrum = m_sEvolve = m_sFftH = m_sFftV = nullptr;
    for (u32 p = 0; p < 2; ++p) {
        m_sFinalize[p] = nullptr;
        m_sMip[p].clear();
        m_slope[p] = nullptr;
    }
    m_h0 = m_fftA = m_fftB = m_disp = nullptr;
    m_simN = 0;
}

bool Water::createSimResources() {
    releaseSimResources();
    const u32 n = resolution();
    m_mipLevels = std::countr_zero(n) - 2; // down to 8 x 8
    m_h0 = MakeArray(m_device, nvrhi::Format::RG32_FLOAT, n, kWaterCascades, 1, "Water spectrum h0");
    m_fftA = MakeArray(m_device, nvrhi::Format::RG32_FLOAT, n, kWaterCascades * 4, 1, "Water FFT A");
    m_fftB = MakeArray(m_device, nvrhi::Format::RG32_FLOAT, n, kWaterCascades * 4, 1, "Water FFT B");
    m_disp = MakeArray(m_device, nvrhi::Format::RGBA16_FLOAT, n, kWaterCascades, m_mipLevels, "Water displacement");
    m_slope[0] = MakeArray(m_device, nvrhi::Format::RGBA16_FLOAT, n, kWaterCascades, m_mipLevels, "Water slope + foam A");
    m_slope[1] = MakeArray(m_device, nvrhi::Format::RGBA16_FLOAT, n, kWaterCascades, m_mipLevels, "Water slope + foam B");
    if (!m_h0 || !m_fftA || !m_fftB || !m_disp || !m_slope[0] || !m_slope[1]) return false;

    using B = nvrhi::BindingSetItem;
    auto set = [&](nvrhi::IBindingLayout* layout, std::initializer_list<nvrhi::BindingSetItem> items) {
        nvrhi::BindingSetDesc bd;
        bd.addItem(B::ConstantBuffer(0, m_simCb));
        for (const auto& it : items) bd.addItem(it);
        return m_device->createBindingSet(bd, layout);
    };
    const u32 all = kWaterCascades;

    m_sSpectrum = set(m_lSpectrum, {B::Texture_UAV(0, m_h0)});
    m_sEvolve = set(m_lEvolve, {B::Texture_SRV(1, m_h0), B::Texture_UAV(1, m_fftA)});
    m_sFftH = set(m_lFft, {B::Texture_SRV(2, m_fftA), B::Texture_UAV(2, m_fftB)});
    m_sFftV = set(m_lFft, {B::Texture_SRV(2, m_fftB), B::Texture_UAV(2, m_fftA)});
    for (u32 p = 0; p < 2; ++p) {
        m_sFinalize[p] = set(m_lFinalize, {B::Texture_SRV(3, m_fftA), B::Texture_SRV(4, m_slope[p ^ 1], nvrhi::Format::UNKNOWN, Mip(0, all)),
                                           B::Texture_UAV(3, m_disp, nvrhi::Format::UNKNOWN, Mip(0, all)),
                                           B::Texture_UAV(4, m_slope[p], nvrhi::Format::UNKNOWN, Mip(0, all))});
        m_sMip[p].clear();
        for (u32 l = 0; l + 1 < m_mipLevels; ++l) {
            m_sMip[p].push_back(set(m_lMip, {B::Texture_SRV(5, m_slope[p], nvrhi::Format::UNKNOWN, Mip(l, all)),
                                             B::Texture_SRV(6, m_disp, nvrhi::Format::UNKNOWN, Mip(l, all)),
                                             B::Texture_UAV(5, m_slope[p], nvrhi::Format::UNKNOWN, Mip(l + 1, all)),
                                             B::Texture_UAV(6, m_disp, nvrhi::Format::UNKNOWN, Mip(l + 1, all))}));
        }
    }
    m_simN = n;
    m_spectrumDirty = true;
    FOH_INFO("water: simulation {}^2 x {} cascades, {} mip levels", n, kWaterCascades, m_mipLevels);
    return true;
}

// ---- settings ------------------------------------------------------------------------------------------------------

void Water::set(const WaterDesc& desc) {
    const WaterDesc next = Sanitize(desc);
    if (!SameWaves(next, m_desc)) m_spectrumDirty = true;
    m_desc = next;
}

bool Water::setBed(const WaterBedDesc& bed) {
    if (bed.width == 0 || bed.height == 0 || bed.heights.size() != static_cast<size_t>(bed.width) * bed.height) return false;
    if (bed.size.x <= 0.0f || bed.size.y <= 0.0f) return false;
    m_bedData.assign(bed.heights.begin(), bed.heights.end());
    m_bedWidth = bed.width;
    m_bedHeight = bed.height;
    m_bedOrigin = bed.origin;
    m_bedSize = bed.size;
    m_bedDirty = true;
    return true;
}

void Water::clearBed() {
    m_bedData.clear();
    m_bedWidth = m_bedHeight = 0;
    m_bedDirty = true;
}

f32 Water::groundAt(f32 x, f32 z) const {
    if (m_bedData.empty()) return 0.0f;
    // Same mapping as the shader (uv * size - 0.5 = texel coordinate, bilinear, clamped).
    const f32 u = (x - m_bedOrigin.x) / m_bedSize.x * static_cast<f32>(m_bedWidth) - 0.5f;
    const f32 v = (z - m_bedOrigin.y) / m_bedSize.y * static_cast<f32>(m_bedHeight) - 0.5f;
    const f32 fu = std::clamp(u, 0.0f, static_cast<f32>(m_bedWidth - 1));
    const f32 fv = std::clamp(v, 0.0f, static_cast<f32>(m_bedHeight - 1));
    const u32 x0 = static_cast<u32>(fu), y0 = static_cast<u32>(fv);
    const u32 x1 = std::min(x0 + 1, m_bedWidth - 1), y1 = std::min(y0 + 1, m_bedHeight - 1);
    const f32 tx = fu - static_cast<f32>(x0), ty = fv - static_cast<f32>(y0);
    auto at = [&](u32 xi, u32 yi) { return m_bedData[static_cast<size_t>(yi) * m_bedWidth + xi]; };
    return glm::mix(glm::mix(at(x0, y0), at(x1, y0), tx), glm::mix(at(x0, y1), at(x1, y1), tx), ty);
}

void Water::uploadBed() {
    const bool has = !m_bedData.empty();
    const u32 w = has ? m_bedWidth : 1, h = has ? m_bedHeight : 1;
    nvrhi::TextureDesc td;
    td.width = w;
    td.height = h;
    td.format = nvrhi::Format::R32_FLOAT;
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true;
    td.debugName = "Water sea floor";
    nvrhi::TextureHandle tex = m_device->createTexture(td);
    const f32 zero = 0.0f;
    m_cmd->open();
    m_cmd->writeTexture(tex, 0, 0, has ? static_cast<const void*>(m_bedData.data()) : &zero, static_cast<size_t>(w) * sizeof(f32));
    m_cmd->close();
    m_device->executeCommandList(m_cmd);
    m_bed = tex;
    m_bedDirty = false;
}

// ---- simulation ----------------------------------------------------------------------------------------------------

f32 Water::tileLength(u32 cascade) const { return m_desc.tileSize / static_cast<f32>(1u << (2 * cascade)); }

void Water::dispatch(const ShaderRef& shader, nvrhi::IBindingLayout* layout, nvrhi::IBindingSet* set, u32 gx, u32 gy, u32 gz) {
    const u64 key = HashCombine(HashCombine(shader.uid, reinterpret_cast<uintptr_t>(layout)), 0x3A7Eu);
    nvrhi::IComputePipeline* pso = m_pipelines->compute(key, [&] {
        nvrhi::ComputePipelineDesc pd;
        pd.CS = shader.shader;
        pd.bindingLayouts = {layout};
        return pd;
    });
    if (!pso) return;
    nvrhi::ComputeState state;
    state.setPipeline(pso).addBindingSet(set);
    m_cmd->setComputeState(state);
    m_cmd->dispatch(gx, gy, gz);
}

void Water::simulate(bool wanted) {
    const auto now = std::chrono::steady_clock::now();
    m_dt = std::min(std::chrono::duration<f32>(now - m_lastSim).count(), 0.1f);
    m_lastSim = now;
    m_time += m_dt;
    if (m_bedDirty) uploadBed();
    if (!wanted || !active() || !fft()) return;
    if (m_simN != resolution() && !createSimResources()) return;

    const ShaderRef spectrum = m_shaders->get("water_sim.csSpectrum", nvrhi::ShaderType::Compute);
    const ShaderRef evolve = m_shaders->get("water_sim.csEvolve", nvrhi::ShaderType::Compute);
    const ShaderRef fftH = m_shaders->get("water_sim.csFftH", nvrhi::ShaderType::Compute);
    const ShaderRef fftV = m_shaders->get("water_sim.csFftV", nvrhi::ShaderType::Compute);
    const ShaderRef finalize = m_shaders->get("water_sim.csFinalize", nvrhi::ShaderType::Compute);
    const ShaderRef mip = m_shaders->get("water_sim.csMip", nvrhi::ShaderType::Compute);
    if (!spectrum || !evolve || !fftH || !fftV || !finalize || !mip) return; // shaders not built (yet)

    const u32 n = m_simN;
    SimConstants sc{};
    sc.dims = Vec4(static_cast<f32>(n), static_cast<f32>(std::countr_zero(n)), static_cast<f32>(kWaterCascades), 0.0f);
    sc.time = Vec4(static_cast<f32>(std::fmod(m_time, kLoopSeconds)), m_dt, m_desc.choppiness, static_cast<f32>(2.0 * 3.14159265358979 / kLoopSeconds));
    sc.lengths = Vec4(tileLength(0), tileLength(1), tileLength(2), 0.0f);
    // Each cascade owns a band of wavenumbers, so nothing is counted twice: up to 70% of what its texels can resolve.
    f32 high[3];
    for (u32 c = 0; c < 3; ++c) high[c] = c < 2 ? 0.7f * kPiF * static_cast<f32>(n) / tileLength(c) : 1.0e9f;
    sc.bandLow = Vec4(0.0f, high[0], high[1], 0.0f);
    sc.bandHigh = Vec4(high[0], high[1], high[2], 0.0f);
    sc.sea = Vec4(m_desc.waveHeight, 2.0f * kPiF / m_desc.wavePeriod, Radians(m_desc.windDirection), m_desc.spread);
    sc.swell = Vec4(m_desc.swellHeight, 2.0f * kPiF / m_desc.swellPeriod, Radians(m_desc.swellDirection), 0.0f);
    sc.foam = Vec4(m_desc.foamThreshold, 3.0f, std::exp(-m_dt / m_desc.foamPersistence), 1.0f);

    m_current ^= 1u;
    const u32 groups = n / 8;
    m_cmd->open();
    m_cmd->writeBuffer(m_simCb, &sc, sizeof(sc));
    if (m_spectrumDirty) {
        dispatch(spectrum, m_lSpectrum, m_sSpectrum, groups, groups, kWaterCascades);
        m_spectrumDirty = false;
    }
    dispatch(evolve, m_lEvolve, m_sEvolve, groups, groups, kWaterCascades);
    dispatch(fftH, m_lFft, m_sFftH, n, kWaterCascades * 4, 1); // one thread group per row and field
    dispatch(fftV, m_lFft, m_sFftV, n, kWaterCascades * 4, 1);
    dispatch(finalize, m_lFinalize, m_sFinalize[m_current], groups, groups, kWaterCascades);
    for (u32 l = 0; l + 1 < m_mipLevels; ++l) {
        const u32 g = std::max(1u, (n >> (l + 1)) / 8);
        dispatch(mip, m_lMip, m_sMip[m_current][l], g, g, kWaterCascades);
    }
    m_cmd->close();
    m_device->executeCommandList(m_cmd);
}

// ---- per-view constants --------------------------------------------------------------------------------------------

u32 Water::ringLevels(f32 farZ) const { return WaterRingLevels(kRingCell[m_desc.quality], farZ); }

void Water::fill(WaterConstants& c, u64 frame, f32 cloudAltitude) const {
    const WaterDesc& d = m_desc;
    const f32 cell = kRingCell[d.quality];
    const f32 windRad = Radians(d.windDirection);
    const f32 texel0 = m_simN ? tileLength(0) / static_cast<f32>(m_simN) : 1.0f;
    const bool hasBed = !m_bedData.empty();

    c = WaterConstants{};
    c.level = Vec4(d.seaLevel, static_cast<f32>(std::fmod(m_time, 36000.0)), d.choppiness, 0.0f);
    c.cascade = Vec4(tileLength(0), tileLength(1), tileLength(2), fft() ? 1.0f : 0.0f);
    c.grid = Vec4(cell, static_cast<f32>(kWaterGridCells), 0.0f /* ring levels: set per view */, texel0);
    c.absorb = Vec4(d.absorption, d.refraction);
    c.body = Vec4(d.bodyColor, d.turbidity);
    c.foam = Vec4(d.foam, d.shoreFoam, 1.6f, 1.4f);
    c.bed = Vec4(m_bedOrigin.x, m_bedOrigin.y, 1.0f / m_bedSize.x, 1.0f / m_bedSize.y);
    c.bedParams = Vec4(hasBed ? 1.0f : 0.0f, d.shoalingDepth, 200.0f, d.sandBrightness);
    const f32 steps = d.reflections ? (d.quality >= 2 ? 24.0f : d.quality == 1 ? 12.0f : 0.0f) : 0.0f;
    c.reflection = Vec4(steps, 1500.0f, 1.0f, cloudAltitude);
    c.misc = Vec4(static_cast<f32>(frame % 4096), static_cast<f32>(m_mipLevels > 0 ? m_mipLevels - 1 : 0), windRad, std::getenv("FOH_WATER_DEBUG") ? static_cast<f32>(std::atof(std::getenv("FOH_WATER_DEBUG"))) : 0.55f); // QA-TEMP

    // The low tier: six Gerstner waves standing in for the spectra. Amplitudes follow Hs^2 = 8 * sum(A^2).
    if (!fft()) {
        struct W {
            bool swell;
            f32 dirOffset, wavelengthScale, energy;
        };
        const W waves[kWaterGerstnerWaves] = {{true, 0.0f, 1.0f, 0.7f},    {true, 12.0f, 0.55f, 0.3f},   {false, 0.0f, 1.0f, 0.45f},
                                              {false, -22.0f, 0.55f, 0.3f}, {false, 28.0f, 0.3f, 0.18f}, {false, -8.0f, 0.16f, 0.07f}};
        for (u32 i = 0; i < kWaterGerstnerWaves; ++i) {
            const W& w = waves[i];
            const f32 period = w.swell ? d.swellPeriod : d.wavePeriod;
            const f32 height = w.swell ? d.swellHeight : d.waveHeight;
            const f32 length = kGravity * period * period / (2.0f * kPiF) * w.wavelengthScale;
            const f32 amplitude = std::sqrt(w.energy * height * height / 8.0f);
            const f32 dir = Radians((w.swell ? d.swellDirection : d.windDirection) + w.dirOffset);
            c.gerstner[i] = Vec4(std::cos(dir), std::sin(dir), amplitude, 2.0f * kPiF / length);
        }
    }
}

} // namespace foh
