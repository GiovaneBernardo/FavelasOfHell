#include "foh/rhi/graphics_device.h"

#include <atomic>
#include <vector>

#include "foh/assert.h"
#include "foh/log.h"

#if defined(FOH_PLATFORM_WINDOWS)
    #include <d3d12.h>
    #include <dxgi1_6.h>
    #include <wrl/client.h>

    #include <nvrhi/d3d12.h>
    #include <nvrhi/validation.h>
#endif

namespace foh {

namespace {

// Routes NVRHI (and validation layer) messages into the engine log and counts
// errors so automated runs can fail on them.
class MessageCallback final : public nvrhi::IMessageCallback {
public:
    void message(nvrhi::MessageSeverity severity, const char* text) override {
        switch (severity) {
            case nvrhi::MessageSeverity::Info: FOH_DEBUG("[rhi] {}", text); break;
            case nvrhi::MessageSeverity::Warning: FOH_WARN("[rhi] {}", text); break;
            case nvrhi::MessageSeverity::Error:
                errors.fetch_add(1, std::memory_order_relaxed);
                FOH_ERROR("[rhi] {}", text);
                break;
            case nvrhi::MessageSeverity::Fatal:
                errors.fetch_add(1, std::memory_order_relaxed);
                FOH_FATAL("[rhi] {}", text);
                break;
        }
    }
    std::atomic<u32> errors{0};
};

} // namespace

struct GraphicsDevice::Impl {
    // Declaration order is destruction order in reverse: queries, then the NVRHI
    // devices, then the API objects they wrap, then the message sink.
    MessageCallback messages;
#if defined(FOH_PLATFORM_WINDOWS)
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    Microsoft::WRL::ComPtr<ID3D12Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue1> infoQueue;
    DWORD infoCallbackCookie = 0;
    nvrhi::DeviceHandle rawDevice; // unwrapped; must outlive the validation wrapper
#endif
    nvrhi::DeviceHandle device;    // what everybody uses (validation-wrapped if enabled)
    std::vector<nvrhi::EventQueryHandle> frameQueries;
    nvrhi::GraphicsAPI api = nvrhi::GraphicsAPI::D3D12;
    std::string adapterName;
    std::vector<bool> frameQueryPending;
    u32 framesInFlight = 2;
    u64 frame = 0;
    bool tearing = false;
};

GraphicsDevice::GraphicsDevice() : m_impl(std::make_unique<Impl>()) {}

GraphicsDevice::~GraphicsDevice() {
    if (!m_impl) return;
    waitIdle();
#if defined(FOH_PLATFORM_WINDOWS)
    if (m_impl->infoQueue && m_impl->infoCallbackCookie) {
        m_impl->infoQueue->UnregisterMessageCallback(m_impl->infoCallbackCookie);
    }
#endif
    // Frame queries and the device must die before the API objects below them.
    m_impl->frameQueries.clear();
    if (m_impl->device) m_impl->device->runGarbageCollection();
}

#if defined(FOH_PLATFORM_WINDOWS)

std::unique_ptr<GraphicsDevice> GraphicsDevice::create(const GraphicsDeviceDesc& desc) {
    using Microsoft::WRL::ComPtr;

    if (desc.backend == GraphicsBackend::Vulkan) {
        FOH_ERROR("The Vulkan backend is not implemented yet");
        return nullptr;
    }

    std::unique_ptr<GraphicsDevice> self(new GraphicsDevice());
    Impl& s = *self->m_impl;
    s.framesInFlight = desc.framesInFlight < 1 ? 1 : (desc.framesInFlight > 4 ? 4 : desc.framesInFlight);

    UINT factoryFlags = 0;
    if (desc.validation) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        } else {
            FOH_WARN("D3D12 debug layer unavailable (install the Graphics Tools optional feature)");
        }
    }
    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&s.factory)))) {
        FOH_ERROR("CreateDXGIFactory2 failed");
        return nullptr;
    }

    BOOL tearing = FALSE;
    if (SUCCEEDED(s.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
        s.tearing = tearing != FALSE;
    }

    // Pick the most powerful hardware adapter that can run FL 12.0.
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; s.factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                           IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&s.d3dDevice)))) {
            char name[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
            s.adapterName = name;
            break;
        }
        adapter.Reset();
    }
    if (!s.d3dDevice) {
        FOH_ERROR("No D3D12 (feature level 12.0) hardware adapter found");
        return nullptr;
    }

    // Slang emits shader model 6.2 DXIL (see shaders/CMakeLists.txt).
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_2};
    if (FAILED(s.d3dDevice->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_2) {
        FOH_ERROR("The GPU/driver does not support shader model 6.2");
        return nullptr;
    }

    if (desc.validation && SUCCEEDED(s.d3dDevice.As(&s.infoQueue))) {
        // Debug layer messages go to our log and error counter (so automated runs
        // fail on them); the debugger breaks on them only when one is attached.
        s.infoQueue->RegisterMessageCallback(
            [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID, LPCSTR text, void* ctx) {
                auto* sink = static_cast<MessageCallback*>(ctx);
                switch (severity) {
                    case D3D12_MESSAGE_SEVERITY_CORRUPTION: sink->message(nvrhi::MessageSeverity::Fatal, text); break;
                    case D3D12_MESSAGE_SEVERITY_ERROR: sink->message(nvrhi::MessageSeverity::Error, text); break;
                    case D3D12_MESSAGE_SEVERITY_WARNING: sink->message(nvrhi::MessageSeverity::Warning, text); break;
                    default: break; // info/message severities are far too chatty
                }
            },
            D3D12_MESSAGE_CALLBACK_FLAG_NONE, &s.messages, &s.infoCallbackCookie);
        if (IsDebuggerPresent()) {
            s.infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            s.infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        }
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(s.d3dDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&s.queue)))) {
        FOH_ERROR("CreateCommandQueue failed");
        return nullptr;
    }

    nvrhi::d3d12::DeviceDesc dd;
    dd.errorCB = &s.messages;
    dd.pDevice = s.d3dDevice.Get();
    dd.pGraphicsCommandQueue = s.queue.Get();
    dd.shaderResourceViewHeapSize = 65536; // the bindless texture table alone takes 8192; the default is 16384 in total
    s.rawDevice = nvrhi::d3d12::createDevice(dd);
    if (!s.rawDevice) {
        FOH_ERROR("nvrhi::d3d12::createDevice failed");
        return nullptr;
    }
    s.device = desc.validation ? nvrhi::validation::createValidationLayer(s.rawDevice) : s.rawDevice;
    s.api = nvrhi::GraphicsAPI::D3D12;

    s.frameQueries.resize(s.framesInFlight);
    s.frameQueryPending.assign(s.framesInFlight, false);
    for (auto& q : s.frameQueries) q = s.device->createEventQuery();

    FOH_INFO("GPU: {} (D3D12, validation {}, {} frames in flight)", s.adapterName,
             desc.validation ? "on" : "off", s.framesInFlight);
    return self;
}

