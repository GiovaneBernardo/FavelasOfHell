#pragma once

namespace foh {

class Window;

// Editor look: one font (system UI font when available), a dark theme, and a single UI scale
// that follows the display. Call update() once per frame before ImGui::NewFrame(); it is cheap
// unless the scale actually changed (monitor switch, Windows scaling setting).
//
// Thread-safety: main thread only.
class EditorTheme {
public:
    // Loads the font and applies the style for the window's current scale.
    void init(const Window& window);
    void update(const Window& window);

    // UI scale relative to a 96 dpi layout (1.0 on a 100% display, 1.5 at 150%, ...).
    float scale() const { return m_scale; }

    // Accent colors reused by the custom-drawn widgets (content browser, workshop).
    static unsigned int accent();
    static unsigned int panelBg();
    static unsigned int tileBg();

private:
    void apply(float scale);
    float computeScale(const Window& window) const;

    float m_scale = 0.0f;
    bool m_fontLoaded = false;
};

} // namespace foh
