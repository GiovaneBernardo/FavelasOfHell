#include "foh/window.h"

#include <cstring>
#include <vector>

#include <SDL3/SDL.h>

#include "foh/log.h"

namespace foh {

namespace {

Key translateScancode(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        return static_cast<Key>(static_cast<int>(Key::A) + (sc - SDL_SCANCODE_A));
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
        return static_cast<Key>(static_cast<int>(Key::Num1) + (sc - SDL_SCANCODE_1));
    }
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) {
        return static_cast<Key>(static_cast<int>(Key::F1) + (sc - SDL_SCANCODE_F1));
    }
    switch (sc) {
        case SDL_SCANCODE_0: return Key::Num0;
        case SDL_SCANCODE_ESCAPE: return Key::Escape;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: return Key::Enter;
        case SDL_SCANCODE_TAB: return Key::Tab;
        case SDL_SCANCODE_SPACE: return Key::Space;
        case SDL_SCANCODE_BACKSPACE: return Key::Backspace;
        case SDL_SCANCODE_DELETE: return Key::Delete;
        case SDL_SCANCODE_INSERT: return Key::Insert;
        case SDL_SCANCODE_HOME: return Key::Home;
        case SDL_SCANCODE_END: return Key::End;
        case SDL_SCANCODE_PAGEUP: return Key::PageUp;
        case SDL_SCANCODE_PAGEDOWN: return Key::PageDown;
        case SDL_SCANCODE_UP: return Key::Up;
        case SDL_SCANCODE_DOWN: return Key::Down;
        case SDL_SCANCODE_LEFT: return Key::Left;
        case SDL_SCANCODE_RIGHT: return Key::Right;
        case SDL_SCANCODE_LSHIFT: return Key::LShift;
        case SDL_SCANCODE_RSHIFT: return Key::RShift;
        case SDL_SCANCODE_LCTRL: return Key::LCtrl;
        case SDL_SCANCODE_RCTRL: return Key::RCtrl;
        case SDL_SCANCODE_LALT: return Key::LAlt;
        case SDL_SCANCODE_RALT: return Key::RAlt;
        case SDL_SCANCODE_LGUI: return Key::LSuper;
        case SDL_SCANCODE_RGUI: return Key::RSuper;
        case SDL_SCANCODE_MINUS: return Key::Minus;
        case SDL_SCANCODE_EQUALS: return Key::Equals;
        case SDL_SCANCODE_LEFTBRACKET: return Key::LeftBracket;
        case SDL_SCANCODE_RIGHTBRACKET: return Key::RightBracket;
        case SDL_SCANCODE_BACKSLASH: return Key::Backslash;
        case SDL_SCANCODE_SEMICOLON: return Key::Semicolon;
        case SDL_SCANCODE_APOSTROPHE: return Key::Apostrophe;
        case SDL_SCANCODE_GRAVE: return Key::Grave;
        case SDL_SCANCODE_COMMA: return Key::Comma;
        case SDL_SCANCODE_PERIOD: return Key::Period;
        case SDL_SCANCODE_SLASH: return Key::Slash;
        case SDL_SCANCODE_CAPSLOCK: return Key::CapsLock;
        default: return Key::Unknown;
    }
}

SDL_SystemCursor toSystemCursor(CursorShape s) {
    switch (s) {
        case CursorShape::TextInput: return SDL_SYSTEM_CURSOR_TEXT;
        case CursorShape::ResizeAll: return SDL_SYSTEM_CURSOR_MOVE;
        case CursorShape::ResizeNS: return SDL_SYSTEM_CURSOR_NS_RESIZE;
        case CursorShape::ResizeEW: return SDL_SYSTEM_CURSOR_EW_RESIZE;
        case CursorShape::ResizeNESW: return SDL_SYSTEM_CURSOR_NESW_RESIZE;
        case CursorShape::ResizeNWSE: return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
        case CursorShape::Hand: return SDL_SYSTEM_CURSOR_POINTER;
        case CursorShape::NotAllowed: return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
        default: return SDL_SYSTEM_CURSOR_DEFAULT;
    }
}

} // namespace

