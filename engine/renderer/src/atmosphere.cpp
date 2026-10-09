#include "atmosphere.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <type_traits>

#include "foh/log.h"

namespace foh {

namespace {

// LUT sizes. MIRRORED by shaders/common/atmosphere_core.slangh.
constexpr u32 kTransmittanceW = 256, kTransmittanceH = 64;
constexpr u32 kScatteringW = 256, kScatteringH = 128, kScatteringD = 32; // (nu 8 x mu_s 32) x mu 128 x r 32
constexpr u32 kIrradianceW = 64, kIrradianceH = 16;
constexpr u32 kTexelBytes = 8; // RGBA16F
constexpr u64 kTransmittanceBytes = u64(kTransmittanceW) * kTransmittanceH * kTexelBytes;
constexpr u64 kScatteringBytes = u64(kScatteringW) * kScatteringH * kScatteringD * kTexelBytes;
constexpr u64 kIrradianceBytes = u64(kIrradianceW) * kIrradianceH * kTexelBytes;

constexpr f32 kSunAngularRadius = 0.00935f / 2.0f;     // radians
constexpr f32 kMaxSunZenithAngle = 102.0f * 3.14159265f / 180.0f; // the tables cover the sun this far below the horizon
constexpr u32 kSlicesPerDispatch = 8; // the heavy passes run in slices of the r axis so no single dispatch nears the GPU timeout

// Bump when the shaders or this file change what the tables contain: a cache file from another version is ignored.
constexpr u32 kCacheVersion = 1;

struct CacheHeader {
    char magic[8];
    u32 version;
    u32 sizes[7];
    AtmosphereDesc desc;
    u64 bytes[3]; // transmittance, scattering, irradiance
};
static_assert(std::is_trivially_copyable_v<AtmosphereDesc>, "AtmosphereDesc is written to the cache file as is");

constexpr u32 kExpectedSizes[7] = {kTransmittanceW, kTransmittanceH, kScatteringW, kScatteringH, kScatteringD, kIrradianceW, kIrradianceH};

// CPU mirror of PrecomputeConstants in shaders/atmosphere_precompute.slang.
struct PrecomputeConstants {
    Vec4 radii, rayleigh, mieScat, mieExt, ozone, ground, solar;
    u32 pass[4];
};
static_assert(sizeof(PrecomputeConstants) == 8 * 16, "must match atmosphere_precompute.slang");

// Earth, RGB at roughly 680 / 550 / 440 nm (per km), after Hillaire's production-ready values.
const Vec3 kRayleighScattering{5.802e-3f, 13.558e-3f, 33.1e-3f};
constexpr f32 kRayleighScaleHeight = 8.0f;
constexpr f32 kMieScattering = 3.996e-3f;
constexpr f32 kMieSingleScatteringAlbedo = 0.9f;
constexpr f32 kMieScaleHeight = 1.2f;
const Vec3 kOzoneAbsorption{0.650e-3f, 1.881e-3f, 0.085e-3f};

AtmosphereDesc Sanitize(AtmosphereDesc d) {
    d.bottomRadiusKm = std::clamp(d.bottomRadiusKm, 1000.0f, 20000.0f);
    d.topRadiusKm = std::clamp(d.topRadiusKm, d.bottomRadiusKm + 10.0f, d.bottomRadiusKm + 200.0f);
    d.rayleighScale = std::clamp(d.rayleighScale, 0.0f, 10.0f);
    d.mieScale = std::clamp(d.mieScale, 0.0f, 20.0f);
    d.mieAnisotropy = std::clamp(d.mieAnisotropy, 0.0f, 0.99f);
    d.ozoneScale = std::clamp(d.ozoneScale, 0.0f, 10.0f);
    d.groundAlbedo = glm::clamp(d.groundAlbedo, Vec3(0.0f), Vec3(1.0f));
    d.scatteringOrders = std::clamp(d.scatteringOrders, 1u, 6u);
    return d;
}

PrecomputeConstants MakeConstants(const AtmosphereDesc& d) {
    PrecomputeConstants pc{};
    pc.radii = Vec4(d.bottomRadiusKm, d.topRadiusKm, kSunAngularRadius, std::cos(kMaxSunZenithAngle));
    pc.rayleigh = Vec4(kRayleighScattering * d.rayleighScale, kRayleighScaleHeight);
    pc.mieScat = Vec4(Vec3(kMieScattering * d.mieScale), kMieScaleHeight);
    pc.mieExt = Vec4(Vec3(kMieScattering / kMieSingleScatteringAlbedo * d.mieScale), d.mieAnisotropy);
    pc.ozone = Vec4(kOzoneAbsorption * d.ozoneScale, 0.0f);
    pc.ground = Vec4(d.groundAlbedo, 0.0f);
    pc.solar = Vec4(1.0f); // the tables are per unit of sun irradiance; the sun color scales them at runtime
    return pc;
}

u64 FileSize(const std::filesystem::path& p) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(p, ec);
    return ec ? 0 : static_cast<u64>(n);
}

// A set of textures for one dispatch of the precompute layout; null slots get a stand-in.
struct Slots {
    nvrhi::ITexture* srv[8] = {};
    nvrhi::ITexture* uav[5] = {};
};

} // namespace

