// ===========================================================================
// Build 90: the game's own 2D pictures for our menus (included into mod.cpp
// before overlay.inc)
// ===========================================================================
// Nothing of the game is shipped with the mod: every picture is read from the
// player's own game folder (flashrom\Itd\Eng\<name>.itd + .pil) while the game runs.
//
// File layout (worked out 2026-11-16 from the game's loader at 0x53A560 and its
// unpacker at 0x637F20):
//   .pil  +0x0C number of sheets, then 0x24 bytes per sheet from +0x10:
//         +0x04 width, +0x06 height (u16), +0x14 format (0x29 = 8-bit colour
//         numbers), +0x1C offset and +0x20 size of the packed data.
//         Packed data, in 16-bit words: a flag word gives the next 16 steps
//         (highest bit first). Flag 0: copy the next word. Flag 1: the next word
//         says "repeat (w >> 12) + 2 words from (w & 0xFFF) + 1 words back".
//   .itd  +0x34: (100, number of colour tables), then 256 x 4 bytes (B, G, R, A)
//         per table. +0x18: offset of the sprite list: (100, count), then 44
//         bytes per sprite: format, sheet, colour table, x, y, width, height,
//         centre x, centre y, ...
// Each sprite has its own colour table, so sprites are turned into normal
// 32-bit pictures one by one (only the ones a screen asks for).
//
// Threads: files are read and unpacked on a helper thread; pictures are made on
// the drawing thread, through MakeTex/FreeTex (Direct3D 9 in the game, plain
// memory in the layout-check program).
// ===========================================================================
#include "imgui.h"
namespace art {

struct Sprite { int sheet, pal, x, y, w, h; };
struct File {
    char name[48] = "";
    int state = 0;                          // 0 not asked, 1 loading, 2 ready, 3 failed
    char err[160] = "";
    std::vector<std::vector<uint8_t>> pix;  // per sheet: one byte per pixel
    std::vector<int> sw, sh;
    std::vector<uint8_t> pals;              // 1024 bytes per colour table
    std::vector<Sprite> spr;
    uint32_t lastUse = 0;
};
struct Tex { void* tex = nullptr; int w = 0, h = 0; uint32_t lastUse = 0; };

// set by the drawing side
static void* (*MakeTex)(const uint32_t* bgra, int w, int h) = nullptr;
static void (*FreeTex)(void* t) = nullptr;

static CRITICAL_SECTION g_lock;
static bool g_lockInit = false;
static std::vector<File*> g_files;
static std::map<uint64_t, Tex> g_tex;       // (file index << 32 | sprite) -> picture
static size_t g_texBytes = 0;
static uint32_t g_frame = 0;
static HANDLE g_wake = nullptr;
static const size_t TEX_BUDGET = 96u << 20; // pictures kept at most (bytes); older ones go first

static bool Unpack(const uint8_t* src, size_t srcBytes, std::vector<uint8_t>& out, size_t expect) {
    size_t n = srcBytes / 2, i = 0, o = 0;
    out.assign(expect + 64, 0);
    uint16_t* dst = reinterpret_cast<uint16_t*>(out.data());
    size_t cap = (expect + 64) / 2;
    const uint16_t* w = reinterpret_cast<const uint16_t*>(src);
    uint32_t flags = 0;
    int left = 0;
    while (i < n) {
        if (left == 0) { flags = w[i++]; left = 16; if (i >= n) break; }
        bool rep = flags & 0x8000;
        flags = (flags << 1) & 0xFFFF;
        left--;
        uint16_t t = w[i++];
        if (!rep) { if (o >= cap) return false; dst[o++] = t; continue; }
        size_t back = (t & 0xFFF) + 1, len = (t >> 12) + 2;
        if (back > o || o + len > cap) return false;
        for (size_t k = 0; k < len; k++, o++) dst[o] = dst[o - back];
    }
    out.resize(o * 2);
    return out.size() >= expect;
}

static bool ReadAll(const wchar_t* path, std::vector<uint8_t>& d) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, nullptr), got = 0;
    bool ok = sz != INVALID_FILE_SIZE && sz < (64u << 20);
    if (ok) { d.resize(sz); ok = ReadFile(h, d.data(), sz, &got, nullptr) && got == sz; }
    CloseHandle(h);
    return ok;
}
static const wchar_t* GameDir() {
    static wchar_t dir[MAX_PATH] = L"";
    if (!dir[0]) {
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        wchar_t* s = wcsrchr(dir, L'\\');
        if (s) s[1] = 0;
    }
    return dir;
}
template <class T> static bool Rd(const std::vector<uint8_t>& d, size_t at, T& v) {
    if (at + sizeof(T) > d.size()) return false;
    memcpy(&v, d.data() + at, sizeof(T));
    return true;
}

