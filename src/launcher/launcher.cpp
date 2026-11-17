// Arcana Blood +R launcher (build 120).
//
// A small window in the mod's crimson style (Dear ImGui on Direct3D 9, the mod's fonts and
// logo). It checks GitHub for a newer version (update.inc) and can install it, and Play
// starts the game with the mod (game.inc): it makes sure our small dinput8.dll is in the game
// folder, then starts the game with ARCANA_PLUSR_DIR set, which tells that DLL to load the mod
// from this folder. The game started from Steam directly stays the plain game. Nothing is
// injected into the game, and nothing downloaded is run except the updated launcher itself.
//
// Usage:
//   arcana_launcher.exe             the window
//   arcana_launcher.exe --play      skip the window: put the DLL in place and start the game
//   (--updated v0.2.0 beta: started by the updater; shows "updated to")
//
// Screens (the mock-up): checking, up to date, update available, updating, updated, update did
// not work, plus starting the game / its problems. Controls: mouse, Enter = the gold button,
// Esc = close, controller A = the gold button, X = the other button, B = close.

#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <atomic>
#include "../mod/version.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include <wincodec.h>

// ---- log: launcher_log.txt next to the launcher ----
static FILE* g_log = nullptr;
static void Say(const wchar_t* fmt, ...) {
    if (!g_log) return;
    wchar_t line[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(line, 1023, fmt, ap);
    va_end(ap);
    line[1023] = 0;
    fwprintf(g_log, L"%ls\n", line);
    fflush(g_log);
}

static std::wstring W8(const char* s) { std::wstring w; while (*s) w += (wchar_t)(unsigned char)*s++; return w; }
#include "update.inc"
#include "game.inc"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---- window + Direct3D ----
static const float WW = 720, WH = 440;      // design size (scaled by the screen's DPI)
static float g_s = 1.0f;                    // DPI scale
static HWND g_hwnd = nullptr;
static IDirect3D9* g_d3d = nullptr;
static IDirect3DDevice9* g_dev = nullptr;
static D3DPRESENT_PARAMETERS g_pp{};
static IDirect3DTexture9* g_logo = nullptr;
static float g_logoW = 1, g_logoH = 1;
static ImFont *g_fTitle = nullptr, *g_fText = nullptr, *g_fBold = nullptr;

static inline ImVec2 P(float x, float y) { return ImVec2(x * g_s, y * g_s); }
static inline ImU32 C(int r, int g, int b, float a = 1.0f) { return IM_COL32(r, g, b, (int)(a * 255)); }
// house colours (the mod's Crimson palette)
static const ImU32 kGold = IM_COL32(255, 206, 96, 255), kCream = IM_COL32(255, 247, 232, 255), kDim = IM_COL32(232, 196, 170, 255),
                   kFaint = IM_COL32(200, 154, 140, 255), kRed = IM_COL32(215, 38, 32, 255), kGreen = IM_COL32(150, 230, 170, 255),
                   kWarn = IM_COL32(255, 138, 112, 255), kInk = IM_COL32(42, 4, 4, 255);

static LRESULT WINAPI WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    switch (m) {
    case WM_NCHITTEST: {   // drag the window by its title bar (not by its two buttons)
        POINT pt{(short)LOWORD(l), (short)HIWORD(l)};
        ScreenToClient(h, &pt);
        if (pt.y >= 0 && pt.y < 34 * g_s && pt.x < (WW - 80) * g_s) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SIZE:
        if (g_dev && w != SIZE_MINIMIZED) {
            g_pp.BackBufferWidth = LOWORD(l);
            g_pp.BackBufferHeight = HIWORD(l);
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool LoadLogo(const std::wstring& path) {
    static const GUID FMT_BGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = false;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) hr = f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &fr);
    if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&cv);
    if (SUCCEEDED(hr)) hr = cv->Initialize(fr, FMT_BGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = cv->GetSize(&w, &h);
    if (SUCCEEDED(hr) && w && h && w <= 4096 && h <= 4096) {
        std::vector<uint32_t> px((size_t)w * h);
        if (SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)(px.size() * 4), (BYTE*)px.data())) &&
            SUCCEEDED(g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_logo, nullptr))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(g_logo->LockRect(0, &lr, nullptr, 0))) {
                for (UINT y = 0; y < h; y++) memcpy((uint8_t*)lr.pBits + (size_t)y * lr.Pitch, px.data() + (size_t)y * w, (size_t)w * 4);
                g_logo->UnlockRect(0);
                g_logoW = (float)w; g_logoH = (float)h;
                ok = true;
            }
        }
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (f) f->Release();
    return ok;
}

// ---- controller (XInput, loaded by hand: no extra DLL needed when there is none) ----
struct XState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XState*);
static XInputGetState_t g_xget = nullptr;
static WORD g_padPrev = 0;
static WORD PadPressed() {
    if (!g_xget) {
        static bool tried = false;
        if (tried) return 0;
        tried = true;
        HMODULE x = LoadLibraryW(L"xinput1_4.dll");
        if (!x) x = LoadLibraryW(L"xinput1_3.dll");
        if (!x) x = LoadLibraryW(L"xinput9_1_0.dll");
        if (x) g_xget = (XInputGetState_t)GetProcAddress(x, "XInputGetState");
        if (!g_xget) return 0;
    }
    WORD now = 0;
    for (DWORD i = 0; i < 4; i++) { XState s{}; if (g_xget(i, &s) == 0) now |= s.buttons; }
    WORD pressed = now & ~g_padPrev;
    g_padPrev = now;
    if (GetForegroundWindow() != g_hwnd) return 0;
    return pressed;
}
static const WORD PAD_A = 0x1000, PAD_B = 0x2000, PAD_X = 0x4000;

