// ---------------------------------------------------------------------------
// Build 104: Settings > Logs > Pack logs. Puts the mod's logs and settings files (the
// .txt / .log / .ini files next to the mod) into one zip on the Desktop, with Steam IDs,
// the Steam name, the PC name and the Windows user name hidden. Big logs: only the
// last 4 MB. Runs on its own thread; nothing is sent anywhere.
// Build 121: compressed, the last 5 runs of each log, crash reports (crash_*.txt) and memory
// snapshots (desync_*.bin); kept under 8 MB by leaving out the oldest logs first.
// ---------------------------------------------------------------------------
#ifndef UI_HARNESS
#include "miniz.h"                 // build 121: deflate for the zip (third_party/miniz, MIT)
#endif
namespace logpack {

static std::atomic<int> g_state{0};        // 0 idle, 1 packing, 2 done, 3 failed
static char g_msg[300] = "";
static wchar_t g_zip[MAX_PATH] = L"";

#ifndef UI_HARNESS
static uint32_t Crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
    static uint32_t t[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t r = i;
            for (int k = 0; k < 8; k++) r = (r >> 1) ^ (0xEDB88320u & (0u - (r & 1)));
            t[i] = r;
        }
        init = true;
    }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void ReplaceAll(std::string& s, const std::string& what, const char* with) {
    if (what.size() < 2) return;
    for (size_t at = 0; (at = s.find(what, at)) != std::string::npos; at += strlen(with)) s.replace(at, what.size(), with);
}
// 17-digit Steam IDs (7656119...) -> 7656119##########
static void MaskSteamIds(std::string& s) {
    for (size_t i = 0; i + 17 <= s.size(); i++) {
        if (s.compare(i, 7, "7656119") != 0) continue;
        if (i > 0 && isdigit((unsigned char)s[i - 1])) continue;
        size_t k = i + 7;
        while (k < s.size() && isdigit((unsigned char)s[k])) k++;
        if (k - i != 17) continue;
        for (size_t j = i + 7; j < k; j++) s[j] = '#';
        i = k;
    }
}
struct Entry { std::string name, data; uint32_t crc; uint32_t off; uint32_t size; uint16_t method; };
// Build 121: binary files (desync_*.bin memory snapshots) keep their layout: the hidden names
// become the same number of '#' (as plain text and as UTF-16).
static void MaskSameLen(std::string& s, const std::string& what) {
    if (what.size() < 3) return;
    for (int wide = 0; wide < 2; wide++) {
        std::string pat;
        for (char c : what) { pat.push_back(c); if (wide) pat.push_back(0); }
        for (size_t at = 0; (at = s.find(pat, at)) != std::string::npos; at += pat.size())
            for (size_t i = 0; i < pat.size(); i += wide ? 2 : 1) s[at + i] = '#';
    }
}
// Build 121: which files go first when the zip would be too big for a Discord upload (8 MB):
// the oldest runs' logs, then older memory snapshots. Lower = dropped first.
static int KeepRank(const std::string& n) {
    if (n.find("_previous4") != std::string::npos) return 0;
    if (n.find("_previous3") != std::string::npos) return 1;
    if (n.find("_previous2") != std::string::npos) return 2;
    if (n.compare(0, 7, "desync_") == 0) return 3;
    return 9;
}
static void Put16(std::string& o, uint16_t v) { o.push_back((char)(v & 0xFF)); o.push_back((char)(v >> 8)); }
static void Put32(std::string& o, uint32_t v) { Put16(o, (uint16_t)(v & 0xFFFF)); Put16(o, (uint16_t)(v >> 16)); }

