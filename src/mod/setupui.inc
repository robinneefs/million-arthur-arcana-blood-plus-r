// ---------------------------------------------------------------------------
// Build 102: My setup editor (main menu Y, or a click on the My setup panel).
// One screen: character grid, colour, stage (left), a preview (middle) and
// the assist presets (right); A on a preset opens the assist picker with the
// game's full assist card. Saved in rollback.ini [setup] and used for every match.
//
// Game data used (worked out 2026-11-16, see the tech doc):
//   MA10_CSEL  portraits per character id (SU_PORTRAIT), 48 random portrait,
//              6-13 the 8 stage pictures (stage ids 00-07), 14 random stage
//   MA10_KCSTMZ  assist banners per assist id (SU_BANNER)
//   MA10_KSEL  card pieces: frames 2-5, type labels 11, element icons 12,
//              hearts 13, stars 14, cost 18, stat labels 22-25
//   act\chara  fight sprite picture 0 (standing pose) in the 21 colours (art::GetChara)
//   Movie\MA10_CutIn_P%02d_A  assist art on the card (art::GetStill)
//   the game's assist table (exe 0x783998, 0x54 bytes per assist): HP bonus,
//   cost, element, type and three stats in half stars (SU_STATS below is a copy)
// ---------------------------------------------------------------------------
static const char* const SU_CHARS[13] = {
    "Twinblade Arthur", "Blade-Protector Arthur", "Thief Arthur", "Zechs Siegfried", "Hybrid Nimue",
    "Type II Bisclavret", "Void Eternal Flame", "Unique Snow White", "Otherworldly Riesz", "Iai Arthur",
    "Otherworldly Koume Sakiyama", "Wildcat Arthur", "Otherworldly Iori Yagami"};
static const char* const SU_ASSISTS[31] = {
    "Clone Techno-Smith Arthur", "Clone Magic Guild Arthur", "Clone Fay", "Clone Lif\xC3\xA9" "e", "Clone Elle",
    "Type I Lancelot", "Type I Gawain", "Type I Galahad", "Ruler Supreme Guinevere", "Type I Mordred",
    "Monarch Constantine", "Type II Peridod", "Clone Mercenary Arthur", "Clone Merchant Arthur", "Clone Diva Arthur",
    "Clone Scathach", "Clone Uathach", "Clone Aife", "Clone Cuchulainn", "Clone Pharsalia",
    "Summer Evaine", "Supporter Enide", "Melancholy Princess Kaguya", "Supporter Claire", "Type II Tor",
    "Type II Balin", "Unique Little Grey", "Foible Elle", "Clone Azia Kriemhild", "Otherworldly Hawkeye",
    "Otherworldly Akira Ono"};
// MA10_CSEL portrait of each character id; MA10_KCSTMZ banner of each assist id
// (matched against the characters' and assists' cut-in art, 2026-11-16)
static const int SU_PORTRAIT[13] = {44, 46, 47, 42, 37, 41, 40, 39, 38, 43, 36, 35, 45};
static const int SU_BANNER[31] = {62, 53, 35, 51, 60, 45, 54, 63, 31, 49, 56, 59, 46, 33, 64, 65,
                                  34, 41, 50, 58, 52, 61, 40, 47, 55, 44, 32, 43, 57, 48, 42};
static inline int SuAssistSprite(int a) { return SU_BANNER[a >= 0 && a < 31 ? a : 0]; }
static const int SU_NCHAR = 13, SU_NSTAGE = 8, SU_NPRESET = 8;
static const int SU_RANDOM = 0xFF;

// the game's assist table: element 1 fire 2 water 3 wind 4 light, type 1 attack
// 2 support 3 sabotage, HP bonus, then ATTACK / SPEED / RANGE in half stars (0-6), cost 1-3
// (all 31 checked against the game's cards, 2026-11-16)
struct SuStats { int elem, type, hp, atk, spd, rng, cost; };
static const SuStats SU_STATS[31] = {
    {3, 1, 1500, 3, 6, 2, 2}, {2, 1, 1500, 3, 5, 5, 2}, {1, 1, 1000, 2, 3, 4, 1}, {3, 1, 1000, 2, 3, 4, 1}, {2, 1, 1000, 2, 3, 4, 1},
    {2, 3, 1500, 4, 1, 3, 2}, {1, 1, 1500, 4, 2, 2, 2}, {3, 1, 1500, 3, 3, 5, 2}, {4, 3, 1800, 2, 6, 1, 3}, {1, 1, 2000, 6, 3, 6, 3},
    {2, 3, 1300, 4, 2, 3, 2}, {2, 1, 1800, 5, 3, 6, 3}, {1, 1, 1500, 4, 4, 4, 2}, {4, 2, 1500, 0, 1, 0, 2}, {3, 2, 1300, 3, 4, 1, 2},
    {3, 1, 1300, 3, 3, 6, 2}, {3, 1, 2000, 6, 3, 3, 3}, {3, 1, 1500, 4, 5, 5, 2}, {1, 1, 2000, 6, 2, 6, 3}, {2, 1, 2000, 6, 3, 6, 3},
    {2, 1, 800, 2, 3, 6, 1},  {3, 1, 800, 3, 4, 1, 1},  {3, 2, 1800, 2, 5, 6, 3}, {1, 3, 1300, 4, 3, 4, 2}, {2, 1, 1500, 4, 4, 4, 2},
    {1, 1, 1500, 4, 4, 4, 2}, {4, 1, 800, 1, 1, 6, 1},  {1, 1, 800, 2, 2, 3, 1},  {2, 1, 1500, 3, 5, 2, 2}, {1, 1, 1500, 3, 2, 6, 2},
    {4, 3, 1300, 3, 1, 4, 2}};
// hearts (half hearts 1-6) from the HP bonus, like the game (thresholds at exe 0xE399C8)
static int SuHearts(int hp) {
    static const int thr[6] = {334, 667, 1000, 1333, 1666, 1999};
    int r = 0;
    for (int i = 0; i < 6; i++) if (hp > thr[i]) r = i;
    return r + 1;
}

