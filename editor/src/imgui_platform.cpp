#include "imgui_platform.h"

#include <cfloat>

#include <imgui.h>

namespace foh {

namespace {

ImGuiKey toImGuiKey(Key key) {
    if (key >= Key::A && key <= Key::Z) {
        return static_cast<ImGuiKey>(ImGuiKey_A + (static_cast<int>(key) - static_cast<int>(Key::A)));
    }
    if (key >= Key::Num0 && key <= Key::Num9) {
        return static_cast<ImGuiKey>(ImGuiKey_0 + (static_cast<int>(key) - static_cast<int>(Key::Num0)));
    }
    if (key >= Key::F1 && key <= Key::F12) {
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (static_cast<int>(key) - static_cast<int>(Key::F1)));
    }
    switch (key) {
        case Key::Escape: return ImGuiKey_Escape;
        case Key::Enter: return ImGuiKey_Enter;
        case Key::Tab: return ImGuiKey_Tab;
        case Key::Space: return ImGuiKey_Space;
        case Key::Backspace: return ImGuiKey_Backspace;
        case Key::Delete: return ImGuiKey_Delete;
        case Key::Insert: return ImGuiKey_Insert;
        case Key::Home: return ImGuiKey_Home;
        case Key::End: return ImGuiKey_End;
        case Key::PageUp: return ImGuiKey_PageUp;
        case Key::PageDown: return ImGuiKey_PageDown;
        case Key::Up: return ImGuiKey_UpArrow;
        case Key::Down: return ImGuiKey_DownArrow;
        case Key::Left: return ImGuiKey_LeftArrow;
        case Key::Right: return ImGuiKey_RightArrow;
        case Key::LShift: return ImGuiKey_LeftShift;
        case Key::RShift: return ImGuiKey_RightShift;
        case Key::LCtrl: return ImGuiKey_LeftCtrl;
        case Key::RCtrl: return ImGuiKey_RightCtrl;
        case Key::LAlt: return ImGuiKey_LeftAlt;
        case Key::RAlt: return ImGuiKey_RightAlt;
        case Key::LSuper: return ImGuiKey_LeftSuper;
        case Key::RSuper: return ImGuiKey_RightSuper;
        case Key::Minus: return ImGuiKey_Minus;
        case Key::Equals: return ImGuiKey_Equal;
        case Key::LeftBracket: return ImGuiKey_LeftBracket;
        case Key::RightBracket: return ImGuiKey_RightBracket;
        case Key::Backslash: return ImGuiKey_Backslash;
        case Key::Semicolon: return ImGuiKey_Semicolon;
        case Key::Apostrophe: return ImGuiKey_Apostrophe;
        case Key::Grave: return ImGuiKey_GraveAccent;
        case Key::Comma: return ImGuiKey_Comma;
        case Key::Period: return ImGuiKey_Period;
        case Key::Slash: return ImGuiKey_Slash;
        case Key::CapsLock: return ImGuiKey_CapsLock;
        default: return ImGuiKey_None;
    }
}

CursorShape toCursor(ImGuiMouseCursor c) {
    switch (c) {
        case ImGuiMouseCursor_None: return CursorShape::Hidden;
        case ImGuiMouseCursor_TextInput: return CursorShape::TextInput;
        case ImGuiMouseCursor_ResizeAll: return CursorShape::ResizeAll;
        case ImGuiMouseCursor_ResizeNS: return CursorShape::ResizeNS;
        case ImGuiMouseCursor_ResizeEW: return CursorShape::ResizeEW;
        case ImGuiMouseCursor_ResizeNESW: return CursorShape::ResizeNESW;
        case ImGuiMouseCursor_ResizeNWSE: return CursorShape::ResizeNWSE;
        case ImGuiMouseCursor_Hand: return CursorShape::Hand;
        case ImGuiMouseCursor_NotAllowed: return CursorShape::NotAllowed;
        default: return CursorShape::Arrow;
    }
}

} // namespace

