// ===========================================================================
// Build 107: the after-match screen (Rematch / Back to room), drawn over the
// finished fight while the winner poses, in the house style (menu.inc helpers).
// Two player cards sit where the game's own WINNER / LOSER boxes were, the
// choices run along the bottom like the main menu's entries.
// ===========================================================================

enum : uint8_t { PM_NONE = 0, PM_REMATCH = 1, PM_ROOM = 2 };
struct PmView {
    const char* name[2];      // player 1, player 2
    const char* chara[2];
    int wins[2];              // fights won in this set
    int winner;               // 0 / 1, -1 draw
    uint8_t choice[2];        // PM_*
    int me;                   // which side this PC plays
    int sel;                  // 0 Rematch, 1 Back to room
    int matchNo;
    float t;                  // seconds since the screen opened
    const char* status;       // instead of the description once both have chosen
    int setLen;               // build 113: > 0 and the set is over: only Back to room (build 125: first to setLen wins)
};
static const float PM_Y = 846, PM_XS[2] = {560, 1010}, PM_WS[2] = {350, 470};   // the two choices (canvas)
static const char* const PM_ITEMS[2] = {"REMATCH", "BACK TO ROOM"};
static const char* const PM_HELP[2] = {"Play the same fight again straight away. It starts as soon as you both pick it.",
                                       "Go back to the room to change your setup or play someone else."};

static float PmEase(float t, float start, float len) {
    float v = ImClamp((t - start) / len, 0.0f, 1.0f);
    return 1 - (1 - v) * (1 - v) * (1 - v);
}

// one player's card; side 0 left, 1 right (mirrored)
static void PmCard(ImDrawList* dl, const PmView& v, int side, float in, float now) {
    const MmPal& P = MmP();
    const float W = 723, H = 416, Y0 = 318;   // build 120h: was 456 (more space under WINS than above PLAYER)
    float x0 = side == 0 ? 120 - (1 - in) * 80 : 1080 + (1 - in) * 80, y0 = Y0, x1 = x0 + W, y1 = y0 + H;
    bool won = v.winner == side;
    // panel: dark fill, gold frame (same nine-slice as the My setup panel), soft glow behind the winner's
    if (won) MmImg(dl, "MA10_CMN", 39, x0 - 120, y0 - 160, W + 240, H + 320, MmGlowCol(P.glow, 0.30f * in), ImVec2(0, 0), ImVec2(1, 1), true);
    dl->AddRectFilledMultiColor(CP(x0 + 6, y0 + 6), CP(x1 - 6, y1 - 6), MmC(P.bgTop, in), MmC(P.bgTop, in),
                                MmC(P.bgBot, in), MmC(P.bgBot, in));
    MmNine(dl, "MA10_CS_NETWORK", 45, 26, 26, x0, y0, x1, y1, MmRGBA(255, 255, 255, in));
    float L = x0 + 44, R = x1 - 44;
    // top row: PLAYER n (+ YOU) on the outer side, WINNER on the inner side
    char who[32];
    snprintf(who, sizeof who, "PLAYER %d%s", side + 1, v.me == side ? "  (YOU)" : "");
    MmText(dl, g_fMenu, 17, side == 0 ? L : R, y0 + 50, MmC(P.dim, in), who, side == 0 ? 0 : 2);
    if (won) {
        float gl = 0.75f + 0.25f * sinf(now * 3.0f);
        float wx = side == 0 ? R : L;
        MmText(dl, g_fMenu, 30, wx, y0 + 52, MmC(P.selOut, in), "WINNER", side == 0 ? 2 : 0, 4, MmC(P.selOut, in));
        MmText(dl, g_fMenu, 30, wx, y0 + 52, MmC(P.gold, in * gl + (1 - gl) * 0.6f), "WINNER", side == 0 ? 2 : 0, 0, 0, 1);
    }
    MmLine(dl, L, y0 + 84, R - L, 2, MmC(P.acc, 0.7f * in));
    // name + character
    MmText(dl, g_fUiBold, 46, side == 0 ? L : R, y0 + 140, MmC(P.text, in), v.name[side], side == 0 ? 0 : 2, 1, MmC(P.ink, in));
    MmText(dl, g_fUi, 28, side == 0 ? L : R, y0 + 192, MmC(P.dim, in), v.chara[side], side == 0 ? 0 : 2);
    // set score: a big number on the outer side, "WINS" under it
    char n[8];
    snprintf(n, sizeof n, "%d", v.wins[side]);
    float sx = side == 0 ? L + 50 : R - 50;
    MmText(dl, g_fMenu, 92, sx, y0 + 296, MmC(P.ink, in), n, 1, 5, MmC(P.ink, in));
    MmText(dl, g_fMenu, 92, sx, y0 + 296, won ? MmC(P.gold, in) : MmC(P.text, in), n, 1, 0, 0, 1);
    MmText(dl, g_fMenu, 15, sx, y0 + 366, MmC(P.faint, in), v.wins[side] == 1 ? "WIN" : "WINS", 1);
    // choice, on the inner side next to the score
    float cx0 = side == 0 ? L + 150 : L, cx1 = side == 0 ? R : R - 150, cy = y0 + 310;
    uint8_t c = v.choice[side];
    ImU32 fill = c == PM_REMATCH ? MmC(P.selFill, 0.95f * in) : MmC(P.panel, 0.9f * in);
    ImU32 edge = c == PM_REMATCH ? MmC(P.selOut, in) : c == PM_ROOM ? MmC(P.dim, 0.8f * in) : MmC(P.acc, 0.6f * in);
    dl->AddRectFilled(CP(cx0, cy - 40), CP(cx1, cy + 40), fill, CV(40));
    dl->AddRect(CP(cx0, cy - 40), CP(cx1, cy + 40), edge, CV(40), 0, CV(2));
    ImU32 tc = c == PM_REMATCH ? MmC(P.selOut, in) : c == PM_ROOM ? MmC(P.text, in) : MmC(P.faint, in);
    if (c == PM_REMATCH || c == PM_ROOM) {
        MmText(dl, g_fMenu, 19, (cx0 + cx1) * 0.5f, cy, tc, c == PM_REMATCH ? "WANTS A REMATCH" : "BACK TO ROOM", 1);
    } else {
        // build 120d: CHOOSING stays put (centred as if all three dots were there); the dots come and go after it
        int k = (int)(now * 2.5f) % 4;
        float full = MmText(dl, g_fMenu, 19, -9999, -9999, 0, "CHOOSING...");
        float x = (cx0 + cx1) * 0.5f - full * 0.5f;
        float w = MmText(dl, g_fMenu, 19, x, cy, tc, "CHOOSING");
        if (k) MmText(dl, g_fMenu, 19, x + w, cy, tc, &"..."[3 - k]);
    }
}