// Readback of the finished tables, waiting for the GPU.
struct Atmosphere::Pack {
    nvrhi::BufferHandle gpu[3], readback[3];
    nvrhi::EventQueryHandle query;
    AtmosphereDesc desc;
};

struct Atmosphere::SaveJob {
    std::filesystem::path file;
    std::vector<u8> blob; // header + the three tables
    JobCounter counter;
    bool ok = false;
};

Atmosphere::Atmosphere() = default;
Atmosphere::~Atmosphere() { shutdown(); }

void Atmosphere::shutdown() {
    if (m_save && m_jobs) m_jobs->wait(m_save->counter);
    m_save.reset();
    m_pack.reset();
    for (auto& t : m_dummySrv) t = nullptr;
    for (auto& t : m_dummyUav) t = nullptr;
    m_dummyPackSrv3D = nullptr;
    m_transmittance = nullptr;
    m_scattering = nullptr;
    m_irradiance = nullptr;
    m_sampler = nullptr;
    m_layoutPrecompute = nullptr;
    m_layoutPack = nullptr;
    m_ready = false;
}

bool Atmosphere::init(nvrhi::IDevice* device, ShaderLibrary* shaders, PipelineCache* pipelines, JobSystem* jobs,
                      std::filesystem::path cacheFile) {
    m_device = device;
    m_shaders = shaders;
    m_pipelines = pipelines;
    m_jobs = jobs;
    m_cacheFile = std::move(cacheFile);
    m_active = Sanitize(AtmosphereDesc{});

    auto makeTable = [&](nvrhi::TextureDimension dim, u32 w, u32 h, u32 d, const char* name) {
        nvrhi::TextureDesc td;
        td.dimension = dim;
        td.width = w;
        td.height = h;
        td.depth = d;
        td.format = nvrhi::Format::RGBA16_FLOAT;
        td.isUAV = true;
        td.initialState = nvrhi::ResourceStates::ShaderResource;
        td.keepInitialState = true; // every command list assumes (and restores) the shader-resource state
        td.debugName = name;
        return device->createTexture(td);
    };
    m_transmittance = makeTable(nvrhi::TextureDimension::Texture2D, kTransmittanceW, kTransmittanceH, 1, "Atmosphere transmittance");
    m_scattering = makeTable(nvrhi::TextureDimension::Texture3D, kScatteringW, kScatteringH, kScatteringD, "Atmosphere scattering");
    m_irradiance = makeTable(nvrhi::TextureDimension::Texture2D, kIrradianceW, kIrradianceH, 1, "Atmosphere irradiance");

    nvrhi::SamplerDesc sd;
    sd.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    m_sampler = device->createSampler(sd);

    nvrhi::BindingLayoutDesc ld;
    ld.visibility = nvrhi::ShaderType::Compute;
    ld.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
    for (u32 i = 0; i < 8; ++i) ld.addItem(nvrhi::BindingLayoutItem::Texture_SRV(i));
    ld.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    for (u32 i = 0; i < 5; ++i) ld.addItem(nvrhi::BindingLayoutItem::Texture_UAV(i));
    m_layoutPrecompute = device->createBindingLayout(ld);

    nvrhi::BindingLayoutDesc pl;
    pl.visibility = nvrhi::ShaderType::Compute;
    pl.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    pl.addItem(nvrhi::BindingLayoutItem::Texture_SRV(1));
    pl.addItem(nvrhi::BindingLayoutItem::RawBuffer_UAV(0));
    m_layoutPack = device->createBindingLayout(pl);

    createDummies();

    if (!load()) request(AtmosphereDesc{});
    return true;
}