// ---- drawing helpers ----
// Build 120b: every text size has its own font in the atlas, made at exactly that pixel size,
// so small text is sharp (it was shrunk from one big font before), and small sizes are a bit
// bigger than in the mock-up so they read well.
static float Bump(float px) { return px <= 15 ? px + 2 : px; }
struct SizedFont { int px; ImFont* f; };
struct Family { const char* file; std::vector<SizedFont> sizes; };
static Family g_famTitle{"Michroma-Regular.ttf", {}}, g_famText{"SairaSemiCondensed-Regular.ttf", {}}, g_famBold{"SairaSemiCondensed-Bold.ttf", {}};
static Family* Fam(ImFont* f) { return f == g_fTitle ? &g_famTitle : f == g_fBold ? &g_famBold : &g_famText; }
static ImFont* Sized(ImFont* f, float px, float& drawPx) {
    drawPx = Bump(px) * g_s;
    Family* fam = Fam(f);
    int want = (int)(drawPx + 0.5f);
    ImFont* best = f;
    int bestD = 1 << 30;
    for (auto& s : fam->sizes) {
        int d = abs(s.px - want);
        if (d < bestD) { bestD = d; best = s.f; }
    }
    if (best != f && bestD == 0) drawPx = (float)want;
    return best;
}
static float TextW(ImFont* f, float px, const char* s) {
    float sz;
    ImFont* g = Sized(f, px, sz);
    return g->CalcTextSizeA(sz, FLT_MAX, 0, s).x / g_s;
}
static void Text(ImFont* f, float px, float x, float y, ImU32 col, const char* s, int align = 0, float wrap = 0) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float sz;
    ImFont* g = Sized(f, px, sz);
    ImVec2 ts = g->CalcTextSizeA(sz, FLT_MAX, wrap * g_s, s);
    ImVec2 p = P(x, y);
    p.y -= (sz - px * g_s) * 0.5f;          // bigger text stays centred on the same line
    if (align == 1) p.x -= ts.x * 0.5f; else if (align == 2) p.x -= ts.x;
    dl->AddText(g, sz, ImVec2(floorf(p.x), floorf(p.y)), col, s, nullptr, wrap * g_s);
}
static float TextH(ImFont* f, float px, const char* s, float wrap) {
    float sz;
    ImFont* g = Sized(f, px, sz);
    return g->CalcTextSizeA(sz, FLT_MAX, wrap * g_s, s).y / g_s;
}
static bool Hover(float x0, float y0, float x1, float y1) { return ImGui::IsMouseHoveringRect(P(x0, y0), P(x1, y1), false); }
static bool Clicked(float x0, float y0, float x1, float y1) { return Hover(x0, y0, x1, y1) && ImGui::IsMouseReleased(0); }

