#include "clouds.h"

#include <algorithm>
#include <cmath>

#include "foh/log.h"

namespace foh {

namespace {

constexpr u32 kShapeSize = 128;
constexpr u32 kDetailSize = 32;
constexpr u32 kWeatherSize = 256;

// World size (meters) one tile of each noise covers at cloudScale 1.
constexpr f32 kShapeTile = 6000.0f;
constexpr f32 kDetailTile = 500.0f;
constexpr f32 kWeatherTile = 24000.0f;
constexpr f32 kExtinction = 0.03f; // 1 / m at density 1: a dense core is opaque after ~100 m
constexpr f32 kMaxDistance = 25000.0f;

struct Quality {
    f32 stepsUp, stepsGrazing;
};
constexpr Quality kQualities[3] = {{20.0f, 36.0f}, {32.0f, 56.0f}, {48.0f, 96.0f}};

CloudDesc Sanitize(CloudDesc d) {
    d.baseAltitude = std::clamp(d.baseAltitude, 200.0f, 8000.0f);
    d.thickness = std::clamp(d.thickness, 200.0f, 6000.0f);
    d.coverage = std::clamp(d.coverage, 0.0f, 1.0f);
    d.density = std::clamp(d.density, 0.0f, 4.0f);
    d.windSpeed = std::clamp(d.windSpeed, 0.0f, 100.0f);
    d.cloudScale = std::clamp(d.cloudScale, 0.25f, 4.0f);
    d.detail = std::clamp(d.detail, 0.0f, 1.0f);
    d.ambient = std::clamp(d.ambient, 0.0f, 4.0f);
    d.quality = std::min(d.quality, 2u);
    return d;
}

nvrhi::TextureHandle makeNoise(nvrhi::IDevice* dev, nvrhi::TextureDimension dim, u32 w, u32 h, u32 d, const char* name) {
    nvrhi::TextureDesc td;
    td.dimension = dim;
    td.width = w;
    td.height = h;
    td.depth = d;
    td.format = nvrhi::Format::RGBA8_UNORM;
    td.isUAV = true;
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true; // every command list assumes (and restores) the shader-resource state
    td.debugName = name;
    return dev->createTexture(td);
}

} // namespace

bool Clouds::init(nvrhi::IDevice* device, ShaderLibrary* shaders, PipelineCache* pipelines) {
    m_device = device;
    m_shaders = shaders;
    m_pipelines = pipelines;
    m_desc = Sanitize(CloudDesc{});

    using Dim = nvrhi::TextureDimension;
    m_shape = makeNoise(device, Dim::Texture3D, kShapeSize, kShapeSize, kShapeSize, "Cloud shape noise");
    m_detail = makeNoise(device, Dim::Texture3D, kDetailSize, kDetailSize, kDetailSize, "Cloud detail noise");
    m_weather = makeNoise(device, Dim::Texture2D, kWeatherSize, kWeatherSize, 1, "Cloud weather noise");

    nvrhi::SamplerDesc sd;
    sd.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    m_wrap = device->createSampler(sd);

    nvrhi::BindingLayoutDesc nl;
    nl.visibility = nvrhi::ShaderType::Compute;
    nl.addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
    m_noiseLayout = device->createBindingLayout(nl);

    nvrhi::BindingLayoutDesc tl;
    tl.visibility = nvrhi::ShaderType::All;
    tl.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
    tl.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1));
    for (u32 t = 0; t < 7; ++t) tl.addItem(nvrhi::BindingLayoutItem::Texture_SRV(t));
    tl.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    tl.addItem(nvrhi::BindingLayoutItem::Sampler(1));
    m_traceLayout = device->createBindingLayout(tl);

    nvrhi::BindingLayoutDesc pl;
    pl.visibility = nvrhi::ShaderType::All;
    pl.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
    pl.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1));
    pl.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    pl.addItem(nvrhi::BindingLayoutItem::Texture_SRV(1));
    pl.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    m_postLayout = device->createBindingLayout(pl);

    pump();
    return true;
}

