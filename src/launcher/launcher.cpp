// Arcana Blood +R launcher.
//
// Finds the Steam install of Million Arthur: Arcana Blood, starts the game,
// and loads arcana_rollback.dll (sitting next to this exe) into it.
// It never writes anything into the game folder. Before that it looks for a newer
// version of the mod on GitHub and asks before installing it (update.inc).
//
// Usage:
//   arcana_launcher.exe            start the game with the mod
//   arcana_launcher.exe --attach   add the mod to a game that is already running
//   arcana_launcher.exe --game "H:\path\to\Million Arthur.exe"   use this exe

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include <vector>
#include "../mod/version.h"

static const wchar_t* kExeName = L"Million Arthur.exe";
static const wchar_t* kDllName = L"arcana_rollback.dll";
static const wchar_t* kAppId = L"989550";

// Build 117: no console window. What the launcher has to say goes to launcher_log.txt next to
// it; when something goes wrong the last lines are shown in a message box.
static std::wstring g_said;
static FILE* g_log = nullptr;
static void Say(const wchar_t* fmt, ...) {
    wchar_t line[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(line, 1023, fmt, ap);
    va_end(ap);
    line[1023] = 0;
    g_said += line;
    g_said += L"\n";
    if (g_log) { fwprintf(g_log, L"%ls\n", line); fflush(g_log); }
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring ReadTextFile(const std::wstring& p) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return L"";
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    int wn = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], wn);
    return w;
}

// Pulls every quoted "path" value out of libraryfolders.vdf.
static std::vector<std::wstring> SteamLibraries(const std::wstring& steamDir) {
    std::vector<std::wstring> libs{steamDir};
    std::wstring vdf = ReadTextFile(steamDir + L"\\steamapps\\libraryfolders.vdf");
    size_t pos = 0;
    while ((pos = vdf.find(L"\"path\"", pos)) != std::wstring::npos) {
        size_t a = vdf.find(L'"', pos + 6);
        if (a == std::wstring::npos) break;
        size_t b = vdf.find(L'"', a + 1);
        if (b == std::wstring::npos) break;
        std::wstring p = vdf.substr(a + 1, b - a - 1);
        std::wstring clean;
        for (size_t i = 0; i < p.size(); i++) {      // vdf escapes backslashes
            if (p[i] == L'\\' && i + 1 < p.size() && p[i + 1] == L'\\') i++;
            clean += p[i];
        }
        libs.push_back(clean);
        pos = b + 1;
    }
    return libs;
}

static std::wstring FindGameExe() {
    wchar_t steam[MAX_PATH] = L"";
    DWORD sz = sizeof steam;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &sz) != ERROR_SUCCESS) {
        sz = sizeof steam;
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ, nullptr, steam, &sz);
    }
    if (!steam[0]) return L"";
    for (wchar_t* c = steam; *c; c++) if (*c == L'/') *c = L'\\';
    for (auto& lib : SteamLibraries(steam)) {
        std::wstring manifest = lib + L"\\steamapps\\appmanifest_" + kAppId + L".acf";
        std::wstring acf = ReadTextFile(manifest);
        std::wstring dir = L"Million Arthur Arcana Blood";
        size_t p = acf.find(L"\"installdir\"");
        if (p != std::wstring::npos) {
            size_t a = acf.find(L'"', p + 12), b = acf.find(L'"', a + 1);
            if (a != std::wstring::npos && b != std::wstring::npos) dir = acf.substr(a + 1, b - a - 1);
        }
        std::wstring exe = lib + L"\\steamapps\\common\\" + dir + L"\\" + kExeName;
        if (FileExists(exe)) return exe;
    }
    return L"";
}

static DWORD FindGamePid(DWORD exclude) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e{sizeof e};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (_wcsicmp(e.szExeFile, kExeName) == 0 && e.th32ProcessID != exclude) { pid = e.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

static bool HasModule(DWORD pid, const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W m{sizeof m};
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &m); ok; ok = Module32NextW(snap, &m))
        if (_wcsicmp(m.szModule, name) == 0) { found = true; break; }
    CloseHandle(snap);
    return found;
}