static DWORD WINAPI Worker(void*) {
    // what to hide
    std::vector<std::pair<std::string, const char*>> hide;
    const char* sn = net::SteamMyName();
    if (sn[0]) hide.push_back({sn, "<steam name>"});
    char pc[128] = ""; DWORD n = sizeof pc;
    if (GetComputerNameA(pc, &n) && pc[0]) hide.push_back({pc, "<pc>"});
    char user[128] = ""; n = sizeof user;
    if (GetUserNameA(user, &n) && user[0]) hide.push_back({user, "<user>"});
    if (g_uiName[0]) hide.push_back({g_uiName, "<name>"});

    std::vector<Entry> files;
    wchar_t pat[MAX_PATH];
    _snwprintf(pat, MAX_PATH, L"%ls*", util::ModDir());
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
            bool bin = ext && !_wcsicmp(ext, L".bin") && !_wcsnicmp(fd.cFileName, L"desync_", 7);   // build 121: memory snapshots
            if (!ext || (!bin && _wcsicmp(ext, L".txt") && _wcsicmp(ext, L".log") && _wcsicmp(ext, L".ini"))) continue;
            wchar_t path[MAX_PATH];
            _snwprintf(path, MAX_PATH, L"%ls%ls", util::ModDir(), fd.cFileName);
            HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE) continue;
            LARGE_INTEGER sz{};
            GetFileSizeEx(f, &sz);
            const LONGLONG cap = 4 << 20;
            LONGLONG from = sz.QuadPart > cap ? sz.QuadPart - cap : 0;
            LARGE_INTEGER mv; mv.QuadPart = from;
            SetFilePointerEx(f, mv, nullptr, FILE_BEGIN);
            Entry e;
            e.data.resize((size_t)(sz.QuadPart - from));
            DWORD got = 0;
            if (!e.data.empty()) ReadFile(f, &e.data[0], (DWORD)e.data.size(), &got, nullptr);
            CloseHandle(f);
            e.data.resize(got);
            if (bin) {
                for (auto& x : hide) MaskSameLen(e.data, x.first);
            } else {
                if (from) e.data = "(only the last 4 MB of this file)\r\n" + e.data;
                MaskSteamIds(e.data);
                for (auto& x : hide) ReplaceAll(e.data, x.first, x.second);
            }
            char nm[MAX_PATH];
            snprintf(nm, sizeof nm, "%ls", fd.cFileName);
            e.name = nm;
            e.crc = Crc32((const uint8_t*)e.data.data(), e.data.size());
            // build 121: compressed (deflate); text logs get about 10 times smaller
            e.size = (uint32_t)e.data.size();
            e.method = 0;
            size_t clen = 0;
            void* c = e.data.empty() ? nullptr : tdefl_compress_mem_to_heap(e.data.data(), e.data.size(), &clen, tdefl_create_comp_flags_from_zip_params(6, -15, 0));
            if (c && clen < e.data.size()) { e.data.assign((const char*)c, clen); e.method = 8; }
            if (c) mz_free(c);
            files.push_back(std::move(e));
        } while (FindNextFileW(h, &fd) && files.size() < 64);
        FindClose(h);
    }
    if (files.empty()) { snprintf(g_msg, sizeof g_msg, "No log files found next to the mod."); g_state = 3; return 0; }
    // build 121: at most about 8 MB (Discord's upload limit for most people)
    uint32_t dropped = 0;
    for (;;) {
        size_t total = 0;
        for (auto& e : files) total += e.data.size() + e.name.size() + 80;
        if (total <= (8u << 20) - (64u << 10)) break;
        int low = 9; size_t at = 0;
        for (size_t i = 0; i < files.size(); i++) {
            int r = KeepRank(files[i].name);
            if (r < low || (r == low && r == 3 && files[i].name < files[at].name)) { low = r; at = i; }
        }
        if (low == 9) break;
        files.erase(files.begin() + at);
        dropped++;
    }

    // a zip (build 121: deflate, stored when that is not smaller)
    SYSTEMTIME st;
    GetLocalTime(&st);
    uint16_t dt = (uint16_t)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay), tm = (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    std::string z;
    for (auto& e : files) {
        e.off = (uint32_t)z.size();
        Put32(z, 0x04034B50); Put16(z, 20); Put16(z, 0); Put16(z, e.method); Put16(z, tm); Put16(z, dt);
        Put32(z, e.crc); Put32(z, (uint32_t)e.data.size()); Put32(z, e.size);
        Put16(z, (uint16_t)e.name.size()); Put16(z, 0);
        z += e.name;
        z += e.data;
    }
    uint32_t cd = (uint32_t)z.size();
    for (auto& e : files) {
        Put32(z, 0x02014B50); Put16(z, 20); Put16(z, 20); Put16(z, 0); Put16(z, e.method); Put16(z, tm); Put16(z, dt);
        Put32(z, e.crc); Put32(z, (uint32_t)e.data.size()); Put32(z, e.size);
        Put16(z, (uint16_t)e.name.size()); Put16(z, 0); Put16(z, 0); Put16(z, 0); Put16(z, 0); Put32(z, 0); Put32(z, e.off);
        z += e.name;
    }
    uint32_t cdLen = (uint32_t)z.size() - cd;
    Put32(z, 0x06054B50); Put16(z, 0); Put16(z, 0); Put16(z, (uint16_t)files.size()); Put16(z, (uint16_t)files.size());
    Put32(z, cdLen); Put32(z, cd); Put16(z, 0);

    wchar_t desk[MAX_PATH] = L"";
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk)) || !desk[0]) wcscpy(desk, util::ModDir());
    size_t dl = wcslen(desk);
    if (dl && desk[dl - 1] == L'\\') desk[dl - 1] = 0;
    _snwprintf(g_zip, MAX_PATH, L"%ls\\arcana-rollback-logs-%04d-%02d-%02d-%02d%02d.zip", desk, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    HANDLE f = CreateFileW(g_zip, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wr = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, z.data(), (DWORD)z.size(), &wr, nullptr) && wr == z.size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) { snprintf(g_msg, sizeof g_msg, "Could not write the zip to the Desktop (error %lu).", GetLastError()); g_state = 3; return 0; }
    const wchar_t* base = wcsrchr(g_zip, L'\\');
    snprintf(g_msg, sizeof g_msg, "Saved on the Desktop: %ls  (%u files, %u KB)%s", base ? base + 1 : g_zip, (unsigned)files.size(), (unsigned)((z.size() + 1023) / 1024),
             dropped ? " - the oldest logs were left out to stay under 8 MB" : "");
    Log("logs: packed %u files into %ls (%u bytes)", (unsigned)files.size(), g_zip, (unsigned)z.size());
    g_state = 2;
    return 0;
}
static void Start() {
    if (g_state == 1) return;
    g_state = 1;
    snprintf(g_msg, sizeof g_msg, "Packing...");
    HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else { snprintf(g_msg, sizeof g_msg, "Could not start packing."); g_state = 3; }
}
// the zip selected in Explorer if there is one, else the mod's folder
static void OpenFolder() {
    wchar_t arg[MAX_PATH + 16];
    if (g_state == 2 && g_zip[0]) _snwprintf(arg, MAX_PATH + 16, L"/select,\"%ls\"", g_zip);
    else _snwprintf(arg, MAX_PATH + 16, L"\"%ls\"", util::ModDir());
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg, nullptr, SW_SHOWNORMAL);
}
#else
static void Start() { g_state = 2; snprintf(g_msg, sizeof g_msg, "Saved on the Desktop: arcana-rollback-logs-2026-11-16-1612.zip  (4 files, 412 KB)"); }
static void OpenFolder() {}
#endif
}  // namespace logpack