void Clouds::shutdown() {
    m_shape = nullptr;
    m_detail = nullptr;
    m_weather = nullptr;
    m_wrap = nullptr;
    m_traceLayout = nullptr;
    m_postLayout = nullptr;
    m_noiseLayout = nullptr;
    m_ready = false;
}

void Clouds::pump() {
    if (!m_ready && m_device) m_ready = generate();
}

// One command list, three dispatches: the GPU takes well under a millisecond for all of it. The queue is in
// order, so every later frame sees the finished volumes.
bool Clouds::generate() {
    const ShaderRef shapeCs = m_shaders->get("clouds_noise.csShape", nvrhi::ShaderType::Compute);
    const ShaderRef detailCs = m_shaders->get("clouds_noise.csDetail", nvrhi::ShaderType::Compute);
    const ShaderRef weatherCs = m_shaders->get("clouds_weather.csWeather", nvrhi::ShaderType::Compute);
    if (!shapeCs || !detailCs || !weatherCs) return false; // shaders not built (yet): clouds stay off, retried next frame

    nvrhi::CommandListParameters cp;
    cp.enableImmediateExecution = false;
    nvrhi::CommandListHandle cmd = m_device->createCommandList(cp);
    cmd->open();
    bool ok = true;
    auto dispatch = [&](const ShaderRef& shader, nvrhi::ITexture* target, u32 gx, u32 gy, u32 gz) {
        nvrhi::BindingSetDesc set;
        set.addItem(nvrhi::BindingSetItem::Texture_UAV(0, target));
        nvrhi::BindingSetHandle bindings = m_device->createBindingSet(set, m_noiseLayout);
        const u64 key = HashCombine(HashCombine(shader.uid, reinterpret_cast<uintptr_t>(m_noiseLayout.Get())), 0xC10Du);
        nvrhi::IComputePipeline* pso = m_pipelines->compute(key, [&] {
            nvrhi::ComputePipelineDesc pd;
            pd.CS = shader.shader;
            pd.bindingLayouts = {m_noiseLayout.Get()};
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
    dispatch(shapeCs, m_shape, kShapeSize / 4, kShapeSize / 4, kShapeSize / 4);
    dispatch(detailCs, m_detail, kDetailSize / 4, kDetailSize / 4, kDetailSize / 4);
    dispatch(weatherCs, m_weather, kWeatherSize / 8, kWeatherSize / 8, 1);
    cmd->close();
    if (!ok) {
        FOH_ERROR("clouds: could not create the noise compute pipelines");
        return false;
    }
    m_device->executeCommandList(cmd);
    FOH_INFO("clouds: noise volumes generated");
    return true;
}

void Clouds::set(const CloudDesc& desc) { m_desc = Sanitize(desc); }

void Clouds::fill(CloudConstants& c, f32 time, u64 frame, f32 historyWeight) const {
    const CloudDesc& d = m_desc;
    const f32 angle = d.windDirection * 3.14159265f / 180.0f;
    const Vec3 wind = Vec3(std::cos(angle), 0.0f, std::sin(angle)) * (d.windSpeed * time);
    const Quality& q = kQualities[d.quality];

    c.layer = Vec4(d.baseAltitude, d.baseAltitude + d.thickness, d.coverage, d.density);
    c.noise = Vec4(1.0f / (kShapeTile * d.cloudScale), 1.0f / (kDetailTile * d.cloudScale), 1.0f / (kWeatherTile * d.cloudScale),
                   d.detail * 0.5f);
    c.wind = Vec4(wind, 0.0f);
    c.misc = Vec4(static_cast<f32>(frame % 4096), historyWeight, kMaxDistance, d.ambient);
    c.march = Vec4(q.stepsUp, q.stepsGrazing, kExtinction, 0.0f);
}

} // namespace foh
