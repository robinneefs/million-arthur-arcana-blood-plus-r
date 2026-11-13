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
} U;

static ImFont *g_fBody = nullptr, *g_fTitle = nullptr, *g_fSmall = nullptr;

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
static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
// Build 58 diagnostics: which fields of the title screen object change when the
// "press start" picture turns into the main menu (so a later build can tell the two
// apart exactly). Fields that keep changing (timers) are only logged a few times.
static void UiTitleWatch(uint32_t vt, uint32_t ptr) {
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
// Every frame on the game thread (from PollHotkeys), also when the window is in the background.
static void UiGameThreadTick(bool focused) {
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
    if (toggle) {
        bool v = !U.visible.load();
        U.visible = v;
        if (!v) U.drain = true;
        Log("ui: overlay %s", v ? "shown" : "hidden");
    }
    // Opens by itself the first time the main menu is up (build 58, test 57: not on the
    // "press start" picture). The title picture and the main menu are one screen
    // (CMLTitle), so: wait for a button press on it, its release, then half a second.
    static uint32_t prevAny = 0, titleBits = 0;
    if (!U.autoOpened) {
        if (vt != VT_TITLE) { if (U.titleStep < 3) U.titleStep = 0; }
        else if (U.titleStep == 0) {             // a new press (a button that is always on doesn't count)
            uint32_t fresh = U.anyRaw & ~prevAny;
            bool key = focused && (KeyDown(VK_RETURN) || KeyDown(VK_SPACE));
            if (fresh || key) { U.titleStep = 1; titleBits = fresh; }
        }
        else if (U.titleStep == 1) {
            if (!(U.anyRaw & titleBits) && !(focused && (KeyDown(VK_RETURN) || KeyDown(VK_SPACE)))) { U.titleStep = 2; U.titleWait = 0; }
        }
        else if (U.titleStep == 2 && ++U.titleWait >= 30) {
            U.titleStep = 3;
            U.autoOpened = true;
            if (!U.visible) { U.visible = true; Log("ui: overlay opened on the main menu (F1 hides it)"); }
        }
        if (U.visible) U.autoOpened = true;   // opened by hand with F1 first
        UiTitleWatch(vt, ptr);
    }
    prevAny = U.anyRaw;
    // the game's own synced screens (old menu path) and tests: overlay steps aside
    if (U.visible && (inSynced || g_mode != Mode::Idle || g_auto)) { U.visible = false; U.drain = true; Log("ui: overlay hidden (game screen in step / test running)"); }
    U.capture = U.visible.load() && focused && !inSynced && g_mode == Mode::Idle && !g_auto;
    // requests
    int c = U.cmd.exchange(0);
    if (c == 1 && N.st == NS_OFF) {
        if (g_mode != Mode::Idle || g_auto) SetStatus("stop the test first (F7 / F8), then connect");
        else {
            snprintf(g_uiName, sizeof g_uiName, "%s", U.name);
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
static HWND g_uiHwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static float g_uiScale = 1.0f;
static uint32_t g_presents = 0;
static DWORD g_presentTid = 0;

// Build 58 (test 57: no mouse pointer): the game hides the Windows pointer. While the
// overlay has the input we show it again (on the window's own thread, where the
// pointer's show count lives) and put it back as it was when the overlay lets go.
// Build 59 (test 58: the pointer vanished after closing and opening a few times):
// - checked on every window message, not only once, since the game hides it again;
// - an empty pointer (the game's "no pointer" shape) is replaced by the arrow;
// - the pointer is kept inside the game window while the overlay is open (and put in
//   the middle if it was outside), so it can't wander off to another screen unseen;
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
        if (!g_clipped) {
            RECT rc;
            GetClientRect(h, &rc);
            POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
            ClientToScreen(h, &tl);
            ClientToScreen(h, &br);
            RECT scr{tl.x, tl.y, br.x, br.y};
            if (scr.right - scr.left > 16 && scr.bottom - scr.top > 16) {
                GetClipCursor(&g_oldClip);
                POINT p;
                if (GetCursorPos(&p) && (p.x < scr.left || p.x >= scr.right || p.y < scr.top || p.y >= scr.bottom))
                    SetCursorPos((scr.left + scr.right) / 2, (scr.top + scr.bottom) / 2);
                ClipCursor(&scr);
                g_clipped = true;
            }
        }
    } else {
        while (g_cursorBumps > 0) { ShowCursor(FALSE); g_cursorBumps--; }
        if (g_clipped) {
            g_clipped = false;
            ClipCursor(g_oldClip.right > g_oldClip.left ? &g_oldClip : nullptr);
        }
    }
}

static LRESULT CALLBACK UiWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
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

static void UiLoadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    char dir[MAX_PATH];
    GetWindowsDirectoryA(dir, MAX_PATH);
    char body[MAX_PATH], title[MAX_PATH];
    snprintf(body, MAX_PATH, "%s\\Fonts\\%s", dir, theme::FONT_BODY);
    snprintf(title, MAX_PATH, "%s\\Fonts\\%s", dir, theme::FONT_TITLE);
    auto load = [&](const char* path, float px) -> ImFont* {
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            ImFont* f = io.Fonts->AddFontFromFileTTF(path, px * scale);
            if (f) return f;
        }
        ImFontConfig c;
        c.SizePixels = px * scale;
        return io.Fonts->AddFontDefault(&c);
    };
    g_fBody = load(body, theme::BODY_PX);
    g_fTitle = load(title, theme::TITLE_PX);
    g_fSmall = load(body, theme::SMALL_PX);
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
    g_uiScale = h / 1080.0f;
    if (g_uiScale < 0.6f) g_uiScale = 0.6f;
    UiLoadFonts(g_uiScale);
    theme::Apply(g_uiScale);
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
    ImGui::SameLine();
    ImGui::TextColored(theme::TextDim(), "(not used by the game yet)");
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
static const char* UiColorName(uint8_t c) { static char b[4][16]; static int k; k = (k + 1) & 3; snprintf(b[k], 16, "Colour %u", c + 1u); return b[k]; }
static const char* UiAssistName(uint8_t c) { static char b[4][16]; static int k; k = (k + 1) & 3; snprintf(b[k], 16, "Set %u", c + 1u); return b[k]; }

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
    if (UiPickCombo(id, &s.p.chara, LB_CHARS, true, LbCharName, ed)) LbChanged();
    ImGui::TextColored(theme::TextDim(), "Colour");
    snprintf(id, sizeof id, "##co%d", side);
    if (UiPickCombo(id, &s.p.color, LB_COLORS, false, UiColorName, ed)) LbChanged();
    ImGui::TextColored(theme::TextDim(), "Assist set");
    snprintf(id, sizeof id, "##as%d", side);
    if (UiPickCombo(id, &s.p.assist, LB_ASSISTS, false, UiAssistName, ed)) LbChanged();
    ImGui::TextColored(theme::TextDim(), "Stage wish");
    snprintf(id, sizeof id, "##st%d", side);
    if (UiPickCombo(id, &s.p.stage, LB_STAGES, true, LbStageName, ed)) LbChanged();
    ImGui::Spacing();
    if (s.locked) ImGui::TextColored(theme::Good(), "LOCKED IN");
    else ImGui::TextColored(theme::TextDim(), "choosing...");
}

