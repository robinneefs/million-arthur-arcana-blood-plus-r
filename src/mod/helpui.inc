// ---------------------------------------------------------------------------
// Build 105: Help (main menu "Help", screen 6). Same layout as Settings: tabs on the left
// (A goes into a tab's panel, B back to the tabs, B on the tabs back to the main menu).
//   PLAYING ONLINE      a short how-to
//   CONNECTION TROUBLE  what to try when connecting or playing goes wrong
//   REPORT A PROBLEM    Pack logs, open the folder, open the GitHub page
//   LINKS               the Mizuumi wiki and our GitHub repo (opened in the browser)
//   ABOUT               version, beta note, credits
// ---------------------------------------------------------------------------
static struct {
    int tab = 0, row = 0, focus = 0;
    ImVec2 mouse{-1, -1};
    char note[160] = "";
    double noteUntil = 0;
} HP;
static const char* const HP_TABS[5] = {"PLAYING ONLINE", "CONNECTION TROUBLE", "REPORT A PROBLEM", "LINKS", "ABOUT"};
static const int HP_ROWS[5] = {0, 0, 3, 4, 0};   // tabs with buttons; the others are text only

static void HelpOpen() {
    HP.tab = HP.row = HP.focus = 0;
    MM.screen = 6; MM.roomT = -1; MM.quietFrame = ImGui::GetFrameCount();
}
static void HpOpenUrl(const char* url) {
#ifndef UI_HARNESS
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
    Log("ui: help opened %s", url);
#endif
    snprintf(HP.note, sizeof HP.note, "Opened in your web browser.");
    HP.noteUntil = ImGui::GetTime() + 3.0;
}
static void HpAct() {
    if (HP.tab == 2) {
        if (HP.row == 0) logpack::Start();
        else if (HP.row == 1) logpack::OpenFolder();
        else HpOpenUrl(MOD_REPO_URL "/issues");
    } else if (HP.tab == 3) {
        static const char* const u[4] = {MOD_STEAM_URL, MOD_WIKI_URL, MOD_DISCORD_URL, MOD_REPO_URL};
        HpOpenUrl(u[HP.row & 3]);
    }
    MM_SND |= 2;
}

// word-wrapped text in [x, x + w]; returns the y after the last line
static float HpPara(ImDrawList* dl, ImFont* f, float px, float x, float y, float w, ImU32 col, const char* s, float lh = 0) {
    if (!lh) lh = px * 1.38f;
    char line[512];
    const char* p = s;
    while (*p) {
        int n = 0, lastSp = -1;
        const char* q = p;
        while (*q && *q != '\n' && n < 510) {
            line[n] = *q; line[n + 1] = 0;
            if (*q == ' ') lastSp = n;
            if (MmText(dl, f, px, -9999, -9999, 0, line) > w && lastSp > 0) { n = lastSp; q = p + lastSp; break; }
            n++; q++;
        }
        line[n] = 0;
        MmText(dl, f, px, x, y, col, line);
        y += lh;
        p = q;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
    }
    return y;
}
// a numbered step: gold number in a ring, a bold heading and a wrapped line under it
static float HpStep(ImDrawList* dl, float x, float y, float w, int n, const char* head, const char* body) {
    const MmPal& P = MmP();
    char s[4]; snprintf(s, sizeof s, "%d", n);
    dl->AddCircle(CP(x + 20, y), CV(19), MmC(P.gold), 32, CV(2));
    MmText(dl, g_fMenu, 16, x + 20, y, MmC(P.gold), s, 1);
    MmText(dl, g_fUiBold, 27, x + 60, y, MmC(P.text), head);
    float e = HpPara(dl, g_fUi, 22, x + 60, y + 34, w - 60, MmC(P.dim), body);
    return e + 16;
}
// a tip: small red diamond, bold heading, wrapped line
static float HpTip(ImDrawList* dl, float x, float y, float w, const char* head, const char* body) {
    const MmPal& P = MmP();
    dl->AddQuadFilled(CP(x + 10, y - 9), CP(x + 19, y), CP(x + 10, y + 9), CP(x + 1, y), MmC(P.acc));
    MmText(dl, g_fUiBold, 25, x + 34, y, MmC(P.text), head);
    float e = HpPara(dl, g_fUi, 22, x + 34, y + 32, w - 34, MmC(P.dim), body);
    return e + 12;
}
// the "beta" tag: gold outline pill
static float HpBeta(ImDrawList* dl, float x, float cy, int align = 0) {
    const MmPal& P = MmP();
    const char* s = "BETA";
    float w = MmText(dl, g_fMenu, 13, -9999, -9999, 0, s) + 28;
    if (align == 2) x -= w;
    dl->AddRectFilled(CP(x, cy - 15), CP(x + w, cy + 15), MmC(P.gold, 0.16f), CV(15));
    dl->AddRect(CP(x, cy - 15), CP(x + w, cy + 15), MmC(P.gold), CV(15), 0, CV(1.5f));
    MmText(dl, g_fMenu, 13, x + w / 2, cy, MmC(P.gold), s, 1);
    return w;
}

