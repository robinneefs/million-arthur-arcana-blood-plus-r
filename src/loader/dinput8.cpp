// Arcana Blood +R loader (build 119).
//
// The game loads dinput8.dll (DirectInput) by name when it starts. Windows looks in the
// game's own folder first, so this small DLL is found there instead of the system one. It
// hands every DirectInput call on to the real dinput8.dll from the Windows folder, and loads
// the mod (ArcanaPlusR\arcana_rollback.dll next to it). Nothing is injected from outside and
// no game file is changed: deleting this file and the ArcanaPlusR folder removes the mod.
//
// Playing without the mod: hold Shift while the game starts, or set enabled=0 under [mod]
// in ArcanaPlusR\rollback.ini. If the mod is missing or fails to load, the game runs as usual.
// What happened goes to ArcanaPlusR\loader_log.txt.

#include <windows.h>
#include <cstdio>

static HMODULE g_self = nullptr, g_real = nullptr;
static wchar_t g_dir[MAX_PATH];     // ...\ArcanaPlusR\ (with the backslash)

static void Note(const char* fmt, ...) {
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%lsloader_log.txt", g_dir);
    FILE* f = _wfopen(p, L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static FARPROC Real(const char* name) {
    if (!g_real) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);          // a 32-bit game gets SysWOW64 here
        wcscat(sys, L"\\dinput8.dll");
        g_real = LoadLibraryW(sys);
    }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}

extern "C" {
HRESULT WINAPI P_DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* o, LPUNKNOWN u) {
    auto f = (HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN))Real("DirectInput8Create");
    return f ? f(h, v, r, o, u) : E_FAIL;
}
HRESULT WINAPI P_DllCanUnloadNow() {
    auto f = (HRESULT(WINAPI*)())Real("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
HRESULT WINAPI P_DllGetClassObject(REFCLSID c, REFIID r, LPVOID* o) {
    auto f = (HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*))Real("DllGetClassObject");
    return f ? f(c, r, o) : E_FAIL;
}
HRESULT WINAPI P_DllRegisterServer() {
    auto f = (HRESULT(WINAPI*)())Real("DllRegisterServer");
    return f ? f() : E_FAIL;
}
HRESULT WINAPI P_DllUnregisterServer() {
    auto f = (HRESULT(WINAPI*)())Real("DllUnregisterServer");
    return f ? f() : E_FAIL;
}
LPCVOID WINAPI P_GetdfDIJoystick() {
    auto f = (LPCVOID(WINAPI*)())Real("GetdfDIJoystick");
    return f ? f() : nullptr;
}
}

// Runs once Windows has finished loading the game's DLLs (a thread started from DllMain
// only begins after that), so loading the mod here is safe.
static DWORD WINAPI LoadMod(LPVOID) {
    wchar_t ini[MAX_PATH], dll[MAX_PATH];
    _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", g_dir);
    _snwprintf(dll, MAX_PATH, L"%lsarcana_rollback.dll", g_dir);
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) { Note("Shift held: the game starts without the mod"); return 0; }
    if (!GetPrivateProfileIntW(L"mod", L"enabled", 1, ini)) { Note("enabled=0 in rollback.ini: the game starts without the mod"); return 0; }
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { Note("ArcanaPlusR\\arcana_rollback.dll is missing: the game starts without the mod"); return 0; }
    HMODULE m = LoadLibraryW(dll);
    if (m) Note("mod loaded");
    else Note("the mod did not load (error %lu): the game starts without it", GetLastError());
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = self;
        DisableThreadLibraryCalls(self);
        GetModuleFileNameW(self, g_dir, MAX_PATH);
        wchar_t* slash = wcsrchr(g_dir, L'\\');
        if (slash) slash[1] = 0;
        wcsncat(g_dir, L"ArcanaPlusR\\", MAX_PATH - wcslen(g_dir) - 1);
        HANDLE t = CreateThread(nullptr, 0, LoadMod, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
