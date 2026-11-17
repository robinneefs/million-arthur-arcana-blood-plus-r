#include "version.h"   // build 105: MOD_VERSION, links
// ===========================================================================
// Our main menu: the game boots into it (build 101). Room search / Create room (roomui.inc),
// Settings (settingsui.inc), Help (helpui.inc), Quit; My setup card (setupui.inc).
// House style: Crimson palette, Michroma for titles and entries, Saira Semi Condensed for
// sentences, magic circles + embers background, the game's title-screen characters.
// Game pictures come from the player's install (art::Get) and are never shipped.
// Laid out on the 1920x1080 canvas (CP / CV, canvas.inc).
// ===========================================================================

static ImFont *g_fMenu = nullptr, *g_fUi = nullptr, *g_fUiBold = nullptr;
static const float MM_MENU_PX = 64.0f;    // size the fonts are made at (canvas pixels, line height); drawn at this size or smaller
static const float MM_UI_PX = 56.0f;
// sizes below are given like in a picture editor (em size); ImGui sizes fonts by line height
// Build 120b: each font also made at smaller sizes; MmText draws with the smallest one that is
// at least as big as the text, so small text (like HOST) isn't shrunk from the 64 px font and
// stays sharp. Sizes are line heights in canvas pixels, like MM_MENU_PX.
static const int MM_TIERS = 4;
static const float MM_TIER_PX[MM_TIERS] = {16.0f, 24.0f, 34.0f, 46.0f};
static ImFont *g_tMenu[MM_TIERS] = {}, *g_tUi[MM_TIERS] = {}, *g_tUiBold[MM_TIERS] = {};
static ImFont* MmSharp(ImFont* f, float sz) {
    ImFont** t = f == g_fMenu ? g_tMenu : f == g_fUi ? g_tUi : f == g_fUiBold ? g_tUiBold : nullptr;
    if (!t) return f;
    for (int i = 0; i < MM_TIERS; i++)
        if (t[i] && t[i]->FontSize >= sz * 0.92f) return t[i];
    return f;
}
static inline float MmEm(ImFont* f) { return f == g_fMenu ? 1.4219f : (f == g_fUi || f == g_fUiBold) ? 1.574f : 1.0f; }

static struct Mm {
    int sel = 0;
    double openT = -1;
    char note[128] = "";
    double noteUntil = 0;
    uint32_t prevNav = 0;
    double repeatAt = 0;
    bool wasOpen = false;
    char music[32] = "";            // build 92: name of the music playing (set by the game thread)
    int screen = 0;                 // build 99: 0 main menu, 1 room (lobby) preview
    double roomT = -1;
    int quietFrame = -1;            // build 101: a screen switched to the main menu this frame: no key presses on it
} MM;
// build 92: requests for the game thread (sound.inc): bit 0 move, 1 select, 2 back
static std::atomic<uint32_t> MM_SND{0};
// build 104: Settings switches (rollback.ini [ui]): menu sounds, menu music, connection info in a match
static std::atomic<bool> g_menuSfx{true}, g_menuMusic{true}, g_netInfo{true}, g_musicReapply{false};
static std::atomic<bool> g_borderless{false}, g_borderlessFirst{false};   // build 110: [ui] borderless (windowed game shown as a borderless full-screen window)
static void MmLoadPrefs() {
    static bool done = false;
    if (done) return;
    done = true;
#ifndef UI_HARNESS
    wchar_t ini[MAX_PATH];
    _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
    g_menuSfx = GetPrivateProfileIntW(L"ui", L"menu_sounds", 1, ini) != 0;
    g_menuMusic = GetPrivateProfileIntW(L"ui", L"menu_music", 1, ini) != 0;
    g_netInfo = GetPrivateProfileIntW(L"ui", L"connection_info", 1, ini) != 0;
    {   // build 111: Borderless is the default (first start with this build: no setting yet)
        int b = GetPrivateProfileIntW(L"ui", L"borderless", -1, ini);
        if (b < 0) { g_borderlessFirst = true; b = 1; WritePrivateProfileStringW(L"ui", L"borderless", L"1", ini); }
        g_borderless = b != 0;
    }
#endif
}
static bool MmNetInfoOn() { MmLoadPrefs(); return g_netInfo.load(); }
static void MmSavePref(const wchar_t* key, int v) {
#ifndef UI_HARNESS
    wchar_t ini[MAX_PATH], w[16];
    _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
    _snwprintf(w, 16, L"%d", v);
    WritePrivateProfileStringW(L"ui", key, w, ini);
#else
    (void)key; (void)v;
#endif
}
static net::RoomView g_rv;              // build 100: this frame's copy of the Steam room state (roomui.inc)

#ifndef UI_HARNESS
// additive drawing for the game's glow pictures (they are white-on-black)
static void MmBlendAdd(const ImDrawList*, const ImDrawCmd*) {
    if (!g_dev) return;
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
}
// build 95: on a light background the glow pictures darken instead: dest * (1 - picture colour)
static void MmBlendMul(const ImDrawList*, const ImDrawCmd*) {
    if (!g_dev) return;
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCCOLOR);
}
#else
static void MmBlendAdd(const ImDrawList*, const ImDrawCmd*) {}
static void MmBlendMul(const ImDrawList*, const ImDrawCmd*) {}
#endif

