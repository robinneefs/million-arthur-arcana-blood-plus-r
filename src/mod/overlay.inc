// ===========================================================================
// Build 57: our own on-screen overlay (included into mod.cpp after lobby.inc)
// ===========================================================================
// Drawn with Dear ImGui (MIT licence, third_party/imgui) on top of the game's
// picture. The game renders with Direct3D 9; its renderer object is at
// [0x04DCDB14] and the device at renderer+0x214 (found from the
// Direct3DCreate9 call at 0x62F180 and the CreateDevice call at 0x62F305).
// We replace two entries of the device's function table: Present (draw the
// overlay right before the picture is shown) and Reset (the overlay's
// textures must be let go and made again when the display changes).
//
// Input: keyboard and mouse come from the game window's messages; the
// controller comes from the game's own pad table (raw buttons, same bits as
// an Xbox controller for every pad: see the device read at 0x522D00). While
// the overlay has focus, the game's menus get no input: its "held" and
// "pressed" flags and the key-repeat table are cleared right after the game
// works them out, and keyboard/mouse messages don't reach the game window.
//
// Threads: drawing happens on whatever thread calls Present; the session
// (connect, lobby rules, sending) on the game thread. The overlay only
// writes wishes under g_lobbyLock / atomics; UiGameThreadTick() acts on them.
// ===========================================================================
#include "imgui.h"
#include "imgui_internal.h"
#include "ui_theme.h"
#ifndef UI_HARNESS
#include <wincodec.h>
#endif
#ifndef UI_HARNESS     // (UI_HARNESS: the screens alone, drawn to a picture for checking the layout)
#include <d3d9.h>
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

static const uint32_t G_RENDERER = 0x04DCDB14;   // renderer object; +0x214 = IDirect3DDevice9*
static const uint32_t VT_TITLE = 0x008D2CBC;     // CMLTitle

// Pad raw bits (all pads use the Xbox layout, test: 0x522D00)
enum : uint32_t { PB_UP = 0x1, PB_DOWN = 0x2, PB_LEFT = 0x4, PB_RIGHT = 0x8, PB_START = 0x10, PB_BACK = 0x20,
                  PB_LB = 0x100, PB_RB = 0x200, PB_A = 0x1000, PB_B = 0x2000, PB_X = 0x4000, PB_Y = 0x8000 };

enum UiScreen { US_CONNECT, US_SESSION, US_CONTROLS };
static struct Ui {
    std::atomic<bool> visible{false};
    std::atomic<bool> capture{false};     // the overlay takes the input this frame (set on the game thread)
    std::atomic<bool> drain{false};       // overlay just closed: keep the game's input off until the buttons held then are up
    bool drainArmed = false;              // (build 58) drain only waits for the buttons held when the overlay closed
    uint32_t drainHeld[10] = {};
    int drainFrames = 0;
    bool autoOpened = false;
    int titleStep = 0;                    // build 58: 0 title screen, 1 button down there, 2 released (counting), 3 opened
    int titleWait = 0;
    uint32_t anyRaw = 0;                  // every pad and the keyboard slots, this frame
    int screen = US_CONNECT;
    int backTo = US_CONNECT;              // where Controls returns to
    // connect settings (overlay thread; copied to the session on Connect)
    int mode = 0;                         // 0 loopback, 1 LAN, 2 Steam
    char lanPeer[64] = "auto";
    int side = 0;                         // 0 auto, 1 player 1, 2 player 2
    char name[32] = "";
    int controller = -1;                  // -1 = the game's player 1 controller, else pad 0-9
    // requests for the game thread
    std::atomic<int> cmd{0};              // 1 connect, 2 disconnect/cancel, 3 refresh friends
    // Steam friends (filled on the game thread)
    net::Friend friends[64];
    int nFriends = 0, pickFriend = -1;
    char friendErr[160] = "";
    // pads (game thread writes, overlay reads)
    uint32_t padRaw[10] = {};
    uint32_t navBits = 0;
    bool detect = false, detectArmed = false;
    uint32_t screenVt = 0;
    bool netOn = false, netHello = false;
    std::atomic<int> pgReq{0};            // build 91: 3 open our main menu (button on the overlay's first screen)
} U;

static ImFont *g_fBody = nullptr, *g_fTitle = nullptr, *g_fSmall = nullptr;
static void PgSetOpen(bool on);       // our menu open / closed (canvas.inc)
static bool PgIsOpen();
namespace snd { static void Tick(bool menuOpen); static bool BankReady(); static void SetLog(bool on); }   // build 92 (sound.inc)

#ifndef UI_HARNESS
// ---- settings --------------------------------------------------------------
static void UiLoadSettings() {
    wchar_t ini[MAX_PATH];
    _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
    wchar_t w[64];
    GetPrivateProfileStringW(L"net", L"mode", L"loopback", w, 64, ini);
    U.mode = _wcsicmp(w, L"lan") == 0 ? 1 : _wcsicmp(w, L"steam") == 0 ? 2 : 0;
    GetPrivateProfileStringW(L"net", L"lan_peer", L"auto", w, 64, ini);
    snprintf(U.lanPeer, sizeof U.lanPeer, "%ls", w);
    U.side = GetPrivateProfileIntW(L"net", L"side", 0, ini);
    if (U.side < 0 || U.side > 2) U.side = 0;
    GetPrivateProfileStringW(L"ui", L"name", L"", w, 64, ini);
    snprintf(U.name, sizeof U.name, "%ls", w);
    if (!U.name[0]) {
        DWORD n = sizeof U.name;
        if (!GetComputerNameA(U.name, &n)) snprintf(U.name, sizeof U.name, "player");
    }
    snprintf(g_uiName, sizeof g_uiName, "%s", U.name);
    GetPrivateProfileStringW(L"ui", L"controller", L"auto", w, 64, ini);
    U.controller = _wcsicmp(w, L"auto") == 0 ? -1 : _wtoi(w);
    if (U.controller < -1 || U.controller > 9) U.controller = -1;
}
static void UiIniWrite(const wchar_t* sec, const wchar_t* key, const char* val) {
    wchar_t ini[MAX_PATH], w[96];
    _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
    _snwprintf(w, 96, L"%hs", val);
    WritePrivateProfileStringW(sec, key, w, ini);
}
static void UiSaveConnectSettings() {
    static const char* modes[] = {"loopback", "lan", "steam"};
    char side[8];
    snprintf(side, sizeof side, "%d", U.side);
    UiIniWrite(L"net", L"mode", modes[U.mode]);
    UiIniWrite(L"net", L"lan_peer", U.lanPeer[0] ? U.lanPeer : "auto");
    UiIniWrite(L"net", L"side", side);
    UiIniWrite(L"ui", L"name", U.name);
}
static void UiSaveController() {
    char v[8];
    if (U.controller < 0) snprintf(v, sizeof v, "auto"); else snprintf(v, sizeof v, "%d", U.controller);
    UiIniWrite(L"ui", L"controller", v);
}

// Called by NetBecomeOn (game thread) when a session starts.
static void UiSessionStarted() {
    if (U.controller >= 0) N.localDev = (uint32_t)U.controller;
    Log("ui: connected; this PC's player uses controller slot %u (%s)", N.localDev, U.controller >= 0 ? "picked in the overlay" : "the game's player 1 controller");
    U.screen = US_SESSION;
}