#else

std::unique_ptr<GraphicsDevice> GraphicsDevice::create(const GraphicsDeviceDesc&) {
    FOH_ERROR("No graphics backend is implemented for this platform yet (Vulkan is planned)");
    return nullptr;
}

#endif

nvrhi::IDevice* GraphicsDevice::nvrhi() const { return m_impl->device.Get(); }
nvrhi::GraphicsAPI GraphicsDevice::api() const { return m_impl->api; }
const std::string& GraphicsDevice::adapterName() const { return m_impl->adapterName; }
u32 GraphicsDevice::framesInFlight() const { return m_impl->framesInFlight; }
u64 GraphicsDevice::frameNumber() const { return m_impl->frame; }
u32 GraphicsDevice::errorCount() const { return m_impl->messages.errors.load(std::memory_order_relaxed); }
bool GraphicsDevice::supportsTearing() const { return m_impl->tearing; }

void* GraphicsDevice::nativeFactory() const {
#if defined(FOH_PLATFORM_WINDOWS)
    return m_impl->factory.Get();
#else
    return nullptr;
#endif
}

void GraphicsDevice::beginFrame() {
    Impl& s = *m_impl;
    const size_t slot = s.frame % s.framesInFlight;
    if (s.frameQueryPending[slot]) {
        s.device->waitEventQuery(s.frameQueries[slot].Get());
        s.device->resetEventQuery(s.frameQueries[slot].Get());
        s.frameQueryPending[slot] = false;
    }
    s.device->runGarbageCollection();
}

void GraphicsDevice::endFrame() {
    Impl& s = *m_impl;
    const size_t slot = s.frame % s.framesInFlight;
    s.device->setEventQuery(s.frameQueries[slot].Get(), nvrhi::CommandQueue::Graphics);
    s.frameQueryPending[slot] = true;
    ++s.frame;
}

void GraphicsDevice::waitIdle() {
    if (m_impl->device) m_impl->device->waitForIdle();
}

} // namespace foh
