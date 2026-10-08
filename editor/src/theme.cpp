#include "theme.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <imgui.h>

#include "foh/log.h"
#include "foh/window.h"

namespace foh {

namespace {

namespace fs = std::filesystem;

constexpr float kBaseFontSize = 15.0f; // at 100% scaling

ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a);
}

// The first existing UI font of the platform; empty when none exists (ImGui's built-in font is used).
fs::path findSystemFont() {
    static const char* kCandidates[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    std::error_code ec;
    for (const char* c : kCandidates) {
        if (fs::exists(c, ec)) return c;
    }
    return {};
}

} // namespace

unsigned int EditorTheme::accent() { return IM_COL32(64, 156, 255, 255); }
unsigned int EditorTheme::panelBg() { return IM_COL32(24, 25, 28, 255); }
unsigned int EditorTheme::tileBg() { return IM_COL32(38, 40, 45, 255); }

float EditorTheme::computeScale(const Window& window) const {
    u32 lw = 0, lh = 0, pw = 0, ph = 0;
    window.logicalSize(lw, lh);
    window.getSize(pw, ph);
    // Logical size already shrinks with the display scale on some platforms (macOS style, SDL "points"):
    // then ImGui's framebuffer scale carries the density and the layout scale stays 1.
    const float fb = lw ? static_cast<float>(pw) / static_cast<float>(lw) : 1.0f;
    const float s = window.contentScale() / std::max(fb, 0.01f);
    return std::clamp(std::round(s * 8.0f) / 8.0f, 1.0f, 4.0f); // snap to 1/8 steps: no relayout on tiny jitter
}

void EditorTheme::init(const Window& window) {
    ImGuiIO& io = ImGui::GetIO();
    const fs::path font = findSystemFont();
    if (!font.empty()) {
        const std::string path = font.string();
        // ImGui 1.92 rasterizes fonts at the requested size on demand, so one font serves every scale.
        m_fontLoaded = io.Fonts->AddFontFromFileTTF(path.c_str(), kBaseFontSize) != nullptr;
    }
    if (!m_fontLoaded) FOH_WARN("editor: no system UI font found, using the built-in one");
    apply(computeScale(window));
}

void EditorTheme::update(const Window& window) {
    const float s = computeScale(window);
    if (s != m_scale) apply(s);
}

void EditorTheme::apply(float scale) {
    m_scale = scale;
    ImGuiStyle fresh; // start from defaults every time: ScaleAllSizes is not reversible
    ImGuiStyle& s = ImGui::GetStyle();
    s = fresh;
    ImGui::StyleColorsDark(&s);

    s.WindowRounding = 0.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 6.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 5.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBarOverlineSize = 2.0f;
    s.WindowPadding = ImVec2(10, 8);
    s.FramePadding = ImVec2(9, 5);
    s.ItemSpacing = ImVec2(9, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding = ImVec2(8, 4);
    s.IndentSpacing = 18.0f;
    s.ScrollbarSize = 13.0f;
    s.GrabMinSize = 11.0f;
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextPadding = ImVec2(16, 4);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.DockingSeparatorSize = 2.0f;
    s.TreeLinesFlags = ImGuiTreeNodeFlags_DrawLinesFull;
    s.TreeLinesSize = 1.0f;

    ImVec4* c = s.Colors;
    const ImVec4 accent = rgb(64, 156, 255);
    const ImVec4 accentDim = rgb(36, 92, 158);
    c[ImGuiCol_Text] = rgb(226, 229, 234);
    c[ImGuiCol_TextDisabled] = rgb(126, 132, 142);
    c[ImGuiCol_WindowBg] = rgb(24, 25, 28);
    c[ImGuiCol_ChildBg] = rgb(20, 21, 24);
    c[ImGuiCol_PopupBg] = rgb(30, 32, 36, 0.98f);
    c[ImGuiCol_Border] = rgb(50, 53, 59);
    c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_FrameBg] = rgb(37, 39, 44);
    c[ImGuiCol_FrameBgHovered] = rgb(46, 49, 56);
    c[ImGuiCol_FrameBgActive] = rgb(54, 58, 66);
    c[ImGuiCol_TitleBg] = rgb(18, 19, 21);
    c[ImGuiCol_TitleBgActive] = rgb(24, 25, 28);
    c[ImGuiCol_TitleBgCollapsed] = rgb(18, 19, 21);
    c[ImGuiCol_MenuBarBg] = rgb(18, 19, 21);
    c[ImGuiCol_ScrollbarBg] = rgb(20, 21, 24, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = rgb(66, 70, 78);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(88, 94, 104);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(110, 118, 130);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accentDim;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = rgb(48, 52, 60);
    c[ImGuiCol_ButtonHovered] = rgb(62, 68, 80);
    c[ImGuiCol_ButtonActive] = accentDim;
    c[ImGuiCol_Header] = rgb(44, 64, 92, 0.8f);
    c[ImGuiCol_HeaderHovered] = rgb(52, 76, 110, 0.9f);
    c[ImGuiCol_HeaderActive] = rgb(58, 88, 128);
    c[ImGuiCol_Separator] = rgb(50, 53, 59);
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = rgb(64, 156, 255, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = rgb(64, 156, 255, 0.5f);
    c[ImGuiCol_ResizeGripActive] = rgb(64, 156, 255, 0.9f);
    c[ImGuiCol_Tab] = rgb(28, 30, 34);
    c[ImGuiCol_TabHovered] = rgb(52, 76, 110);
    c[ImGuiCol_TabSelected] = rgb(40, 44, 52);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = rgb(24, 25, 28);
    c[ImGuiCol_TabDimmedSelected] = rgb(34, 37, 43);
    c[ImGuiCol_TabDimmedSelectedOverline] = accentDim;
    c[ImGuiCol_DockingPreview] = rgb(64, 156, 255, 0.35f);
    c[ImGuiCol_DockingEmptyBg] = rgb(14, 15, 17);
    c[ImGuiCol_TableHeaderBg] = rgb(30, 32, 37);
    c[ImGuiCol_TableBorderStrong] = rgb(50, 53, 59);
    c[ImGuiCol_TableBorderLight] = rgb(38, 40, 45);
    c[ImGuiCol_TableRowBg] = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.025f);
    c[ImGuiCol_TextSelectedBg] = rgb(64, 156, 255, 0.35f);
    c[ImGuiCol_DragDropTarget] = rgb(255, 196, 64);
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_TreeLines] = rgb(70, 74, 82);

    s.ScaleAllSizes(scale);
    s.FontSizeBase = kBaseFontSize;
    s.FontScaleDpi = scale;
    FOH_INFO("editor UI scale {:.3f}", scale);
}

} // namespace foh