// ---- game thread ---------------------------------------------------------------
static bool KeyPressed(int vk);
[[maybe_unused]] static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
// Build 58 diagnostics: which fields of the title screen object change when the
// "press start" picture turns into the main menu (so a later build can tell the two
// apart exactly). Fields that keep changing (timers) are only logged a few times.
[[maybe_unused]] static void UiTitleWatch(uint32_t vt, uint32_t ptr) {
    static uint32_t lastPtr = 0, prev[0xC0];
    static uint8_t changes[0xC0];
    static int lines = 0;
    static bool readable = false;
    if (vt != VT_TITLE || !ptr || lines >= 160) return;
    if (ptr != lastPtr) {
        lastPtr = ptr;
        MEMORY_BASIC_INFORMATION mi{};
        readable = VirtualQuery((void*)ptr, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                   (uintptr_t)mi.BaseAddress + mi.RegionSize >= ptr + sizeof prev;
        if (readable) memcpy(prev, (void*)ptr, sizeof prev);
        memset(changes, 0, sizeof changes);
        Log("ui: title screen object at 0x%08X (%s)", ptr, readable ? "watching its fields" : "not readable");
        return;
    }
    if (!readable) return;
    const uint32_t* cur = reinterpret_cast<const uint32_t*>(ptr);
    for (int i = 1; i < 0xC0 && lines < 160; i++) {          // (0 = vtable)
        uint32_t v = cur[i];
        if (v == prev[i]) continue;
        if (changes[i] < 255) changes[i]++;
        if (changes[i] <= 3) { Log("ui: title +0x%03X: %08X -> %08X (pads %08X)", i * 4, prev[i], v, U.anyRaw); lines++; }
        prev[i] = v;
    }
}
// Build 108: connection trouble. Game thread: DcOnEnd (from NetEnd) opens the pop-up and,
// in a fight or on the loading screen, holds that screen (H_ChildUpdate) until OK; then
// g_dcExit ends the fight (boot.inc) and the game goes back to the title = our main menu.
static struct DcState {
    std::atomic<bool> show{false};
    uint8_t why = 0;
    bool inMatch = false;
    char name[32] = "";
    double openT = -1;
    uint32_t prevNav = 0;
} DC;
static std::atomic<bool> g_dcFreeze{false}, g_dcExit{false}, g_dcSound{false};
static std::atomic<uint32_t> g_dcWaitMs{0}, g_dcAwayMs{0};   // in step with the other game: ms since its last packet
static bool g_uiBlock = false;                 // drawing thread: menus get no input under the pop-up
static bool BootInMatch();
namespace snd { static void PlayAlert(); }
static void DcOnEnd(const char* why) {
    static const struct { const char* s; uint8_t c; } kMap[] = {
        {"the other player left", 0}, {"the other player closed the game", 1}, {"the other game restarted", 1},
        {"no packets from the other game for a while", 2},
        {"the other player has a different mod build", 3}, {"the other player's game version is different", 3},
        {"the two games went to different screens", 4}, {"the saved data did not get across", 4},
        {"the two games' saved data have different sizes", 4}, {"the other player switched away for too long", 5}};
    int c = -1;
    for (auto& m : kMap) if (!strcmp(m.s, why)) c = m.c;
    if (c < 0) return;                                  // we ended it ourselves
    if (N.st != NS_ON && c != 3) return;                // never got connected
    char nm[32] = "";
    EnterCriticalSection(&g_lobbyLock);
    if (LB.haveThem && LB.them.name[0]) snprintf(nm, sizeof nm, "%s", LB.them.name);
    LeaveCriticalSection(&g_lobbyLock);
    snprintf(DC.name, sizeof DC.name, "%s", nm[0] ? nm : "The other player");
    DC.why = (uint8_t)c;
    DC.inMatch = BootInMatch();
    uint32_t vt, ptr;
    CurScreen(&vt, &ptr);
    bool hold = DC.inMatch && (vt == SCENE_BTL || vt == 0x008D2604 /*Loading*/);
    g_dcFreeze = hold;
    g_dcExit = false;
    DC.openT = -1;
    DC.show = true;
    g_dcSound = true;
    Log("ui: connection lost pop-up (%s; name \"%s\", in a match %s, %s held)", why, DC.name, DC.inMatch ? "yes" : "no",
        hold ? addr::SceneName(vt) : "no screen");
}

// Every frame on the game thread (from PollHotkeys), also when the window is in the background.
static uint32_t g_presents = 0;
static volatile bool g_holdTitle = false;   // build 101: our main menu covers the title screen; keep it on hold
static volatile int g_drawLog = 0;   // build 62: log the next pictures after F1 opens the overlay
static void UiHookWatch();
static void BorderlessTick(bool focused);
static void UiGameThreadTick(bool focused) {
    UiHookWatch();
    BorderlessTick(focused);   // build 110
    uint32_t vt, ptr;
    CurScreen(&vt, &ptr);
    U.screenVt = vt;
    U.netOn = N.st == NS_ON;
    U.netHello = N.st == NS_HELLO;
    bool inSynced = NetOn() && N.syncedType;
    // F1, or Back + Start on a controller: show / hide
    static bool comboWas = false;
    bool combo = (U.navBits & (PB_BACK | PB_START)) == (PB_BACK | PB_START);
    bool toggle = (KeyPressed(VK_F1) && focused) || (combo && !comboWas);
    comboWas = combo;
    // F3 (or the overlay's button) opens / closes our main menu; not during fights or tests
    {
        int req = U.pgReq.exchange(0);
        bool f3 = KeyPressed(VK_F3) && focused;
        bool wantMm = req == 3 || f3;
        bool can = !inSynced && g_mode == Mode::Idle && !g_auto;
        // build 101: our main menu is the game's front end. It opens by itself on the game's
        // title screen (from its first picture, so the intro never shows) and every time the
        // game comes back to it; closing it by hand (F1 / F3) keeps it shut until the next visit.
        static bool mmShut = false;
        static uint32_t prevVt = 0;
        if (vt == VT_TITLE && prevVt != VT_TITLE) mmShut = false;
        prevVt = vt;
        if (PgIsOpen() && (toggle || !can || f3)) {
            if (can) { mmShut = true; Log("ui: main menu closed by hand (F3 opens it again)"); }
            PgSetOpen(false); toggle = false;
        }
        else if (!PgIsOpen() && wantMm && can) { PgSetOpen(true); mmShut = false; }
        else if (!PgIsOpen() && can && !NetOn() && !mmShut && vt == VT_TITLE) {
            PgSetOpen(true);
            if (!U.visible) U.visible = true;
            U.autoOpened = true;
            Log("ui: main menu opened on the game's title screen (pictures so far %u)", g_presents);
        }
        // the game's title screen stays on hold while our main menu covers it (boot.inc)
        g_holdTitle = PgIsOpen() && U.visible.load();
    }
    // build 92: our main menu plays its own music and sounds through the game's sound engine
    // (and logs the sounds the game plays outside fights)
    {
        static bool hookTried = false;
        snd::SetLog(!inSynced && g_mode == Mode::Idle && !g_auto && !NetOn());
        if (!hookTried && snd::BankReady()) { hookTried = true; HookSoundBank(); }
        snd::Tick(PgIsOpen());
        if (g_dcSound.exchange(false)) snd::PlayAlert();   // build 108
        net::RoomTick((int)NET_BUILD);   // build 100: Steam rooms (lobbies)
    }
    if (toggle) {
        bool v = !U.visible.load();
        U.visible = v;
        if (!v) U.drain = true;
        Log("ui: overlay %s (pictures so far %u)", v ? "shown" : "hidden", g_presents);
        if (v) g_drawLog = 3;
    }
    // (build 58-100: opened by itself after a press on the "press start" picture; build 101
    // opens our main menu straight away instead, above)
    // the game's own synced screens (old menu path) and tests: overlay steps aside
    if (U.visible && (inSynced || g_mode != Mode::Idle || g_auto)) { U.visible = false; U.drain = true; Log("ui: overlay hidden (game screen in step / test running)"); }
    U.capture = (U.visible.load() && focused && !inSynced && g_mode == Mode::Idle && !g_auto) ||
                (g_postOn.load() && focused) ||  // build 107: the after-match screen over the fight
                (DC.show.load() && focused);     // build 108: connection lost pop-up
    g_dcWaitMs = (N.st == NS_ON && N.syncedType) ? GetTickCount() - N.lastRecv : 0;
    g_dcAwayMs = (N.st == NS_ON && N.peerAwaySince) ? GetTickCount() - N.peerAwaySince : 0;   // build 111
    if (g_closeReq.load() && N.st != NS_OFF) { g_byeClosing = true; NetEnd("you closed the game"); }   // build 111: asked by the window thread
    // requests
    int c = U.cmd.exchange(0);
    if (c == 1 && N.st == NS_OFF) {
        if (g_mode != Mode::Idle || g_auto) SetStatus("stop the test first (F7 / F8), then connect");
        else {
            snprintf(g_uiName, sizeof g_uiName, "%s", net::SteamMyName()[0] ? net::SteamMyName() : U.name);
            if (U.mode == 2 && U.pickFriend >= 0 && U.pickFriend < U.nFriends) {
                g_netPick = U.friends[U.pickFriend].id;
                snprintf(g_netPickName, sizeof g_netPickName, "%s", U.friends[U.pickFriend].name);
            }
            Log("ui: connect pressed (mode %d, other PC \"%s\", side %d, name \"%s\")", U.mode, U.lanPeer, U.side, U.name);
            NetStart();
        }
    } else if (c == 2 && N.st != NS_OFF) {
        NetEnd("disconnect pressed");
    } else if (c == 3) {
        char err[160] = "";
        if (!net::SteamOpen(err, sizeof err)) { snprintf(U.friendErr, sizeof U.friendErr, "Steam: %s", err); U.nFriends = 0; }
        else { U.nFriends = net::SteamFriends(U.friends, 64); U.friendErr[0] = 0; if (!U.nFriends) snprintf(U.friendErr, sizeof U.friendErr, "no Steam friends online"); }
        U.pickFriend = U.nFriends ? 0 : -1;
    }
    if (N.st == NS_OFF && U.screen == US_SESSION) U.screen = US_CONNECT;
    if (N.st == NS_OFF && U.backTo == US_SESSION) U.backTo = US_CONNECT;
    if (NetOn()) {
        if (U.controller >= 0) N.localDev = (uint32_t)U.controller;
        // player 1's input delay, between screens only
        EnterCriticalSection(&g_lobbyLock);
        uint8_t d = LB.me.delay;
        LeaveCriticalSection(&g_lobbyLock);
        if (!N.syncedType && d <= 8 && (int)d != g_ncfg.delay) {
            Log("ui: input delay now %u (set by player 1)", d);
            g_ncfg.delay = d;
        }
    }
}

// From H_Input, after the game read its controllers (only when it reads
// them itself, not when the session feeds it input).
static void UiAfterGameInput() {
    uint32_t nav = 0, any = 0;
    for (uint32_t i = 0; i < addr::PAD_COUNT; i++) {
        uint32_t r = PadU32(i, addr::PAD_RAW0);
        U.padRaw[i] = r;
        any |= r;
        if (i < 8) nav |= r;                  // keyboard (8, 9) drives the overlay through window messages instead
    }
    U.navBits = nav;
    if (U.detect) {
        if (!U.detectArmed) { if (!any) U.detectArmed = true; }
        else for (uint32_t i = 0; i < addr::PAD_COUNT; i++)
            if (U.padRaw[i]) {
                U.controller = (int)i;
                U.detect = U.detectArmed = false;
                UiSaveController();
                Log("ui: controller picked by pressing a button: slot %u", i);
                break;
            }
    }
    U.anyRaw = any;
    bool block = U.capture.load();
    if (!block && U.drain.load()) {
        // Build 58 (test 57: the controller stayed blocked after F1): wait only for the
        // buttons that were held when the overlay closed, and never longer than 3/4 s,
        // so a pad that always reports a button (stick drift, a trigger) can't keep it shut.
        if (!U.drainArmed) {
            U.drainArmed = true;
            U.drainFrames = 0;
            for (uint32_t i = 0; i < addr::PAD_COUNT && i < 10; i++) U.drainHeld[i] = U.padRaw[i];
        }
        uint32_t held = 0;
        for (uint32_t i = 0; i < addr::PAD_COUNT && i < 10; i++) { U.drainHeld[i] &= U.padRaw[i]; held |= U.drainHeld[i]; }
        if (!held || ++U.drainFrames > 45) {
            if (held) {
                char b[160]; int n = 0;
                for (uint32_t i = 0; i < addr::PAD_COUNT && i < 10 && n < 140; i++)
                    if (U.drainHeld[i]) n += snprintf(b + n, sizeof b - n, " slot %u=%08X", i, U.drainHeld[i]);
                b[n] = 0;
                Log("ui: game input back (buttons still held:%s)", b);
            } else Log("ui: game input back");
            U.drain = false;
            U.drainArmed = false;
        } else block = true;
    }
    if (block) {
        ClearMenuInputState();
        for (uint32_t i = 0; i < addr::PAD_COUNT; i++) {     // some game code reads the raw buttons (test 53)
            PadU32(i, addr::PAD_RAW0) = 0;
            PadU32(i, addr::PAD_RAW1) = 0;
        }
    }
}

// ---- Direct3D 9 hooks ------------------------------------------------------------
typedef HRESULT(WINAPI* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(WINAPI* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static Present_t o_Present = nullptr;
static Reset_t o_Reset = nullptr;
static IDirect3DDevice9* g_dev = nullptr;
static bool g_imgui = false, g_imguiFailed = false;
static volatile bool g_resetImgui = false;   // build 63: the game moved to a new device
static int g_presentDepth = 0;              // build 63: re-entry guard (another hook calling ours)
static Present_t g_firstPresent = nullptr;  // the device's own Present
static HWND g_uiHwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static float g_uiScale = 1.0f;
static DWORD g_presentTid = 0;

// Build 58 (test 57: no mouse pointer): the game hides the Windows pointer. While the
// overlay has the input we show it again (on the window's own thread, where the
// pointer's show count lives) and put it back as it was when the overlay lets go.
// Build 59 (test 58: the pointer vanished after closing and opening a few times):
// - checked on every window message, not only once, since the game hides it again;
// - an empty pointer (the game's "no pointer" shape) is replaced by the arrow;
// - (build 63: the pointer is no longer locked inside the game window; the user plays
//   windowed and wants it free. Only a clip left over from build 59-62 is undone.)
// - the overlay's own pointer is only drawn while the Windows one is really hidden.
static int g_cursorBumps = 0;
static bool g_clipped = false;
static RECT g_oldClip{};
static std::atomic<bool> g_softCursor{false};    // ImGui draws the pointer itself (set by UiFrame)
static void UiCursorOnWindowThread(HWND h, bool cap) {
    if (cap) {
        CURSORINFO ci{};
        ci.cbSize = sizeof ci;
        if (GetCursorInfo(&ci)) {
            if (!(ci.flags & CURSOR_SHOWING))
                for (int i = 0; i < 20 && g_cursorBumps < 40; i++) { g_cursorBumps++; if (ShowCursor(TRUE) >= 0) break; }
            if (!ci.hCursor && !g_softCursor.load()) SetCursor(LoadCursor(nullptr, IDC_ARROW));
        }
    } else {
        while (g_cursorBumps > 0) { ShowCursor(FALSE); g_cursorBumps--; }
        if (g_clipped) {
            g_clipped = false;
            ClipCursor(g_oldClip.right > g_oldClip.left ? &g_oldClip : nullptr);
        }
    }
}

// Build 110: borderless full screen. The game runs windowed (its own option); we take the
// frame off its window and stretch it over the whole monitor (D3D9 scales the picture to
// the window). Done on the window's own thread (posted message), checked once a second.
static const UINT WM_MOD_BORDERLESS = WM_APP + 0x51;
static struct { bool on = false; LONG_PTR style = 0, exStyle = 0; RECT rect{}; } g_bl;
static void BorderlessApply(HWND h, bool want) {
    if (want) {
        HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoW(mon, &mi)) return;
        const RECT& m = mi.rcMonitor;
        LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
        RECT wr{};
        GetWindowRect(h, &wr);
        bool frameless = !(st & (WS_CAPTION | WS_THICKFRAME));
        bool covers = wr.left == m.left && wr.top == m.top && wr.right == m.right && wr.bottom == m.bottom;
        if (g_bl.on && frameless && covers) return;
        if (!g_bl.on) { g_bl.style = st; g_bl.exStyle = GetWindowLongPtrW(h, GWL_EXSTYLE); g_bl.rect = wr; }
        SetWindowLongPtrW(h, GWL_STYLE, (st & ~(LONG_PTR)(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME)) | WS_POPUP);
        SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) & ~(LONG_PTR)(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));
        SetWindowPos(h, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
        Log("ui: borderless: window %s %ldx%ld at %ld,%ld (was %ldx%ld%s)", g_bl.on ? "put back over" : "now covers", m.right - m.left, m.bottom - m.top,
            m.left, m.top, wr.right - wr.left, wr.bottom - wr.top, frameless ? "" : ", with a frame");
        g_bl.on = true;
    } else if (g_bl.on) {
        g_bl.on = false;
        SetWindowLongPtrW(h, GWL_STYLE, g_bl.style);
        SetWindowLongPtrW(h, GWL_EXSTYLE, g_bl.exStyle);
        SetWindowPos(h, nullptr, g_bl.rect.left, g_bl.rect.top, g_bl.rect.right - g_bl.rect.left, g_bl.rect.bottom - g_bl.rect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
        Log("ui: borderless off: window frame and size put back (%ldx%ld)", g_bl.rect.right - g_bl.rect.left, g_bl.rect.bottom - g_bl.rect.top);
    }
}

static LRESULT CALLBACK UiWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOD_BORDERLESS) { BorderlessApply(h, wp != 0); return 0; }
    if (msg == WM_ACTIVATEAPP || msg == WM_SIZE) {   // build 110: alt-tab check
        static int logs = 0;
        if (logs++ < 60) Log("ui: window %s (%u x %u)", msg == WM_SIZE ? (wp == SIZE_MINIMIZED ? "minimised" : "resized") : wp ? "in front again" : "in the background",
                             (unsigned)LOWORD(lp), (unsigned)HIWORD(lp));
    }
    // build 108: closing the game tells the other player so (instead of an 8 s time-out there)
    if (msg == WM_CLOSE && N.st != NS_OFF) {
        if (OnGameThread()) { g_byeClosing = true; NetEnd("you closed the game"); }
        else {   // build 111: the game thread says goodbye (UiGameThreadTick); wait for it a moment
            g_closeReq = true;
            for (int i = 0; i < 60 && N.st != NS_OFF; i++) Sleep(5);
            Log("net: window closing: goodbye %s", N.st == NS_OFF ? "sent" : "not sent in time (the other player will see a time-out)");
        }
    }
    if (g_imgui) {
        bool cap = U.capture.load();
        UiCursorOnWindowThread(h, cap);
        if (cap || msg == WM_KEYUP || msg == WM_LBUTTONUP || msg == WM_RBUTTONUP) ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
        if (cap) {
            switch (msg) {
            case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
                if (msg == WM_KEYDOWN && wp >= VK_F1 && wp <= VK_F12) break;   // our hotkeys still reach the game window
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) return TRUE;
                break;
            default: break;
            }
        }
    }
    return CallWindowProcW(g_origWndProc, h, msg, wp, lp);
}