struct SuPreset { char name[24]; int a[3]; };
static struct {
    bool loaded = false;
    int sec = 0;                    // 0 character, 1 colour, 2 stage, 3 presets
    int chara = 2, color = 0, stage = 0, inUse = 0;   // chara / stage: SU_RANDOM = random
    int charCur = 2, presetCur = 0;
    int pick = -1;                  // assist picker: slot being changed (0-2), -1 closed
    int pickSel = 0;
    int rename = -1;                // preset being renamed, -1 none
    bool focusName = false;
    int renameFrame = 0;            // frame the rename started (the field gets the keys a frame or two later)
    bool dirty = false;
    SuPreset p[SU_NPRESET];
} SU;

// ---- saving: rollback.ini [setup] ----
#ifndef UI_HARNESS
static void SuIniPath(wchar_t* ini) { _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir()); }
static void SuIniPut(const wchar_t* key, const char* utf8) {
    wchar_t ini[MAX_PATH], w[128];
    SuIniPath(ini);
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 128)) w[0] = 0;
    WritePrivateProfileStringW(L"setup", key, w, ini);
}
static bool SuIniGet(const wchar_t* key, char* out, int cap) {
    wchar_t ini[MAX_PATH], w[128];
    SuIniPath(ini);
    GetPrivateProfileStringW(L"setup", key, L"\x01", w, 128, ini);
    if (w[0] == 1) return false;
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, nullptr, nullptr)) out[0] = 0;
    return true;
}
#endif
static void SuDefaults() {
    static const int def[SU_NPRESET][3] = {{0, 1, 2}, {5, 6, 7}, {8, 9, 10}, {12, 13, 14}, {15, 16, 17}, {20, 21, 22}, {24, 25, 26}, {28, 29, 30}};
    for (int i = 0; i < SU_NPRESET; i++) {
        snprintf(SU.p[i].name, sizeof SU.p[i].name, "Preset %d", i + 1);
        for (int k = 0; k < 3; k++) SU.p[i].a[k] = def[i][k];
    }
}
// keep a preset valid: assists 0-30, each one only once
static void SuFixPreset(SuPreset& p) {
    for (int k = 0; k < 3; k++) {
        if (p.a[k] < 0 || p.a[k] >= 31) p.a[k] = 0;
        for (int j = 0; j < k; j++)
            if (p.a[j] == p.a[k]) {
                int v = 0;
                while (v == p.a[0] || v == p.a[1] || v == p.a[2]) v++;
                p.a[k] = v;
                break;
            }
    }
    if (!p.name[0]) snprintf(p.name, sizeof p.name, "Preset");
}
static void SuApply();
static void SuLoad() {
    if (SU.loaded) return;
    SU.loaded = true;
    SuDefaults();
#ifndef UI_HARNESS
    char v[96];
    if (SuIniGet(L"character", v, sizeof v)) SU.chara = !_stricmp(v, "random") ? SU_RANDOM : atoi(v);
    if (SuIniGet(L"colour", v, sizeof v)) SU.color = atoi(v) - 1;
    if (SuIniGet(L"stage", v, sizeof v)) SU.stage = !_stricmp(v, "random") ? SU_RANDOM : atoi(v) - 1;
    if (SuIniGet(L"preset_in_use", v, sizeof v)) SU.inUse = atoi(v) - 1;
    for (int i = 0; i < SU_NPRESET; i++) {
        wchar_t k[32];
        _snwprintf(k, 32, L"preset%d_name", i + 1);
        if (SuIniGet(k, v, sizeof v)) snprintf(SU.p[i].name, sizeof SU.p[i].name, "%s", v);
        _snwprintf(k, 32, L"preset%d", i + 1);
        int a[3];
        if (SuIniGet(k, v, sizeof v) && sscanf(v, "%d,%d,%d", &a[0], &a[1], &a[2]) == 3)
            for (int j = 0; j < 3; j++) SU.p[i].a[j] = a[j];
    }
#endif
    if (SU.chara != SU_RANDOM && (SU.chara < 0 || SU.chara >= SU_NCHAR)) SU.chara = 2;
    if (SU.color < 0 || SU.color > 20) SU.color = 0;
    if (SU.stage != SU_RANDOM && (SU.stage < 0 || SU.stage >= SU_NSTAGE)) SU.stage = 0;
    if (SU.inUse < 0 || SU.inUse >= SU_NPRESET) SU.inUse = 0;
    for (auto& p : SU.p) SuFixPreset(p);
    SU.charCur = SU.chara == SU_RANDOM ? SU_NCHAR : SU.chara;
    SU.presetCur = SU.inUse;
    SuApply();
}
static void SuSave() {
#ifndef UI_HARNESS
    char v[96];
    if (SU.chara == SU_RANDOM) snprintf(v, sizeof v, "random"); else snprintf(v, sizeof v, "%d", SU.chara);
    SuIniPut(L"character", v);
    snprintf(v, sizeof v, "%d", SU.color + 1); SuIniPut(L"colour", v);
    if (SU.stage == SU_RANDOM) snprintf(v, sizeof v, "random"); else snprintf(v, sizeof v, "%d", SU.stage + 1);
    SuIniPut(L"stage", v);
    snprintf(v, sizeof v, "%d", SU.inUse + 1); SuIniPut(L"preset_in_use", v);
    for (int i = 0; i < SU_NPRESET; i++) {
        wchar_t k[32];
        _snwprintf(k, 32, L"preset%d_name", i + 1); SuIniPut(k, SU.p[i].name);
        _snwprintf(k, 32, L"preset%d", i + 1);
        snprintf(v, sizeof v, "%d,%d,%d", SU.p[i].a[0], SU.p[i].a[1], SU.p[i].a[2]); SuIniPut(k, v);
    }
    char sv[16];
    if (SU.stage == SU_RANDOM) snprintf(sv, sizeof sv, "random"); else snprintf(sv, sizeof sv, "%02d", SU.stage + 1);
    Log("setup: saved (character %s, colour %d, stage %s, %s: knights %02X/%02X/%02X)",
        SU.chara == SU_RANDOM ? "random" : SU_CHARS[SU.chara], SU.color + 1, sv, SU.p[SU.inUse].name,
        SU.p[SU.inUse].a[0], SU.p[SU.inUse].a[1], SU.p[SU.inUse].a[2]);
#endif
    SU.dirty = false;
}
// the setup becomes this PC's picks for the next match
static void SuApply() {
#ifndef UI_HARNESS
    EnterCriticalSection(&g_lobbyLock);
    LbPicks& p = LB.me.p;
    p.chara = SU.chara == SU_RANDOM ? LB_RANDOM : (uint8_t)SU.chara;
    p.color = (uint8_t)SU.color;
    for (int k = 0; k < 3; k++) p.assist[k] = (uint8_t)SU.p[SU.inUse].a[k];
    p.stage = SU.stage == SU_RANDOM ? LB_RANDOM : (uint8_t)SU.stage;
    LbChanged();
    LeaveCriticalSection(&g_lobbyLock);
#endif
}

