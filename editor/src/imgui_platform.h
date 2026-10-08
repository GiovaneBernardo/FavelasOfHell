#pragma once

#include <string>

#include "foh/window.h"

namespace foh {

// Feeds Dear ImGui from the engine Window (events, size, DPI, cursor, clipboard).
// Replaces the stock SDL3 backend on purpose: SDL is private to the engine DLL, and
// a second SDL instance in the editor would not share its state.
class ImGuiPlatform {
public:
    void init(Window& window);
    void shutdown();
    // Call after Window::pollEvents() and before ImGui::NewFrame().
    void newFrame(float deltaSeconds);
    // Call after ImGui::NewFrame()/the UI was built: applies cursor and text-input requests.
    void endFrame();

    // True while ImGui wants the keyboard/mouse (so the game must not also react).
    bool wantsKeyboard() const;
    bool wantsMouse() const;

private:
    Window* m_window = nullptr;
    std::string m_clipboard;
    friend struct ImGuiPlatformCallbacks;
    std::string& clipboard() { return m_clipboard; }
};

} // namespace foh