void Atmosphere::createDummies() {
    // Dimension of each slot as the precompute shader declares it.
    constexpr bool kSrv3D[8] = {false, true, true, true, true, false, true, false};
    constexpr bool kUav3D[5] = {false, true, true, false, true};
    auto make = [&](bool volume, bool uav, const char* name) {
        nvrhi::TextureDesc td;
        td.dimension = volume ? nvrhi::TextureDimension::Texture3D : nvrhi::TextureDimension::Texture2D;
        td.format = nvrhi::Format::RGBA32_FLOAT;
        td.isUAV = uav;
        td.initialState = nvrhi::ResourceStates::ShaderResource;
        td.keepInitialState = true;
        td.debugName = name;
        return m_device->createTexture(td);
    };
    for (u32 i = 0; i < 8; ++i) m_dummySrv[i] = make(kSrv3D[i], false, "Atmosphere stand-in (SRV)");
    for (u32 i = 0; i < 5; ++i) m_dummyUav[i] = make(kUav3D[i], true, "Atmosphere stand-in (UAV)");
    m_dummyPackSrv3D = make(true, false, "Atmosphere stand-in (pack)");
}

// ---- cache ------------------------------------------------------------------------------------------

bool Atmosphere::load() {
    std::ifstream in(m_cacheFile, std::ios::binary);
    if (!in) {
        m_message = "no cache file, generating";
        return false;
    }
    CacheHeader h{};
    in.read(reinterpret_cast<char*>(&h), sizeof(h));
    const bool headerOk = in && std::memcmp(h.magic, "FOHATM\0", 8) == 0 && h.version == kCacheVersion &&
                          std::memcmp(h.sizes, kExpectedSizes, sizeof(kExpectedSizes)) == 0 &&
                          h.bytes[0] == kTransmittanceBytes && h.bytes[1] == kScatteringBytes && h.bytes[2] == kIrradianceBytes;
    if (!headerOk) {
        m_message = "cache file is from another version, regenerating";
        return false;
    }
    std::vector<u8> data(kTransmittanceBytes + kScatteringBytes + kIrradianceBytes);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!in) {
        m_message = "cache file is truncated, regenerating";
        return false;
    }

    nvrhi::CommandListParameters cp;
    cp.enableImmediateExecution = false;
    nvrhi::CommandListHandle cmd = m_device->createCommandList(cp);
    cmd->open();
    const u8* p = data.data();
    cmd->writeTexture(m_transmittance, 0, 0, p, kTransmittanceW * kTexelBytes);
    p += kTransmittanceBytes;
    cmd->writeTexture(m_scattering, 0, 0, p, kScatteringW * kTexelBytes, u64(kScatteringW) * kScatteringH * kTexelBytes);
    p += kScatteringBytes;
    cmd->writeTexture(m_irradiance, 0, 0, p, kIrradianceW * kTexelBytes);
    cmd->close();
    m_device->executeCommandList(cmd);

    m_active = Sanitize(h.desc);
    m_ready = true;
    m_fromCache = true;
    m_cacheBytes = FileSize(m_cacheFile);
    m_message = "loaded from the cache file";
    FOH_INFO("atmosphere: loaded {} ({:.1f} MiB)", m_cacheFile.string(), static_cast<double>(m_cacheBytes) / (1024.0 * 1024.0));
    return true;
}

// ---- generation -----------------------------------------------------------------------------------------

void Atmosphere::request(const AtmosphereDesc& desc) {
    m_requested = Sanitize(desc);
    m_hasRequest = true;
}

void Atmosphere::pump() {
    if (m_state == State::Reading && m_pack && m_device->pollEventQuery(m_pack->query)) finishPack();
    if (m_state == State::Saving && m_save && m_save->counter.pending.load(std::memory_order_acquire) == 0) {
        m_cacheBytes = m_save->ok ? FileSize(m_cacheFile) : m_cacheBytes;
        m_message = m_save->ok ? "generated and cached" : "generated, but the cache file could not be written";
        if (!m_save->ok) FOH_WARN("atmosphere: could not write {}", m_cacheFile.string());
        m_save.reset();
        m_state = State::Idle;
    }
    if (m_hasRequest && m_state == State::Idle) {
        m_hasRequest = false;
        m_genStart = std::chrono::steady_clock::now();
        if (generate(m_requested)) {
            if (!beginPack()) m_message = "generated (not cached: dump shader missing)";
        }
    }
}