// ---- drawing pieces ----
// a picture card with a gold frame when picked, glow when the cursor is on it
static void SuCard(ImDrawList* dl, const char* file, int spr, float x, float y, float w, float h, bool picked, bool cursor, float pulse) {
    const MmPal& P = MmP();
    if (cursor) MmImg(dl, "MA10_CMN", 39, x - 30, y - 30, w + 60, h + 60, MmGlowCol(P.glow, 0.35f + 0.2f * pulse), ImVec2(0, 0), ImVec2(1, 1), true);
    dl->AddRectFilled(CP(x, y), CP(x + w, y + h), MmC(P.bgBot, 1.0f), CV(4));
    MmImg(dl, file, spr, x, y, w, h, picked || cursor ? IM_COL32_WHITE : MmRGBA(180, 160, 160));
    if (picked) {
        dl->AddRect(CP(x - 2, y - 2), CP(x + w + 2, y + h + 2), MmC(P.gold), CV(5), 0, CV(4));
        dl->AddRect(CP(x + 3, y + 3), CP(x + w - 3, y + h - 3), MmC(P.selOut, 0.9f), CV(3), 0, CV(1.5f));
    } else dl->AddRect(CP(x, y), CP(x + w, y + h), MmC(cursor ? P.text : P.acc, cursor ? 0.9f : 0.55f), CV(4), 0, CV(cursor ? 3 : 1.5f));
}
static void SuTex(ImDrawList* dl, void* tex, float x, float y, float w, float h, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1), ImU32 col = IM_COL32_WHITE) {
    if (tex) dl->AddImage((ImTextureID)tex, CP(x, y), CP(x + w, y + h), uv0, uv1, col);
}
// a row of three hearts / stars: sheets of 2 columns x 4 rows (left column 0-3 whole, right column 0.5-2.5)
static void SuPips(ImDrawList* dl, int spr, int halves, float x, float y, float w, float h) {
    if (halves < 0) halves = 0;
    if (halves > 6) halves = 6;
    int whole = halves / 2, half = halves % 2;
    float cu = half ? 0.5f : 0.0f, rv = whole / 4.0f;
    MmImg(dl, "MA10_KSEL", spr, x, y, w, h, IM_COL32_WHITE, ImVec2(cu, rv), ImVec2(cu + 0.5f, rv + 0.25f));
}
// the game's Support Knight card, put together from its own pieces (MA10_KSEL) and the knight's
// cut-in art, laid out like the game's card: name across the top, element and cost in the
// left column, type label and stats at the bottom (ATTACK / SPEED / RANGE left, HP BONUS right)
static void SuCardDraw(ImDrawList* dl, float x, float y, float sc, int a) {
    const MmPal& P = MmP();
    const SuStats& st = SU_STATS[a];
    static const int frameOf[4] = {4, 3, 2, 5};            // fire orange, water blue, wind green, light purple
    int el = (st.elem >= 1 && st.elem <= 4 ? st.elem : 1) - 1, ty = (st.type >= 1 && st.type <= 3 ? st.type : 1) - 1;
    int cost = st.cost >= 1 && st.cost <= 3 ? st.cost : 1;
    float W = 356 * sc, H = 487 * sc;
    auto X = [&](float v) { return x + v * sc; };
    auto Y = [&](float v) { return y + v * sc; };
    dl->AddRectFilled(CP(X(4), Y(4)), CP(X(352), Y(483)), MmRGBA(16, 10, 12), CV(4));
    // art: the cut-in picture, cropped to the card
    dl->PushClipRect(CP(X(6), Y(6)), CP(X(350), Y(481)), true);
    char mv[32]; snprintf(mv, sizeof mv, "MA10_CutIn_P%02d_A", a);
    ImVec2 ts; void* tex = art::GetStill(mv, &ts);
    if (tex && ts.y > 0) {
        float ah = 481 * sc * 1.0f, aw = ah * ts.x / ts.y;
        SuTex(dl, tex, x + W / 2 - aw * 0.40f, Y(20), aw, ah);
    }
    // name strip at the top, dark panel at the bottom
    dl->AddRectFilled(CP(X(88), Y(10)), CP(X(346), Y(46)), MmRGBA(12, 8, 10, 0.88f));
    dl->AddRectFilledMultiColor(CP(X(6), Y(318)), CP(X(350), Y(352)), MmRGBA(10, 6, 8, 0), MmRGBA(10, 6, 8, 0), MmRGBA(10, 6, 8, 0.88f), MmRGBA(10, 6, 8, 0.88f));
    dl->AddRectFilled(CP(X(6), Y(352)), CP(X(350), Y(481)), MmRGBA(10, 6, 8, 0.88f));
    dl->PopClipRect();
    MmImg(dl, "MA10_KSEL", frameOf[el], x, y, W, H);
    size_t nl = strlen(SU_ASSISTS[a]);
    float npx = nl > 17 ? 21.0f * 17 / nl : 21.0f;
    MmText(dl, g_fUiBold, npx * sc / 0.8f, X(217), Y(28), MmC(P.text), SU_ASSISTS[a], 1, 1.5f, MmC(P.ink));
    // element icon centred in the left column (2x2 sheet of 81 x 82), cost under it (3 columns of 57 x 58)
    float ex = (el % 2) * 0.5f, ey = (el / 2) * 0.5f;
    MmImg(dl, "MA10_KSEL", 12, X(10), Y(14), 68 * sc, 69 * sc, IM_COL32_WHITE, ImVec2(ex, ey), ImVec2(ex + 0.5f, ey + 0.5f));
    MmImg(dl, "MA10_KSEL", 18, X(13), Y(90), 62 * sc, 63 * sc, IM_COL32_WHITE, ImVec2((cost - 1) / 3.0f, 0), ImVec2(cost / 3.0f, 1));
    // type label (rows of 300 x 42, with its side lines)
    MmImg(dl, "MA10_KSEL", 11, X(58), Y(338), 240 * sc, 33.6f * sc, IM_COL32_WHITE, ImVec2(0, ty / 3.0f), ImVec2(1, (ty + 1) / 3.0f));
    // stats: labels at their own size, hearts (88 x 28 per row) and stars (77 x 30 per row) unstretched
    const int lab[3] = {23, 25, 24};                       // ATTACK, SPEED, RANGE
    const int val[3] = {st.atk, st.spd, st.rng};
    for (int r = 0; r < 3; r++) {
        float ry = Y(382 + r * 30);
        ImVec2 ls(86, 29); art::Get("MA10_KSEL", lab[r], &ls);
        MmImg(dl, "MA10_KSEL", lab[r], X(22), ry, ls.x * sc * 0.78f, ls.y * sc * 0.78f);
        SuPips(dl, 14, val[r], X(106), ry - 1 * sc, 77 * 0.8f * sc, 30 * 0.8f * sc);
    }
    ImVec2 hs(108, 29); art::Get("MA10_KSEL", 22, &hs);
    MmImg(dl, "MA10_KSEL", 22, X(214), Y(382), hs.x * sc * 0.78f, hs.y * sc * 0.78f);
    SuPips(dl, 13, SuHearts(st.hp), X(214), Y(410), 88 * 1.0f * sc, 28 * 1.0f * sc);
}
// the head of the character in one colour (for the colour row)
static void SuHead(ImDrawList* dl, int chara, int colour, float x, float y, float w, float h) {
    float hb[4] = {0.3f, 0, 0.64f, 0.2f};
    void* tex = art::GetChara(chara, colour, nullptr, hb);
    SuTex(dl, tex, x, y, w, h, ImVec2(hb[0], hb[1]), ImVec2(hb[2], hb[3]));
}