// the gold button (main action); returns true when clicked
static bool GoldButton(float x0, float y0, float x1, float y1, const char* label, bool enabled, float t) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    bool hv = enabled && Hover(x0, y0, x1, y1);
    if (enabled) {
        float glow = 0.45f + 0.15f * sinf(t * 2.4f) + (hv ? 0.2f : 0);
        for (int i = 6; i >= 1; i--) dl->AddRect(P(x0 - i * 2.5f, y0 - i * 2.5f), P(x1 + i * 2.5f, y1 + i * 2.5f), C(255, 80, 36, glow * 0.10f), 4 * g_s, 0, 2.5f * g_s);
        dl->AddRectFilledMultiColor(P(x0, y0), P(x1, y1), hv ? C(255, 232, 160) : C(255, 218, 128), hv ? C(255, 232, 160) : C(255, 218, 128),
                                    hv ? C(255, 196, 80) : C(255, 182, 60), hv ? C(255, 196, 80) : C(255, 182, 60));
        dl->AddRect(P(x0, y0), P(x1, y1), C(90, 14, 6), 0, 0, 2 * g_s);
        Text(g_fTitle, 18, (x0 + x1) / 2, (y0 + y1) / 2 - 12, kInk, label, 1);
    } else {
        dl->AddRectFilled(P(x0, y0), P(x1, y1), C(255, 206, 96, 0.18f));
        dl->AddRect(P(x0, y0), P(x1, y1), C(90, 14, 6, 0.6f), 0, 0, 2 * g_s);
        Text(g_fTitle, 18, (x0 + x1) / 2, (y0 + y1) / 2 - 12, C(255, 247, 232, 0.55f), label, 1);
    }
    return enabled && Clicked(x0, y0, x1, y1);
}
// the dark outlined button (second choice)
static bool DarkButton(float x0, float y0, float x1, float y1, const char* l1, const char* l2 = nullptr, ImU32 col = kDim) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    bool hv = Hover(x0, y0, x1, y1);
    dl->AddRectFilled(P(x0, y0), P(x1, y1), hv ? C(60, 10, 10, 0.95f) : C(26, 5, 5, 0.9f));
    dl->AddRect(P(x0, y0), P(x1, y1), hv ? kGold : C(255, 206, 96, 0.45f), 0, 0, 1 * g_s);
    if (l2) {
        Text(g_fTitle, 10, (x0 + x1) / 2, (y0 + y1) / 2 - 15, hv ? kCream : col, l1, 1);
        Text(g_fTitle, 10, (x0 + x1) / 2, (y0 + y1) / 2 + 1, hv ? kCream : col, l2, 1);
    } else Text(g_fTitle, 10, (x0 + x1) / 2, (y0 + y1) / 2 - 7, hv ? kCream : col, l1, 1);
    return Clicked(x0, y0, x1, y1);
}
// the card with the red border and the thin gold line inside
static void Card(float x0, float y0, float x1, float y1, bool goldEdge = false) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(P(x0, y0), P(x1, y1), C(26, 5, 5, 0.92f));
    dl->AddRect(P(x0, y0), P(x1, y1), goldEdge ? kGold : kRed, 0, 0, 1 * g_s);
    dl->AddRect(P(x0 + 3, y0 + 3), P(x1 - 3, y1 - 3), goldEdge ? C(215, 38, 32, 0.6f) : C(255, 206, 96, 0.45f), 0, 0, 1 * g_s);
}
static void Spinner(float cx, float cy, float r, float t) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddCircle(P(cx, cy), r * g_s, C(255, 206, 96, 0.25f), 32, 3 * g_s);
    dl->PathArcTo(P(cx, cy), r * g_s, t * 6.0f, t * 6.0f + 1.6f, 16);
    dl->PathStroke(kGold, false, 3 * g_s);
}
static void Tick(float x, float y, float sz) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddLine(P(x, y + sz * 0.55f), P(x + sz * 0.38f, y + sz * 0.9f), kGreen, 2.6f * g_s);
    dl->AddLine(P(x + sz * 0.38f, y + sz * 0.9f), P(x + sz, y + sz * 0.15f), kGreen, 2.6f * g_s);
}
static void Warn(float x, float y, float sz) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddTriangle(P(x + sz / 2, y), P(x + sz, y + sz * 0.9f), P(x, y + sz * 0.9f), kWarn, 2.2f * g_s);
    dl->AddLine(P(x + sz / 2, y + sz * 0.35f), P(x + sz / 2, y + sz * 0.6f), kWarn, 2.2f * g_s);
    dl->AddCircleFilled(P(x + sz / 2, y + sz * 0.74f), 1.4f * g_s, kWarn);
}

// background: crimson gradient, a slowly turning magic circle, rising embers
struct Ember { float x, y, v, r, ph; };
static std::vector<Ember> g_embers;
static void Background(float t, float dt) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilledMultiColor(P(0, 0), P(WW, WH), C(74, 10, 12), C(58, 7, 9), C(10, 1, 2), C(22, 3, 4));
    dl->AddCircleFilled(P(560, 110), 260 * g_s, C(255, 80, 36, 0.06f), 64);
    float cx = 600, cy = 150, a = t * 0.08f;
    dl->AddCircle(P(cx, cy), 190 * g_s, C(255, 206, 96, 0.16f), 96, 1.2f * g_s);
    dl->AddCircle(P(cx, cy), 158 * g_s, C(215, 38, 32, 0.18f), 96, 0.8f * g_s);
    for (int k = 0; k < 2; k++) {
        float b = a * (k ? -1.0f : 1.0f) + (k ? 3.14159f / 3 : 0);
        ImVec2 p[3];
        for (int i = 0; i < 3; i++) p[i] = P(cx + cosf(b + i * 2.0944f) * 175, cy + sinf(b + i * 2.0944f) * 175);
        dl->AddTriangle(p[0], p[1], p[2], C(255, 206, 96, 0.13f), 0.9f * g_s);
    }
    if (g_embers.empty())
        for (int i = 0; i < 26; i++) g_embers.push_back({(float)(rand() % 720), (float)(rand() % 440), 8.0f + rand() % 18, 0.8f + (rand() % 10) / 8.0f, (float)(rand() % 100)});
    for (auto& e : g_embers) {
        e.y -= e.v * dt;
        if (e.y < -10) { e.y = WH + 10; e.x = (float)(rand() % 720); }
        float fl = 0.35f + 0.3f * sinf(t * 3 + e.ph);
        dl->AddCircleFilled(P(e.x + sinf(t + e.ph) * 6, e.y), e.r * g_s, C(255, 150, 60, fl * 0.5f));
    }
    dl->AddRect(P(0, 0), P(WW, WH), C(255, 206, 96, 0.35f), 0, 0, 1 * g_s);
}

// ---- the screens ----
enum Ui { U_MAIN, U_STARTING, U_PROBLEM, U_FOREIGN };
static Ui g_ui = U_MAIN;
static std::string g_problem;                // U_PROBLEM: what went wrong starting the game
static std::wstring g_ourDir, g_gameDir;
static std::string g_gameDirShown;
static std::string g_updatedTo;              // --updated
static double g_startT = 0;
static bool g_quit = false;

