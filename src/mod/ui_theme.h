// Build 57: the overlay's look, all in one place.
// Placeholder style until the real design is made: change colours, sizes
// and fonts here, the screens (overlay.inc) only use these values.
#pragma once
#include "imgui.h"

namespace theme {

// Fonts (files in C:\Windows\Fonts; the built-in ImGui font if missing).
// Sizes are for a 1080-pixel-high picture and scale with the game's resolution.
constexpr const char* FONT_BODY = "segoeui.ttf";
constexpr const char* FONT_TITLE = "segoeuib.ttf";
constexpr float BODY_PX = 26.0f;
constexpr float TITLE_PX = 44.0f;
constexpr float SMALL_PX = 21.0f;

// Main panel: size as a share of the screen.
constexpr float PANEL_W = 0.62f;
constexpr float PANEL_H = 0.74f;

inline ImVec4 rgba(int r, int g, int b, int a = 255) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }

// Colours
inline ImVec4 Background()   { return rgba(14, 16, 28, 236); }
inline ImVec4 Panel()        { return rgba(26, 30, 50, 255); }
inline ImVec4 Accent()       { return rgba(214, 170, 82); }      // gold
inline ImVec4 AccentHover()  { return rgba(236, 196, 112); }
inline ImVec4 AccentDim()    { return rgba(120, 96, 50); }
inline ImVec4 Text()         { return rgba(236, 236, 244); }
inline ImVec4 TextDim()      { return rgba(150, 154, 176); }
inline ImVec4 Good()         { return rgba(110, 210, 140); }
inline ImVec4 Bad()          { return rgba(236, 96, 96); }
inline ImVec4 P1()           { return rgba(96, 156, 255); }
inline ImVec4 P2()           { return rgba(255, 110, 110); }

inline void Apply(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 10.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 6.0f;
    s.PopupRounding = 6.0f;
    s.GrabRounding = 6.0f;
    s.TabRounding = 6.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = ImVec2(22, 18);
    s.FramePadding = ImVec2(14, 8);
    s.ItemSpacing = ImVec2(12, 10);
    s.ItemInnerSpacing = ImVec2(10, 6);
    s.ScrollbarSize = 16.0f;
    s.ScaleAllSizes(scale);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = Text();
    c[ImGuiCol_TextDisabled] = TextDim();
    c[ImGuiCol_WindowBg] = Background();
    c[ImGuiCol_ChildBg] = Panel();
    c[ImGuiCol_PopupBg] = rgba(20, 24, 40, 250);
    c[ImGuiCol_Border] = rgba(214, 170, 82, 90);
    c[ImGuiCol_FrameBg] = rgba(40, 46, 74);
    c[ImGuiCol_FrameBgHovered] = rgba(58, 66, 104);
    c[ImGuiCol_FrameBgActive] = rgba(70, 80, 124);
    c[ImGuiCol_TitleBg] = Background();
    c[ImGuiCol_TitleBgActive] = Background();
    c[ImGuiCol_Button] = rgba(48, 54, 88);
    c[ImGuiCol_ButtonHovered] = AccentDim();
    c[ImGuiCol_ButtonActive] = Accent();
    c[ImGuiCol_Header] = rgba(48, 54, 88);
    c[ImGuiCol_HeaderHovered] = AccentDim();
    c[ImGuiCol_HeaderActive] = Accent();
    c[ImGuiCol_CheckMark] = Accent();
    c[ImGuiCol_SliderGrab] = Accent();
    c[ImGuiCol_SliderGrabActive] = AccentHover();
    c[ImGuiCol_Separator] = rgba(214, 170, 82, 70);
    c[ImGuiCol_NavHighlight] = Accent();
    c[ImGuiCol_TableHeaderBg] = rgba(36, 40, 66);
    c[ImGuiCol_TableRowBg] = rgba(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = rgba(255, 255, 255, 10);
}

} // namespace theme
