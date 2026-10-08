#pragma once

#include <memory>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "foh/platform.h"
#include "foh/rhi/graphics_device.h"
#include "foh/types.h"

namespace foh {

struct SwapchainDesc {
    void* windowHandle = nullptr; // HWND on Windows (Window::nativeWindowHandle)
    u32 width = 0;
    u32 height = 0;
    bool vsync = true;
};

// Presentable backbuffers of one OS window.
//
// The backbuffer format is RGBA8 UNORM; shaders write display-encoded (sRGB)
// values. Thread-safety: main thread only.
class FOH_API Swapchain {
public:
    static std::unique_ptr<Swapchain> create(GraphicsDevice& device, const SwapchainDesc& desc);
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Waits for the GPU, then resizes the buffers. No-op if the size is unchanged.
    // Never call with a zero size (minimized): skip the frame instead.
    bool resize(u32 width, u32 height);
    void setVsync(bool vsync) { m_vsync = vsync; }
    bool vsync() const { return m_vsync; }

    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    nvrhi::Format format() const { return nvrhi::Format::RGBA8_UNORM; }

    // The backbuffer to render this frame. Its state at the start of the frame
    // is Present and it must be left in Present (import it into the render graph with
    // initial and final state Present).
    nvrhi::ITexture* currentBackbuffer() const;
    void present();

    // Copies the current backbuffer to the CPU as tightly packed RGBA8 (screenshots, QA).
    // Call after the frame was rendered and before present(). Stalls the GPU; never per frame.
    bool readback(std::vector<u8>& rgba);

private:
    Swapchain(GraphicsDevice& device);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    GraphicsDevice& m_device;
    u32 m_width = 0;
    u32 m_height = 0;
    bool m_vsync = true;
};

} // namespace foh
