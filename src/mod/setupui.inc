// ---------------------------------------------------------------------------
// My setup (main menu Y, a click on the My setup panel, or Y in a room).
// Build 126: redone step by step (issue #7): 1 character, 2 colour, 3 Support
// Knight preset, then a summary page (also stage + input delay). The first time
// it walks through the steps; after that it opens on the summary, and A on a
// line there goes back to that step. Start saves and closes from anywhere.
// Each character keeps its own colour and its own Support Knight presets
// (random character has a list of its own). The Knight grid is the game's own
// (KNIGHTS CUSTOMIZE): rows fire / water / wind / light, every Knight in the
// game's spot, and the cards are the game's cards. Saved in rollback.ini [setup].
//
// Game data used (worked out 2026-11-16 / 2026-11-23, see the tech doc):
//   MA10_CSEL  portraits per character id (SU_PORTRAIT), 48 random portrait,
//              6-13 the 8 stage pictures (stage ids 00-07), 14 random stage
//   MA10_KCSTMZ  (the game's Knight customize screen) everything for the cards:
//              83 + id the big card picture of each Knight (on the B,G,R,A sheets),
//              7/11/10/9 card background and 2/5/4/3 card frame (fire/water/wind/light),
//              1 the Knights' names as the card shows them (rows of 24 px),
//              19 element icons, 30 cost, 18 type labels, 20 hearts, 21 stars,
//              76-79 HP BONUS / ATTACK / RANGE / SPEED, 6 the empty card,
//              small grid cards: 25/22/23/28 frames, 31-65 portraits (SU_BANNER), 26/27 random
//   act\chara  fight sprite picture 0 (standing pose) in the 21 colours (art::GetChara)
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
// MA10_CSEL portrait of each character id; MA10_KCSTMZ small-card portrait of each assist id
static const int SU_PORTRAIT[13] = {44, 46, 47, 42, 37, 41, 40, 39, 38, 43, 36, 35, 45};
static const int SU_BANNER[31] = {62, 53, 35, 51, 60, 45, 54, 63, 31, 49, 56, 59, 46, 33, 64, 65,
                                  34, 41, 50, 58, 52, 61, 40, 47, 55, 44, 32, 43, 57, 48, 42};
static inline int SuAssistSprite(int a) { return SU_BANNER[a >= 0 && a < 31 ? a : 0]; }
static const int SU_NCHAR = 13, SU_NSTAGE = 8, SU_NPRESET = 8;
static const int SU_RANDOM = 0xFF;
static const int SU_NLIST = SU_NCHAR + 1;   // build 126: a preset list per character, the last one for random

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
// build 126: the game's Knight grid (KNIGHTS CUSTOMIZE, user's screenshot 2026-11-23):
// 9 columns, rows fire / water / wind / light; -1 = no card there, SU_GRID_RANDOM = the Random card
static const int SU_GRID_RANDOM = 99;
static const int SU_GRID[4][9] = {{2, 27, 25, 6, 12, 23, 29, 9, 18},
                                  {4, 20, 1, 5, 24, 10, 28, 19, 11},
                                  {3, 21, 0, 7, 14, 15, 17, 16, 22},
                                  {26, -1, -1, 13, SU_GRID_RANDOM, 30, -1, -1, 8}};
static inline int SuEl(int a) { int e = SU_STATS[a >= 0 && a < 31 ? a : 0].elem; return (e >= 1 && e <= 4 ? e : 1) - 1; }
static inline int SuCost(int a) { int c = SU_STATS[a >= 0 && a < 31 ? a : 0].cost; return c >= 1 && c <= 3 ? c : 1; }

struct SuPreset { char name[24]; int a[3]; };
struct SuList { int n = 1, inUse = 0, color = 0; SuPreset p[SU_NPRESET]; };
static struct {
    bool loaded = false;
    bool done = false;              // walked through the steps once (then My setup opens on the summary)
    int chara = 2, stage = 0;       // SU_RANDOM = random
    SuList L[SU_NLIST];
    int step = 3;                   // 0 character, 1 colour, 2 Support Knights, 3 summary
    bool fromSum = false;           // the step was opened from the summary: A goes back there
    int sumRow = 0;                 // summary: 0 character 1 colour 2 knights 3 stage 4 input delay
    int charCur = 2;
    int presetCur = 0;              // Support Knights step: row (n = "new preset")
    int pick = -1;                  // Knight grid open for this slot (0-2), -1 closed
    int gr = 0, gc = 0;             // Knight grid cursor
    int rename = -1;                // preset being renamed, -1 none
    bool focusName = false;
    int renameFrame = 0;
    int delArm = -1; double delAt = 0;   // View / Delete pressed once on this preset (press again to delete)
    bool dirty = false;
} SU;
static inline int SuLi() { return SU.chara == SU_RANDOM || SU.chara < 0 || SU.chara >= SU_NCHAR ? SU_NCHAR : SU.chara; }
static inline SuList& SuCurList() { return SU.L[SuLi()]; }
static inline const SuPreset& SuInUse() { SuList& l = SuCurList(); return l.p[l.inUse >= 0 && l.inUse < l.n ? l.inUse : 0]; }
static inline int SuColor() { return SuCurList().color; }

