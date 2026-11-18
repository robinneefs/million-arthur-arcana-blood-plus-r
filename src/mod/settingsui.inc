// ---------------------------------------------------------------------------
// Build 104: Settings (main menu "Settings", screen 5).
// Tabs on the left like the main menu; the tab's page on the right.
//   CONTROLS  remap the game's inputs for the controller and the keyboard (swap on a taken
//             button), vibration, and a button check pop-up showing what the game reads
//   SOUND     the game's music / sound effects / voices volume, our menu sounds and music
//   DISPLAY   the game's display options (resolution, mode, V-Sync; after a restart)
//   ONLINE    the Steam name, input delay, connection info in a match
//   LOGS      pack the logs into one zip on the Desktop, open the folder, version
// The game's settings are reached through gamecfg.inc and written to its save when you leave.
// ---------------------------------------------------------------------------
#include "gamecfg.inc"
#include "logpack.inc"

// the rows of the controls list: action, game slot, game notation
struct StAction { const char* name; int slot; const char* game; };
static const StAction ST_ACT[8] = {{"Weak Attack", gcfg::S_A, "A"},  {"Medium Attack", gcfg::S_B, "B"}, {"Strong Attack", gcfg::S_C, "C"},
                                   {"Elemental Blade", gcfg::S_D, "D"}, {"Roll", gcfg::S_AB, "AB"},     {"Throw", gcfg::S_BC, "BC"},
                                   {"Burst", gcfg::S_AD, "abcD"},       {"Enchant Boost", gcfg::S_ABC, "ABC"}};
// build 120: Button check right below the buttons, Vibration below it (up/down reaches both)
static const int ST_NACT = 8, ST_ROW_CHECK = 8, ST_ROW_VIB = 9;
static const int ST_ROWS[5] = {10, 5, 3, 2, 2};

static struct {
    bool loaded = false, dirty = false;
    int tab = 0, row = 0, col = 0;          // col: controls tab, 0 controller 1 keyboard
    int focus = 0;                          // build 104c: 0 the tabs on the left, 1 the tab's panel
    gcfg::State g;
    int delay = 2;
    // remapping
    int listen = -1, listenCol = 0;         // row being set
    double listenUntil = 0;
    bool listenArmed = false;
    uint32_t listenPrev = 0;
    bool keyPrev[512] = {};
    // button check
    bool check = false;
    double startSince = -1;
    int lastDir = 5;
    uint32_t lastLetters = 0;
    struct H { int dir; uint32_t letters; } hist[10];
    int nHist = 0;
    ImVec2 mouse{-1, -1};
} ST;
static const char* const ST_TABS[5] = {"CONTROLS", "SOUND", "DISPLAY", "ONLINE", "LOGS"};

// a small pad / key badge: A B X Y round and coloured, others (LB, RB, LT, RT, keys) as a rounded box
static float StBadge(ImDrawList* dl, float x, float cy, const char* b, float a = 1.0f) {
    const MmPal& P = MmP();
    struct { const char* n; ImU32 c; } face[4] = {{"A", MmRGBA(40, 160, 70, a)}, {"B", MmRGBA(190, 40, 40, a)}, {"X", MmRGBA(40, 100, 200, a)}, {"Y", MmRGBA(220, 180, 30, a)}};
    for (auto& f : face)
        if (!strcmp(b, f.n)) {
            dl->AddCircleFilled(CP(x + 17, cy), CV(17), f.c, 32);
            dl->AddCircle(CP(x + 17, cy), CV(17), MmRGBA(255, 255, 255, a), 32, CV(2));
            MmText(dl, g_fUiBold, 22, x + 17, cy, MmRGBA(255, 255, 255, a), b, 1, 1, IM_COL32(0, 0, 0, (int)(200 * a)));
            return 34;
        }
    float w = 22 + MmText(dl, g_fMenu, 13, -9999, -9999, 0, b) + 10;
    dl->AddRectFilled(CP(x, cy - 16), CP(x + w, cy + 16), MmC(P.bgTop, a), CV(8));
    dl->AddRect(CP(x, cy - 16), CP(x + w, cy + 16), MmC(P.text, 0.9f * a), CV(8), 0, CV(2));
    MmText(dl, g_fMenu, 13, x + w / 2, cy, MmC(P.text, a), b, 1);
    return w;
}
// "<  value  >" selector
static void StChoice(ImDrawList* dl, float x0, float x1, float cy, const char* v, bool on) {
    const MmPal& P = MmP();
    dl->AddRectFilled(CP(x0, cy - 22), CP(x1, cy + 22), MmC(P.bgTop, 0.85f), CV(6));
    dl->AddRect(CP(x0, cy - 22), CP(x1, cy + 22), MmC(on ? P.gold : P.acc, 0.8f), CV(6), 0, CV(on ? 2.0f : 1.5f));
    dl->AddTriangleFilled(CP(x0 + 26, cy - 9), CP(x0 + 26, cy + 9), CP(x0 + 14, cy), MmC(on ? P.gold : P.dim));
    dl->AddTriangleFilled(CP(x1 - 26, cy - 9), CP(x1 - 26, cy + 9), CP(x1 - 14, cy), MmC(on ? P.gold : P.dim));
    MmText(dl, g_fUiBold, 24, (x0 + x1) / 2, cy, MmC(P.text), v, 1);
}
// ON / OFF switch
static void StToggle(ImDrawList* dl, float x, float cy, bool v) {
    const MmPal& P = MmP();
    dl->AddRectFilled(CP(x, cy - 16), CP(x + 70, cy + 16), v ? MmC(P.gold, 0.95f) : MmC(P.bgTop, 0.9f), CV(16));
    dl->AddRect(CP(x, cy - 16), CP(x + 70, cy + 16), MmC(P.text, 0.8f), CV(16), 0, CV(1.5f));
    dl->AddCircleFilled(CP(v ? x + 54 : x + 16, cy), CV(12), v ? MmC(P.ink) : MmC(P.dim), 24);
    MmText(dl, g_fMenu, 13, x + 86, cy, v ? MmC(P.gold) : MmC(P.dim), v ? "ON" : "OFF");
}
// volume slider 0-100 with the game's speaker icon
static void StSlider(ImDrawList* dl, float x0, float x1, float cy, int v, bool on) {
    const MmPal& P = MmP();
    int lvl = v == 0 ? 0 : v < 34 ? 1 : v < 67 ? 2 : 3;
    float iu = (lvl % 2) * 0.5f, iv = (lvl / 2) * 0.5f;
    MmImg(dl, "MA10_CS_OPTION", 42, x0, cy - 20, 46, 40, on ? MmC(P.gold) : MmC(P.text), ImVec2(iu, iv), ImVec2(iu + 0.5f, iv + 0.5f));
    float t0 = x0 + 70, t1 = x1 - 80, f = t0 + (t1 - t0) * v / 100.0f;
    dl->AddRectFilled(CP(t0, cy - 5), CP(t1, cy + 5), MmC(P.bgTop, 0.95f), CV(5));
    dl->AddRectFilledMultiColor(CP(t0, cy - 5), CP(f, cy + 5), MmC(P.acc), MmC(P.gold), MmC(P.gold), MmC(P.acc));
    for (int k = 1; k < 10; k++) dl->AddLine(CP(t0 + (t1 - t0) * k / 10, cy + 9), CP(t0 + (t1 - t0) * k / 10, cy + 14), MmC(P.faint, 0.7f), CV(1));
    if (on) dl->AddCircleFilled(CP(f, cy), CV(20), MmGlowCol(P.glow, 0.35f), 24);
    dl->AddCircleFilled(CP(f, cy), CV(13), MmC(P.text), 24);
    dl->AddCircle(CP(f, cy), CV(13), MmC(on ? P.gold : P.acc), 24, CV(3));
    char s[8]; snprintf(s, sizeof s, "%d", v);
    MmText(dl, g_fMenu, 18, x1, cy, on ? MmC(P.gold) : MmC(P.text), s, 2);
}
// one settings row: label (left), its control drawn by the caller; the cursor row glows
static void StRow(ImDrawList* dl, float x0, float x1, float y, const char* label, bool cur, float pulse, const char* sub = nullptr) {
    const MmPal& P = MmP();
    if (cur) MmImg(dl, "MA10_CMN", 83, x0 - 10, y - 38, x1 - x0 + 20, 76, MmGlowCol(P.glow, 0.40f + 0.1f * pulse), ImVec2(0, 0), ImVec2(1, 1), true);
    MmText(dl, g_fUiBold, 27, x0 + 20, sub ? y - 10 : y, cur ? MmC(P.gold) : MmC(P.text), label);
    if (sub) MmText(dl, g_fUi, 19, x0 + 20, y + 18, MmC(P.faint), sub);
}