static inline ImU32 MmRGBA(int r, int g, int b, float a = 1.0f) { return IM_COL32(r, g, b, (int)(ImClamp(a, 0.0f, 1.0f) * 255)); }

// ---- build 95: colour scheme taken from the mod's logo (red, cream white, gold); the user picked Crimson ----
struct MmRgb { int r, g, b; };
static inline ImU32 MmC(MmRgb c, float a = 1.0f) { return MmRGBA(c.r, c.g, c.b, a); }
struct MmPal {
    const char* name;
    MmRgb bgTop, bgBot;      // background gradient
    MmRgb acc;               // lines, frames
    MmRgb gold;              // notes, small highlights
    MmRgb text, dim, faint;  // text: main, secondary, hints
    MmRgb ink;               // outline around menu letters
    MmRgb panel;             // card / panel fill
    MmRgb selFill, selOut;   // the selected entry
    MmRgb glow;              // glows and particles
    MmRgb shade;             // darkening under the description line
    MmRgb charTint;          // the character behind
    bool light;              // light background: glows drawn normally instead of added
};
static const MmPal MM_PALS[] = {
    {"CRIMSON", {46, 5, 7}, {8, 1, 2}, {215, 38, 32}, {255, 206, 96}, {255, 247, 232}, {232, 196, 170}, {170, 120, 110},
     {40, 4, 4}, {26, 5, 5}, {255, 206, 96}, {90, 14, 6}, {255, 80, 36}, {10, 0, 0}, {255, 236, 230}, false},
};
static const int MM_NPALS = sizeof MM_PALS / sizeof MM_PALS[0];
static inline const MmPal& MmP() { return MM_PALS[0]; }   // build 104: one colour scheme only (the others are gone)
static inline float MmHash(int i, int k) {   // 0..1, fixed per (i, k)
    uint32_t h = (uint32_t)i * 374761393u + (uint32_t)k * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFFF) / (float)0x1000000;
}
// glow pictures (white on black): added on dark schemes; on a light scheme they tint the
// background toward `c` instead (the colour given to the picture is then 1 - c, times strength)
static inline ImU32 MmGlowCol(MmRgb c, float a) {
    if (!MmP().light) return MmC(c, a);
    a = ImClamp(a, 0.0f, 1.0f);
    return IM_COL32((int)((255 - c.r) * a), (int)((255 - c.g) * a), (int)((255 - c.b) * a), 255);
}
static inline void MmGlowBegin(ImDrawList* dl) { dl->AddCallback(MmP().light ? MmBlendMul : MmBlendAdd, nullptr); }

// one sprite into a canvas rectangle (w/h <= 0: the sprite's own size)
static void MmImg(ImDrawList* dl, const char* file, int spr, float x, float y, float w, float h, ImU32 tint = IM_COL32_WHITE,
                  ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1), bool additive = false) {
    ImVec2 ts;
    void* tex = art::Get(file, spr, &ts);
    if (!tex) return;
    if (w <= 0) w = ts.x * (uv1.x - uv0.x);
    if (h <= 0) h = ts.y * (uv1.y - uv0.y);
    if (additive) MmGlowBegin(dl);
    dl->AddImage((ImTextureID)tex, CP(x, y), CP(x + w, y + h), uv0, uv1, tint);
    if (additive) dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}
// a sprite turned by `ang` around its centre (canvas pixels)
static void MmImgRot(ImDrawList* dl, const char* file, int spr, float cx, float cy, float size, float ang, ImU32 tint, bool additive) {
    ImVec2 ts;
    void* tex = art::Get(file, spr, &ts);
    if (!tex) return;
    float hw = size * 0.5f, hh = size * 0.5f * ts.y / ts.x, c = cosf(ang), s = sinf(ang);
    ImVec2 p[4];
    const float dx[4] = {-hw, hw, hw, -hw}, dy[4] = {-hh, -hh, hh, hh};
    for (int i = 0; i < 4; i++) p[i] = CP(cx + dx[i] * c - dy[i] * s, cy + dx[i] * s + dy[i] * c);
    if (additive) MmGlowBegin(dl);
    dl->AddImageQuad((ImTextureID)tex, p[0], p[1], p[2], p[3], ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), tint);
    if (additive) dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}