static void UiLoadFonts(float scale);   // (after canvas.inc: it uses its font lookup)

// Build 90: one scale for the whole overlay: the canvas is 1920x1080, fitted into the picture.
static float UiScaleFor(float w, float h) {
    float s = w / 1920.0f < h / 1080.0f ? w / 1920.0f : h / 1080.0f;
    return s < 0.5f ? 0.5f : s;
}
// Build 90: pictures for the game's own sprites (gameart.inc). Managed pool: they
// survive a display reset; a new device drops them (art::FreeAll).
static void* UiMakeTex(const uint32_t* bgra, int w, int h) {
    IDirect3DTexture9* t = nullptr;
    if (!g_dev || FAILED(g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) || !t) return nullptr;
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(0, &lr, nullptr, 0))) { t->Release(); return nullptr; }
    for (int y = 0; y < h; y++) memcpy((uint8_t*)lr.pBits + (size_t)y * lr.Pitch, bgra + (size_t)y * w, (size_t)w * 4);
    t->UnlockRect(0);
    return t;
}
static void UiFreeTex(void* t) { if (t) static_cast<IDirect3DTexture9*>(t)->Release(); }
// build 94: read a picture file of the mod (PNG etc.) with Windows' own image decoder (WIC)
static bool UiLoadImage(const wchar_t* path, std::vector<uint32_t>& px, int& w, int& h) {
    static const GUID CLSID_WicFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
    static const GUID IID_WicFactory = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
    static const GUID FMT_BGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = false;
    HRESULT hr = CoCreateInstance(CLSID_WicFactory, nullptr, CLSCTX_INPROC_SERVER, IID_WicFactory, (void**)&f);
    if (SUCCEEDED(hr)) hr = f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &fr);
    if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&cv);
    if (SUCCEEDED(hr)) hr = cv->Initialize(fr, FMT_BGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT uw = 0, uh = 0;
    if (SUCCEEDED(hr)) hr = cv->GetSize(&uw, &uh);
    if (SUCCEEDED(hr) && uw && uh && uw <= 4096 && uh <= 4096) {
        px.resize((size_t)uw * uh);
        hr = cv->CopyPixels(nullptr, uw * 4, (UINT)(px.size() * 4), (BYTE*)px.data());
        ok = SUCCEEDED(hr);
        w = (int)uw; h = (int)uh;
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (f) f->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    char n[MAX_PATH];
    snprintf(n, sizeof n, "%ls", wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path);
    if (ok) Log("ui: picture %s read (%dx%d)", n, w, h);
    else Log("ui: picture %s could not be read (0x%08lX)", n, hr);
    return ok;
}

static bool UiInit(IDirect3DDevice9* dev) {
    HWND hwnd = *reinterpret_cast<HWND*>(addr::G_HWND);
    if (!hwnd) return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    D3DVIEWPORT9 vp;
    dev->GetViewport(&vp);
    IDirect3DSurface9* bb = nullptr;
    float h = 720.0f;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC d;
        if (SUCCEEDED(bb->GetDesc(&d))) h = (float)d.Height;
        bb->Release();
    }
    {
        float w = 1920.0f;
        IDirect3DSurface9* b2 = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &b2)) && b2) {
            D3DSURFACE_DESC d;
            if (SUCCEEDED(b2->GetDesc(&d))) w = (float)d.Width;
            b2->Release();
        }
        g_uiScale = UiScaleFor(w, h);
    }
    UiLoadFonts(g_uiScale);
    theme::Apply(g_uiScale);
    art::MakeTex = UiMakeTex;
    art::FreeTex = UiFreeTex;
    art::LoadImageFile = UiLoadImage;
    if (!ImGui_ImplWin32_Init(hwnd) || !ImGui_ImplDX9_Init(dev)) {
        Log("ui: overlay could not start (ImGui init failed)");
        return false;
    }
    g_uiHwnd = hwnd;
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)UiWndProc);
    Log("ui: overlay ready (picture %.0f pixels high, scale %.2f, drawing thread %lu, game thread %lu)", h, g_uiScale,
        GetCurrentThreadId(), g_gameTid);
    return true;
}

