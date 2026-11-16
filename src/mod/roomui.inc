// ===========================================================================
// Build 99: the room (lobby) screen in the main menu's style (Crimson,
// Michroma titles, circles + embers background).
// Build 100: real Steam rooms (net::Room* in transport.cpp):
//   screen 1  the room: players, host settings, chat, invite friends, leave
//   screen 2  room search: public rooms of this build, join
//   screen 3  create room: name, public / private, size, rounds, timer, set length
// Tables and seats are drawn but not used yet (next step). The overlay only
// asks for things; the game thread does the Steam calls (RoomTick).
// Game pictures used: MA10_CSEL 35-48 (portraits), MA10_CS_NETWORK 149 (VS),
// 10 (stage), 45 (gold frame), MA10_CMN 39 (soft light), 83 (glow bar).
// ===========================================================================
static const char* const MM_TIMER_NAMES[4] = {"30", "60", "99", "NO LIMIT"};

// ---- input for the room screens: keyboard, controllers (with repeat), not while typing ----
struct MmNav { int dy = 0, dx = 0; bool a = false, b = false, x = false, y = false, lb = false, rb = false; };   // build 102: + LB / RB (Page Up / Page Down)
static MmNav MmReadNav(bool typing) {
    MmNav n;
    ImGuiIO& io = ImGui::GetIO();
    double now = ImGui::GetTime();
#ifndef UI_HARNESS
    if (g_uiBlock) { MM.prevNav = U.capture.load() ? U.navBits : 0; return n; }   // build 108: the pop-up has the input
#endif
    if (!typing) {
        if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_UpArrow])) n.dy = -1;
        if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_DownArrow])) n.dy = 1;
        if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_LeftArrow])) n.dx = -1;
        if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_RightArrow])) n.dx = 1;
        if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Enter]) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Space])) n.a = true;
        if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Escape]) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Backspace])) n.b = true;
        if (ImGui::IsKeyPressed('X')) n.x = true;
        if (ImGui::IsKeyPressed('Y')) n.y = true;
        if (ImGui::IsKeyPressed(0x21)) n.lb = true;   // Page Up
        if (ImGui::IsKeyPressed(0x22)) n.rb = true;   // Page Down
    }
#ifndef UI_HARNESS
    uint32_t nb = U.capture.load() ? U.navBits : 0, fresh = nb & ~MM.prevNav;
    if (typing) {   // build 101: in a text field the controller only leaves it again (B or A)
        if (fresh & (PB_A | PB_B)) { ImGui::ClearActiveID(); MM_SND |= 4; }
        MM.prevNav = nb;
        return n;
    }
    const uint32_t dirs = PB_UP | PB_DOWN | PB_LEFT | PB_RIGHT;
    if (fresh & dirs) MM.repeatAt = now + 0.35;
    else if ((nb & dirs) && now >= MM.repeatAt) { fresh |= nb & dirs; MM.repeatAt = now + 0.12; }
    if (fresh & PB_UP) n.dy = -1;
    if (fresh & PB_DOWN) n.dy = 1;
    if (fresh & PB_LEFT) n.dx = -1;
    if (fresh & PB_RIGHT) n.dx = 1;
    if (fresh & PB_A) n.a = true;
    if (fresh & PB_B) n.b = true;
    if (fresh & PB_X) n.x = true;
    if (fresh & PB_Y) n.y = true;
    if (fresh & PB_LB) n.lb = true;
    if (fresh & PB_RB) n.rb = true;
    MM.prevNav = nb;
#else
    (void)now;
#endif
    return n;
}
static bool MmHover(float x0, float y0, float x1, float y1) {
#ifndef UI_HARNESS
    if (g_uiBlock) return false;
#endif
    ImVec2 m = ImGui::GetIO().MousePos, a = CP(x0, y0), b = CP(x1, y1);
    return m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
}
static bool MmClicked(float x0, float y0, float x1, float y1) { return ImGui::GetIO().MouseClicked[0] && MmHover(x0, y0, x1, y1); }

// ---- the page every room screen sits on ----
static ImDrawList* MmPageBegin(const char* id, float tm, float pulse) {
    g_mmDrew = true;
    MmBackground(ImGui::GetBackgroundDrawList(), tm, pulse);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(g_disp);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), g_disp, MmC(MmP().bgBot, 0.45f));   // a veil so busy screens read well
    dl->PushClipRect(CP(0, 0), CP(1920, 1080), true);
    return dl;
}
static void MmPageEnd(ImDrawList* dl) {
    dl->PopClipRect();
    ImGui::End();
}
// the title of a page and the red + gold line under it
static void MmPageTitle(ImDrawList* dl, const char* small, const char* title, float in) {
    const MmPal& P = MmP();
    if (small) MmText(dl, g_fMenu, 13, 60, 38, MmC(P.faint, in), small);
    MmText(dl, g_fMenu, 30, 60, 74, MmC(P.text, in), title, 0, 2, MmC(P.ink, in));
    dl->PushClipRect(ImVec2(0, 0), g_disp, false);
    MmLine(dl, -g_cvO.x / g_cv, 118, g_disp.x / g_cv, 3, MmC(P.acc, 0.95f * in));
    MmLine(dl, -g_cvO.x / g_cv, 122, g_disp.x / g_cv, 1, MmC(P.gold, 0.6f * in));
    dl->PopClipRect();
}
static void MmNoteDraw(ImDrawList* dl, double now, float x, float y) {
    if (MM.note[0] && now < MM.noteUntil) {
        float a = ImClamp((float)(MM.noteUntil - now) * 2.0f, 0.0f, 1.0f);
        MmText(dl, g_fUi, 24, x, y, MmC(MmP().gold, a), MM.note);
    }
}
// a text field drawn in our style (ImGui does the typing); returns true when Enter was pressed
static bool MmTextField(const char* id, char* buf, int cap, float x0, float y0, float x1, float y1, const char* hint, bool focus) {
    const MmPal& P = MmP();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(CP(x0, y0), CP(x1, y1), MmC(P.bgTop, 0.9f), CV(6));
    bool active = ImGui::GetActiveID() == ImGui::GetID(id);
    dl->AddRect(CP(x0, y0), CP(x1, y1), MmC(active ? P.gold : P.acc, 0.85f), CV(6), 0, CV(active ? 2.0f : 1.5f));
    float px = 23 * MmEm(g_fUi);          // drawn line height in canvas pixels
    ImGui::PushFont(g_fUi);
    float scale = CV(px) / g_fUi->FontSize;
    ImGui::SetWindowFontScale(scale);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, MmC(P.text));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, MmC(P.faint));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(CV(8), CV(2)));
    float fh = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(CP(x0, 0).x + CV(6), (CP(0, y0).y + CP(0, y1).y) * 0.5f - fh * 0.5f));
    ImGui::PushItemWidth(CV(x1 - x0 - 12));
    if (focus) ImGui::SetKeyboardFocusHere(0);
    bool enter = ImGui::InputTextWithHint(id, hint, buf, cap, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    return enter;
}
// wraps `s` into lines no wider than `w` canvas pixels; returns how many
static int MmWrap(ImFont* f, float px, const char* s, float w, char out[][200], int maxLines) {
    float sz = CV(px * MmEm(f));
    int n = 0;
    const char* p = s;
    while (*p && n < maxLines) {
        const char* end = f->CalcWordWrapPositionA(sz / f->FontSize, p, p + strlen(p), CV(w));
        if (end == p) end = p + 1;
        int len = (int)(end - p);
        if (len > 199) len = 199;
        memcpy(out[n], p, len);
        out[n][len] = 0;
        n++;
        p = end;
        while (*p == ' ') p++;
    }
    return n;
}

