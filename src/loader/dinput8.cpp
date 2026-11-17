// Arcana Blood +R loader (build 119, launcher-only since build 120).
//
// The game loads dinput8.dll (DirectInput) by name when it starts. Windows looks in the
// game's own folder first, so this small DLL is found there instead of the system one. It
// always hands every DirectInput call on to the real dinput8.dll from the Windows folder.
//
// It loads the mod ONLY when our launcher started the game: the launcher sets the
// environment variable ARCANA_PLUSR_DIR to its own folder (where arcana_rollback.dll,
// rollback.ini and the logs live). Started from Steam directly, the variable is missing and
// the game runs as usual, untouched. Holding Shift while it starts also skips the mod.
// Nothing is injected from outside and no game file is changed. The launcher puts this file
// back after a game reinstall; uninstalling the mod removes it.
// What happened goes to loader_log.txt in the launcher's folder.

#include <windows.h>
#include <cstdio>

static HMODULE g_self = nullptr, g_real = nullptr;
static wchar_t g_dir[MAX_PATH];     // the launcher's folder (from ARCANA_PLUSR_DIR), with a backslash at the end

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
    wchar_t dll[MAX_PATH];
    _snwprintf(dll, MAX_PATH, L"%lsarcana_rollback.dll", g_dir);
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) { Note("Shift held: the game starts without the mod"); return 0; }
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { Note("arcana_rollback.dll is missing: the game starts without the mod"); return 0; }
    HMODULE m = LoadLibraryW(dll);
    if (m) Note("mod loaded");
    else Note("the mod did not load (error %lu): the game starts without it", GetLastError());
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = self;
        DisableThreadLibraryCalls(self);
        DWORD n = GetEnvironmentVariableW(L"ARCANA_PLUSR_DIR", g_dir, MAX_PATH - 2);
        if (!n || n >= MAX_PATH - 2) return TRUE;           // not started by our launcher: DirectInput only
        if (g_dir[n - 1] != L'\\') { g_dir[n] = L'\\'; g_dir[n + 1] = 0; }
        SetEnvironmentVariableW(L"ARCANA_PLUSR_DIR", nullptr);   // programs the game starts don't get it
        HANDLE t = CreateThread(nullptr, 0, LoadMod, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