static void ShortPath(const std::wstring& d) {
    char a[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, d.c_str(), -1, a, sizeof a, nullptr, nullptr);
    std::string s = a;
    if (s.size() > 52) {            // H:\SteamLibrary\...\Million Arthur Arcana Blood
        size_t first = s.find('\\', 3), last = s.rfind('\\');
        if (first != std::string::npos && last != std::string::npos && last > first) s = s.substr(0, first) + "\\...\\" + s.substr(last + 1);
    }
    g_gameDirShown = "Game found: " + s;
}

static void Play(bool replaceForeign) {
    g_gameDir = game::FindGameDir();
    if (g_gameDir.empty()) { g_ui = U_PROBLEM; g_problem = "Steam doesn't have Million Arthur: Arcana Blood installed (or Steam isn't installed). Install the game in Steam, then press Play again."; return; }
    ShortPath(g_gameDir);
    if (game::FindGamePid()) { g_ui = U_PROBLEM; g_problem = "The game is already running. Close it first, then press Play again."; return; }
    game::MoveFrom119(g_gameDir, g_ourDir);
    switch (game::PrepareGameFolder(g_gameDir, g_ourDir, replaceForeign)) {
    case game::P_OK: break;
    case game::P_FOREIGN: g_ui = U_FOREIGN; return;
    case game::P_NO_SOURCE: g_ui = U_PROBLEM; g_problem = "dinput8.dll is missing from the launcher's folder. Please reinstall Arcana Blood +R."; return;
    case game::P_RUNNING: g_ui = U_PROBLEM; g_problem = "The game is already running. Close it first, then press Play again."; return;
    default: g_ui = U_PROBLEM; g_problem = "Could not put the mod's dinput8.dll into the game folder. Is your antivirus blocking it? See launcher_log.txt."; return;
    }
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ArcanaBloodPlusR", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"GameDir", 0, REG_SZ, (const BYTE*)g_gameDir.c_str(), (DWORD)((g_gameDir.size() + 1) * 2));   // the uninstaller removes dinput8.dll there
        RegCloseKey(k);
    }
    std::string why;
    if (!game::Start(g_gameDir, g_ourDir, why)) { g_ui = U_PROBLEM; g_problem = why; return; }
    g_ui = U_STARTING;
    g_startT = ImGui::GetTime();
}