bool Atmosphere::generate(const AtmosphereDesc& desc) {
    const char* kEntries[6] = {"csTransmittance", "csDirectIrradiance", "csSingleScattering",
                               "csScatteringDensity", "csIndirectIrradiance", "csMultipleScattering"};
    ShaderRef cs[6];
    for (int i = 0; i < 6; ++i) {
        cs[i] = m_shaders->get(std::string("atmosphere_precompute.") + kEntries[i], nvrhi::ShaderType::Compute);
        if (!cs[i]) {
            m_message = std::string("shader missing: atmosphere_precompute.") + kEntries[i] + " (build the foh_shaders target)";
            FOH_ERROR("atmosphere: {}", m_message);
            return false;
        }
    }
    enum { kTransmittance, kDirect, kSingle, kDensity, kIndirect, kMulti };

    const PrecomputeConstants constants = MakeConstants(desc);

    // Intermediate results: full precision, released when the generation is submitted.
    auto temp = [&](nvrhi::TextureDimension dim, u32 w, u32 h, u32 d, nvrhi::Format format, const char* name) {
        nvrhi::TextureDesc td;
        td.dimension = dim;
        td.width = w;
        td.height = h;
        td.depth = d;
        td.format = format;
        td.isUAV = true;
        td.initialState = nvrhi::ResourceStates::ShaderResource;
        td.keepInitialState = true;
        td.debugName = name;
        return m_device->createTexture(td);
    };
    using Dim = nvrhi::TextureDimension;
    nvrhi::TextureHandle deltaRayleigh = temp(Dim::Texture3D, kScatteringW, kScatteringH, kScatteringD, nvrhi::Format::RGBA32_FLOAT, "Atmosphere delta rayleigh");
    nvrhi::TextureHandle deltaMie = temp(Dim::Texture3D, kScatteringW, kScatteringH, kScatteringD, nvrhi::Format::RGBA32_FLOAT, "Atmosphere delta mie");
    nvrhi::TextureHandle deltaMulti = temp(Dim::Texture3D, kScatteringW, kScatteringH, kScatteringD, nvrhi::Format::RGBA32_FLOAT, "Atmosphere delta multiple");
    nvrhi::TextureHandle deltaDensity = temp(Dim::Texture3D, kScatteringW, kScatteringH, kScatteringD, nvrhi::Format::RGBA32_FLOAT, "Atmosphere delta density");
    nvrhi::TextureHandle deltaIrradiance = temp(Dim::Texture2D, kIrradianceW, kIrradianceH, 1, nvrhi::Format::RGBA32_FLOAT, "Atmosphere delta irradiance");
    nvrhi::TextureHandle scatteringSpare = temp(Dim::Texture3D, kScatteringW, kScatteringH, kScatteringD, nvrhi::Format::RGBA16_FLOAT, "Atmosphere scattering (spare)");
    nvrhi::TextureHandle irradianceSpare = temp(Dim::Texture2D, kIrradianceW, kIrradianceH, 1, nvrhi::Format::RGBA16_FLOAT, "Atmosphere irradiance (spare)");

    // Accumulated tables ping-pong between the final texture and a spare: each further order reads one and writes
    // the other. Start on the side that makes the last write land in the final texture.
    const u32 accumulations = desc.scatteringOrders - 1;
    nvrhi::ITexture* scatteringCur = accumulations % 2 == 0 ? m_scattering.Get() : scatteringSpare.Get();
    nvrhi::ITexture* scatteringNext = accumulations % 2 == 0 ? scatteringSpare.Get() : m_scattering.Get();
    nvrhi::ITexture* irradianceCur = accumulations % 2 == 0 ? m_irradiance.Get() : irradianceSpare.Get();
    nvrhi::ITexture* irradianceNext = accumulations % 2 == 0 ? irradianceSpare.Get() : m_irradiance.Get();

    nvrhi::BufferDesc bd;
    bd.byteSize = sizeof(PrecomputeConstants);
    bd.isConstantBuffer = true;
    bd.isVolatile = true;
    bd.maxVersions = 64;
    bd.debugName = "Atmosphere precompute constants";
    nvrhi::BufferHandle cb = m_device->createBuffer(bd);

    nvrhi::CommandListParameters cp;
    cp.enableImmediateExecution = false;
    nvrhi::CommandListHandle cmd = m_device->createCommandList(cp);

    PrecomputeConstants pc = constants;
    bool ok = true;
    auto dispatch = [&](const ShaderRef& shader, const Slots& slots, u32 gx, u32 gy, u32 gz) {
        cmd->writeBuffer(cb, &pc, sizeof(pc));
        nvrhi::BindingSetDesc set;
        set.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, cb));
        for (u32 i = 0; i < 8; ++i) {
            set.addItem(nvrhi::BindingSetItem::Texture_SRV(i, slots.srv[i] ? slots.srv[i] : m_dummySrv[i].Get()));
        }
        set.addItem(nvrhi::BindingSetItem::Sampler(0, m_sampler));
        for (u32 i = 0; i < 5; ++i) {
            set.addItem(nvrhi::BindingSetItem::Texture_UAV(i, slots.uav[i] ? slots.uav[i] : m_dummyUav[i].Get()));
        }
        nvrhi::BindingSetHandle bindings = m_device->createBindingSet(set, m_layoutPrecompute);
        const u64 key = HashCombine(HashCombine(shader.uid, reinterpret_cast<uintptr_t>(m_layoutPrecompute.Get())), 0xA7u);
        nvrhi::IComputePipeline* pso = m_pipelines->compute(key, [&] {
            nvrhi::ComputePipelineDesc pd;
            pd.CS = shader.shader;
            pd.bindingLayouts = {m_layoutPrecompute.Get()};
            return pd;
        });
        if (!pso || !bindings) {
            ok = false;
            return;
        }
        nvrhi::ComputeState state;
        state.setPipeline(pso).addBindingSet(bindings);
        cmd->setComputeState(state);
        cmd->dispatch(gx, gy, gz);
    };
    // One dispatch per slice of the r axis.
    auto sliced = [&](const ShaderRef& shader, const Slots& slots) {
        for (u32 first = 0; first < kScatteringD; first += kSlicesPerDispatch) {
            pc.pass[1] = first;
            dispatch(shader, slots, kScatteringW / 8, kScatteringH / 8, kSlicesPerDispatch);
        }
        pc.pass[1] = 0;
    };

    // First list: transmittance, direct irradiance and the single scattering of every table.
    cmd->open();
    cmd->clearTextureFloat(irradianceCur, nvrhi::AllSubresources, nvrhi::Color(0.0f)); // only indirect light is accumulated
    {
        Slots s;
        s.uav[0] = m_transmittance;
        dispatch(cs[kTransmittance], s, kTransmittanceW / 8, kTransmittanceH / 8, 1);
    }
    {
        Slots s;
        s.srv[0] = m_transmittance;
        s.uav[0] = deltaIrradiance;
        dispatch(cs[kDirect], s, kIrradianceW / 8, kIrradianceH / 8, 1);
    }
    {
        Slots s;
        s.srv[0] = m_transmittance;
        s.uav[1] = deltaRayleigh;
        s.uav[2] = deltaMie;
        s.uav[4] = scatteringCur;
        sliced(cs[kSingle], s);
    }
    cmd->close();
    m_device->executeCommandList(cmd);

    // Each further order is its own list: scattering density, indirect irradiance, multiple scattering.
    for (u32 order = 2; order <= desc.scatteringOrders && ok; ++order) {
        cmd->open();
        pc.pass[0] = order;
        {
            Slots s;
            s.srv[0] = m_transmittance;
            s.srv[1] = deltaRayleigh;
            s.srv[2] = deltaMie;
            s.srv[3] = deltaMulti;
            s.srv[5] = deltaIrradiance;
            s.uav[1] = deltaDensity;
            sliced(cs[kDensity], s);
        }
        pc.pass[0] = order - 1;
        {
            Slots s;
            s.srv[1] = deltaRayleigh;
            s.srv[2] = deltaMie;
            s.srv[3] = deltaMulti;
            s.srv[7] = irradianceCur;
            s.uav[0] = deltaIrradiance;
            s.uav[3] = irradianceNext;
            dispatch(cs[kIndirect], s, kIrradianceW / 8, kIrradianceH / 8, 1);
        }
        std::swap(irradianceCur, irradianceNext);
        pc.pass[0] = order;
        {
            Slots s;
            s.srv[0] = m_transmittance;
            s.srv[4] = deltaDensity;
            s.srv[6] = scatteringCur;
            s.uav[1] = deltaMulti;
            s.uav[4] = scatteringNext;
            sliced(cs[kMulti], s);
        }
        std::swap(scatteringCur, scatteringNext);
        cmd->close();
        m_device->executeCommandList(cmd);
    }

    if (!ok) {
        m_message = "generation failed (compute pipeline could not be created)";
        FOH_ERROR("atmosphere: {}", m_message);
        return false;
    }

    // The queue is in order: from here on every later command list sees the new tables.
    m_active = desc;
    m_ready = true;
    m_fromCache = false;
    m_message = "generated";
    FOH_INFO("atmosphere: tables submitted ({} scattering orders)", desc.scatteringOrders);
    return true;
}