// a thin line that fades out at both ends (instead of the game's blue line pictures)
static void MmLine(ImDrawList* dl, float x, float y, float w, float th, ImU32 col) {
    ImU32 clear = col & ~IM_COL32_A_MASK;
    float q = w * 0.15f;
    dl->AddRectFilledMultiColor(CP(x, y), CP(x + q, y + th), clear, col, col, clear);
    dl->AddRectFilled(CP(x + q, y), CP(x + w - q, y + th), col);
    dl->AddRectFilledMultiColor(CP(x + w - q, y), CP(x + w, y + th), col, clear, clear, col);
}
// a sprite with its left edge fading out (left colour -> right colour across `fadeW` canvas pixels)
static void MmImgFadeLeft(ImDrawList* dl, const char* file, int spr, float x, float y, float w, float h, float fadeW, ImU32 tint) {
    ImVec2 ts;
    void* tex = art::Get(file, spr, &ts);
    if (!tex) return;
    float u = ImClamp(fadeW / w, 0.0f, 1.0f);
    ImU32 clear = tint & ~IM_COL32_A_MASK;
    dl->PushTextureID((ImTextureID)tex);
    dl->PrimReserve(12, 8);
    ImVec2 a = CP(x, y), m = CP(x + fadeW, y + h), b = CP(x + w, y + h);
    auto quad = [&](ImVec2 p0, ImVec2 p1, float u0, float u1, ImU32 c0, ImU32 c1) {
        ImDrawIdx i = (ImDrawIdx)dl->_VtxCurrentIdx;
        dl->PrimWriteIdx(i); dl->PrimWriteIdx((ImDrawIdx)(i + 1)); dl->PrimWriteIdx((ImDrawIdx)(i + 2));
        dl->PrimWriteIdx(i); dl->PrimWriteIdx((ImDrawIdx)(i + 2)); dl->PrimWriteIdx((ImDrawIdx)(i + 3));
        dl->PrimWriteVtx(ImVec2(p0.x, p0.y), ImVec2(u0, 0), c0);
        dl->PrimWriteVtx(ImVec2(p1.x, p0.y), ImVec2(u1, 0), c1);
        dl->PrimWriteVtx(ImVec2(p1.x, p1.y), ImVec2(u1, 1), c1);
        dl->PrimWriteVtx(ImVec2(p0.x, p1.y), ImVec2(u0, 1), c0);
    };
    quad(a, ImVec2(m.x, b.y), 0, u, clear, tint);
    quad(ImVec2(m.x, a.y), b, u, 1, tint, tint);
    dl->PopTextureID();
}
// a frame picture stretched without stretching its corners; slice in picture pixels, drawn `on` canvas pixels
static void MmNine(ImDrawList* dl, const char* file, int spr, float slice, float on, float x0, float y0, float x1, float y1, ImU32 tint = IM_COL32_WHITE) {
    ImVec2 ts;
    void* tex = art::Get(file, spr, &ts);
    if (!tex) return;
    float xs[4] = {x0, x0 + on, x1 - on, x1}, ys[4] = {y0, y0 + on, y1 - on, y1};
    float us[4] = {0, slice / ts.x, 1 - slice / ts.x, 1}, vs[4] = {0, slice / ts.y, 1 - slice / ts.y, 1};
    for (int yy = 0; yy < 3; yy++)
        for (int xx = 0; xx < 3; xx++)
            dl->AddImage((ImTextureID)tex, CP(xs[xx], ys[yy]), CP(xs[xx + 1], ys[yy + 1]), ImVec2(us[xx], vs[yy]), ImVec2(us[xx + 1], vs[yy + 1]), tint);
}
// text: align 0 left / 1 centre / 2 right, vertically centred on y; outline and extra weight in canvas pixels
static float MmText(ImDrawList* dl, ImFont* f, float px, float x, float y, ImU32 col, const char* s, int align = 0, float out = 0,
                    ImU32 oc = 0, float bold = 0) {
    if (!f) f = ImGui::GetFont();
    float sz = CV(px * MmEm(f));
    ImVec2 ts = f->CalcTextSizeA(sz, FLT_MAX, 0.0f, s);
    ImVec2 p = CP(x, y);
    if (align == 1) p.x -= ts.x * 0.5f;
    else if (align == 2) p.x -= ts.x;
    p.y -= ts.y * 0.5f;
    p.x = floorf(p.x); p.y = floorf(p.y);
    f = MmSharp(f, sz);
    if (out > 0) {
        float o = CV(out);
        for (int i = 0; i < 12; i++) {
            float a = i * IM_PI * 2 / 12;
            dl->AddText(f, sz, ImVec2(p.x + cosf(a) * o, p.y + sinf(a) * o), oc, s);
        }
    }
    if (bold > 0) {
        float o = CV(bold);
        dl->AddText(f, sz, ImVec2(p.x - o, p.y), col, s);
        dl->AddText(f, sz, ImVec2(p.x + o, p.y), col, s);
    }
    dl->AddText(f, sz, p, col, s);
    return ts.x / g_cv;
}
// a controller button badge (A green, B red, X blue, Y yellow)
// Build 117: a click on a badge (or the word next to it) works like pressing that button; the
// screen reads it the next frame (MmTakePadClick), since screens read input before they draw.
// (ImGui::IsMouseHoveringRect needs "false" outside a window: else it only tests ImGui's default 400x400 window.)
static char g_padClick = 0;
static int g_padClickFrame = -10;
static char MmTakePadClick() {
    char c = g_padClick;
    g_padClick = 0;
    return ImGui::GetFrameCount() - g_padClickFrame <= 1 ? c : 0;
}
static void MmPadButton(ImDrawList* dl, float x, float y, char letter, ImU32 fill) {
    {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 a = CP(x - 6, y - 6), b = CP(x + 128, y + 42), m = io.MousePos;
        bool blocked = false;
#ifndef UI_HARNESS
        blocked = g_uiBlock;
#endif
        if (!blocked && io.MouseClicked[0] && m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y) { g_padClick = letter; g_padClickFrame = ImGui::GetFrameCount(); }
    }
    ImVec2 c = CP(x + 18, y + 18);
    dl->AddCircleFilled(c, CV(18), fill, 32);
    dl->AddCircle(c, CV(18), IM_COL32_WHITE, 32, CV(2));
    char s[2] = {letter, 0};
    MmText(dl, g_fUiBold, 24, x + 18, y + 18, IM_COL32_WHITE, s, 1, 1, IM_COL32(0, 0, 0, 200));
}
// the game's data box: navy rounded border, a label on the top edge between two lines
static void MmFieldBox(ImDrawList* dl, float x0, float y0, float x1, float y1, const char* label, ImU32 fill) {
    dl->AddRectFilled(CP(x0, y0), CP(x1, y1), fill, CV(8));
    const MmPal& P = MmP();
    dl->AddRect(CP(x0, y0), CP(x1, y1), MmC(P.acc, 0.9f), CV(8), 0, CV(3));
    dl->AddRect(CP(x0 + 4, y0 + 4), CP(x1 - 4, y1 - 4), MmC(P.gold, 0.45f), CV(6), 0, CV(1));
    if (label && *label) {
        float cx = (x0 + x1) * 0.5f;
        float tw = (g_fMenu ? g_fMenu : ImGui::GetFont())->CalcTextSizeA(CV(17 * MmEm(g_fMenu)), FLT_MAX, 0.0f, label).x / g_cv;
        dl->AddLine(CP(x0 + 18, y0 - 15), CP(cx - tw / 2 - 14, y0 - 15), MmC(P.dim), CV(2));
        dl->AddLine(CP(cx + tw / 2 + 14, y0 - 15), CP(x1 - 18, y0 - 15), MmC(P.dim), CV(2));
        MmText(dl, g_fMenu, 17, cx, y0 - 15, MmC(P.text), label, 1, 1, MmC(P.ink));
    }
}
// round icons for the entries the game has no picture for
static void MmRoundIcon(ImDrawList* dl, float x, float y, char kind, ImU32 fill, ImU32 ink) {
    ImVec2 c = CP(x + 26, y + 28);
    dl->AddCircleFilled(c, CV(23), fill, 40);
    dl->AddCircle(c, CV(23), ink, 40, CV(2));
    if (kind == '?') MmText(dl, g_fUiBold, 34, x + 26, y + 28, ink, "?", 1);
    else {   // power symbol
        dl->PathArcTo(c, CV(12), -IM_PI * 0.5f + 0.6f, IM_PI * 1.5f - 0.6f, 28);
        dl->PathStroke(ink, false, CV(4));
        dl->AddLine(CP(x + 26, y + 12), CP(x + 26, y + 28), ink, CV(4));
    }
}