static void MmHelpFrame() {
    const MmPal& P = MmP();
    if (MM.roomT < 0) MM.roomT = ImGui::GetTime();
    MmT T = MmTimes(MM.roomT);
    ImGuiIO& io = ImGui::GetIO();
    MmNav nav = MmReadNav(false);
    bool click = io.MouseClicked[0];
    bool moved = io.MousePos.x != HP.mouse.x || io.MousePos.y != HP.mouse.y;
    HP.mouse = io.MousePos;
    if (ImGui::GetFrameCount() == MM.quietFrame) { nav = MmNav(); click = false; }
    // ---- input ----
    if (HP.focus == 0) {
        if (nav.b) { MM_SND |= 4; MmGoMenu(); return; }
        if (nav.dy) { HP.tab = (HP.tab + nav.dy + 5) % 5; HP.row = 0; MM_SND |= 1; }
        if ((nav.a || nav.dx > 0) && HP_ROWS[HP.tab]) { HP.focus = 1; HP.row = 0; MM_SND |= 2; }
        nav = MmNav();
    } else if (nav.b) { HP.focus = 0; MM_SND |= 4; nav = MmNav(); }
    int nr = HP_ROWS[HP.tab];
    if (nr && (nav.dy || nav.dx)) { HP.row = (HP.row + nav.dy + nav.dx + nr) % nr; MM_SND |= 1; }
    if (nav.a && nr) HpAct();

    ImDrawList* dl = MmPageBegin("##help", T.tm, T.pulse);
    MmPageTitle(dl, "MAIN MENU", "HELP", MmEase(T, 0.0f, 0.5f));
    float bw = HpBeta(dl, 1860, 74, 2);
    MmText(dl, g_fUi, 22, 1860 - bw - 16, 74, MmC(P.dim), MOD_VERSION_NUM "   Problems are expected, and reports help a lot.", 2);

    // ---- tabs ----
    for (int i = 0; i < 5; i++) {
        float y = 230 + i * 92;
        if (click && MmHover(40, y - 40, 500, y + 40)) { if (HP.tab != i) { HP.tab = i; HP.row = 0; MM_SND |= 1; } HP.focus = 0; click = false; }
        bool on = HP.tab == i;
        if (on) {
            float ga = HP.focus ? 0.22f : 0.5f + 0.12f * T.pulse;
            MmImg(dl, "MA10_CMN", 83, 40, y - 40, 470, 80, MmGlowCol(P.glow, ga), ImVec2(0, 0), ImVec2(1, 1), true);
            float px = 54, py = y;
            dl->AddTriangleFilled(CP(px, py - 13), CP(px + 21, py), CP(px, py + 13), MmC(P.selOut));
            dl->AddTriangleFilled(CP(px + 4, py - 8), CP(px + 15, py), CP(px + 4, py + 8), MmC(P.selFill));
        }
        if (i) MmLine(dl, 90, y - 46, 380, 2, MmC(P.acc, 0.5f));
        float x = on ? 100 : 90;
        float fs = strlen(HP_TABS[i]) > 15 ? 21 : 24;
        MmText(dl, g_fMenu, fs, x, y, on ? MmC(P.selOut) : MmC(P.ink), HP_TABS[i], 0, 3, on ? MmC(P.selOut) : MmC(P.ink));
        MmText(dl, g_fMenu, fs, x, y, on ? MmC(P.selFill) : MmC(P.text), HP_TABS[i], 0, 0, 0, 1);
    }
    auto btnHit = [&](int row, float x0, float y0, float x1, float y1) -> bool {
        if (!MmHover(x0, y0, x1, y1)) return false;
        if (click || (moved && HP.focus)) { if (HP.row != row && HP.focus) MM_SND |= 1; HP.row = row; }
        if (click) HP.focus = 1;
        return click;
    };

    const float X0 = 560, X1 = 1860, Y0 = 170, Y1 = 940;
    const float LX = X0 + 50, LW = X1 - X0 - 100;
    const int SR = HP.focus ? HP.row : -1;
    if (HP.tab == 0) {
        MmSection(dl, X0, Y0, X1, Y1, "PLAYING ONLINE");
        float y = Y0 + 80;
        y = HpStep(dl, LX, y, LW, 1, "Start the game with the Arcana Blood +R launcher",
                   "The launcher keeps the mod up to date and starts the game with it (started from Steam, the game is the plain game). Both players need Steam running and the same version of this mod, shown in the bottom right of the main menu.");
        y = HpStep(dl, LX, y, LW, 2, "Set up your fighter",
                   "On the main menu, pick your character, colour, Support Knights and favourite stage under My setup. It's remembered, so a match starts straight away.");
        y = HpStep(dl, LX, y, LW, 3, "Make a room, or join one",
                   "Create room opens your own room (public or private) and lets you invite Steam friends. Room search lists the public rooms. A friend's invite also shows up in Steam.");
        y = HpStep(dl, LX, y, LW, 4, "Sit at a table and confirm",
                   "Take a free seat next to your opponent. When both of you confirm, the fight starts.");
        y = HpStep(dl, LX, y, LW, 5, "Rematch or go back",
                   "After a fight, pick Rematch to play again, or Back to room to change your setup or opponent.");
    } else if (HP.tab == 1) {
        MmSection(dl, X0, Y0, X1, Y1, "CONNECTION TROUBLE");
        float y = Y0 + 76;
        y = HpTip(dl, LX, y, LW, "Connecting takes long or times out",
                  "Leave the room and try again, and check that Steam is online on both PCs (bottom right of the main menu). You don't need to open any ports: Steam carries the connection.");
        y = HpTip(dl, LX, y, LW, "It says the other player runs a different version",
                  "Both players need the same version of the mod: close the game and open the launcher, it updates the mod. If it says the game files differ, both players: in Steam, right-click the game > Properties > Installed Files > Verify integrity.");
        y = HpTip(dl, LX, y, LW, "The game stutters or jumps a lot",
                  "Raise your input delay a little in Settings > Online (3 or 4 frames). A wired connection, and pausing downloads and streams, also helps.");
        y = HpTip(dl, LX, y, LW, "The connection drops after switching windows",
                  "Alt-tabbing out of the game can drop the connection in this beta. Try to stay in the game window while connecting and fighting.");
        y = HpTip(dl, LX, y, LW, "The match stops with a \"desync\" message",
                  "Both games stopped agreeing on what happened. That's a bug: please pack your logs and report it (Help > Report a problem).");
    } else if (HP.tab == 2) {
        MmSection(dl, X0, Y0, X1, Y1, "REPORT A PROBLEM");
        float y = Y0 + 80;
        y = HpPara(dl, g_fUi, 25, LX, y, LW, MmC(P.text),
                   "This mod is in beta, so problems are expected. Every report helps make it better.") + 20;
        y = HpStep(dl, LX, y, LW, 1, "Pack your logs",
                   "This puts the mod's logs into one zip on your Desktop. Steam IDs, names and your PC name are hidden. Nothing is sent by itself.");
        y = HpStep(dl, LX, y, LW, 2, "Tell us what happened",
                   "Open the GitHub page, start a new issue, say what happened and when, and drag the zip into it. Your opponent's logs help too.");
        y = HpStep(dl, LX, y, LW, 3, "Add a screenshot",
                   "After a crash or an error message, a screenshot of what was on screen always helps. Drag it into the issue with the zip.");
        float by = y + 20;
        if (btnHit(0, LX, by, LX + 330, by + 65)) HpAct();
        if (btnHit(1, LX + 360, by, LX + 690, by + 65)) HpAct();
        if (btnHit(2, LX + 720, by, LX + 1120, by + 65)) HpAct();
        MmButton(dl, LX, by, LX + 330, by + 65, "PACK LOGS", SR == 0, T.pulse);
        MmButton(dl, LX + 360, by, LX + 690, by + 65, "OPEN FOLDER", SR == 1, T.pulse);
        MmButton(dl, LX + 720, by, LX + 1120, by + 65, "OPEN GITHUB PAGE", SR == 2, T.pulse);
        int ls = logpack::g_state;
        if (ls) MmText(dl, g_fUi, 22, LX, by + 110, ls == 3 ? MmC(P.acc) : ls == 2 ? MmC(P.gold) : MmC(P.dim), logpack::g_msg);
    } else if (HP.tab == 3) {
        MmSection(dl, X0, Y0, X1, Y1, "LINKS");
        const char* head[4] = {"Million Arthur: Arcana Blood on Steam", "Mizuumi wiki", "Million Arthur Discord", "This mod on GitHub"};
        const char* sub[4] = {"The original game's store page.", "Characters, moves, combos and the game's systems.",
                              "The community: find players, talk about the game.", "Downloads of new versions, the source code, and known problems."};
        const char* url[4] = {MOD_STEAM_URL, MOD_WIKI_URL, MOD_DISCORD_URL, MOD_REPO_URL};
        for (int i = 0; i < 4; i++) {
            float y = Y0 + 100 + i * 150;
            bool cur = SR == i;
            if (btnHit(i, LX - 20, y - 60, X1 - 30, y + 70)) HpAct();
            if (cur) MmImg(dl, "MA10_CMN", 83, LX - 30, y - 60, X1 - X0 - 40, 130, MmGlowCol(P.glow, 0.40f + 0.1f * T.pulse), ImVec2(0, 0), ImVec2(1, 1), true);
            MmText(dl, g_fUiBold, 30, LX, y - 22, cur ? MmC(P.gold) : MmC(P.text), head[i]);
            MmText(dl, g_fUi, 22, LX, y + 14, MmC(P.dim), sub[i]);
            MmText(dl, g_fUi, 20, LX, y + 44, MmC(P.faint), url[i]);
            MmButton(dl, X1 - 330, y - 32, X1 - 50, y + 32, "OPEN", cur, T.pulse);
        }
        MmText(dl, g_fUi, 21, LX, Y0 + 700, MmC(P.faint), "Links open in your web browser. The game keeps running.");
    } else {
        MmSection(dl, X0, Y0, X1, Y1, "ABOUT");
        float y = Y0 + 80;
        MmText(dl, g_fMenu, 13, LX, y, MmC(P.faint), "VERSION");
        char v[96];
        snprintf(v, sizeof v, "Arcana Blood +R  %s", MOD_VERSION_NUM);
        float vw = MmText(dl, g_fUiBold, 28, LX + 230, y, MmC(P.text), v);
        HpBeta(dl, LX + 250 + vw, y);
        char b[48]; snprintf(b, sizeof b, "build %d", MOD_BUILD);
        MmText(dl, g_fUi, 21, LX + 250 + vw + 110, y, MmC(P.faint), b);
        y += 50;
        MmText(dl, g_fMenu, 13, LX, y, MmC(P.faint), "GAME");
        MmText(dl, g_fUi, 24, LX + 230, y, MmC(P.dim), "Million Arthur: Arcana Blood (Steam)");
        y += 42;
        y = HpPara(dl, g_fUi, 22, LX + 230, y, LW - 230, MmC(P.gold),
                   "This is a beta: expect bugs and rough edges. It's a fan-made mod, not made or supported by the game's publishers.") + 10;
        MmLine(dl, LX, y, LW, 1, MmC(P.acc, 0.6f));
        y += 44;
        MmText(dl, g_fMenu, 13, LX, y, MmC(P.faint), "CREDITS");
        struct { const char* who; const char* what; } cr[] = {
            {"GGenie", "Creator of Arcana Blood +R"},
            {"Dear ImGui", "Omar Cornut and contributors, MIT licence. The menus."},
            {"stb_truetype, stb_rect_pack", "Sean Barrett, public domain. Font drawing."},
            {"Michroma", "Vernon Adams, SIL Open Font License."},
            {"Saira Semi Condensed", "Omnibus-Type, SIL Open Font License."},
            {"Steamworks", "Valve. Rooms and online play through Steam."},
            {"GGPO", "Tony Cannon. The rollback idea this mod follows."},
            {"Mizuumi wiki", "The community's knowledge of the game."},
            {"Million Arthur: Arcana Blood", "Square Enix and Team Arcana, who made the game."},
        };
        for (auto& c : cr) {
            MmText(dl, g_fUiBold, 23, LX + 230, y, MmC(P.text), c.who);
            MmText(dl, g_fUi, 22, LX + 640, y, MmC(P.dim), c.what);
            y += 40;
        }
    }

    // ---- description line + prompts ----
    dl->PushClipRect(ImVec2(0, 0), g_disp, false);
    MmLine(dl, -g_cvO.x / g_cv, 960, g_disp.x / g_cv, 3, MmC(P.acc, 0.95f));
    MmLine(dl, -g_cvO.x / g_cv, 964, g_disp.x / g_cv, 1, MmC(P.gold, 0.6f));
    dl->PopClipRect();
    const char* h = HP.focus ? HP.tab == 3 ? "Up / down picks a link, A opens it." : "Left / right picks a button, A uses it." : HP_ROWS[HP.tab] ? "Pick a tab. A goes to its buttons." : "Pick a tab to read it.";
    if (ImGui::GetTime() < HP.noteUntil) h = HP.note;
    MmText(dl, g_fUi, 26, 60, 994, MmC(P.text), h);
    float x = 60;
    if (HP.focus || HP_ROWS[HP.tab]) {
        MmPadButton(dl, x, 1022, 'A', MmRGBA(40, 160, 70)); MmText(dl, g_fMenu, 13, x + 44, 1040, MmC(P.text), "Select"); x += 170;
    }
    MmPadButton(dl, x, 1022, 'B', MmRGBA(190, 40, 40)); MmText(dl, g_fMenu, 13, x + 44, 1040, MmC(P.text), HP.focus ? "Tabs" : "Back");
    MmPageEnd(dl);
}