// The game's own input icons (MA10_CMN, white circle + black letter / arrows), tinted with our own
// colours so they never look like Xbox A/B/X/Y: A weak = cream, B medium = gold, C strong = ember, D elemental = violet.
static const int ST_BTN_SPR[4] = {156, 144, 145, 146};          // A B C D
static MmRgb StBtnRgb(int b) {
    static const MmRgb c[4] = {{246, 236, 214}, {255, 196, 70}, {244, 104, 46}, {176, 110, 255}};
    return c[b];
}
static ImU32 StBtnCol(int b, float a = 1.0f) { return MmC(StBtnRgb(b), a); }
static void StGameBtn(ImDrawList* dl, float cx, float cy, float d, int b, bool lit = true, bool glow = false, float pulse = 0) {
    const MmPal& P = MmP();
    if (glow) MmImg(dl, "MA10_CMN", 39, cx - d * 1.0f, cy - d * 1.0f, d * 2.0f, d * 2.0f, MmGlowCol(StBtnRgb(b), 0.55f + 0.25f * pulse), ImVec2(0, 0), ImVec2(1, 1), true);
    float w = d * 48 / 41.0f;
    MmImg(dl, "MA10_CMN", ST_BTN_SPR[b], cx - w / 2, cy - d / 2, w, d, lit ? StBtnCol(b) : MmC(P.faint, 0.55f));
}
// direction arrows (numpad order 1..9, 5 = none)
static const int ST_DIR_SPR[10] = {0, 152, 153, 154, 158, 0, 151, 148, 147, 157};
static void StDir(ImDrawList* dl, float cx, float cy, float d, int n, ImU32 col) {
    if (n == 5) { dl->AddCircleFilled(CP(cx, cy), CV(d * 0.16f), col, 16); return; }
    float w = d * 48 / 41.0f;
    MmImg(dl, "MA10_CMN", ST_DIR_SPR[n], cx - w / 2, cy - d / 2, w, d, col);
}
// game notation in icons: "AB" = A + B; lower case = any one of them ("abcD" = A/B/C + D)
static float StCombo(ImDrawList* dl, float x, float cy, float d, const char* bs) {
    const MmPal& P = MmP();
    float x0 = x;
    bool prevAny = false;
    for (const char* c = bs; *c; c++) {
        bool any = *c >= 'a' && *c <= 'd';
        if (c != bs) {
            const char* sep = (any && prevAny) ? "/" : "+";
            MmText(dl, g_fUiBold, d * 0.6f, x + 8, cy, MmC(P.dim), sep, 1); x += 16;
        }
        StGameBtn(dl, x + d * 0.6f, cy, d, any ? *c - 'a' : *c - 'A');
        x += d * 1.2f;
        prevAny = any;
    }
    return x - x0;
}