// ---- input ----
static void SuCharMove(int dx, int dy) {   // 7 x 2 grid, 14 = random
    int c = SU.charCur, col = c % 7, row = c / 7;
    if (dx) {
        col += dx;
        if (col > 6) { SU.sec = 3; return; }
        if (col < 0) col = 6;
    }
    if (dy) {
        row += dy;
        if (row > 1) { SU.sec = 1; return; }
        if (row < 0) row = 1;
    }
    SU.charCur = row * 7 + col;
    SU.chara = SU.charCur >= SU_NCHAR ? SU_RANDOM : SU.charCur;
    SU.dirty = true;
}
static void SuColorMove(int dx, int dy) {   // rows of 11 and 10
    int c = SU.color, row = c >= 11 ? 1 : 0, col = row ? c - 11 : c;
    if (dx) {
        col += dx;
        int n = row ? 10 : 11;
        if (col >= n) { SU.sec = 3; return; }
        if (col < 0) col = n - 1;
    }
    if (dy) {
        row += dy;
        if (row < 0) { SU.sec = 0; return; }
        if (row > 1) { SU.sec = 2; return; }
        if (row == 1 && col > 9) col = 9;
    }
    SU.color = row ? 11 + col : col;
    SU.dirty = true;
}
static void SuStageMove(int dx, int dy) {   // 5 + 4 cards, the last one = random
    int c = SU.stage == SU_RANDOM ? SU_NSTAGE : SU.stage, row = c / 5, col = c % 5;
    if (dx) {
        col += dx;
        int n = row ? 4 : 5;
        if (col >= n) { SU.sec = 3; return; }
        if (col < 0) col = n - 1;
    }
    if (dy) {
        row += dy;
        if (row < 0) { SU.sec = 1; return; }
        if (row > 1) row = 1;
        if (row == 1 && col > 3) col = 3;
    }
    c = row * 5 + col;
    SU.stage = c >= SU_NSTAGE ? SU_RANDOM : c;
    SU.dirty = true;
}
static void SuPutAssist(int slot, int a) {
    SuPreset& p = SU.p[SU.presetCur];
    for (int k = 0; k < 3; k++)
        if (k != slot && p.a[k] == a) p.a[k] = p.a[slot];   // already in the preset: the two swap places
    p.a[slot] = a;
    SU.dirty = true;
}
static void SuLeave() {
    if (SU.dirty) SuSave();
    SuApply();
    MM_SND |= 4;
    MmGoMenu();
}
static void SuPickNext() {   // after putting an assist in: on to the next slot, or close after the last
    if (SU.pick < 2) { SU.pick++; SU.pickSel = SU.p[SU.presetCur].a[SU.pick]; }
    else SU.pick = -1;
}