static void LoadFile(File* f) {
    wchar_t pi[MAX_PATH], pp[MAX_PATH];
    _snwprintf(pi, MAX_PATH, L"%lsflashrom\\Itd\\Eng\\%hs.itd", GameDir(), f->name);
    _snwprintf(pp, MAX_PATH, L"%lsflashrom\\Itd\\Eng\\%hs.pil", GameDir(), f->name);
    std::vector<uint8_t> itd, pil;
    if (!ReadAll(pi, itd) || !ReadAll(pp, pil)) { snprintf(f->err, sizeof f->err, "could not read %s.itd / .pil in the game folder", f->name); f->state = 3; return; }
    uint32_t npal = 0, sprOff = 0, nspr = 0, nsheet = 0;
    if (!Rd(itd, 0x38, npal) || npal > 4096 || 0x3C + (size_t)npal * 1024 > itd.size() || !Rd(itd, 0x18, sprOff) ||
        !Rd(itd, sprOff + 4, nspr) || nspr > 20000 || sprOff + 8 + (size_t)nspr * 44 > itd.size() || !Rd(pil, 0x0C, nsheet) || nsheet > 64) {
        snprintf(f->err, sizeof f->err, "%s: unexpected file layout", f->name);
        f->state = 3;
        return;
    }
    std::vector<uint8_t> pals(itd.begin() + 0x3C, itd.begin() + 0x3C + npal * 1024);
    std::vector<Sprite> spr;
    for (uint32_t i = 0; i < nspr; i++) {
        int32_t v[7];
        memcpy(v, itd.data() + sprOff + 8 + i * 44, sizeof v);
        spr.push_back(Sprite{v[1], v[2], v[3], v[4], v[5], v[6]});
    }
    std::vector<std::vector<uint8_t>> pix(nsheet);
    std::vector<int> sw(nsheet), sh(nsheet);
    for (uint32_t s = 0; s < nsheet; s++) {
        size_t e = 0x10 + s * 0x24;
        uint16_t w = 0, h = 0;
        uint32_t fmt = 0, off = 0, sz = 0;
        if (!Rd(pil, e + 4, w) || !Rd(pil, e + 6, h) || !Rd(pil, e + 0x14, fmt) || !Rd(pil, e + 0x1C, off) || !Rd(pil, e + 0x20, sz) ||
            (size_t)off + sz > pil.size() || fmt != 0x29 || !w || !h) {
            snprintf(f->err, sizeof f->err, "%s: sheet %u has an unknown kind (format %X)", f->name, s, fmt);
            continue;                       // sprites on it simply stay empty
        }
        if (!Unpack(pil.data() + off, sz, pix[s], (size_t)w * h)) { snprintf(f->err, sizeof f->err, "%s: sheet %u did not unpack", f->name, s); pix[s].clear(); continue; }
        sw[s] = w; sh[s] = h;
    }
    EnterCriticalSection(&g_lock);
    f->pals.swap(pals); f->spr.swap(spr); f->pix.swap(pix); f->sw.swap(sw); f->sh.swap(sh);
    f->state = 2;
    LeaveCriticalSection(&g_lock);
    Log("art: %s loaded (%u sheets, %u sprites, %u colour tables)%s%s", f->name, nsheet, nspr, npal, f->err[0] ? ", note: " : "", f->err);
}