#ifndef UI_HARNESS
static const wchar_t* MmLogoPath() {
    static wchar_t p[MAX_PATH] = L"";
    if (!p[0]) _snwprintf(p, MAX_PATH, L"%lsrollback_logo.png", util::ModDir());
    return p;
}
#else
static const wchar_t* MmLogoPath() { return L"raw_logo.raw"; }
#endif
static const char* const MM_ITEMS[5] = {"ROOM SEARCH", "CREATE ROOM", "SETTINGS", "HELP", "QUIT"};
static const char* const MM_HELP[5] = {"Find a public room, or join a friend's room by invite.",
                                       "Open your own room: public or private, 2 to 8 players.",
                                       "Controls, sound, display, online and logs.",
                                       "How to play online, connection trouble, reporting problems, links and credits.",
                                       "Close the game (press twice)."};

static void MmNote(const char* s) {
    snprintf(MM.note, sizeof MM.note, "%s", s);
    MM.noteUntil = ImGui::GetTime() + 2.5;
}
static void SettingsOpen();   // build 104 (settingsui.inc)
static void HelpOpen();       // build 105 (helpui.inc)
static void MmActivate(int i) {
    MM_SND |= 2;
    if (i == 4) {   // build 101: Quit closes the game (press it twice)
        static double askedAt = -10;
        double now = ImGui::GetTime();
        if (now - askedAt > 3.0) { askedAt = now; MmNote("Press A again to close the game"); return; }
#ifndef UI_HARNESS
        Log("ui: Quit picked on the main menu, closing the game");
        if (g_uiHwnd) PostMessageW(g_uiHwnd, WM_CLOSE, 0, 0);
#endif
        return;
    }
    // build 100: in a room already: back to it; else Room search (0) / Create room (1)
    if (i == 0 || i == 1) { MM.screen = g_rv.state == net::ROOM_IN ? 1 : i == 0 ? 2 : 3; MM.roomT = -1; return; }
    if (i == 2) { SettingsOpen(); return; }   // build 104
    if (i == 3) { HelpOpen(); return; }       // build 105
}

static void SuOpen();   // build 102 (setupui.inc)
// input: arrows / Enter / Esc, mouse, and the controllers (navBits from the game thread)
static void MmInput(const ImVec2* itemA, const ImVec2* itemB) {
    ImGuiIO& io = ImGui::GetIO();
    double now = ImGui::GetTime();
    int move = 0;
    bool act = false, back = false, edit = false, join = false;
    if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_UpArrow])) move = -1;
    if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_DownArrow])) move = 1;
    if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Enter]) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Space])) act = true;
    if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Escape]) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Backspace])) back = true;