static void Frame(float t, float dt) {
    using namespace upd;
    ImGuiIO& io = ImGui::GetIO();
    WORD pad = PadPressed();
    bool enter = ImGui::IsKeyPressed(VK_RETURN, false) || ImGui::IsKeyPressed(VK_SPACE, false) || (pad & PAD_A);
    bool esc = ImGui::IsKeyPressed(VK_ESCAPE, false) || (pad & PAD_B);
    bool other = (pad & PAD_X) != 0;
    (void)io;
    Background(t, dt);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    int st = g_state.load();
    bool busy = st == S_UPDATING;

    // title bar
    dl->AddRectFilled(P(0, 0), P(WW, 34), C(8, 1, 2, 0.55f));
    dl->AddLine(P(0, 34), P(WW, 34), C(215, 38, 32, 0.5f), 1 * g_s);
    Text(g_fTitle, 10, 14, 11, kDim, "ARCANA BLOOD +R");
    {
        bool hv = Hover(WW - 80, 4, WW - 44, 30);
        dl->AddLine(P(WW - 68, 18), P(WW - 56, 18), hv ? kCream : kDim, 1.4f * g_s);
        if (hv && ImGui::IsMouseReleased(0)) ShowWindow(g_hwnd, SW_MINIMIZE);
        if (!busy) {
            bool hc = Hover(WW - 42, 4, WW - 6, 30);
            if (hc) dl->AddRectFilled(P(WW - 42, 4), P(WW - 6, 30), C(215, 38, 32, 0.7f));
            dl->AddLine(P(WW - 30, 11), P(WW - 18, 23), hc ? kCream : kDim, 1.4f * g_s);
            dl->AddLine(P(WW - 18, 11), P(WW - 30, 23), hc ? kCream : kDim, 1.4f * g_s);
            if (hc && ImGui::IsMouseReleased(0)) g_quit = true;
        }
    }
    // logo
    if (g_logo) {
        float w = 300, h = 300 * g_logoH / g_logoW;
        dl->AddImage((ImTextureID)g_logo, P(28, 58), P(28 + w, 58 + h));
    } else Text(g_fTitle, 26, 40, 120, kCream, "ARCANA BLOOD +R");

    // bottom bar
    dl->AddRectFilled(P(0, WH - 92), P(WW, WH), C(8, 1, 2, 0.5f));
    dl->AddLine(P(0, WH - 92), P(WW, WH - 92), C(215, 38, 32, 0.55f), 1 * g_s);
    bool steam = game::SteamRunning();
    // a status dot (drawn: the font has no dot character, it showed as "?")
    dl->AddCircleFilled(P(33, WH - 57), 4.5f * g_s, steam ? kGreen : kWarn, 16);
    Text(g_fText, 14, 44, WH - 66, steam ? kGreen : kWarn, steam ? "Steam online" : "Steam is not running: start Steam first");
    const char* sub = g_gameDirShown.c_str();
    if (st == S_AVAILABLE || st == S_FAILED) sub = "Both players need the same version online";
    Text(g_fText, 13, 28, WH - 45, kFaint, sub);

    const float RX = 372, RW = 318;
    const float BX0 = WW - 28 - 210, BY0 = WH - 74, BX1 = WW - 28, BY1 = WH - 18;

    if (g_ui == U_STARTING) {
        Text(g_fTitle, 11, RX, 70, kGold, "STARTING THE GAME");
        Card(RX, 92, RX + RW, 160);
        Spinner(RX + 30, 126, 11, t);
        Text(g_fTitle, 12, RX + 56, 106, kCream, "PLEASE WAIT");
        Text(g_fText, 15, RX + 56, 126, kDim, "The game opens in a moment");
        Text(g_fText, 14, RX, 176, kDim, "This window closes by itself once the game is up.", 0, RW);
        GoldButton(BX0, BY0, BX1, BY1, "PLAY", false, t);
        // close once the game has a window (or after 20 s)
        DWORD pid = game::FindGamePid();
        HWND w = nullptr;
        if (pid) {
            for (HWND h = FindWindowW(nullptr, nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
                DWORD p = 0;
                GetWindowThreadProcessId(h, &p);
                if (p == pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { w = h; break; }
            }
        }
        if (w || ImGui::GetTime() - g_startT > 20) g_quit = true;
        return;
    }
    if (g_ui == U_PROBLEM || g_ui == U_FOREIGN) {
        bool foreign = g_ui == U_FOREIGN;
        Text(g_fTitle, 11, RX, 70, kGold, foreign ? "GAME FOLDER" : "COULD NOT START");
        const char* msg = foreign ? "Another mod's dinput8.dll is in the game folder (for example a mod loader). Ours has to take its place. Yours is kept as dinput8.dll.before-plusr and comes back when you uninstall Arcana Blood +R."
                                  : g_problem.c_str();
        float h = TextH(g_fText, 15, msg, RW - 70);
        Card(RX, 92, RX + RW, 92 + h + 32);
        Warn(RX + 16, 106, 22);
        Text(g_fText, 15, RX + 50, 106, kDim, msg, 0, RW - 70);
        if (foreign) {
            if (DarkButton(BX0 - 150, BY0, BX0 - 16, BY1, "CANCEL") || esc) g_ui = U_MAIN;
            else if (GoldButton(BX0, BY0, BX1, BY1, "REPLACE", true, t) || enter) Play(true);
        } else {
            if (DarkButton(BX0 - 150, BY0, BX0 - 16, BY1, "BACK") || esc) g_ui = U_MAIN;
            else if (GoldButton(BX0, BY0, BX1, BY1, "PLAY", true, t) || enter) Play(false);
        }
        return;
    }

    // ---- the update side ----
    EnterCriticalSection(&g_lock);
    Release rel = g_rel;
    std::string fail = g_failWhy;
    bool canUpdate = g_canUpdate;
    LeaveCriticalSection(&g_lock);
    std::string mine = Pretty(MOD_VERSION);
    char line[160];
    bool playOk = !busy, primaryIsUpdate = false;

    if (!g_updatedTo.empty() && (st == S_CHECKING || st == S_UPTODATE || st == S_OFFLINE)) {
        Text(g_fTitle, 11, RX, 70, kGold, "VERSION");
        Card(RX, 92, RX + RW, 160, true);
        Tick(RX + 18, 114, 22);
        snprintf(line, sizeof line, "UPDATED TO %s", g_updatedTo.c_str());
        for (char* c = line; *c; c++) *c = (char)toupper((unsigned char)*c);
        Text(g_fTitle, 12, RX + 56, 106, kCream, line);
        Text(g_fText, 15, RX + 56, 126, kDim, "Settings and My setup kept");
        Text(g_fText, 14, RX, 176, kDim, "Press Play. The game folder is checked too, so the mod's small dinput8.dll is always in place.", 0, RW);
    } else if (st == S_CHECKING) {
        Text(g_fTitle, 11, RX, 70, kGold, "VERSION");
        Card(RX, 92, RX + RW, 160);
        Spinner(RX + 30, 126, 11, t);
        Text(g_fTitle, 12, RX + 56, 106, kCream, "CHECKING FOR UPDATES");
        snprintf(line, sizeof line, "You have %s", mine.c_str());
        Text(g_fText, 15, RX + 56, 126, kDim, line);
        Text(g_fText, 14, RX, 176, kDim, "You can already play: the check keeps going in the background and takes a second or two.", 0, RW);
    } else if (st == S_UPTODATE || st == S_OFFLINE) {
        Text(g_fTitle, 11, RX, 70, kGold, "VERSION");
        Card(RX, 92, RX + RW, 160);
        if (st == S_UPTODATE) {
            Tick(RX + 18, 114, 22);
            Text(g_fTitle, 12, RX + 56, 106, kCream, "UP TO DATE");
            snprintf(line, sizeof line, "%s is the newest version", mine.c_str());
        } else {
            Warn(RX + 16, 112, 22);
            Text(g_fTitle, 12, RX + 56, 106, kCream, "COULD NOT CHECK");
            snprintf(line, sizeof line, "You have %s (GitHub not reachable)", mine.c_str());
        }
        Text(g_fText, 15, RX + 56, 126, kDim, line);
        Text(g_fText, 14, RX, 176, kDim, "Press Play (or Enter, or A on a controller). This window closes once the game is up.", 0, RW);
    } else if (st == S_AVAILABLE) {
        primaryIsUpdate = canUpdate;
        Text(g_fTitle, 11, RX, 56, kGold, "UPDATE AVAILABLE");
        float notesH = 0;
        for (auto& n : rel.notes) notesH += TextH(g_fText, 14, n.c_str(), RW - 50) + 2;
        float y1 = 78 + 62 + notesH + (rel.notes.empty() ? 0 : 22) + 26;
        Card(RX, 78, RX + RW, y1);
        Text(g_fText, 15, RX + 18, 90, kFaint, mine.c_str());
        float w = TextW(g_fText, 15, mine.c_str());
        {   // arrow, drawn (not every font has the character)
            float ax = RX + 28 + w, ay = 100;
            dl->AddLine(P(ax, ay), P(ax + 13, ay), kRed, 2 * g_s);
            dl->AddTriangleFilled(P(ax + 16, ay), P(ax + 10, ay - 4), P(ax + 10, ay + 4), kRed);
        }
        std::string nv = rel.pretty;
        for (auto& c : nv) c = (char)toupper((unsigned char)c);
        Text(g_fTitle, 14, RX + 48 + w, 91, kGold, nv.c_str());
        dl->AddLine(P(RX + 18, 116), P(RX + RW - 18, 116), C(215, 38, 32, 0.6f), 1 * g_s);
        float y = 124;
        if (!rel.notes.empty()) {
            Text(g_fTitle, 9, RX + 18, y, kDim, "WHAT'S NEW");
            y += 18;
            for (auto& n : rel.notes) {
                dl->AddCircleFilled(P(RX + 24, y + 9), 2 * g_s, kCream);
                Text(g_fText, 14, RX + 32, y, kCream, n.c_str(), 0, RW - 50);
                y += TextH(g_fText, 14, n.c_str(), RW - 50) + 2;
            }
        }
        bool link = Hover(RX + 18, y + 2, RX + 160, y + 20);
        Text(g_fText, 13, RX + 18, y + 2, link ? kCream : kGold, "All changes on GitHub");
        if (link && ImGui::IsMouseReleased(0))
            ShellExecuteW(nullptr, L"open", (L"https://github.com/" + g_repo + L"/releases").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (canUpdate) {
            snprintf(line, sizeof line, "%.1f MB. Your settings and My setup are kept.", rel.size / 1048576.0);
            Text(g_fText, 13, RX, y1 + 8, kFaint, line);
        } else Text(g_fText, 13, RX, y1 + 8, kFaint, "Get it from the GitHub page (this release can't be installed from here).", 0, RW);
    } else if (st == S_UPDATING) {
        snprintf(line, sizeof line, "UPDATING TO %s", rel.pretty.c_str());
        for (char* c = line; *c; c++) *c = (char)toupper((unsigned char)*c);
        Text(g_fTitle, 11, RX, 70, kGold, line);
        Card(RX, 92, RX + RW, 252);
        static const char* steps[3] = {"Download from GitHub", "Check the file is complete and genuine", "Put the new files in place"};
        int s = g_step.load();
        for (int i = 0; i < 3; i++) {
            float y = 108 + i * 26;
            if (i < s) Tick(RX + 18, y + 2, 15);
            else if (i == s) Spinner(RX + 25, y + 10, 7, t);
            else dl->AddCircle(P(RX + 25, y + 10), 7 * g_s, C(200, 154, 140, 0.5f), 16, 2 * g_s);
            Text(g_fText, 15, RX + 44, y, i <= s ? kCream : kFaint, steps[i]);
        }
        uint64_t got = g_got.load(), tot = g_total.load();
        float f = s >= 1 ? 1.0f : tot ? (float)got / (float)tot : 0.0f;
        if (f > 1) f = 1;
        dl->AddRectFilled(P(RX + 18, 192), P(RX + RW - 18, 202), C(8, 1, 2, 0.8f));
        dl->AddRectFilledMultiColor(P(RX + 18, 192), P(RX + 18 + (RW - 36) * f, 202), kRed, kGold, kGold, kRed);
        dl->AddRect(P(RX + 18, 192), P(RX + RW - 18, 202), C(255, 206, 96, 0.4f), 0, 0, 1 * g_s);
        if (s == 0) snprintf(line, sizeof line, "%.1f of %.1f MB", got / 1048576.0, tot / 1048576.0);
        else snprintf(line, sizeof line, s == 1 ? "Downloaded %.1f MB, checking it" : "Downloaded %.1f MB, installing", got / 1048576.0);
        Text(g_fText, 13, RX + 18, 212, kDim, line);
        Text(g_fText, 13, RX, 262, kFaint, "Takes a few seconds. If anything fails, nothing is changed.", 0, RW);
    } else if (st == S_UPDATED) {
        Text(g_fTitle, 11, RX, 70, kGold, "VERSION");
        Card(RX, 92, RX + RW, 160, true);
        Tick(RX + 18, 114, 22);
        snprintf(line, sizeof line, "UPDATED TO %s", rel.pretty.c_str());
        for (char* c = line; *c; c++) *c = (char)toupper((unsigned char)*c);
        Text(g_fTitle, 12, RX + 56, 106, kCream, line);
        Text(g_fText, 15, RX + 56, 126, kDim, "Settings and My setup kept");
    } else if (st == S_FAILED) {
        Text(g_fTitle, 11, RX, 70, kGold, "UPDATE");
        snprintf(line, sizeof line, "%s Nothing was changed: you still have %s.", fail.c_str(), mine.c_str());
        float h = TextH(g_fText, 15, line, RW - 70);
        Card(RX, 92, RX + RW, 92 + h + 48);
        Warn(RX + 16, 106, 22);
        Text(g_fTitle, 12, RX + 50, 104, kCream, "THE UPDATE DID NOT WORK");
        Text(g_fText, 15, RX + 50, 124, kDim, line, 0, RW - 70);
        float by = 92 + h + 60;
        if (DarkButton(RX, by, RX + 120, by + 38, "TRY AGAIN", nullptr, kGold) || other) {
            g_state = S_UPDATING;
            CreateThread(nullptr, 0, UpdateThread, nullptr, 0, nullptr);
        }
        bool link = Hover(RX + 136, by + 10, RX + RW, by + 30);
        Text(g_fText, 14, RX + 136, by + 10, link ? kCream : kGold, "Download it from GitHub instead");
        if (link && ImGui::IsMouseReleased(0))
            ShellExecuteW(nullptr, L"open", (L"https://github.com/" + g_repo + L"/releases").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // help link + buttons
    if (primaryIsUpdate) {
        if (DarkButton(BX0 - 150, BY0, BX0 - 16, BY1, "PLAY WITHOUT", "UPDATING") || other) Play(false);
        else if (GoldButton(BX0, BY0, BX1, BY1, "UPDATE", true, t) || enter) {
            g_state = S_UPDATING;
            CreateThread(nullptr, 0, UpdateThread, nullptr, 0, nullptr);
        }
    } else {
        bool hv = Hover(BX0 - 70, BY0 + 14, BX0 - 16, BY0 + 42);
        Text(g_fTitle, 10, BX0 - 24, BY0 + 21, hv ? kCream : kDim, "HELP", 2);
        if (hv && ImGui::IsMouseReleased(0))
            ShellExecuteW(nullptr, L"open", (L"https://github.com/" + g_repo + L"#readme").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if ((GoldButton(BX0, BY0, BX1, BY1, "PLAY", playOk, t) || (enter && playOk))) Play(false);
    }
    if (esc && !busy) g_quit = true;
}

static bool MakeDevice() {
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) return false;
    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_pp.EnableAutoDepthStencil = FALSE;
    g_pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (FAILED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_pp, &g_dev)) &&
        FAILED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, g_hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_pp, &g_dev)))
        return false;
    return true;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_ourDir = self;
    g_ourDir = g_ourDir.substr(0, g_ourDir.find_last_of(L'\\') + 1);
    g_log = _wfopen((g_ourDir + L"launcher_log.txt").c_str(), L"w, ccs=UTF-8");
    Say(L"Arcana Blood +R launcher %hs (build %d)", MOD_VERSION, MOD_BUILD);
    DeleteFileW((g_ourDir + L"arcana_launcher.exe.old").c_str());   // left from the last update
    InitializeCriticalSection(&upd::g_lock);
    upd::g_dir = g_ourDir;
    wchar_t repo[128];
    GetPrivateProfileStringW(L"update", L"repo", L"robinneefs/million-arthur-arcana-blood-plus-r", repo, 128, (g_ourDir + L"rollback.ini").c_str());
    upd::g_repo = repo;
    bool quick = false;
    for (int i = 1; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"--play")) quick = true;
        else if (!_wcsicmp(argv[i], L"--updated")) {    // the v0.1.0 updater passes no version
            char a[64] = "";
            if (i + 1 < argc && argv[i + 1][0] != L'-') WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, a, sizeof a, nullptr, nullptr);
            g_updatedTo = a[0] ? a : upd::Pretty(MOD_VERSION);
        }
    }
    g_gameDir = game::FindGameDir();
    if (!g_gameDir.empty()) ShortPath(g_gameDir);
    else g_gameDirShown = "Game not found in Steam";
    Say(L"Game folder: %ls", g_gameDir.empty() ? L"(not found)" : g_gameDir.c_str());

    SetProcessDPIAware();
    HDC sdc = GetDC(nullptr);
    g_s = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, sdc);
    if (g_s < 1) g_s = 1;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{sizeof wc, CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"ArcanaBloodPlusRLauncher", nullptr};
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    int w = (int)(WW * g_s), h = (int)(WH * g_s);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Arcana Blood +R", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                             wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd || !MakeDevice()) {
        Say(L"No window or Direct3D: starting the game straight away.");
        quick = true;
    }
    if (quick) {   // no window: just put the DLL in place and start
        g_gameDir = game::FindGameDir();
        bool ok = false;
        std::string why = "Steam doesn't have the game installed.";
        if (!g_gameDir.empty()) {
            game::MoveFrom119(g_gameDir, g_ourDir);
            int p = game::PrepareGameFolder(g_gameDir, g_ourDir, false);
            if (p == game::P_OK) ok = game::Start(g_gameDir, g_ourDir, why);
            else why = p == game::P_FOREIGN ? "Another mod's dinput8.dll is in the game folder: open the launcher without --play to replace it."
                                            : "Could not put dinput8.dll into the game folder (see launcher_log.txt).";
        }
        if (!ok) MessageBoxA(nullptr, why.c_str(), "Arcana Blood +R", MB_OK | MB_ICONWARNING);
        if (g_log) fclose(g_log);
        return ok ? 0 : 1;
    }
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2190, 0x2193, 0x25CF, 0x25CF, 0};
    std::wstring fd = g_ourDir + L"fonts\\";
    ImFontConfig c;
    c.OversampleH = 3;
    c.OversampleV = 1;
    // the sizes the screens use (after Bump), each made at its real pixel size
    static const int titlePx[] = {11, 12, 13, 14, 16, 18, 20, 26};
    static const int textPx[] = {15, 16, 17};
    auto load = [&](Family& fam, const int* px, int n) {
        std::wstring path = fd + W8(fam.file);
        if (!game::FileExists(path)) return;
        char p8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, p8, sizeof p8, nullptr, nullptr);
        for (int i = 0; i < n; i++) {
            int sz = (int)(px[i] * g_s + 0.5f);
            if (ImFont* f = io.Fonts->AddFontFromFileTTF(p8, (float)sz, &c, ranges)) fam.sizes.push_back({sz, f});
        }
    };
    load(g_famTitle, titlePx, 8);
    load(g_famText, textPx, 3);
    load(g_famBold, textPx, 3);
    if (!g_famTitle.sizes.empty()) g_fTitle = g_famTitle.sizes.back().f;
    if (!g_famText.sizes.empty()) g_fText = g_famText.sizes.back().f;
    if (!g_famBold.sizes.empty()) g_fBold = g_famBold.sizes.back().f;
    ImFont* def = io.Fonts->AddFontDefault();
    if (!g_fTitle) g_fTitle = def;
    if (!g_fText) g_fText = def;
    if (!g_fBold) g_fBold = g_fText;
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX9_Init(g_dev);
    if (!LoadLogo(g_ourDir + L"rollback_logo.png")) Say(L"rollback_logo.png could not be read");

    CreateThread(nullptr, 0, upd::CheckThread, nullptr, 0, nullptr);
    LARGE_INTEGER fq, t0, tp;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    tp = t0;
    MSG msg{};
    while (!g_quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_quit = true;
        }
        if (g_quit) break;
        // the updater replaced this launcher: hand over to the new one
        if (upd::g_state == upd::S_UPDATED) {
            EnterCriticalSection(&upd::g_lock);
            std::wstring next = upd::g_restart;
            std::string ver = upd::g_rel.pretty;
            LeaveCriticalSection(&upd::g_lock);
            if (!next.empty()) {
                std::wstring cmd = L"\"" + next + L"\" --updated \"" + upd::W(ver) + L"\"";
                STARTUPINFOW si{sizeof si};
                PROCESS_INFORMATION pi{};
                if (CreateProcessW(next.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_ourDir.c_str(), &si, &pi)) {
                    Say(L"Started the new launcher.");
                    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                    break;
                }
                Say(L"Could not start the new launcher (error %lu); this one goes on.", GetLastError());
                EnterCriticalSection(&upd::g_lock);
                upd::g_restart.clear();
                LeaveCriticalSection(&upd::g_lock);
            }
        }
        QueryPerformanceCounter(&tp);
        static LARGE_INTEGER last = t0;
        float dt = (float)(tp.QuadPart - last.QuadPart) / fq.QuadPart;
        last = tp;
        float t = (float)(tp.QuadPart - t0.QuadPart) / fq.QuadPart;
        if (IsIconic(g_hwnd)) { Sleep(30); continue; }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Frame(t, dt > 0.1f ? 0.1f : dt);
        ImGui::EndFrame();
        g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(8, 1, 2), 1.0f, 0);
        if (g_dev->BeginScene() >= 0) {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_dev->EndScene();
        }
        HRESULT r = g_dev->Present(nullptr, nullptr, nullptr, nullptr);
        if (r == D3DERR_DEVICELOST && g_dev->TestCooperativeLevel() == D3DERR_DEVICENOTRESET) {
            ImGui_ImplDX9_InvalidateDeviceObjects();
            if (g_dev->Reset(&g_pp) >= 0) ImGui_ImplDX9_CreateDeviceObjects();
        }
    }
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_logo) g_logo->Release();
    if (g_dev) g_dev->Release();
    if (g_d3d) g_d3d->Release();
    DestroyWindow(g_hwnd);
    Say(L"Launcher closed.");
    if (g_log) fclose(g_log);
    return 0;
}