// a rounded label ("READY", "PUBLIC" ...): filled, or outlined when !fill
static void MmPill(ImDrawList* dl, float cx, float cy, float w, float h, const char* s, MmRgb col, bool filled, float a = 1.0f) {
    const MmPal& P = MmP();
    ImVec2 p0 = CP(cx - w / 2, cy - h / 2), p1 = CP(cx + w / 2, cy + h / 2);
    if (filled) dl->AddRectFilled(p0, p1, MmC(col, a), CV(h / 2));
    else dl->AddRectFilled(p0, p1, MmC(P.panel, 0.8f * a), CV(h / 2));
    dl->AddRect(p0, p1, MmC(filled ? P.text : col, a), CV(h / 2), 0, CV(2));
    MmText(dl, g_fMenu, h * 0.42f, cx, cy, filled ? MmC(P.ink, a) : MmC(col, a), s, 1, 0, 0, filled ? 0.5f : 0);
}
// a button at the bottom: gold (main action) or dark with a cream outline
static void MmButton(ImDrawList* dl, float x0, float y0, float x1, float y1, const char* s, bool primary, float pulse) {
    const MmPal& P = MmP();
    if (primary) {
        MmImg(dl, "MA10_CMN", 39, x0 - 30, y0 - 50, x1 - x0 + 60, y1 - y0 + 100, MmGlowCol(P.glow, 0.25f + 0.15f * pulse), ImVec2(0, 0), ImVec2(1, 1), true);
        dl->AddRectFilledMultiColor(CP(x0, y0), CP(x1, y1), MmRGBA(255, 222, 130), MmRGBA(255, 222, 130), MmRGBA(220, 150, 40), MmRGBA(220, 150, 40));
        dl->AddRect(CP(x0, y0), CP(x1, y1), MmC(P.text), CV(6), 0, CV(2));
        MmText(dl, g_fMenu, 19, (x0 + x1) / 2, (y0 + y1) / 2, MmC(P.ink), s, 1, 0, 0, 0.6f);
    } else {
        dl->AddRectFilled(CP(x0, y0), CP(x1, y1), MmC(P.panel, 0.92f), CV(6));
        dl->AddRect(CP(x0, y0), CP(x1, y1), MmC(P.dim, 0.9f), CV(6), 0, CV(2));
        MmText(dl, g_fMenu, 19, (x0 + x1) / 2, (y0 + y1) / 2, MmC(P.text), s, 1, 0, 0, 0.4f);
    }
}
// a small loading ring
static void MmSpinner(ImDrawList* dl, float cx, float cy, float r, float tm, MmRgb col) {
    float a0 = tm * 5.0f;
    dl->PathArcTo(CP(cx, cy), CV(r), a0, a0 + IM_PI * 1.4f, 24);
    dl->PathStroke(MmC(col), false, CV(4));
}
// a field box with a title line above it, like the main menu's invite card
static void MmSection(ImDrawList* dl, float x0, float y0, float x1, float y1, const char* title, float a = 1.0f) {
    MmFieldBox(dl, x0, y0, x1, y1, title, MmC(MmP().panel, 0.86f * a));
}

struct MmSeat { int port; const char* name; const char* pill; };   // port < 0: free seat
static void MmSeatDraw(ImDrawList* dl, float x, float y, const MmSeat& s, int side, bool canSit) {
    const MmPal& P = MmP();
    const float pw = 130, ph = 190;
    if (s.port < 0) {
        dl->AddRectFilled(CP(x, y), CP(x + pw, y + ph), MmC(P.bgTop, 0.55f), CV(10));
        dl->AddRect(CP(x, y), CP(x + pw, y + ph), MmC(P.acc, 0.7f), CV(10), 0, CV(2));
        MmText(dl, g_fMenu, 18, x + pw / 2, y + ph / 2 - 14, MmC(P.dim, 0.8f), "FREE", 1);
        MmText(dl, g_fMenu, 18, x + pw / 2, y + ph / 2 + 14, MmC(P.dim, 0.8f), "SEAT", 1);
        if (canSit) MmText(dl, g_fUi, 23, x + pw / 2, y + ph + 26, MmC(P.gold), "Sit here", 1);
        return;
    }
    MmImg(dl, "MA10_CSEL", s.port, x, y, pw, ph);
    dl->AddRect(CP(x, y), CP(x + pw, y + ph), MmRGBA(214, 170, 82), 0, 0, CV(1.5f));
    // 1P red, 2P gold badge on the top left corner
    MmRgb bc = side == 1 ? P.acc : P.gold;
    // build 114: the 1P / 2P badge sits centred on the top edge, above the seat cursor (friend test)
    float bx = x + pw / 2 - 27, by = y - 26;
    dl->AddRectFilled(CP(bx, by), CP(bx + 54, by + 28), MmC(bc), CV(5));
    dl->AddRect(CP(bx, by), CP(bx + 54, by + 28), MmC(P.text), CV(5), 0, CV(1.5f));
    MmText(dl, g_fMenu, 13, bx + 27, by + 14, MmC(side == 1 ? P.text : P.ink), side == 1 ? "1P" : "2P", 1, 0, 0, 0.5f);
    bool you = !strcmp(s.name, "You");
    MmText(dl, g_fUiBold, 25, x + pw / 2 + 1, y + ph + 23, MmRGBA(0, 0, 0, 0.6f), s.name, 1);
    MmText(dl, g_fUiBold, 25, x + pw / 2, y + ph + 22, you ? MmC(P.gold) : MmC(P.text), s.name, 1);
    if (s.pill) {
        bool ready = !strcmp(s.pill, "READY"), play = !strcmp(s.pill, "PLAY");
        MmPill(dl, x + pw / 2, y + ph + 58, 120, 30, s.pill, ready ? P.gold : play ? P.acc : P.dim, ready || play);
    }
}


// ---- the room settings the player last used (ui_theme.ini [room]) ----
static net::RoomSettings g_rs;
static bool g_rsRead = false;
static void MmRoomSettingsRead() {
    if (g_rsRead) return;
    g_rsRead = true;
    g_rs = net::RoomSettings{};
    g_rs.size = 8; g_rs.rounds = 2; g_rs.timer = 2; g_rs.setLen = 0; g_rs.priv = false;
#ifndef UI_HARNESS
    wchar_t ini[MAX_PATH], w[64];
    _snwprintf(ini, MAX_PATH, L"%lsui_theme.ini", util::ModDir());
    GetPrivateProfileStringW(L"room", L"name", L"", w, 64, ini);
    snprintf(g_rs.name, sizeof g_rs.name, "%ls", w);
    g_rs.priv = GetPrivateProfileIntW(L"room", L"private", 0, ini) != 0;
    g_rs.size = ImClamp((int)GetPrivateProfileIntW(L"room", L"size", 8, ini), 2, 8);
    g_rs.rounds = ImClamp((int)GetPrivateProfileIntW(L"room", L"rounds", 2, ini), 1, 5);
    g_rs.timer = ImClamp((int)GetPrivateProfileIntW(L"room", L"timer", 2, ini), 0, 3);
    g_rs.setLen = ImClamp((int)GetPrivateProfileIntW(L"room", L"set_length", 0, ini), 0, 10);
#endif
}
static void MmRoomSettingsSave() {
#ifndef UI_HARNESS
    wchar_t ini[MAX_PATH], v[64];
    _snwprintf(ini, MAX_PATH, L"%lsui_theme.ini", util::ModDir());
    _snwprintf(v, 64, L"%hs", g_rs.name); WritePrivateProfileStringW(L"room", L"name", v, ini);
    _snwprintf(v, 64, L"%d", g_rs.priv ? 1 : 0); WritePrivateProfileStringW(L"room", L"private", v, ini);
    _snwprintf(v, 64, L"%d", g_rs.size); WritePrivateProfileStringW(L"room", L"size", v, ini);
    _snwprintf(v, 64, L"%d", g_rs.rounds); WritePrivateProfileStringW(L"room", L"rounds", v, ini);
    _snwprintf(v, 64, L"%d", g_rs.timer); WritePrivateProfileStringW(L"room", L"timer", v, ini);
    _snwprintf(v, 64, L"%d", g_rs.setLen); WritePrivateProfileStringW(L"room", L"set_length", v, ini);
#endif
}
static void MmSetLenText(int n, char* out, int cap) {
    if (n <= 0) snprintf(out, cap, "NO LIMIT");
    else snprintf(out, cap, "%d GAME%s", n, n == 1 ? "" : "S");
}
// one settings row: label, value; `dir` -1 / +1 changes it. Returns the new value.
static int MmStep(int v, int dir, int lo, int hi) { v += dir; return v < lo ? hi : v > hi ? lo : v; }
static void MmGoMenu() { MM.screen = 0; MM.openT = -1; MM.quietFrame = ImGui::GetFrameCount(); }