static void PmDraw(const PmView& v) {
    const MmPal& P = MmP();
    float t = v.t, now = (float)ImGui::GetTime();
    float pulse = 0.5f + 0.5f * sinf(now * 0.8f);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    // the finished fight stays visible: a light crimson shade over it, darker at the top and bottom
    float fade = PmEase(t, 0.0f, 0.5f);
    dl->PushClipRect(ImVec2(0, 0), g_disp, false);
    dl->AddRectFilled(ImVec2(0, 0), g_disp, MmC(P.bgBot, 0.28f * fade));
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(g_disp.x, CP(0, 300).y), MmC(P.bgBot, 0.85f * fade), MmC(P.bgBot, 0.85f * fade), MmC(P.bgBot, 0), MmC(P.bgBot, 0));
    dl->AddRectFilledMultiColor(ImVec2(0, CP(0, 780).y), g_disp, MmC(P.shade, 0), MmC(P.shade, 0), MmC(P.shade, 0.92f * fade), MmC(P.shade, 0.92f * fade));
    dl->PopClipRect();
    dl->PushClipRect(CP(0, 0), CP(1920, 1080), true);

    // title: MATCH OVER, the winner under it
    {
        float in = PmEase(t, 0.1f, 0.5f);
        char top[48];   // build 125: SET OVER, first to N, once someone has won the set
        if (v.setLen > 0) snprintf(top, sizeof top, "SET OVER  \xC2\xB7  FIRST TO %d", v.setLen);
        else snprintf(top, sizeof top, "MATCH OVER");
        MmText(dl, g_fMenu, 20, 960, 70 - (1 - in) * 20, MmC(P.dim, in), top, 1);
        MmLine(dl, 660, 98, 600, 2, MmC(P.acc, 0.9f * in));
        MmLine(dl, 710, 102, 500, 1, MmC(P.gold, 0.6f * in));
        char w[64];
        int sw = v.wins[0] > v.wins[1] ? 0 : 1;   // build 125: set winner
        if (v.setLen > 0) snprintf(w, sizeof w, "%s WINS %d - %d", v.name[sw], v.wins[sw], v.wins[1 - sw]);
        else if (v.winner < 0) snprintf(w, sizeof w, "DRAW");
        else snprintf(w, sizeof w, "%s WINS", v.name[v.winner]);
        for (char* p = w; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        float s = 0.9f + 0.1f * in;
        MmImg(dl, "MA10_CMN", 39, 960 - 520, 60, 1040, 260, MmGlowCol(P.glow, 0.35f * in), ImVec2(0, 0), ImVec2(1, 1), true);
        MmText(dl, g_fMenu, 54 * s, 960, 172, MmC(P.selOut, in), w, 1, 6, MmC(P.selOut, in));
        MmText(dl, g_fMenu, 54 * s, 960, 172, MmC(P.gold, in), w, 1, 0, 0, 1.5f);
    }

    PmCard(dl, v, 0, PmEase(t, 0.2f, 0.5f), now);
    PmCard(dl, v, 1, PmEase(t, 0.28f, 0.5f), now);

    // the two choices, side by side above the description line
    {
        const float Y = PM_Y, *XS = PM_XS, *WS = PM_WS;
        for (int i = 0; i < 2; i++) {
            if (v.setLen > 0 && i == 0) {   // build 113: set over, no Rematch
                float in = PmEase(t, 0.4f, 0.45f);
                char s[48];
                snprintf(s, sizeof s, "SET OVER");
                MmText(dl, g_fMenu, 22, XS[0] + WS[0] - 60, Y + 28, MmC(P.faint, in), s, 2);
                continue;
            }
            float in = PmEase(t, 0.4f + 0.08f * i, 0.45f);
            bool on = v.sel == i;
            float h = on ? 1.0f : 0.0f;
            float x = XS[i], y = Y + (1 - in) * 30;
            if (h > 0.01f) {
                MmImg(dl, "MA10_CMN", 83, x - 70, y - 16, WS[i] + 140, 88, MmGlowCol(P.glow, (0.50f + 0.12f * pulse) * h * in), ImVec2(0, 0), ImVec2(1, 1), true);
                float bob = sinf(now * 5.0f) * 4.0f;
                float px = x - 46 + bob, py = y + 28;
                dl->AddTriangleFilled(CP(px, py - 15), CP(px + 24, py), CP(px, py + 15), MmC(P.selOut, in));
                dl->AddTriangleFilled(CP(px + 4, py - 9), CP(px + 17, py), CP(px + 4, py + 9), MmC(P.selFill, in));
            }
            ImU32 fill = on ? MmC(P.selFill, in) : MmC(P.text, in), oc = on ? MmC(P.selOut, in) : MmC(P.ink, in);
            MmText(dl, g_fMenu, 31, x, y + 28, oc, PM_ITEMS[i], 0, 4, oc);
            MmText(dl, g_fMenu, 31, x, y + 28, fill, PM_ITEMS[i], 0, 0, 0, 1);
        }
    }

    // description line + button prompts (as on the main menu)
    dl->PushClipRect(ImVec2(0, 0), g_disp, false);
    MmLine(dl, -g_cvO.x / g_cv, 948, g_disp.x / g_cv, 3, MmC(P.acc, 0.95f));
    MmLine(dl, -g_cvO.x / g_cv, 952, g_disp.x / g_cv, 1, MmC(P.gold, 0.6f));
    dl->PopClipRect();
    const char* help = v.status ? v.status : v.setLen > 0 ? "The set is over. Back to the room, then READY for the next set." : PM_HELP[v.sel];
    MmText(dl, g_fUi, 30, 112, 986, MmC(P.bgBot, 0.6f), help);
    MmText(dl, g_fUi, 30, 110, 984, v.status ? MmC(P.gold) : MmC(P.text), help);
    if (!v.status) {
        MmPadButton(dl, 110, 1022, 'A', MmRGBA(40, 160, 70));
        MmText(dl, g_fMenu, 15, 156, 1040, MmC(P.text), "Select");
    }
    if (!v.status && v.choice[v.me] != PM_NONE) {
        MmPadButton(dl, 290, 1022, 'B', MmRGBA(200, 50, 40));
        MmText(dl, g_fMenu, 15, 336, 1040, MmC(P.text), "Change my mind");
    }
    char m[32];
    snprintf(m, sizeof m, "MATCH %d", v.matchNo);
    MmText(dl, g_fMenu, 15, 1810, 1040, MmC(P.faint), m, 2);
    dl->PopClipRect();
}