static void UiSetupPhase(LbSide& me, LbSide& them, bool host) {
    UiHeading("Match setup");
    UiNote("Placeholder lists until the game's own characters and stages are found. If the two stage wishes differ, one of them is picked at random.");
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
    UiNote("Build 57 stops here: starting the fight from these settings comes in a later build.");
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
        row("Assist set", UiAssistName(s.assist[0]), UiAssistName(s.assist[1]));
        ImGui::EndTable();
    }
    ImGui::Text("Stage: %s    Rounds: %u    Input delay: %u    Match %u", LbStageName(s.stage), s.rounds, s.delay, me.matchNo);
    ImGui::Text("Settings checksum %08X", LB.sum);
    ImGui::SameLine();
    if (LB.confirmed > 0) ImGui::TextColored(theme::Good(), host ? "- same on the other PC" : "- received from player 1");
    else if (LB.confirmed < 0) ImGui::TextColored(theme::Bad(), "- DIFFERENT on the other PC!");
    else ImGui::TextColored(theme::TextDim(), "- waiting for the other PC to confirm");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (UiBigButton("Test: pretend the match ended")) { me.phase = LP_POST; me.choice = LCH_NONE; me.choiceMatch = 0; LbChanged(); LbNote("match %u ended (test button)", me.matchNo); }
}

