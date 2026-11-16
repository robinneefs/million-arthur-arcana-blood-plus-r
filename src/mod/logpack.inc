// ---------------------------------------------------------------------------
// Build 104: Settings > Logs > Pack logs. Puts the mod's logs and settings files (the
// .txt / .log / .ini files next to the mod) into one zip on the Desktop, with Steam IDs,
// the Steam name, the PC name and the Windows user name hidden. Big logs: only the
// last 4 MB. Runs on its own thread; nothing is sent anywhere.
// ---------------------------------------------------------------------------
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
struct Entry { std::string name, data; uint32_t crc; uint32_t off; };
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
            if (!ext || (_wcsicmp(ext, L".txt") && _wcsicmp(ext, L".log") && _wcsicmp(ext, L".ini"))) continue;
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
            if (from) e.data = "(only the last 4 MB of this file)\r\n" + e.data;
            MaskSteamIds(e.data);
            for (auto& x : hide) ReplaceAll(e.data, x.first, x.second);
            char nm[MAX_PATH];
            snprintf(nm, sizeof nm, "%ls", fd.cFileName);
            e.name = nm;
            e.crc = Crc32((const uint8_t*)e.data.data(), e.data.size());
            files.push_back(std::move(e));
        } while (FindNextFileW(h, &fd) && files.size() < 64);
        FindClose(h);
    }
    if (files.empty()) { snprintf(g_msg, sizeof g_msg, "No log files found next to the mod."); g_state = 3; return 0; }

    // a stored (uncompressed) zip
    SYSTEMTIME st;
    GetLocalTime(&st);
    uint16_t dt = (uint16_t)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay), tm = (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    std::string z;
    for (auto& e : files) {
        e.off = (uint32_t)z.size();
        Put32(z, 0x04034B50); Put16(z, 20); Put16(z, 0); Put16(z, 0); Put16(z, tm); Put16(z, dt);
        Put32(z, e.crc); Put32(z, (uint32_t)e.data.size()); Put32(z, (uint32_t)e.data.size());
        Put16(z, (uint16_t)e.name.size()); Put16(z, 0);
        z += e.name;
        z += e.data;
    }
    uint32_t cd = (uint32_t)z.size();
    for (auto& e : files) {
        Put32(z, 0x02014B50); Put16(z, 20); Put16(z, 20); Put16(z, 0); Put16(z, 0); Put16(z, tm); Put16(z, dt);
        Put32(z, e.crc); Put32(z, (uint32_t)e.data.size()); Put32(z, (uint32_t)e.data.size());
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
    snprintf(g_msg, sizeof g_msg, "Saved on the Desktop: %ls  (%u files, %u KB)", base ? base + 1 : g_zip, (unsigned)files.size(), (unsigned)((z.size() + 1023) / 1024));
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