struct Window::Impl {
    SDL_Window* window = nullptr;
    bool quit = false;
    bool resized = false;
    bool relativeMouse = false;
    bool textInput = false;
    InputState input;
    std::vector<WindowEvent> events;
    SDL_Cursor* cursors[static_cast<size_t>(CursorShape::Hidden) + 1] = {};
    CursorShape cursor = CursorShape::Arrow;
};

Window::Window() : m_impl(std::make_unique<Impl>()) {}

std::unique_ptr<Window> Window::create(const WindowDesc& desc) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        FOH_ERROR("SDL_Init failed: {}", SDL_GetError());
        return nullptr;
    }
    // High pixel density so the swapchain matches the real display resolution.
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (desc.maximized) flags |= SDL_WINDOW_MAXIMIZED;
    SDL_Window* sdl = SDL_CreateWindow(desc.title.c_str(), static_cast<int>(desc.width),
                                       static_cast<int>(desc.height), flags);
    if (!sdl) {
        FOH_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_Quit();
        return nullptr;
    }
    std::unique_ptr<Window> window(new Window());
    window->m_impl->window = sdl;
    return window;
}

Window::~Window() {
    if (m_impl && m_impl->window) {
        if (m_impl->relativeMouse) SDL_SetWindowRelativeMouseMode(m_impl->window, false);
        if (m_impl->textInput) SDL_StopTextInput(m_impl->window);
        SDL_SetCursor(SDL_GetDefaultCursor());
        for (SDL_Cursor* c : m_impl->cursors) {
            if (c) SDL_DestroyCursor(c);
        }
        SDL_DestroyWindow(m_impl->window);
        SDL_Quit();
    }
}

bool Window::pollEvents() {
    Impl& s = *m_impl;
    InputState& in = s.input;
    in.mouseDeltaX = in.mouseDeltaY = in.wheelY = 0.0f;
    for (bool& pressed : in.keysPressed) pressed = false;
    s.resized = false;
    s.events.clear();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: s.quit = true; break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_RESTORED: s.resized = true; break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED: {
                WindowEvent e;
                e.type = WindowEvent::Type::FocusGained;
                s.events.push_back(e);
                break;
            }
            case SDL_EVENT_WINDOW_FOCUS_LOST: {
                // A key released while unfocused never reports its up event.
                for (bool& k : in.keys) k = false;
                for (bool& b : in.mouseButtons) b = false;
                WindowEvent e;
                e.type = WindowEvent::Type::FocusLost;
                s.events.push_back(e);
                break;
            }
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                const Key key = translateScancode(event.key.scancode);
                if (key == Key::Unknown) break;
                const auto i = static_cast<size_t>(key);
                const bool down = event.type == SDL_EVENT_KEY_DOWN;
                if (down && !in.keys[i]) in.keysPressed[i] = true;
                in.keys[i] = down;
                WindowEvent e;
                e.type = down ? WindowEvent::Type::KeyDown : WindowEvent::Type::KeyUp;
                e.key = key;
                e.repeat = down && event.key.repeat;
                s.events.push_back(e);
                break;
            }
            case SDL_EVENT_TEXT_INPUT: {
                WindowEvent e;
                e.type = WindowEvent::Type::Text;
                std::strncpy(e.text, event.text.text, sizeof(e.text) - 1);
                s.events.push_back(e);
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                MouseButton b;
                if (event.button.button == SDL_BUTTON_LEFT) b = MouseButton::Left;
                else if (event.button.button == SDL_BUTTON_RIGHT) b = MouseButton::Right;
                else if (event.button.button == SDL_BUTTON_MIDDLE) b = MouseButton::Middle;
                else break;
                in.mouseButtons[static_cast<size_t>(b)] = down;
                WindowEvent e;
                e.type = down ? WindowEvent::Type::MouseDown : WindowEvent::Type::MouseUp;
                e.button = b;
                e.x = event.button.x;
                e.y = event.button.y;
                s.events.push_back(e);
                break;
            }
            case SDL_EVENT_MOUSE_MOTION: {
                in.mouseX = event.motion.x;
                in.mouseY = event.motion.y;
                in.mouseDeltaX += event.motion.xrel;
                in.mouseDeltaY += event.motion.yrel;
                WindowEvent e;
                e.type = WindowEvent::Type::MouseMove;
                e.x = event.motion.x;
                e.y = event.motion.y;
                s.events.push_back(e);
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL: {
                in.wheelY += event.wheel.y;
                WindowEvent e;
                e.type = WindowEvent::Type::Wheel;
                e.x = event.wheel.x;
                e.y = event.wheel.y;
                s.events.push_back(e);
                break;
            }
            default: break;
        }
    }
    return !s.quit;
}