// ---- the screen ----
static void MmSetupFrame() {
    const MmPal& P = MmP();
    SuLoad();
    if (MM.roomT < 0) MM.roomT = ImGui::GetTime();
    MmT T = MmTimes(MM.roomT);
    MmNav nav = MmReadNav(SU.rename >= 0 && ImGui::GetActiveID() != 0);
    ImGuiIO& io = ImGui::GetIO();
    bool click = io.MouseClicked[0];
    if (ImGui::GetFrameCount() == MM.quietFrame) { nav = MmNav(); click = false; }
    bool pickWasOpen = SU.pick >= 0;

    // ---- keys / controller ----
    if (SU.rename >= 0) {
        // renaming: the text field takes the keys; leaving it (Enter, a click elsewhere, pad A / B) ends it
        if (!SU.focusName && ImGui::GetFrameCount() > SU.renameFrame + 3 && ImGui::GetActiveID() == 0) { SU.rename = -1; SU.dirty = true; MM_SND |= 2; nav = MmNav(); click = false; }
    } else if (SU.pick >= 0) {
        if (nav.b) { SU.pick = -1; MM_SND |= 4; }
        else {
            if (nav.lb) { SU.pick = (SU.pick + 2) % 3; SU.pickSel = SU.p[SU.presetCur].a[SU.pick]; MM_SND |= 1; }
            if (nav.rb) { SU.pick = (SU.pick + 1) % 3; SU.pickSel = SU.p[SU.presetCur].a[SU.pick]; MM_SND |= 1; }
            if (nav.dx || nav.dy) {
                int c = SU.pickSel + nav.dx + nav.dy * 6;
                if (c >= 0 && c < 31) { SU.pickSel = c; MM_SND |= 1; }
            }
            if (nav.a) { SuPutAssist(SU.pick, SU.pickSel); MM_SND |= 2; SuPickNext(); }
        }
    } else {
        if (nav.b) { SuLeave(); return; }
        if (nav.lb) { SU.sec = (SU.sec + 3) % 4; MM_SND |= 1; }
        if (nav.rb) { SU.sec = (SU.sec + 1) % 4; MM_SND |= 1; }
        if (nav.dx || nav.dy) {
            MM_SND |= 1;
            if (SU.sec == 0) SuCharMove(nav.dx, nav.dy);
            else if (SU.sec == 1) SuColorMove(nav.dx, nav.dy);
            else if (SU.sec == 2) SuStageMove(nav.dx, nav.dy);
            else if (nav.dx < 0) SU.sec = 0;
            else if (nav.dy) SU.presetCur = (SU.presetCur + nav.dy + SU_NPRESET) % SU_NPRESET;
        }
        if (nav.a) {
            MM_SND |= 2;
            if (SU.sec < 3) SU.sec++;                       // picked: on to the next part
            else if (SU.inUse != SU.presetCur) { SU.inUse = SU.presetCur; SU.dirty = true; }   // build 103: A uses the preset
        }
        if (SU.sec == 3 && nav.x) { SU.pick = 0; SU.pickSel = SU.p[SU.presetCur].a[0]; MM_SND |= 2; }   // build 103: X edits it
        if (SU.sec == 3 && nav.y) { SU.rename = SU.presetCur; SU.focusName = true; SU.renameFrame = ImGui::GetFrameCount(); MM_SND |= 2; click = false; }
    }

    ImDrawList* dl = MmPageBegin("##setup", T.tm, T.pulse);
    auto ease = [&](float s, float l) { return MmEase(T, s, l); };
    MmPageTitle(dl, "MAIN MENU", "MY SETUP", ease(0.0f, 0.5f));
    MmText(dl, g_fUi, 22, 1860, 74, MmC(P.dim), "Used for every match you play. Saved on this PC.", 2);
    bool mouseOk = click && SU.pick < 0 && SU.rename < 0;
    auto secFrame = [&](int s, float x0, float y0, float x1, float y1) {
        if (SU.sec == s && SU.pick < 0) dl->AddRect(CP(x0 - 4, y0 - 4), CP(x1 + 4, y1 + 4), MmC(P.gold, 0.35f + 0.25f * T.pulse), CV(10), 0, CV(2));
    };

    // ---- left: character ----
    MmSection(dl, 60, 170, 760, 470, "CHARACTER");
    secFrame(0, 60, 170, 760, 470);
    for (int i = 0; i < 14; i++) {
        float x = 78 + (i % 7) * 97, y = 192 + (i / 7) * 136;
        bool rnd = i == SU_NCHAR;
        bool picked = rnd ? SU.chara == SU_RANDOM : i == SU.chara;
        SuCard(dl, "MA10_CSEL", rnd ? 48 : SU_PORTRAIT[i], x, y, 88, 128, picked, SU.sec == 0 && SU.pick < 0 && i == SU.charCur, T.pulse);
        if (mouseOk && MmHover(x, y, x + 88, y + 128)) { SU.sec = 0; SU.charCur = i; SU.chara = rnd ? SU_RANDOM : i; SU.dirty = true; MM_SND |= 1; }
    }

    // ---- left: colour (the character's head in each of its 21 colours) ----
    {
        char t[32]; snprintf(t, sizeof t, "COLOUR  %d / 21", SU.color + 1);
        MmSection(dl, 60, 510, 760, 690, t);
        secFrame(1, 60, 510, 760, 690);
        int who = SU.chara == SU_RANDOM ? -1 : SU.chara;
        for (int i = 0; i < 21; i++) {
            float x = 78 + (i % 11) * 62 + (i >= 11 ? 31 : 0), y = 532 + (i / 11) * 74;
            bool on = i == SU.color, cur = on && SU.sec == 1 && SU.pick < 0;
            if (cur) MmImg(dl, "MA10_CMN", 39, x - 20, y - 20, 96, 104, MmGlowCol(P.glow, 0.4f + 0.2f * T.pulse), ImVec2(0, 0), ImVec2(1, 1), true);
            dl->AddRectFilled(CP(x, y), CP(x + 56, y + 64), MmC(P.bgTop, 1.0f), CV(5));
            if (who >= 0) SuHead(dl, who, i, x, y, 56, 64);
            else { char n[4]; snprintf(n, sizeof n, "%d", i + 1); MmText(dl, g_fMenu, 16, x + 28, y + 32, on ? MmC(P.gold) : MmC(P.dim), n, 1); }
            dl->AddRect(CP(x, y), CP(x + 56, y + 64), on ? MmC(P.gold) : MmC(P.acc, 0.5f), CV(5), 0, CV(on ? 3.0f : 1.2f));
            if (mouseOk && MmHover(x, y, x + 56, y + 64)) { SU.sec = 1; SU.color = i; SU.dirty = true; MM_SND |= 1; }
        }
    }

    // ---- left: stage (the 8 stages of normal play + random) ----
    {
        MmSection(dl, 60, 730, 760, 940, "STAGE");
        secFrame(2, 60, 730, 760, 940);
        for (int i = 0; i <= SU_NSTAGE; i++) {
            float x = 80 + (i % 5) * 134, y = 752 + (i / 5) * 92;
            bool rnd = i == SU_NSTAGE;
            bool picked = rnd ? SU.stage == SU_RANDOM : i == SU.stage;
            SuCard(dl, "MA10_CSEL", rnd ? 14 : 6 + i, x, y, 124, 79, picked, SU.sec == 2 && SU.pick < 0 && picked, T.pulse);
            if (mouseOk && MmHover(x, y, x + 124, y + 79)) { SU.sec = 2; SU.stage = rnd ? SU_RANDOM : i; SU.dirty = true; MM_SND |= 1; }
        }
        MmText(dl, g_fUi, 17, 80 + 4 * 134 + 62, 876, MmC(P.faint), "In a match: random", 1);
        MmText(dl, g_fUi, 17, 80 + 4 * 134 + 62, 898, MmC(P.faint), "between both picks", 1);
    }

    // ---- middle: preview: the fight sprite in the picked colour, standing on the picked stage ----
    {
        float x0 = 800, x1 = 1260;
        MmSection(dl, x0, 170, x1, 940, "PREVIEW");
        float sx0 = x0 + 16, sx1 = x1 - 16, sy0 = 186, sy1 = 640;
        dl->PushClipRect(CP(sx0, sy0), CP(sx1, sy1), true);
        float bh = sy1 - sy0, bw = bh * 723.0f / 460.0f;
        MmImg(dl, "MA10_CSEL", SU.stage == SU_RANDOM ? 14 : 6 + SU.stage, (sx0 + sx1) / 2 - bw / 2, sy0, bw, bh, MmRGBA(255, 255, 255, 0.9f));
        dl->AddRectFilledMultiColor(CP(sx0, sy1 - 160), CP(sx1, sy1), MmC(P.panel, 0), MmC(P.panel, 0), MmC(P.panel, 0.95f), MmC(P.panel, 0.95f));
        if (SU.chara == SU_RANDOM) {
            float ph = 360, pw = ph * 0.6875f;
            MmImg(dl, "MA10_CSEL", 48, (x0 + x1) / 2 - pw / 2, sy1 - ph - 30, pw, ph);
        } else {
            ImVec2 ss;
            void* tex = art::GetChara(SU.chara, SU.color, &ss);
            if (tex && ss.y > 0) {
                float ph = 430, pw = ph * ss.x / ss.y;
                if (pw > sx1 - sx0 - 10) { pw = sx1 - sx0 - 10; ph = pw * ss.y / ss.x; }
                SuTex(dl, tex, (x0 + x1) / 2 - pw / 2, sy1 - ph - 8, pw, ph);
            } else MmText(dl, g_fUi, 22, (x0 + x1) / 2, (sy0 + sy1) / 2, MmC(P.faint), "Loading...", 1);
        }
        dl->PopClipRect();
        dl->AddRect(CP(sx0, sy0), CP(sx1, sy1), MmC(P.gold, 0.6f), CV(3), 0, CV(1.5f));
        float y = 676;
        const char* nm = SU.chara == SU_RANDOM ? "Random character" : SU_CHARS[SU.chara];
        MmText(dl, g_fUiBold, strlen(nm) > 22 ? 28.0f : 34.0f, (x0 + x1) / 2, y, MmC(P.text), nm, 1, 2, MmC(P.ink));
        char s2[64];
        if (SU.stage == SU_RANDOM) snprintf(s2, sizeof s2, "Colour %d      Random stage", SU.color + 1);
        else snprintf(s2, sizeof s2, "Colour %d      Stage %02d", SU.color + 1, SU.stage + 1);
        MmText(dl, g_fUi, 24, (x0 + x1) / 2, y + 42, MmC(P.dim), s2, 1);
        MmLine(dl, x0 + 30, y + 72, x1 - x0 - 60, 1, MmC(P.acc, 0.6f));
        MmText(dl, g_fMenu, 13, x0 + 30, y + 96, MmC(P.faint), "SUPPORT KNIGHTS");
        MmText(dl, g_fUiBold, 22, x1 - 30, y + 96, MmC(P.gold), SU.p[SU.inUse].name, 2);
        for (int k = 0; k < 3; k++) {
            float bx = x0 + 30 + k * 138, by = y + 116;
            MmImg(dl, "MA10_KCSTMZ", SuAssistSprite(SU.p[SU.inUse].a[k]), bx, by, 128, 59);
            dl->AddRect(CP(bx, by), CP(bx + 128, by + 59), MmC(P.gold, 0.7f), CV(3), 0, CV(1.5f));
        }
    }

    // ---- right: assist presets ----
    {
        float x0 = 1300, x1 = 1860;
        MmSection(dl, x0, 170, x1, 940, "SUPPORT KNIGHT PRESETS");
        secFrame(3, x0, 170, x1, 940);
        for (int i = 0; i < SU_NPRESET; i++) {
            float y = 190 + i * 93;
            bool cur = SU.sec == 3 && i == SU.presetCur, use = i == SU.inUse;
            if (cur && SU.pick < 0) MmImg(dl, "MA10_CMN", 83, x0 + 10, y - 4, x1 - x0 - 20, 92, MmGlowCol(P.glow, 0.45f), ImVec2(0, 0), ImVec2(1, 1), true);
            if (use) dl->AddRect(CP(x0 + 14, y), CP(x1 - 14, y + 84), MmC(P.gold, 0.9f), CV(6), 0, CV(2));
            char n[4]; snprintf(n, sizeof n, "%d", i + 1);
            MmText(dl, g_fMenu, 22, x0 + 40, y + 42, use ? MmC(P.gold) : MmC(P.faint), n, 1);
            if (SU.rename == i) {
                if (MmTextField("##presetname", SU.p[i].name, sizeof SU.p[i].name, x0 + 64, y + 2, x1 - 150, y + 36, "Name of the preset", SU.focusName)) {
                    ImGui::ClearActiveID();
                    SU.rename = -1; SU.dirty = true; MM_SND |= 2;
                }
                SU.focusName = false;
            } else MmText(dl, g_fUiBold, 24, x0 + 70, y + 20, cur || use ? MmC(P.gold) : MmC(P.text), SU.p[i].name);
            if (use) MmPill(dl, x1 - 80, y + 20, 100, 26, "IN USE", P.gold, true);
            for (int k = 0; k < 3; k++) {
                float bx = x0 + 70 + k * 108, by = y + 36;
                MmImg(dl, "MA10_KCSTMZ", SuAssistSprite(SU.p[i].a[k]), bx, by, 100, 46, cur || use ? IM_COL32_WHITE : MmRGBA(200, 180, 180));
                dl->AddRect(CP(bx, by), CP(bx + 100, by + 46), MmC(P.gold, cur || use ? 0.7f : 0.45f), CV(3), 0, CV(1.5f));
            }
            if (mouseOk && MmHover(x0 + 70, y + 36, x0 + 70 + 3 * 108, y + 82)) {   // a click on the knights: edit them
                SU.sec = 3; SU.presetCur = i; SU.pick = 0; SU.pickSel = SU.p[i].a[0]; MM_SND |= 2;
            } else if (mouseOk && MmHover(x0 + 14, y, x1 - 14, y + 84)) {          // elsewhere on the row: use it
                SU.sec = 3; SU.presetCur = i;
                if (SU.inUse != i) { SU.inUse = i; SU.dirty = true; }
                MM_SND |= 2;
            }
        }
    }

    // ---- assist picker (A on a preset): the 31 assists, the full card of the one under the cursor ----
    if (SU.pick >= 0) {
        dl->AddRectFilled(CP(0, 0), CP(1920, 1080), MmC(P.bgBot, 0.8f));
        float x0 = 160, x1 = 1760, y0 = 170, y1 = 930;
        dl->AddRectFilled(CP(x0, y0 - 40), CP(x1, y1), MmC(P.bgBot, 1.0f), CV(10));
        SuPreset& pr = SU.p[SU.presetCur];
        char t[64]; snprintf(t, sizeof t, "%s  -  SUPPORT KNIGHT %d", pr.name, SU.pick + 1);
        MmSection(dl, x0, y0, x1, y1, t);
        bool pickClick = click && pickWasOpen;   // not the click that opened the picker
        for (int k = 0; k < 3; k++) {   // the preset's three slots
            float bx = x0 + 40 + k * 190, by = y0 + 34;
            bool on = k == SU.pick;
            char sl[16]; snprintf(sl, sizeof sl, "KNIGHT %d", k + 1);
            MmText(dl, g_fMenu, 12, bx + 84, by, on ? MmC(P.gold) : MmC(P.faint), sl, 1);
            MmImg(dl, "MA10_KCSTMZ", SuAssistSprite(pr.a[k]), bx, by + 14, 168, 77, on ? IM_COL32_WHITE : MmRGBA(150, 130, 130));
            dl->AddRect(CP(bx - 2, by + 12), CP(bx + 170, by + 93), on ? MmC(P.gold) : MmC(P.acc, 0.6f), CV(4), 0, CV(on ? 3 : 1.5f));
            if (pickClick && MmHover(bx - 2, by + 12, bx + 170, by + 93)) { SU.pick = k; SU.pickSel = pr.a[k]; MM_SND |= 1; }
        }
        int total = SU_STATS[pr.a[0]].cost + SU_STATS[pr.a[1]].cost + SU_STATS[pr.a[2]].cost;
        char tc[32]; snprintf(tc, sizeof tc, "Total cost %d", total);
        MmText(dl, g_fUi, 20, x0 + 640, y0 + 70, MmC(P.dim), tc, 0);
        MmLine(dl, x0 + 30, y0 + 128, 1010, 1, MmC(P.acc, 0.6f));
        for (int i = 0; i < 31 && SU.pick >= 0; i++) {
            float bx = x0 + 40 + (i % 6) * 168, by = y0 + 150 + (i / 6) * 84;
            bool cur = i == SU.pickSel, inSet = false;
            for (int k = 0; k < 3; k++) inSet |= pr.a[k] == i;
            if (cur) MmImg(dl, "MA10_CMN", 39, bx - 26, by - 26, 212, 124, MmGlowCol(P.glow, 0.4f + 0.2f * T.pulse), ImVec2(0, 0), ImVec2(1, 1), true);
            MmImg(dl, "MA10_KCSTMZ", SuAssistSprite(i), bx, by, 156, 72, cur || inSet ? IM_COL32_WHITE : MmRGBA(170, 150, 150));
            dl->AddRect(CP(bx, by), CP(bx + 156, by + 72), cur ? MmC(P.gold) : MmC(P.acc, inSet ? 0.9f : 0.4f), CV(3), 0, CV(cur ? 3 : 1.5f));
            if (inSet && !cur) MmPill(dl, bx + 126, by + 14, 52, 22, "IN", P.gold, true);
            if (MmHover(bx, by, bx + 156, by + 72) && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || pickClick)) {
                if (SU.pickSel != i) MM_SND |= 1;
                SU.pickSel = i;
                if (pickClick) { SuPutAssist(SU.pick, i); MM_SND |= 2; SuPickNext(); }
            }
        }
        // the full card
        if (SU.pick >= 0) SuCardDraw(dl, 1260, y0 + 60, 1.25f, SU.pickSel);
        MmLine(dl, x0 + 30, y1 - 66, x1 - x0 - 60, 1, MmC(P.acc, 0.5f));
        MmPadButton(dl, x0 + 30, y1 - 52, 'A', MmRGBA(40, 160, 70));
        MmText(dl, g_fMenu, 13, x0 + 74, y1 - 34, MmC(P.text), "Put in slot");
        MmPadButton(dl, x0 + 250, y1 - 52, 'B', MmRGBA(190, 40, 40));
        MmText(dl, g_fMenu, 13, x0 + 294, y1 - 34, MmC(P.text), "Done");
        MmText(dl, g_fMenu, 13, x0 + 420, y1 - 34, MmC(P.text), "LB / RB  SLOT");
        MmText(dl, g_fUi, 19, 1200, y1 - 34, MmC(P.faint), "Each Support Knight can be in a preset once", 2);
    }

    // ---- description line + prompts ----
    if (SU.pick < 0) {
        dl->PushClipRect(ImVec2(0, 0), g_disp, false);
        MmLine(dl, -g_cvO.x / g_cv, 960, g_disp.x / g_cv, 3, MmC(P.acc, 0.95f));
        MmLine(dl, -g_cvO.x / g_cv, 964, g_disp.x / g_cv, 1, MmC(P.gold, 0.6f));
        dl->PopClipRect();
        static const char* help[4] = {"Pick the character you play with.", "Pick a colour. The preview shows it on your character.",
                                      "Your stage pick. In a match it's random between both players' picks.",
                                      "A uses this preset in your matches, X edits its Support Knights, Y renames it."};
        MmText(dl, g_fUi, 26, 60, 994, MmC(P.text), SU.rename >= 0 ? "Type a name with the keyboard, then press Enter." : help[SU.sec]);
        float x = 60;
        MmPadButton(dl, x, 1022, 'A', MmRGBA(40, 160, 70)); MmText(dl, g_fMenu, 13, x + 44, 1040, MmC(P.text), SU.sec == 3 ? "Use" : "Next"); x += 150;
        if (SU.sec == 3) {
            MmPadButton(dl, x, 1022, 'X', MmRGBA(40, 100, 200)); MmText(dl, g_fMenu, 13, x + 44, 1040, MmC(P.text), "Edit"); x += 140;
            MmPadButton(dl, x, 1022, 'Y', MmRGBA(220, 180, 30)); MmText(dl, g_fMenu, 13, x + 44, 1040, MmC(P.text), "Rename"); x += 170;
        }
        MmPadButton(dl, x, 1022, 'B', MmRGBA(190, 40, 40)); MmText(dl, g_fMenu, 13, x + 44, 1040, MmC(P.text), "Save and back"); x += 240;
        MmText(dl, g_fMenu, 13, x, 1040, MmC(P.dim), "LB / RB  SECTION");
        MmNoteDraw(dl, ImGui::GetTime(), 1100, 994);
    }
    MmPageEnd(dl);
    // every change counts for the next match straight away (saved to the file on the way out)
    if (SU.dirty && SU.rename < 0) SuApply();
}

