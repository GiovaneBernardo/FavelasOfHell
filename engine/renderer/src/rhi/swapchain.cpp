#include "foh/rhi/swapchain.h"

#include <cstring>
#include <vector>

#include "foh/log.h"

#if defined(FOH_PLATFORM_WINDOWS)
    #include <d3d12.h>
    #include <dxgi1_6.h>
    #include <wrl/client.h>

    #include <nvrhi/d3d12.h>
#endif

namespace foh {

namespace {
constexpr u32 kBufferCount = 3;
}

struct Swapchain::Impl {
#if defined(FOH_PLATFORM_WINDOWS)
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
    // Signalled on the graphics queue right after every Present. nvrhi's waitForIdle
    // only covers command lists, not the presentation work that still references the
    // backbuffers, so before resizing or releasing them we wait on this fence.
    Microsoft::WRL::ComPtr<ID3D12Fence> presentFence;
    ID3D12CommandQueue* queue = nullptr; // owned by the device
    HANDLE fenceEvent = nullptr;
    u64 fenceValue = 0;
    bool tearing = false;

    void waitForPresents() {
        if (!presentFence || presentFence->GetCompletedValue() >= fenceValue) return;
        presentFence->SetEventOnCompletion(fenceValue, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
#endif
    std::vector<nvrhi::TextureHandle> backbuffers;
};

Swapchain::Swapchain(GraphicsDevice& device) : m_impl(std::make_unique<Impl>()), m_device(device) {}

Swapchain::~Swapchain() {
    if (!m_impl) return;
    m_device.waitIdle();
#if defined(FOH_PLATFORM_WINDOWS)
    m_impl->waitForPresents();
#endif
    m_impl->backbuffers.clear();
    m_device.nvrhi()->runGarbageCollection();
#if defined(FOH_PLATFORM_WINDOWS)
    if (m_impl->fenceEvent) CloseHandle(m_impl->fenceEvent);
#endif
}

#if defined(FOH_PLATFORM_WINDOWS)

namespace {

void wrapBackbuffers(GraphicsDevice& device, IDXGISwapChain3& swapchain, u32 width, u32 height,
                     std::vector<nvrhi::TextureHandle>& out) {
    out.clear();
    nvrhi::TextureDesc td;
    td.width = width;
    td.height = height;
    td.format = nvrhi::Format::RGBA8_UNORM;
    td.isRenderTarget = true;
    td.initialState = nvrhi::ResourceStates::Present;
    td.keepInitialState = false;
    td.debugName = "Backbuffer";
    for (u32 i = 0; i < kBufferCount; ++i) {
        Microsoft::WRL::ComPtr<ID3D12Resource> res;
        swapchain.GetBuffer(i, IID_PPV_ARGS(&res));
        out.push_back(device.nvrhi()->createHandleForNativeTexture(
            nvrhi::ObjectTypes::D3D12_Resource, nvrhi::Object(res.Get()), td));
    }
}

} // namespace

std::unique_ptr<Swapchain> Swapchain::create(GraphicsDevice& device, const SwapchainDesc& desc) {
    using Microsoft::WRL::ComPtr;
    if (!desc.windowHandle || desc.width == 0 || desc.height == 0) {
        FOH_ERROR("Swapchain::create: invalid window handle or size");
        return nullptr;
    }
    std::unique_ptr<Swapchain> self(new Swapchain(device));
    Impl& s = *self->m_impl;
    s.tearing = device.supportsTearing();

    auto* factory = static_cast<IDXGIFactory6*>(device.nativeFactory());
    auto* queue = device.nvrhi()->getNativeQueue(nvrhi::ObjectTypes::D3D12_CommandQueue, nvrhi::CommandQueue::Graphics)
                      .pointer;
    s.queue = static_cast<ID3D12CommandQueue*>(queue);
    ComPtr<ID3D12Device> d3dDevice;
    s.queue->GetDevice(IID_PPV_ARGS(&d3dDevice));
    d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.presentFence));
    s.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = desc.width;
    sd.Height = desc.height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBufferCount;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = s.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory->CreateSwapChainForHwnd(static_cast<IUnknown*>(queue), static_cast<HWND>(desc.windowHandle),
                                               &sd, nullptr, nullptr, &sc1)) ||
        FAILED(sc1.As(&s.swapchain))) {
        FOH_ERROR("CreateSwapChainForHwnd failed");
        return nullptr;
    }
    // We handle fullscreen ourselves (borderless); no Alt+Enter exclusive mode.
    factory->MakeWindowAssociation(static_cast<HWND>(desc.windowHandle), DXGI_MWA_NO_ALT_ENTER);

    self->m_width = desc.width;
    self->m_height = desc.height;
    self->m_vsync = desc.vsync;
    wrapBackbuffers(device, *s.swapchain.Get(), desc.width, desc.height, s.backbuffers);
    return self;
}