// shared timing for the room screens
struct MmT { double now; float t, tm, pulse, anim; };
static MmT MmTimes(double since) {
    MmT r;
    r.now = ImGui::GetTime();
    r.anim = theme::T().animSpeed > 0 ? theme::T().animSpeed : 0.0f;
    r.t = (float)(r.now - since);
    r.tm = (float)(r.now * r.anim);
    r.pulse = 0.5f + 0.5f * sinf(r.tm * 0.8f);
    return r;
}
static float MmEase(const MmT& T, float start, float len) {
    if (T.anim <= 0) return 1.0f;
    float v = ImClamp((T.t * T.anim - start) / len, 0.0f, 1.0f);
    return 1 - (1 - v) * (1 - v) * (1 - v);
}

// ============================ create room (screen 3) ============================
static int g_crSel = 0;
static bool g_crFocusName = false;
static void MmCreateFrame() {
    const MmPal& P = MmP();
    if (MM.roomT < 0) { MM.roomT = ImGui::GetTime(); g_crSel = 0; }
    MmRoomSettingsRead();
    if (!g_rs.name[0] && g_rv.me) snprintf(g_rs.name, sizeof g_rs.name, "%s's room", net::SteamFriendName(g_rv.me));
    MmT T = MmTimes(MM.roomT);
    bool typing = ImGui::GetIO().WantTextInput;
    MmNav nav = MmReadNav(typing);
    const int ROWS = 7;   // name, type, size, rounds, timer, set length, CREATE
    if (nav.dy) { g_crSel = (g_crSel + nav.dy + ROWS) % ROWS; MM_SND |= 1; }
    int change = nav.dx;
    bool create = false;
    if (nav.a) { if (g_crSel == 0) g_crFocusName = true; else if (g_crSel == 6) create = true; else change = 1; }
    if (nav.b) { MM_SND |= 4; MmGoMenu(); return; }

    ImDrawList* dl = MmPageBegin("##create", T.tm, T.pulse);
    float in = MmEase(T, 0, 0.5f);
    MmPageTitle(dl, "MAIN MENU", "CREATE ROOM", in);
    const float X0 = 560, X1 = 1360, Y0 = 200, RH = 84;
    MmSection(dl, X0, Y0, X1, Y0 + 40 + RH * 6 + 20, "ROOM SETTINGS", in);
    static const char* const labels[6] = {"ROOM NAME", "TYPE", "ROOM SIZE", "ROUNDS", "ROUND TIMER", "SET LENGTH"};
    for (int i = 0; i < 6; i++) {
        float y = Y0 + 40 + RH * i + RH / 2, rx0 = X0 + 16, rx1 = X1 - 16;
        if (MmHover(rx0, y - RH / 2, rx1, y + RH / 2) && ImGui::GetIO().MouseDelta.x + ImGui::GetIO().MouseDelta.y != 0 && g_crSel != i) { g_crSel = i; MM_SND |= 1; }
        bool on = g_crSel == i;
        if (on) MmImg(dl, "MA10_CMN", 83, rx0, y - 36, rx1 - rx0, 72, MmGlowCol(P.glow, (0.38f + 0.1f * T.pulse) * in), ImVec2(0, 0), ImVec2(1, 1), true);
        MmText(dl, g_fMenu, 17, X0 + 40, y, on ? MmC(P.gold, in) : MmC(P.dim, in), labels[i]);
        if (i < 5) MmLine(dl, X0 + 30, y + RH / 2, X1 - X0 - 60, 1, MmC(P.acc, 0.45f * in));
        char v[48] = "";
        bool clicked = MmClicked(X0 + 360, y - RH / 2, rx1, y + RH / 2);
        int dir = on ? change : 0;
        if (clicked && i > 0) dir = 1;
        switch (i) {
        case 0: {
            bool enter = MmTextField("##roomname", g_rs.name, sizeof g_rs.name, X0 + 360, y - 24, X1 - 40, y + 24, "Name of the room", g_crFocusName);
            g_crFocusName = false;
            if (enter) { g_crSel = 1; ImGui::SetWindowFocus(nullptr); }
            if (ImGui::IsItemActive()) g_crSel = 0;
            break;
        }
        case 1: if (dir) g_rs.priv = !g_rs.priv; snprintf(v, sizeof v, "%s", g_rs.priv ? "PRIVATE" : "PUBLIC"); break;
        case 2: if (dir) g_rs.size = MmStep(g_rs.size, dir, 2, 8); snprintf(v, sizeof v, "%d PLAYERS", g_rs.size); break;
        case 3: if (dir) g_rs.rounds = MmStep(g_rs.rounds, dir, 1, 5); snprintf(v, sizeof v, "%d", g_rs.rounds); break;
        case 4: if (dir) g_rs.timer = MmStep(g_rs.timer, dir, 0, 3); snprintf(v, sizeof v, "%s", MM_TIMER_NAMES[g_rs.timer]); break;
        case 5: if (dir) g_rs.setLen = MmStep(g_rs.setLen, dir, 0, 10); MmSetLenText(g_rs.setLen, v, sizeof v); break;
        }
        if (dir && i > 0) MM_SND |= 1;
        if (i > 0) {
            float cx = (X0 + 360 + X1 - 40) / 2;
            MmText(dl, g_fMenu, 22, cx, y, MmC(P.text, in), v, 1, 0, 0, 0.5f);
            ImU32 ac = on ? MmC(P.gold, in) : MmC(P.faint, 0.6f * in);   // < > arrows
            dl->AddTriangleFilled(CP(X0 + 380, y), CP(X0 + 394, y - 10), CP(X0 + 394, y + 10), ac);
            dl->AddTriangleFilled(CP(X1 - 60, y), CP(X1 - 74, y - 10), CP(X1 - 74, y + 10), ac);
        }
    }
    const char* hints[6] = {"Shown in Room search and to the friends you invite.",
                            "Public rooms show up in Room search. Private rooms get a code to share; join with the code or an invite.",
                            "2 to 8 players. Two players share a table, so 8 players means 4 tables.",
                            "Rounds needed to win a game.", "Time limit of each round, in seconds.",
                            "How many games two players play before both go back to the room."};
    if (g_crSel < 6) MmText(dl, g_fUi, 24, (X0 + X1) / 2, Y0 + 40 + RH * 6 + 60, MmC(P.dim, in), hints[g_crSel], 1);
    // CREATE button
    {
        float bx0 = 810, bx1 = 1110, by0 = 830, by1 = 900;
        if (MmHover(bx0, by0, bx1, by1) && g_crSel != 6 && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) { g_crSel = 6; MM_SND |= 1; }
        if (MmClicked(bx0, by0, bx1, by1)) create = true;
        if (g_crSel == 6) MmImg(dl, "MA10_CMN", 83, bx0 - 80, by0 - 20, bx1 - bx0 + 160, by1 - by0 + 40, MmGlowCol(P.glow, 0.5f * in), ImVec2(0, 0), ImVec2(1, 1), true);
        MmButton(dl, bx0, by0, bx1, by1, "OPEN THE ROOM", g_crSel == 6, T.pulse);
    }
    if (create) {
        if (!g_rv.steam) MmNote("Steam isn't running, so rooms can't be opened");
        else {
            if (!g_rs.name[0]) snprintf(g_rs.name, sizeof g_rs.name, "%s's room", net::SteamFriendName(g_rv.me));
            MmRoomSettingsSave();
            net::RoomCreate(g_rs);
            MM_SND |= 2;
            MM.screen = 1; MM.roomT = -1;
        }
    }
    MmPadButton(dl, 50, 1004, 'A', MmRGBA(40, 160, 70));
    MmText(dl, g_fMenu, 13, 94, 1022, MmC(P.text), "Select");
    MmPadButton(dl, 200, 1004, 'B', MmRGBA(190, 40, 40));
    MmText(dl, g_fMenu, 13, 244, 1022, MmC(P.text), "Back");
    MmText(dl, g_fUi, 20, 340, 1022, MmC(P.faint), "Left / right changes a setting");
    MmNoteDraw(dl, T.now, 560, 960);
    MmPageEnd(dl);
}