const InputState& Window::input() const { return m_impl->input; }
std::span<const WindowEvent> Window::events() const { return m_impl->events; }
bool Window::wasResized() const { return m_impl->resized; }

bool Window::isMinimized() const {
    if (SDL_GetWindowFlags(m_impl->window) & SDL_WINDOW_MINIMIZED) return true;
    u32 w = 0, h = 0;
    getSize(w, h);
    return w == 0 || h == 0;
}

bool Window::hasFocus() const { return (SDL_GetWindowFlags(m_impl->window) & SDL_WINDOW_INPUT_FOCUS) != 0; }

void Window::setRelativeMouse(bool enabled) {
    if (m_impl->relativeMouse == enabled) return;
    m_impl->relativeMouse = enabled;
    SDL_SetWindowRelativeMouseMode(m_impl->window, enabled);
}

bool Window::isRelativeMouse() const { return m_impl->relativeMouse; }

void Window::setCursor(CursorShape shape) {
    Impl& s = *m_impl;
    if (shape == s.cursor) return;
    s.cursor = shape;
    if (shape == CursorShape::Hidden) {
        SDL_HideCursor();
        return;
    }
    SDL_ShowCursor();
    SDL_Cursor*& c = s.cursors[static_cast<size_t>(shape)];
    if (!c) c = SDL_CreateSystemCursor(toSystemCursor(shape));
    if (c) SDL_SetCursor(c);
}

void Window::setTextInput(bool enabled) {
    if (m_impl->textInput == enabled) return;
    m_impl->textInput = enabled;
    if (enabled) {
        SDL_StartTextInput(m_impl->window);
    } else {
        SDL_StopTextInput(m_impl->window);
    }
}

std::string Window::clipboardText() const {
    char* text = SDL_GetClipboardText();
    std::string result = text ? text : "";
    SDL_free(text);
    return result;
}

void Window::setClipboardText(const std::string& text) { SDL_SetClipboardText(text.c_str()); }

void Window::setTitle(const std::string& title) { SDL_SetWindowTitle(m_impl->window, title.c_str()); }

void Window::setSize(u32 logicalWidth, u32 logicalHeight) {
    SDL_SetWindowSize(m_impl->window, static_cast<int>(logicalWidth), static_cast<int>(logicalHeight));
}

void Window::getSize(u32& width, u32& height) const {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(m_impl->window, &w, &h);
    width = static_cast<u32>(w);
    height = static_cast<u32>(h);
}

void Window::logicalSize(u32& width, u32& height) const {
    int w = 0, h = 0;
    SDL_GetWindowSize(m_impl->window, &w, &h);
    width = static_cast<u32>(w);
    height = static_cast<u32>(h);
}

f32 Window::contentScale() const {
    const f32 scale = SDL_GetWindowDisplayScale(m_impl->window);
    return scale > 0.0f ? scale : 1.0f;
}

void* Window::nativeWindowHandle() const {
    const SDL_PropertiesID props = SDL_GetWindowProperties(m_impl->window);
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}

} // namespace foh