#endif // UI_HARNESS

// ---- screens ---------------------------------------------------------------------
static void UiDisabled(bool on) {
    ImGui::PushItemFlag(ImGuiItemFlags_Disabled, on);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (on ? 0.45f : 1.0f));
}
static void UiDisabledEnd() { ImGui::PopStyleVar(); ImGui::PopItemFlag(); }
static bool UiBigButton(const char* label, bool accent = false, float w = 0) {
    if (accent) {
        ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::AccentHover());
        ImGui::PushStyleColor(ImGuiCol_Text, theme::rgba(20, 20, 30));
    }
    bool r = ImGui::Button(label, ImVec2(w, 0));
    if (accent) ImGui::PopStyleColor(3);
    return r;
}
static void UiNote(const char* t) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim());
    ImGui::TextWrapped("%s", t);
    ImGui::PopStyleColor();
}
static void UiHeading(const char* t) {
    ImGui::PushFont(g_fTitle);
    ImGui::TextColored(theme::Accent(), "%s", t);
    ImGui::PopFont();
}
static void UiStatusLine() {
    char ns[200], gs[256];
    EnterCriticalSection(&g_statusLock);
    snprintf(ns, sizeof ns, "%s", g_netStatus);
    snprintf(gs, sizeof gs, "%s", g_status);
    LeaveCriticalSection(&g_statusLock);
    ImGui::PushFont(g_fSmall);
    ImGui::TextColored(theme::TextDim(), "%s", ns[0] ? ns : gs);
    ImGui::PopFont();
}

static const char* const PAD_NAMES[10] = {"Xbox controller 1", "Xbox controller 2", "Xbox controller 3", "Xbox controller 4",
                                          "Other controller 1", "Other controller 2", "Other controller 3", "Other controller 4",
                                          "Keyboard (left side)", "Keyboard (right side)"};

static void UiConnectScreen() {
    UiHeading("Play online");
    ImGui::Spacing();
    bool busy = U.netHello;
    UiDisabled(busy);
    ImGui::Text("Your name");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::InputText("##name", U.name, sizeof U.name);
    ImGui::Spacing();
    ImGui::Text("Connect through");
    ImGui::RadioButton("Home network (LAN)", &U.mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Steam friend", &U.mode, 2);
    ImGui::SameLine();
    ImGui::RadioButton("Test on this PC (stand-in player)", &U.mode, 0);
    if (U.mode == 1) {
        ImGui::Text("Other PC's address");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        ImGui::InputText("##peer", U.lanPeer, sizeof U.lanPeer);
        ImGui::SameLine();
        ImGui::TextColored(theme::TextDim(), "\"auto\" finds it on your network");
    } else if (U.mode == 2) {
        if (ImGui::Button("Refresh friend list")) U.cmd = 3;
        if (U.friendErr[0]) ImGui::TextColored(theme::Bad(), "%s", U.friendErr);
        ImGui::BeginChild("friends", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 5.5f), true);
        for (int i = 0; i < U.nFriends; i++) {
            char l[96];
            snprintf(l, sizeof l, "%s%s##f%d", U.friends[i].name, U.friends[i].inGame ? "  (in this game)" : "", i);
            if (ImGui::Selectable(l, U.pickFriend == i)) U.pickFriend = i;
        }
        if (!U.nFriends && !U.friendErr[0]) ImGui::TextColored(theme::TextDim(), "Press Refresh to see who is online.");
        ImGui::EndChild();
    } else {
        UiNote("A stand-in on this PC plays player 2 and copies your choices.");
    }
    ImGui::Text("Play as");
    ImGui::RadioButton("Decide for me", &U.side, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Player 1 (host)", &U.side, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Player 2", &U.side, 2);
    UiDisabledEnd();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (!busy) {
        bool can = U.mode != 2 || U.pickFriend >= 0;
        UiDisabled(!can);
        if (UiBigButton("Connect", true)) { UiSaveConnectSettings(); U.cmd = 1; }
        UiDisabledEnd();
    } else {
        ImGui::Text("Connecting...");
        ImGui::SameLine();
        if (UiBigButton("Cancel")) U.cmd = 2;
    }
    ImGui::SameLine();
    if (UiBigButton("Controls")) { U.backTo = US_CONNECT; U.screen = US_CONTROLS; }
    ImGui::SameLine();
    if (UiBigButton("Main menu (F3)")) U.pgReq = 3;
}

static int UiSuggestDelay() {
    int ms = (int)N.rtt / 2;
    int d = (ms * 60 + 999) / 1000 + 1;
    return d < 1 ? 1 : d > 8 ? 8 : d;
}

static void UiPlayerCard(int side, const LbSide& s, bool present, bool mine) {
    ImGui::PushStyleColor(ImGuiCol_Text, side == 0 ? theme::P1() : theme::P2());
    ImGui::PushFont(g_fTitle);
    ImGui::Text("P%d", side + 1);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("%s%s", present ? (s.name[0] ? s.name : "player") : "waiting...", mine ? "  (you)" : "");
    if (present) {
        if (s.phase == LP_LOBBY) ImGui::TextColored(s.ready ? theme::Good() : theme::TextDim(), "%s", s.ready ? "Ready" : "Not ready");
        else ImGui::TextColored(theme::TextDim(), "in %s", LB_PHASE_NAME[s.phase]);
    }
    ImGui::EndGroup();
}