// ---- what the game reads right now (button check) ----
// letters bit 0..3 = A..D held; returns the numpad direction
static int StReadGame(uint32_t* letters, bool active[gcfg::NSLOT]) {
    uint32_t held = gcfg::PadHeld(), d = gcfg::PadDirs();
    static const uint32_t SLOT_LETTERS[gcfg::NSLOT] = {1, 2, 4, 8, 3, 5, 9, 6, 10, 12, 7, 0};
    uint32_t l = 0;
    for (int s = 0; s < gcfg::NSLOT; s++) {
        int b = ST.g.pad[s];
        bool on = (b >= 0 && b < gcfg::BTN_NONE && (held & gcfg::BTN_MASK[b])) || (ST.g.key[s] >= 0 && gcfg::KeyHeld(ST.g.key[s]));
        active[s] = on;
        if (on) l |= SLOT_LETTERS[s];
    }
    *letters = l;
    int x = (d & 0x8) ? 1 : (d & 0x4) ? -1 : 0, y = (d & 0x1) ? 1 : (d & 0x2) ? -1 : 0;
    return 5 + x + 3 * y;
}

static void StButtonCheck(ImDrawList* dl, float pulse) {
    const MmPal& P = MmP();
    dl->AddRectFilled(CP(0, 0), CP(1920, 1080), IM_COL32(0, 0, 0, 200));
    const float x0 = 380, y0 = 230, x1 = 1540, y1 = 870;
    dl->AddRectFilledMultiColor(CP(x0, y0), CP(x1, y1), MmC(P.bgTop), MmC(P.bgTop), MmC(P.bgBot), MmC(P.bgBot));
    MmFieldBox(dl, x0, y0, x1, y1, "BUTTON CHECK", MmC(P.panel, 0.97f));
    MmText(dl, g_fUi, 22, (x0 + x1) / 2, y0 + 50, MmC(P.dim), "Press your buttons: what the game reads lights up.", 1);
    uint32_t letters = 0;
    bool act[gcfg::NSLOT];
    int held = StReadGame(&letters, act);
    // history: a new entry when the direction changes or a button goes down
    uint32_t fresh = letters & ~ST.lastLetters;
    if (held != ST.lastDir || fresh) {
        if (held != 5 || fresh) {
            if (ST.nHist == 10) { memmove(ST.hist, ST.hist + 1, sizeof ST.hist[0] * 9); ST.nHist = 9; }
            ST.hist[ST.nHist++] = {held, fresh};
        }
        ST.lastDir = held;
    }
    ST.lastLetters = letters;
    // directions 3x3
    float gx = x0 + 190, gy = y0 + 250, cs = 76;
    MmText(dl, g_fMenu, 13, gx, y0 + 110, MmC(P.faint), "DIRECTION", 1);
    for (int n = 1; n <= 9; n++) {
        int col = (n - 1) % 3, row = 2 - (n - 1) / 3;
        float cx = gx + (col - 1) * cs, cy = gy + (row - 1) * cs;
        bool on = n == held;
        if (on) MmImg(dl, "MA10_CMN", 39, cx - 60, cy - 60, 120, 120, MmGlowCol(P.glow, 0.5f + 0.2f * pulse), ImVec2(0, 0), ImVec2(1, 1), true);
        dl->AddRect(CP(cx - 34, cy - 34), CP(cx + 34, cy + 34), MmC(on ? P.gold : P.acc, on ? 1.0f : 0.35f), CV(8), 0, CV(on ? 2.5f : 1.2f));
        StDir(dl, cx, cy, 44, n, on ? MmC(P.gold) : MmC(P.dim, 0.6f));
    }
    // A B C D
    static const char* nm[4] = {"Weak", "Medium", "Strong", "Elem. Blade"};
    float bx = x0 + 400;
    MmText(dl, g_fMenu, 13, bx + 3 * 55, y0 + 110, MmC(P.faint), "BUTTONS", 1);
    for (int b = 0; b < 4; b++) {
        float cx = bx + b * 110, cy = gy;
        bool on = (letters >> b) & 1;
        StGameBtn(dl, cx, cy, 78, b, on, on, pulse);
        MmText(dl, g_fUi, 20, cx, cy + 66, on ? StBtnCol(b) : MmC(P.faint), nm[b], 1);
    }
    // shortcuts (lit when their own button is pressed, or all their buttons are held)
    float sx = x0 + 800;
    MmText(dl, g_fMenu, 13, sx, y0 + 110, MmC(P.faint), "SHORTCUTS");
    static const char* sc[4] = {"Roll", "Throw", "Burst", "Enchant Boost"};
    static const char* scg[4] = {"AB", "BC", "abcD", "ABC"};
    bool scOn[4] = {(letters & 3) == 3, (letters & 6) == 6, (letters & 8) && (letters & 7), (letters & 7) == 7};
    for (int i = 0; i < 4; i++) {
        float y = y0 + 170 + i * 52;
        bool on = scOn[i];
        if (on) MmImg(dl, "MA10_CMN", 83, sx - 20, y - 26, 360, 52, MmGlowCol(P.glow, 0.45f + 0.15f * pulse), ImVec2(0, 0), ImVec2(1, 1), true);
        dl->AddCircleFilled(CP(sx + 8, y), CV(6), on ? MmC(P.gold) : MmC(P.faint, 0.5f), 12);
        MmText(dl, g_fUi, 22, sx + 26, y, on ? MmC(P.gold) : MmC(P.dim), sc[i]);
        StCombo(dl, sx + 185, y, 26, scg[i]);
    }
    // history
    MmLine(dl, x0 + 50, y0 + 420, x1 - x0 - 100, 1, MmC(P.acc, 0.6f));
    MmText(dl, g_fMenu, 13, x0 + 60, y0 + 455, MmC(P.faint), "LAST INPUTS");
    float hx = x0 + 70, hy = y0 + 510;
    if (!ST.nHist) MmText(dl, g_fUi, 22, hx, hy, MmC(P.faint), "Nothing yet.");
    for (int k = 0; k < ST.nHist && hx < x1 - 140; k++) {
        const auto& h = ST.hist[k];
        StDir(dl, hx + 24, hy, 40, h.dir, MmC(P.text));
        hx += 52;
        bool first = true;
        for (int b = 0; b < 4; b++) {
            if (!((h.letters >> b) & 1)) continue;
            MmText(dl, g_fUiBold, 24, hx + 2, hy, MmC(P.dim), "+", 1);
            StGameBtn(dl, hx + 36, hy, 40, b);
            hx += 56;
            first = false;
        }
        if (!first) hx += 14;
        hx += 14;
    }
    MmText(dl, g_fUi, 20, x0 + 70, y0 + 562, MmC(P.faint), "Newest on the right, the way the game reads it: a rebound button shows its new action.");
    MmText(dl, g_fMenu, 13, (x0 + x1) / 2, y1 - 30, MmC(P.dim), "HOLD  START  TO CLOSE   (OR ESC / CLICK)", 1);
}

