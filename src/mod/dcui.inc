// ===========================================================================
// Build 108: connection trouble, in the house style (menu.inc helpers).
// - DcBanner: a small banner under the fight timer while no packets arrive from
//   the other game ("Waiting for <NAME>... 7"), counting down to the give-up.
// - DcPopup: CONNECTION LOST over the frozen game, with the reason in plain
//   words and A OK.
// ===========================================================================

enum : uint8_t { DCW_LEFT = 0, DCW_CLOSED, DCW_TIMEOUT, DCW_VERSION, DCW_OTHER, DCW_AWAY, DCW_GAME };

static float DcEase(float t, float start, float len) {
    float v = ImClamp((t - start) / len, 0.0f, 1.0f);
    return 1 - (1 - v) * (1 - v) * (1 - v);
}

// a ring of 8 dots, one bright one going round (canvas centre, radius)
static void DcSpinner(ImDrawList* dl, float cx, float cy, float r, float now, float in) {
    const MmPal& P = MmP();
    int head = (int)(now * 10.0f) % 8;
    for (int i = 0; i < 8; i++) {
        float a = i * IM_PI * 2 / 8 - IM_PI / 2;
        int age = (head - i + 8) % 8;
        float k = 1.0f - age / 8.0f;
        dl->AddCircleFilled(CP(cx + cosf(a) * r, cy + sinf(a) * r), CV(2.5f + 2.5f * k),
                            age == 0 ? MmC(P.gold, in) : MmC(P.dim, (0.15f + 0.6f * k) * in), 12);
    }
}