#ifndef UI_HARNESS
    uint32_t nb = U.capture.load() ? U.navBits : 0, fresh = nb & ~MM.prevNav;
    if (fresh & PB_UP) { move = -1; MM.repeatAt = now + 0.35; }
    else if (fresh & PB_DOWN) { move = 1; MM.repeatAt = now + 0.35; }
    else if ((nb & (PB_UP | PB_DOWN)) && now >= MM.repeatAt) { move = (nb & PB_UP) ? -1 : 1; MM.repeatAt = now + 0.12; }
    if (fresh & PB_A) act = true;
    if (fresh & PB_B) back = true;
    if (fresh & PB_Y) edit = true;
    if (fresh & PB_X) join = true;
    MM.prevNav = nb;
#endif
    if (ImGui::IsKeyPressed('Y')) edit = true;
    if (ImGui::IsKeyPressed('X')) join = true;
    if (ImGui::GetFrameCount() == MM.quietFrame) { move = 0; act = back = edit = join = false; }
    if (join) {   // build 100: X joins the room of the invite on the card
        if (g_rv.haveInvite) { net::RoomJoin(g_rv.invite.lobby); MM_SND |= 2; MM.screen = 1; MM.roomT = -1; return; }
        if (g_rv.state == net::ROOM_IN) { MM_SND |= 2; MM.screen = 1; MM.roomT = -1; return; }
    }
    if (move) { MM.sel = (MM.sel + move + 5) % 5; MM_SND |= 1; }
    // mouse: hovering picks, a click activates
    if (io.MousePos.x > -FLT_MAX / 2 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || io.MouseClicked[0]))
        for (int i = 0; i < 5; i++)
            if (io.MousePos.x >= itemA[i].x && io.MousePos.x < itemB[i].x && io.MousePos.y >= itemA[i].y && io.MousePos.y < itemB[i].y) {
                if (MM.sel != i) MM_SND |= 1;
                MM.sel = i;
                if (io.MouseClicked[0]) act = true;
            }
    if (act) MmActivate(MM.sel);
    if (edit) { MM_SND |= 2; SuOpen(); return; }   // build 102: My setup editor
    (void)back;   // build 101: the main menu is the front end now; B / Esc don't leave it (F1 still closes it)
}

// ---- build 96: the background: slowly turning magic circles in drifting warm light, with rising embers ----
static void MmBackground(ImDrawList* bg, float tm, float pulse) {
    const MmPal& P = MmP();
    ImVec2 A(0, 0), B = g_disp;
    bg->AddRectFilledMultiColor(A, B, MmC(P.bgTop), MmC(P.bgTop), MmC(P.bgBot), MmC(P.bgBot));
    // canvas coordinates that cover the whole screen (wider / taller screens too)
    float L = -g_cvO.x / g_cv, T = -g_cvO.y / g_cv, R = L + g_disp.x / g_cv, Bt = T + g_disp.y / g_cv;
    (void)T;
    bg->PushClipRect(A, B, false);
    // a big soft light behind the characters
    MmImg(bg, "MA10_CMN", 39, 760, -120, 1300, 1300, MmGlowCol(P.glow, 0.28f + 0.06f * pulse), ImVec2(0, 0), ImVec2(1, 1), true);
    // warm clouds of light drifting slowly
    for (int i = 0; i < 4; i++) {
        float x = 300 + 1400 * MmHash(i, 1) + sinf(tm * (0.05f + 0.05f * MmHash(i, 2)) + i * 2) * 260;
        float y = 200 + 700 * MmHash(i, 3) + cosf(tm * 0.07f + i) * 120, sz = 700 + 500 * MmHash(i, 4);
        MmImg(bg, "MA10_CMN", 39, x - sz / 2, y - sz / 2, sz, sz, MmGlowCol(i % 2 ? P.acc : P.gold, 0.16f), ImVec2(0, 0), ImVec2(1, 1), true);
    }
    // magic circles (the game's white ring pictures, tinted), each turning at its own speed
    struct Ring { int spr; float x, y, size, speed, a; bool gold; };
    static const Ring rings[] = {
        {32, 1330, 470, 1250, 0.030f, 0.20f, true}, {3, 1330, 470, 860, -0.050f, 0.18f, false}, {13, 1330, 470, 620, 0.080f, 0.14f, true},
        {9, 240, 180, 520, -0.060f, 0.09f, false}, {32, 420, 1020, 700, 0.045f, 0.09f, true}, {3, 1800, 980, 420, -0.070f, 0.11f, false},
    };
    for (const Ring& r : rings)
        MmImgRot(bg, "MA10_CMN", r.spr, r.x, r.y, r.size, tm * r.speed, MmGlowCol(r.gold ? P.gold : P.acc, r.a), true);
    // embers rising with a flicker, a few of them larger and closer
    MmGlowBegin(bg);
    for (int i = 0; i < 90; i++) {
        float sp = 25 + 70 * MmHash(i, 1), x = L + (R - L) * MmHash(i, 2) + sinf(tm * (0.5f + MmHash(i, 6)) + i) * 30;
        float y = Bt + 20 - fmodf(tm * sp + MmHash(i, 3) * 1400, 1400);
        float fl = 0.55f + 0.45f * sinf(tm * (3 + 4 * MmHash(i, 4)) + i * 1.7f);
        float r = 1.5f + 3.0f * MmHash(i, 5) * MmHash(i, 8);
        MmRgb c = MmHash(i, 7) < 0.6f ? P.glow : P.gold;
        bg->AddCircleFilled(CP(x, y), CV(r * 3), MmGlowCol(c, 0.10f * fl), 12);
        bg->AddCircleFilled(CP(x, y), CV(r), MmGlowCol(c, 0.85f * fl), 8);
    }
    bg->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    bg->PopClipRect();
}