static void UiLobbyPhase(LbSide& me, const LbSide& them, bool host) {
    UiHeading("Lobby");
    ImGui::Spacing();
    if (ImGui::BeginTable("players", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        if (N.side == 0) UiPlayerCard(0, me, true, true); else UiPlayerCard(0, them, LB.haveThem, false);
        ImGui::TableNextColumn();
        if (N.side == 1) UiPlayerCard(1, me, true, true); else UiPlayerCard(1, them, LB.haveThem, false);
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::Text("Ping %u ms   |   mod build %u on both PCs", N.rtt, NET_BUILD);
    int delay = me.delay;
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Input delay");
    ImGui::SameLine();
    if (host) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.35f);
        if (ImGui::SliderInt("##delay", &delay, 0, 8, "%d frames")) { me.delay = (uint8_t)delay; LbChanged(); }
        ImGui::SameLine();
        ImGui::TextColored(theme::TextDim(), "suggested %d for this ping", UiSuggestDelay());
    } else {
        ImGui::Text("%d frames", delay);
        ImGui::SameLine();
        ImGui::TextColored(theme::TextDim(), "(player 1 sets this)");
    }
    static const char* const roundsNames[] = {"1", "2", "3", "4", "5"};
    int rounds = me.rounds >= 1 && me.rounds <= 5 ? me.rounds - 1 : 1;
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Rounds to win");
    ImGui::SameLine();
    if (host) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
        if (ImGui::Combo("##rounds", &rounds, roundsNames, 5)) { me.rounds = (uint8_t)(rounds + 1); LbChanged(); }
    } else ImGui::Text("%d", rounds + 1);
    const char* const* timerNames = LB_TIMER_NAMES;
    int timer = me.timer < LB_TIMERS ? me.timer : 2;
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Round timer");
    ImGui::SameLine();
    if (host) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
        if (ImGui::Combo("##timer", &timer, timerNames, LB_TIMERS)) { me.timer = (uint8_t)timer; LbChanged(); }
    } else ImGui::Text("%s", timerNames[timer]);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (UiBigButton(me.ready ? "Not ready" : "Ready", !me.ready)) { me.ready = !me.ready; LbChanged(); }
    ImGui::SameLine();
    if (UiBigButton("Controls")) { U.backTo = US_SESSION; U.screen = US_CONTROLS; }
    ImGui::SameLine();
    if (UiBigButton("Disconnect")) U.cmd = 2;
    if (me.ready && !(LB.haveThem && them.ready)) ImGui::TextColored(theme::TextDim(), "Waiting for the other player to be ready...");
}