// ---- the My setup panel on the main menu ----
static void SuPanel(ImDrawList* dl, float x0, float y0, float in) {
    const MmPal& P = MmP();
    SuLoad();
    bool rnd = SU.chara == SU_RANDOM;
    MmImg(dl, "MA10_CSEL", rnd ? 48 : SU_PORTRAIT[SU.chara], x0 + 24, y0 + 62, 112, 164, MmRGBA(255, 255, 255, in));
    float x = x0 + 154, y = y0 + 72;
    const char* nm = rnd ? "Random" : SU_CHARS[SU.chara];
    MmText(dl, g_fUiBold, strlen(nm) > 18 ? 21.0f : 27.0f, x, y, MmC(P.text, in), nm);
    dl->AddRectFilled(CP(x, y + 20), CP(x + 26, y + 46), MmC(P.bgTop, in), CV(4));
    if (!rnd) SuHead(dl, SU.chara, SU.color, x, y + 20, 26, 26);
    dl->AddRect(CP(x, y + 20), CP(x + 26, y + 46), MmRGBA(255, 210, 110, in), CV(4), 0, CV(2));
    char c[24]; snprintf(c, sizeof c, "Colour %d", SU.color + 1);
    MmText(dl, g_fUi, 21, x + 36, y + 33, MmC(P.dim, in), c);
    MmImg(dl, "MA10_CSEL", SU.stage == SU_RANDOM ? 14 : 6 + SU.stage, x, y + 58, 74, 47, MmRGBA(255, 255, 255, in));
    dl->AddRect(CP(x, y + 58), CP(x + 74, y + 105), MmRGBA(214, 170, 82, in), 0, 0, CV(1));
    char s[24];
    if (SU.stage == SU_RANDOM) snprintf(s, sizeof s, "Random stage"); else snprintf(s, sizeof s, "Stage %02d", SU.stage + 1);
    MmText(dl, g_fUi, 21, x + 84, y + 81, MmC(P.dim, in), s);
    MmText(dl, g_fMenu, 11, x, y + 124, MmC(P.faint, in), "KNIGHTS");
    MmText(dl, g_fUiBold, 21, x + 90, y + 124, MmC(P.text, in), SU.p[SU.inUse].name);
    for (int k = 0; k < 3; k++) MmImg(dl, "MA10_KCSTMZ", SuAssistSprite(SU.p[SU.inUse].a[k]), x + k * 88, y + 142, 82, 38, MmRGBA(255, 255, 255, in));
}
// build 112: My setup as one row (the room's MY MATCH SETTINGS bar)
static int SuPortraitOf(int chara) { return chara >= 0 && chara < SU_NCHAR ? SU_PORTRAIT[chara] : 48; }
static void SuRoomPanel(ImDrawList* dl, float x0, float y0, float x1, float y1, float in) {
    const MmPal& P = MmP();
    SuLoad();
    bool rnd = SU.chara == SU_RANDOM;
    MmImg(dl, "MA10_CSEL", rnd ? 48 : SU_PORTRAIT[SU.chara], x0 + 16, y0 + 12, 70, 102, MmRGBA(255, 255, 255, in));
    float cy = (y0 + y1) / 2;
    MmText(dl, g_fMenu, 11, x0 + 102, cy - 20, MmC(P.faint, in), "CHARACTER");
    const char* nm = rnd ? "Random" : SU_CHARS[SU.chara];
    MmText(dl, g_fUiBold, strlen(nm) > 16 ? 19.0f : 25.0f, x0 + 102, cy + 10, MmC(P.text, in), nm);
    float x = x0 + 330;
    MmText(dl, g_fMenu, 11, x, cy - 20, MmC(P.faint, in), "COLOUR");
    dl->AddRectFilled(CP(x, cy - 2), CP(x + 26, cy + 24), MmC(P.bgTop, in), CV(4));
    if (!rnd) SuHead(dl, SU.chara, SU.color, x, cy - 2, 26, 26);
    dl->AddRect(CP(x, cy - 2), CP(x + 26, cy + 24), MmRGBA(255, 210, 110, in), CV(4), 0, CV(2));
    char c[24]; snprintf(c, sizeof c, "Colour %d", SU.color + 1);
    MmText(dl, g_fUi, 21, x + 36, cy + 11, MmC(P.dim, in), c);
    x = x0 + 470;
    MmImg(dl, "MA10_CSEL", SU.stage == SU_RANDOM ? 14 : 6 + SU.stage, x, cy - 30, 100, 64, MmRGBA(255, 255, 255, in));
    dl->AddRect(CP(x, cy - 30), CP(x + 100, cy + 34), MmRGBA(214, 170, 82, in), 0, 0, CV(1));
    MmText(dl, g_fMenu, 11, x + 114, cy - 20, MmC(P.faint, in), "STAGE");
    char st[24];
    if (SU.stage == SU_RANDOM) snprintf(st, sizeof st, "Random"); else snprintf(st, sizeof st, "Stage %02d", SU.stage + 1);
    MmText(dl, g_fUiBold, 25, x + 114, cy + 10, MmC(P.text, in), st);
    x = x0 + 700;
    MmText(dl, g_fMenu, 11, x, cy - 26, MmC(P.faint, in), "SUPPORT KNIGHTS");
    MmText(dl, g_fUiBold, 21, x, cy + 0, MmC(P.text, in), SU.p[SU.inUse].name);
    for (int k = 0; k < 3; k++) MmImg(dl, "MA10_KCSTMZ", SuAssistSprite(SU.p[SU.inUse].a[k]), x + k * 62, cy + 16, 58, 27, MmRGBA(255, 255, 255, in));
    MmPadButton(dl, x1 - 104, cy - 18, 'Y', MmRGBA(210, 170, 20, in));
    MmText(dl, g_fMenu, 14, x1 - 60, cy, MmC(P.text, in), "Edit");
}
// open the editor from the main menu
static void SuOpen() {
    SuLoad();
    SU.sec = 0; SU.pick = -1; SU.rename = -1;
    SU.charCur = SU.chara == SU_RANDOM ? SU_NCHAR : SU.chara;
    SU.presetCur = SU.inUse;
    MM.screen = 4; MM.roomT = -1; MM.quietFrame = ImGui::GetFrameCount();
}
