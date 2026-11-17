// Build 120: finding the game, keeping our dinput8.dll in its folder, starting it (included
// into launcher.cpp).
//
// The game folder only ever gets dinput8.dll (src/loader). It loads the mod only when the
// game was started from here (ARCANA_PLUSR_DIR names our folder), so a start from Steam is
// the plain game. Each Play checks the file again, so a reinstalled or moved game gets it back.

namespace game {

static const wchar_t* kExeName = L"Million Arthur.exe";
static const wchar_t* kAppId = L"989550";

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::string ReadAll(const std::wstring& p) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    std::string s;
    if (!f) return s;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}
static std::wstring ReadText(const std::wstring& p) {
    std::string s = ReadAll(p);
    int wn = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], wn);
    return w;
}

// every quoted "path" in libraryfolders.vdf
static std::vector<std::wstring> SteamLibraries(const std::wstring& steamDir) {
    std::vector<std::wstring> libs{steamDir};
    std::wstring vdf = ReadText(steamDir + L"\\steamapps\\libraryfolders.vdf");
    size_t pos = 0;
    while ((pos = vdf.find(L"\"path\"", pos)) != std::wstring::npos) {
        size_t a = vdf.find(L'"', pos + 6);
        if (a == std::wstring::npos) break;
        size_t b = vdf.find(L'"', a + 1);
        if (b == std::wstring::npos) break;
        std::wstring p = vdf.substr(a + 1, b - a - 1), clean;
        for (size_t i = 0; i < p.size(); i++) {      // vdf escapes backslashes
            if (p[i] == L'\\' && i + 1 < p.size() && p[i + 1] == L'\\') i++;
            clean += p[i];
        }
        libs.push_back(clean);
        pos = b + 1;
    }
    return libs;
}
// the game's folder (no backslash at the end), "" if Steam doesn't have it
static std::wstring FindGameDir() {
    wchar_t steam[MAX_PATH] = L"";
    DWORD sz = sizeof steam;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &sz) != ERROR_SUCCESS) {
        sz = sizeof steam;
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ, nullptr, steam, &sz);
    }
    if (!steam[0]) return L"";
    for (wchar_t* c = steam; *c; c++) if (*c == L'/') *c = L'\\';
    for (auto& lib : SteamLibraries(steam)) {
        std::wstring acf = ReadText(lib + L"\\steamapps\\appmanifest_" + kAppId + L".acf");
        std::wstring dir = L"Million Arthur Arcana Blood";
        size_t p = acf.find(L"\"installdir\"");
        if (p != std::wstring::npos) {
            size_t a = acf.find(L'"', p + 12), b = a == std::wstring::npos ? a : acf.find(L'"', a + 1);
            if (a != std::wstring::npos && b != std::wstring::npos) dir = acf.substr(a + 1, b - a - 1);
        }
        std::wstring d = lib + L"\\steamapps\\common\\" + dir;
        if (FileExists(d + L"\\" + kExeName)) return d;
    }
    return L"";
}

static bool SteamRunning() { return FindWindowW(L"vguiPopupWindow", nullptr) || FindWindowW(L"SDL_app", L"Steam"); }

static DWORD FindGamePid() {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e{sizeof e};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (_wcsicmp(e.szExeFile, kExeName) == 0) { pid = e.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

// is this dinput8.dll one of ours? (its file details say "Arcana Blood +R")
static bool IsOurs(const std::wstring& p) {
    DWORD h = 0, n = GetFileVersionInfoSizeW(p.c_str(), &h);
    if (!n) return false;
    std::vector<char> buf(n);
    if (!GetFileVersionInfoW(p.c_str(), 0, n, buf.data())) return false;
    wchar_t* v = nullptr;
    UINT len = 0;
    return VerQueryValueW(buf.data(), L"\\StringFileInfo\\040904B0\\ProductName", (void**)&v, &len) && v && !wcscmp(v, L"Arcana Blood +R");
}

enum Prep { P_OK, P_NO_GAME, P_FOREIGN, P_LOCKED, P_NO_SOURCE, P_RUNNING };
// make sure the game folder holds our current dinput8.dll; replaceForeign: the player said yes
static Prep PrepareGameFolder(const std::wstring& gameDir, const std::wstring& ourDir, bool replaceForeign) {
    if (gameDir.empty()) return P_NO_GAME;
    std::wstring src = ourDir + L"dinput8.dll", dst = gameDir + L"\\dinput8.dll";
    if (!FileExists(src)) return P_NO_SOURCE;
    if (FileExists(dst)) {
        if (!IsOurs(dst)) {
            if (!replaceForeign) return P_FOREIGN;
            std::wstring keep = gameDir + L"\\dinput8.dll.before-plusr";
            if (!FileExists(keep) && !MoveFileW(dst.c_str(), keep.c_str())) return P_LOCKED;
            Say(L"Another dinput8.dll was in the game folder: kept as dinput8.dll.before-plusr");
        } else if (ReadAll(dst) == ReadAll(src)) return P_OK;        // already the right one
    }
    if (FindGamePid()) return P_RUNNING;
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) { Say(L"Could not copy dinput8.dll into the game folder (error %lu).", GetLastError()); return P_LOCKED; }
    Say(L"dinput8.dll put into the game folder.");
    return P_OK;
}

// Build 119 put the whole mod in <game>\ArcanaPlusR\: take its settings over once, then tidy it away
static void MoveFrom119(const std::wstring& gameDir, const std::wstring& ourDir) {
    std::wstring old = gameDir + L"\\ArcanaPlusR";
    if (!DirExists(old)) return;
    std::wstring ini = ourDir + L"rollback.ini", oldIni = old + L"\\rollback.ini";
    if (FileExists(oldIni) && ReadText(ini).find(L"[setup]") == std::wstring::npos) {
        CopyFileW(oldIni.c_str(), ini.c_str(), FALSE);
        Say(L"Settings and My setup taken over from the game folder (build 119).");
    }
    upd::RemoveTree(old);
    Say(L"Removed the old ArcanaPlusR folder from the game folder.");
}

// start the game with the mod; false + why when it could not start
static bool Start(const std::wstring& gameDir, const std::wstring& ourDir, std::string& why) {
    std::wstring exe = gameDir + L"\\" + kExeName;
    // Tell the Steam layer which game this is, so it runs without being relaunched by Steam
    // (Steam itself must be running), and tell our dinput8.dll where the mod is.
    SetEnvironmentVariableW(L"SteamAppId", kAppId);
    SetEnvironmentVariableW(L"SteamGameId", kAppId);
    SetEnvironmentVariableW(L"ARCANA_PLUSR_DIR", ourDir.c_str());
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\"";
    BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, gameDir.c_str(), &si, &pi);
    SetEnvironmentVariableW(L"ARCANA_PLUSR_DIR", nullptr);
    if (!ok) { why = "Windows could not start the game."; Say(L"Could not start the game (error %lu).", GetLastError()); return false; }
    Say(L"Game started (pid %lu).", pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace game
