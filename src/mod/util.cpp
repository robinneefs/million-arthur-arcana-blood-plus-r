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

// Build 121: the last lines of the log, for crash reports (WriteCrashReport).
static const int TAIL_N = 300, TAIL_LEN = 400;
static char g_tail[TAIL_N][TAIL_LEN];
static int g_tailNext = 0, g_tailCount = 0;
static bool g_prevUnclean = false;

void LogOpen(const wchar_t* path) {
    InitializeCriticalSection(&g_logLock);
    // Build 89: keep the last session's log (a crash log survives a restart).
    // Build 121: the last 5 runs: rollback_log.txt, _previous.txt, _previous2.txt .. _previous4.txt
    wchar_t base[MAX_PATH];
    wcsncpy(base, path, MAX_PATH - 1); base[MAX_PATH - 1] = 0;
    wchar_t* dot = wcsrchr(base, L'.');
    if (dot && (size_t)(dot - base) + 20 < MAX_PATH) {
        *dot = 0;
        wchar_t from[MAX_PATH], to[MAX_PATH];
        for (int i = 4; i >= 1; i--) {
            if (i == 1) _snwprintf(from, MAX_PATH, L"%ls.txt", base);
            else if (i == 2) _snwprintf(from, MAX_PATH, L"%ls_previous.txt", base);
            else _snwprintf(from, MAX_PATH, L"%ls_previous%d.txt", base, i - 1);
            if (i == 1) _snwprintf(to, MAX_PATH, L"%ls_previous.txt", base);
            else _snwprintf(to, MAX_PATH, L"%ls_previous%d.txt", base, i);
            MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
        }
    }
    g_log = _wfopen(path, L"w");
    // Build 121: a flag file that only a normal close removes (DllMain): if it is still there,
    // the last run crashed or was closed by force.
    wchar_t flag[MAX_PATH];
    _snwprintf(flag, MAX_PATH, L"%lsrollback_running.flag", g_modDir);
    g_prevUnclean = GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES;
    HANDLE f = CreateFileW(flag, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
}
bool PrevRunUnclean() { return g_prevUnclean; }
void MarkCleanExit() {
    wchar_t flag[MAX_PATH];
    _snwprintf(flag, MAX_PATH, L"%lsrollback_running.flag", g_modDir);
    DeleteFileW(flag);
}

// Build 128 (security): packets from the other player can make the mod log a line each, so a
// flood could fill the disk and stutter the game (every line is flushed). Lines are let through
// from a budget: bursts of up to 5000 lines (the desync reports), then 200 a second; the rest
// is counted and said once the budget is back. A run's log stops at 100 MB.
static double g_logBudget = 5000;
static DWORD g_logBudgetAt = 0;
static uint32_t g_logDropped = 0;
static uint64_t g_logBytes = 0;
static const uint64_t LOG_MAX_BYTES = 100ull << 20;
void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    {
        DWORD now = GetTickCount();
        if (g_logBudgetAt) g_logBudget += (now - g_logBudgetAt) * 0.2;   // 200 lines a second
        if (g_logBudget > 5000) g_logBudget = 5000;
        g_logBudgetAt = now;
        if (g_logBytes > LOG_MAX_BYTES) {
            static bool said = false;
            if (!said) { said = true; fputs("(the log is full: 100 MB; nothing more is written this run)\n", g_log); fflush(g_log); }
            LeaveCriticalSection(&g_logLock);
            return;
        }
        if (g_logBudget < 1) { g_logDropped++; LeaveCriticalSection(&g_logLock); return; }
        g_logBudget -= 1;
        if (g_logDropped) {
            char d[96];
            int dn = snprintf(d, sizeof d, "(%u log lines were left out: too many in a short time)\n", g_logDropped);
            fputs(d, g_log);
            g_logBytes += dn > 0 ? dn : 0;
            g_logDropped = 0;
        }
    }
    SYSTEMTIME t; GetLocalTime(&t);
    char line[8192];
    int n = snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    fputs(line, g_log);
    fputc('\n', g_log);
    g_logBytes += strlen(line) + 1;
    fflush(g_log);
    strncpy(g_tail[g_tailNext], line, TAIL_LEN - 1);
    g_tail[g_tailNext][TAIL_LEN - 1] = 0;
    g_tailNext = (g_tailNext + 1) % TAIL_N;
    if (g_tailCount < TAIL_N) g_tailCount++;
    LeaveCriticalSection(&g_logLock);
}

// Build 121: crash_<date>_<time>.txt next to the mod: what happened plus the last 300 log
// lines. Only the 5 newest are kept. Pack logs takes them with the logs.
void WriteCrashReport(const char* text) {
    wchar_t pat[MAX_PATH];
    _snwprintf(pat, MAX_PATH, L"%lscrash_*.txt", g_modDir);
    WIN32_FIND_DATAW fd;
    wchar_t names[64][MAX_PATH];
    int n = 0;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (n < 64) { wcsncpy(names[n], fd.cFileName, MAX_PATH - 1); names[n][MAX_PATH - 1] = 0; n++; } } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // oldest first (names hold the date and time), keep 4 + this one
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (wcscmp(names[j], names[i]) < 0) { wchar_t tmp[MAX_PATH]; wcscpy(tmp, names[i]); wcscpy(names[i], names[j]); wcscpy(names[j], tmp); }
    for (int i = 0; i + 4 < n; i++) {
        wchar_t p[MAX_PATH];
        _snwprintf(p, MAX_PATH, L"%ls%ls", g_modDir, names[i]);
        DeleteFileW(p);
    }
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%lscrash_%04d-%02d-%02d_%02d%02d%02d.txt", g_modDir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    FILE* f = _wfopen(path, L"w");
    if (!f) return;
    fprintf(f, "Arcana Blood +R crash report\n%s\n\nLast %d lines of rollback_log.txt:\n", text, g_tailCount);
    // no lock: the crashing thread may hold it; the lines are only read
    for (int i = 0; i < g_tailCount; i++) {
        int k = (g_tailNext - g_tailCount + i + TAIL_N) % TAIL_N;
        fputs(g_tail[k], f);
        fputc('\n', f);
    }
    fclose(f);
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
