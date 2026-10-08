#pragma once

#include <memory>
#include <string>

#include "gtabr/platform.h"
#include "gtabr/types.h"

namespace gtabr {

struct WindowDesc {
    std::string title = "gtaBR";
    u32 width = 1600;
    u32 height = 900;
    bool resizable = true;
};

enum class Key { Escape, F5 };

// OS window + event pump (SDL3 behind the scenes; SDL types never leak here).
class GTABR_API Window {
public:
    // Called for every raw OS event (an SDL_Event*), e.g. to feed ImGui.
    using RawEventCallback = void (*)(const void* sdlEvent, void* user);

    static std::unique_ptr<Window> create(const WindowDesc& desc);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Pumps OS events. Returns false once the user asked to close the window.
    bool pollEvents();
    bool isKeyDown(Key key) const;

    void setRawEventCallback(RawEventCallback callback, void* user);
    void setTitle(const std::string& title);
    void getSize(u32& width, u32& height) const;
    void* nativeWindowHandle() const; // HWND on Windows
    void* sdlWindow() const;          // SDL_Window*, for backends that need it

private:
    Window();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace gtabr