// ---- saving: rollback.ini [setup] ----
#ifndef UI_HARNESS
static void SuIniPath(wchar_t* ini) { _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir()); }
static void SuIniPut(const wchar_t* key, const char* utf8) {
    wchar_t ini[MAX_PATH], w[128];
    SuIniPath(ini);
    if (!utf8) { WritePrivateProfileStringW(L"setup", key, nullptr, ini); return; }
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
static void SuFixList(SuList& l) {
    if (l.n < 1 || l.n > SU_NPRESET) l.n = 1;
    if (l.inUse < 0 || l.inUse >= l.n) l.inUse = 0;
    if (l.color < 0 || l.color > 20) l.color = 0;
    for (int i = 0; i < l.n; i++) SuFixPreset(l.p[i]);
}
static void SuApply();
static void SuLoad() {
    if (SU.loaded) return;
    SU.loaded = true;
    for (auto& l : SU.L) {   // a fresh start: one preset
        l = SuList();
        snprintf(l.p[0].name, sizeof l.p[0].name, "Preset 1");
        l.p[0].a[0] = 0; l.p[0].a[1] = 1; l.p[0].a[2] = 2;
    }
#ifndef UI_HARNESS
    char v[96];
    if (SuIniGet(L"character", v, sizeof v)) SU.chara = !_stricmp(v, "random") ? SU_RANDOM : atoi(v);
    if (SuIniGet(L"stage", v, sizeof v)) SU.stage = !_stricmp(v, "random") ? SU_RANDOM : atoi(v) - 1;
    if (SuIniGet(L"version", v, sizeof v) && atoi(v) >= 2) {
        // build 126: a list per character: cNN_colour, cNN_presets (how many), cNN_preset_in_use, cNN_presetK(_name)
        SU.done = SuIniGet(L"steps_done", v, sizeof v) && atoi(v) != 0;
        for (int c = 0; c < SU_NLIST; c++) {
            SuList& l = SU.L[c];
            wchar_t k[40];
            _snwprintf(k, 40, L"c%02d_colour", c); if (SuIniGet(k, v, sizeof v)) l.color = atoi(v) - 1;
            _snwprintf(k, 40, L"c%02d_presets", c); if (SuIniGet(k, v, sizeof v)) l.n = atoi(v);
            _snwprintf(k, 40, L"c%02d_preset_in_use", c); if (SuIniGet(k, v, sizeof v)) l.inUse = atoi(v) - 1;
            if (l.n < 1 || l.n > SU_NPRESET) l.n = 1;
            for (int i = 0; i < l.n; i++) {
                _snwprintf(k, 40, L"c%02d_preset%d_name", c, i + 1);
                if (SuIniGet(k, v, sizeof v)) snprintf(l.p[i].name, sizeof l.p[i].name, "%s", v);
                else snprintf(l.p[i].name, sizeof l.p[i].name, "Preset %d", i + 1);
                _snwprintf(k, 40, L"c%02d_preset%d", c, i + 1);
                int a[3];
                if (SuIniGet(k, v, sizeof v) && sscanf(v, "%d,%d,%d", &a[0], &a[1], &a[2]) == 3)
                    for (int j = 0; j < 3; j++) l.p[i].a[j] = a[j];
            }
        }
    } else {
        // the old single setup (builds 102-125): every character gets the old colour and the
        // preset that was in use (its name kept); the other old presets are not carried over
        int color = 0, inUse = 0;
        if (SuIniGet(L"colour", v, sizeof v)) color = atoi(v) - 1;
        if (SuIniGet(L"preset_in_use", v, sizeof v)) inUse = atoi(v) - 1;
        if (inUse < 0 || inUse >= 8) inUse = 0;
        SuPreset old{};
        snprintf(old.name, sizeof old.name, "Preset 1");
        old.a[0] = 0; old.a[1] = 1; old.a[2] = 2;
        wchar_t k[32];
        _snwprintf(k, 32, L"preset%d_name", inUse + 1);
        if (SuIniGet(k, v, sizeof v)) snprintf(old.name, sizeof old.name, "%s", v);
        _snwprintf(k, 32, L"preset%d", inUse + 1);
        int a[3];
        if (SuIniGet(k, v, sizeof v) && sscanf(v, "%d,%d,%d", &a[0], &a[1], &a[2]) == 3)
            for (int j = 0; j < 3; j++) old.a[j] = a[j];
        bool hadOld = SuIniGet(L"character", v, sizeof v);
        for (auto& l : SU.L) { l.n = 1; l.inUse = 0; l.color = color; l.p[0] = old; }
        SU.done = hadOld;   // someone who had a setup already gets the summary straight away
        if (hadOld) Log("setup: moved the old setup to per-character presets (\"%s\" for every character)", old.name);
        SU.dirty = true;
    }
#endif
    if (SU.chara != SU_RANDOM && (SU.chara < 0 || SU.chara >= SU_NCHAR)) SU.chara = 2;
    if (SU.stage != SU_RANDOM && (SU.stage < 0 || SU.stage >= SU_NSTAGE)) SU.stage = 0;
    for (auto& l : SU.L) SuFixList(l);
    SU.charCur = SU.chara == SU_RANDOM ? SU_NCHAR : SU.chara;
    SU.presetCur = SuCurList().inUse;
    SuApply();
}
static void SuSave() {
#ifndef UI_HARNESS
    char v[96];
    SuIniPut(L"version", "2");
    SuIniPut(L"steps_done", SU.done ? "1" : "0");
    if (SU.chara == SU_RANDOM) snprintf(v, sizeof v, "random"); else snprintf(v, sizeof v, "%d", SU.chara);
    SuIniPut(L"character", v);
    if (SU.stage == SU_RANDOM) snprintf(v, sizeof v, "random"); else snprintf(v, sizeof v, "%d", SU.stage + 1);
    SuIniPut(L"stage", v);
    for (int c = 0; c < SU_NLIST; c++) {
        const SuList& l = SU.L[c];
        wchar_t k[40];
        _snwprintf(k, 40, L"c%02d_colour", c); snprintf(v, sizeof v, "%d", l.color + 1); SuIniPut(k, v);
        _snwprintf(k, 40, L"c%02d_presets", c); snprintf(v, sizeof v, "%d", l.n); SuIniPut(k, v);
        _snwprintf(k, 40, L"c%02d_preset_in_use", c); snprintf(v, sizeof v, "%d", l.inUse + 1); SuIniPut(k, v);
        for (int i = 0; i < SU_NPRESET; i++) {
            _snwprintf(k, 40, L"c%02d_preset%d_name", c, i + 1); SuIniPut(k, i < l.n ? l.p[i].name : nullptr);
            _snwprintf(k, 40, L"c%02d_preset%d", c, i + 1);
            snprintf(v, sizeof v, "%d,%d,%d", l.p[i].a[0], l.p[i].a[1], l.p[i].a[2]);
            SuIniPut(k, i < l.n ? v : nullptr);
        }
    }
    // the old keys (builds 102-125) are not used any more
    SuIniPut(L"colour", nullptr);
    SuIniPut(L"preset_in_use", nullptr);
    for (int i = 0; i < 8; i++) {
        wchar_t k[32];
        _snwprintf(k, 32, L"preset%d", i + 1); SuIniPut(k, nullptr);
        _snwprintf(k, 32, L"preset%d_name", i + 1); SuIniPut(k, nullptr);
    }
    const SuPreset& p = SuInUse();
    char sv[16];
    if (SU.stage == SU_RANDOM) snprintf(sv, sizeof sv, "random"); else snprintf(sv, sizeof sv, "%02d", SU.stage + 1);
    Log("setup: saved (character %s, colour %d, stage %s, %s: knights %02X/%02X/%02X)",
        SU.chara == SU_RANDOM ? "random" : SU_CHARS[SU.chara], SuColor() + 1, sv, p.name, p.a[0], p.a[1], p.a[2]);
#endif
    SU.dirty = false;
}
// the setup becomes this PC's picks for the next match
static void SuApply() {
#ifndef UI_HARNESS
    EnterCriticalSection(&g_lobbyLock);
    LbPicks& p = LB.me.p;
    const SuPreset& pr = SuInUse();
    p.chara = SU.chara == SU_RANDOM ? LB_RANDOM : (uint8_t)SU.chara;
    p.color = (uint8_t)SuColor();
    for (int k = 0; k < 3; k++) p.assist[k] = (uint8_t)pr.a[k];
    p.stage = SU.stage == SU_RANDOM ? LB_RANDOM : (uint8_t)SU.stage;
    LbChanged();
    LeaveCriticalSection(&g_lobbyLock);
#endif
}

// ---- drawing pieces ----
static float SuTW(ImFont* f, float px, const char* s) {
    if (!f) f = ImGui::GetFont();
    return f->CalcTextSizeA(CV(px * MmEm(f)), FLT_MAX, 0.0f, s).x / g_cv;
}
static void SuTex(ImDrawList* dl, void* tex, float x, float y, float w, float h, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1), ImU32 col = IM_COL32_WHITE) {
    if (tex) dl->AddImage((ImTextureID)tex, CP(x, y), CP(x + w, y + h), uv0, uv1, col);
}
// the gold "this one" frame with a soft glow
static void SuSelFrame(ImDrawList* dl, float x0, float y0, float x1, float y1, float pulse, float pad = 4) {
    const MmPal& P = MmP();
    MmImg(dl, "MA10_CMN", 39, x0 - 26, y0 - 26, x1 - x0 + 52, y1 - y0 + 52, MmGlowCol(P.glow, 0.35f + 0.2f * pulse), ImVec2(0, 0), ImVec2(1, 1), true);
    dl->AddRect(CP(x0 - pad, y0 - pad), CP(x1 + pad, y1 + pad), MmC(P.gold), CV(3), 0, CV(4));
    dl->AddRect(CP(x0 - pad - 3, y0 - pad - 3), CP(x1 + pad + 3, y1 + pad + 3), MmC(P.selOut, 0.9f), CV(4), 0, CV(1.5f));
}
// a sheet of 2 columns x 4 rows of three hearts / stars: halves 0-6 (left column whole, right column + half)
static void SuPips(ImDrawList* dl, int spr, int halves, float x, float y, float w, float h) {
    if (halves < 0) halves = 0;
    if (halves > 6) halves = 6;
    float cu = (halves % 2) * 0.5f, rv = (halves / 2) / 4.0f;
    MmImg(dl, "MA10_KCSTMZ", spr, x, y, w, h, IM_COL32_WHITE, ImVec2(cu, rv), ImVec2(cu + 0.5f, rv + 0.25f));
}
// build 126: the game's big Knight card (356 x 487 at sc 1), all from the game's own pieces (MA10_KCSTMZ),
// laid out like the game's card (checked against the user's screenshot). a < 0: the empty card.
static void SuBigCard(ImDrawList* dl, float x, float y, float sc, int a, ImU32 tint = IM_COL32_WHITE) {
    auto X = [&](float v) { return x + v * sc; };
    auto Y = [&](float v) { return y + v * sc; };
    if (a < 0 || a >= 31) { MmImg(dl, "MA10_KCSTMZ", 6, X(1), Y(2), 354 * sc, 484 * sc, tint); return; }
    static const int bgOf[4] = {7, 11, 10, 9}, frameOf[4] = {2, 5, 4, 3};
    const SuStats& st = SU_STATS[a];
    int el = SuEl(a), ty = (st.type >= 1 && st.type <= 3 ? st.type : 1) - 1, cost = SuCost(a);
    MmImg(dl, "MA10_KCSTMZ", bgOf[el], X(2), Y(3), 351 * sc, 481 * sc, tint);
    MmImg(dl, "MA10_KCSTMZ", 83 + a, X(2), Y(3), 351 * sc, 481 * sc, tint);
    MmImg(dl, "MA10_KCSTMZ", frameOf[el], x, y, 356 * sc, 487 * sc, tint);
    float ex = (el % 2) * 0.5f, ey = (el / 2) * 0.5f;
    MmImg(dl, "MA10_KCSTMZ", 19, X(12), Y(9), 72 * sc, 73 * sc, tint, ImVec2(ex, ey), ImVec2(ex + 0.5f, ey + 0.5f));
    MmImg(dl, "MA10_KCSTMZ", 30, X(14), Y(92), 57 * sc, 58 * sc, tint, ImVec2((cost - 1) / 3.0f, 0), ImVec2(cost / 3.0f, 1));
    // the name strip: a dark bar with the game's own lettering of the name (one 24 px row per Knight)
    dl->AddRectFilledMultiColor(CP(X(88), Y(28)), CP(X(344), Y(54)), MmRGBA(48, 30, 16, 0.55f), MmRGBA(48, 30, 16, 0.94f),
                                MmRGBA(48, 30, 16, 0.94f), MmRGBA(48, 30, 16, 0.55f));
    MmImg(dl, "MA10_KCSTMZ", 1, X(88), Y(29), 254 * sc, 24 * sc, tint, ImVec2(0, a * 24 / 768.0f), ImVec2(1, (a + 1) * 24 / 768.0f));
    MmImg(dl, "MA10_KCSTMZ", 18, X(28), Y(360), 300 * sc, 42 * sc, tint, ImVec2(0, ty / 3.0f), ImVec2(1, (ty + 1) / 3.0f));
    const int lab[3] = {77, 79, 78};                       // ATTACK, SPEED, RANGE
    const float labW[3] = {86, 73, 77};
    const int val[3] = {st.atk, st.spd, st.rng};
    for (int r = 0; r < 3; r++) {
        float ry = 398 + r * 25.0f;
        MmImg(dl, "MA10_KCSTMZ", lab[r], X(30), Y(ry), labW[r] * sc, 29 * sc, tint);
        SuPips(dl, 21, val[r], X(118), Y(ry), 77 * sc, 30 * sc);
    }
    MmImg(dl, "MA10_KCSTMZ", 76, X(220), Y(398), 108 * sc, 29 * sc, tint);
    SuPips(dl, 20, SuHearts(st.hp), X(232), Y(424), 88 * sc, 28 * sc);
}
// build 126: the game's small grid card (192 x 77 at sc 1): element frame, portrait, cost.
// a = SU_GRID_RANDOM: the Random card. dim: darker, like the cards the cursor is not on.
static void SuSmallCard(ImDrawList* dl, float x, float y, float sc, int a, bool dim = false, float alpha = 1.0f) {
    // build 126b: rounded corners, an element gradient behind the portrait (like the game's), and the
    // frame drawn last so its gold inner border covers the portrait's edges
    ImU32 tint = dim ? MmRGBA(165, 150, 150, alpha) : MmRGBA(255, 255, 255, alpha);
    const float rnd = CV(7 * sc);
    auto img = [&](int spr, float ix, float iy, float iw, float ih, float r) {
        ImVec2 ts;
        void* tex = art::Get("MA10_KCSTMZ", spr, &ts);
        if (tex) dl->AddImageRounded((ImTextureID)tex, CP(ix, iy), CP(ix + iw, iy + ih), ImVec2(0, 0), ImVec2(1, 1), tint, r);
    };
    if (a == SU_GRID_RANDOM) img(dim ? 26 : 27, x, y, 192 * sc, 77 * sc, rnd);
    else if (a >= 0 && a < 31) {
        static const int frameOf[4] = {25, 22, 23, 28};
        static const uint8_t top[4][3] = {{255, 132, 0}, {0, 240, 250}, {90, 215, 105}, {200, 50, 215}};
        static const uint8_t bot[4][3] = {{214, 4, 0}, {0, 125, 232}, {18, 130, 40}, {110, 0, 240}};
        int el = SuEl(a);
        float k = dim ? 0.65f : 1.0f;
        ImU32 ct = MmRGBA((int)(top[el][0] * k), (int)(top[el][1] * k), (int)(top[el][2] * k), alpha);
        ImU32 cb = MmRGBA((int)(bot[el][0] * k), (int)(bot[el][1] * k), (int)(bot[el][2] * k), alpha);
        dl->AddRectFilledMultiColor(CP(x + 7 * sc, y + 7 * sc), CP(x + 144 * sc, y + 69 * sc), ct, ct, cb, cb);
        img(SuAssistSprite(a), x + 7 * sc, y + 6 * sc, 139 * sc, 64 * sc, 0);
        img(frameOf[el], x, y, 192 * sc, 77 * sc, rnd);
        int cost = SuCost(a);
        MmImg(dl, "MA10_KCSTMZ", 30, x + 147 * sc, y + 9 * sc, 44 * sc, 45 * sc, tint, ImVec2((cost - 1) / 3.0f, 0), ImVec2(cost / 3.0f, 1));
    } else img(24, x, y, 192 * sc, 77 * sc, rnd);
    if (dim) dl->AddRectFilled(CP(x, y), CP(x + 192 * sc, y + 77 * sc), MmRGBA(10, 0, 0, 0.22f * alpha), rnd);
}
// the head of the character in one colour (for the colour squares)
static void SuHead(ImDrawList* dl, int chara, int colour, float x, float y, float w, float h) {
    float hb[4] = {0.3f, 0, 0.64f, 0.2f};
    ImVec2 ss(0, 0);
    void* tex = art::GetChara(chara, colour, &ss, hb);
    if (!tex || ss.x <= 0 || ss.y <= 0 || w <= 0 || h <= 0) return;
    // build 126c: keep the sprite's own proportions: widen the head box (around its centre) or make it
    // taller (downwards) until it has the shape of the box it is drawn in
    float cw = (hb[2] - hb[0]) * ss.x, ch = (hb[3] - hb[1]) * ss.y, want = w / h;
    if (cw / ch > want) {
        float nh = cw / want / ss.y;
        hb[3] = hb[1] + nh;
        if (hb[3] > 1) { hb[1] = ImMax(0.0f, hb[1] - (hb[3] - 1)); hb[3] = 1; }
    } else {
        float nw = ch * want / ss.x, mid = (hb[0] + hb[2]) / 2;
        hb[0] = mid - nw / 2; hb[2] = mid + nw / 2;
        if (hb[0] < 0) { hb[2] -= hb[0]; hb[0] = 0; }
        if (hb[2] > 1) { hb[0] = ImMax(0.0f, hb[0] - (hb[2] - 1)); hb[2] = 1; }
    }
    SuTex(dl, tex, x, y, w, h, ImVec2(hb[0], hb[1]), ImVec2(hb[2], hb[3]));
}
// the fight sprite in its colour, standing on the stage picture (preview box)
static void SuStagePreview(ImDrawList* dl, float x0, float y0, float x1, float y1, int chara, int colour, int stage) {
    const MmPal& P = MmP();
    dl->PushClipRect(CP(x0, y0), CP(x1, y1), true);
    float bh = y1 - y0, bw = bh * 723.0f / 460.0f;
    if (bw < x1 - x0) { bw = x1 - x0; bh = bw * 460.0f / 723.0f; }
    MmImg(dl, "MA10_CSEL", stage == SU_RANDOM ? 14 : 6 + stage, (x0 + x1) / 2 - bw / 2, (y0 + y1) / 2 - bh / 2, bw, bh, MmRGBA(255, 255, 255, 0.95f));
    if (chara == SU_RANDOM) {
        float ph = (y1 - y0) * 0.8f, pw = ph * 0.6875f;
        MmImg(dl, "MA10_CSEL", 48, (x0 + x1) / 2 - pw / 2, y1 - ph - 24, pw, ph);
    } else {
        ImVec2 ss;
        void* tex = art::GetChara(chara, colour, &ss);
        if (tex && ss.y > 0) {
            float ph = (y1 - y0) - 50, pw = ph * ss.x / ss.y;
            if (pw > x1 - x0 - 10) { pw = x1 - x0 - 10; ph = pw * ss.y / ss.x; }
            float cx = (x0 + x1) / 2;
            { ImVec2 c0 = CP(cx, y1 - 36), pts[32]; float rx = CV(pw * 0.42f), ry = CV(14);
              for (int q = 0; q < 32; q++) { float t = q * 6.2831853f / 32; pts[q] = ImVec2(c0.x + cosf(t) * rx, c0.y + sinf(t) * ry); }
              dl->AddConvexPolyFilled(pts, 32, MmRGBA(0, 0, 0, 0.35f)); }
            SuTex(dl, tex, cx - pw / 2, y1 - ph - 28, pw, ph);
        } else MmText(dl, g_fUi, 22, (x0 + x1) / 2, (y0 + y1) / 2, MmC(P.faint), "Loading...", 1);
    }
    dl->PopClipRect();
    dl->AddRect(CP(x0, y0), CP(x1, y1), MmC(P.gold, 0.6f), CV(3), 0, CV(1.5f));
}