// ---- opening / leaving ----
static void StLoad() {
    gcfg::Read(ST.g);
    MmLoadPrefs();
#ifndef UI_HARNESS
    ST.delay = g_ncfg.delay;
#endif
    ST.loaded = true;
    ST.dirty = false;
}
static void SettingsOpen() {
    StLoad();
    ST.tab = ST.row = ST.col = ST.focus = 0;
    ST.listen = -1;
    ST.check = false;
    MM.screen = 5; MM.roomT = -1; MM.quietFrame = ImGui::GetFrameCount();
}
static void StLeave() {
    if (ST.dirty) gcfg::Save();
    ST.dirty = false;
    ST.loaded = false;
    MM_SND |= 4;
    MmGoMenu();
}

// ---- remapping ----
static void StAssign(int row, int col, int v) {
    int slot = ST_ACT[row].slot;
    int* arr = col ? ST.g.key : ST.g.pad;
    int none = col ? -1 : gcfg::BTN_NONE;
    int old = arr[slot];
    if (v != none)
        for (int j = 0; j < gcfg::NSLOT; j++)
            if (j != slot && arr[j] == v) arr[j] = old;     // taken: the two swap places
    arr[slot] = v;
    if (col) gcfg::SetKeys(ST.g); else gcfg::SetPad(ST.g);
    ST.dirty = true;
}
static void StDefaults(int col) {
    for (int i = 0; i < gcfg::NSLOT; i++) {
        if (col) ST.g.key[i] = gcfg::KEY_DEFAULT[i];
        else ST.g.pad[i] = gcfg::PAD_DEFAULT[i];
    }
    if (col) gcfg::SetKeys(ST.g); else gcfg::SetPad(ST.g);
    ST.dirty = true;
}
static void StListenStart(int row, int col) {
    ST.listen = row; ST.listenCol = col;
    ST.listenUntil = ImGui::GetTime() + 4.0;
    ST.listenArmed = false;
    ST.listenPrev = gcfg::PadHeld();
    ImGuiIO& io = ImGui::GetIO();
    for (int k = 0; k < 512; k++) ST.keyPrev[k] = io.KeysDown[k];
    MM_SND |= 2;
}
// while listening: the next button / key goes to the row
static void StListenTick() {
    ImGuiIO& io = ImGui::GetIO();
    double now = ImGui::GetTime();
    if (now > ST.listenUntil || io.MouseClicked[1]) { ST.listen = -1; MM_SND |= 4; return; }
    if (ST.listenCol == 0) {
        uint32_t h = gcfg::PadHeld();
        if (!ST.listenArmed) { if (!(h & ~0xFu)) ST.listenArmed = true; ST.listenPrev = h; }
        else {
            uint32_t fresh = h & ~ST.listenPrev;
            ST.listenPrev = h;
            for (int b = 0; b < 32; b++)
                if (fresh & (1u << b)) {
                    int btn = gcfg::ButtonOfBit(1u << b);
                    if (btn < 0) continue;
                    StAssign(ST.listen, 0, btn);
                    ST.listen = -1;
                    MM_SND |= 2;
                    return;
                }
        }
        if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Escape])) { ST.listen = -1; MM_SND |= 4; }
    } else {
#ifndef UI_HARNESS
        for (int k = 8; k < 256; k++) {
            bool down = io.KeysDown[k];
            bool fresh = down && !ST.keyPrev[k];
            ST.keyPrev[k] = down;
            if (!fresh) continue;
            if (k == VK_ESCAPE) { ST.listen = -1; MM_SND |= 4; return; }
            if (k == VK_SHIFT || k == VK_CONTROL || k == VK_MENU) continue;   // wait for the left / right one
            int dik = gcfg::KeyOfVk(k);
            if (dik < 0) continue;
            StAssign(ST.listen, 1, dik);
            ST.listen = -1;
            MM_SND |= 2;
            return;
        }
        // the generic Shift / Ctrl / Alt: use the side that is down
        static const int sided[6] = {VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU};
        static bool prevSided[6] = {};
        for (int i = 0; i < 6; i++) {
            bool down = (GetAsyncKeyState(sided[i]) & 0x8000) != 0;
            bool fresh = down && !prevSided[i];
            prevSided[i] = down;
            if (fresh && now > ST.listenUntil - 3.9) {
                int dik = gcfg::KeyOfVk(sided[i]);
                if (dik >= 0) { StAssign(ST.listen, 1, dik); ST.listen = -1; MM_SND |= 2; return; }
            }
        }
#endif
    }
}