static bool g_mmDrew = false;   // build 101: a page was drawn this frame
#include "roomui.inc"
#include "setupui.inc"   // build 102: My setup editor
#include "settingsui.inc"   // build 104: Settings
#include "helpui.inc"       // build 105: Help
static void MmMainFrame();
static void MmFrame() {
    net::RoomGet(&g_rv);   // build 100: this frame's copy of the Steam room state
    // build 101: while you are in a room (or getting into one, e.g. from a Steam invite),
    // the room screen is the only screen; leaving the room brings the main menu back
    bool roomBusy = g_rv.state == net::ROOM_IN || g_rv.state == net::ROOM_CREATING || g_rv.state == net::ROOM_JOINING;
    if (roomBusy && MM.screen != 1 && MM.screen != 4) { MM.screen = 1; MM.roomT = -1; }   // build 112: My setup can be edited from the room
    // a screen that hands over to another one mid-frame draws nothing itself: draw the
    // next one in the same frame, so the game's picture never shows through (build 100 flash)
    for (int k = 0; k < 3; k++) {
        int sc = MM.screen;
        g_mmDrew = false;
        if (sc == 1) MmRoomFrame();
        else if (sc == 2) MmSearchFrame();
        else if (sc == 3) MmCreateFrame();
        else if (sc == 4) MmSetupFrame();
        else if (sc == 5) MmSettingsFrame();
        else if (sc == 6) MmHelpFrame();
        else { MmMainFrame(); return; }
        if (g_mmDrew || MM.screen == sc) return;
    }
}
static void MmMainFrame() {
    double now = ImGui::GetTime();
    if (!MM.wasOpen || MM.openT < 0) MM.openT = now;
#ifndef UI_HARNESS
    if (!MM.wasOpen) MM.prevNav = 0xFFFFFFFFu;   // build 108b: a button still held (e.g. OK on a pop-up) must be let go first
#endif
    MM.wasOpen = true;
    const MmPal& P = MmP();
    float t = (float)(now - MM.openT);
    float anim = theme::T().animSpeed > 0 ? theme::T().animSpeed : 0.0f;
    auto ease = [&](float start, float len) {   // 0..1 intro curve
        if (anim <= 0) return 1.0f;
        float v = ImClamp((t * anim - start) / len, 0.0f, 1.0f);
        return 1 - (1 - v) * (1 - v) * (1 - v);
    };

    // ---- background, over the whole picture ----
    float pulse = 0.5f + 0.5f * sinf((float)(now * anim) * 0.8f);
    MmBackground(ImGui::GetBackgroundDrawList(), (float)(now * anim), pulse);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(g_disp);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##mm", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(CP(0, 0), CP(1920, 1080), true);

    // ---- the two characters of the game's title screen ----
    {
        float in1 = ease(0.05f, 0.7f), in2 = ease(0.15f, 0.7f);
        float sway = sinf((float)(now * anim) * 0.6f) * 4.0f;
        // girl (MA10_TITLE 2) behind, to the left; boy (MA10_TITLE 0) in front
        // build 96: a soft white glow around each of them, like the game's title screen but fainter: the game's
        // glowing versions of the pictures (MA10_TITLE 3 and 1) drawn just behind, so only their edge shows
        float glow = 0.42f + 0.10f * pulse;
        // build 97: placed like the game's title screen: he sits on the right edge (his arm goes off screen),
        // she stands to his left, behind him
        // build 98: both a touch larger than the screen height and placed a little above the top, so her
        // hands (cut in the picture itself) always run off the top edge, even while they sway
        const float cs = 1.012f;
        float gx = 700 + (1 - in2) * 80, gy = -7 + sway * 0.5f, bx = 1920 - 1033 * cs + (1 - in1) * 80, by = -7 - sway * 0.5f;
        MmImgFadeLeft(dl, "MA10_TITLE", 3, gx, gy, 992 * cs, 1080 * cs, 300, MmRGBA(255, 244, 236, glow * in2));
        MmImgFadeLeft(dl, "MA10_TITLE", 2, gx, gy, 992 * cs, 1080 * cs, 300, MmC(P.charTint, in2));
        MmImgFadeLeft(dl, "MA10_TITLE", 1, bx, by, 1033 * cs, 1080 * cs, 120, MmRGBA(255, 244, 236, glow * in1));
        MmImgFadeLeft(dl, "MA10_TITLE", 0, bx, by, 1033 * cs, 1080 * cs, 120, MmRGBA(255, 255, 255, in1));
    }
    // darken (or lighten) the bottom for the description line and the left side for the menu
    // (these two reach the picture's edges on screens wider or taller than 16:9)
    dl->PushClipRect(ImVec2(0, 0), g_disp, false);
    dl->AddRectFilledMultiColor(ImVec2(0, CP(0, 930).y), g_disp, MmC(P.shade, 0), MmC(P.shade, 0), MmC(P.shade, 0.85f), MmC(P.shade, 0.85f));
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(CP(900, 0).x, g_disp.y), MmC(P.shade, 0.40f), MmC(P.shade, 0), MmC(P.shade, 0), MmC(P.shade, 0.40f));
    dl->PopClipRect();

    // ---- logo ----
    {
        float in = ease(0.0f, 0.6f);
        // build 94: the mod's own logo (rollback_logo.png next to the mod); the game's logo if it is missing
        ImVec2 ls;
        void* logo = art::GetImage(MmLogoPath(), &ls);
        if (logo && ls.x > 0) {
            // build 104: centred over the menu items (x 110-670) and a bit wider than them, and
            // centred between the top of the screen and the first item (y 494); never taller than 454
            float w = 717, h = w * ls.y / ls.x, sc = 0.94f + 0.06f * in;
            if (h > 454) { w *= 454 / h; h = 454; }
            float cx = 390, cy = 247;
            dl->AddImage((ImTextureID)logo, CP(cx - w * sc / 2, cy - h * sc / 2), CP(cx + w * sc / 2, cy + h * sc / 2), ImVec2(0, 0), ImVec2(1, 1), MmRGBA(255, 255, 255, in));
        } else {
            MmImg(dl, "MA10_CS_TITLE", 2, 40, 40, 760, 279, MmRGBA(255, 255, 255, 1.0f * in), ImVec2(0, 0), ImVec2(1, 1), true);
        }
    }

    // ---- the menu ----
    const float MX = 110, MY = 494, STEP = 90;
    ImVec2 ia[5], ib[5];
    for (int i = 0; i < 5; i++) { ia[i] = CP(MX - 60, MY + i * STEP - 16); ib[i] = CP(MX + 620, MY + i * STEP + 72); }
    MmInput(ia, ib);
    for (int i = 1; i < 5; i++) MmLine(dl, MX, MY + i * STEP - 21, 560, 2, MmC(P.acc, 0.55f));
    for (int i = 0; i < 5; i++) {
        float in = ease(0.2f + 0.07f * i, 0.45f);
        float h = PgAnim(ImGui::GetID(MM_ITEMS[i]), MM.sel == i, 14.0f);
        float y = MY + i * STEP, x = MX + (1 - in) * -40 + 10 * h;
        if (h > 0.01f) {
            MmImg(dl, "MA10_CMN", 83, MX - 60, y - 16, 680 * (0.6f + 0.4f * h), 88, MmGlowCol(P.light ? P.gold : P.glow, (P.light ? 0.45f : 0.50f + 0.12f * pulse) * h), ImVec2(0, 0), ImVec2(1, 1), true);
            float bob = sinf((float)(now * anim) * 5.0f) * 4.0f;
            float px = MX - 50 + bob, py = y + 28;   // pointer: a small arrow head
            dl->AddTriangleFilled(CP(px, py - 15), CP(px + 24, py), CP(px, py + 15), MmC(P.selOut, h));
            dl->AddTriangleFilled(CP(px + 4, py - 9), CP(px + 17, py), CP(px + 4, py + 9), MmC(P.selFill, h));
        }
        float a = in;
        bool on = MM.sel == i;
        ImU32 fill = on ? MmC(P.selFill, a) : MmC(P.text, a);
        ImU32 oc = on ? MmC(P.selOut, a) : MmC(P.ink, a);
        ImU32 iconTint = on ? MmC(P.selFill, a) : MmC(P.text, a);
        if (i < 2) MmImg(dl, "MA10_CS_NETWORK", 62, x, y, 52, 56, iconTint, ImVec2(0, 0), ImVec2(52.0f / 509.0f, 1));
        else if (i == 2) MmImg(dl, "MA10_CS_OPTION", 22, x, y + 1, 44, 54, iconTint, ImVec2(0, 0), ImVec2(44.0f / 520.0f, 1));
        else MmRoundIcon(dl, x, y, i == 3 ? '?' : 'p', iconTint, on ? MmC(P.selOut, a) : MmC(P.light ? P.ink : P.bgBot, a));
        float tx = x + (i == 2 ? 52 : 60);
        MmText(dl, g_fMenu, 31, tx, y + 28, oc, MM_ITEMS[i], 0, 4, oc);
        MmText(dl, g_fMenu, 31, tx, y + 28, fill, MM_ITEMS[i], 0, 0, 0, 1);
    }

    // ---- description line + button prompts ----
    dl->PushClipRect(ImVec2(0, 0), g_disp, false);
    MmLine(dl, -g_cvO.x / g_cv, 948, g_disp.x / g_cv, 3, MmC(P.acc, 0.95f));
    MmLine(dl, -g_cvO.x / g_cv, 952, g_disp.x / g_cv, 1, MmC(P.gold, 0.6f));
    dl->PopClipRect();
    MmText(dl, g_fUi, 30, 112, 986, MmC(P.light ? P.ink : P.bgBot, 0.6f), MM_HELP[MM.sel]);
    MmText(dl, g_fUi, 30, 110, 984, MmC(P.text), MM_HELP[MM.sel]);
    MmPadButton(dl, 110, 1022, 'A', MmRGBA(40, 160, 70));
    MmText(dl, g_fMenu, 15, 156, 1040, MmC(P.text), "Select");
    if (MM.note[0] && now < MM.noteUntil) {
        float a = ImClamp((float)(MM.noteUntil - now) * 2.0f, 0.0f, 1.0f);
        MmText(dl, g_fUi, 26, 800, 986, MmC(P.gold, a), MM.note);
    }

    // ---- build 100: invite card (a Steam invite to a room), or the room you are in ----
    if (g_rv.haveInvite || g_rv.state == net::ROOM_IN) {
        float in = ease(0.6f, 0.5f);
        float ox = (1 - in) * 60;
        float x0 = 1380 + ox, y0 = 58, x1 = 1870 + ox, y1 = 193;
        bool inv = g_rv.haveInvite;
        MmFieldBox(dl, x0, y0, x1, y1, inv ? "INVITE" : "YOUR ROOM", MmC(P.panel, 0.88f * in));
        char a[96], b[64];
        if (inv) { snprintf(a, sizeof a, "%s invited you to", g_rv.invite.name); snprintf(b, sizeof b, "THEIR ROOM"); }
        else { snprintf(a, sizeof a, "%d of %d players", g_rv.nMembers, g_rv.set.size); snprintf(b, sizeof b, "%s", g_rv.set.name); }
        // build 114: text and button centred in the card (friend test: the old layout looked off)
        float cy = (y0 + y1) / 2 + 4, bx = x1 - (inv ? 128 : 186);
        MmText(dl, g_fUi, 24, x0 + 30, cy - 18, MmC(P.dim, in), a);
        MmText(dl, g_fMenu, 19, x0 + 30, cy + 18, MmC(P.text, in), b, 0, 1, MmC(P.ink, in));
        MmPadButton(dl, bx, cy - 18, 'X', MmRGBA(40, 100, 200, in));
        MmText(dl, g_fMenu, 15, bx + 46, cy, MmC(P.text, in), inv ? "Join" : "Back to it");
        if (ImGui::GetIO().MouseClicked[0] && ImGui::IsMouseHoveringRect(CP(x0, y0 - 30), CP(x1, y1), false)) {
            if (inv) net::RoomJoin(g_rv.invite.lobby);
            MM_SND |= 2; MM.screen = 1; MM.roomT = -1;
        }
    }

    // ---- my setup (corner panel) ----
    {
        float in = ease(0.5f, 0.5f);
        float oy = (1 - in) * 40;
        float x0 = 1430, y0 = 672 + oy, x1 = 1870, y1 = 1000 + oy;
        dl->AddRectFilled(CP(x0 + 6, y0 + 6), CP(x1 - 6, y1 - 6), MmC(P.panel, 0.9f * in), CV(12));
        MmNine(dl, "MA10_CS_NETWORK", 45, 26, 26, x0, y0, x1, y1, MmRGBA(255, 255, 255, in));
        MmText(dl, g_fMenu, 19, x0 + 26, y0 + 34, MmC(P.text, in), "MY SETUP", 0, 1, MmC(P.ink, in));
        SuPanel(dl, x0, y0, in);   // build 102: the saved setup
        MmLine(dl, x0 + 24, y1 - 56, x1 - x0 - 48, 2, MmC(P.acc, 0.6f * in));
        MmPadButton(dl, x0 + 24, y1 - 46, 'Y', MmRGBA(210, 170, 20, in));
        MmText(dl, g_fMenu, 15, x0 + 70, y1 - 28, MmC(P.text, in), "Edit setup");
        if (MM.screen == 0 && ImGui::GetIO().MouseClicked[0] && ImGui::IsMouseHoveringRect(CP(x0, y0), CP(x1, y1), false)) { MM_SND |= 2; SuOpen(); }
    }

    // ---- bottom right: Steam + version ----
    MmImg(dl, "MA10_CS_NETWORK", 147, 1430, 1022, 34, 34);
    MmText(dl, g_fMenu, 14, 1474, 1040, g_rv.steam ? MmRGBA(150, 220, 170) : MmC(P.faint), g_rv.steam ? "STEAM ONLINE" : "STEAM NOT FOUND");
    MmText(dl, g_fMenu, 13, 1870, 1040, MmC(P.gold), MOD_VERSION_SHORT, 2);
    if (const char* nv = upd::NewVersion()) {   // build 120: the launcher installs updates
        char t[112];
        snprintf(t, sizeof t, "UPDATE AVAILABLE: V%s  -  CLOSE THE GAME AND OPEN THE LAUNCHER TO UPDATE", nv);
        for (char* c = t; *c; c++) *c = (char)toupper((unsigned char)*c);
        MmText(dl, g_fMenu, 13, 1870, 1012, MmC(P.gold), t, 2);
    }

    dl->PopClipRect();
    ImGui::End();
}