// ---------------------------------------------------------------------------
// Build 102: the characters' fight sprites and the movies' first pictures
// (for the My setup editor). Worked out 2026-11-16 from the files:
//   act\chara\chara_XX.pil  +0x0C number of pictures; a table of 0x24 bytes per
//         picture starts at T, the first offset with entry[7] == T + count*0x24.
//         Entry dwords: [1] = height << 16 | width, [7] offset, [8] packed size
//         (same packing as above), one colour number per pixel. Picture 0 is
//         the first frame of the standing (idle) pose for every character.
//   act\chara\chara_XX_00.pal  16 bytes, then 64 colour tables of 256 x B,G,R,A;
//         tables 0-20 are the character's 21 colours.
//   Movie\<name>.pmt  +0 width, +4 height, +0x18 offset, +0x1C packed size of the
//         first picture (B,G,R,A per pixel, same packing).
// The files are big (chara_XX.pil is 40-150 MB), so only the parts needed are read.
// ---------------------------------------------------------------------------
static const int CHARA_MAX = 16;
struct Chara {
    int state = 0;                  // 0 not asked, 1 loading, 2 ready, 3 failed
    std::vector<uint8_t> idx;       // picture 0: one colour number per pixel
    int w = 0, h = 0;
    std::vector<uint8_t> pal;       // 21 colour tables of 1024 bytes
    float head[4] = {0, 0, 1, 1};   // where the head is (texture coordinates)
    uint32_t lastUse = 0;
};
static Chara g_chara[CHARA_MAX];
struct Still { char name[48] = ""; int state = 0; std::vector<uint32_t> px; int w = 0, h = 0; };
static std::vector<Still*> g_stills;

static bool ReadRange(const wchar_t* path, uint32_t off, uint32_t n, std::vector<uint8_t>& d) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, nullptr), got = 0;
    bool ok = sz != INVALID_FILE_SIZE && off < sz;
    if (ok) {
        if (n > sz - off) n = sz - off;
        d.resize(n);
        ok = SetFilePointer(h, (LONG)off, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER && ReadFile(h, d.data(), n, &got, nullptr) && got == n;
    }
    CloseHandle(h);
    return ok;
}
// a box around the head: the top of the figure, centred on where its pixels are
// Build 120f: where each character's face is in its standing picture (pixels: x from the left,
// y from the top of the figure), measured by hand from the game's pictures; the colour squares
// are centred on it. Characters not listed use the guess below.
static const short FACE_XY[14][2] = {{352, 115}, {494, 117}, {213, 123}, {475, 90}, {318, 122}, {305, 110}, {237, 65},
                                     {598, 69}, {243, 182}, {228, 87}, {340, 80}, {228, 150}, {300, 60}, {218, 72}};