// a value change on the current row (dx = -1 / +1, or 0 = A)
static void StChange(int dx) {
    auto& g = ST.g;
    int r = ST.row;
    if (ST.tab == 0) {
        if (r < ST_NACT) { if (!dx) StListenStart(r, ST.col); }
        else if (r == ST_ROW_VIB) { gcfg::SetVibration(g, !g.vib); ST.dirty = true; MM_SND |= 2; }
        else if (r == ST_ROW_CHECK && !dx) { ST.check = true; ST.startSince = -1; ST.nHist = 0; ST.lastDir = 5; ST.lastLetters = 0; MM_SND |= 2; }
    } else if (ST.tab == 1) {
        if (r < 3) { if (dx) { gcfg::SetVolume(g, r, g.vol[r] + dx * 5); ST.dirty = true; MM_SND |= 1; } }
        else if (r == 3) { g_menuSfx = !g_menuSfx.load(); MmSavePref(L"menu_sounds", g_menuSfx); MM_SND |= 2; }
        else { g_menuMusic = !g_menuMusic.load(); MmSavePref(L"menu_music", g_menuMusic); g_musicReapply = true; MM_SND |= 2; }
    } else if (ST.tab == 2) {
        if (r == 0 && g.nRes > 0) { int d = dx ? dx : 1; g.resIdx = (g.resIdx + d + g.nRes) % g.nRes; gcfg::SetDisplay(g); }
        else if (r == 1) {   // build 110: Fullscreen -> Windowed -> Borderless
            int mode = !g.windowed ? 0 : g_borderless.load() ? 2 : 1, d = dx ? dx : 1;
            mode = (mode + d + 3) % 3;
            if ((mode != 0) != g.windowed) gcfg::SetWindowed(g, mode != 0);
            g_borderless = mode == 2;
            MmSavePref(L"borderless", mode == 2 ? 1 : 0);
        }
        else if (r == 2) { g.vsync = !g.vsync; gcfg::SetDisplay(g); }
        ST.dirty = true;
        MM_SND |= 1;
    } else if (ST.tab == 3) {
        if (r == 0) {
            int d = dx ? dx : 1;
            ST.delay = (ST.delay + d + 9) % 9;
#ifndef UI_HARNESS
            g_ncfg.delay = ST.delay;
            wchar_t ini[MAX_PATH], w[8];
            _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
            _snwprintf(w, 8, L"%d", ST.delay);
            WritePrivateProfileStringW(L"net", L"input_delay", w, ini);
            Log("settings: input delay %d", ST.delay);
#endif
            MM_SND |= 1;
        } else { g_netInfo = !g_netInfo.load(); MmSavePref(L"connection_info", g_netInfo); MM_SND |= 2; }
    } else {
        if (dx) return;
        if (r == 0) logpack::Start(); else logpack::OpenFolder();
        MM_SND |= 2;
    }
}

