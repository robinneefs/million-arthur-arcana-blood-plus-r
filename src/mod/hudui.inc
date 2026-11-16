// ===========================================================================
// Build 109: the in-fight overlay in the house style (menu.inc helpers).
// - HudBar: connection info centred at the top, over the timer's upper ring:
//   quality dot, PING, DELAY, ROLLBACK, STUTTERS (+ DESYNC in red, only if any).
// - HudName: each player's name under their life bar, next to the Knight cards.
// ===========================================================================

struct HudInfo {
    int ping;          // ms
    int delay;         // frames
    int rollback;      // biggest rollback in the last second (frames)
    int stutters;      // times this match the game waited for the other player
    int desyncs;
    int quality;       // 0 good, 1 so-so, 2 bad
};

static float HudTextW(ImFont* f, float px, const char* s) {
    if (!f) f = ImGui::GetFont();
    return f->CalcTextSizeA(CV(px * MmEm(f)), FLT_MAX, 0.0f, s).x / g_cv;
}

static const float HUD_LBL = 14, HUD_VAL = 15, HUD_GAP = 8, HUD_SEP = 18;
static void HudBar(const HudInfo& h, float cy) {
    const MmPal& P = MmP();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(CP(0, 0), CP(1920, 1080), true);
    // Build 109: fixed layout. Every value has a slot as wide as its widest value and is
    // right-aligned in it, so nothing moves when the numbers change; a desync note sits
    // outside the plate (right of it) and never shifts the rest.
    char v[4][24];
    const char* lbl[4] = {"PING", "DELAY", "ROLLBACK", "STUTTERS"};
    static const char* const widest[4] = {"888 ms", "88", "88", "888"};
    snprintf(v[0], 24, "%d ms", ImClamp(h.ping, 0, 999));
    snprintf(v[1], 24, "%d", ImClamp(h.delay, 0, 99));
    snprintf(v[2], 24, "%d", ImClamp(h.rollback, 0, 99));
    snprintf(v[3], 24, "%d", ImClamp(h.stutters, 0, 999));
    float slot[4], w = 20;   // dot
    for (int i = 0; i < 4; i++) {
        slot[i] = HudTextW(g_fUiBold, HUD_VAL, widest[i]);
        w += HudTextW(g_fUiBold, HUD_LBL, lbl[i]) + HUD_GAP + slot[i] + (i ? HUD_SEP : 0);
    }
    w += 22;
    const float H = 22, x0 = 960 - w / 2, x1 = 960 + w / 2, y0 = cy - H / 2, y1 = cy + H / 2;
    // dark plate with soft ends and a thin gold line under it
    ImU32 bg = MmC(P.bgBot, 0.86f), clear = MmC(P.bgBot, 0.0f);
    dl->AddRectFilledMultiColor(CP(x0 - 24, y0), CP(x0 + 6, y1), clear, bg, bg, clear);
    dl->AddRectFilled(CP(x0 + 6, y0), CP(x1 - 6, y1), bg);
    dl->AddRectFilledMultiColor(CP(x1 - 6, y0), CP(x1 + 24, y1), bg, clear, clear, bg);
    MmLine(dl, x0 - 12, y1, w + 24, 1, MmC(P.gold, 0.35f));
    float x = x0 + 12;
    static const MmRgb dots[3] = {{90, 205, 100}, {240, 190, 60}, {225, 55, 40}};
    MmRgb dc = dots[h.quality < 0 ? 0 : h.quality > 2 ? 2 : h.quality];
    dl->AddCircleFilled(CP(x + 4, cy), CV(7), MmC(dc, 0.22f), 16);
    dl->AddCircleFilled(CP(x + 4, cy), CV(4.5f), MmC(dc, 0.9f), 16);
    x += 20;
    for (int i = 0; i < 4; i++) {
        if (i) {
            dl->AddLine(CP(x + HUD_SEP / 2 - 1, cy - 6), CP(x + HUD_SEP / 2 - 1, cy + 6), MmC(P.acc, 0.6f), CV(1));
            x += HUD_SEP;
        }
        x += MmText(dl, g_fUiBold, HUD_LBL, x, cy, MmC(P.dim, 0.9f), lbl[i]) + HUD_GAP;
        x += slot[i];
        MmText(dl, g_fUiBold, HUD_VAL, x, cy, MmC(P.text, 0.92f), v[i], 2);
    }
    if (h.desyncs > 0) {
        char d[24];
        snprintf(d, sizeof d, "DESYNC %d", ImClamp(h.desyncs, 0, 99));
        MmText(dl, g_fUiBold, HUD_LBL + 2, x1 + 30, cy + 1, MmRGBA(255, 90, 70), d, 0, 1, MmC(P.ink));
    }
    dl->PopClipRect();
}

// side 0: under player 1's bar (left), 1: under player 2's bar (right)
static void HudName(int side, const char* name) {
    const MmPal& P = MmP();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(CP(0, 0), CP(1920, 1080), true);
    const float cy = 146, H = 28;
    char tag[4];
    snprintf(tag, sizeof tag, "P%d", side + 1);
    float nw = HudTextW(g_fUiBold, 22, name), tw = 32;
    float w = tw + 10 + nw + 26;
    float ax = side == 0 ? 378 : 1542;            // inner edge next to the Knight cards
    float x0 = side == 0 ? ax : ax - w, x1 = side == 0 ? ax + w : ax;
    ImU32 bg = MmC(P.bgBot, 0.65f), clear = MmC(P.bgBot, 0.0f);
    if (side == 0) {
        dl->AddRectFilled(CP(x0, cy - H / 2), CP(x1 - 40, cy + H / 2), bg);
        dl->AddRectFilledMultiColor(CP(x1 - 40, cy - H / 2), CP(x1 + 20, cy + H / 2), bg, clear, clear, bg);
        dl->AddRectFilled(CP(x0, cy - H / 2), CP(x0 + 2, cy + H / 2), MmC(P.acc, 0.8f));
    } else {
        dl->AddRectFilledMultiColor(CP(x0 - 20, cy - H / 2), CP(x0 + 40, cy + H / 2), clear, bg, bg, clear);
        dl->AddRectFilled(CP(x0 + 40, cy - H / 2), CP(x1, cy + H / 2), bg);
        dl->AddRectFilled(CP(x1 - 2, cy - H / 2), CP(x1, cy + H / 2), MmC(P.acc, 0.8f));
    }
    // P1 / P2 badge (gold) + name
    float bx = side == 0 ? x0 + 9 : x1 - 9 - tw;
    dl->AddRectFilled(CP(bx, cy - 9), CP(bx + tw, cy + 9), MmC(P.gold, 0.9f), CV(3));
    MmText(dl, g_fUiBold, 15, bx + tw / 2, cy, MmC(P.ink), tag, 1);
    float nx = side == 0 ? bx + tw + 10 : bx - 10;
    MmText(dl, g_fUiBold, 22, nx, cy, MmC(P.text, 0.95f), name, side == 0 ? 0 : 2, 1, MmC(P.ink));
    dl->PopClipRect();
}