template <class F> static void HeadBox(int w, int h, F solid, float out[4], int id = -1) {
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (solid(x, y)) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    if (x1 < 0) { out[0] = out[1] = 0; out[2] = out[3] = 1; return; }
    if (id >= 0 && id < 14 && FACE_XY[id][0] < w) {
        const float bh = 150, bw = bh * 56.0f / 64.0f;
        float l = FACE_XY[id][0] - bw / 2, t = y0 + FACE_XY[id][1] - bh / 2;
        if (l < 0) l = 0;
        if (l + bw > w) l = w - bw;
        if (t < 0) t = 0;
        if (t + bh > h) t = h - bh;
        out[0] = l / w; out[1] = t / h; out[2] = (l + bw) / w; out[3] = (t + bh) / h;
        return;
    }
    float bh = (float)(y1 - y0 + 1), hb = bh * 0.24f;
    double sx = 0; int n = 0;
    for (int y = y0; y < y0 + (int)hb && y < h; y++)
        for (int x = x0; x <= x1; x++) if (solid(x, y)) { sx += x; n++; }
    float cx = n ? (float)(sx / n) : (x0 + x1) * 0.5f, bw = hb * 56.0f / 64.0f;
    float l = cx - bw / 2;
    if (l < 0) l = 0;
    if (l + bw > w) l = w - bw;
    out[0] = l / w; out[1] = (float)y0 / h; out[2] = (l + bw) / w; out[3] = (y0 + hb) / h;
}
static void LoadChara(int id) {
    Chara& c = g_chara[id];
    wchar_t pp[MAX_PATH], pl[MAX_PATH];
    _snwprintf(pp, MAX_PATH, L"%lsflashrom\\act\\chara\\chara_%02X.pil", GameDir(), id);
    _snwprintf(pl, MAX_PATH, L"%lsflashrom\\act\\chara\\chara_%02X_00.pal", GameDir(), id);
    std::vector<uint8_t> hd, pk, pal, idx;
    uint32_t nt = 0, T = 0, e[9] = {};
    bool ok = ReadRange(pp, 0, 0x9000, hd) && Rd(hd, 0x0C, nt) && nt > 0 && nt < 200000;
    if (ok) {
        ok = false;
        for (uint32_t t = 0x10; t + 0x24 <= hd.size() && t < 0x8000; t += 4) {
            uint32_t v = 0;
            if (Rd(hd, t + 7 * 4, v) && v == t + nt * 0x24) { T = t; ok = true; break; }
        }
    }
    if (ok) { memcpy(e, hd.data() + T, sizeof e); }
    int w = e[1] & 0xFFFF, h = e[1] >> 16;
    ok = ok && w > 0 && h > 0 && w <= 2048 && h <= 2048 && e[8] > 0 && e[8] < (32u << 20) && ReadRange(pp, e[7], e[8], pk) && pk.size() == e[8] &&
         Unpack(pk.data(), pk.size(), idx, (size_t)w * h);
    bool palOk = ok && ReadAll(pl, pal) && pal.size() >= 16 + 21 * 1024;
    EnterCriticalSection(&g_lock);
    if (!palOk) {
        c.state = 3;
        LeaveCriticalSection(&g_lock);
        Log("art: fight sprite of character %02X could not be read (%s)", id, ok ? "colour file" : "picture file");
        return;
    }
    c.pal.assign(pal.begin() + 16, pal.begin() + 16 + 21 * 1024);
    c.idx.swap(idx);
    c.w = w; c.h = h;
    const uint8_t* p0 = c.pal.data();
    const uint8_t* ix = c.idx.data();
    HeadBox(w, h, [&](int x, int y) { return p0[ix[(size_t)y * w + x] * 4 + 3] > 0x20; }, c.head, id);
    c.state = 2;
    LeaveCriticalSection(&g_lock);
    Log("art: fight sprite of character %02X loaded (%dx%d)", id, w, h);
}
static void LoadStill(Still* t) {
    wchar_t pm[MAX_PATH];
    _snwprintf(pm, MAX_PATH, L"%lsflashrom\\Movie\\%hs.pmt", GameDir(), t->name);
    std::vector<uint8_t> hd, pk, out;
    uint32_t w = 0, h = 0, off = 0, sz = 0;
    bool ok = ReadRange(pm, 0, 0x24, hd) && Rd(hd, 0, w) && Rd(hd, 4, h) && Rd(hd, 0x18, off) && Rd(hd, 0x1C, sz) && w && h && w <= 4096 && h <= 4096 &&
              sz < (32u << 20) && ReadRange(pm, off, sz, pk) && pk.size() == sz && Unpack(pk.data(), pk.size(), out, (size_t)w * h * 4);
    EnterCriticalSection(&g_lock);
    if (ok) {
        t->px.resize((size_t)w * h);
        memcpy(t->px.data(), out.data(), (size_t)w * h * 4);
        t->w = w; t->h = h; t->state = 2;
    } else t->state = 3;
    LeaveCriticalSection(&g_lock);
    if (!ok) Log("art: movie picture %s could not be read", t->name);
}