// ---- dump to the cache file ---------------------------------------------------------------------------------

bool Atmosphere::beginPack() {
    const ShaderRef pack2D = m_shaders->get("atmosphere_pack.csPack2D", nvrhi::ShaderType::Compute);
    const ShaderRef pack3D = m_shaders->get("atmosphere_pack.csPack3D", nvrhi::ShaderType::Compute);
    if (!pack2D || !pack3D) return false;

    auto pack = std::make_unique<Pack>();
    pack->desc = m_active;
    const u64 sizes[3] = {kTransmittanceBytes, kScatteringBytes, kIrradianceBytes};
    for (int i = 0; i < 3; ++i) {
        nvrhi::BufferDesc gd;
        gd.byteSize = sizes[i];
        gd.canHaveUAVs = true;
        gd.canHaveRawViews = true;
        gd.initialState = nvrhi::ResourceStates::UnorderedAccess;
        gd.keepInitialState = true;
        gd.debugName = "Atmosphere dump";
        pack->gpu[i] = m_device->createBuffer(gd);
        nvrhi::BufferDesc rd;
        rd.byteSize = sizes[i];
        rd.cpuAccess = nvrhi::CpuAccessMode::Read;
        rd.initialState = nvrhi::ResourceStates::CopyDest;
        rd.keepInitialState = true;
        rd.debugName = "Atmosphere dump (readback)";
        pack->readback[i] = m_device->createBuffer(rd);
    }

    nvrhi::CommandListParameters cp;
    cp.enableImmediateExecution = false;
    nvrhi::CommandListHandle cmd = m_device->createCommandList(cp);
    cmd->open();
    auto dump = [&](const ShaderRef& shader, nvrhi::ITexture* src2D, nvrhi::ITexture* src3D, int index, u32 gx, u32 gy, u32 gz) {
        nvrhi::BindingSetDesc set;
        set.addItem(nvrhi::BindingSetItem::Texture_SRV(0, src2D ? src2D : m_dummySrv[0].Get()));
        set.addItem(nvrhi::BindingSetItem::Texture_SRV(1, src3D ? src3D : m_dummyPackSrv3D.Get()));
        set.addItem(nvrhi::BindingSetItem::RawBuffer_UAV(0, pack->gpu[index]));
        nvrhi::BindingSetHandle bindings = m_device->createBindingSet(set, m_layoutPack);
        const u64 key = HashCombine(HashCombine(shader.uid, reinterpret_cast<uintptr_t>(m_layoutPack.Get())), 0xB1u);
        nvrhi::IComputePipeline* pso = m_pipelines->compute(key, [&] {
            nvrhi::ComputePipelineDesc pd;
            pd.CS = shader.shader;
            pd.bindingLayouts = {m_layoutPack.Get()};
            return pd;
        });
        if (!pso || !bindings) return;
        nvrhi::ComputeState state;
        state.setPipeline(pso).addBindingSet(bindings);
        cmd->setComputeState(state);
        cmd->dispatch(gx, gy, gz);
        cmd->copyBuffer(pack->readback[index], 0, pack->gpu[index], 0, sizes[index]);
    };
    dump(pack2D, m_transmittance, nullptr, 0, kTransmittanceW / 8, kTransmittanceH / 8, 1);
    dump(pack3D, nullptr, m_scattering, 1, kScatteringW / 8, kScatteringH / 8, kScatteringD);
    dump(pack2D, m_irradiance, nullptr, 2, kIrradianceW / 8, kIrradianceH / 8, 1);
    cmd->close();
    m_device->executeCommandList(cmd);

    pack->query = m_device->createEventQuery();
    m_device->setEventQuery(pack->query, nvrhi::CommandQueue::Graphics);
    m_pack = std::move(pack);
    m_state = State::Reading;
    return true;
}