static void MmSettingsFrame() {
    const MmPal& P = MmP();
    if (!ST.loaded) StLoad();
    if (MM.roomT < 0) MM.roomT = ImGui::GetTime();
    MmT T = MmTimes(MM.roomT);
    ImGuiIO& io = ImGui::GetIO();
    MmNav nav = MmReadNav(false);
    bool click = io.MouseClicked[0];
    bool moved = io.MousePos.x != ST.mouse.x || io.MousePos.y != ST.mouse.y;
    ST.mouse = io.MousePos;
    if (ImGui::GetFrameCount() == MM.quietFrame) { nav = MmNav(); click = false; }
    // ---- input ----
    if (ST.listen >= 0) { StListenTick(); nav = MmNav(); click = false; }
    else if (ST.check) {
        bool start = (gcfg::PadHeld() & 0x10) != 0;   // START
        double now = ImGui::GetTime();
        if (start) { if (ST.startSince < 0) ST.startSince = now; }
        else ST.startSince = -1;
        if ((ST.startSince >= 0 && now - ST.startSince > 0.8) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Escape]) || click) {
            ST.check = false; MM_SND |= 4;
        }
        nav = MmNav(); click = false;
    } else {
        if (ST.focus == 0) {
            // build 104c: the tabs first; A (or right) goes into the tab's panel, B back to the main menu
            if (nav.b) { StLeave(); return; }
            if (nav.dy) { ST.tab = (ST.tab + nav.dy + 5) % 5; ST.row = 0; ST.col = 0; MM_SND |= 1; }
            if (nav.a || nav.dx > 0) { ST.focus = 1; ST.row = 0; ST.col = 0; MM_SND |= 2; }
            nav = MmNav();
        } else if (nav.b) { ST.focus = 0; MM_SND |= 4; nav = MmNav(); }
        if (nav.dy) { ST.row = (ST.row + nav.dy + ST_ROWS[ST.tab]) % ST_ROWS[ST.tab]; MM_SND |= 1; }
        if (nav.dx) {
            if (ST.tab == 0 && ST.row < ST_NACT) { int c = ST.col + nav.dx; if (c >= 0 && c <= 1) { ST.col = c; MM_SND |= 1; } }
            else StChange(nav.dx);
        }
        if (nav.a) StChange(0);
        if (ST.tab == 0 && ST.row < ST_NACT) {
            if (nav.x) { StAssign(ST.row, ST.col, ST.col ? -1 : gcfg::BTN_NONE); MM_SND |= 4; }
            if (nav.y) { StDefaults(ST.col); MM_SND |= 2; }
        }
    }

    ImDrawList* dl = MmPageBegin("##settings", T.tm, T.pulse);
    MmPageTitle(dl, "MAIN MENU", "SETTINGS", MmEase(T, 0.0f, 0.5f));
    MmText(dl, g_fUi, 22, 1860, 74, MmC(P.dim), "Changes are kept when you leave this screen.", 2);

    // ---- tabs ----
    for (int i = 0; i < 5; i++) {
        float y = 230 + i * 92;
        if (click && MmHover(40, y - 40, 500, y + 40)) { if (ST.tab != i) { ST.tab = i; ST.row = 0; ST.col = 0; MM_SND |= 1; } ST.focus = 0; click = false; }
        bool on = ST.tab == i;
        if (on) {
            float ga = ST.focus ? 0.22f : 0.5f + 0.12f * T.pulse;   // dimmer while the panel has the controls
            MmImg(dl, "MA10_CMN", 83, 40, y - 40, 470, 80, MmGlowCol(P.glow, ga), ImVec2(0, 0), ImVec2(1, 1), true);
            float px = 54, py = y;
            dl->AddTriangleFilled(CP(px, py - 13), CP(px + 21, py), CP(px, py + 13), MmC(P.selOut));
            dl->AddTriangleFilled(CP(px + 4, py - 8), CP(px + 15, py), CP(px + 4, py + 8), MmC(P.selFill));
        }
        if (i) MmLine(dl, 90, y - 46, 380, 2, MmC(P.acc, 0.5f));
        float x = on ? 100 : 90;
        MmText(dl, g_fMenu, 26, x, y, on ? MmC(P.selOut) : MmC(P.ink), ST_TABS[i], 0, 3, on ? MmC(P.selOut) : MmC(P.ink));
        MmText(dl, g_fMenu, 26, x, y, on ? MmC(P.selFill) : MmC(P.text), ST_TABS[i], 0, 0, 0, 1);
    }

    // mouse over a row picks it; a click acts on it (hit areas given per tab below)
    auto rowHit = [&](int row, float x0, float y0, float x1, float y1, int col = -1) -> bool {
        if (!MmHover(x0, y0, x1, y1)) return false;
        if (click || (moved && ST.focus)) { if (ST.row != row && ST.focus) MM_SND |= 1; ST.row = row; if (col >= 0) ST.col = col; }
        if (click) ST.focus = 1;
        return click;
    };

    const float X0 = 560, X1 = 1860, Y0 = 170, Y1 = 940;
    const int SR = ST.focus ? ST.row : -1;   // the highlighted row (none while the tabs have the controls)
    const auto& g = ST.g;
    if (ST.tab == 0) {
        MmSection(dl, X0, Y0, X1, Y1, "BUTTONS");
        float lx0 = X0 + 30, lx1 = X1 - 30;
        const float cC = X0 + 900, cK = X0 + 1140;        // column centres: controller, keyboard
        MmText(dl, g_fMenu, 13, X0 + 160, Y0 + 46, MmC(P.faint), "GAME INPUT", 1);
        MmText(dl, g_fMenu, 13, cC, Y0 + 46, MmC(P.faint), "CONTROLLER", 1);
        MmText(dl, g_fMenu, 13, cK, Y0 + 46, MmC(P.faint), "KEYBOARD", 1);
        for (int i = 0; i < ST_NACT; i++) {
            float y = Y0 + 100 + i * 58;
            for (int c = 0; c < 2; c++) {
                float cx = c ? cK : cC;
                if (rowHit(i, cx - 120, y - 30, cx + 120, y + 30, c)) StChange(0);
            }
            if (rowHit(i, lx0, y - 30, cC - 130, y + 30)) StChange(0);
            bool cur = SR == i;
            if (cur) MmImg(dl, "MA10_CMN", 83, lx0 - 10, y - 34, lx1 - lx0 + 20, 68, MmGlowCol(P.glow, 0.40f + 0.1f * T.pulse), ImVec2(0, 0), ImVec2(1, 1), true);
            const char* gn = ST_ACT[i].game;
            float w = StCombo(dl, -9999, -9999, 34, gn);
            StCombo(dl, X0 + 160 - w / 2, y, 34, gn);
            MmText(dl, g_fUiBold, 27, X0 + 330, y, cur ? MmC(P.gold) : MmC(P.text), ST_ACT[i].name);
            int slot = ST_ACT[i].slot;
            for (int c = 0; c < 2; c++) {
                float cx = c ? cK : cC;
                bool here = cur && ST.col == c;
                if (ST.listen == i && ST.listenCol == c) {
                    float a = 0.6f + 0.4f * T.pulse;
                    dl->AddRectFilled(CP(cx - 115, y - 22), CP(cx + 115, y + 22), MmC(P.gold, 0.18f), CV(6));
                    dl->AddRect(CP(cx - 115, y - 22), CP(cx + 115, y + 22), MmC(P.gold, a), CV(6), 0, CV(2));
                    char t[48];
                    int left = (int)(ST.listenUntil - ImGui::GetTime()) + 1;
                    snprintf(t, sizeof t, c ? "Press a key...  %d" : "Press a button...  %d", left < 1 ? 1 : left);
                    MmText(dl, g_fUi, 21, cx, y, MmC(P.gold), t, 1);
                    continue;
                }
                if (here) dl->AddRect(CP(cx - 100, y - 24), CP(cx + 100, y + 24), MmC(P.gold, 0.6f + 0.4f * T.pulse), CV(6), 0, CV(2));
                char b[32];
                if (c) gcfg::KeyName(g.key[slot], b, sizeof b);
                else snprintf(b, sizeof b, "%s", g.pad[slot] >= 0 && g.pad[slot] < gcfg::BTN_NONE ? gcfg::BTN_NAME[g.pad[slot]] : "-");
                if (!strcmp(b, "-")) { MmText(dl, g_fUi, 22, cx, y, MmC(P.faint), "not set", 1); continue; }
                float bw = StBadge(dl, -9999, -9999, b, 0.0f);
                StBadge(dl, cx - bw / 2, y, b);
            }
        }
        MmLine(dl, lx0 + 20, Y0 + 552, lx1 - lx0 - 40, 1, MmC(P.acc, 0.6f));
        float y = Y0 + 600;
        bool bc = SR == ST_ROW_CHECK;
        float b0 = lx0 + 20, b1 = lx0 + 460;
        if (rowHit(ST_ROW_CHECK, b0, y - 26, b1, y + 26)) StChange(0);
        if (bc) MmImg(dl, "MA10_CMN", 83, b0 - 20, y - 40, b1 - b0 + 40, 80, MmGlowCol(P.glow, 0.45f + 0.1f * T.pulse), ImVec2(0, 0), ImVec2(1, 1), true);
        dl->AddRectFilled(CP(b0, y - 26), CP(b1, y + 26), bc ? MmC(P.gold, 0.95f) : MmC(P.bgTop, 0.9f), CV(6));
        dl->AddRect(CP(b0, y - 26), CP(b1, y + 26), MmC(bc ? P.selOut : P.text, 0.9f), CV(6), 0, CV(2));
        MmText(dl, g_fMenu, 18, (b0 + b1) / 2, y, bc ? MmC(P.ink) : MmC(P.text), "BUTTON CHECK", 1);
        MmText(dl, g_fUi, 21, b1 + 40, y, MmC(P.faint), "Press your buttons and see what the game reads");
        y = Y0 + 672;
        if (rowHit(ST_ROW_VIB, lx0, y - 34, lx1, y + 34)) StChange(0);
        StRow(dl, lx0, lx1, y, "Vibration", SR == ST_ROW_VIB, T.pulse);
        StToggle(dl, X0 + 400, y, g.vib);
        MmText(dl, g_fUi, 20, lx0 + 20, Y0 + 730, MmC(P.faint), "Start / Pause is always START (controller). Pressing a shortcut's buttons together also works.");
    } else if (ST.tab == 1) {
        MmSection(dl, X0, Y0, X1, Y1, "SOUND");
        const char* lab[3] = {"Music", "Sound effects", "Voices"};
        const char* sub[3] = {"Menu music and the music in fights", "Hits, menu sounds, announcer", "The characters' voices"};
        for (int i = 0; i < 3; i++) {
            float y = Y0 + 110 + i * 110;
            float t0 = X0 + 590, t1 = X1 - 160;
            if (rowHit(i, X0 + 30, y - 45, X1 - 30, y + 45) && io.MousePos.x >= CP(t0 - 20, 0).x) {
                float f = (io.MousePos.x - CP(t0, 0).x) / (CP(t1, 0).x - CP(t0, 0).x);
                int v = (int)(ImClamp(f, 0.0f, 1.0f) * 20 + 0.5f) * 5;
                gcfg::SetVolume(ST.g, i, v); ST.dirty = true; MM_SND |= 1;
            }
            StRow(dl, X0 + 30, X1 - 30, y, lab[i], SR == i, T.pulse, sub[i]);
            StSlider(dl, X0 + 520, X1 - 80, y, g.vol[i], SR == i);
        }
        MmLine(dl, X0 + 50, Y0 + 400, X1 - X0 - 100, 1, MmC(P.acc, 0.6f));
        if (rowHit(3, X0 + 30, Y0 + 420, X1 - 30, Y0 + 500)) StChange(0);
        StRow(dl, X0 + 30, X1 - 30, Y0 + 460, "Menu sounds", SR == 3, T.pulse, "Move / select / back sounds in our menus");
        StToggle(dl, X0 + 520, Y0 + 460, g_menuSfx);
        if (rowHit(4, X0 + 30, Y0 + 520, X1 - 30, Y0 + 600)) StChange(0);
        StRow(dl, X0 + 30, X1 - 30, Y0 + 560, "Menu music", SR == 4, T.pulse, "Music on the main menu and in rooms");
        StToggle(dl, X0 + 520, Y0 + 560, g_menuMusic);
    } else if (ST.tab == 2) {
        MmSection(dl, X0, Y0, X1, Y1, "DISPLAY");
        char res[32] = "-";
        if (g.nRes > 0) snprintf(res, sizeof res, "%d x %d", g.res[g.resIdx].w, g.res[g.resIdx].h);
        float ys[3] = {Y0 + 110, Y0 + 210, Y0 + 310};
        for (int r = 0; r < 3; r++) {
            if (!rowHit(r, X0 + 30, ys[r] - 45, X1 - 30, ys[r] + 45)) continue;
            float mx = io.MousePos.x;
            StChange(r < 2 && mx < CP(X0 + 760, 0).x ? -1 : 1);   // (row 1: three modes)
        }
        StRow(dl, X0 + 30, X1 - 30, ys[0], "Resolution", SR == 0, T.pulse);
        StChoice(dl, X0 + 520, X0 + 1000, ys[0], res, SR == 0);
        StRow(dl, X0 + 30, X1 - 30, ys[1], "Display mode", SR == 1, T.pulse);
        StChoice(dl, X0 + 520, X0 + 1000, ys[1], !g.windowed ? "Fullscreen" : g_borderless.load() ? "Borderless" : "Windowed", SR == 1);
        StRow(dl, X0 + 30, X1 - 30, ys[2], "V-Sync", SR == 2, T.pulse);
        StToggle(dl, X0 + 520, ys[2], g.vsync);
        MmLine(dl, X0 + 50, Y0 + 390, X1 - X0 - 100, 1, MmC(P.acc, 0.6f));
        MmText(dl, g_fUi, 24, X0 + 50, Y0 + 440, MmC(P.gold), "Resolution and display mode change right away. V-Sync changes the next time you start the game.");
        MmText(dl, g_fUi, 21, X0 + 50, Y0 + 476, MmC(P.faint), "They are the game's own display settings: the game's options menu shows the same.");
    } else if (ST.tab == 3) {
        MmSection(dl, X0, Y0, X1, Y1, "ONLINE");
        StRow(dl, X0 + 30, X1 - 30, Y0 + 110, "Your name", false, T.pulse, "Always your Steam name");
        const char* sn = net::SteamMyName();
        float nw = MmText(dl, g_fUiBold, 28, X0 + 520, Y0 + 110, MmC(sn[0] ? P.text : P.faint), sn[0] ? sn : "Steam isn't running");
        MmText(dl, g_fMenu, 13, X0 + 540 + nw, Y0 + 112, MmC(P.faint), "FROM STEAM");
        if (rowHit(0, X0 + 30, Y0 + 175, X1 - 30, Y0 + 265)) StChange(io.MousePos.x < CP(X0 + 670, 0).x ? -1 : 1);
        StRow(dl, X0 + 30, X1 - 30, Y0 + 220, "Input delay", SR == 0, T.pulse, "Higher = fewer rollbacks, less snappy");
        char d[24];
        snprintf(d, sizeof d, ST.delay == 1 ? "%d frame" : "%d frames", ST.delay);
        StChoice(dl, X0 + 520, X0 + 820, Y0 + 220, d, SR == 0);
        MmText(dl, g_fUi, 21, X0 + 850, Y0 + 206, MmC(P.faint), "2 suits most connections.");
        MmText(dl, g_fUi, 21, X0 + 850, Y0 + 234, MmC(P.faint), "Each player picks their own.");
        if (rowHit(1, X0 + 30, Y0 + 285, X1 - 30, Y0 + 375)) StChange(0);
        StRow(dl, X0 + 30, X1 - 30, Y0 + 330, "Connection info in a match", SR == 1, T.pulse, "Ping and rollback frames in a corner");
        StToggle(dl, X0 + 520, Y0 + 330, g_netInfo);
    } else {
        MmSection(dl, X0, Y0, X1, Y1, "LOGS");
        MmText(dl, g_fUi, 26, X0 + 50, Y0 + 90, MmC(P.text), "Something went wrong? Pack the logs into one file and send it along.");
        MmText(dl, g_fUi, 22, X0 + 50, Y0 + 126, MmC(P.dim), "Steam IDs and names in it are hidden. Nothing is sent by itself.");
        if (rowHit(0, X0 + 50, Y0 + 180, X0 + 380, Y0 + 245)) StChange(0);
        if (rowHit(1, X0 + 410, Y0 + 180, X0 + 740, Y0 + 245)) StChange(0);
        MmButton(dl, X0 + 50, Y0 + 180, X0 + 380, Y0 + 245, "PACK LOGS", SR == 0, T.pulse);
        MmButton(dl, X0 + 410, Y0 + 180, X0 + 740, Y0 + 245, "OPEN FOLDER", SR == 1, T.pulse);
        int ls = logpack::g_state;
        if (ls) MmText(dl, g_fUi, 22, X0 + 50, Y0 + 290, ls == 3 ? MmC(P.acc) : ls == 2 ? MmC(P.gold) : MmC(P.dim), logpack::g_msg);
        MmLine(dl, X0 + 50, Y0 + 350, X1 - X0 - 100, 1, MmC(P.acc, 0.6f));
        MmText(dl, g_fMenu, 13, X0 + 50, Y0 + 395, MmC(P.faint), "VERSION");
        char v[64];
#ifndef UI_HARNESS
        snprintf(v, sizeof v, "Arcana Blood +R  %s  (build %u)", MOD_VERSION_SHORT, (unsigned)NET_BUILD);
#else
        snprintf(v, sizeof v, "Arcana Blood +R  %s  (build %d)", MOD_VERSION_SHORT, MOD_BUILD);
#endif
        MmText(dl, g_fUiBold, 26, X0 + 300, Y0 + 395, MmC(P.text), v);
        MmText(dl, g_fMenu, 13, X0 + 50, Y0 + 445, MmC(P.faint), "GAME");
        MmText(dl, g_fUi, 24, X0 + 300, Y0 + 445, MmC(P.dim), "Million Arthur: Arcana Blood (Steam)");
    }

    // ---- description line + prompts ----
    dl->PushClipRect(ImVec2(0, 0), g_disp, false);
    MmLine(dl, -g_cvO.x / g_cv, 960, g_disp.x / g_cv, 3, MmC(P.acc, 0.95f));
    MmLine(dl, -g_cvO.x / g_cv, 964, g_disp.x / g_cv, 1, MmC(P.gold, 0.6f));
    dl->PopClipRect();
    static const char* help[5] = {"Pick an action and press A, then the button or key you want. One that's taken swaps places.",
                                  "Left / right changes the volume.", "Left / right changes the setting. It is used the next time the game starts.",
                                  "Your own input delay. The other player picks theirs.", "Pack the logs when you report a problem."};
    const char* h = ST.focus ? help[ST.tab] : "Pick a tab and press A.";
    if (ST.listen >= 0) h = ST.listenCol ? "Press the key you want. Esc keeps the old one." : "Press the button you want. Wait 4 seconds to keep the old one.";
    MmText(dl, g_fUi, 26, 60, 994, MmC(P.text), h);
    float x = 60;
    bool remapRow = ST.focus && ST.tab == 0 && ST.row < ST_NACT;
    MmPadButton(dl, x, 1022, 'A', MmRGBA(40, 160, 70)); MmText(dl, g_fMenu, 13, x + 44, 1040, MmC(P.text), remapRow ? "Change" : "Select"); x += 170;
    if (remapRow) {
        MmPadButton(dl, x, 1022, 'X', MmRGBA(40, 100, 200)); MmText(dl, g_fMenu, 13, x + 44, 1040, MmC(P.text), "Clear"); x += 150;
        MmPadButton(dl, x, 1022, 'Y', MmRGBA(220, 180, 30)); MmText(dl, g_fMenu, 13, x + 44, 1040, MmC(P.text), "Defaults"); x += 180;
    }
    MmPadButton(dl, x, 1022, 'B', MmRGBA(190, 40, 40)); MmText(dl, g_fMenu, 13, x + 44, 1040, MmC(P.text), ST.focus ? "Tabs" : "Back"); x += 140;
    if (ST.check) StButtonCheck(dl, T.pulse);
    MmPageEnd(dl);
}
