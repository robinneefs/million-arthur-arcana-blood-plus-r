// Small helpers: log file, code patching, fast page hashing.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace util {

// ---- Log file (written next to the mod DLL, never in the game folder) ----
void LogOpen(const wchar_t* path);
void Log(const char* fmt, ...);
void LogClose();
// Build 121: crash report file (crash_<date>_<time>.txt, 5 kept) with the last log lines
void WriteCrashReport(const char* text);
// Build 121: the last run did not close normally (crash or forced close); MarkCleanExit at a normal close
bool PrevRunUnclean();
void MarkCleanExit();

// Folder our DLL lives in, with trailing backslash.
const wchar_t* ModDir();
void SetModDir(HMODULE self);

// ---- Patching ----
// Returns true if memory at `at` equals `expect` (n bytes).
bool BytesMatch(uint32_t at, const uint8_t* expect, size_t n);
// Writes bytes into code, temporarily making it writable.
bool WriteCode(uint32_t at, const void* bytes, size_t n);
// Writes a 5-byte CALL or JMP (opcode 0xE8 / 0xE9) to `target`.
bool WriteRel32(uint32_t at, uint8_t opcode, const void* target);
// Swaps a pointer in the import table, returns the old pointer.
void* SwapIat(uint32_t slot, void* replacement);

// ---- Hashing ----
// Hash of one 4 KB page. Fast, not cryptographic: only used to spot changes.
inline uint32_t HashPage(const void* p) {
    const uint32_t* q = static_cast<const uint32_t*>(p);
    uint32_t h0 = 0x811C9DC5u, h1 = 0x01000193u, h2 = 0x9E3779B1u, h3 = 0x85EBCA77u;
    for (int i = 0; i < 1024; i += 4) {
        h0 = (h0 ^ q[i + 0]) * 0x9E3779B1u;
        h1 = (h1 ^ q[i + 1]) * 0x85EBCA77u;
        h2 = (h2 ^ q[i + 2]) * 0xC2B2AE3Du;
        h3 = (h3 ^ q[i + 3]) * 0x27D4EB2Fu;
    }
    return h0 ^ ((h1 << 7) | (h1 >> 25)) ^ ((h2 << 13) | (h2 >> 19)) ^ ((h3 << 23) | (h3 >> 9));
}

} // namespace util
