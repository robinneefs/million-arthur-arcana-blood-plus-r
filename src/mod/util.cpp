#include "util.h"

namespace util {

static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logLock;
static wchar_t g_modDir[MAX_PATH] = L"";

void SetModDir(HMODULE self) {
    GetModuleFileNameW(self, g_modDir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_modDir, L'\\');
    if (slash) slash[1] = 0;
}

const wchar_t* ModDir() { return g_modDir; }

void LogOpen(const wchar_t* path) {
    InitializeCriticalSection(&g_logLock);
    // Build 89: keep the last session's log (a crash log survives a restart).
    wchar_t prev[MAX_PATH];
    wcsncpy(prev, path, MAX_PATH - 1); prev[MAX_PATH - 1] = 0;
    wchar_t* dot = wcsrchr(prev, L'.');
    if (dot && (size_t)(dot - prev) + 16 < MAX_PATH) {
        wcscpy(dot, L"_previous.txt");
        MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);
    }
    g_log = _wfopen(path, L"w");
}

void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

void LogClose() {
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

bool BytesMatch(uint32_t at, const uint8_t* expect, size_t n) {
    return memcmp(reinterpret_cast<const void*>(at), expect, n) == 0;
}

bool WriteCode(uint32_t at, const void* bytes, size_t n) {
    DWORD old;
    void* p = reinterpret_cast<void*>(at);
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, bytes, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return true;
}

bool WriteRel32(uint32_t at, uint8_t opcode, const void* target) {
    uint8_t buf[5];
    buf[0] = opcode;
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(target) - (at + 5));
    memcpy(buf + 1, &rel, 4);
    return WriteCode(at, buf, 5);
}

void* SwapIat(uint32_t slot, void* replacement) {
    void** p = reinterpret_cast<void**>(slot);
    DWORD old;
    VirtualProtect(p, sizeof(void*), PAGE_READWRITE, &old);
    void* prev = InterlockedExchangePointer(p, replacement);
    VirtualProtect(p, sizeof(void*), old, &old);
    return prev;
}

} // namespace util