static DWORD WINAPI Worker(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_wake, 1000);
        for (;;) {
            File* todo = nullptr;
            EnterCriticalSection(&g_lock);
            for (File* f : g_files) if (f->state == 1 && !todo) todo = f;
            int chara = -1; Still* still = nullptr;
            if (!todo) for (int i = 0; i < CHARA_MAX && chara < 0; i++) if (g_chara[i].state == 1) chara = i;
            if (!todo && chara < 0) for (Still* t : g_stills) if (t->state == 1 && !still) still = t;
            LeaveCriticalSection(&g_lock);
            if (todo) LoadFile(todo);
            else if (chara >= 0) LoadChara(chara);
            else if (still) LoadStill(still);
            else break;
        }
    }
    return 0;
}
static void Init() {
    if (g_lockInit) return;
    InitializeCriticalSection(&g_lock);
    g_lockInit = true;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

// Index of a file (asks for it to be loaded the first time).
static int FileIndex(const char* name) {
    Init();
    EnterCriticalSection(&g_lock);
    int idx = -1;
    for (size_t i = 0; i < g_files.size(); i++) if (!strcmp(g_files[i]->name, name)) idx = (int)i;
    if (idx < 0) {
        File* f = new File();
        snprintf(f->name, sizeof f->name, "%s", name);
        g_files.push_back(f);
        idx = (int)g_files.size() - 1;
    }
    File* f = g_files[idx];
    f->lastUse = g_frame;
    if (f->state == 0) { f->state = 1; SetEvent(g_wake); }
    LeaveCriticalSection(&g_lock);
    return idx;
}

static void DropTex(std::map<uint64_t, Tex>::iterator it) {
    if (it->second.tex && FreeTex) FreeTex(it->second.tex);
    g_texBytes -= (size_t)it->second.w * it->second.h * 4;
    g_tex.erase(it);
}
// Drawing thread: the picture of one sprite (nullptr while its file is loading).
// size gets the sprite's size in pixels.
static void* Get(const char* name, int sprite, ImVec2* size = nullptr) {
    if (!MakeTex) return nullptr;
    int fi = FileIndex(name);
    uint64_t key = ((uint64_t)fi << 32) | (uint32_t)sprite;
    auto it = g_tex.find(key);
    if (it != g_tex.end()) {
        it->second.lastUse = g_frame;
        if (size) *size = ImVec2((float)it->second.w, (float)it->second.h);
        return it->second.tex;
    }
    EnterCriticalSection(&g_lock);
    File* f = g_files[fi];
    bool ok = f->state == 2 && sprite >= 0 && sprite < (int)f->spr.size();
    Sprite s{};
    if (ok) {
        s = f->spr[sprite];
        ok = s.sheet >= 0 && s.sheet < (int)f->pix.size() && !f->pix[s.sheet].empty() && s.w > 0 && s.h > 0 && s.w <= 4096 && s.h <= 4096 &&
             s.x >= 0 && s.y >= 0 && s.x + s.w <= f->sw[s.sheet] && s.y + s.h <= f->sh[s.sheet] && s.pal >= 0 && (size_t)(s.pal + 1) * 1024 <= f->pals.size();
    }
    if (!ok) { LeaveCriticalSection(&g_lock); return nullptr; }
    // make room
    size_t need = (size_t)s.w * s.h * 4;
    while (g_texBytes + need > TEX_BUDGET && !g_tex.empty()) {
        auto old = g_tex.begin();
        for (auto i = g_tex.begin(); i != g_tex.end(); ++i) if (i->second.lastUse < old->second.lastUse) old = i;
        if (old->second.lastUse == g_frame) break;     // everything is in use this frame
        DropTex(old);
    }
    std::vector<uint32_t> px((size_t)s.w * s.h);
    const uint32_t* pal = reinterpret_cast<const uint32_t*>(f->pals.data() + s.pal * 1024);   // B,G,R,A = A8R8G8B8
    const uint8_t* src = f->pix[s.sheet].data();
    int sw = f->sw[s.sheet];
    for (int y = 0; y < s.h; y++)
        for (int x = 0; x < s.w; x++) px[(size_t)y * s.w + x] = pal[src[(size_t)(s.y + y) * sw + s.x + x]];
    LeaveCriticalSection(&g_lock);
    Tex t;
    t.tex = MakeTex(px.data(), s.w, s.h);
    t.w = s.w; t.h = s.h; t.lastUse = g_frame;
    if (!t.tex) return nullptr;
    g_tex[key] = t;
    g_texBytes += need;
    if (size) *size = ImVec2((float)s.w, (float)s.h);
    return t.tex;
}
// Build 94: a picture file of the mod itself (e.g. the logo), decoded by LoadImageFile
// (Windows' image decoder in the game, raw pixels in the layout-check program). Kept as
// pixels after the first read; the texture lives with the others (FreeAll drops it).
static bool (*LoadImageFile)(const wchar_t* path, std::vector<uint32_t>& bgra, int& w, int& h) = nullptr;
struct Extra { std::wstring path; std::vector<uint32_t> px; int w = 0, h = 0; bool tried = false; };
static std::vector<Extra> g_extra;
static void* GetImage(const wchar_t* path, ImVec2* size = nullptr) {
    if (!MakeTex || !LoadImageFile) return nullptr;
    size_t i = 0;
    while (i < g_extra.size() && g_extra[i].path != path) i++;
    if (i == g_extra.size()) { g_extra.push_back(Extra()); g_extra.back().path = path; }
    Extra& e = g_extra[i];
    uint64_t key = (0xFFFFull << 32) | (uint32_t)i;
    auto it = g_tex.find(key);
    if (it != g_tex.end()) {
        it->second.lastUse = g_frame;
        if (size) *size = ImVec2((float)it->second.w, (float)it->second.h);
        return it->second.tex;
    }
    if (!e.tried) { e.tried = true; if (!LoadImageFile(path, e.px, e.w, e.h)) { e.px.clear(); e.w = e.h = 0; } }
    if (e.px.empty()) return nullptr;
    Tex t;
    t.tex = MakeTex(e.px.data(), e.w, e.h);
    if (!t.tex) return nullptr;
    t.w = e.w; t.h = e.h; t.lastUse = g_frame;
    g_tex[key] = t;
    g_texBytes += (size_t)e.w * e.h * 4;
    if (size) *size = ImVec2((float)e.w, (float)e.h);
    return t.tex;
}
// Drawing thread: the fight sprite of a character in one of its 21 colours
// (nullptr while loading). head gets where its head is, in texture coordinates.
static void* GetChara(int id, int colour, ImVec2* size = nullptr, float* head = nullptr) {
    if (!MakeTex || id < 0 || id >= CHARA_MAX || colour < 0 || colour > 20) return nullptr;
    Init();
    Chara& c = g_chara[id];
    uint64_t key = (0xFFFEull << 32) | ((uint32_t)id << 8) | (uint32_t)colour;
    EnterCriticalSection(&g_lock);
    int st = c.state;
    if (st == 0) { c.state = 1; SetEvent(g_wake); }
    c.lastUse = g_frame;
    if (st == 2 && head) memcpy(head, c.head, sizeof c.head);
    LeaveCriticalSection(&g_lock);
    if (st != 2) return nullptr;
    auto it = g_tex.find(key);
    if (it != g_tex.end()) {
        it->second.lastUse = g_frame;
        if (size) *size = ImVec2((float)it->second.w, (float)it->second.h);
        return it->second.tex;
    }
    size_t need = (size_t)c.w * c.h * 4;
    while (g_texBytes + need > TEX_BUDGET && !g_tex.empty()) {
        auto old = g_tex.begin();
        for (auto i = g_tex.begin(); i != g_tex.end(); ++i) if (i->second.lastUse < old->second.lastUse) old = i;
        if (old->second.lastUse == g_frame) break;
        DropTex(old);
    }
    std::vector<uint32_t> px((size_t)c.w * c.h);
    const uint32_t* pal = reinterpret_cast<const uint32_t*>(c.pal.data() + colour * 1024);
    for (size_t i = 0; i < px.size(); i++) px[i] = pal[c.idx[i]];
    Tex t;
    t.tex = MakeTex(px.data(), c.w, c.h);
    if (!t.tex) return nullptr;
    t.w = c.w; t.h = c.h; t.lastUse = g_frame;
    g_tex[key] = t;
    g_texBytes += need;
    if (size) *size = ImVec2((float)c.w, (float)c.h);
    return t.tex;
}
// Drawing thread: the first picture of a movie (Movie\<name>.pmt), e.g. an assist's
// cut-in art. The pixels are let go once the picture is made; read again if needed.
static void* GetStill(const char* name, ImVec2* size = nullptr) {
    if (!MakeTex) return nullptr;
    Init();
    size_t i = 0;
    EnterCriticalSection(&g_lock);
    while (i < g_stills.size() && strcmp(g_stills[i]->name, name)) i++;
    if (i == g_stills.size()) { Still* t = new Still(); snprintf(t->name, sizeof t->name, "%s", name); g_stills.push_back(t); }
    Still* t = g_stills[i];
    LeaveCriticalSection(&g_lock);
    uint64_t key = (0xFFFDull << 32) | (uint32_t)i;
    auto it = g_tex.find(key);
    if (it != g_tex.end()) {
        it->second.lastUse = g_frame;
        if (size) *size = ImVec2((float)it->second.w, (float)it->second.h);
        return it->second.tex;
    }
    EnterCriticalSection(&g_lock);
    int st = t->state;
    if (st == 0 || st == 4) { t->state = 1; SetEvent(g_wake); }
    LeaveCriticalSection(&g_lock);
    if (st != 2) return nullptr;
    size_t need = (size_t)t->w * t->h * 4;
    while (g_texBytes + need > TEX_BUDGET && !g_tex.empty()) {
        auto old = g_tex.begin();
        for (auto j = g_tex.begin(); j != g_tex.end(); ++j) if (j->second.lastUse < old->second.lastUse) old = j;
        if (old->second.lastUse == g_frame) break;
        DropTex(old);
    }
    Tex x;
    x.tex = MakeTex(t->px.data(), t->w, t->h);
    if (!x.tex) return nullptr;
    x.w = t->w; x.h = t->h; x.lastUse = g_frame;
    g_tex[key] = x;
    g_texBytes += need;
    EnterCriticalSection(&g_lock);
    t->px.clear(); t->px.shrink_to_fit(); t->state = 4;     // 4 = only the picture is kept
    LeaveCriticalSection(&g_lock);
    if (size) *size = ImVec2((float)x.w, (float)x.h);
    return x.tex;
}
static void NewFrame() { g_frame++; }
// Let go of all pictures (drawing thread), and of the unpacked files not used for a while.
static void FreeAll(bool files) {
    while (!g_tex.empty()) DropTex(g_tex.begin());
    if (!files || !g_lockInit) return;
    EnterCriticalSection(&g_lock);
    for (File* f : g_files)
        if (f->state == 2) { f->pix.clear(); f->pix.shrink_to_fit(); f->spr.clear(); f->pals.clear(); f->state = 0; }
    LeaveCriticalSection(&g_lock);
}
// Keep at most `keep` unpacked files in memory (they are 4-12 MB each).
static void TrimFiles(int keep) {
    if (!g_lockInit) return;
    EnterCriticalSection(&g_lock);
    for (;;) {
        int ready = 0; File* oldest = nullptr;
        for (File* f : g_files) if (f->state == 2) { ready++; if (!oldest || f->lastUse < oldest->lastUse) oldest = f; }
        if (ready <= keep || !oldest || oldest->lastUse + 2 >= g_frame) break;
        oldest->pix.clear(); oldest->pix.shrink_to_fit(); oldest->state = 0;
        // its pictures stay until the budget pushes them out
    }
    LeaveCriticalSection(&g_lock);
}

} // namespace art