// ---- input helpers ----
static int SuDelay() {
#ifndef UI_HARNESS
    return g_ncfg.delay;
#else
    return 2;
#endif
}
// Build 122: each player's own input delay (rollback.ini [net] input_delay). A change counts
// from the next match (the netcode locks the delay at the start of each screen).
static void SuDelayChange(int dx) {
#ifndef UI_HARNESS
    int d = g_ncfg.delay + dx;
    if (d < 0 || d > 8) return;
    g_ncfg.delay = d;
    wchar_t ini[MAX_PATH], w[8];
    SuIniPath(ini);
    _snwprintf(w, 8, L"%d", d);
    WritePrivateProfileStringW(L"net", L"input_delay", w, ini);
    Log("setup: input delay %d (from the next match)", d);
#endif
    MM_SND |= 1;
}
static void SuStageStep(int dx) {   // stages 0-7, then random
    int c = SU.stage == SU_RANDOM ? SU_NSTAGE : SU.stage;
    c = (c + dx + SU_NSTAGE + 1) % (SU_NSTAGE + 1);
    SU.stage = c == SU_NSTAGE ? SU_RANDOM : c;
    SU.dirty = true;
    MM_SND |= 1;
}
static void SuSetChara(int i) {   // 0-12, 13 = random
    SU.charCur = i;
    SU.chara = i >= SU_NCHAR ? SU_RANDOM : i;
    SU.presetCur = SuCurList().inUse;
    SU.dirty = true;
}
static void SuPutAssist(int slot, int a) {
    SuPreset& p = SuCurList().p[SU.presetCur];
    for (int k = 0; k < 3; k++)
        if (k != slot && p.a[k] == a) p.a[k] = p.a[slot];   // already in the preset: the two swap places
    p.a[slot] = a;
    SU.dirty = true;
}
static void SuGridFind(int a) {   // put the grid cursor on Knight a
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 9; c++)
            if (SU_GRID[r][c] == a) { SU.gr = r; SU.gc = c; return; }
}
static void SuGridMove(int dx, int dy) {   // skips the empty spots, wraps around like the game's grid
    if (dx) {
        int c = SU.gc;
        for (int i = 0; i < 9; i++) { c = (c + dx + 9) % 9; if (SU_GRID[SU.gr][c] >= 0) break; }
        SU.gc = c;
    }
    if (dy) {
        int r = (SU.gr + dy + 4) % 4;
        if (SU_GRID[r][SU.gc] < 0) {   // nearest card in that row
            int best = -1;
            for (int d = 1; d < 9 && best < 0; d++) {
                if (SU.gc - d >= 0 && SU_GRID[r][SU.gc - d] >= 0) best = SU.gc - d;
                else if (SU.gc + d < 9 && SU_GRID[r][SU.gc + d] >= 0) best = SU.gc + d;
            }
            if (best >= 0) SU.gc = best;
        }
        SU.gr = r;
    }
}
static int SuRandomKnight(const SuPreset& p, int slot) {   // any Knight not in the other two slots
    for (int tries = 0; tries < 64; tries++) {
        int a = (int)(GetTickCount() * 2654435761u >> 7) % 31;
        a = (a + tries * 7) % 31;
        bool used = false;
        for (int k = 0; k < 3; k++) if (k != slot && p.a[k] == a) used = true;
        if (!used) return a;
    }
    return p.a[slot];
}
static void SuOpenGrid(int slot) {
    SU.pick = slot;
    SuGridFind(SuCurList().p[SU.presetCur].a[slot]);
}
static void SuLeave() {
    if (SU.dirty) SuSave();
    SuApply();
    MM_SND |= 4;
    MmGoMenu();
}
static void SuGoSummary() {
    if (!SU.done) { SU.done = true; SU.dirty = true; }
    SU.step = 3; SU.fromSum = false;
}
static void SuGoStep(int s) {   // A on a summary line
    SU.fromSum = true;
    if (s == 1 && SU.chara == SU_RANDOM) s = 0;
    SU.step = s;
    if (s == 0) SU.charCur = SU.chara == SU_RANDOM ? SU_NCHAR : SU.chara;
    if (s == 2) SU.presetCur = SuCurList().inUse;
}
static void SuNext() {   // A on a step: on to the next one (or back to the summary)
    MM_SND |= 2;
    if (SU.fromSum) { SuGoSummary(); return; }
    int s = SU.step + 1;
    if (s == 1 && SU.chara == SU_RANDOM) s = 2;   // random character: no colour to pick
    if (s >= 3) { SuGoSummary(); return; }
    SU.step = s;
    if (s == 2) SU.presetCur = SuCurList().inUse;
}
static void SuBack() {   // B on a step
    MM_SND |= 4;
    if (SU.fromSum || SU.done) {
        if (SU.step == 0 || SU.fromSum) { SuGoSummary(); return; }
    }
    if (SU.step == 0) { SuLeave(); return; }
    int s = SU.step - 1;
    if (s == 1 && SU.chara == SU_RANDOM) s = 0;
    SU.step = s;
}