static bool UiPickCombo(const char* id, uint8_t* v, int count, bool allowRandom, const char* (*name)(uint8_t), bool enabled) {
    bool changed = false;
    UiDisabled(!enabled);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo(id, name(*v))) {
        if (allowRandom && ImGui::Selectable("Random", *v == LB_RANDOM)) { *v = LB_RANDOM; changed = true; }
        for (int i = 0; i < count; i++) {
            if (ImGui::Selectable(name((uint8_t)i), *v == i)) { *v = (uint8_t)i; changed = true; }
            if (*v == i) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    UiDisabledEnd();
    return changed;
}
#define UiColorName LbColorName
#define UiAssistName LbAssistName

static void UiSideColumn(int side, LbSide& s, bool mine, bool present) {
    ImGui::PushStyleColor(ImGuiCol_Text, side == 0 ? theme::P1() : theme::P2());
    ImGui::Text("PLAYER %d%s", side + 1, mine ? "  (you)" : "");
    ImGui::PopStyleColor();
    ImGui::Text("%s", present ? s.name : "waiting...");
    if (!present) return;
    bool ed = mine && !s.locked;
    char id[32];
    ImGui::TextColored(theme::TextDim(), "Character");
    snprintf(id, sizeof id, "##ch%d", side);
    if (UiPickCombo(id, &s.p.chara, LbCharCount(), true, LbCharName, ed)) {   // build 70: boss left out (crash in a rollback, test 69)
        if (s.p.color >= LbColorCount(s.p.chara)) s.p.color = 0;
        LbChanged();
    }
    ImGui::TextColored(theme::TextDim(), "Colour");
    snprintf(id, sizeof id, "##co%d", side);
    if (UiPickCombo(id, &s.p.color, s.p.chara == LB_RANDOM ? LB_COLORS : LbColorCount(s.p.chara), false, UiColorName, ed)) LbChanged();
    static const char* const slot[3] = {"Support Knight (QCB + L)", "Support Knight (QCB + M)", "Support Knight (QCB + H)"};
    for (int k = 0; k < 3; k++) {
        ImGui::TextColored(theme::TextDim(), "%s", slot[k]);
        snprintf(id, sizeof id, "##as%d_%d", side, k);
        if (UiPickCombo(id, &s.p.assist[k], LB_ASSISTS, false, UiAssistName, ed)) LbChanged();
    }
    ImGui::TextColored(theme::TextDim(), "Stage wish");
    snprintf(id, sizeof id, "##st%d", side);
    if (UiPickCombo(id, &s.p.stage, LB_STAGES, true, LbStageName, ed)) LbChanged();
    ImGui::Spacing();
    if (s.locked) ImGui::TextColored(theme::Good(), "LOCKED IN");
    else ImGui::TextColored(theme::TextDim(), "choosing...");
}

static void UiSetupPhase(LbSide& me, LbSide& them, bool host) {
    UiHeading("Match setup");
    UiNote("If the two stage wishes differ, one of them is picked at random.");
    ImGui::Spacing();
    if (ImGui::BeginTable("sides", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        if (N.side == 0) UiSideColumn(0, me, true, true); else UiSideColumn(0, them, false, LB.haveThem);
        ImGui::TableNextColumn();
        if (N.side == 1) UiSideColumn(1, me, true, true); else UiSideColumn(1, them, false, LB.haveThem);
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::Spacing();
    bool bothLocked = me.locked && them.locked && them.phase == LP_SETUP;
    if (!me.locked) { if (UiBigButton("Lock in", true)) { me.locked = true; LbChanged(); } }
    else if (!bothLocked) { if (UiBigButton("Change picks")) { me.locked = false; LbChanged(); } }
    if (bothLocked) {
        if (host) {
            float left = LB.startAt ? (int32_t)(LB.startAt - GetTickCount()) / 1000.0f : 3.0f;
            if (left < 0) left = 0;
            ImGui::Text("Starting in %.1f s", left);
            ImGui::SameLine();
            if (UiBigButton("Start now", true)) LB.startNow = true;
        } else ImGui::Text("Both locked in. Player 1's game starts the match...");
    }
    ImGui::SameLine();
    if (UiBigButton("Back to lobby")) { me.phase = LP_LOBBY; me.ready = me.locked = false; LbChanged(); }
    ImGui::SameLine();
    if (UiBigButton("Controls")) { U.backTo = US_SESSION; U.screen = US_CONTROLS; }
}

static void UiMatchPhase(LbSide& me, bool host) {
    UiHeading("Match settings agreed");
    UiNote("Both games go from the main menu straight to the loading screen and the fight. Close the overlay (F1) to watch.");
    ImGui::Spacing();
    if (!LB.haveSettings) { ImGui::Text("Waiting for player 1's settings..."); return; }
    const MatchSettings& s = LB.s;
    if (ImGui::BeginTable("ms", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("Player 1");
        ImGui::TableSetupColumn("Player 2");
        ImGui::TableHeadersRow();
        auto row = [&](const char* what, const char* a, const char* b) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(theme::TextDim(), "%s", what);
            ImGui::TableNextColumn(); ImGui::Text("%s", a);
            ImGui::TableNextColumn(); ImGui::Text("%s", b);
        };
        row("Character", LbCharName(s.chara[0]), LbCharName(s.chara[1]));
        row("Colour", UiColorName(s.color[0]), UiColorName(s.color[1]));
        static const char* const slotName[3] = {"Knight L", "Knight M", "Knight H"};
        for (int k = 0; k < 3; k++) row(slotName[k], UiAssistName(s.assist[0][k]), UiAssistName(s.assist[1][k]));
        ImGui::EndTable();
    }
    ImGui::Text("Stage: %s    Rounds to win: %u    Timer: %s    Input delay: %u    Match %u", LbStageIdName(s.stage), s.rounds, LbTimerName(s.timer), s.delay, me.matchNo);
    ImGui::Text("Settings checksum %08X", LB.sum);
    ImGui::SameLine();
    if (LB.confirmed > 0) ImGui::TextColored(theme::Good(), host ? "- same on the other PC" : "- received from player 1");
    else if (LB.confirmed < 0) ImGui::TextColored(theme::Bad(), "- DIFFERENT on the other PC!");
    else ImGui::TextColored(theme::TextDim(), "- waiting for the other PC to confirm");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (UiBigButton("Stuck? End this match")) { me.phase = LP_POST; me.choice = LCH_NONE; me.choiceMatch = 0; LbChanged(); LbNote("match %u ended by hand", me.matchNo); }
}

static void UiPostPhase(LbSide& me, const LbSide& them) {
    UiHeading("Match over");
    ImGui::Spacing();
    struct { uint8_t c; const char* label; } opts[] = {{LCH_REMATCH, "Rematch"}, {LCH_LOBBY, "Back to lobby"}};
    for (auto& o : opts) {
        bool sel = me.choice == o.c && me.choiceMatch == me.matchNo;
        if (UiBigButton(o.label, sel)) { me.choice = o.c; me.choiceMatch = me.matchNo; LbChanged(); }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Spacing();
    bool theirs = them.choice != LCH_NONE && them.choiceMatch == me.matchNo;
    ImGui::Text("You: %s", me.choice != LCH_NONE && me.choiceMatch == me.matchNo ? LB_CHOICE_NAME[me.choice] : "choose one");
    ImGui::Text("%s: %s", them.name, theirs ? LB_CHOICE_NAME[them.choice] : "choosing...");
    UiNote("It happens when you both choose the same. Rematch starts the same fight again with the same settings.");
}

static void UiSessionScreen() {
    EnterCriticalSection(&g_lobbyLock);
    LbSide& me = LB.me;
    LbSide& them = LB.them;
    bool host = N.side == 0;
    switch (me.phase) {
    case LP_LOBBY: UiLobbyPhase(me, them, host); break;
    case LP_SETUP: UiSetupPhase(me, them, host); break;
    case LP_MATCH: UiMatchPhase(me, host); break;
    case LP_POST: UiPostPhase(me, them); break;
    }
    if (LB.note[0] && GetTickCount() - LB.noteAt < 6000) {
        ImGui::Spacing();
        ImGui::TextColored(theme::Accent(), "%s", LB.note);
    }
    LeaveCriticalSection(&g_lobbyLock);
}

static void UiButtonsText(uint32_t r, char* out, size_t n) {
    static const struct { uint32_t b; const char* name; } names[] = {
        {PB_UP, "Up"}, {PB_DOWN, "Down"}, {PB_LEFT, "Left"}, {PB_RIGHT, "Right"}, {PB_A, "A"}, {PB_B, "B"}, {PB_X, "X"}, {PB_Y, "Y"},
        {PB_LB, "LB"}, {PB_RB, "RB"}, {0x10000, "LT"}, {0x20000, "RT"}, {PB_START, "Start"}, {PB_BACK, "Back"}, {0x40, "LS"}, {0x80, "RS"}};
    out[0] = 0;
    size_t k = 0;
    for (auto& x : names)
        if (r & x.b) k += snprintf(out + k, k < n ? n - k : 0, "%s%s", k ? " " : "", x.name);
    if (!k && r) snprintf(out, n, "%08X", r);
}

static void UiControlsScreen() {
    UiHeading("Controls");
    UiNote("Which controller is yours on this PC. The button layout is in the main menu: Settings > Controls.");
    ImGui::Spacing();
    uint32_t gameDev = PadOfPlayer(0);
    char autoL[96];
    snprintf(autoL, sizeof autoL, "Automatic: the game's player 1 controller (%s)", gameDev < 10 ? PAD_NAMES[gameDev] : "none");
    if (ImGui::RadioButton(autoL, U.controller < 0)) { U.controller = -1; UiSaveController(); }
    if (ImGui::BeginTable("pads", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        for (int i = 0; i < 10; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char l[64];
            snprintf(l, sizeof l, "%s##pad%d", PAD_NAMES[i], i);
            if (ImGui::RadioButton(l, U.controller == i)) { U.controller = i; UiSaveController(); }
            ImGui::TableNextColumn();
            char b[96];
            UiButtonsText(U.padRaw[i], b, sizeof b);
            if (b[0]) ImGui::TextColored(theme::Good(), "%s", b);
            else ImGui::TextColored(theme::TextDim(), "-");
        }
        ImGui::EndTable();
    }
    if (!U.detect) { if (UiBigButton("Pick by pressing a button")) { U.detect = true; U.detectArmed = false; } }
    else { ImGui::TextColored(theme::Accent(), "Press any button on your controller..."); ImGui::SameLine(); if (UiBigButton("Cancel")) U.detect = false; }
    ImGui::Spacing();
    if (UiBigButton("Back", true)) U.screen = U.backTo;
}

static void UiMainPanel(const ImVec2& disp) {
    ImVec2 size(disp.x * theme::PANEL_W, disp.y * theme::PANEL_H);
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin("##arcana_online", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(g_fSmall);
    ImGui::TextColored(theme::AccentDim(), "ARCANA BLOOD ONLINE  -  rollback mod build %u", NET_BUILD);
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("F1 hides this").x);
    ImGui::TextColored(theme::TextDim(), "F1 hides this");
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::BeginChild("content", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), false);
    if (U.screen == US_CONTROLS) UiControlsScreen();
    else if (U.screen == US_SESSION && U.netOn) UiSessionScreen();
    else UiConnectScreen();
    ImGui::EndChild();
    UiStatusLine();
    ImGui::End();
}

static bool MmNetInfoOn();   // menu.inc (build 104)
static void UiStatusBar(const ImVec2& disp) {
    char ns[200];
    EnterCriticalSection(&g_statusLock);
    snprintf(ns, sizeof ns, "%s", g_netStatus);
    LeaveCriticalSection(&g_statusLock);
    if (!U.netOn || !ns[0] || !MmNetInfoOn()) return;   // build 104: Settings > Online > Connection info
    ImGui::SetNextWindowPos(ImVec2(disp.x - 10, 10), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PushFont(g_fSmall);
    ImGui::Text("%s", ns);
    ImGui::PopFont();
    ImGui::End();
}

#include "canvas.inc"   // build 106: was playground.inc
#include "menu.inc"
#include "postui.inc"   // build 107: after-match screen
#include "dcui.inc"     // build 108: connection lost pop-up + waiting banner
#include "hudui.inc"    // build 109: info bar + player names in fights
#ifndef UI_HARNESS
#include "sound.inc"
#endif
static bool PgIsOpen() { return PG.open.load(); }

#ifndef UI_HARNESS
// Our fonts (mod fonts folder first, then Windows' fonts) at the canvas scale.
static void UiLoadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    static const ImWchar latin[] = {0x0020, 0x00FF, 0};
    auto load = [&](const char* file, float px) -> ImFont* {
        char path[MAX_PATH];
        if (PgFontPath(file, path, sizeof path)) {
            ImFontConfig c;
            c.OversampleH = 2;
            ImFont* f = io.Fonts->AddFontFromFileTTF(path, px * scale, &c, latin);
            if (f) return f;
            Log("ui: font %s could not be read; using the built-in one", file);
        }
        ImFontConfig c;
        c.SizePixels = px * scale;
        return io.Fonts->AddFontDefault(&c);
    };
    const theme::Look& t = theme::T();
    g_fBody = load(t.fontBody, t.bodyPx);
    g_fTitle = load(t.fontTitle, t.titlePx);
    g_fSmall = load(t.fontBody, t.smallPx);
    g_fMenu = load("Michroma-Regular.ttf", MM_MENU_PX);                 // build 91: main menu preview
    g_fUi = load("SairaSemiCondensed-Regular.ttf", MM_UI_PX);
    g_fUiBold = load("SairaSemiCondensed-Bold.ttf", MM_UI_PX);
    io.Fonts->Build();
}

// Build 108: the waiting banner and the connection lost pop-up (drawing thread)
// Build 110 (game thread): borderless request (the window thread does it) + how the game runs in the background
static void BorderlessTick(bool focused) {
        static bool bootWindowed = false, haveBoot = false;
        static uint32_t lastPost = 0, bgSince = 0, bgFrames = 0, bgLast = 0;
        static bool wasFocused = true;
        if (!haveBoot) { haveBoot = true; MmLoadPrefs(); bootWindowed = *reinterpret_cast<int*>(0x04DCAD00 + 0x40) != 0;
                         Log("ui: started %s; borderless %s", bootWindowed ? "windowed" : "full screen", g_borderless.load() ? "on" : "off"); }
        HWND hw = *reinterpret_cast<HWND*>(addr::G_HWND);
        uint32_t now = GetTickCount();
        if (g_borderlessFirst.load() && U.screenVt == VT_TITLE) {   // build 111: first start: Borderless by default
            static uint32_t titleSince = 0;
            if (!titleSince) titleSince = now;
            if (now - titleSince > 2000) {
                g_borderlessFirst = false;
                if (!*reinterpret_cast<uint8_t*>(0x051CD27C)) {
                    gcfg::State st;
                    gcfg::Read(st);
                    gcfg::SetWindowed(st, true);
                    Log("ui: first start with Borderless as the default: the game switches from full screen to a window");
                }
            }
        }
        gcfg::ApplyDisplayNow();               // build 110b: Settings > Display changes right away
        bool winNow = *reinterpret_cast<uint8_t*>(0x051CD27C) != 0;   // the mode the game runs in now
        static bool lastWin = winNow;
        if (winNow != lastWin) { lastWin = winNow; lastPost = 0; }
        if (hw && now - lastPost >= (g_bl.on || !winNow ? 1000u : 250u)) {
            lastPost = now;
            bool want = winNow && g_borderless.load();
            if (want || g_bl.on) PostMessageW(hw, WM_MOD_BORDERLESS, want ? 1 : 0, 0);
        }
        if (!focused) {
            if (wasFocused) { bgSince = now; bgFrames = 0; bgLast = now; Log("ui: game in the background (online %s)", NetOn() ? "yes" : "no"); }
            bgFrames++;
            if (now - bgLast >= 5000) { Log("ui: in the background for %u s: %u game frames ran", (now - bgSince) / 1000, bgFrames); bgLast = now; }
        } else if (!wasFocused) {
            Log("ui: game in front again after %u ms; %u game frames ran meanwhile (%.0f per second); online %s", now - bgSince, bgFrames,
                now > bgSince ? bgFrames * 1000.0 / (now - bgSince) : 0.0, NetOn() ? "yes" : "no");
        }
        wasFocused = focused;
    }

// Build 109: info bar + names over our online fights (drawing thread)
static void HudFrame() {
    if (!U.netOn || U.screenVt != SCENE_BTL) return;
    char nm[2][32];
    EnterCriticalSection(&g_lobbyLock);
    int my = N.side == 1 ? 1 : 0;
    snprintf(nm[my], 32, "%s", LB.me.name[0] ? LB.me.name : "You");
    snprintf(nm[1 - my], 32, "%s", LB.haveThem && LB.them.name[0] ? LB.them.name : "Opponent");
    LeaveCriticalSection(&g_lobbyLock);
    HudName(0, nm[0]);
    HudName(1, nm[1]);
    if (!MmNetInfoOn()) return;                       // Settings > Online > Connection info
    HudInfo h{};
    h.ping = (int)N.rtt;
    h.delay = (int)ScreenDelay();
    h.rollback = (int)HudRollbackShown();
    h.stutters = (int)g_hud.stutters.load();
    h.desyncs = (int)N.desyncs;
    int s5 = HudRecentStutters(5000), s10 = HudRecentStutters(10000);
    h.quality = (h.ping > 180 || s10 >= 3) ? 2 : (h.ping >= 100 || s5 >= 1) ? 1 : 0;
    HudBar(h, 17);
}

static void DcFrame() {
    double now = ImGui::GetTime();
    if (DC.show.load()) {
        if (DC.openT < 0) { DC.openT = now; DC.prevNav = 0xFFFFFFFFu; }   // buttons held from the fight don't count
        float t = (float)(now - DC.openT);
        DcPopup(DC.name, DC.why, DC.inMatch, t);
        uint32_t nb = U.capture.load() ? U.navBits : 0, fresh = nb & ~DC.prevNav;
        DC.prevNav = nb;
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 m = io.MousePos, a = CP(DC_OK_X0, DC_OK_Y0), b = CP(DC_OK_X1, DC_OK_Y1);
        bool click = ImGui::IsMouseClicked(0) && m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
        bool ok = (fresh & PB_A) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Enter]) || ImGui::IsKeyPressed(io.KeyMap[ImGuiKey_Space]) || click;
        if (ok && t > 0.6f) {
            bool held = g_dcFreeze.exchange(false);
            if (held) g_dcExit = true;
            DC.show = false;
            MM.prevNav = 0xFFFFFFFFu;   // build 108b: the OK press must not also pick a main menu entry
            Log("ui: connection lost pop-up: OK (%s)", held ? "ending the fight, back to the main menu" : "nothing held");
        }
        return;
    }
    uint32_t w = g_dcWaitMs.load(), lim = (uint32_t)g_ncfg.timeoutMs, away = g_dcAwayMs.load();
    static double since = -1;
    const char* why = "The connection is not responding";
    if (away > 0) { w = away; lim = NET_AWAY_MS; why = "Switched away from the game"; }   // build 111
    if (w < 1000 || w >= lim) { since = -1; return; }
    if (since < 0) since = now;
    static char nm[32];
    EnterCriticalSection(&g_lobbyLock);
    snprintf(nm, sizeof nm, "%s", LB.haveThem && LB.them.name[0] ? LB.them.name : "the other player");
    LeaveCriticalSection(&g_lobbyLock);
    DcBanner(nm, (int)((lim - w + 999) / 1000), (float)(now - since), why);
}

static void UiFrame(IDirect3DDevice9* dev) {
    ImGuiIO& io = ImGui::GetIO();
    // the mouse pointer is only ours while the overlay has the input
    if (U.capture.load() && !g_softCursor.load()) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // build 90: one scale for the whole overlay (canvas 1920x1080 fitted into the picture);
    // fonts are made again at the new size when the resolution changes, so text stays sharp
    {
        IDirect3DSurface9* b0 = nullptr;
        D3DSURFACE_DESC d0{};
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &b0)) && b0) { b0->GetDesc(&d0); b0->Release(); }
        if (d0.Width && d0.Height) {
            float sc = UiScaleFor((float)d0.Width, (float)d0.Height);
            if (sc > g_uiScale * 1.004f || sc < g_uiScale * 0.996f || g_fontsDirty) {
                if (!g_fontsDirty) Log("ui: picture is now %ux%u, overlay scale %.2f -> %.2f", d0.Width, d0.Height, g_uiScale, sc);
                g_uiScale = sc;
                g_fontsDirty = false;
                UiLoadFonts(sc);
                ImGui_ImplDX9_InvalidateDeviceObjects();     // the font picture is made again by NewFrame
            }
            g_disp = ImVec2((float)d0.Width, (float)d0.Height);
            g_cv = g_uiScale;
            g_cvO = ImVec2((d0.Width - 1920.0f * g_cv) * 0.5f, (d0.Height - 1080.0f * g_cv) * 0.5f);
        }
        theme::Apply(g_uiScale);
        art::NewFrame();
        if (PG.freeArt.exchange(false)) { art::FreeAll(true); Log("ui: game pictures let go"); }
        else art::TrimFiles(PG.open.load() ? 5 : 0);   // build 101: keep the menu's pictures loaded across screens
    }
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // the picture may be bigger or smaller than the window: draw in back-buffer pixels
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC d;
        if (SUCCEEDED(bb->GetDesc(&d)) && io.DisplaySize.x > 0 && io.DisplaySize.y > 0) {
            float sx = d.Width / io.DisplaySize.x, sy = d.Height / io.DisplaySize.y;
            if (io.MousePos.x > -FLT_MAX / 2) { io.MousePos.x *= sx; io.MousePos.y *= sy; }
            io.DisplaySize = ImVec2((float)d.Width, (float)d.Height);
        }
        bb->Release();
    }
    bool show = U.visible.load();
    bool cap = U.capture.load();
    // the Windows pointer (see UiCursorOnWindowThread); while it is hidden for more than
    // a sixth of a second, ImGui draws one itself (back off once Windows shows it again)
    {
        static int hidden = 0, shown = 0, logs = 0;
        static bool wasCap = false;
        CURSORINFO ci{};
        ci.cbSize = sizeof ci;
        bool hw = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor;
        if (cap && !wasCap && logs < 12) {
            logs++;
            RECT cr{}, clip{};
            GetClientRect(g_uiHwnd, &cr);
            GetClipCursor(&clip);
            POINT p = ci.ptScreenPos;
            ScreenToClient(g_uiHwnd, &p);
            Log("ui: mouse: pointer %s (flags %lX, shape %p), at %ld,%ld in a %ldx%ld window, clip %ld,%ld-%ld,%ld, foreground %s, own pointer %s",
                hw ? "visible" : "hidden", ci.flags, (void*)ci.hCursor, p.x, p.y, cr.right, cr.bottom, clip.left, clip.top, clip.right, clip.bottom,
                GetForegroundWindow() == g_uiHwnd ? "yes" : "no", g_softCursor.load() ? "on" : "off");
        }
        wasCap = cap;
        if (!cap) { hidden = shown = 0; g_softCursor = false; }
        else if (!g_softCursor.load()) {
            shown = 0;
            if (!hw && ++hidden > 10) { g_softCursor = true; if (logs < 40) { logs++; Log("ui: mouse: the Windows pointer is hidden; the overlay draws its own"); } }
            if (hw) hidden = 0;
        } else {
            // our own pointer makes ImGui ask Windows for "no pointer", so only the show count tells
            hidden = 0;
            if ((ci.flags & CURSOR_SHOWING) && ++shown > 60) g_softCursor = false;
            if (!(ci.flags & CURSOR_SHOWING)) shown = 0;
        }
        if (g_softCursor.load()) io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   // don't fight over the shape
    }
    bool post = g_postOn.load();   // build 107
    if (!post) PM.wasOn = false;
    bool dc = DC.show.load();      // build 108
    g_uiBlock = dc;
    io.MouseDrawCursor = (show || post || dc) && cap && g_softCursor.load();
    if (cap) {
        uint32_t b = U.navBits;
        io.NavInputs[ImGuiNavInput_Activate] = (b & PB_A) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_Cancel] = (b & PB_B) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_Input] = (b & PB_Y) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_DpadLeft] = (b & PB_LEFT) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_DpadRight] = (b & PB_RIGHT) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_DpadUp] = (b & PB_UP) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_DpadDown] = (b & PB_DOWN) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_FocusPrev] = (b & PB_LB) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_FocusNext] = (b & PB_RB) ? 1.0f : 0.0f;
    } else {
        // nothing reaches the overlay while it doesn't have the input
        io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        for (bool& m : io.MouseDown) m = false;
        for (bool& k : io.KeysDown) k = false;
        io.KeyCtrl = io.KeyShift = io.KeyAlt = false;
    }
    ImGui::NewFrame();
    ImGui::PushFont(g_fBody);
    if (!(show && PG.open.load())) MM.wasOpen = false;   // intro plays again next time
    if (post) PmFrame();
    else if (show && PG.open.load()) MmFrame();
    else if (show) UiMainPanel(io.DisplaySize);
    else if (g_cfg.devTools && U.screenVt != SCENE_BTL) UiStatusBar(io.DisplaySize);   // build 109: old text line for testing only
    if (!post && !dc) HudFrame();
    DcFrame();
    ImGui::PopFont();
    ImGui::EndFrame();
}

