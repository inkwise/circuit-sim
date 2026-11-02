#include "Theme.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace Theme {
namespace {

const ImU32 kBackground = IM_COL32(11, 15, 20, 255);    // #0b0f14
const ImU32 kPanel = IM_COL32(18, 24, 33, 255);         // #121821
const ImU32 kPanelAlt = IM_COL32(26, 34, 48, 255);      // #1a2230
const ImU32 kSurface = IM_COL32(32, 42, 57, 255);       // #202a39
const ImU32 kSurfaceHigh = IM_COL32(41, 53, 70, 255);   // #293546
const ImU32 kCanvas = IM_COL32(11, 15, 20, 255);        // #0b0f14
const ImU32 kText = IM_COL32(243, 245, 247, 255);       // #f3f5f7
const ImU32 kTextDim = IM_COL32(183, 192, 204, 255);    // #b7c0cc
const ImU32 kMuted = IM_COL32(126, 138, 153, 255);      // #7e8a99
const ImU32 kAccent = IM_COL32(76, 141, 255, 255);      // #4c8dff
const ImU32 kGrid = IM_COL32(30, 39, 51, 255);          // #1e2733
const ImU32 kAxis = IM_COL32(44, 58, 76, 255);          // #2c3a4c
const ImU32 kDanger = IM_COL32(225, 91, 91, 255);       // #e15b5b
const ImU32 kWarning = IM_COL32(242, 179, 61, 255);     // #f2b33d
const ImU32 kGood = IM_COL32(51, 196, 129, 255);        // #33c481
const ImU32 kWire = IM_COL32(72, 178, 122, 255);       // #48b27a copper green
const ImU32 kStroke = IM_COL32(255, 255, 255, 26);
const ImU32 kAccentSoft = IM_COL32(76, 141, 255, 38);

// The dark categorical slots from the dataviz reference palette. Adjacent
// pairs clear the CVD/normal-vision gates against the app surface.
const ImU32 kSeries[] = {
    IM_COL32(57, 135, 229, 255),   // blue
    IM_COL32(217, 89, 38, 255),    // orange
    IM_COL32(25, 158, 112, 255),   // aqua
    IM_COL32(201, 133, 0, 255),    // yellow
    IM_COL32(213, 81, 129, 255),   // magenta
    IM_COL32(0, 131, 0, 255),      // green
    IM_COL32(144, 133, 233, 255),  // violet
    IM_COL32(230, 103, 103, 255),  // red
};

}  // namespace

void apply(float scale)
{
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    // All spacing/rounding values below are in density-independent pixels;
    // ScaleAllSizes() converts them to the phone's physical pixels.
    style.WindowPadding = ImVec2(14.0f, 10.0f);
    style.FramePadding = ImVec2(12.0f, 9.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
    style.TouchExtraPadding = ImVec2(6.0f, 6.0f);
    style.ScrollbarSize = 16.0f;
    style.GrabMinSize = 26.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowRounding = 16.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 10.0f;
    style.PopupRounding = 16.0f;
    style.ScrollbarRounding = 10.0f;
    style.GrabRounding = 10.0f;
    style.TabRounding = 10.0f;
    style.SelectableTextAlign = ImVec2(0.5f, 0.5f);
    style.WindowMinSize = ImVec2(80.0f, 60.0f);
    style.ScaleAllSizes(scale);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = toVec4(kText);
    colors[ImGuiCol_TextDisabled] = toVec4(kMuted);
    colors[ImGuiCol_WindowBg] = toVec4(kBackground);
    colors[ImGuiCol_ChildBg] = toVec4(kPanel);
    colors[ImGuiCol_PopupBg] = toVec4(kPanelAlt);
    colors[ImGuiCol_Border] = toVec4(kStroke);
    colors[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.13f, 0.17f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.13f, 0.18f, 0.24f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.17f, 0.23f, 0.31f, 1.0f);
    colors[ImGuiCol_TitleBg] = toVec4(kPanel);
    colors[ImGuiCol_TitleBgActive] = toVec4(kPanelAlt);
    colors[ImGuiCol_MenuBarBg] = toVec4(kPanel);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.15f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.34f, 0.42f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.42f, 0.51f, 1.0f);
    colors[ImGuiCol_CheckMark] = toVec4(kAccent);
    colors[ImGuiCol_SliderGrab] = toVec4(kAccent);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.44f, 0.68f, 0.98f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.11f, 0.15f, 0.20f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.15f, 0.21f, 0.29f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.19f, 0.27f, 0.37f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.14f, 0.20f, 0.27f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.26f, 0.35f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.31f, 0.42f, 1.0f);
    colors[ImGuiCol_Separator] = toVec4(kStroke);
    colors[ImGuiCol_Tab] = ImVec4(0.11f, 0.15f, 0.20f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.20f, 0.28f, 0.37f, 1.0f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.16f, 0.22f, 0.30f, 1.0f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.13f, 0.17f, 0.22f, 1.0f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
}

void loadFont(float scale)
{
    ImGuiIO& io = ImGui::GetIO();

    // On Android the APK's asset namespace is rooted at app/src/main/assets,
    // so the path is "fonts/..." (not "assets/fonts/..."). Keep a desktop-style
    // fallback so the same binary can also run outside the APK.
    const char* candidates[] = {
        "fonts/CircuitLabSansSC.ttf",
        "assets/fonts/CircuitLabSansSC.ttf",
    };
    void* data = nullptr;
    size_t dataSize = 0;
    for (const char* path : candidates) {
        data = SDL_LoadFile(path, &dataSize);
        if (data && dataSize > 0) {
            break;
        }
        data = nullptr;
        dataSize = 0;
    }

    if (data && dataSize > 0) {
        ImFontConfig config;
        config.OversampleH = 2;
        config.OversampleV = 1;
        config.PixelSnapH = false;
        // ImGui 1.92's dynamic atlas loads glyphs on demand. Passing no glyph
        // ranges keeps every character in the bundled font available.
        ImFont* font = io.Fonts->AddFontFromMemoryTTF(
            data, static_cast<int>(dataSize), 14.0f * scale, &config, nullptr);
        if (font) {
            io.FontDefault = font;
            return;
        }
        SDL_Log("Failed to add bundled CJK font");
        SDL_free(data);
    } else {
        SDL_Log("Could not load bundled font: %s", SDL_GetError());
    }
    io.Fonts->AddFontDefault();
}

ImU32 background() { return kBackground; }
ImU32 panel() { return kPanel; }
ImU32 panelAlt() { return kPanelAlt; }
ImU32 surface() { return kSurface; }
ImU32 surfaceHigh() { return kSurfaceHigh; }
ImU32 stroke() { return kStroke; }
ImU32 accentSoft() { return kAccentSoft; }
ImU32 canvas() { return kCanvas; }
ImU32 text() { return kText; }
ImU32 textDim() { return kTextDim; }
ImU32 muted() { return kMuted; }
ImU32 accent() { return kAccent; }

ImU32 wire() { return kWire; }
ImU32 grid() { return kGrid; }
ImU32 axis() { return kAxis; }
ImU32 danger() { return kDanger; }
ImU32 warning() { return kWarning; }
ImU32 good() { return kGood; }

ImU32 seriesColor(int index)
{
    const int count = static_cast<int>(sizeof(kSeries) / sizeof(kSeries[0]));
    if (index < 0) {
        index = 0;
    }
    if (index >= count) {
        return kMuted;
    }
    return kSeries[index];
}

ImVec4 toVec4(ImU32 color)
{
    return ImGui::ColorConvertU32ToFloat4(color);
}

}  // namespace Theme