// Classic DLL loading: write the DLL path into the game and have one of its
// threads call LoadLibraryW on it.
static bool Inject(HANDLE proc, const std::wstring& dll) {
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    bool ok = false;
    if (WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr)) {
        auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE t = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
        if (t) {
            if (WaitForSingleObject(t, 20000) == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeThread(t, &code);
                ok = code != 0;      // HMODULE of our DLL, 0 on failure
            }
            CloseHandle(t);
        }
    }
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return ok;
}

static bool InjectPid(DWORD pid, const std::wstring& dll) {
    if (HasModule(pid, kDllName)) { Say(L"The mod is already loaded in this game."); return true; }
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { Say(L"Could not open the game process (error %lu).", GetLastError()); return false; }
    bool ok = Inject(proc, dll);
    CloseHandle(proc);
    return ok;
}

#include "update.inc"

static int Finish(int code) {
    if (code) {
        std::wstring t = g_said;
        if (t.size() > 1500) t = L"..." + t.substr(t.size() - 1500);
        MessageBoxW(nullptr, t.c_str(), L"Arcana Blood +R", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
    if (g_log) fclose(g_log);
    return code;
}

int wmain(int argc, wchar_t** argv) {
    {
        wchar_t self0[MAX_PATH];
        GetModuleFileNameW(nullptr, self0, MAX_PATH);
        std::wstring lp(self0);
        lp = lp.substr(0, lp.find_last_of(L'\\') + 1) + L"launcher_log.txt";
        g_log = _wfopen(lp.c_str(), L"w, ccs=UTF-8");
    }
    Say(L"Arcana Blood +R launcher  %hs BETA (build %d)", MOD_VERSION_NUM, MOD_BUILD);
    bool attach = false;
    std::wstring exe;
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--attach") == 0) attach = true;
        else if (_wcsicmp(argv[i], L"--game") == 0 && i + 1 < argc) exe = argv[++i];
    }

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir(self);
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    std::wstring dll = dir + kDllName;
    if (!FileExists(dll)) { Say(L"Missing %ls next to the launcher.", kDllName); return Finish(1); }

    if (attach) {
        Say(L"Waiting for the game to be running...");
        DWORD pid = 0;
        for (int i = 0; i < 600 && !(pid = FindGamePid(0)); i++) Sleep(100);
        if (!pid) { Say(L"Game not found."); return Finish(1); }
        bool ok = InjectPid(pid, dll);
        Say(ok ? L"Mod loaded." : L"Could not load the mod.");
        return Finish(ok ? 0 : 1);
    }

    if (DWORD running = FindGamePid(0)) {
        Say(L"The game is already running. Close it first, then start the launcher again.");
        (void)running;
        return Finish(1);
    }
    if (upd::CheckAndUpdate(dir, argc, argv)) return 0;   // build 115: the updated launcher took over
    if (exe.empty()) exe = FindGameExe();
    if (exe.empty() || !FileExists(exe)) { Say(L"Could not find the game. Use --game \"path to Million Arthur.exe\"."); return Finish(1); }
    Say(L"Game: %ls", exe.c_str());
    std::wstring gameDir = exe.substr(0, exe.find_last_of(L'\\'));

    // Tell the Steam layer which game this is, so it runs without being
    // relaunched by Steam. Steam itself must be running and logged in.
    SetEnvironmentVariableW(L"SteamAppId", kAppId);
    SetEnvironmentVariableW(L"SteamGameId", kAppId);

    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, gameDir.c_str(), &si, &pi)) {
        Say(L"Could not start the game (error %lu).", GetLastError());
        return Finish(1);
    }
    Say(L"Game started (pid %lu). Loading the mod...", pi.dwProcessId);
    Sleep(300);   // let Windows finish setting up the process

    bool ok = false;
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) ok = Inject(pi.hProcess, dll);
    if (!ok && WaitForSingleObject(pi.hProcess, 3000) == WAIT_OBJECT_0) {
        // The Steam layer restarted the game through Steam. Find the new one.
        Say(L"The game restarted through Steam. Waiting for it...");
        DWORD pid = 0;
        for (int i = 0; i < 600 && !(pid = FindGamePid(pi.dwProcessId)); i++) Sleep(100);
        if (pid) { Sleep(300); ok = InjectPid(pid, dll); }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (!ok) { Say(L"Could not load the mod into the game."); return Finish(1); }
    Say(L"Mod loaded.");
    return Finish(0);
}
