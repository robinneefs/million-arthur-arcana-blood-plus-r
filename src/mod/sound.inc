// ===========================================================================
// Build 92: sounds and music for our menus, played by the game's own sound
// engine (its XACT sound bank), so the game's volume settings apply and
// nothing of the game is shipped.
// ===========================================================================
// - Cue names come from the game's flashrom\snd\db.xsb (read once).
// - Play: the game's own helper 0x635840 (this = sound bank holder 0x0515EAB0,
//   args: cue number, where to put the cue (or null), time offset), which calls
//   the bank's Play. Stop a cue: its function table [1] Stop(0), [3] Destroy.
// - The game's music: 0x4B2C70 stops the current track; 0x4A34B0(track, 1, 0, 1)
//   asks for a track again; the last track asked for is at 0x00A7E124.
// - Everything here runs on the game thread (UiGameThreadTick); the drawing
//   thread only leaves requests (MM_SND in menu.inc).
// - Logging: outside fights, every sound the game itself starts is logged
//   ("sound: game played ...") so the menu sounds can be matched by name.
// ===========================================================================

namespace snd {

static const uint32_t BANK_HOLDER = 0x0515EAB0;
static const uint32_t FN_BANK_PLAY = 0x00635840;
static const uint32_t FN_BGM_STOP = 0x004B2C70;
static const uint32_t FN_BGM_REQUEST = 0x004A34B0;
static const uint32_t G_BGM_LAST = 0x00A7E124;

typedef HRESULT(__attribute__((thiscall)) * BankPlay_t)(void* holder, uint32_t cue, void** pp, uint32_t offset);
typedef void(__cdecl* BgmStop_t)();
typedef void(__cdecl* BgmRequest_t)(uint32_t track, uint32_t force, uint32_t a, uint32_t b);
typedef HRESULT(WINAPI* CueStop_t2)(void* cue, DWORD flags);
typedef HRESULT(WINAPI* CueDestroy_t2)(void* cue);

static std::vector<std::string> g_names;     // cue number -> name (db.xsb)
static bool g_namesTried = false;
static thread_local int t_own = 0;           // our own plays: not logged
static int g_logged = 0;

static void LoadNames() {
    if (g_namesTried) return;
    g_namesTried = true;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%lsflashrom\\snd\\db.xsb", art::GameDir());
    std::vector<uint8_t> d;
    if (!art::ReadAll(path, d) || d.size() < 0x4A || memcmp(d.data(), "SDBK", 4)) { Log("sound: db.xsb not read; sounds can only be named by number"); return; }
    uint16_t total = 0;
    uint32_t hashVals = 0;
    art::Rd(d, 25, total);
    art::Rd(d, 66, hashVals);
    g_names.resize(total);
    for (uint32_t i = 0; i < total; i++) {
        uint32_t no = 0;
        if (!art::Rd(d, hashVals + 6 * i, no) || no >= d.size()) continue;
        const char* s = (const char*)d.data() + no;
        size_t n = strnlen(s, d.size() - no);
        g_names[i].assign(s, n < 48 ? n : 48);
    }
    Log("sound: %u cue names read from db.xsb", (unsigned)total);
}
static const char* Name(uint32_t cue) { return cue < g_names.size() && !g_names[cue].empty() ? g_names[cue].c_str() : "?"; }
static int Find(const char* name) {
    LoadNames();
    for (size_t i = 0; i < g_names.size(); i++) if (g_names[i] == name) return (int)i;
    return -1;
}

static bool BankReady() {
    uint32_t sb = 0;
    return SafeRead32(BANK_HOLDER, &sb) && sb;
}
// start a cue; returns the cue (when wanted) or nullptr
static void* Play(int cue, bool keep) {
    if (cue < 0 || !BankReady()) return nullptr;
    void* c = nullptr;
    t_own++;
    HRESULT hr = ((BankPlay_t)FN_BANK_PLAY)((void*)BANK_HOLDER, (uint32_t)cue, keep ? &c : nullptr, 0);
    t_own--;
    if (FAILED(hr)) { Log("sound: could not start %s (%d): 0x%08lX", Name(cue), cue, hr); return nullptr; }
    return c;
}
static void StopCue(void*& c) {
    if (!c) return;
    uint32_t vt = 0, fs = 0, fd = 0;
    if (SafeRead32((uint32_t)c, &vt) && SafeRead32(vt + 4, &fs) && SafeRead32(vt + 12, &fd)) {
        t_own++;
        ((CueStop_t2)fs)(c, 0);
        ((CueDestroy_t2)fd)(c);
        t_own--;
    }
    c = nullptr;
}

// called from the sound bank hook (mod.cpp) for every sound the game starts
static volatile bool g_logOn = false;
static void SetLog(bool on) { g_logOn = on; }
static void LogGamePlay(uint32_t cue) {
    if (t_own || !g_logOn || g_logged >= 400) return;
    LoadNames();
    g_logged++;
    Log("sound: game played %s (cue %u) on screen %08X", Name(cue), cue, U.screenVt);
}

// ---- our menu ----------------------------------------------------------------
// the menu music (picked by the user after test 92; build 106: no more switching with LB / RB)
static const char* const MENU_MUSIC = "bgm_5_02";

static struct {
    bool active = false;       // our menu has the music
    void* music = nullptr;
    uint32_t gameTrack = 0xFFFFFFFF;
    const char* sfx[3] = {"sys_073", "sys_074", "sys_072"};   // move, select, back: the game's own menu sounds (test 92 log)
    int sfxCue[3] = {-1, -1, -1};
    bool cuesFound = false;
} M;

static void FindCues() {
    if (M.cuesFound) return;
    M.cuesFound = true;
    for (int i = 0; i < 3; i++) M.sfxCue[i] = Find(M.sfx[i]);
    Log("sound: menu sounds move %s (%d), select %s (%d), back %s (%d); music %s", M.sfx[0], M.sfxCue[0], M.sfx[1], M.sfxCue[1],
        M.sfx[2], M.sfxCue[2], MENU_MUSIC);
}
static void StartMusic() {
    StopCue(M.music);
    M.music = nullptr;
    if (!g_menuMusic.load()) { MM.music[0] = 0; return; }   // build 104: Settings > Sound > Menu music off
    M.music = Play(Find(MENU_MUSIC), true);
    snprintf(MM.music, sizeof MM.music, "%s", MENU_MUSIC);
    Log("sound: menu music %s%s", MENU_MUSIC, M.music ? "" : " did not start");
}

// build 108: the connection lost pop-up (one of the game's menu sounds; off with the menu sounds)
static void PlayAlert() {
    if (!BankReady() || !g_menuSfx.load()) return;
    FindCues();
    Play(M.sfxCue[2], false);
}
// game thread, every frame: our menu open or not
static void Tick(bool menuOpen) {
    if (menuOpen && !M.active) {
        if (!BankReady()) return;
        FindCues();
        MmLoadPrefs();
        M.active = true;
        SafeRead32(G_BGM_LAST, &M.gameTrack);
        t_own++;
        ((BgmStop_t)FN_BGM_STOP)();
        t_own--;
        StartMusic();
    } else if (!menuOpen && M.active) {
        M.active = false;
        StopCue(M.music);
        MM.music[0] = 0;
        if (M.gameTrack < 0x2F) {
            t_own++;
            ((BgmRequest_t)FN_BGM_REQUEST)(M.gameTrack, 1, 0, 1);
            t_own--;
            Log("sound: game music %u back", M.gameTrack);
        }
    }
    if (!M.active) {
        // build 118: the after-match screen over the fight has the menu sounds too (move / select / back);
        // played between game updates like the connection-lost alert, never during a rollback replay
        uint32_t s = MM_SND.exchange(0);
        if (g_postOn.load() && s && g_menuSfx.load() && BankReady()) {
            FindCues();
            for (int i = 0; i < 3; i++) if (s & (1u << i)) { Play(M.sfxCue[i], false); break; }
        }
        return;
    }
    // build 101: the game asked for its own music while our menu is up: stop it again
    // (it comes back when our menu closes, as before)
    {
        uint32_t cur = 0;
        if (SafeRead32(G_BGM_LAST, &cur) && cur != M.gameTrack) {
            Log("sound: the game started its music %u under our menu, stopped it", cur);
            M.gameTrack = cur;
            t_own++;
            ((BgmStop_t)FN_BGM_STOP)();
            t_own--;
            StartMusic();
        }
    }
    if (g_musicReapply.exchange(false)) StartMusic();   // build 104: menu music switched on / off
    uint32_t s = MM_SND.exchange(0);
    if (!g_menuSfx.load()) s = 0;
    for (int i = 0; i < 3; i++) if (s & (1u << i)) Play(M.sfxCue[i], false);
}

} // namespace snd
