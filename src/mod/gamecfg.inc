// ---------------------------------------------------------------------------
// Build 104: the game's own settings, read and changed for our Settings screen.
// Everything here runs on the game thread (our menu is drawn from Present), only while
// our main menu is up (the game's title screen is on hold), never during a fight.
//
// RE (2026-11-16, static analysis of the game code):
//   OPTION save section 0x04DCAD00 (0x48 bytes): +0x0C vibration (0 on, 1 off),
//     +0x14 music, +0x18 sound effects, +0x1C voices, +0x20 voice chat (0-100, default 30),
//     +0x34 resolution index, +0x38 width, +0x3C height, +0x40 windowed, +0x44 V-Sync.
//   0x5D0F80()  applies the options (volumes -> sound engine 0x4B2B60.., vibration 0x522C20).
//   Display: the option menu fills 0x04DCACE0 {index, w, h, windowed, vsync} and calls
//     0x5D1190() (copies it into OPTION and the Screen.ini values). The list of resolutions is
//     at 0x051D21FC (16 bytes each: w, h, ..), count 0x051D2A04; 0x61F470(this 0x051CA9C0, windowed)
//     rebuilds it for windowed / fullscreen. The game reads Screen.ini at start-up, so display
//     changes show after a restart.
//   Controller layout, player p: 0x04DCAAC8 + p*0x30 = 12 dwords, one per game input
//     (A, B, C, D, AB, AC, AD, BC, BD, CD, ABC, sticker), each a button number 0-17 (18 = none);
//     button number -> raw bit table 0xC7FF28. Set with 0x59E4E0(int[12], p).
//   Keyboard layout: 0x04DC3080 + p*0x30 = 12 DirectInput key codes (-1 = none); set with
//     0x51D290(int[12], p) (also rebuilds 0x04DC3020).
//   Defaults: controller 0x6D601C, keyboard 0x6E8720 + p*0x30.
//   Save: 0x517E40(1, 0, 1) writes SaveData.dat (and Screen.ini).
// ---------------------------------------------------------------------------
namespace gcfg {

static const int NSLOT = 12;
enum Slot { S_A, S_B, S_C, S_D, S_AB, S_AC, S_AD, S_BC, S_BD, S_CD, S_ABC, S_STAMP };
// button number -> raw bit (0xC7FF28)
static const uint32_t BTN_MASK[18] = {0x1, 0x8, 0x2, 0x4, 0x4000, 0x8000, 0x2000, 0x1000, 0x20, 0x10,
                                      0x100, 0x200, 0x10000, 0x20000, 0x40, 0x80, 0x400, 0x40000};
static const char* const BTN_NAME[18] = {"UP", "RIGHT", "DOWN", "LEFT", "X", "Y", "B", "A", "BACK", "START",
                                         "LB", "RB", "LT", "RT", "LS", "RS", "?", "?"};
static const int BTN_NONE = 18;
static const int PAD_DEFAULT[NSLOT] = {4, 5, 6, 7, 11, 18, 10, 12, 18, 18, 13, 18};
static const int KEY_DEFAULT[NSLOT] = {0x16, 0x17, 0x18, 0x24, 0x25, -1, 0x12, 0x10, -1, -1, 0x26, 0x2A};

struct Res { int w, h; };
struct State {
    int vol[3] = {30, 30, 30};     // music, sound effects, voices (0-100)
    bool vib = true;
    int nRes = 0;
    Res res[64] = {};
    int resIdx = 0;
    bool windowed = true, vsync = false;
    int pad[NSLOT] = {};
    int key[NSLOT] = {};
};

// raw bit -> button number (-1 if it is not one we let people bind)
static int ButtonOfBit(uint32_t bit) {
    for (int i = 4; i < 16; i++)
        if (BTN_MASK[i] == bit && i != 8 && i != 9) return i;   // no directions, Back or Start
    return -1;
}

// DirectInput key code <-> name / virtual key
static void KeyName(int dik, char* out, size_t n) {
    if (dik < 0) { snprintf(out, n, "-"); return; }
#ifndef UI_HARNESS
    wchar_t w[32] = {};
    LONG lp = ((dik & 0x7F) << 16) | ((dik & 0x80) ? (1 << 24) : 0);
    if (GetKeyNameTextW(lp, w, 32) > 0) {
        snprintf(out, n, "%ls", w);
        for (char* c = out; *c; c++) *c = (char)toupper((unsigned char)*c);
        return;
    }
#endif
    static const char* row1 = "QWERTYUIOP", *row2 = "ASDFGHJKL", *row3 = "ZXCVBNM";
    if (dik >= 0x02 && dik <= 0x0B) snprintf(out, n, "%d", (dik - 1) % 10);
    else if (dik >= 0x10 && dik <= 0x19) snprintf(out, n, "%c", row1[dik - 0x10]);
    else if (dik >= 0x1E && dik <= 0x26) snprintf(out, n, "%c", row2[dik - 0x1E]);
    else if (dik >= 0x2C && dik <= 0x32) snprintf(out, n, "%c", row3[dik - 0x2C]);
    else if (dik == 0x2A) snprintf(out, n, "LEFT SHIFT");
    else if (dik == 0x39) snprintf(out, n, "SPACE");
    else snprintf(out, n, "KEY %02X", dik);
}
#ifndef UI_HARNESS
static int KeyOfVk(int vk) {
    UINT sc = MapVirtualKeyW((UINT)vk, 0 /* MAPVK_VK_TO_VSC */);
    if (!sc) return -1;
    switch (vk) {   // extended keys: DirectInput sets the top bit
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: case VK_RCONTROL: case VK_RMENU: case VK_DIVIDE: case VK_LWIN: case VK_RWIN:
        sc |= 0x80; break;
    }
    return (int)(sc & 0xFF);
}
static int VkOfKey(int dik) {
    if (dik < 0) return 0;
    UINT sc = (dik & 0x7F) | ((dik & 0x80) ? 0xE000 : 0);
    return (int)MapVirtualKeyW(sc, 3 /* MAPVK_VSC_TO_VK_EX */);
}
#endif

#ifndef UI_HARNESS
static const uint32_t OPT = 0x04DCAD00, PENDING = 0x04DCACE0, RES_LIST = 0x051D21FC, RES_COUNT = 0x051D2A04, RES_OBJ = 0x051CA9C0;
static const uint32_t PADCFG = 0x04DCAAC8, KEYCFG = 0x04DC3080;
static inline int& OptI(int off) { return *(int*)(OPT + off); }

static void ReadRes(State& s) {
    int n = *(int*)RES_COUNT;
    if (n < 0) n = 0;
    if (n > 64) n = 64;
    s.nRes = n;
    for (int i = 0; i < n; i++) { s.res[i].w = *(int*)(RES_LIST + i * 16); s.res[i].h = *(int*)(RES_LIST + i * 16 + 4); }
}
static void RebuildResList(bool windowed) {
    typedef void(__thiscall * F)(void*, bool);
    ((F)0x61F470)((void*)RES_OBJ, windowed);
}
static void Read(State& s) {
    s.vol[0] = OptI(0x14); s.vol[1] = OptI(0x18); s.vol[2] = OptI(0x1C);
    for (int& v : s.vol) v = v < 0 ? 0 : v > 100 ? 100 : v;
    s.vib = OptI(0x0C) == 0;
    s.windowed = OptI(0x40) != 0;
    s.vsync = OptI(0x44) != 0;
    RebuildResList(s.windowed);
    ReadRes(s);
    int w = OptI(0x38), h = OptI(0x3C);
    s.resIdx = 0;
    for (int i = 0; i < s.nRes; i++) if (s.res[i].w == w && s.res[i].h == h) s.resIdx = i;
    const uint32_t* cfg = N.saved ? N.origCfg : (const uint32_t*)PADCFG;   // player 1's own layout
    for (int i = 0; i < NSLOT; i++) s.pad[i] = (int)cfg[i];
    for (int i = 0; i < NSLOT; i++) s.key[i] = *(int*)(KEYCFG + i * 4);
    Log("settings: read music %d, effects %d, voices %d, vibration %d, %dx%d %s v-sync %d (%d resolutions)", s.vol[0], s.vol[1], s.vol[2],
        s.vib, w, h, s.windowed ? "windowed" : "fullscreen", s.vsync, s.nRes);
}
static void Save() {
    typedef int(__cdecl * F)(int, int, int);
    int r = ((F)0x517E40)(1, 0, 1);
    Log("settings: saved (game save writer returned %d)", r);
}
static void ApplyOptions() { ((void(__cdecl*)())0x5D0F80)(); }
static void SetVolume(State& s, int i, int v) {
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    s.vol[i] = v;
    OptI(0x14 + 4 * i) = v;
    ApplyOptions();
}
static void SetVibration(State& s, bool on) {
    s.vib = on;
    OptI(0x0C) = on ? 0 : 1;
    ApplyOptions();
}
static void SetDisplay(State& s) {
    if (s.nRes <= 0) return;
    if (s.resIdx < 0 || s.resIdx >= s.nRes) s.resIdx = 0;
    int* p = (int*)PENDING;
    p[0] = s.resIdx; p[1] = s.res[s.resIdx].w; p[2] = s.res[s.resIdx].h; p[3] = s.windowed ? 1 : 0; p[4] = s.vsync ? 1 : 0;
    ((void(__cdecl*)())0x5D1190)();
    Log("settings: display set to %dx%d %s, v-sync %d (after a restart)", p[1], p[2], s.windowed ? "windowed" : "fullscreen", p[4]);
}
// windowed <-> fullscreen has its own list: keep the same size if it is there
static void SetWindowed(State& s, bool windowed) {
    Res keep = s.nRes > 0 ? s.res[s.resIdx] : Res{1280, 720};
    s.windowed = windowed;
    RebuildResList(windowed);
    ReadRes(s);
    s.resIdx = 0;
    for (int i = 0; i < s.nRes; i++) if (s.res[i].w == keep.w && s.res[i].h == keep.h) s.resIdx = i;
    SetDisplay(s);
}
static void SetPad(State& s) {
    int a[NSLOT];
    for (int i = 0; i < NSLOT; i++) a[i] = s.pad[i];
    if (N.saved) {
        for (int i = 0; i < NSLOT; i++) N.origCfg[i] = (uint32_t)a[i];   // a session holds the live layout: it uses this at its next fight
    } else {
        ((void(__cdecl*)(const int*, int))0x59E4E0)(a, 0);
    }
    Log("settings: controller layout %d %d %d %d | %d %d %d %d %d %d %d %d", a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
}
static void SetKeys(State& s) {
    int a[NSLOT];
    for (int i = 0; i < NSLOT; i++) a[i] = s.key[i];
    ((void(__cdecl*)(const int*, int))0x51D290)(a, 0);
    Log("settings: keyboard layout %02X %02X %02X %02X | %02X %02X %02X %02X %02X %02X %02X %02X", a[0] & 0xFF, a[1] & 0xFF, a[2] & 0xFF,
        a[3] & 0xFF, a[4] & 0xFF, a[5] & 0xFF, a[6] & 0xFF, a[7] & 0xFF, a[8] & 0xFF, a[9] & 0xFF, a[10] & 0xFF, a[11] & 0xFF);
}
// what is held right now: OR of the controllers (raw bits), keyboard keys by code
static uint32_t PadHeld() { return U.navBits; }
static uint32_t PadDirs() {
    uint32_t d = 0;
    for (uint32_t i = 0; i < 10; i++) d |= U.padRaw[i] & 0xF;
    return d;
}
static bool KeyHeld(int dik) {
    int vk = VkOfKey(dik);
    return vk && (GetAsyncKeyState(vk) & 0x8000);
}
#else
// ---- harness: sample data ----
static void Read(State& s) {
    s.vol[0] = 70; s.vol[1] = 85; s.vol[2] = 100;
    static const Res r[] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}};
    s.nRes = 5; for (int i = 0; i < 5; i++) s.res[i] = r[i];
    s.resIdx = 3; s.windowed = true; s.vsync = false; s.vib = true;
    for (int i = 0; i < NSLOT; i++) { s.pad[i] = PAD_DEFAULT[i]; s.key[i] = KEY_DEFAULT[i]; }
}
static void Save() {}
static void SetVolume(State& s, int i, int v) { s.vol[i] = v < 0 ? 0 : v > 100 ? 100 : v; }
static void SetVibration(State& s, bool on) { s.vib = on; }
static void SetDisplay(State&) {}
static void SetWindowed(State& s, bool w) { s.windowed = w; }
static void SetPad(State&) {}
static void SetKeys(State&) {}
static uint32_t PadHeld() { return 0x2000 | 0x4000; }
static uint32_t PadDirs() { return 0x2 | 0x8; }
static bool KeyHeld(int) { return false; }
#endif
}  // namespace gcfg