// ---- shared page parts ----
static const char* SU_STEP_NAMES[4] = {"1  CHARACTER", "2  COLOUR", "3  SUPPORT KNIGHTS", "SUMMARY"};
static void SuStepBar(ImDrawList* dl, int step, float in) {
    const MmPal& P = MmP();
    const float px = 13, gap = 66;
    float w[4], total = 0;
    for (int i = 0; i < 4; i++) { w[i] = SuTW(g_fMenu, px, SU_STEP_NAMES[i]); total += w[i]; }
    total += gap * 3;
    float x = 1860 - total, y = 56;
    for (int i = 0; i < 4; i++) {
        bool on = i == step, done = i < step;
        if (on) {
            dl->AddRectFilled(CP(x - 16, y - 19), CP(x + w[i] + 16, y + 19), MmC(P.gold, in), CV(19));
            dl->AddRect(CP(x - 16, y - 19), CP(x + w[i] + 16, y + 19), MmC(P.selOut, in), CV(19), 0, CV(2));
        }
        MmText(dl, g_fMenu, px, x, y, on ? MmC(P.ink, in) : MmC(done ? P.dim : P.faint, in), SU_STEP_NAMES[i], 0, 0, 0, on ? 0.4f : 0);
        if (i < 3) MmLine(dl, x + w[i] + 22, y, gap - 44, 2, MmC(P.acc, 0.85f * in));
        x += w[i] + gap;
    }
}
static void SuPrompts(ImDrawList* dl, const char* help, const char* const* labels, const char* keys) {
    // keys: one letter per label: A B X Y (pad badges), S = Start, L = LB / RB, R = left / right, V = View
    const MmPal& P = MmP();
    dl->PushClipRect(ImVec2(0, 0), g_disp, false);
    MmLine(dl, -g_cvO.x / g_cv, 960, g_disp.x / g_cv, 3, MmC(P.acc, 0.95f));
    MmLine(dl, -g_cvO.x / g_cv, 964, g_disp.x / g_cv, 1, MmC(P.gold, 0.6f));
    dl->PopClipRect();
    MmText(dl, g_fUi, 26, 60, 994, MmC(P.text), help);
    float x = 60;
    for (int i = 0; keys[i]; i++) {
        char k = keys[i];
        ImU32 col = k == 'A' ? MmRGBA(40, 160, 70) : k == 'B' ? MmRGBA(190, 40, 40) : k == 'X' ? MmRGBA(40, 100, 200) : MmRGBA(220, 180, 30);
        if (k == 'A' || k == 'B' || k == 'X' || k == 'Y') {
            MmPadButton(dl, x, 1022, k, col);
            x += 44 + MmText(dl, g_fMenu, 13, x + 44, 1040, MmC(P.text), labels[i]) + 50;
        } else {
            const char* kt = k == 'S' ? "START" : k == 'L' ? "LB / RB" : k == 'V' ? "VIEW" : "LEFT / RIGHT";
            float kw = MmText(dl, g_fMenu, 13, x, 1040, MmC(P.gold), kt);
            float lw = MmText(dl, g_fMenu, 13, x + kw + 12, 1040, MmC(P.text), labels[i]);
            if ((k == 'S' || k == 'V') && MmClicked(x - 6, 1020, x + kw + lw + 18, 1060)) { g_padClick = k; g_padClickFrame = ImGui::GetFrameCount(); }
            x += kw + 12 + lw + 50;
        }
    }
}
// right-hand preview on the character and colour steps
static void SuPreviewPanel(ImDrawList* dl, float x0, float y0, float x1, float y1) {
    const MmPal& P = MmP();
    MmSection(dl, x0, y0, x1, y1, "PREVIEW");
    float px0 = x0 + 18, px1 = x1 - 18, py0 = y0 + 18, py1 = y0 + 488;
    SuStagePreview(dl, px0, py0, px1, py1, SU.chara, SuColor(), SU.stage);
    float cx = (x0 + x1) / 2;
    const char* nm = SU.chara == SU_RANDOM ? "Random character" : SU_CHARS[SU.chara];
    MmText(dl, g_fUiBold, strlen(nm) > 22 ? 32.0f : 40.0f, cx, py1 + 44, MmC(P.text), nm, 1, 2, MmC(P.ink));
    char sub[32];
    if (SU.chara == SU_RANDOM) snprintf(sub, sizeof sub, "Random colour"); else snprintf(sub, sizeof sub, "Colour %d", SuColor() + 1);
    MmText(dl, g_fUi, 26, cx, py1 + 88, MmC(P.dim), sub, 1);
    float ky = py1 + 128;
    MmText(dl, g_fMenu, 12, px0 + 4, ky, MmC(P.faint), "SUPPORT KNIGHTS");
    MmText(dl, g_fUiBold, 24, px1 - 4, ky, MmC(P.gold), SuInUse().name, 2);
    float kw = (px1 - px0 - 24) / 3, sc = kw / 192;
    for (int k = 0; k < 3; k++) SuSmallCard(dl, px0 + k * (kw + 12), ky + 22, sc, SuInUse().a[k]);
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
    bool gridWasOpen = SU.pick >= 0;
    SuList& L = SuCurList();
    if (SU.presetCur < 0 || SU.presetCur > L.n) SU.presetCur = L.inUse;
    double now = ImGui::GetTime();
    if (SU.delArm >= 0 && now > SU.delAt + 3) SU.delArm = -1;

    // ---- keys / controller ----
    if (SU.rename >= 0) {
        if (!SU.focusName && ImGui::GetFrameCount() > SU.renameFrame + 3 && ImGui::GetActiveID() == 0) { SU.rename = -1; SU.dirty = true; MM_SND |= 2; nav = MmNav(); click = false; }
    } else if (nav.start) {
        SuGoSummary(); SuLeave(); return;
    } else if (SU.pick >= 0) {   // the Knight grid
        SuPreset& pr = L.p[SU.presetCur];
        if (nav.b) { SU.pick = -1; MM_SND |= 4; }
        else {
            if (nav.lb) { SuOpenGrid((SU.pick + 2) % 3); MM_SND |= 1; }
            if (nav.rb) { SuOpenGrid((SU.pick + 1) % 3); MM_SND |= 1; }
            if (nav.dx || nav.dy) { SuGridMove(nav.dx, nav.dy); MM_SND |= 1; }
            int a = SU_GRID[SU.gr][SU.gc];
            if (nav.y) a = SU_GRID_RANDOM;
            if (nav.a || nav.y) {
                if (a == SU_GRID_RANDOM) a = SuRandomKnight(pr, SU.pick);
                if (a >= 0 && a < 31) {
                    SuPutAssist(SU.pick, a); MM_SND |= 2;
                    if (SU.pick < 2) SuOpenGrid(SU.pick + 1); else SU.pick = -1;
                }
            }
        }
    } else if (SU.step == 0) {   // character: 7 x 2, the 14th = random
        if (nav.b) { SuBack(); }
        else {
            if (nav.dx || nav.dy) {
                int c = SU.charCur, col = (c % 7 + nav.dx + 7) % 7, row = (c / 7 + nav.dy + 2) % 2;
                SuSetChara(row * 7 + col); MM_SND |= 1;
            }
            if (nav.a) SuNext();
        }
    } else if (SU.step == 1) {   // colour: 7 x 3
        if (nav.b) { SuBack(); }
        else {
            if (nav.dx || nav.dy) {
                int c = L.color, col = (c % 7 + nav.dx + 7) % 7, row = (c / 7 + nav.dy + 3) % 3;
                L.color = row * 7 + col; SU.dirty = true; MM_SND |= 1;
            }
            if (nav.a) SuNext();
        }
    } else if (SU.step == 2) {   // the character's presets, then "new preset"
        int rows = L.n + (L.n < SU_NPRESET ? 1 : 0);
        if (nav.b) { SuBack(); }
        else {
            if (nav.dy) { SU.presetCur = (SU.presetCur + nav.dy + rows) % rows; SU.delArm = -1; MM_SND |= 1; }
            bool onNew = SU.presetCur >= L.n;
            if (nav.a) {
                if (onNew) {   // a new preset: a copy of the one in use, straight into the grid
                    L.p[L.n] = L.p[L.inUse];
                    snprintf(L.p[L.n].name, sizeof L.p[L.n].name, "Preset %d", L.n + 1);
                    SU.presetCur = L.n; L.n++; L.inUse = SU.presetCur; SU.dirty = true;
                    SuOpenGrid(0); MM_SND |= 2;
                } else {
                    if (L.inUse != SU.presetCur) { L.inUse = SU.presetCur; SU.dirty = true; }
                    SuNext();
                }
            }
            if (!onNew && nav.x) { SuOpenGrid(0); MM_SND |= 2; }
            if (!onNew && nav.y) { SU.rename = SU.presetCur; SU.focusName = true; SU.renameFrame = ImGui::GetFrameCount(); MM_SND |= 2; click = false; }
            if (!onNew && nav.back) {
                if (L.n <= 1) MmNote("A character needs at least one preset.");
                else if (SU.delArm != SU.presetCur) { SU.delArm = SU.presetCur; SU.delAt = now; MmNote("Press VIEW (Delete) again to delete this preset."); MM_SND |= 1; }
                else {
                    for (int i = SU.presetCur; i < L.n - 1; i++) L.p[i] = L.p[i + 1];
                    L.n--;
                    if (L.inUse == SU.presetCur) L.inUse = 0; else if (L.inUse > SU.presetCur) L.inUse--;
                    if (SU.presetCur >= L.n) SU.presetCur = L.n - 1;
                    SU.delArm = -1; SU.dirty = true; MM_SND |= 4;
                    MmNote("Preset deleted.");
                }
            }
        }
    } else {   // summary
        if (nav.b) { SuLeave(); return; }
        if (nav.dy) { SU.sumRow = (SU.sumRow + nav.dy + 5) % 5; MM_SND |= 1; }
        if (nav.dx && SU.sumRow == 3) SuStageStep(nav.dx);
        if (nav.dx && SU.sumRow == 4) SuDelayChange(nav.dx);
        if (nav.a) {
            if (SU.sumRow <= 2) { MM_SND |= 2; SuGoStep(SU.sumRow); }
            else if (SU.sumRow == 3) SuStageStep(1);
        }
    }

    ImDrawList* dl = MmPageBegin("##setup", T.tm, T.pulse);
    auto ease = [&](float s, float l) { return MmEase(T, s, l); };
    MmPageTitle(dl, "MAIN MENU", "MY SETUP", ease(0.0f, 0.5f));
    SuStepBar(dl, SU.step, ease(0.05f, 0.5f));
    bool mouseOk = click && SU.rename < 0;
    const char* chName = SU.chara == SU_RANDOM ? "Random character" : SU_CHARS[SU.chara];

    if (SU.pick >= 0) {
        // ================= the Knight grid =================
        SuPreset& pr = L.p[SU.presetCur];
        char note[96]; snprintf(note, sizeof note, "Editing %s's preset \"%s\"", chName, pr.name);
        MmText(dl, g_fUi, 22, 1860, 98, MmC(P.dim), note, 2);
        int hov = SU_GRID[SU.gr][SU.gc];
        // the three slots as big cards (the slot being filled shows the Knight under the cursor)
        const float cw = 300, sc = cw / 356, chh = 487 * sc, x0 = 60, y0 = 150;
        for (int j = 0; j < 3; j++) {
            float x = x0 + j * (cw + 22);
            bool cur = j == SU.pick;
            int a = cur ? (hov == SU_GRID_RANDOM ? -1 : hov) : pr.a[j];
            if (cur) SuSelFrame(dl, x, y0, x + cw, y0 + chh, T.pulse);
            SuBigCard(dl, x, y0, sc, a);
            if (cur && hov == SU_GRID_RANDOM) MmText(dl, g_fMenu, 22, x + cw / 2, y0 + chh / 2, MmC(P.text), "RANDOM", 1, 2, MmC(P.ink));
            MmPill(dl, x + cw / 2, y0 + chh + 26, 52, 32, j == 0 ? "A" : j == 1 ? "B" : "C", cur ? P.gold : P.faint, cur);
            if (gridWasOpen && mouseOk && MmHover(x, y0, x + cw, y0 + chh)) { SuOpenGrid(j); MM_SND |= 1; }
        }
        // the preset: names, costs, total
        {
            float bx0 = 1050, by0 = 176, bx1 = 1860, by1 = 560;
            MmSection(dl, bx0, by0, bx1, by1, pr.name);
            float tx = bx0 + 36, ty = by0 + 46;
            int total = 0;
            for (int j = 0; j < 3; j++) {
                bool cur = j == SU.pick;
                int a = cur ? hov : pr.a[j];
                char lt[2] = {(char)('A' + j), 0};
                MmText(dl, g_fMenu, 17, tx, ty, cur ? MmC(P.gold) : MmC(P.faint), lt);
                char nm[40], cs[16];
                if (a == SU_GRID_RANDOM) { snprintf(nm, sizeof nm, "Random Knight"); cs[0] = 0; }
                else { snprintf(nm, sizeof nm, "%s", SU_ASSISTS[a]); snprintf(cs, sizeof cs, "Cost %d", SuCost(a)); total += SuCost(a); }
                MmText(dl, g_fUiBold, 28, tx + 46, ty, cur ? MmC(P.gold) : MmC(P.text), nm);
                MmText(dl, g_fUi, 24, bx1 - 36, ty, MmC(P.dim), cs, 2);
                ty += 58;
            }
            MmLine(dl, bx0 + 30, ty, bx1 - bx0 - 60, 1, MmC(P.acc, 0.6f));
            char tc[32]; snprintf(tc, sizeof tc, "TOTAL COST  %d", total);
            MmText(dl, g_fMenu, 19, tx, ty + 44, MmC(P.gold), tc);
            char hint[80]; snprintf(hint, sizeof hint, "Choosing for slot %c. A Knight can be in a preset only once.", 'A' + SU.pick);
            MmText(dl, g_fUi, 23, tx, ty + 92, MmC(P.faint), hint);
            MmText(dl, g_fUi, 23, tx, ty + 126, MmC(P.faint), "Same grid as the game: fire, water, wind and light rows.");
        }
        // the grid, like the game's: 9 columns, rows fire / water / wind / light
        const float gw = 192, gh = 77, gx = 8, gy = 6, X0 = (1920 - (9 * gw + 8 * gx)) / 2, Y0 = 612;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 9; c++) {
                int a = SU_GRID[r][c];
                if (a < 0) continue;
                float x = X0 + c * (gw + gx), y = Y0 + r * (gh + gy);
                bool on = r == SU.gr && c == SU.gc;
                if (on) SuSelFrame(dl, x, y, x + gw, y + gh, T.pulse, 3);
                SuSmallCard(dl, x, y, 1, a, !on);
                if (a != SU_GRID_RANDOM)
                    for (int k = 0; k < 3; k++)
                        if (k != SU.pick && pr.a[k] == a) {
                            dl->AddRectFilled(CP(x + 4, y + 4), CP(x + 40, y + 32), MmC(P.gold), CV(12));
                            dl->AddRect(CP(x + 4, y + 4), CP(x + 40, y + 32), MmC(P.selOut), CV(12), 0, CV(2));
                            char lt[2] = {(char)('A' + k), 0};
                            MmText(dl, g_fMenu, 13, x + 22, y + 18, MmC(P.ink), lt, 1);
                        }
                if (MmHover(x, y, x + gw, y + gh) && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || (gridWasOpen && mouseOk))) {
                    if (!on) MM_SND |= 1;
                    SU.gr = r; SU.gc = c;
                    if (gridWasOpen && mouseOk && SU.pick >= 0) {
                        int put = a == SU_GRID_RANDOM ? SuRandomKnight(pr, SU.pick) : a;
                        SuPutAssist(SU.pick, put); MM_SND |= 2;
                        if (SU.pick < 2) SuOpenGrid(SU.pick + 1); else SU.pick = -1;
                    }
                }
            }
        static const char* lab[4] = {"Put in slot", "Done", "Slot", "Random Knight"};
        char put[24]; snprintf(put, sizeof put, "Put in slot %c", 'A' + (SU.pick >= 0 ? SU.pick : 0));
        const char* labs[4] = {put, lab[1], lab[2], lab[3]};
        SuPrompts(dl, "Pick a Support Knight. The big card shows the one under the cursor.", labs, "ABLY");
    } else if (SU.step == 0) {
        // ================= 1 character =================
        MmText(dl, g_fUi, 22, 1860, 98, MmC(P.dim), "Used for every match you play. Saved on this PC.", 2);
        MmSection(dl, 60, 176, 1180, 940, "CHARACTER");
        const float cw = 136, ch = 200, gap = 14, gx = 60 + (1120 - (7 * cw + 6 * gap)) / 2, gy = 214;
        for (int i = 0; i < 14; i++) {
            float x = gx + (i % 7) * (cw + gap), y = gy + (i / 7) * (ch + gap + 44);
            bool rnd = i == SU_NCHAR, on = i == SU.charCur;
            if (on) SuSelFrame(dl, x, y, x + cw, y + ch, T.pulse);
            MmImg(dl, "MA10_CSEL", rnd ? 48 : SU_PORTRAIT[i], x, y, cw, ch, on ? IM_COL32_WHITE : MmRGBA(190, 170, 170));
            if (!on) dl->AddRect(CP(x, y), CP(x + cw, y + ch), MmC(P.acc, 0.55f), CV(3), 0, CV(1.5f));
            char nm[40]; snprintf(nm, sizeof nm, "%s", rnd ? "Random" : SU_CHARS[i]);
            if (!strncmp(nm, "Otherworldly ", 13)) memmove(nm, nm + 13, strlen(nm + 13) + 1);
            if (!strncmp(nm, "Blade-Protector", 15)) snprintf(nm, sizeof nm, "Blade-Prot. Arthur");
            float px = 19;
            while (px > 13 && SuTW(g_fUi, px, nm) > cw + 10) px -= 1;
            MmText(dl, g_fUi, px, x + cw / 2, y + ch + 20, on ? MmC(P.gold) : MmC(P.dim), nm, 1);
            if (mouseOk && MmHover(x, y, x + cw, y + ch)) { if (i == SU.charCur) SuNext(); else { SuSetChara(i); MM_SND |= 1; } }
        }
        MmLine(dl, 90, 720, 1060, 1, MmC(P.acc, 0.6f));
        char up[48]; snprintf(up, sizeof up, "%s", chName);
        for (char* c = up; *c; c++) *c = (char)toupper((unsigned char)*c);
        MmText(dl, g_fMenu, 20, 100, 756, MmC(P.gold), up);
        char last[96];
        if (SU.chara == SU_RANDOM) snprintf(last, sizeof last, "Support Knights \"%s\" (random has its own presets).", SuInUse().name);
        else snprintf(last, sizeof last, "Last used: colour %d, Support Knights \"%s\".", SuColor() + 1, SuInUse().name);
        MmText(dl, g_fUi, 25, 100, 802, MmC(P.dim), last);
        MmText(dl, g_fUi, 25, 100, 840, MmC(P.faint), "Each character keeps its own colour and Support Knight presets.");
        SuPreviewPanel(dl, 1220, 176, 1860, 940);
        const char* labs[3] = {SU.fromSum ? "Done" : (SU.chara == SU_RANDOM ? "Next: Support Knights" : "Next: colour"), SU.done || SU.fromSum ? "Back to summary" : "Back", "Save and close"};
        SuPrompts(dl, "Pick the character you play with.", labs, "ABS");
    } else if (SU.step == 1) {
        // ================= 2 colour =================
        MmText(dl, g_fUi, 22, 1860, 98, MmC(P.dim), "Used for every match you play. Saved on this PC.", 2);
        char t[32]; snprintf(t, sizeof t, "COLOUR  %d / 21", L.color + 1);
        MmSection(dl, 60, 176, 1180, 940, t);
        const float sq = 118, gap = 18, gx = 60 + (1120 - (7 * sq + 6 * gap)) / 2, gy = 222;
        int who = SU.chara == SU_RANDOM ? -1 : SU.chara;
        for (int i = 0; i < 21; i++) {
            float x = gx + (i % 7) * (sq + gap), y = gy + (i / 7) * (sq + gap);
            bool on = i == L.color;
            if (on) SuSelFrame(dl, x, y, x + sq, y + sq, T.pulse);
            dl->AddRectFilled(CP(x, y), CP(x + sq, y + sq), MmRGBA(48, 10, 8), CV(4));
            if (who >= 0) SuHead(dl, who, i, x + 4, y + 4, sq - 8, sq - 8);
            if (!on) dl->AddRect(CP(x, y), CP(x + sq, y + sq), MmC(P.acc, 0.55f), CV(4), 0, CV(1.5f));
            char n[4]; snprintf(n, sizeof n, "%d", i + 1);
            MmText(dl, g_fMenu, 11, x + sq - 8, y + sq - 12, on ? MmC(P.gold) : MmC(P.faint), n, 2, 1.5f, MmC(P.ink));
            if (mouseOk && MmHover(x, y, x + sq, y + sq)) { if (on) SuNext(); else { L.color = i; SU.dirty = true; MM_SND |= 1; } }
        }
        MmLine(dl, 90, 720, 1060, 1, MmC(P.acc, 0.6f));
        char up[64]; snprintf(up, sizeof up, "%s  -  COLOUR %d", chName, L.color + 1);
        for (char* c = up; *c; c++) *c = (char)toupper((unsigned char)*c);
        MmText(dl, g_fMenu, 20, 100, 756, MmC(P.gold), up);
        MmText(dl, g_fUi, 25, 100, 802, MmC(P.dim), "If your opponent picks the same character and colour,");
        MmText(dl, g_fUi, 25, 100, 838, MmC(P.dim), "the game gives one of you the next free colour.");
        SuPreviewPanel(dl, 1220, 176, 1860, 940);
        const char* labs[3] = {SU.fromSum ? "Done" : "Next: Support Knights", SU.fromSum ? "Back to summary" : "Back: character", "Save and close"};
        SuPrompts(dl, "Pick a colour. The preview shows it on your character.", labs, "ABS");
    } else if (SU.step == 2) {
        // ================= 3 Support Knights: this character's presets =================
        MmText(dl, g_fUi, 22, 1860, 98, MmC(P.dim), "Used for every match you play. Saved on this PC.", 2);
        char t[64];
        if (SU.chara == SU_RANDOM) snprintf(t, sizeof t, "PRESETS FOR RANDOM CHARACTER");
        else {
            snprintf(t, sizeof t, "%s'S PRESETS", SU_CHARS[SU.chara]);
            for (char* c = t; *c; c++) *c = (char)toupper((unsigned char)*c);
        }
        MmSection(dl, 60, 176, 1180, 940, t);
        int rows = L.n + (L.n < SU_NPRESET ? 1 : 0);
        float rh = ImMin(132.0f, 690.0f / rows), rx0 = 90, rx1 = 1150;
        float csc = ImMin(1.0f, (rh - 18) / 77);
        float y = 206;
        for (int i = 0; i < rows; i++) {
            float ry0 = y, ry1 = y + rh - 12;
            bool cur = i == SU.presetCur;
            if (i >= L.n) {   // new preset
                dl->AddRect(CP(rx0, ry0), CP(rx1, ry1), MmC(cur ? P.gold : P.acc, cur ? 1.0f : 0.5f), CV(8), 0, CV(cur ? 3 : 1.5f));
                if (cur) MmImg(dl, "MA10_CMN", 83, rx0 - 20, ry0 - 14, rx1 - rx0 + 40, ry1 - ry0 + 28, MmGlowCol(P.glow, 0.3f), ImVec2(0, 0), ImVec2(1, 1), true);
                MmText(dl, g_fMenu, 17, (rx0 + rx1) / 2, (ry0 + ry1) / 2, cur ? MmC(P.gold) : MmC(P.faint), "+  NEW PRESET", 1);
                if (mouseOk && MmHover(rx0, ry0, rx1, ry1)) { SU.presetCur = i; nav = MmNav(); MM_SND |= 1; }
                y += rh;
                continue;
            }
            const SuPreset& pr = L.p[i];
            bool use = i == L.inUse;
            if (cur) {
                MmImg(dl, "MA10_CMN", 83, rx0 - 20, ry0 - 14, rx1 - rx0 + 40, ry1 - ry0 + 28, MmGlowCol(P.glow, 0.4f), ImVec2(0, 0), ImVec2(1, 1), true);
                dl->AddRectFilled(CP(rx0, ry0), CP(rx1, ry1), MmRGBA(70, 14, 8, 0.85f), CV(8));
                dl->AddRect(CP(rx0, ry0), CP(rx1, ry1), MmC(P.gold), CV(8), 0, CV(3));
            } else dl->AddRect(CP(rx0, ry0), CP(rx1, ry1), MmC(P.acc, 0.55f), CV(8), 0, CV(1.5f));
            float cy = (ry0 + ry1) / 2;
            char n[4]; snprintf(n, sizeof n, "%d", i + 1);
            MmText(dl, g_fMenu, 24, rx0 + 34, cy, use || cur ? MmC(P.gold) : MmC(P.faint), n, 1);
            int total = SuCost(pr.a[0]) + SuCost(pr.a[1]) + SuCost(pr.a[2]);
            char tc[24]; snprintf(tc, sizeof tc, "Total cost %d", total);
            if (SU.rename == i) {
                if (MmTextField("##presetname", L.p[i].name, sizeof L.p[i].name, rx0 + 64, cy - 22, rx0 + 300, cy + 22, "Name of the preset", SU.focusName)) {
                    ImGui::ClearActiveID();
                    SU.rename = -1; SU.dirty = true; MM_SND |= 2;
                }
                SU.focusName = false;
            } else if (rh >= 100) {
                MmText(dl, g_fUiBold, 30, rx0 + 70, cy - 20, cur ? MmC(P.gold) : MmC(P.text), pr.name);
                MmText(dl, g_fUi, 23, rx0 + 70, cy + 20, MmC(P.dim), tc);
            } else {
                MmText(dl, g_fUiBold, 24, rx0 + 70, cy - 12, cur ? MmC(P.gold) : MmC(P.text), pr.name);
                MmText(dl, g_fUi, 18, rx0 + 70, cy + 14, MmC(P.dim), tc);
            }
            for (int k = 0; k < 3; k++) SuSmallCard(dl, rx0 + 310 + k * (192 * csc + 16), cy - 77 * csc / 2, csc, pr.a[k], !(cur || use));
            if (use) MmPill(dl, rx1 - 74, ry0 + 24, 116, 28, "IN USE", P.gold, true);
            if (SU.delArm == i) MmPill(dl, rx1 - 74, ry1 - 22, 116, 26, "DELETE?", P.acc, true);
            if (mouseOk && MmHover(rx0, ry0, rx1, ry1)) {
                if (SU.presetCur == i && MmHover(rx0 + 310, ry0, rx0 + 310 + 3 * (192 * csc + 16), ry1)) { SuOpenGrid(0); MM_SND |= 2; }
                else if (SU.presetCur == i) { if (L.inUse != i) { L.inUse = i; SU.dirty = true; } SuNext(); }
                else { SU.presetCur = i; MM_SND |= 1; }
            }
            y += rh;
        }
        char cnt[96]; snprintf(cnt, sizeof cnt, "%d of %d presets used. These presets belong to %s only.", L.n, SU_NPRESET, SU.chara == SU_RANDOM ? "random character" : SU_CHARS[SU.chara]);
        MmText(dl, g_fUi, 21, 100, 912, MmC(P.faint), cnt);
        // right: the preset under the cursor as the game's big cards
        {
            int show = SU.presetCur < L.n ? SU.presetCur : L.inUse;
            const SuPreset& pr = L.p[show];
            float bx0 = 1220, bx1 = 1860;
            MmSection(dl, bx0, 176, bx1, 940, pr.name);
            const float cw = 194, sc = cw / 356, ch = 487 * sc;
            for (int j = 0; j < 3; j++) {
                float x = bx0 + 18 + j * (cw + 11);
                SuBigCard(dl, x, 214, sc, pr.a[j]);
                char lt[2] = {(char)('A' + j), 0};
                MmText(dl, g_fMenu, 16, x + cw / 2, 214 + ch + 26, MmC(P.gold), lt, 1);
            }
            float ty = 214 + ch + 70;
            for (int j = 0; j < 3; j++) {
                char ln[64]; snprintf(ln, sizeof ln, "%c   %s", 'A' + j, SU_ASSISTS[pr.a[j]]);
                MmText(dl, g_fUi, 22, bx0 + 34, ty, MmC(P.text), ln);
                char cs[12]; snprintf(cs, sizeof cs, "Cost %d", SuCost(pr.a[j]));
                MmText(dl, g_fUi, 22, bx1 - 34, ty, MmC(P.dim), cs, 2);
                ty += 34;
            }
            MmLine(dl, bx0 + 30, ty + 2, bx1 - bx0 - 60, 1, MmC(P.acc, 0.6f));
            char tc[24]; snprintf(tc, sizeof tc, "TOTAL COST  %d", SuCost(pr.a[0]) + SuCost(pr.a[1]) + SuCost(pr.a[2]));
            MmText(dl, g_fMenu, 18, bx0 + 34, ty + 40, MmC(P.gold), tc);
            MmText(dl, g_fUi, 22, bx0 + 34, ty + 82, MmC(P.faint), "X opens the Knight grid to change them.");
        }
        if (SU.rename >= 0) {
            const char* labs[1] = {"Done"};
            SuPrompts(dl, "Type a name with the keyboard, then press Enter.", labs, "A");
        } else if (SU.presetCur >= L.n) {
            const char* labs[3] = {"New preset", SU.fromSum ? "Back to summary" : "Back: colour", "Save and close"};
            SuPrompts(dl, "Make a new preset for this character (starts as a copy of the one in use).", labs, "ABS");
        } else {
            const char* labs[6] = {SU.fromSum ? "Use it" : "Use it, then summary", "Edit Knights", "Rename", "Delete", SU.fromSum ? "Back to summary" : "Back: colour", "Save and close"};
            char help[96]; snprintf(help, sizeof help, "Pick the Support Knight preset for %s.", SU.chara == SU_RANDOM ? "random character" : SU_CHARS[SU.chara]);
            SuPrompts(dl, help, labs, "AXYVBS");
        }
    } else {
        // ================= summary =================
        MmText(dl, g_fUi, 22, 1860, 98, MmC(P.dim), "Used for every match you play. Saved on this PC.", 2);
        MmSection(dl, 60, 176, 700, 940, "PREVIEW");
        SuStagePreview(dl, 78, 194, 682, 754, SU.chara, SuColor(), SU.stage);
        MmText(dl, g_fUiBold, strlen(chName) > 22 ? 32.0f : 40.0f, 380, 800, MmC(P.text), chName, 1, 2, MmC(P.ink));
        char sub[64], stg[24];
        if (SU.stage == SU_RANDOM) snprintf(stg, sizeof stg, "Random stage"); else snprintf(stg, sizeof stg, "Stage %02d", SU.stage + 1);
        if (SU.chara == SU_RANDOM) snprintf(sub, sizeof sub, "%s", stg); else snprintf(sub, sizeof sub, "Colour %d  -  %s", SuColor() + 1, stg);
        MmText(dl, g_fUi, 26, 380, 846, MmC(P.dim), sub, 1);

        MmSection(dl, 740, 176, 1860, 940, "YOUR SETUP");
        static const float hts[5] = {100, 100, 170, 150, 160};
        static const char* lab[5] = {"CHARACTER", "COLOUR", "SUPPORT KNIGHTS", "STAGE", "INPUT DELAY"};
        float y = 206, rx0 = 770, rx1 = 1830;
        for (int i = 0; i < 5; i++) {
            float ry0 = y, ry1 = y + hts[i] - 14, cy = (ry0 + ry1) / 2;
            bool on = i == SU.sumRow;
            bool off = i == 1 && SU.chara == SU_RANDOM;
            if (on) {
                MmImg(dl, "MA10_CMN", 83, rx0 - 20, ry0 - 14, rx1 - rx0 + 40, ry1 - ry0 + 28, MmGlowCol(P.glow, 0.4f), ImVec2(0, 0), ImVec2(1, 1), true);
                dl->AddRectFilled(CP(rx0, ry0), CP(rx1, ry1), MmRGBA(70, 14, 8, 0.85f), CV(8));
                dl->AddRect(CP(rx0, ry0), CP(rx1, ry1), MmC(P.gold), CV(8), 0, CV(3));
            } else dl->AddRect(CP(rx0, ry0), CP(rx1, ry1), MmC(P.acc, 0.55f), CV(8), 0, CV(1.5f));
            MmText(dl, g_fMenu, 14, rx0 + 30, i <= 1 ? cy - 21 : ry0 + 30, on ? MmC(P.gold) : MmC(P.faint), lab[i]);
            if (i <= 2) MmText(dl, g_fUiBold, 34, rx1 - 26, cy, MmC(off ? P.faint : P.dim), ">", 2);
            if (i == 0) {
                MmText(dl, g_fUiBold, 30, rx0 + 30, cy + 14, MmC(P.text), chName);
                MmImg(dl, "MA10_CSEL", SuPortraitOf(SU.chara == SU_RANDOM ? -1 : SU.chara), rx1 - 200, ry0 + 6, 130, 74, IM_COL32_WHITE, ImVec2(0, 0.1f), ImVec2(1, 0.5f));
            } else if (i == 1) {
                char c[32];
                if (off) snprintf(c, sizeof c, "Random"); else snprintf(c, sizeof c, "Colour %d", SuColor() + 1);
                MmText(dl, g_fUiBold, 30, rx0 + 30, cy + 14, MmC(off ? P.faint : P.text), c);
                if (!off) {
                    dl->AddRectFilled(CP(rx1 - 200, ry0 + 6), CP(rx1 - 70, ry0 + 80), MmRGBA(48, 10, 8), CV(4));
                    SuHead(dl, SU.chara, SuColor(), rx1 - 200, ry0 + 6, 130, 74);
                }
            } else if (i == 2) {
                const SuPreset& pr = SuInUse();
                MmText(dl, g_fUiBold, 30, rx0 + 30, ry0 + 70, MmC(P.text), pr.name);
                char tc[24]; snprintf(tc, sizeof tc, "Total cost %d", SuCost(pr.a[0]) + SuCost(pr.a[1]) + SuCost(pr.a[2]));
                MmText(dl, g_fUi, 23, rx0 + 30, ry0 + 108, MmC(P.dim), tc);
                for (int k = 0; k < 3; k++) SuSmallCard(dl, rx0 + 330 + k * 216, cy - 40, 200.0f / 192, pr.a[k]);
            } else if (i == 3) {
                MmImg(dl, "MA10_CSEL", SU.stage == SU_RANDOM ? 14 : 6 + SU.stage, rx0 + 30, ry0 + 50, 104, 66);
                dl->AddRect(CP(rx0 + 30, ry0 + 50), CP(rx0 + 134, ry0 + 116), MmC(P.gold, 0.6f), 0, 0, CV(1.5f));
                MmText(dl, g_fUi, 21, rx0 + 150, ry0 + 70, MmC(P.faint), "In a match: random");
                MmText(dl, g_fUi, 21, rx0 + 150, ry0 + 96, MmC(P.faint), "between both picks");
            } else {
                MmText(dl, g_fUi, 21, rx0 + 30, ry0 + 76, MmC(P.faint), "More delay = fewer rollbacks for you,");
                MmText(dl, g_fUi, 21, rx0 + 30, ry0 + 104, MmC(P.faint), "slower controls. Counts from the next match.");
            }
            if (i >= 3) {   // left / right value
                float vx = rx0 + 600;
                char v[24];
                if (i == 3) snprintf(v, sizeof v, "%s", stg);
                else { int d = SuDelay(); snprintf(v, sizeof v, d == 1 ? "%d frame" : "%d frames", d); }
                ImU32 ac = on ? MmC(P.gold) : MmC(P.faint);
                dl->AddTriangleFilled(CP(vx - 160, cy + 2), CP(vx - 140, cy - 12), CP(vx - 140, cy + 16), ac);
                dl->AddTriangleFilled(CP(vx + 160, cy + 2), CP(vx + 140, cy - 12), CP(vx + 140, cy + 16), ac);
                MmText(dl, g_fUiBold, 32, vx, cy + 2, MmC(P.text), v, 1);
                if (i == 4) {
                    int d = SuDelay();
                    for (int k = 0; k <= 8; k++) {
                        float dx = rx1 - 40 - (8 - k) * 26;
                        dl->AddCircleFilled(CP(dx, cy), CV(7), k <= d ? MmC(P.gold) : MmRGBA(60, 16, 12), 16);
                        dl->AddCircle(CP(dx, cy), CV(7), MmC(P.selOut), 16, CV(1.5f));
                        char n[4]; snprintf(n, sizeof n, "%d", k);
                        MmText(dl, g_fMenu, 9, dx, cy + 22, MmC(P.faint), n, 1);
                    }
                }
                if (mouseOk && MmHover(vx - 180, ry0, vx - 120, ry1)) { SU.sumRow = i; if (i == 3) SuStageStep(-1); else SuDelayChange(-1); }
                else if (mouseOk && MmHover(vx + 120, ry0, vx + 180, ry1)) { SU.sumRow = i; if (i == 3) SuStageStep(1); else SuDelayChange(1); }
                else if (mouseOk && MmHover(rx0, ry0, rx1, ry1)) { SU.sumRow = i; MM_SND |= 1; }
            } else if (mouseOk && MmHover(rx0, ry0, rx1, ry1)) {
                if (SU.sumRow == i && !off) { MM_SND |= 2; SuGoStep(i); } else { SU.sumRow = i; MM_SND |= 1; }
            }
            y += hts[i];
        }
        const char* labs[3] = {SU.sumRow == 3 ? "Next stage" : SU.sumRow == 4 ? "Change" : "Change", "Save and back", "Stage and delay"};
        SuPrompts(dl, "Your setup for every match. Pick a line to change it.", labs, "ABR");
    }
    MmNoteDraw(dl, ImGui::GetTime(), 1100, 994);
    MmPageEnd(dl);
    // every change counts for the next match straight away (saved to the file on the way out)
    if (SU.dirty && SU.rename < 0) SuApply();
}