static void UiPostPhase(LbSide& me, const LbSide& them) {
    UiHeading("Match over");
    ImGui::Spacing();
    struct { uint8_t c; const char* label; } opts[] = {{LCH_REMATCH, "Rematch"}, {LCH_SETUP, "Back to match setup"}, {LCH_LOBBY, "Back to lobby"}};
    for (auto& o : opts) {
        bool sel = me.choice == o.c && me.choiceMatch == me.matchNo;
        if (UiBigButton(o.label, sel)) { me.choice = o.c; me.choiceMatch = me.matchNo; LbChanged(); }
        ImGui::SameLine();
    }
    if (UiBigButton("Leave")) LB.leaveNow = true;
    ImGui::Spacing();
    bool theirs = them.choice != LCH_NONE && them.choiceMatch == me.matchNo;
    ImGui::Text("You: %s", me.choice != LCH_NONE && me.choiceMatch == me.matchNo ? LB_CHOICE_NAME[me.choice] : "choose one");
    ImGui::Text("%s: %s", them.name, theirs ? LB_CHOICE_NAME[them.choice] : "choosing...");
    UiNote("It happens when you both choose the same. Leave always works straight away.");
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
    UiNote("Which controller is yours on this PC. Changing the button layout here comes in a later build.");
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
    ImGui::Separator();
    ImGui::Text("Game's button layout for player 1");
    ImGui::PushFont(g_fSmall);
    const uint32_t* cfg = N.saved ? N.origCfg : (const uint32_t*)G_PAD_CFG;
    for (int i = 0; i < 12; i++) {
        char b[96];
        UiButtonsText(cfg[i], b, sizeof b);
        ImGui::Text("Game button %2d: %s", i + 1, b[0] ? b : "-");
        if (i % 3 != 2) ImGui::SameLine(ImGui::GetContentRegionAvail().x * ((i % 3) + 1) / 3.0f);
    }
    ImGui::PopFont();
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

static void UiStatusBar(const ImVec2& disp) {
    char ns[200];
    EnterCriticalSection(&g_statusLock);
    snprintf(ns, sizeof ns, "%s", g_netStatus);
    LeaveCriticalSection(&g_statusLock);
    if (!U.netOn || !ns[0]) return;
    ImGui::SetNextWindowPos(ImVec2(disp.x - 10, 10), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PushFont(g_fSmall);
    ImGui::Text("%s", ns);
    ImGui::PopFont();
    ImGui::End();
}

#ifndef UI_HARNESS
static void UiFrame(IDirect3DDevice9* dev) {
    ImGuiIO& io = ImGui::GetIO();
    // the mouse pointer is only ours while the overlay has the input
    if (U.capture.load() && !g_softCursor.load()) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
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
    io.MouseDrawCursor = show && cap && g_softCursor.load();
    if (cap) {
        uint32_t b = U.navBits;
        io.NavInputs[ImGuiNavInput_Activate] = (b & PB_A) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_Cancel] = (b & PB_B) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_Input] = (b & PB_Y) ? 1.0f : 0.0f;
        io.NavInputs[ImGuiNavInput_Menu] = (b & PB_X) ? 1.0f : 0.0f;
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
    if (show) UiMainPanel(io.DisplaySize);
    else UiStatusBar(io.DisplaySize);
    ImGui::PopFont();
    ImGui::EndFrame();
}

static HRESULT WINAPI H_Present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    if (dev == g_dev && !g_imguiFailed) {
        if (g_presents++ == 0) { g_presentTid = GetCurrentThreadId(); Log("ui: first picture presented (thread %lu)", g_presentTid); }
        if (!g_imgui) {
            g_imgui = UiInit(dev);
            if (!g_imgui && *reinterpret_cast<HWND*>(addr::G_HWND)) g_imguiFailed = true;
        }
        if (g_imgui && dev->TestCooperativeLevel() == D3D_OK) {
            UiFrame(dev);
            ImGui::Render();
            ImDrawData* dd = ImGui::GetDrawData();
            if (dd && dd->CmdListsCount > 0) {
                IDirect3DSurface9 *oldRt = nullptr, *bb = nullptr;
                dev->GetRenderTarget(0, &oldRt);
                if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) dev->SetRenderTarget(0, bb);
                if (SUCCEEDED(dev->BeginScene())) {
                    ImGui_ImplDX9_RenderDrawData(dd);
                    dev->EndScene();
                }
                if (oldRt) { dev->SetRenderTarget(0, oldRt); oldRt->Release(); }
                if (bb) bb->Release();
            }
        }
    }
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
static void UiStart() {
    InitializeCriticalSection(&g_lobbyLock);
    UiLoadSettings();
    CreateThread(nullptr, 0, UiHookThread, nullptr, 0, nullptr);
}
#endif // UI_HARNESS