void Atmosphere::finishPack() {
    m_lastGenerateMs = std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - m_genStart).count();
    FOH_INFO("atmosphere: generation took {:.0f} ms", m_lastGenerateMs);

    auto job = std::make_unique<SaveJob>();
    job->file = m_cacheFile;
    CacheHeader h{};
    std::memcpy(h.magic, "FOHATM\0", 8);
    h.version = kCacheVersion;
    std::memcpy(h.sizes, kExpectedSizes, sizeof(kExpectedSizes));
    h.desc = m_pack->desc;
    h.bytes[0] = kTransmittanceBytes;
    h.bytes[1] = kScatteringBytes;
    h.bytes[2] = kIrradianceBytes;
    job->blob.resize(sizeof(h) + kTransmittanceBytes + kScatteringBytes + kIrradianceBytes);
    std::memcpy(job->blob.data(), &h, sizeof(h));
    u8* dst = job->blob.data() + sizeof(h);
    bool mapped = true;
    for (int i = 0; i < 3; ++i) {
        const void* src = m_device->mapBuffer(m_pack->readback[i], nvrhi::CpuAccessMode::Read);
        if (!src) {
            mapped = false;
            break;
        }
        std::memcpy(dst, src, h.bytes[i]);
        dst += h.bytes[i];
        m_device->unmapBuffer(m_pack->readback[i]);
    }
    m_pack.reset();
    if (!mapped) {
        m_message = "generated (not cached: readback failed)";
        m_state = State::Idle;
        return;
    }

    SaveJob* raw = job.get();
    m_save = std::move(job);
    m_state = State::Saving;
    auto write = [](void* arg) {
        auto* j = static_cast<SaveJob*>(arg);
        std::error_code ec;
        std::filesystem::create_directories(j->file.parent_path(), ec);
        std::filesystem::path tmp = j->file;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(j->blob.data()), static_cast<std::streamsize>(j->blob.size()));
            j->ok = static_cast<bool>(out);
        }
        if (j->ok) {
            std::filesystem::rename(tmp, j->file, ec); // replaces an existing file; a reader never sees half of one
            j->ok = !ec;
        }
        if (!j->ok) std::filesystem::remove(tmp, ec);
        std::vector<u8>().swap(j->blob);
    };
    if (m_jobs) {
        m_jobs->submit(write, raw, raw->counter, JobPriority::Low);
    } else {
        write(raw);
    }
}