static HRESULT WINAPI H_PresentInner(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d);
static HRESULT WINAPI H_Present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    if (g_presentDepth > 0 && g_firstPresent) return g_firstPresent(dev, a, b, c, d);   // called again through another hook
    g_presentDepth++;
    HRESULT r = H_PresentInner(dev, a, b, c, d);
    g_presentDepth--;
    return r;
}
static HRESULT WINAPI H_PresentInner(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    if (g_resetImgui && dev == g_dev && g_imgui) {
        g_resetImgui = false;
        art::FreeAll(false);                     // build 90: game pictures belong to the old device
        ImGui_ImplDX9_Shutdown();
        bool ok = ImGui_ImplDX9_Init(dev);
        Log("ui: overlay moved to the new device (%s)", ok ? "ok" : "FAILED");
        if (!ok) g_imguiFailed = true;
    }
    if (dev == g_dev && !g_imguiFailed) {
        if (g_presents++ == 0) { g_presentTid = GetCurrentThreadId(); Log("ui: first picture presented (thread %lu)", g_presentTid); }
        if (!g_imgui) {
            g_imgui = UiInit(dev);
            if (!g_imgui && *reinterpret_cast<HWND*>(addr::G_HWND)) g_imguiFailed = true;
        }
        HRESULT coop = g_imgui ? dev->TestCooperativeLevel() : E_FAIL;
        if (g_imgui && coop != D3D_OK && g_drawLog > 0) { g_drawLog--; Log("ui: draw check: device not ready (%08lX), nothing drawn", (unsigned long)coop); }
        if (g_imgui && coop == D3D_OK) {
            UiFrame(dev);
            ImGui::Render();
            ImDrawData* dd = ImGui::GetDrawData();
            if (g_drawLog > 0) {
                g_drawLog--;
                IDirect3DSurface9* rt = nullptr;
                D3DSURFACE_DESC rd{};
                if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &rt)) && rt) { rt->GetDesc(&rd); rt->Release(); }
                DWORD cw = 0, st = 0, sc = 0;
                dev->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
                dev->GetRenderState(D3DRS_STENCILENABLE, &st);
                dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &sc);
                Log("ui: draw check: picture %u, %d draw lists, %d vertices, display %.0fx%.0f, back buffer %ux%u, game states before ours: colour write %lX, stencil %lu, scissor %lu",
                    g_presents, dd ? dd->CmdListsCount : -1, dd ? dd->TotalVtxCount : -1, dd ? dd->DisplaySize.x : 0.f, dd ? dd->DisplaySize.y : 0.f,
                    rd.Width, rd.Height, cw, st, sc);
            }
            if (g_drawLog > 0) {
                IDirect3DSurface9* ds = nullptr;
                D3DSURFACE_DESC dsd{};
                if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds) { ds->GetDesc(&dsd); ds->Release(); }
                Log("ui: draw check: game depth surface %ux%u", dsd.Width, dsd.Height);
            }
            if (dd && dd->CmdListsCount > 0) {
                IDirect3DSurface9 *oldRt = nullptr, *bb = nullptr;
                dev->GetRenderTarget(0, &oldRt);
                if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) dev->SetRenderTarget(0, bb);
                // build 62: no depth surface while we draw (a smaller one left set by the
                // game makes drawing to the full-size back buffer fail)
                IDirect3DSurface9* oldDs = nullptr;
                dev->GetDepthStencilSurface(&oldDs);
                if (oldDs) dev->SetDepthStencilSurface(nullptr);
                if (SUCCEEDED(dev->BeginScene())) {
                    ImGui_ImplDX9_RenderDrawData(dd);
                    dev->EndScene();
                }
                if (oldDs) { dev->SetDepthStencilSurface(oldDs); oldDs->Release(); }
                if (oldRt) { dev->SetRenderTarget(0, oldRt); oldRt->Release(); }
                if (bb) bb->Release();
            }
        }
    }
    else if (g_drawLog > 0) { g_drawLog--; Log("ui: draw check: picture presented by another device (%p, ours %p) or the overlay is off (%d)", (void*)dev, (void*)g_dev, (int)g_imguiFailed); }
    return o_Present(dev, a, b, c, d);
}
static HRESULT WINAPI H_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_imgui && dev == g_dev) ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT r = o_Reset(dev, pp);
    if (g_imgui && dev == g_dev && SUCCEEDED(r)) ImGui_ImplDX9_CreateDeviceObjects();
    Log("ui: display reset (%s)", SUCCEEDED(r) ? "ok" : "failed");
    return r;
}