// t: seconds since the packets stopped coming; secsLeft: whole seconds before giving up
static void DcBanner(const char* name, int secsLeft, float t, const char* why = "The connection is not responding") {
    const MmPal& P = MmP();
    float now = (float)ImGui::GetTime(), in = DcEase(t, 0.0f, 0.25f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(CP(0, 0), CP(1920, 1080), true);
    const float W = 760, H = 84, x0 = 960 - W / 2, y0 = 214 - (1 - in) * 20, x1 = x0 + W, y1 = y0 + H;
    MmImg(dl, "MA10_CMN", 83, x0 - 80, y0 - 30, W + 160, H + 60, MmGlowCol(P.glow, 0.35f * in), ImVec2(0, 0), ImVec2(1, 1), true);
    dl->AddRectFilledMultiColor(CP(x0 + 4, y0 + 4), CP(x1 - 4, y1 - 4), MmC(P.bgTop, 0.94f * in), MmC(P.bgTop, 0.94f * in),
                                MmC(P.bgBot, 0.94f * in), MmC(P.bgBot, 0.94f * in));
    MmNine(dl, "MA10_CS_NETWORK", 45, 26, 18, x0, y0, x1, y1, MmRGBA(255, 255, 255, in));
    DcSpinner(dl, x0 + 58, (y0 + y1) * 0.5f, 18, now, in);
    char who[64], n[8];
    snprintf(who, sizeof who, "WAITING FOR %s", name);
    for (char* p = who; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    MmText(dl, g_fMenu, 17, x0 + 100, y0 + 30, MmC(P.text, in), who, 0, 1, MmC(P.ink, in));
    MmText(dl, g_fUi, 24, x0 + 100, y0 + 58, MmC(P.dim, in), why);
    snprintf(n, sizeof n, "%d", secsLeft);
    float pulse = 0.8f + 0.2f * sinf(now * IM_PI * 2);
    MmText(dl, g_fMenu, 40, x1 - 54, (y0 + y1) * 0.5f, MmC(P.selOut, in), n, 1, 4, MmC(P.selOut, in));
    MmText(dl, g_fMenu, 40, x1 - 54, (y0 + y1) * 0.5f, MmC(P.gold, in * pulse), n, 1, 0, 0, 1);
    dl->AddLine(CP(x1 - 104, y0 + 18), CP(x1 - 104, y1 - 18), MmC(P.acc, 0.7f * in), CV(2));
    dl->PopClipRect();
}

// reason line for the pop-up
static void DcReason(char* out, size_t n, uint8_t why, const char* name, bool inMatch) {
    switch (why) {
    case DCW_LEFT:    snprintf(out, n, "%s left the %s.", name, inMatch ? "match" : "room"); break;
    case DCW_CLOSED:  snprintf(out, n, "%s closed the game.", name); break;
    case DCW_TIMEOUT: snprintf(out, n, "The connection to %s stopped responding.", name); break;
    case DCW_VERSION: snprintf(out, n, "%s has a different version of the mod.", name); break;
    case DCW_AWAY:   snprintf(out, n, "%s switched away from the game for too long.", name); break;
    case DCW_GAME:   snprintf(out, n, "%s's game files differ from yours. Both: Steam > Verify game files.", name); break;   // build 119
    default:         snprintf(out, n, "The connection to %s was lost.", name); break;
    }
}

// t: seconds since the pop-up opened; inMatch: a match was on (it ends, OK goes to the main menu)
static const float DC_OK_X0 = 850, DC_OK_Y0 = 636, DC_OK_X1 = 1070, DC_OK_Y1 = 700;   // OK button once in place (canvas)
static void DcPopup(const char* name, uint8_t why, bool inMatch, float t) {
    const MmPal& P = MmP();
    float now = (float)ImGui::GetTime(), fade = DcEase(t, 0.0f, 0.3f), in = DcEase(t, 0.08f, 0.35f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // the frozen game stays visible behind a dark crimson shade
    dl->PushClipRect(ImVec2(0, 0), g_disp, false);
    dl->AddRectFilled(ImVec2(0, 0), g_disp, MmC(P.bgBot, 0.62f * fade));
    dl->PopClipRect();
    dl->PushClipRect(CP(0, 0), CP(1920, 1080), true);
    const float W = 1040, H = 400, x0 = 960 - W / 2, y0 = 340 + (1 - in) * 30, x1 = x0 + W, y1 = y0 + H;
    MmImg(dl, "MA10_CMN", 39, x0 - 160, y0 - 200, W + 320, H + 400, MmGlowCol(P.glow, 0.28f * in), ImVec2(0, 0), ImVec2(1, 1), true);
    dl->AddRectFilledMultiColor(CP(x0 + 6, y0 + 6), CP(x1 - 6, y1 - 6), MmC(P.bgTop, in), MmC(P.bgTop, in), MmC(P.bgBot, in), MmC(P.bgBot, in));
    MmNine(dl, "MA10_CS_NETWORK", 45, 26, 26, x0, y0, x1, y1, MmRGBA(255, 255, 255, in));
    // title
    MmText(dl, g_fMenu, 46, 960, y0 + 82, MmC(P.selOut, in), "CONNECTION LOST", 1, 5, MmC(P.selOut, in));
    MmText(dl, g_fMenu, 46, 960, y0 + 82, MmC(P.gold, in), "CONNECTION LOST", 1, 0, 0, 1.2f);
    MmLine(dl, x0 + 170, y0 + 128, W - 340, 2, MmC(P.acc, 0.9f * in));
    MmLine(dl, x0 + 230, y0 + 133, W - 460, 1, MmC(P.gold, 0.6f * in));
    // reason + what happens next
    char r[160], s[96];
    DcReason(r, sizeof r, why, name, inMatch);
    MmText(dl, g_fUiBold, 36, 960, y0 + 190, MmC(P.text, in), r, 1, 1, MmC(P.ink, in));
    snprintf(s, sizeof s, "%s", inMatch ? "The match has ended. You will go back to the main menu." : "You are no longer connected to this player.");
    MmText(dl, g_fUi, 28, 960, y0 + 242, MmC(P.dim, in), s, 1);
    // OK button
    float pulse = 0.5f + 0.5f * sinf(now * 3.0f);
    const float bw = 220, bx0 = 960 - bw / 2, by0 = y1 - 104, bx1 = bx0 + bw, by1 = by0 + 64;
    MmImg(dl, "MA10_CMN", 83, bx0 - 60, by0 - 18, bw + 120, 100, MmGlowCol(P.glow, (0.4f + 0.15f * pulse) * in), ImVec2(0, 0), ImVec2(1, 1), true);
    dl->AddRectFilled(CP(bx0, by0), CP(bx1, by1), MmC(P.selFill, in), CV(32));
    dl->AddRect(CP(bx0, by0), CP(bx1, by1), MmC(P.selOut, in), CV(32), 0, CV(2));
    // build 120h: badge + OK centred in the button together
    float okw = MmText(dl, g_fMenu, 26, -9999, -9999, 0, "OK"), cx0 = (bx0 + bx1) * 0.5f - (36 + 16 + okw) * 0.5f;
    MmPadButton(dl, cx0, by0 + 14, 'A', MmRGBA(40, 160, 70, in));
    MmText(dl, g_fMenu, 26, cx0 + 52, (by0 + by1) * 0.5f, MmC(P.selOut, in), "OK", 0, 3, MmC(P.selOut, in));
    MmText(dl, g_fMenu, 26, cx0 + 52, (by0 + by1) * 0.5f, MmC(P.selFill, in), "OK", 0, 0, 0, 1);
    dl->PopClipRect();
}
