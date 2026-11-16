// The overlay's look, all in one place.
// Build 57: fixed values. Build 90 (UI playground): the values live in theme::T()
// and can be changed while the game runs and saved to ui_theme.ini next to the mod;
// the screens only use these values.
//
// Sizes are in "canvas pixels": the overlay is designed for a 1920x1080 picture and
// everything is scaled with the game's resolution (scale = min(width/1920, height/1080)).
#pragma once
#include "imgui.h"
#include <cstring>

namespace theme {

inline ImVec4 rgba(int r, int g, int b, int a = 255) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }

// Main panel of the normal overlay: size as a share of the screen.
constexpr float PANEL_W = 0.62f;
constexpr float PANEL_H = 0.74f;

struct Look {
    // fonts: a file name, looked for in <mod folder>\fonts\ first, then C:\Windows\Fonts
    char fontBody[64] = "segoeui.ttf";
    char fontTitle[64] = "segoeuib.ttf";
    float bodyPx = 26.0f, titlePx = 44.0f, smallPx = 21.0f;
    // own colours (the ImGui ones are in base.Colors)
    ImVec4 accent = rgba(214, 170, 82), accentHover = rgba(236, 196, 112), accentDim = rgba(120, 96, 50);
    ImVec4 accent2 = rgba(96, 156, 255);
    ImVec4 text = rgba(236, 236, 244), textDim = rgba(150, 154, 176);
    ImVec4 good = rgba(110, 210, 140), bad = rgba(236, 96, 96);
    ImVec4 p1 = rgba(96, 156, 255), p2 = rgba(255, 110, 110);
    // playground / full-screen menus
    int bgMode = 1;                         // 0 solid, 1 gradient, 2 game picture
    ImVec4 bgTop = rgba(10, 12, 24), bgBottom = rgba(36, 20, 44);
    int bgArtSheet = 0, bgArtSprite = 1;    // game picture (sheet index in the playground list, sprite)
    float bgDim = 0.45f;                    // darkening over a game picture
    float glow = 0.6f;                      // strength of glows (0 = none)
    ImVec4 glowColor = rgba(214, 170, 82);
    float animSpeed = 1.0f;                 // 0 = no animation
    int buttonStyle = 0;                    // 0 filled, 1 outlined + glow, 2 slanted
    bool panelArt = true;                   // panels use the game's frame picture
    int panelArtSprite = 5;                 // MA10_CSEL frame sprite (5: thin gold frame)
    float panelArtSlice = 24.0f;            // corner size of that frame (canvas pixels)
    ImGuiStyle base;                        // unscaled ImGui style (sizes for a 1080p picture)
};

inline void DefaultBase(ImGuiStyle& s) {
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
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = rgba(236, 236, 244);
    c[ImGuiCol_TextDisabled] = rgba(150, 154, 176);
    c[ImGuiCol_WindowBg] = rgba(14, 16, 28, 236);
    c[ImGuiCol_ChildBg] = rgba(26, 30, 50, 255);
    c[ImGuiCol_PopupBg] = rgba(20, 24, 40, 250);
    c[ImGuiCol_Border] = rgba(214, 170, 82, 90);
    c[ImGuiCol_FrameBg] = rgba(40, 46, 74);
    c[ImGuiCol_FrameBgHovered] = rgba(58, 66, 104);
    c[ImGuiCol_FrameBgActive] = rgba(70, 80, 124);
    c[ImGuiCol_TitleBg] = rgba(14, 16, 28, 236);
    c[ImGuiCol_TitleBgActive] = rgba(14, 16, 28, 236);
    c[ImGuiCol_Button] = rgba(48, 54, 88);
    c[ImGuiCol_ButtonHovered] = rgba(120, 96, 50);
    c[ImGuiCol_ButtonActive] = rgba(214, 170, 82);
    c[ImGuiCol_Header] = rgba(48, 54, 88);
    c[ImGuiCol_HeaderHovered] = rgba(120, 96, 50);
    c[ImGuiCol_HeaderActive] = rgba(214, 170, 82);
    c[ImGuiCol_CheckMark] = rgba(214, 170, 82);
    c[ImGuiCol_SliderGrab] = rgba(214, 170, 82);
    c[ImGuiCol_SliderGrabActive] = rgba(236, 196, 112);
    c[ImGuiCol_Separator] = rgba(214, 170, 82, 70);
    c[ImGuiCol_NavHighlight] = rgba(214, 170, 82);
    c[ImGuiCol_TableHeaderBg] = rgba(36, 40, 66);
    c[ImGuiCol_TableRowBg] = rgba(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = rgba(255, 255, 255, 10);
}

inline Look& T() {
    static Look* t = nullptr;
    if (!t) { t = new Look(); DefaultBase(t->base); }
    return *t;
}
inline void ResetToDefault() { Look* n = new Look(); DefaultBase(n->base); T() = *n; delete n; }

// Colours used by the screens
inline ImVec4 Background()   { return T().base.Colors[ImGuiCol_WindowBg]; }
inline ImVec4 Panel()        { return T().base.Colors[ImGuiCol_ChildBg]; }
inline ImVec4 Accent()       { return T().accent; }
inline ImVec4 AccentHover()  { return T().accentHover; }
inline ImVec4 AccentDim()    { return T().accentDim; }
inline ImVec4 Text()         { return T().text; }
inline ImVec4 TextDim()      { return T().textDim; }
inline ImVec4 Good()         { return T().good; }
inline ImVec4 Bad()          { return T().bad; }
inline ImVec4 P1()           { return T().p1; }
inline ImVec4 P2()           { return T().p2; }

// The live ImGui style = the theme's base style, scaled to the picture.
inline void Apply(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = T().base;
    s.Colors[ImGuiCol_Text] = T().text;
    s.Colors[ImGuiCol_TextDisabled] = T().textDim;
    s.ScaleAllSizes(scale);
}

} // namespace theme