bool Swapchain::resize(u32 width, u32 height) {
    if (width == 0 || height == 0) return false;
    if (width == m_width && height == m_height) return true;
    Impl& s = *m_impl;
    // All references to the old buffers must be gone before ResizeBuffers.
    m_device.waitIdle();
    s.waitForPresents();
    s.backbuffers.clear();
    m_device.nvrhi()->runGarbageCollection();

    const UINT flags = s.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(s.swapchain->ResizeBuffers(kBufferCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, flags))) {
        FOH_ERROR("IDXGISwapChain::ResizeBuffers({}, {}) failed", width, height);
        return false;
    }
    m_width = width;
    m_height = height;
    wrapBackbuffers(m_device, *s.swapchain.Get(), width, height, s.backbuffers);
    return true;
}

nvrhi::ITexture* Swapchain::currentBackbuffer() const {
    return m_impl->backbuffers[m_impl->swapchain->GetCurrentBackBufferIndex()].Get();
}

void Swapchain::present() {
    Impl& s = *m_impl;
    const bool tear = s.tearing && !m_vsync;
    s.swapchain->Present(m_vsync ? 1 : 0, tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
    s.queue->Signal(s.presentFence.Get(), ++s.fenceValue);
}

bool Swapchain::readback(std::vector<u8>& rgba) {
    nvrhi::IDevice* dev = m_device.nvrhi();
    nvrhi::ITexture* bb = currentBackbuffer();

    nvrhi::TextureDesc td;
    td.width = m_width;
    td.height = m_height;
    td.format = nvrhi::Format::RGBA8_UNORM;
    td.debugName = "backbuffer readback";
    nvrhi::StagingTextureHandle staging = dev->createStagingTexture(td, nvrhi::CpuAccessMode::Read);
    if (!staging) return false;

    nvrhi::CommandListParameters params;
    params.enableImmediateExecution = false;
    nvrhi::CommandListHandle cmd = dev->createCommandList(params);
    cmd->open();
    cmd->beginTrackingTextureState(bb, nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
    cmd->setTextureState(bb, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
    cmd->commitBarriers();
    cmd->copyTexture(staging, nvrhi::TextureSlice(), bb, nvrhi::TextureSlice());
    cmd->setTextureState(bb, nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
    cmd->commitBarriers();
    cmd->close();
    nvrhi::ICommandList* raw = cmd.Get();
    dev->executeCommandLists(&raw, 1);
    dev->waitForIdle();

    size_t pitch = 0;
    const void* data = dev->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch);
    if (!data) return false;
    rgba.resize(static_cast<size_t>(m_width) * m_height * 4);
    for (u32 y = 0; y < m_height; ++y) {
        std::memcpy(rgba.data() + static_cast<size_t>(y) * m_width * 4, static_cast<const u8*>(data) + y * pitch,
                    static_cast<size_t>(m_width) * 4);
    }
    dev->unmapStagingTexture(staging);
    return true;
}

#else

bool Swapchain::readback(std::vector<u8>&) { return false; }
std::unique_ptr<Swapchain> Swapchain::create(GraphicsDevice&, const SwapchainDesc&) { return nullptr; }
bool Swapchain::resize(u32, u32) { return false; }
nvrhi::ITexture* Swapchain::currentBackbuffer() const { return nullptr; }
void Swapchain::present() {}

#endif

} // namespace foh