// ---- runtime ------------------------------------------------------------------------------------------------

void Atmosphere::fillView(ViewConstants& c) const {
    const AtmosphereDesc& d = m_active;
    c.atmosphere0 = Vec4(d.bottomRadiusKm, d.topRadiusKm, std::cos(kMaxSunZenithAngle), d.mieAnisotropy);
    c.atmosphere1 = Vec4(kSunAngularRadius, m_ready ? 1.0f : 0.0f, 0.0f, 0.0f);
    // The tables keep only the red channel of the single Mie term; the other channels follow from the ratio of the
    // two spectra (see GetExtrapolatedSingleMieScattering in the reference implementation).
    const Vec3 rayleigh = kRayleighScattering * std::max(d.rayleighScale, 1e-6f);
    const Vec3 mie = Vec3(kMieScattering * std::max(d.mieScale, 1e-6f));
    c.atmosphere2 = Vec4((rayleigh.x / mie.x) * (mie / rayleigh), 0.0f);
    c.atmosphere3 = Vec4(d.groundAlbedo, 0.0f);
}

AtmosphereStatus Atmosphere::status() const {
    AtmosphereStatus s;
    s.ready = m_ready;
    s.fromCache = m_fromCache;
    s.lastGenerateMs = m_lastGenerateMs;
    s.lutBytes = kTransmittanceBytes + kScatteringBytes + kIrradianceBytes;
    s.cacheBytes = m_cacheBytes;
    s.cachePath = m_cacheFile.string();
    s.message = m_message;
    if (m_hasRequest || m_state == State::Reading) s.phase = AtmosphereStatus::Phase::Generating;
    else if (m_state == State::Saving) s.phase = AtmosphereStatus::Phase::Saving;
    else s.phase = m_ready ? AtmosphereStatus::Phase::Ready : AtmosphereStatus::Phase::Missing;
    return s;
}

} // namespace foh