#ifndef UI_HARNESS
// Build 107: the after-match screen in the game (drawing thread): data from the lobby,
// input from this PC's controller (g_postNav, read on the game thread), keys and mouse.
static struct Pm { int sel = 0; uint32_t prevNav = 0; double openT = 0; bool wasOn = false; } PM;
static void PmFrame() {
    double now = ImGui::GetTime();
    if (!PM.wasOn) { PM.wasOn = true; PM.openT = now; PM.sel = 0; PM.prevNav = 0xFFFFFFFFu; }   // buttons held at the end of the fight don't count
    static char nm[2][32];
    PmView v{};
    EnterCriticalSection(&g_lobbyLock);
    LbSide& me = LB.me;
    const LbSide& them = LB.them;
    int my = N.side == 1 ? 1 : 0;
    snprintf(nm[my], 32, "%s", me.name[0] ? me.name : "You");
    snprintf(nm[1 - my], 32, "%s", them.name[0] ? them.name : "Opponent");
    for (int i = 0; i < 2; i++) {
        v.name[i] = nm[i];
        v.chara[i] = LbCharName(LB.s.chara[i]);
        v.wins[i] = (int)LB.setWins[i];
    }
    v.winner = LB.lastWinner;
    v.me = my;
    v.matchNo = (int)LB.postMatch;
    auto map = [](const LbSide& x) -> uint8_t {
        if (x.choiceMatch != LB.postMatch) return PM_NONE;
        return x.choice == LCH_REMATCH ? PM_REMATCH : (x.choice == LCH_LOBBY || x.choice == LCH_SETUP) ? PM_ROOM : PM_NONE;
    };
    v.choice[my] = map(me);
    v.choice[1 - my] = LB.haveThem ? map(them) : PM_NONE;
    bool open = me.phase == LP_POST;                  // still choosing (not agreed yet)
    // build 125: first to N wins (was: N games played); draws count for nobody
    if (g_roomCfg.on && g_roomCfg.setLen > 0 && ImMax(v.wins[0], v.wins[1]) >= g_roomCfg.setLen) { v.setLen = g_roomCfg.setLen; PM.sel = 1; }
    if (!open) v.status = me.phase == LP_MATCH ? "You both picked Rematch. Here we go!" : "Back to the room...";
    // input
    if (open) {
        ImGuiIO& io = ImGui::GetIO();
        uint32_t nb = g_postNav.load(), fresh = nb & ~PM.prevNav;
        PM.prevNav = nb;
        int move = 0;
        bool act = false, back = false;
        if (fresh & (PB_LEFT | PB_UP)) move = -1;
        if (fresh & (PB_RIGHT | PB_DOWN)) move = 1;
        if (fresh & PB_A) act = true;
        if (fresh & PB_B) back = true;
        if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_LeftArrow]) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_UpArrow])) move = -1;
        if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_RightArrow]) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_DownArrow])) move = 1;
        if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Enter]) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Space])) act = true;
        if (ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Escape]) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Backspace])) back = true;
        {   // build 117: clicking the A / B prompts at the bottom, right click = B
            char pc = MmTakePadClick();
            if (pc == 'A') act = true;
            if (pc == 'B' || io.MouseClicked[1]) back = true;
        }
        if (move && !v.setLen) { PM.sel = (PM.sel + 1) % 2; MM_SND |= 1; }
        if (io.MousePos.x > -FLT_MAX / 2)
            for (int i = 0; i < 2; i++)
                if ((!v.setLen || i == 1) && ImGui::IsMouseHoveringRect(CP(PM_XS[i] - 60, PM_Y - 16), CP(PM_XS[i] + PM_WS[i] + 20, PM_Y + 72), false)) {
                    if ((io.MouseDelta.x != 0 || io.MouseDelta.y != 0) && PM.sel != i) { PM.sel = i; MM_SND |= 1; }   // build 118: sounds
                    if (io.MouseClicked[0]) { PM.sel = i; act = true; }
                }
        if (now - PM.openT < 0.5) act = back = false;   // the screen has to be there first
        if (act) {
            uint8_t c = PM.sel == 0 ? LCH_REMATCH : LCH_LOBBY;
            if (me.choice != c || me.choiceMatch != LB.postMatch) {
                MM_SND |= 2;
                me.choice = c;
                me.choiceMatch = LB.postMatch;
                LbChanged();
                Log("ui: after-match screen: picked %s", LB_CHOICE_NAME[c]);
            }
        } else if (back && me.choice != LCH_NONE && me.choiceMatch == LB.postMatch) {
            me.choice = LCH_NONE;
            MM_SND |= 4;
            LbChanged();
            Log("ui: after-match screen: choice taken back");
        }
    }
    v.sel = PM.sel;
    LeaveCriticalSection(&g_lobbyLock);
    v.t = (float)(now - PM.openT);
    PmDraw(v);
}
#endif
