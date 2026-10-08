#include "gtabr/window.h"

#include <SDL3/SDL.h>

#include "gtabr/log.h"

namespace gtabr {

struct Window::Impl {
    SDL_Window* window = nullptr;
    bool quit = false;
    RawEventCallback callback = nullptr;
    void* callbackUser = nullptr;
};

Window::Window() : m_impl(std::make_unique<Impl>()) {}

std::unique_ptr<Window> Window::create(const WindowDesc& desc) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        GTABR_ERROR("SDL_Init failed: {}", SDL_GetError());
        return nullptr;
    }
    SDL_WindowFlags flags = 0;
    if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
    SDL_Window* sdl = SDL_CreateWindow(desc.title.c_str(), static_cast<int>(desc.width),
                                       static_cast<int>(desc.height), flags);
    if (!sdl) {
        GTABR_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_Quit();
        return nullptr;
    }
    std::unique_ptr<Window> window(new Window());
    window->m_impl->window = sdl;
    return window;
}

Window::~Window() {
    if (m_impl && m_impl->window) {
        SDL_DestroyWindow(m_impl->window);
        SDL_Quit();
    }
}

bool Window::pollEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (m_impl->callback) m_impl->callback(&event, m_impl->callbackUser);
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            m_impl->quit = true;
        }
    }
    return !m_impl->quit;
}

bool Window::isKeyDown(Key key) const {
    const bool* state = SDL_GetKeyboardState(nullptr);
    switch (key) {
        case Key::Escape: return state[SDL_SCANCODE_ESCAPE];
        case Key::F5:     return state[SDL_SCANCODE_F5];
    }
    return false;
}

void Window::setRawEventCallback(RawEventCallback callback, void* user) {
    m_impl->callback = callback;
    m_impl->callbackUser = user;
}

void Window::setTitle(const std::string& title) { SDL_SetWindowTitle(m_impl->window, title.c_str()); }

void Window::getSize(u32& width, u32& height) const {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(m_impl->window, &w, &h);
    width = static_cast<u32>(w);
    height = static_cast<u32>(h);
}

void* Window::nativeWindowHandle() const {
    const SDL_PropertiesID props = SDL_GetWindowProperties(m_impl->window);
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}

void* Window::sdlWindow() const { return m_impl->window; }

} // namespace gtabr
