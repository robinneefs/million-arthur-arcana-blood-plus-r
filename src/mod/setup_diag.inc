// Build 60: watch-only diagnostics for "boot straight into the fight".
// Logs what character select and the loading screen (with its assist set menu)
// write, so the next build can write the same values itself:
//  - memory windows (match setup / options .data, fight manager .data, the two
//    player structs) compared every frame; dwords that change on many frames
//    are treated as counters and left out;
//  - every field change of the loading screen and character select objects;
//  - pad presses with their frame, to line changes up with what was pressed;
//  - full dumps of the per-player setup blocks when the fight starts.
// Nothing is written to game memory. [diag] setup_watch=0 turns it off.

namespace sdiag {

struct Win { uint32_t lo, hi; const char* name; };
static const Win WINS[] = {
    {0x04DC0000, 0x04DCB000, "setup"},     // 0x04DC9658 + p*0x200 per-player setup, mode 0x04DC9650, options 0x04DCAD00
    {0x04B79000, 0x04B7B000, "fightmgr"},  // fight phase 0x04B79340 etc.
    {0x05130000, 0x05146000, "players"},   // player structs 0x05130CD8, stride 0xA520
};
static const int NW = sizeof(WINS) / sizeof(WINS[0]);

static bool on = false, inited = false;
static std::vector<uint32_t> snap[NW];      // last frame
static std::vector<uint32_t> base[NW];      // at the last screen change
static std::vector<uint16_t> nchg[NW];      // frames with a change, per dword
static std::vector<uint8_t>  nlog[NW];      // lines logged, per dword
static uint32_t lines = 0, screenLines = 0, frameOnScreen = 0, prevPads = 0, padLines = 0;
static uint32_t lastVt = 0, lastPtr = 0;
static std::vector<uint32_t> objPrev;
static std::vector<uint8_t> objLogged;
static const uint32_t MAX_LINES = 9000, MAX_SCREEN_LINES = 1500;

static const char* WinName(uint32_t a) {
    if (a >= 0x05130CD8 && a < 0x05130CD8 + 2 * 0xA520) return (a - 0x05130CD8) < 0xA520 ? "P1" : "P2";
    if (a >= 0x04DC9658 && a < 0x04DC9658 + 0x400) return (a - 0x04DC9658) < 0x200 ? "setupP1" : "setupP2";
    for (auto& w : WINS) if (a >= w.lo && a < w.hi) return w.name;
    return "?";
}
static uint32_t WinOff(uint32_t a) {
    if (a >= 0x05130CD8 && a < 0x05130CD8 + 2 * 0xA520) return (a - 0x05130CD8) % 0xA520;
    if (a >= 0x04DC9658 && a < 0x04DC9658 + 0x400) return (a - 0x04DC9658) % 0x200;
    return 0;
}

static void Read(int i, std::vector<uint32_t>& out) {
    uint32_t n = (WINS[i].hi - WINS[i].lo) / 4;
    out.resize(n);
    memcpy(out.data(), reinterpret_cast<const void*>(WINS[i].lo), n * 4);
}

static void Init() {
    inited = true;
    wchar_t ini[MAX_PATH];
    _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
    on = GetPrivateProfileIntW(L"diag", L"setup_watch", 1, ini) != 0;
    if (!on) return;
    for (int i = 0; i < NW; i++) {
        Read(i, snap[i]);
        base[i] = snap[i];
        nchg[i].assign(snap[i].size(), 0);
        nlog[i].assign(snap[i].size(), 0);
    }
    Log("setup watch (build 60): on. Watching %d memory windows, the loading screen and character select objects, and pad presses.", NW);
}

static void DumpBlock(const char* title, uint32_t a, uint32_t size) {
    Log("setup: dump %s 0x%08X (%u bytes):", title, a, size);
    const uint32_t* p = reinterpret_cast<const uint32_t*>(a);
    for (uint32_t off = 0; off < size; off += 32) {
        Log("setup:   +%03X  %08X %08X %08X %08X  %08X %08X %08X %08X", off,
            p[off / 4], p[off / 4 + 1], p[off / 4 + 2], p[off / 4 + 3], p[off / 4 + 4], p[off / 4 + 5], p[off / 4 + 6], p[off / 4 + 7]);
    }
}

static void FightStartDumps() {
    DumpBlock("mode/flags", 0x04DC9600, 0x58);
    DumpBlock("setup P1", 0x04DC9658, 0x200);
    DumpBlock("setup P2", 0x04DC9858, 0x200);
    DumpBlock("online setup copy", 0x04DC2500, 0x80);
    DumpBlock("options (save)", 0x04DCAD00, 0x48);
    for (int p = 0; p < 2; p++) {
        uint32_t s = 0x05130CD8 + p * 0xA520;
        char t[40];
        snprintf(t, sizeof t, "P%d +0x7200", p + 1); DumpBlock(t, s + 0x7200, 0x80);
        snprintf(t, sizeof t, "P%d +0x7B80", p + 1); DumpBlock(t, s + 0x7B80, 0x40);
        snprintf(t, sizeof t, "P%d +0x5660", p + 1); DumpBlock(t, s + 0x5660, 0x60);
    }
}

// Changes since the last screen change, minus counters (called on a screen change).
static void ScreenDiff(const char* from, const char* to) {
    uint32_t n = 0, skipped = 0;
    for (int i = 0; i < NW; i++) {
        std::vector<uint32_t> cur;
        Read(i, cur);
        for (size_t k = 0; k < cur.size(); k++) {
            if (cur[k] == base[i][k]) continue;
            if (nchg[i][k] > 30) { skipped++; continue; }
            if (n < 400 && lines < MAX_LINES) {
                uint32_t a = WINS[i].lo + (uint32_t)k * 4;
                Log("setup: %s -> %s  0x%08X (%s +0x%X): %08X -> %08X", from, to, a, WinName(a), WinOff(a), base[i][k], cur[k]);
                lines++;
            }
            n++;
        }
        base[i] = cur;
        nchg[i].assign(cur.size(), 0);
        nlog[i].assign(cur.size(), 0);
    }
    Log("setup: %s -> %s: %u values changed during %s (%u busy counters left out)", from, to, n, from, skipped);
}

static void Tick() {
    if (!inited) Init();
    if (!on) return;
    uint32_t vt, ptr;
    CurScreen(&vt, &ptr);
    if (vt != lastVt || ptr != lastPtr) {
        ScreenDiff(lastVt ? addr::SceneName(lastVt) : "start", vt ? addr::SceneName(vt) : "none");
        lastVt = vt;
        lastPtr = ptr;
        frameOnScreen = 0;
        screenLines = 0;
        objPrev.clear();
        objLogged.clear();
    }
    frameOnScreen++;
    const char* sn = addr::SceneName(vt);
    bool fight = vt == SCENE_BTL;
    if (fight && frameOnScreen == 3) FightStartDumps();

    // Pad presses (every pad, rising bits), with the frame.
    uint32_t pads = 0;
    for (int d = 0; d < 10; d++) pads |= *reinterpret_cast<const uint32_t*>(addr::PAD_TABLE + d * 0xF8);
    uint32_t fresh = pads & ~prevPads;
    prevPads = pads;
    if (fresh && padLines < 1500 && !(fight && frameOnScreen > 5)) {
        Log("setup: %s f%u press %08X", sn, frameOnScreen, fresh);
        padLines++;
    }

    // Memory windows: count changes per dword every frame; log live changes
    // outside the fight (and its first 5 frames).
    bool logLive = !fight || frameOnScreen <= 5;
    for (int i = 0; i < NW; i++) {
        const uint32_t* cur = reinterpret_cast<const uint32_t*>(WINS[i].lo);
        std::vector<uint32_t>& s = snap[i];
        for (size_t k = 0; k < s.size(); k++) {
            uint32_t v = cur[k];
            if (v == s[k]) continue;
            if (nchg[i][k] < 0xFFFF) nchg[i][k]++;
            if (logLive && nchg[i][k] <= 3 && nlog[i][k] < 3 && screenLines < MAX_SCREEN_LINES && lines < MAX_LINES) {
                uint32_t a = WINS[i].lo + (uint32_t)k * 4;
                Log("setup: %s f%u 0x%08X (%s +0x%X): %08X -> %08X", sn, frameOnScreen, a, WinName(a), WinOff(a), s[k], v);
                nlog[i][k]++;
                screenLines++;
                lines++;
            }
            s[k] = v;
        }
    }

    // Loading screen and character select objects: every field change.
    uint32_t objSize = vt == 0x008D2604 ? 0x104 : vt == 0x008D1BF8 ? 0x8B0 : 0;
    if (objSize && ptr) {
        const uint32_t* o = reinterpret_cast<const uint32_t*>(ptr);
        if (objPrev.empty()) {
            objPrev.assign(o, o + objSize / 4);
            objLogged.assign(objSize / 4, 0);
            Log("setup: %s object at 0x%08X, watching 0x%X bytes", sn, ptr, objSize);
        } else {
            for (uint32_t k = 0; k < objSize / 4; k++) {
                if (o[k] == objPrev[k]) continue;
                if (objLogged[k] < 6 && screenLines < MAX_SCREEN_LINES && lines < MAX_LINES) {
                    Log("setup: %s f%u obj +0x%03X: %08X -> %08X", sn, frameOnScreen, k * 4, objPrev[k], o[k]);
                    objLogged[k]++;
                    screenLines++;
                    lines++;
                }
                objPrev[k] = o[k];
            }
        }
    }
}

} // namespace sdiag