// ============================ room search (screen 2) ============================
static int g_srSel = 0;
static double g_srLast = -100;
static uint32_t g_srSeq = 0;
static bool g_codeOpen = false, g_codeFocus = false;   // build 114: "join with code" box
static char g_codeIn[16] = "";
static void MmSearchFrame() {
    const MmPal& P = MmP();
    if (MM.roomT < 0) { MM.roomT = ImGui::GetTime(); g_srSel = 0; g_srLast = -100; g_codeOpen = false; }
    MmT T = MmTimes(MM.roomT);
    if (g_codeOpen) {   // build 114: type the code, Enter joins, Esc / B closes
        ImGuiIO& io = ImGui::GetIO();
        bool typing = io.WantTextInput;
        MmNav nav = MmReadNav(typing);
        if ((nav.b && !typing) || (typing && ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Escape]))) { g_codeOpen = false; MM_SND |= 4; ImGui::SetWindowFocus(nullptr); }
        ImDrawList* dl = MmPageBegin("##search", T.tm, T.pulse);
        MmPageTitle(dl, "MAIN MENU", "JOIN WITH CODE", 1);
        float x0 = 660, x1 = 1260, y0 = 380, y1 = 640;
        MmSection(dl, x0, y0, x1, y1, "ROOM CODE", 1);
        MmText(dl, g_fUi, 24, 960, y0 + 60, MmC(P.dim), "Type the code the host shares (6 letters and numbers).", 1);
        bool enter = MmTextField("##code", g_codeIn, 8, 760, y0 + 100, 1160, y0 + 160, "ABC123", g_codeFocus);
        g_codeFocus = false;
        MmText(dl, g_fUi, 22, 960, y0 + 210, MmC(P.faint), "Enter joins  -  Esc closes", 1);
        if (enter && strlen(g_codeIn) >= 4) {
            net::RoomJoinCode(g_codeIn);
            g_codeIn[0] = 0; g_codeOpen = false; MM_SND |= 2;
            MM.screen = 1; MM.roomT = -1;
        }
        MmPageEnd(dl);
        return;
    }
    if (T.now - g_srLast > 15.0 && !g_rv.searching) { net::RoomSearch(); g_srLast = T.now; }   // refresh now and then
    MmNav nav = MmReadNav(false);
    int n = g_rv.nRooms;
    if (g_rv.searchSeq != g_srSeq) { g_srSeq = g_rv.searchSeq; if (g_srSel >= n) g_srSel = n ? n - 1 : 0; }
    if (nav.dy && n) { g_srSel = (g_srSel + nav.dy + n) % n; MM_SND |= 1; }
    if (nav.y) { net::RoomSearch(); g_srLast = T.now; MM_SND |= 1; }
    if (nav.x) { g_codeOpen = true; g_codeFocus = true; g_codeIn[0] = 0; MM_SND |= 2; return; }
    if (nav.b) { MM_SND |= 4; MmGoMenu(); return; }
    int join = -1;
    if (nav.a && n) join = g_srSel;

    ImDrawList* dl = MmPageBegin("##search", T.tm, T.pulse);
    float in = MmEase(T, 0, 0.5f);
    MmPageTitle(dl, "MAIN MENU", "ROOM SEARCH", in);
    MmPadButton(dl, 1640, 56, 'Y', MmRGBA(210, 170, 20, in));
    MmText(dl, g_fMenu, 18, 1686, 74, MmC(P.text, in), "REFRESH");
    if (MmClicked(1630, 46, 1880, 100)) { net::RoomSearch(); g_srLast = T.now; }
    MmPadButton(dl, 1240, 56, 'X', MmRGBA(40, 100, 200, in));   // build 114
    MmText(dl, g_fMenu, 18, 1286, 74, MmC(P.text, in), "JOIN WITH CODE");
    if (MmClicked(1230, 46, 1600, 100)) { g_codeOpen = true; g_codeFocus = true; g_codeIn[0] = 0; MM_SND |= 2; }
    const float X0 = 360, X1 = 1560, Y0 = 190, RH = 74;
    float listBottom = 930;
    MmSection(dl, X0, Y0, X1, listBottom, "PUBLIC ROOMS", in);
    MmText(dl, g_fMenu, 12, X0 + 40, Y0 + 40, MmC(P.faint, in), "ROOM");
    MmText(dl, g_fMenu, 12, X0 + 700, Y0 + 40, MmC(P.faint, in), "HOST");
    MmText(dl, g_fMenu, 12, X1 - 40, Y0 + 40, MmC(P.faint, in), "PLAYERS", 2);
    if (!g_rv.steam) {
        MmText(dl, g_fUi, 28, (X0 + X1) / 2, 500, MmC(P.dim, in), "Steam isn't running, so rooms can't be found.", 1);
    } else if (n == 0) {
        if (g_rv.searching) {
            MmSpinner(dl, (X0 + X1) / 2, 470, 20, T.tm, P.gold);
            MmText(dl, g_fUi, 28, (X0 + X1) / 2, 530, MmC(P.dim, in), "Looking for rooms...", 1);
        } else {
            MmText(dl, g_fMenu, 22, (X0 + X1) / 2, 470, MmC(P.text, in), "NO PUBLIC ROOMS RIGHT NOW", 1);
            MmText(dl, g_fUi, 26, (X0 + X1) / 2, 526, MmC(P.dim, in), "Open one with Create room, or ask a friend to invite you to theirs.", 1);
        }
    } else {
        int first = g_srSel >= 9 ? g_srSel - 8 : 0;
        for (int k = 0; k < 9 && first + k < n; k++) {
            int i = first + k;
            const net::RoomInfo& r = g_rv.rooms[i];
            float y = Y0 + 70 + k * RH + RH / 2;
            if (MmHover(X0 + 16, y - RH / 2, X1 - 16, y + RH / 2)) {
                if ((ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0) && g_srSel != i) { g_srSel = i; MM_SND |= 1; }
                if (ImGui::GetIO().MouseClicked[0]) join = i;
            }
            bool on = g_srSel == i, full = r.limit > 0 && r.members >= r.limit;
            if (on) MmImg(dl, "MA10_CMN", 83, X0 + 16, y - 34, X1 - X0 - 32, 68, MmGlowCol(P.glow, (0.38f + 0.1f * T.pulse) * in), ImVec2(0, 0), ImVec2(1, 1), true);
            MmText(dl, g_fMenu, 20, X0 + 40, y, on ? MmC(P.gold, in) : MmC(P.text, in), r.name, 0, 0, 0, 0.4f);
            MmText(dl, g_fUiBold, 26, X0 + 700, y, MmC(P.dim, in), r.host);
            char pl[32];
            snprintf(pl, sizeof pl, "%d / %d", r.members, r.limit);
            MmText(dl, g_fMenu, 20, X1 - 40, y, full ? MmC(P.faint, in) : MmC(P.text, in), pl, 2);
            if (full) MmPill(dl, X1 - 220, y, 90, 26, "FULL", P.faint, false, in);
            if (k < 8 && first + k + 1 < n) MmLine(dl, X0 + 30, y + RH / 2, X1 - X0 - 60, 1, MmC(P.acc, 0.4f * in));
        }
        if (g_rv.searching) MmSpinner(dl, X1 - 40, Y0 + 40, 10, T.tm, P.gold);
    }
    if (join >= 0 && join < n) {
        MM_SND |= 2;
        net::RoomJoin(g_rv.rooms[join].id);
        MM.screen = 1; MM.roomT = -1;
    }
    MmText(dl, g_fUi, 22, (X0 + X1) / 2, 966, MmC(P.faint, in), "Only rooms of this build of the mod are shown. Private rooms: X and the room code, or a Steam invite.", 1);
    MmPadButton(dl, 50, 1004, 'A', MmRGBA(40, 160, 70));
    MmText(dl, g_fMenu, 13, 94, 1022, MmC(P.text), "Join");
    MmPadButton(dl, 180, 1004, 'B', MmRGBA(190, 40, 40));
    MmText(dl, g_fMenu, 13, 224, 1022, MmC(P.text), "Back");
    MmNoteDraw(dl, T.now, 360, 1022);
    MmPageEnd(dl);
}