// Waits for the game's Direct3D device, then puts our two functions in its table.
static DWORD WINAPI UiHookThread(LPVOID) {
    for (int i = 0; i < 6000; i++) {          // up to 10 minutes
        uint32_t r = *reinterpret_cast<volatile uint32_t*>(G_RENDERER);
        IDirect3DDevice9* dev = r ? *reinterpret_cast<IDirect3DDevice9* volatile*>(r + 0x214) : nullptr;
        if (dev) {
            Sleep(500);                       // let the game finish setting it up
            void** vt = *reinterpret_cast<void***>(dev);
            DWORD old;
            if (!VirtualProtect(&vt[16], 2 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
                Log("ui: could not hook the renderer (VirtualProtect failed, error %lu); no overlay", GetLastError());
                return 0;
            }
            g_dev = dev;
            o_Reset = (Reset_t)vt[16];
            o_Present = (Present_t)vt[17];
            g_firstPresent = o_Present;
            vt[16] = (void*)H_Reset;
            vt[17] = (void*)H_Present;
            VirtualProtect(&vt[16], 2 * sizeof(void*), old, &old);
            FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
            Log("ui: renderer found (device %p), overlay hooks in place", (void*)dev);
            Sleep(5000);
            if (!g_presents) Log("ui: WARNING: no picture presented through the hook in 5 s; the overlay may not show");
            return 0;
        }
        Sleep(100);
    }
    Log("ui: the game's renderer never appeared; no overlay");
    return 0;
}

// Build 63 (test 61/62): during the fight the game stopped presenting through our
// hook, so the overlay was invisible afterwards. Every game frame: if no picture went
// through the hook for half a second, say what the device table holds now and put
// our two functions back (chaining to whatever is there now).
static void UiHookWatch() {
    static uint32_t lastPresents = 0, still = 0;
    static int logs = 0;
    if (!o_Present || g_imguiFailed) return;
    if (g_presents != lastPresents) { lastPresents = g_presents; still = 0; return; }
    if (++still != 30) return;
    uint32_t r = *reinterpret_cast<volatile uint32_t*>(G_RENDERER);
    IDirect3DDevice9* dev = r ? *reinterpret_cast<IDirect3DDevice9* volatile*>(r + 0x214) : nullptr;
    if (!dev) { if (logs < 20) { logs++; Log("ui: hook watch: no pictures for half a second, and no device right now"); } return; }
    void** vt = *reinterpret_cast<void***>(dev);
    auto modName = [](void* a, char* out, size_t n) {
        HMODULE m = nullptr;
        out[0] = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(m, path, MAX_PATH)) { const char* b = strrchr(path, '\\'); snprintf(out, n, "%s", b ? b + 1 : path); }
        }
        if (!out[0]) snprintf(out, n, "no module");
    };
    char mp[64], mr[64], mt[64];
    modName(vt[17], mp, sizeof mp);
    modName(vt[16], mr, sizeof mr);
    modName(vt, mt, sizeof mt);
    if (logs < 20) {
        logs++;
        Log("ui: hook watch: no pictures through our hook for half a second (%u so far). Device %p (ours %p), table %p (%s), Present %p (%s, ours %p), Reset %p (%s)",
            g_presents, (void*)dev, (void*)g_dev, (void*)vt, mt, vt[17], mp, (void*)H_Present, vt[16], mr);
    }
    if (dev != g_dev) {
        // a new device: ImGui's textures belong to the old one, so start ImGui over on the new one
        if (logs < 20) { logs++; Log("ui: hook watch: the game uses a new device; the overlay moves to it"); }
        g_resetImgui = true;
        g_dev = dev;
    }
    DWORD old;
    if (vt[17] != (void*)H_Present || vt[16] != (void*)H_Reset) {
        if (VirtualProtect(&vt[16], 2 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            if (vt[16] != (void*)H_Reset) { o_Reset = (Reset_t)vt[16]; vt[16] = (void*)H_Reset; }
            if (vt[17] != (void*)H_Present) { o_Present = (Present_t)vt[17]; vt[17] = (void*)H_Present; }
            VirtualProtect(&vt[16], 2 * sizeof(void*), old, &old);
            FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
            if (logs < 20) { logs++; Log("ui: hook watch: our hook put back in the device table"); }
        } else if (logs < 20) { logs++; Log("ui: hook watch: could not put the hook back (error %lu)", GetLastError()); }
    }
}
static void UiStart() {
    InitializeCriticalSection(&g_lobbyLock);
    UiLoadSettings();
    CreateThread(nullptr, 0, UiHookThread, nullptr, 0, nullptr);
}
#endif // UI_HARNESS