// ---- the My setup panel on the main menu ----
static void SuPanel(ImDrawList* dl, float x0, float y0, float in) {
    const MmPal& P = MmP();
    SuLoad();
    bool rnd = SU.chara == SU_RANDOM;
    MmImg(dl, "MA10_CSEL", rnd ? 48 : SU_PORTRAIT[SU.chara], x0 + 24, y0 + 62, 110, 161, MmRGBA(255, 255, 255, in));
    float x = x0 + 150, y = y0 + 72;
    const char* nm = rnd ? "Random" : SU_CHARS[SU.chara];
    MmText(dl, g_fUiBold, strlen(nm) > 18 ? 21.0f : 27.0f, x, y, MmC(P.text, in), nm);
    dl->AddRectFilled(CP(x, y + 20), CP(x + 26, y + 46), MmC(P.bgTop, in), CV(4));
    if (!rnd) SuHead(dl, SU.chara, SuColor(), x, y + 20, 26, 26);
    dl->AddRect(CP(x, y + 20), CP(x + 26, y + 46), MmRGBA(255, 210, 110, in), CV(4), 0, CV(2));
    char c[24];
    if (rnd) snprintf(c, sizeof c, "Random colour"); else snprintf(c, sizeof c, "Colour %d", SuColor() + 1);
    MmText(dl, g_fUi, 21, x + 36, y + 33, MmC(P.dim, in), c);
    MmImg(dl, "MA10_CSEL", SU.stage == SU_RANDOM ? 14 : 6 + SU.stage, x, y + 58, 74, 47, MmRGBA(255, 255, 255, in));
    dl->AddRect(CP(x, y + 58), CP(x + 74, y + 105), MmRGBA(214, 170, 82, in), 0, 0, CV(1));
    char s[24];
    if (SU.stage == SU_RANDOM) snprintf(s, sizeof s, "Random stage"); else snprintf(s, sizeof s, "Stage %02d", SU.stage + 1);
    MmText(dl, g_fUi, 21, x + 84, y + 81, MmC(P.dim, in), s);
    MmText(dl, g_fMenu, 11, x, y + 124, MmC(P.faint, in), "KNIGHTS");
    MmText(dl, g_fUiBold, 21, x + 90, y + 124, MmC(P.text, in), SuInUse().name);
    for (int k = 0; k < 3; k++) SuSmallCard(dl, x + k * 91, y + 141, 86.0f / 192, SuInUse().a[k], false, in);
}
// build 112: My setup as one row (the room's MY MATCH SETTINGS bar)
static int SuPortraitOf(int chara) { return chara >= 0 && chara < SU_NCHAR ? SU_PORTRAIT[chara] : 48; }
static void SuRoomPanel(ImDrawList* dl, float x0, float y0, float x1, float y1, float in) {
    const MmPal& P = MmP();
    SuLoad();
    bool rnd = SU.chara == SU_RANDOM;
    MmImg(dl, "MA10_CSEL", rnd ? 48 : SU_PORTRAIT[SU.chara], x0 + 20, y0 + 13, 70, 102, MmRGBA(255, 255, 255, in));
    float cy = (y0 + y1) / 2;
    MmText(dl, g_fMenu, 11, x0 + 106, cy - 20, MmC(P.faint, in), "CHARACTER");
    const char* nm = rnd ? "Random" : SU_CHARS[SU.chara];
    MmText(dl, g_fUiBold, strlen(nm) > 16 ? 19.0f : 25.0f, x0 + 106, cy + 10, MmC(P.text, in), nm);
    float x = x0 + 336;
    MmText(dl, g_fMenu, 11, x, cy - 20, MmC(P.faint, in), "COLOUR");
    dl->AddRectFilled(CP(x, cy - 2), CP(x + 26, cy + 24), MmC(P.bgTop, in), CV(4));
    if (!rnd) SuHead(dl, SU.chara, SuColor(), x, cy - 2, 26, 26);
    dl->AddRect(CP(x, cy - 2), CP(x + 26, cy + 24), MmRGBA(255, 210, 110, in), CV(4), 0, CV(2));
    char c[24];
    if (rnd) snprintf(c, sizeof c, "Random"); else snprintf(c, sizeof c, "Colour %d", SuColor() + 1);
    MmText(dl, g_fUi, 21, x + 36, cy + 11, MmC(P.dim, in), c);
    x = x0 + 470;
    MmImg(dl, "MA10_CSEL", SU.stage == SU_RANDOM ? 14 : 6 + SU.stage, x, cy - 29, 90, 57, MmRGBA(255, 255, 255, in));
    dl->AddRect(CP(x, cy - 29), CP(x + 90, cy + 28), MmRGBA(214, 170, 82, in), 0, 0, CV(1));
    x -= 10;
    MmText(dl, g_fMenu, 11, x + 114, cy - 20, MmC(P.faint, in), "STAGE");
    char st[24];
    if (SU.stage == SU_RANDOM) snprintf(st, sizeof st, "Random"); else snprintf(st, sizeof st, "Stage %02d", SU.stage + 1);
    MmText(dl, g_fUiBold, 25, x + 114, cy + 10, MmC(P.text, in), st);
    x = x0 + 676;
    MmText(dl, g_fMenu, 11, x, cy - 26, MmC(P.faint, in), "SUPPORT KNIGHTS");
    MmText(dl, g_fUiBold, 21, x, cy + 0, MmC(P.text, in), SuInUse().name);
    for (int k = 0; k < 3; k++) SuSmallCard(dl, x + k * 64, cy + 16, 60.0f / 192, SuInUse().a[k], false, in);
    MmPadButton(dl, x1 - 56, cy - 28, 'Y', MmRGBA(210, 170, 20, in));
    MmText(dl, g_fMenu, 10, x1 - 38, cy + 26, MmC(P.text, in), "EDIT", 1);
}
// open My setup (main menu / room): the summary once the steps were walked through, else step 1
static void SuOpen() {
    SuLoad();
    SU.step = SU.done ? 3 : 0;
    SU.fromSum = false; SU.sumRow = 0;
    SU.pick = -1; SU.rename = -1; SU.delArm = -1;
    SU.charCur = SU.chara == SU_RANDOM ? SU_NCHAR : SU.chara;
    SU.presetCur = SuCurList().inUse;
    MM.screen = 4; MM.roomT = -1; MM.quietFrame = ImGui::GetFrameCount();
}