struct ImGuiPlatformCallbacks {
    static const char* getClipboard(ImGuiContext*) {
        auto* self = static_cast<ImGuiPlatform*>(ImGui::GetIO().BackendPlatformUserData);
        self->clipboard() = self->m_window->clipboardText();
        return self->clipboard().c_str();
    }
    static void setClipboard(ImGuiContext*, const char* text) {
        auto* self = static_cast<ImGuiPlatform*>(ImGui::GetIO().BackendPlatformUserData);
        self->m_window->setClipboardText(text);
    }
};

void ImGuiPlatform::init(Window& window) {
    m_window = &window;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "foh_window";
    io.BackendPlatformUserData = this;
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_GetClipboardTextFn = &ImGuiPlatformCallbacks::getClipboard;
    pio.Platform_SetClipboardTextFn = &ImGuiPlatformCallbacks::setClipboard;
}

void ImGuiPlatform::shutdown() {
    if (!m_window) return;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformUserData = nullptr;
    m_window->setTextInput(false);
    m_window->setCursor(CursorShape::Arrow);
    m_window = nullptr;
}

void ImGuiPlatform::newFrame(float deltaSeconds) {
    ImGuiIO& io = ImGui::GetIO();
    u32 lw = 0, lh = 0, pw = 0, ph = 0;
    m_window->logicalSize(lw, lh);
    m_window->getSize(pw, ph);
    io.DisplaySize = ImVec2(static_cast<float>(lw), static_cast<float>(lh));
    io.DisplayFramebufferScale = lw && lh ? ImVec2(static_cast<float>(pw) / lw, static_cast<float>(ph) / lh) : ImVec2(1, 1);
    io.DeltaTime = deltaSeconds > 0.0f ? deltaSeconds : 1.0f / 60.0f;

    if (m_mouseCaptured != m_mouseWasCaptured) {
        m_mouseWasCaptured = m_mouseCaptured;
        if (m_mouseCaptured) {
            for (int b = 0; b < ImGuiMouseButton_COUNT; ++b) io.AddMouseButtonEvent(b, false);
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); // "no mouse": nothing is hovered
        }
    }

    for (const WindowEvent& e : m_window->events()) {
        if (m_mouseCaptured &&
            (e.type == WindowEvent::Type::MouseMove || e.type == WindowEvent::Type::MouseDown || e.type == WindowEvent::Type::MouseUp ||
             e.type == WindowEvent::Type::Wheel))
            continue;
        switch (e.type) {
            case WindowEvent::Type::KeyDown:
            case WindowEvent::Type::KeyUp: {
                const ImGuiKey k = toImGuiKey(e.key);
                if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == WindowEvent::Type::KeyDown);
                break;
            }
            case WindowEvent::Type::Text: io.AddInputCharactersUTF8(e.text); break;
            case WindowEvent::Type::MouseMove: io.AddMousePosEvent(e.x, e.y); break;
            case WindowEvent::Type::MouseDown:
            case WindowEvent::Type::MouseUp:
                io.AddMousePosEvent(e.x, e.y);
                io.AddMouseButtonEvent(static_cast<int>(e.button), e.type == WindowEvent::Type::MouseDown);
                break;
            case WindowEvent::Type::Wheel: io.AddMouseWheelEvent(e.x, e.y); break;
            case WindowEvent::Type::FocusGained: io.AddFocusEvent(true); break;
            case WindowEvent::Type::FocusLost: io.AddFocusEvent(false); break;
        }
    }
}

void ImGuiPlatform::endFrame() {
    ImGuiIO& io = ImGui::GetIO();
    if (!(io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)) {
        m_window->setCursor(io.MouseDrawCursor ? CursorShape::Hidden : toCursor(ImGui::GetMouseCursor()));
    }
    m_window->setTextInput(io.WantTextInput);
}

bool ImGuiPlatform::wantsKeyboard() const { return ImGui::GetIO().WantCaptureKeyboard; }
bool ImGuiPlatform::wantsMouse() const { return ImGui::GetIO().WantCaptureMouse; }

} // namespace foh