// ============================ the room (screen 1) ============================
static void SuOpen();                                   // setupui.inc
static int SuPortraitOf(int chara);
static void SuRoomPanel(ImDrawList* dl, float x0, float y0, float x1, float y1, float in);
#ifndef UI_HARNESS
static int RmViewState() { return RMV.st.load(); }    // build 112 (roommatch.inc)
static uint32_t RmViewT0() { return RMV.t0.load(); }
static bool RmViewConnected() { return RMV.connected.load(); }
static uint32_t RmNowMs() { return GetTickCount(); }
#else
static int g_hRm = 0;
static int RmViewState() { return g_hRm; }
static uint32_t RmViewT0() { return 0; }
static bool RmViewConnected() { return false; }
static uint32_t RmNowMs() { return 1200; }
#endif
static int g_seatSel = 0;
static const net::RoomMember* MmSeatOwner(const net::RoomView& R, int seat) {
    for (int i = 0; i < R.nMembers; i++) if (R.members[i].seat == seat) return &R.members[i];
    return nullptr;
}
// build 112: A on a seat: sit down (or move), or READY / not ready on your own seat
static void MmSeatPress(const net::RoomView& R, int seat) {
    if (RmViewState() == 2) { MmNote("Your match is still finishing"); return; }
    const net::RoomMember* o = MmSeatOwner(R, seat);
    if (seat == R.mySeat) { net::RoomSetReady(!R.myReady); MM_SND |= R.myReady ? 4 : 2; return; }
    if (o) { MmNote(o->playing ? "That table is playing a match" : "That seat is taken"); MM_SND |= 4; return; }
    net::RoomSit(seat);
    MM_SND |= 2;
}
static char g_chatIn[200] = "";
static bool g_chatFocus = false;
static bool g_invOpen = false;
static int g_invSel = 0;
static void MmRoomFrame() {
    const MmPal& P = MmP();
    if (MM.roomT < 0) { MM.roomT = ImGui::GetTime(); g_invOpen = false; g_seatSel = g_rv.mySeat >= 0 ? g_rv.mySeat : 0; }
    MmT T = MmTimes(MM.roomT);
    ImGuiIO& io = ImGui::GetIO();
    bool typing = io.WantTextInput;
    if (typing && ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Escape])) { ImGui::SetWindowFocus(nullptr); typing = false; }
    MmNav nav = MmReadNav(typing);
    const net::RoomView& R = g_rv;
    bool inRoom = R.state == net::ROOM_IN && R.id;
    bool isHost = inRoom && R.owner == R.me;

    // ---- waiting / failed ----
    if (!inRoom) {
        bool busy = R.state == net::ROOM_CREATING || R.state == net::ROOM_JOINING;
        if (nav.b || (!busy && nav.a)) { if (busy) net::RoomLeave(); MM_SND |= 4; MmGoMenu(); return; }
        ImDrawList* dl = MmPageBegin("##room", T.tm, T.pulse);
        MmPageTitle(dl, "ROOM", busy ? (R.state == net::ROOM_CREATING ? "OPENING THE ROOM" : "JOINING THE ROOM") : "NO ROOM", 1);
        if (busy) {
            MmSpinner(dl, 960, 500, 26, T.tm, P.gold);
            MmText(dl, g_fUi, 28, 960, 570, MmC(P.dim), R.state == net::ROOM_CREATING ? "Asking Steam for a room..." : "Getting in...", 1);
        } else {
            MmText(dl, g_fMenu, 22, 960, 480, MmC(P.text), R.err[0] ? R.err : "You are not in a room", 1);
            MmText(dl, g_fUi, 24, 960, 540, MmC(P.dim), "Press A or B to go back to the main menu.", 1);
        }
        MmPadButton(dl, 50, 1004, 'B', MmRGBA(190, 40, 40));
        MmText(dl, g_fMenu, 13, 94, 1022, MmC(P.text), busy ? "Cancel" : "Back");
        MmPageEnd(dl);
        return;
    }

    // ---- invite friends panel takes the input while open ----
    if (g_invOpen) {
        int nf = R.nFriends;
        if (nav.dy && nf) { g_invSel = (g_invSel + nav.dy + nf) % nf; MM_SND |= 1; }
        if (nav.b || nav.x) { g_invOpen = false; MM_SND |= 4; nav = MmNav(); }
        else if (nav.a && nf) { net::RoomInviteFriend(R.friends[g_invSel].id); MM_SND |= 2; nav = MmNav(); }
        else nav = MmNav();
    }
    if (nav.x && !typing) { g_invOpen = true; g_invSel = 0; net::RoomRefreshFriends(); MM_SND |= 2; }
    // build 112: the seat cursor (columns: A1P A2P B1P B2P, rows: A/B over C/D)
    int nTables = ImClamp(R.set.size / 2, 1, 4), nSeats = nTables * 2;
    if (g_seatSel >= nSeats) g_seatSel = 0;
    if (!typing && (nav.dx || nav.dy)) {
        int t = g_seatSel / 2, col = (t % 2) * 2 + (g_seatSel & 1), row = t / 2;
        col += nav.dx; row += nav.dy;
        if (col >= 0 && col < 4 && row >= 0 && row < 2) {
            int s2 = (row * 2 + col / 2) * 2 + (col & 1);
            if (s2 < nSeats) { g_seatSel = s2; MM_SND |= 1; }
        }
    }
    if (nav.a && !typing) MmSeatPress(R, g_seatSel);
    bool seated = R.mySeat >= 0;
    if (nav.y && !typing) { MM_SND |= 2; SuOpen(); return; }
    // B / Esc: build 101: you can't go to the main menu while you are in a room; holding B 1 s
    // leaves the room (then the main menu). A short press only says so.
    static double bStart = -1;
    float hold = 0;
    {
        bool bDown = !typing && ImGui::IsKeyDown(io.KeyMap[ImGuiKey_Escape]);
#ifndef UI_HARNESS
        if (U.capture.load() && (U.navBits & PB_B)) bDown = true;
#endif
        if (nav.b && bStart < 0) bStart = T.now;
        if (bStart >= 0) {
            hold = (float)(T.now - bStart);
            if (!bDown) {
                bStart = -1;
                if (seated && RmViewState() != 2) { net::RoomSit(-1); MM_SND |= 4; }   // build 112: B stands up
                else MmNote("Hold B to leave the room");
            }
            if (hold >= 1.0f) { bStart = -1; net::RoomLeave(); MM_SND |= 4; MmGoMenu(); return; }
        }
    }
    if (!typing && ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Tab])) g_chatFocus = true;

    ImDrawList* dl = MmPageBegin("##room", T.tm, T.pulse);
    auto ease = [&](float s, float l) { return MmEase(T, s, l); };

    // ---- top bar ----
    {
        float in = ease(0.0f, 0.5f);
        MmText(dl, g_fMenu, 13, 60, 38, MmC(P.faint, in), "ROOM");
        float w = MmText(dl, g_fMenu, 30, 60, 74, MmC(P.text, in), R.set.name, 0, 2, MmC(P.ink, in));
        float px = ImMax(760.0f, 60 + w + 100);
        MmPill(dl, px, 74, 140, 36, R.set.priv ? "PRIVATE" : "PUBLIC", P.gold, false, in);
        MmText(dl, g_fMenu, 15, px + 120, 74, MmC(P.dim, in), "PLAYERS");
        char nb[8];
        snprintf(nb, sizeof nb, "%d", R.nMembers);
        float nw = MmText(dl, g_fMenu, 34, px + 240, 74, MmC(P.gold, in), nb, 0, 2, MmC(P.ink, in));
        char lb[8];
        snprintf(lb, sizeof lb, "/ %d", R.set.size);
        MmText(dl, g_fMenu, 22, px + 250 + nw, 80, MmC(P.dim, in), lb);
        if (R.code[0]) {   // build 114: the private room's code
            float cx = px + 420;
            MmText(dl, g_fMenu, 15, cx, 74, MmC(P.dim, in), "CODE");
            MmText(dl, g_fMenu, 30, cx + 80, 74, MmC(P.gold, in), R.code, 0, 2, MmC(P.ink, in));
        }
        MmPadButton(dl, 1600, 56, 'B', MmRGBA(190, 40, 40, in));
        if (hold > 0.15f) {   // how long B has been held
            dl->PathArcTo(CP(1618, 74), CV(24), -IM_PI / 2, -IM_PI / 2 + IM_PI * 2 * ImMin(hold, 1.0f), 32);
            dl->PathStroke(MmC(P.gold), false, CV(4));
        }
        MmText(dl, g_fMenu, 12, 1646, 62, MmC(P.faint, in), "HOLD TO");
        MmText(dl, g_fMenu, 18, 1646, 86, MmRGBA(255, 150, 130, in), "LEAVE ROOM");
        if (MmClicked(1590, 46, 1890, 100)) { net::RoomLeave(); MM_SND |= 4; MmPageEnd(dl); MmGoMenu(); return; }
        dl->PushClipRect(ImVec2(0, 0), g_disp, false);
        MmLine(dl, -g_cvO.x / g_cv, 118, g_disp.x / g_cv, 3, MmC(P.acc, 0.95f * in));
        MmLine(dl, -g_cvO.x / g_cv, 122, g_disp.x / g_cv, 1, MmC(P.gold, 0.6f * in));
        dl->PopClipRect();
    }

    // ---- left: room settings + players ----
    const float LX0 = 50, LX1 = 470;
    {
        float in = ease(0.1f, 0.5f), ox = (1 - in) * -40;
        MmSection(dl, LX0 + ox, 176, LX1 + ox, 452, "ROOM SETTINGS", in);
        char v[5][48];
        snprintf(v[0], 48, "%d", R.set.rounds);
        snprintf(v[1], 48, "%s", MM_TIMER_NAMES[R.set.timer & 3]);
        MmSetLenText(R.set.setLen, v[2], 48);
        snprintf(v[3], 48, "%d", R.set.size);
        const char* hostName = "?";
        for (int i = 0; i < R.nMembers; i++) if (R.members[i].host) hostName = R.members[i].name;
        snprintf(v[4], 48, "%s", hostName);
        const char* rows[5] = {"ROUNDS", "ROUND TIMER", "SET LENGTH", "ROOM SIZE", "HOST"};
        net::RoomSettings ch = R.set;
        bool changed = false;
        for (int i = 0; i < 5; i++) {
            float y = 210 + i * 46;
            bool can = isHost && i < 4;
            bool hov = can && MmHover(LX0 + 10, y - 22, LX1 - 10, y + 22);
            if (hov) MmImg(dl, "MA10_CMN", 83, LX0 + 8 + ox, y - 22, LX1 - LX0 - 16, 44, MmGlowCol(P.glow, 0.30f * in), ImVec2(0, 0), ImVec2(1, 1), true);
            MmText(dl, g_fMenu, 14, LX0 + 24 + ox, y, MmC(P.dim, in), rows[i]);
            MmText(dl, g_fUiBold, 27, LX1 - 24 + ox, y, hov ? MmC(P.gold, in) : MmC(P.text, in), v[i], 2);
            if (i < 4) MmLine(dl, LX0 + 20 + ox, y + 22, LX1 - LX0 - 40, 1, MmC(P.acc, 0.5f * in));
            if (hov && (io.MouseClicked[0] || io.MouseClicked[1])) {
                int dir = io.MouseClicked[1] ? -1 : 1;
                if (i == 0) ch.rounds = MmStep(ch.rounds, dir, 1, 5);
                if (i == 1) ch.timer = MmStep(ch.timer, dir, 0, 3);
                if (i == 2) ch.setLen = MmStep(ch.setLen, dir, 0, 10);
                if (i == 3) { ch.size = MmStep(ch.size, dir, 2, 8); if (ch.size < R.nMembers) ch.size = dir > 0 ? R.nMembers : 8; }
                changed = true;
            }
        }
        if (changed) { net::RoomChange(ch); MM_SND |= 1; }
        MmText(dl, g_fUi, 19, LX0 + 24 + ox, 430, MmC(P.faint, in), isHost ? "You are the host: click a setting or a player" : "Only the host can change these");

        MmSection(dl, LX0 + ox, 506, LX1 + ox, 962, "PLAYERS", in);
        for (int k = 0; k < R.nMembers && k < 7; k++) {
            const net::RoomMember& m = R.members[k];
            float y = 546 + k * 54;
            if (m.me) MmImg(dl, "MA10_CMN", 83, LX0 + 8 + ox, y - 26, LX1 - LX0 - 16, 52, MmGlowCol(P.glow, 0.40f * in), ImVec2(0, 0), ImVec2(1, 1), true);
            float w = MmText(dl, g_fUiBold, 27, LX0 + 30 + ox, y, m.me ? MmC(P.gold, in) : MmC(P.text, in), m.name);
            if (m.host) MmPill(dl, LX0 + 30 + w + 42 + ox, y, 64, 24, "HOST", P.gold, false, in);
            char where[24];   // build 112
        if (m.seat >= 0) snprintf(where, sizeof where, "%s %c", m.playing ? "PLAYING" : "TABLE", 'A' + m.seat / 2);
        else snprintf(where, sizeof where, "%s", m.me ? "YOU" : "IN ROOM");
        // build 113: the host clicks a player to remove them (a second click within 3 s confirms)
        static uint64_t kickArm = 0;
        static double kickAt = 0;
        bool canKick = isHost && !m.me && !m.standin && !g_invOpen;
        bool hovK = canKick && MmHover(LX0 + 10, y - 26, LX1 - 10, y + 26);
        bool armed = kickArm == m.id && T.now - kickAt < 3.0;
        if (hovK && io.MouseClicked[0]) {
            if (armed) { net::RoomKick(m.id); kickArm = 0; MM_SND |= 4; }
            else { kickArm = m.id; kickAt = T.now; MM_SND |= 1; armed = true; }
        }
        if (armed) MmPill(dl, LX1 - 70 + ox, y, 96, 26, "KICK?", MmRgb{230, 70, 60}, true, in);
        else if (hovK) MmPill(dl, LX1 - 70 + ox, y, 96, 26, "KICK", MmRgb{230, 70, 60}, false, in);
        else MmText(dl, g_fMenu, 12, LX1 - 24 + ox, y, m.seat >= 0 ? MmC(P.dim, in) : MmC(P.faint, in), where, 2);
        }
        if (R.nMembers > 7) { char s[32]; snprintf(s, sizeof s, "+ %d more", R.nMembers - 7); MmText(dl, g_fUi, 20, LX0 + 30 + ox, 920, MmC(P.dim, in), s); }
        MmPadButton(dl, LX0 + 20 + ox, 912, 'X', MmRGBA(40, 100, 200, in));
        MmText(dl, g_fMenu, 14, LX0 + 66 + ox, 930, MmC(P.text, in), "Invite friends");
        MmText(dl, g_fMenu, 12, LX1 - 24 + ox, 930, MmC(P.faint, in), "STEAM", 2);
        if (MmClicked(LX0 + 10, 900, LX1 - 10, 960)) { g_invOpen = true; g_invSel = 0; net::RoomRefreshFriends(); MM_SND |= 2; }
    }

    // ---- centre: tables (build 112: real seats) ----
    const float TX0 = 500, TY0 = 168, TW = 455, TH = 350, GAP = 18;
    for (int ti = 0; ti < 4; ti++) {
        float in = ease(0.15f + 0.07f * ti, 0.5f);
        float x0 = TX0 + (ti % 2) * (TW + GAP), y0 = TY0 + (ti / 2) * (TH + GAP) + (1 - in) * 30, x1 = x0 + TW, y1 = y0 + TH;
        bool used = ti < nTables, mine = seated && R.mySeat / 2 == ti;
        float a = used ? in : 0.35f * in;
        if (mine) MmImg(dl, "MA10_CMN", 39, x0 - 40, y0 - 40, TW + 80, TH + 80, MmGlowCol(P.glow, (0.30f + 0.15f * T.pulse) * in), ImVec2(0, 0), ImVec2(1, 1), true);
        dl->AddRectFilled(CP(x0 + 6, y0 + 6), CP(x1 - 6, y1 - 6), MmC(P.panel, 0.88f * a), CV(14));
        MmNine(dl, "MA10_CS_NETWORK", 45, 26, 26, x0, y0, x1, y1, MmRGBA(255, 255, 255, a));
        if (mine) dl->AddRect(CP(x0 + 4, y0 + 4), CP(x1 - 4, y1 - 4), MmC(P.gold, 0.8f * in), CV(12), 0, CV(2.5f));
        ImVec2 c = CP(x0 + 40, y0 + 40);
        dl->AddCircleFilled(c, CV(20), MmC(P.acc, a), 32);
        dl->AddCircle(c, CV(20), MmC(P.text, a), 32, CV(2));
        char L[2] = {(char)('A' + ti), 0};
        MmText(dl, g_fMenu, 18, x0 + 40, y0 + 40, MmC(P.text, a), L, 1, 0, 0, 0.5f);
        char nm[16];
        snprintf(nm, sizeof nm, "TABLE %c", 'A' + ti);
        MmText(dl, g_fMenu, 20, x0 + 74, y0 + 40, MmC(P.text, a), nm, 0, 1, MmC(P.ink, a));
        if (!used) { MmText(dl, g_fUi, 20, (x0 + x1) / 2, y0 + 200, MmC(P.faint, a), "Not used at this room size", 1); continue; }
        const net::RoomMember* sm[2] = {MmSeatOwner(R, ti * 2), MmSeatOwner(R, ti * 2 + 1)};
        bool playing = sm[0] && sm[1] && (sm[0]->playing || sm[1]->playing);
        bool canSit = RmViewState() != 2;
        float sx[2] = {x0 + 30, x1 - 160}, sy = y0 + 74;
        for (int k = 0; k < 2; k++) {
            int seat = ti * 2 + k;
            // the cursor (glow behind the seat) and the mouse
            bool hov = MmHover(sx[k] - 10, sy - 14, sx[k] + 140, sy + 250);
            if (hov && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) && g_seatSel != seat) { g_seatSel = seat; MM_SND |= 1; }
            if (hov && io.MouseClicked[0] && !g_invOpen) { g_seatSel = seat; MmSeatPress(R, seat); }
            if (g_seatSel == seat && !g_invOpen)
                MmImg(dl, "MA10_CMN", 39, sx[k] - 50, sy - 50, 230, 330, MmGlowCol(P.glow, (0.45f + 0.2f * T.pulse) * in), ImVec2(0, 0), ImVec2(1, 1), true);
            const net::RoomMember* m = sm[k];
            MmSeat st{-1, "", nullptr};
            if (m) {
                st.port = m->hasSetup ? SuPortraitOf(m->chara == 0xFF ? -1 : m->chara) : 48;
                st.name = m->me ? "You" : m->name;
                st.pill = playing ? "PLAY" : m->ready ? "READY" : "WAIT";
            }
            if (g_seatSel == seat && !g_invOpen) dl->AddRect(CP(sx[k] - 4, sy - 4), CP(sx[k] + 134, sy + 194), MmC(P.gold, in), CV(10), 0, CV(3));
            MmSeatDraw(dl, sx[k], sy, st, k + 1, canSit && !m);   // (build 114: over the cursor, so the badge stays on top)
        }
        float cx = (x0 + x1) / 2;
        bool both = sm[0] && sm[1];
        MmImg(dl, "MA10_CS_NETWORK", 149, cx - 44, y0 + 108, 88, 58, MmRGBA(255, 255, 255, (both ? 1.0f : 0.35f) * in));
        if (!sm[0] && !sm[1]) {
            MmText(dl, g_fMenu, 16, cx, y0 + 230, MmC(P.faint, in), "OPEN", 1);
        } else if (!both) {
            MmSpinner(dl, cx, y0 + 214, 14, T.tm, P.dim);
            MmText(dl, g_fUi, 21, cx, y0 + 256, MmC(P.dim, in), "Waiting for", 1);
            MmText(dl, g_fUi, 21, cx, y0 + 280, MmC(P.dim, in), "a challenger", 1);
        } else if (playing) {
            const net::RoomMember* sc = sm[0]->playing ? sm[0] : sm[1];
            char score[24];
            snprintf(score, sizeof score, "%d - %d", sc->wins[0], sc->wins[1]);
            MmText(dl, g_fMenu, 40, cx, y0 + 214, MmC(P.gold, in), score, 1, 2, MmC(P.ink, in));
            char g[32];
            if (R.set.setLen > 0) snprintf(g, sizeof g, "GAME %d OF %d", sc->game > 0 ? sc->game : 1, R.set.setLen);
            else snprintf(g, sizeof g, "GAME %d", sc->game > 0 ? sc->game : 1);
            MmText(dl, g_fMenu, 12, cx, y0 + 256, MmC(P.dim, in), g, 1);
            MmText(dl, g_fMenu, 12, cx, y0 + 300, MmC(P.faint, in), "IN MATCH", 1);
        } else if (mine && RmViewState() == 1) {
            // both READY: the countdown, then (if the connection takes longer) "connecting"
            uint32_t el = RmNowMs() - RmViewT0();
            int left = el < 3000 ? 3 - (int)(el / 1000) : 0;
            MmText(dl, g_fMenu, 12, cx, y0 + 196, MmC(P.dim, in), "STARTS IN", 1);
            if (left > 0) {
                char n[4]; snprintf(n, sizeof n, "%d", left);
                float f = (el % 1000) / 1000.0f;
                MmText(dl, g_fMenu, 46 + 10 * (1 - f), cx, y0 + 244, MmC(P.gold, in), n, 1, 2, MmC(P.ink, in));
            } else {
                MmSpinner(dl, cx, y0 + 240, 14, T.tm, P.gold);
                MmText(dl, g_fUi, 20, cx, y0 + 280, MmC(P.dim, in), RmViewConnected() ? "Starting..." : "Connecting...", 1);
            }
        } else {
            MmText(dl, g_fMenu, 11, cx, y0 + 200, MmC(P.faint, in), "STAGE", 1);
            MmText(dl, g_fUi, 20, cx, y0 + 226, MmC(P.dim, in), "random of", 1);
            MmText(dl, g_fUi, 20, cx, y0 + 248, MmC(P.dim, in), "both picks", 1);
            bool r0 = sm[0]->ready, r1 = sm[1]->ready;
            MmText(dl, g_fUi, 20, cx, y0 + 290, MmC(r0 || r1 ? P.gold : P.dim, in), r0 && r1 ? "Starting..." : "Both READY", 1);
            MmText(dl, g_fUi, 20, cx, y0 + 312, MmC(r0 || r1 ? P.gold : P.dim, in), r0 && r1 ? "" : "to start", 1);
        }
    }

    // ---- bottom centre: my match settings (My setup; Y edits it) ----
    {
        float in = ease(0.4f, 0.5f), oy = (1 - in) * 30;
        float x0 = TX0, x1 = TX0 + 2 * TW + GAP, y0 = 922 + oy, y1 = 1050 + oy;
        MmSection(dl, x0, y0, x1, y1, "MY MATCH SETTINGS", in);
        SuRoomPanel(dl, x0, y0, x1, y1, in);
        if (MmClicked(x0, y0, x1, y1)) { MM_SND |= 2; SuOpen(); MmPageEnd(dl); return; }
    }

    // ---- right: room chat + buttons ----
    const float RX0 = 1464, RX1 = 1870;
    {
        float in = ease(0.2f, 0.5f), ox = (1 - in) * 40;
        MmSection(dl, RX0 + ox, 176, RX1 + ox, 940, "ROOM CHAT", in);
        // newest at the bottom: lay out from the end until the box is full
        static char lines[6][200];
        float yb = 846, ytop = 200;
        for (int i = R.nChat - 1; i >= 0 && yb > ytop; i--) {
            const net::ChatLine& c = R.chat[i];
            if (c.system) {
                int nl = MmWrap(g_fUi, 21, c.text, RX1 - RX0 - 44, lines, 3);
                for (int k = nl - 1; k >= 0 && yb > ytop; k--) { MmText(dl, g_fUi, 21, RX0 + 22 + ox, yb, MmC(P.faint, in), lines[k]); yb -= 28; }
                yb -= 12;
                continue;
            }
            int nl = MmWrap(g_fUi, 23, c.text, RX1 - RX0 - 44, lines, 6);
            for (int k = nl - 1; k >= 0 && yb > ytop; k--) { MmText(dl, g_fUi, 23, RX0 + 22 + ox, yb, MmC(P.text, in), lines[k]); yb -= 30; }
            if (yb <= ytop) break;
            bool mine = c.from == R.me;
            MmRgb nc = mine ? P.gold : (c.from % 3 == 0 ? MmRgb{255, 120, 100} : c.from % 3 == 1 ? MmRgb{240, 170, 110} : P.dim);
            MmText(dl, g_fUiBold, 23, RX0 + 22 + ox, yb, MmC(nc, in), mine ? "You" : c.name);
            yb -= 44;
        }
        if (MmTextField("##chat", g_chatIn, sizeof g_chatIn, RX0 + 16 + ox, 868, RX1 - 16 + ox, 920, "Say something... (Tab)", g_chatFocus)) {
            if (g_chatIn[0]) net::RoomSay(g_chatIn);
            g_chatIn[0] = 0;
            g_chatFocus = true;   // stay in the field to keep talking
        } else g_chatFocus = false;
        if (seated) {   // build 112
            MmButton(dl, RX0 + ox, 972, RX0 + 196 + ox, 1036, R.myReady ? "NOT READY" : "READY", !R.myReady, T.pulse);
            MmButton(dl, RX0 + 212 + ox, 972, RX1 + ox, 1036, "STAND UP", false, T.pulse);
            if (MmClicked(RX0, 972, RX0 + 196, 1036)) MmSeatPress(R, R.mySeat);
            if (MmClicked(RX0 + 212, 972, RX1, 1036) && RmViewState() != 2) { net::RoomSit(-1); MM_SND |= 4; }
        } else {
            MmButton(dl, RX0 + ox, 972, RX0 + 196 + ox, 1036, "INVITE", true, T.pulse);
            MmButton(dl, RX0 + 212 + ox, 972, RX1 + ox, 1036, "LEAVE", false, T.pulse);
            if (MmClicked(RX0, 972, RX0 + 196, 1036)) { g_invOpen = true; g_invSel = 0; net::RoomRefreshFriends(); MM_SND |= 2; }
            if (MmClicked(RX0 + 212, 972, RX1, 1036)) { net::RoomLeave(); MM_SND |= 4; MmPageEnd(dl); MmGoMenu(); return; }
        }
    }

    // ---- bottom left: prompts ----
    {   // build 112
        const net::RoomMember* o = MmSeatOwner(R, g_seatSel);
        const char* aText = g_seatSel == R.mySeat ? (R.myReady ? "Not ready" : "Ready") : o ? "" : seated ? "Move" : "Sit";
        float x = 50;
        if (aText[0]) { MmPadButton(dl, x, 1004, 'A', MmRGBA(40, 160, 70)); MmText(dl, g_fMenu, 13, x + 44, 1022, MmC(P.text), aText); x += 150; }
        MmPadButton(dl, x, 1004, 'B', MmRGBA(190, 40, 40));
        MmText(dl, g_fMenu, 13, x + 44, 1022, MmC(P.text), seated ? "Stand up" : "Hold: leave");
        MmText(dl, g_fUi, 18, 50, 1056, MmC(P.faint), "X Invite   Y My setup   Tab Chat");
    }
    MmNoteDraw(dl, T.now, 50, 970);

    // ---- invite friends panel ----
    if (g_invOpen) {
        dl->AddRectFilled(CP(0, 0), CP(1920, 1080), MmC(P.bgBot, 0.75f));
        float x0 = 660, x1 = 1260, y0 = 170, y1 = 930;
        dl->AddRectFilled(CP(x0, y0 - 40), CP(x1, y1), MmC(P.bgBot, 1.0f), CV(10));   // solid under the panel
        MmSection(dl, x0, y0, x1, y1, "INVITE FRIENDS", 1);
        if (R.nFriends == 0) MmText(dl, g_fUi, 24, (x0 + x1) / 2, 500, MmC(P.dim), "No Steam friends online right now", 1);
        int first = g_invSel >= 11 ? g_invSel - 10 : 0;
        for (int k = 0; k < 11 && first + k < R.nFriends; k++) {
            int i = first + k;
            const net::Friend& f = R.friends[i];
            float y = y0 + 50 + k * 58;
            if (MmHover(x0 + 10, y - 29, x1 - 10, y + 29)) {
                if ((io.MouseDelta.x != 0 || io.MouseDelta.y != 0) && g_invSel != i) { g_invSel = i; MM_SND |= 1; }
                if (io.MouseClicked[0]) { net::RoomInviteFriend(f.id); MM_SND |= 2; }
            }
            bool on = g_invSel == i;
            if (on) MmImg(dl, "MA10_CMN", 83, x0 + 10, y - 28, x1 - x0 - 20, 56, MmGlowCol(P.glow, 0.45f), ImVec2(0, 0), ImVec2(1, 1), true);
            MmText(dl, g_fUiBold, 26, x0 + 30, y, on ? MmC(P.gold) : MmC(P.text), f.name);
            MmText(dl, g_fMenu, 12, x1 - 30, y, f.inGame ? MmC(P.gold) : MmC(P.faint), f.inGame ? "PLAYING ARCANA BLOOD" : "ONLINE", 2);
        }
        MmLine(dl, x0 + 20, y1 - 70, x1 - x0 - 40, 1, MmC(P.acc, 0.5f));
        MmPadButton(dl, x0 + 24, y1 - 54, 'A', MmRGBA(40, 160, 70));
        MmText(dl, g_fMenu, 13, x0 + 68, y1 - 36, MmC(P.text), "Invite");
        MmPadButton(dl, x0 + 180, y1 - 54, 'B', MmRGBA(190, 40, 40));
        MmText(dl, g_fMenu, 13, x0 + 224, y1 - 36, MmC(P.text), "Close");
        MmText(dl, g_fUi, 19, x1 - 24, y1 - 36, MmC(P.faint), "They get a Steam invite to this room", 2);
    }
    MmPageEnd(dl);
}
