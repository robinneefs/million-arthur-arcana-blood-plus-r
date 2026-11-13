// Arcana Blood rollback mod - step 3 prototype (test build 59: own overlay for lobby and match setup).
//
// What this DLL does once the launcher has loaded it into the game:
//   1. Waits until Steam's DRM layer has decrypted the game code.
//   2. Checks that the code is exactly the version we studied (else does nothing).
//   3. Redirects the game's per-frame "scene update" call to our function,
//      and swaps rand()/srand() for a copy whose seed we can save.
//   4. Adds three hotkeys (only while the game window is focused):
//        F9  = Profile: for ~10 s, record which memory changes each frame,
//              both in the game's fixed memory and in its heap.
//              Writes rollback_profile.txt.
//        F10 = Sync test on/off: every frame, rewind N frames and replay them,
//              then check memory is byte-for-byte identical. Needs a profile
//              from the same game session.
//        F11 = Change rewind depth N (1, 2, 4, 8).
//   Status is shown in the game window title. Details go to rollback_log.txt.
//
// Changes in build 2 (after the first test):
//   - The game reads the real clock once per frame. During a replay we now
//     hand it the same clock values it saw the first time.
//   - Part of the fight lives in heap memory (pointers like 0x070DAC10 in
//     the first log). The profile now finds those heap blocks too and the
//     sync test saves/restores them.
//   - Memory that is written both inside and outside the update is restored
//     now (leaving it out caused the "speed up" and the crash), except two
//     pages that belong to other threads / timing.
//
// Changes in build 6 (after test 5 froze in the game's drawing code):
//   - Some fight memory changed while the sync test was mid-rewind, which
//     means another thread of the game writes it at the same time. After the
//     F9 profile, a "writer check" now runs for ~5 s and notes which thread
//     and which game code writes each piece of fight memory.
//   - The sync test leaves out memory that other threads write, so a rewind
//     can no longer undo their work behind their back.
//
// Changes in build 7 (after test 6): the writer check showed the memory other
// threads write is graphics-driver and sound buffers, not fight data. But the
// fight data lives in a few big heap blocks (1.4 MB+), and the profile only
// sampled pieces of them, so rarely-changed parts were never rewound. The
// sync test now saves and restores those blocks whole.
//
// Changes in build 8 (after test 7: only 12 bytes off, then a heap crash):
//   During a replay, memory requests got the block the original run got,
//   matched only by size. If the replay asked from different code (likely
//   the sound system, which is not rewound and plays sounds twice), two
//   owners ended up with one block and both freed it. Now a replayed request
//   must also come from the same code; otherwise it gets fresh memory.
//
// Changes in build 9 (test 8 crashed in the same place): the game's memory
// manager turned out to be the Windows process heap, shared by every DLL in
// the game (graphics driver, sound, Steam, and this mod). Some "fight" heap
// blocks belonged to those. Now: the mod uses a private heap, and the sync
// test leaves out any heap block that non-game code writes.
//
// Nothing is written into the game folder. All files sit next to this DLL.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <map>
#include <algorithm>
#include <atomic>
#include <new>
#include <unordered_map>
#include "addr.h"
#include "util.h"
#include "transport.h"
#include <cstdarg>
#include <cstddef>

using util::Log;

// ---------------------------------------------------------------------------
// The game's memory manager uses the Windows "process heap", which every DLL
// in the game shares, this mod included. Test 8 showed that this let the
// rewind touch memory that isn't the game's. So all of this mod's own memory
// now comes from a private heap the game never sees.
// ---------------------------------------------------------------------------
static HANDLE ModHeap() {
    static HANDLE h = HeapCreate(0, 0, 0);
    return h;
}
void* operator new(size_t n) {
    void* p = HeapAlloc(ModHeap(), 0, n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return HeapAlloc(ModHeap(), 0, n ? n : 1); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return HeapAlloc(ModHeap(), 0, n ? n : 1); }
void operator delete(void* p) noexcept { if (p) HeapFree(ModHeap(), 0, p); }
void operator delete[](void* p) noexcept { if (p) HeapFree(ModHeap(), 0, p); }
void operator delete(void* p, size_t) noexcept { if (p) HeapFree(ModHeap(), 0, p); }
void operator delete[](void* p, size_t) noexcept { if (p) HeapFree(ModHeap(), 0, p); }

// ---------------------------------------------------------------------------
// Settings (rollback.ini next to the DLL, all optional)
// ---------------------------------------------------------------------------
struct Settings {
    int profileFrames = 600;     // how long F9 profiles (60 = 1 second)
    int depth = 1;               // sync test rewind depth
    int includeContested = 1;    // restore memory written inside AND outside update
    int maxDiffEvents = 30;      // detailed desync reports to write
    int heap = 1;                // also profile heap memory
    int heapRestore = 1;         // also restore heap memory found by the profile
    int writerFrames = 240;      // writer check after the profile (0 = off)
    int skipThreadChunks = 1;    // sync test leaves out memory other threads write
    int wholeBlocks = 1;         // sync test saves whole heap blocks, not just sampled chunks
    int hunt = 1;                // during the sync test, watch the rest of .data and add what the update writes
    int dirtyTracking = 1;       // build 35: save/restore only memory that changed
    int useProfile = 0;          // build 49: 0 = fixed list + memory watch (no profile), 1 = old profile (F9 first)
    int watchHeapMB = 1024;      // build 49: most heap memory the watch covers
    // Build 30: fake-lag rollback test (F7)
    int fakeLag = 4;             // frames until the "other player's" input arrives
    int fakeJitter = 2;          // up to this many frames extra, at random
    int fakeLoss = 5;            // % of inputs that arrive 4 frames later still (a lost packet resent)
    int fakeRemote = 1;          // which player's input is treated as coming over the network (1 or 2)
    int verifyEvery = 4;         // every N frames, rewind 8 frames and check the replay matches (0 = off)
    int onlineVerifyEvery = 0;   // build 57: the same check in online fights (test 56: it cost the laptop ~20 ms a time, so off)
};
static Settings g_cfg;

static void LoadSettings() {
    wchar_t ini[MAX_PATH];
    _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
    g_cfg.profileFrames    = GetPrivateProfileIntW(L"rollback", L"profile_frames", 600, ini);
    g_cfg.depth            = GetPrivateProfileIntW(L"rollback", L"depth", 1, ini);
    g_cfg.includeContested = GetPrivateProfileIntW(L"rollback", L"include_contested", 1, ini);
    g_cfg.maxDiffEvents    = GetPrivateProfileIntW(L"rollback", L"max_diff_reports", 30, ini);
    g_cfg.heap             = GetPrivateProfileIntW(L"rollback", L"heap", 1, ini);
    g_cfg.heapRestore      = GetPrivateProfileIntW(L"rollback", L"heap_restore", 1, ini);
    g_cfg.writerFrames     = GetPrivateProfileIntW(L"rollback", L"writer_frames", 240, ini);
    g_cfg.skipThreadChunks = GetPrivateProfileIntW(L"rollback", L"skip_thread_chunks", 1, ini);
    g_cfg.wholeBlocks      = GetPrivateProfileIntW(L"rollback", L"whole_blocks", 1, ini);
    g_cfg.hunt             = GetPrivateProfileIntW(L"rollback", L"hunt", 1, ini);
    g_cfg.dirtyTracking    = GetPrivateProfileIntW(L"rollback", L"changed_memory_only", 1, ini);
    g_cfg.useProfile       = GetPrivateProfileIntW(L"rollback", L"use_profile", 0, ini);
    g_cfg.watchHeapMB      = GetPrivateProfileIntW(L"rollback", L"watch_heap_mb", 1024, ini);
    if (g_cfg.watchHeapMB < 0) g_cfg.watchHeapMB = 0;
    if (g_cfg.watchHeapMB > 1536) g_cfg.watchHeapMB = 1536;
    g_cfg.fakeLag          = GetPrivateProfileIntW(L"rollback", L"fake_lag_frames", 4, ini);
    g_cfg.fakeJitter       = GetPrivateProfileIntW(L"rollback", L"fake_jitter_frames", 2, ini);
    g_cfg.fakeLoss         = GetPrivateProfileIntW(L"rollback", L"fake_loss_percent", 5, ini);
    g_cfg.fakeRemote       = GetPrivateProfileIntW(L"rollback", L"fake_remote_player", 1, ini);
    g_cfg.verifyEvery      = GetPrivateProfileIntW(L"rollback", L"verify_every", 4, ini);
    g_cfg.onlineVerifyEvery = GetPrivateProfileIntW(L"rollback", L"online_verify_every", 0, ini);
    if (g_cfg.fakeLag < 0) g_cfg.fakeLag = 0;
    if (g_cfg.fakeLag > 6) g_cfg.fakeLag = 6;
    if (g_cfg.fakeJitter < 0) g_cfg.fakeJitter = 0;
    if (g_cfg.fakeRemote != 2) g_cfg.fakeRemote = 1;
    if (g_cfg.depth < 1) g_cfg.depth = 1;
    if (g_cfg.depth > 8) g_cfg.depth = 8;
    if (g_cfg.profileFrames < 60) g_cfg.profileFrames = 60;
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
typedef uint32_t(__attribute__((thiscall)) * SceneUpdateFn)(void* scene);
static const SceneUpdateFn OrigSceneUpdate = reinterpret_cast<SceneUpdateFn>(addr::FN_SCENE_UPDATE);

static DWORD g_gameTid = 0;              // the game logic thread
static volatile int g_stage = 0;         // where the sync test is, for the freeze watchdog
static volatile uint32_t g_tick = 0;     // bumps every frame
static std::atomic<bool> g_inUpdate{false};
static uint32_t g_frame = 0;             // main scene updates seen

enum class Mode { Idle, Profiling, Writers, SyncTest };
static Mode g_mode = Mode::Idle;

static char g_status[256] = "loaded";
static CRITICAL_SECTION g_statusLock;
static void SetStatus(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    EnterCriticalSection(&g_statusLock);
    strcpy(g_status, buf);
    LeaveCriticalSection(&g_statusLock);
}

static inline HANDLE CrtHeap() { return *reinterpret_cast<HANDLE*>(addr::G_CRT_HEAP); }
static inline bool OnGameThread() { return GetCurrentThreadId() == g_gameTid; }
static inline bool Watching() { return g_inUpdate.load(std::memory_order_relaxed) && OnGameThread(); }

// ---------------------------------------------------------------------------
// rand()/srand() replacement. Same formula as the C runtime, same starting
// seed (we read the game's current per-thread seed the first time).
// ---------------------------------------------------------------------------
typedef uint32_t*(__cdecl* GetPtdFn)();
static const GetPtdFn GameGetPtd = reinterpret_cast<GetPtdFn>(addr::FN_GETPTD);
static __thread uint32_t t_seed = 1;
static __thread bool t_seedInit = false;
static std::atomic<uint32_t> g_randCallsInUpdate{0};

static inline void SeedInit() {
    if (!t_seedInit) { t_seed = GameGetPtd()[5]; t_seedInit = true; }  // ptd->_holdrand
}
// Build 24: the game thread's rand calls outside the update (drawing,
// effects) get their own seed, so they can't shift the game logic's
// random numbers. The sync test also logs which code calls rand, so a
// replay that calls it a different number of times shows where it went
// another way (test 23: many desyncs where only the seed differed).
static __thread uint32_t t_seedOut = 1;
static __thread bool t_seedOutInit = false;
static std::atomic<uint32_t> g_randOutside{0};
struct RandCall { uint32_t ret, s[3]; };
static std::vector<RandCall>* g_randCur = nullptr;   // this frame's calls (first run) or the list to match (replay)
static int g_randMode = 0;                            // 0 off, 1 first run, 2 replay
static uint32_t g_randIdx = 0;
static bool g_randBad = false;
static uint32_t g_randFrame = 0, g_randStep = 0;
static uint32_t g_randMissFrames = 0, g_randMissLogs = 0;
struct RandMissKey { uint32_t ex, got; bool operator==(const RandMissKey& o) const { return ex == o.ex && got == o.got; } };
static std::vector<std::pair<RandMissKey, uint32_t>> g_randMissPairs;

static inline bool IsCodeAddr(uint32_t a) { return a >= 0x00401000 && a < 0x0065C000; }
static void RandStack(RandCall& rc, const uint32_t* sp) {
    int n = 0;
    for (int i = 0; i < 96 && n < 3; i++) if (IsCodeAddr(sp[i])) rc.s[n++] = sp[i];
    for (; n < 3; n++) rc.s[n] = 0;
}
static void RandMiss(const RandCall* got) {
    g_randBad = true;
    g_randMissFrames++;
    const RandCall* ex = g_randIdx < g_randCur->size() ? &(*g_randCur)[g_randIdx] : nullptr;
    RandMissKey k{ex ? ex->ret : 0, got ? got->ret : 0};
    bool found = false;
    for (auto& e : g_randMissPairs) if (e.first == k) { e.second++; found = true; break; }
    if (!found && g_randMissPairs.size() < 200) g_randMissPairs.push_back({k, 1});
    if (g_randMissLogs++ < 40) {
        char a[80] = "none (first run had no more calls)", b[80] = "none (replay made fewer calls)";
        if (ex) snprintf(a, sizeof a, "0x%08X (callers 0x%08X 0x%08X 0x%08X)", ex->ret, ex->s[0], ex->s[1], ex->s[2]);
        if (got) snprintf(b, sizeof b, "0x%08X (callers 0x%08X 0x%08X 0x%08X)", got->ret, got->s[0], got->s[1], got->s[2]);
        Log("    rand: replay of frame %u (step %u) went another way at call %u of %u: first run %s, replay %s",
            g_randFrame, g_randStep, g_randIdx + 1, (unsigned)g_randCur->size(), a, b);
    }
}
static void RandLogStats() {
    Log("  rand: %u replayed frames called rand differently, %u calls on the game thread outside the update (own seed)",
        g_randMissFrames, g_randOutside.load());
    std::sort(g_randMissPairs.begin(), g_randMissPairs.end(),
              [](const std::pair<RandMissKey, uint32_t>& a, const std::pair<RandMissKey, uint32_t>& b) { return a.second > b.second; });
    for (size_t i = 0; i < g_randMissPairs.size() && i < 20; i++)
        Log("    first run 0x%08X / replay 0x%08X  %u times", g_randMissPairs[i].first.ex, g_randMissPairs[i].first.got,
            g_randMissPairs[i].second);
}
extern "C" int __attribute__((noinline)) __cdecl ModRand() {
    if (OnGameThread() && !g_inUpdate.load(std::memory_order_relaxed)) {
        g_randOutside++;
        if (!t_seedOutInit) { SeedInit(); t_seedOut = t_seed ^ 0x5EED1234u; t_seedOutInit = true; }
        t_seedOut = t_seedOut * 214013u + 2531011u;
        return static_cast<int>((t_seedOut >> 16) & 0x7FFF);
    }
    if (Watching()) {
        g_randCallsInUpdate++;
        if (g_randMode && g_randCur) {
            RandCall rc;
            // Entered through the stack-wiping stub: the game's return address is one slot further.
            rc.ret = *(const uint32_t*)((uint8_t*)__builtin_frame_address(0) + 8);
            RandStack(rc, (const uint32_t*)((uint8_t*)__builtin_frame_address(0) + 12));
            if (g_randMode == 1) g_randCur->push_back(rc);
            else {
                if (!g_randBad && (g_randIdx >= g_randCur->size() || (*g_randCur)[g_randIdx].ret != rc.ret)) RandMiss(&rc);
                g_randIdx++;
            }
        }
    }
    SeedInit();
    t_seed = t_seed * 214013u + 2531011u;
    return static_cast<int>((t_seed >> 16) & 0x7FFF);
}
static bool NetIgnoreSrand();
extern "C" void __cdecl ModSrand(unsigned s) {
    if (NetIgnoreSrand()) return;     // online: both games keep the seed the session gave them
    t_seedInit = true;
    t_seed = s;
}

// ---------------------------------------------------------------------------
// Memory chunks we watch: every 4 KB page of .data, plus (with heap=1) the
// game's live heap blocks cut into pieces of at most 4 KB.
// ---------------------------------------------------------------------------
struct Chunk {
    uint32_t addr;
    uint32_t len;     // <= 4096
    int32_t block;    // index into g_blocks, -1 for .data
};
struct Block {
    uint32_t start, size;
};
static std::vector<Chunk> g_chunks;
static std::vector<Block> g_blocks;              // sorted by start
static std::vector<uint8_t> g_blockDead;         // set when the game frees a block
static std::vector<uint32_t> g_blockFirst;       // build 49: first chunk of each block
static std::atomic<bool> g_blocksActive{false};  // free-hook checks enabled
static uint32_t g_firstHeapChunk = 0;
static uint64_t g_heapBytes = 0;

static int FindBlock(uint32_t p) {               // exact start match
    auto it = std::lower_bound(g_blocks.begin(), g_blocks.end(), p,
                               [](const Block& b, uint32_t v) { return b.start < v; });
    if (it != g_blocks.end() && it->start == p) return int(it - g_blocks.begin());
    return -1;
}

static bool InInputRanges(uint32_t a, uint32_t len) {
    for (auto& r : addr::INPUT_RANGES) if (a < r.end && r.start < a + len) return true;
    return false;
}
// Build 27: pages the state hunt added stay in the state for later tests in
// the same game session (test 26: they were found again after every depth
// change, and the frames before that desynced).
static std::vector<uint32_t> g_huntKept;
static bool InForceRanges(uint32_t a, uint32_t len) {
    for (auto& r : addr::FORCE_RANGES) if (a < r.end && r.start < a + len) return true;
    for (uint32_t p : g_huntKept) if (a < p + addr::PAGE && p < a + len) return true;
    return false;
}
static bool NeverRestore(uint32_t a) {
    for (uint32_t p : addr::NEVER_RESTORE_PAGES) if (a == p) return true;
    return false;
}

static void BuildChunks() {
    g_blocksActive = false;
    Sleep(20);   // let any free-hook that is mid-check finish before we rebuild the lists
    g_chunks.clear();
    g_blocks.clear();
    g_blockFirst.clear();
    for (uint32_t i = 0; i < addr::DATA_PAGES; i++) {
        uint32_t off = i * addr::PAGE;
        uint32_t len = std::min<uint32_t>(addr::PAGE, addr::DATA_SIZE - off);
        uint32_t a = addr::DATA_START + off;
        if (a == addr::PART_PAGE) g_chunks.push_back({addr::PART_RANGE.start, addr::PART_RANGE.end - addr::PART_RANGE.start, -1});
        else g_chunks.push_back({a, len, -1});
    }
    g_firstHeapChunk = (uint32_t)g_chunks.size();
    g_heapBytes = 0;
    HANDLE heap = CrtHeap();
    if (g_cfg.heap && heap) {
        HeapLock(heap);
        PROCESS_HEAP_ENTRY e{};
        while (HeapWalk(heap, &e)) {
            if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY) || e.cbData == 0) continue;
            g_blocks.push_back({(uint32_t)e.lpData, (uint32_t)e.cbData});
        }
        HeapUnlock(heap);
        std::sort(g_blocks.begin(), g_blocks.end(), [](const Block& a, const Block& b) { return a.start < b.start; });
        g_blockFirst.assign(g_blocks.size(), 0);
        for (uint32_t b = 0; b < g_blocks.size(); b++) {
            uint32_t s = g_blocks[b].start, n = g_blocks[b].size;
            g_heapBytes += n;
            g_blockFirst[b] = (uint32_t)g_chunks.size();
            for (uint32_t o = 0; o < n; o += addr::PAGE)
                g_chunks.push_back({s + o, std::min<uint32_t>(addr::PAGE, n - o), (int32_t)b});
        }
    }
    g_blockDead.assign(g_blocks.size(), 0);
    g_blocksActive = true;
    Log("watching %u .data pages and %u heap blocks (%.1f MB heap, %u chunks)",
        addr::DATA_PAGES, (unsigned)g_blocks.size(), g_heapBytes / 1048576.0,
        (unsigned)(g_chunks.size() - g_firstHeapChunk));
}

static inline bool ChunkAlive(const Chunk& c) { return c.block < 0 || !g_blockDead[c.block]; }

static inline uint32_t HashChunk(const Chunk& c) {
    if (c.len == addr::PAGE) return util::HashPage(reinterpret_cast<const void*>(c.addr));
    static uint8_t tmp[addr::PAGE];
    memset(tmp, 0, sizeof tmp);
    memcpy(tmp, reinterpret_cast<const void*>(c.addr), c.len);
    return util::HashPage(tmp);
}

// ---------------------------------------------------------------------------
// Clock record/replay. While replaying a frame, the game gets the same
// timeGetTime / QueryPerformanceCounter values it got the first time.
// ---------------------------------------------------------------------------
struct TimeRec {
    uint32_t nTime = 0, nQpc = 0;
    uint32_t time[64];
    int64_t qpc[64];
};
enum class Clock { Real, Record, Replay, Resim };
static Clock g_clock = Clock::Real;
static TimeRec* g_clockRec = nullptr;
static uint32_t g_clockIdxT = 0, g_clockIdxQ = 0, g_clockMiss = 0;

static void ClockRecord(TimeRec* r) { r->nTime = r->nQpc = 0; g_clockRec = r; g_clock = Clock::Record; }
static void ClockReplay(TimeRec* r) { g_clockRec = r; g_clockIdxT = g_clockIdxQ = 0; g_clock = Clock::Replay; }
static void ClockReal() { g_clock = Clock::Real; g_clockRec = nullptr; }
// Build 30: a resimulation (rollback with corrected inputs) gets the clock
// values the first run got where it can, and records what it used as the
// new truth for that frame.
static TimeRec* g_clockOld = nullptr;
static void ClockResim(TimeRec* rec, TimeRec* old) {
    g_clockOld = old; g_clockRec = rec; rec->nTime = rec->nQpc = 0;
    g_clockIdxT = g_clockIdxQ = 0; g_clock = Clock::Resim;
}

// ---------------------------------------------------------------------------
// Import hooks: clocks and heap.
// ---------------------------------------------------------------------------
struct CallSite { uint32_t ret; uint32_t count; };
struct SiteTable {
    CallSite e[128] = {};
    void Hit(uint32_t ret) {
        for (auto& s : e) {
            if (s.ret == ret) { s.count++; return; }
            if (s.ret == 0) { s.ret = ret; s.count = 1; return; }
        }
    }
    void Clear() { memset(e, 0, sizeof e); }
};
enum Api { A_timeGetTime, A_QPC, A_GetTickCount, A_HeapAlloc, A_HeapFree, A_HeapReAlloc, A_COUNT };
static const char* kApiNames[A_COUNT] = {"timeGetTime", "QueryPerformanceCounter", "GetTickCount",
                                         "HeapAlloc", "HeapFree", "HeapReAlloc"};
static SiteTable g_sites[A_COUNT];
static std::atomic<uint32_t> g_apiCount[A_COUNT];

// ---- Stack wiping around our hooks ----------------------------------------
// Test 13: the game copies a few bytes of leftover stack memory into its fight
// data. Our hooks use the stack differently in the first run (they call the
// real Windows / sound functions, which go deep) and in a replay (they answer
// from the journal), so that leftover memory differed and every frame looked
// out of sync. Every hook is now entered through a tiny stub that, after the
// hook returns, zeroes the stack area the hook (and whatever it called) used.
// Then the game sees the same leftover stack in both runs.
extern "C" {
void* g_wt_timeGetTime; void* g_wt_QPC; void* g_wt_GetTickCount;
void* g_wt_HeapAlloc; void* g_wt_HeapFree; void* g_wt_HeapReAlloc;
void* g_wt_CuePlay; void* g_wt_CueStop; void* g_wt_CueGetState; void* g_wt_CueDestroy;
void* g_wt_SbPrepare; void* g_wt_SbPlay; void* g_wt_Rand;
void* g_wt_CreateFileW; void* g_wt_CreateFileA; void* g_wt_CloseHandle;
void S_timeGetTime(); void S_QPC(); void S_GetTickCount();
void S_HeapAlloc(); void S_HeapFree(); void S_HeapReAlloc();
void S_CuePlay(); void S_CueStop(); void S_CueGetState(); void S_CueDestroy();
void S_SbPrepare(); void S_SbPlay(); void S_Rand();
void S_CreateFileW(); void S_CreateFileA(); void S_CloseHandle();
void wipe_stubs_begin(); void wipe_stubs_end();
void S_HitCheckEntry();
void* g_hitCheckCont = (void*)(addr::FN_HIT_CHECK + 5);
}
// Build 38: the two-object check 0x597C40 has a local struct (ebp+0x14,
// ebp = entry esp - 0x74) that is only partly filled in; 0x598DD0 copies 0x30
// bytes of it into the fight pool (+0x200 + i*0x30). Whatever was left on the
// stack ends up in the fight state: zero, or (after a page fault or another
// machine's stack) something else. Zero those 0x40 bytes at entry.
__asm__(".text\n.p2align 4\n.globl _S_HitCheckEntry\n_S_HitCheckEntry:\n"
        "\tpushl %edi\n\tpushl %ecx\n\tpushl %eax\n"
        "\tleal (12-0x60)(%esp), %edi\n"
        "\txorl %eax, %eax\n\tmovl $0x10, %ecx\n\tcld\n\trep stosl\n"
        "\tpopl %eax\n\tpopl %ecx\n\tpopl %edi\n"
        "\tpushl %ebp\n\tleal -0x70(%esp), %ebp\n"
        "\tjmp *_g_hitCheckCont\n");
// NAME: hook name, NARGS: stdcall argument count, WIPE: bytes to zero.
#define WIPE_STUB(NAME, NARGS, WIPE)                                          \
    ".p2align 4\n.globl _S_" #NAME "\n_S_" #NAME ":\n"                        \
    ".rept " #NARGS "\n\tpushl " #NARGS "*4(%esp)\n.endr\n"                   \
    "\tcall *_g_wt_" #NAME "\n"                                               \
    "\tmovl %eax, %edx\n"                                                     \
    "\tpushl %edi\n"                                                          \
    "\tleal -4(%esp), %edi\n"                                                 \
    "\tmovl $(" #WIPE "/4), %ecx\n"                                           \
    /* never write closer than 12 KB to the end of this thread's stack */     \
    "\tmovl %fs:0xE0C, %eax\n"                                                \
    "\taddl $0x3000, %eax\n"                                                  \
    "\tnegl %eax\n"                                                           \
    "\tleal 4(%edi,%eax), %eax\n"                                             \
    "\ttestl %eax, %eax\n"                                                    \
    "\tjg 1f\n"                                                               \
    "\txorl %eax, %eax\n"                                                     \
    "1:\tshrl $2, %eax\n"                                                     \
    "\tcmpl %ecx, %eax\n"                                                     \
    "\tcmovb %eax, %ecx\n"                                                    \
    "\txorl %eax, %eax\n"                                                     \
    "\tstd\n\trep stosl\n\tcld\n"                                             \
    "\tpopl %edi\n"                                                           \
    "\tmovl %edx, %eax\n"                                                     \
    "\txorl %edx, %edx\n"                                                     \
    "\tret $(" #NARGS "*4)\n"
__asm__(".text\n"
        ".p2align 4\n.globl _wipe_stubs_begin\n_wipe_stubs_begin:\n"
        WIPE_STUB(timeGetTime, 0, 2048)
        WIPE_STUB(QPC, 1, 2048)
        WIPE_STUB(GetTickCount, 0, 2048)
        WIPE_STUB(HeapAlloc, 3, 8192)
        WIPE_STUB(HeapFree, 3, 8192)
        WIPE_STUB(HeapReAlloc, 4, 8192)
        WIPE_STUB(CuePlay, 1, 16384)
        WIPE_STUB(CueStop, 2, 16384)
        WIPE_STUB(CueGetState, 2, 16384)
        WIPE_STUB(CueDestroy, 1, 16384)
        WIPE_STUB(SbPrepare, 5, 16384)
        WIPE_STUB(SbPlay, 5, 16384)
        WIPE_STUB(Rand, 0, 2048)
        WIPE_STUB(CreateFileW, 7, 8192)
        WIPE_STUB(CreateFileA, 7, 8192)
        WIPE_STUB(CloseHandle, 1, 4096)
        ".globl _wipe_stubs_end\n_wipe_stubs_end:\n"
        ".text\n");
static inline bool InWipeStub(uint32_t a) {
    return a >= (uint32_t)wipe_stubs_begin && a < (uint32_t)wipe_stubs_end;
}
// A hook reached through a stub sees the stub as its caller. The game's own
// return address sits just above the hook's arguments.
#define GAME_RET(NARGS) (*(uint32_t*)((uint8_t*)__builtin_frame_address(0) + 8 + 4 * (NARGS)))

// Caller of malloc (one level above the HeapAlloc call), for the log.
// Who asked for memory: the code that called operator new (or malloc),
// plus the function above it, folded into one number. Walks the game's
// EBP frame chain, staying inside this thread's stack.
static uint32_t __attribute__((noinline)) AllocSite() {
    uint32_t lo = __readfsdword(8), hi = __readfsdword(4);   // stack limit / base
    auto ok = [&](uint32_t* p) { return (uint32_t)p >= lo && (uint32_t)p + 8 <= hi; };
    uint32_t* ebp = static_cast<uint32_t*>(__builtin_frame_address(0));   // AllocSite
    if (!ok(ebp)) return 0;
    ebp = (uint32_t*)ebp[0];                                              // H_HeapAlloc
    uint32_t sig = 0;
    int got = 0;
    for (int depth = 0; depth < 8 && got < 2 && ok(ebp); depth++) {
        uint32_t ret = ebp[1];
        ebp = (uint32_t*)ebp[0];
        if (ret == addr::RET_IN_MALLOC || ret == addr::RET_IN_NEW) continue;   // inside malloc / new
        if (InWipeStub(ret)) continue;                                        // our stack-wipe stub
        sig = sig * 0x9E3779B1u + ret;
        got++;
    }
    return sig;
}

static uint32_t __attribute__((noinline)) MallocCaller(uint32_t ret) {
    if (ret != addr::RET_IN_MALLOC) return ret;
    // Our frame -> hook's frame -> malloc's frame (malloc uses EBP frames).
    uint32_t* hookEbp = static_cast<uint32_t*>(__builtin_frame_address(1));
    uint32_t* mallocEbp = reinterpret_cast<uint32_t*>(hookEbp[0]);
    return mallocEbp ? mallocEbp[1] : ret;
}

// Heap blocks allocated by the game logic during update and not freed yet.
struct LiveAlloc { uint32_t ptr, size, caller, frame; };
static std::vector<LiveAlloc> g_live;
static CRITICAL_SECTION g_liveLock;

static bool HuntReleaseRange(uint32_t a, uint32_t n);
static void NoteFree(uint32_t p) {
    if (g_blocksActive.load(std::memory_order_relaxed)) {
        int b = FindBlock(p);
        if (b >= 0) {
            g_blockDead[b] = 1;
            HuntReleaseRange(g_blocks[b].start, g_blocks[b].size);   // build 49: never keep a freed block read-only
        }
    }
    if (g_live.empty()) return;
    EnterCriticalSection(&g_liveLock);
    for (size_t i = 0; i < g_live.size(); i++)
        if (g_live[i].ptr == p) { g_live[i] = g_live.back(); g_live.pop_back(); break; }
    LeaveCriticalSection(&g_liveLock);
}

typedef DWORD(WINAPI* timeGetTime_t)();
typedef BOOL(WINAPI* QPC_t)(LARGE_INTEGER*);
typedef DWORD(WINAPI* GetTickCount_t)();
typedef LPVOID(WINAPI* HeapAlloc_t)(HANDLE, DWORD, SIZE_T);
typedef BOOL(WINAPI* HeapFree_t)(HANDLE, DWORD, LPVOID);
typedef LPVOID(WINAPI* HeapReAlloc_t)(HANDLE, DWORD, LPVOID, SIZE_T);
static timeGetTime_t o_timeGetTime;
static QPC_t o_QPC;
static GetTickCount_t o_GetTickCount;
static HeapAlloc_t o_HeapAlloc;
static HeapFree_t o_HeapFree;
static HeapReAlloc_t o_HeapReAlloc;

// Build 42: the date (time()) the game logic reads. Test 41 found one byte
// of the fight's replay header (the date in seconds) differ after a rewind,
// because the replay ran a second later. Game logic now gets a date that is
// fixed when each screen starts, so first runs and replays read the same.
typedef void(WINAPI* GSTAFT_t)(LPFILETIME);
static GSTAFT_t o_GSTAFT;
static FILETIME g_logicTime{};
static bool g_logicTimeSet = false;
static uint32_t g_logicTimeCalls = 0;
static void LatchLogicTime() { if (o_GSTAFT) { o_GSTAFT(&g_logicTime); g_logicTimeSet = true; } }
static void WINAPI H_GSTAFT(LPFILETIME ft) {
    if (!Watching()) { o_GSTAFT(ft); return; }
    if (!g_logicTimeSet) LatchLogicTime();
    g_logicTimeCalls++;
    *ft = g_logicTime;
}

static DWORD WINAPI H_timeGetTime() {
    if (!Watching()) return o_timeGetTime();
    g_apiCount[A_timeGetTime]++;
    g_sites[A_timeGetTime].Hit(GAME_RET(0));
    if (g_clock == Clock::Replay && g_clockRec) {
        if (g_clockIdxT < g_clockRec->nTime) return g_clockRec->time[g_clockIdxT++];
        g_clockMiss++;
    }
    if (g_clock == Clock::Resim && g_clockRec && g_clockOld) {
        DWORD v = g_clockIdxT < g_clockOld->nTime ? g_clockOld->time[g_clockIdxT++] : o_timeGetTime();
        if (g_clockRec->nTime < 64) g_clockRec->time[g_clockRec->nTime++] = v;
        return v;
    }
    DWORD v = o_timeGetTime();
    if (g_clock == Clock::Record && g_clockRec && g_clockRec->nTime < 64) g_clockRec->time[g_clockRec->nTime++] = v;
    return v;
}
static BOOL WINAPI H_QPC(LARGE_INTEGER* p) {
    if (!Watching()) return o_QPC(p);
    g_apiCount[A_QPC]++;
    g_sites[A_QPC].Hit(GAME_RET(1));
    if (g_clock == Clock::Replay && g_clockRec) {
        if (g_clockIdxQ < g_clockRec->nQpc) { p->QuadPart = g_clockRec->qpc[g_clockIdxQ++]; return TRUE; }
        g_clockMiss++;
    }
    if (g_clock == Clock::Resim && g_clockRec && g_clockOld) {
        BOOL ok = TRUE;
        if (g_clockIdxQ < g_clockOld->nQpc) p->QuadPart = g_clockOld->qpc[g_clockIdxQ++];
        else ok = o_QPC(p);
        if (g_clockRec->nQpc < 64) g_clockRec->qpc[g_clockRec->nQpc++] = p->QuadPart;
        return ok;
    }
    BOOL ok = o_QPC(p);
    if (g_clock == Clock::Record && g_clockRec && g_clockRec->nQpc < 64) g_clockRec->qpc[g_clockRec->nQpc++] = p->QuadPart;
    return ok;
}
static DWORD WINAPI H_GetTickCount() {
    if (Watching()) { g_apiCount[A_GetTickCount]++; g_sites[A_GetTickCount].Hit(GAME_RET(0)); }
    return o_GetTickCount();
}
// ---- Which heap blocks belong to the game ----------------------------------
// The heap is shared with every DLL (graphics, input, sound, Windows itself).
// Test 10 restored small blocks holding input-device names that the input
// DLL then freed and reused behind our back, which broke the heap. So we keep
// a list of every block the game's own code got (through its import table)
// since the mod loaded, and only ever restore those.
struct OwnSet {
    std::vector<uint32_t> slots;     // 0 = empty, 1 = deleted, else pointer
    uint32_t used = 0, live = 0;
    CRITICAL_SECTION lock;
    static uint32_t H(uint32_t p) { return (p >> 3) * 0x9E3779B1u; }
    void Grow() {
        std::vector<uint32_t> old;
        old.swap(slots);
        slots.assign(old.empty() ? (1u << 20) : old.size() * 2, 0);
        used = live = 0;
        for (uint32_t v : old) if (v > 1) InsertNoLock(v);
    }
    void InsertNoLock(uint32_t p) {
        if ((used + 1) * 10 > slots.size() * 6) Grow();
        uint32_t m = (uint32_t)slots.size() - 1, i = H(p) & m, tomb = 0xFFFFFFFF;
        while (slots[i]) {
            if (slots[i] == p) return;
            if (slots[i] == 1 && tomb == 0xFFFFFFFF) tomb = i;
            i = (i + 1) & m;
        }
        if (tomb != 0xFFFFFFFF) i = tomb; else used++;
        slots[i] = p;
        live++;
    }
    void Insert(uint32_t p) { EnterCriticalSection(&lock); InsertNoLock(p); LeaveCriticalSection(&lock); }
    void Erase(uint32_t p) {
        EnterCriticalSection(&lock);
        if (!slots.empty()) {
            uint32_t m = (uint32_t)slots.size() - 1, i = H(p) & m;
            while (slots[i]) {
                if (slots[i] == p) { slots[i] = 1; live--; break; }
                i = (i + 1) & m;
            }
        }
        LeaveCriticalSection(&lock);
    }
    bool Has(uint32_t p) {
        EnterCriticalSection(&lock);
        bool r = false;
        if (!slots.empty()) {
            uint32_t m = (uint32_t)slots.size() - 1, i = H(p) & m;
            while (slots[i]) { if (slots[i] == p) { r = true; break; } i = (i + 1) & m; }
        }
        LeaveCriticalSection(&lock);
        return r;
    }
};
static OwnSet g_own;

// ---- Heap journal (sync test only) ----------------------------------------
// Rewinding must never make the game free the same memory twice, or touch
// memory that is already gone. So while the sync test runs:
//   - when the game logic frees heap memory, we only note it ("logically
//     freed") and really free it once no rewind can reach back past that frame;
//   - when a replayed frame allocates memory, it gets the very same block it
//     got the first time (it still exists, since frees are delayed);
//   - blocks allocated during the test are saved/restored with the state.
struct AllocEv { uint32_t ptr, size, caller = 0; uint8_t used = 0; };
struct Journal {
    std::vector<AllocEv> allocs;
    std::vector<uint32_t> frees;
    std::vector<uint32_t> cueDestroys;                    // sound cues the game deleted this frame
    std::vector<std::pair<uint32_t, uint32_t>> cueStates; // (cue, state) answers given this frame
    uint32_t cueIdx = 0;
    struct CueMade { uint32_t idx, cue; HRESULT hr; uint8_t wantCue; };
    std::vector<CueMade> cuePlays;                        // cues the sound bank made this frame
    uint32_t cuePlayIdx = 0;
    std::vector<uint8_t> cuePlayUsed;                     // resimulation: which of them the new run took over
    // Build 29: files. A replay gets the handles the first run got, closing
    // a handle waits until no rewind can reach back past it, and a replayed
    // read starts at the same file position as the first run's read.
    std::vector<uint32_t> fileOpens;                      // handles CreateFile returned this frame
    uint32_t fileIdx = 0;
    std::vector<uint32_t> fileCloses;                     // handles closed this frame (closed for real later)
    std::vector<std::pair<uint32_t, int64_t>> fileReads;  // (handle, position before the read)
    uint32_t readIdx = 0;
    std::vector<uint8_t> fileOpenUsed;                    // resimulation: which opens the new run took over
};
// Resim (build 30): a rollback re-runs frames with corrected inputs. Each
// re-run frame takes over what the old run of that frame got when it asks
// for the same thing (same heap block, same sound cue, same file), gets new
// ones otherwise, and records everything as the new truth. What the old run
// had and the new one did not ask for is released afterwards.
enum class JMode { Off, Record, Replay, Resim };
static JMode g_jmode = JMode::Off;
static Journal* g_jcur = nullptr;
static Journal* g_jold = nullptr;            // resimulation: the old run's journal of this frame
static uint32_t g_jIdx = 0;
static uint32_t g_allocMiss = 0, g_reallocs = 0, g_released = 0;
static std::vector<AllocEv> g_dyn;          // blocks allocated during the test, logically alive
static std::vector<AllocEv> g_carry;        // g_dyn kept across a depth change
static CRITICAL_SECTION g_jLock;            // guards g_dyn and the journals (other threads free too)
static std::atomic<bool> g_jActive{false};  // sync test journal is running
static Journal* g_jOutside = nullptr;       // where frees outside the update go (current frame)

struct JLock { JLock() { EnterCriticalSection(&g_jLock); } ~JLock() { LeaveCriticalSection(&g_jLock); } };

// Journals of the frames a rewind can reach (set by the sync test / rollback).
static std::vector<Journal>* g_jAll = nullptr;
static bool FreePending(uint32_t p) {        // already logically freed in a frame we can still rewind to? (g_jLock held)
    if (!g_jAll) return false;
    for (auto& j : *g_jAll)
        for (uint32_t q : j.frees) if (q == p) return true;
    return false;
}
static bool DynContains(uint32_t p) {
    for (auto& b : g_dyn) if (b.ptr == p) return true;
    return false;
}

static void DynRemove(uint32_t p) {
    for (size_t i = 0; i < g_dyn.size(); i++)
        if (g_dyn[i].ptr == p) { g_dyn[i] = g_dyn.back(); g_dyn.pop_back(); return; }
}

// ---- Sound cues (XACT) ---------------------------------------------------
// Test 11: the game's sound manager keeps a table of playing sound "cues".
// Each frame it asks every cue "are you finished?" and deletes finished ones.
// A rewind put back the table from before the delete, so the replay asked a
// deleted cue and deleted it a second time, which broke the heap. Now, while
// the sync test runs:
//   - deleting a cue is postponed until no rewind can reach back to it
//     (the same trick as for heap memory);
//   - during a replay, "play" and "stop" do nothing (no more doubled sounds)
//     and "are you finished?" gets the same answer as the first time.
// The sound library itself is not changed; we only sit in front of four of
// its functions (Play, Stop, GetState, Destroy of a cue).
typedef HRESULT(WINAPI* CuePlay_t)(void*);
typedef HRESULT(WINAPI* CueStop_t)(void*, DWORD);
typedef HRESULT(WINAPI* CueGetState_t)(void*, DWORD*);
typedef HRESULT(WINAPI* CueDestroy_t)(void*);
static CuePlay_t o_CuePlay;
static CueStop_t o_CueStop;
static CueGetState_t o_CueGetState;
static CueDestroy_t o_CueDestroy;
static uint32_t g_cueVt = 0;
static std::vector<uint32_t> g_cuePending;   // cues whose delete is postponed (guarded by g_jLock)
static uint32_t g_cueDeferred = 0, g_cueReplayCalls = 0, g_cueStateMiss = 0;

static bool CuePending(uint32_t c) {
    for (uint32_t x : g_cuePending) if (x == c) return true;
    return false;
}
static void CueRelease(uint32_t c) {         // really delete (called with g_jLock held)
    for (size_t i = 0; i < g_cuePending.size(); i++)
        if (g_cuePending[i] == c) { g_cuePending[i] = g_cuePending.back(); g_cuePending.pop_back(); break; }
    o_CueDestroy((void*)c);
}
// Test 16: the sound library's own Play calls its Prepare (and the cue's
// functions) through the same function tables we sit in front of. Those
// inner calls were recorded too, so the first run's list had every sound
// twice and replays drifted out of step. Calls made from inside the sound
// library are now passed straight through.
static __thread int t_inXact = 0;
static std::vector<uint32_t> g_resimNewCues;   // cues a resimulation made itself (g_jLock)
static inline bool CueReplaying() {
    if (!Watching()) return false;
    if (g_jmode == JMode::Replay) return true;
    return false;
}
static bool CueOldRun(void* cue) {          // resim: Play/Stop of a cue the old run already played
    if (!Watching() || g_jmode != JMode::Resim) return false;
    JLock lock;
    return std::find(g_resimNewCues.begin(), g_resimNewCues.end(), (uint32_t)cue) == g_resimNewCues.end();
}

static HRESULT WINAPI H_CuePlay(void* cue) {
    if (t_inXact) return o_CuePlay(cue);
    if (CueReplaying() || CueOldRun(cue)) { g_cueReplayCalls++; return S_OK; }
    return o_CuePlay(cue);
}
static HRESULT WINAPI H_CueStop(void* cue, DWORD flags) {
    if (t_inXact) return o_CueStop(cue, flags);
    if (CueReplaying() || CueOldRun(cue)) { g_cueReplayCalls++; return S_OK; }
    return o_CueStop(cue, flags);
}
static HRESULT WINAPI H_CueGetState(void* cue, DWORD* st) {
    if (t_inXact) return o_CueGetState(cue, st);
    if (Watching() && g_jcur && g_jmode != JMode::Off) {
        JLock lock;
        Journal& j = *g_jcur;
        if (g_jmode == JMode::Replay) {
            if (j.cueIdx < j.cueStates.size() && j.cueStates[j.cueIdx].first == (uint32_t)cue) {
                if (st) *st = j.cueStates[j.cueIdx].second;
                j.cueIdx++;
                return S_OK;
            }
            if (g_cueStateMiss++ < 20)
                Log("    sound: replay asked the state of cue 0x%08X (answer %u of %u; first run asked 0x%08X) at frame %u",
                    (uint32_t)cue, j.cueIdx, (unsigned)j.cueStates.size(),
                    j.cueIdx < j.cueStates.size() ? j.cueStates[j.cueIdx].first : 0, g_frame);
        }
        if (g_jmode == JMode::Resim && g_jold) {
            Journal& o = *g_jold;
            if (o.cueIdx < o.cueStates.size() && o.cueStates[o.cueIdx].first == (uint32_t)cue) {
                DWORD v = o.cueStates[o.cueIdx++].second;
                if (st) *st = v;
                j.cueStates.push_back({(uint32_t)cue, v});
                return S_OK;
            }
        }
        HRESULT hr = o_CueGetState(cue, st);
        if ((g_jmode == JMode::Record || g_jmode == JMode::Resim) && st) j.cueStates.push_back({(uint32_t)cue, *st});
        return hr;
    }
    return o_CueGetState(cue, st);
}
static uint32_t g_cueReplayDestroys = 0;
static HRESULT WINAPI H_CueDestroy(void* cue) {
    if (t_inXact) return o_CueDestroy(cue);
    // Test 14 crash: a replay that went a different way deleted a cue the
    // real run still used; the real delete happened a frame later and the
    // real run then touched the deleted cue. Deletes in a replay are now
    // ignored, like frees in a replay: only the real run's deletes count.
    if (CueReplaying()) { g_cueReplayDestroys++; return S_OK; }
    if (g_jActive.load()) {
        JLock lock;
        uint32_t c = (uint32_t)cue;
        if (CuePending(c)) return S_OK;                  // already deleted once (a replay)
        Journal* j = Watching() ? g_jcur : g_jOutside;
        if (j) {
            j->cueDestroys.push_back(c);
            g_cuePending.push_back(c);
            g_cueDeferred++;
            return S_OK;
        }
    }
    return o_CueDestroy(cue);
}

// Test 13: replays still asked the sound bank to start sounds, which made
// brand new cues (doubled sounds, and different cue pointers in the game's
// cue table than in the first run). Now, while the sync test runs, the first
// run's answers are recorded and a replay gets the same cue back without
// anything being played again. Play and Prepare share one argument list.
typedef HRESULT(WINAPI* SbPlay_t)(void*, DWORD, DWORD, LONG, void**);
static SbPlay_t o_SbPrepare, o_SbPlay;
static uint32_t g_sbVt = 0, g_sbReplayed = 0, g_sbMiss = 0, g_sbRecorded = 0;
static HRESULT SbCommon(SbPlay_t real, void* sb, DWORD idx, DWORD fl, LONG off, void** pp) {
    if (t_inXact) return real(sb, idx, fl, off, pp);
    if (Watching() && g_jcur && g_jmode != JMode::Off) {
        uint32_t key = idx & 0xFFFF;
        if (g_jmode == JMode::Replay) {
            JLock lock;
            Journal& j = *g_jcur;
            if (j.cuePlayIdx < j.cuePlays.size() && j.cuePlays[j.cuePlayIdx].idx == key &&
                j.cuePlays[j.cuePlayIdx].wantCue == (pp != nullptr)) {
                auto& r = j.cuePlays[j.cuePlayIdx++];
                if (pp) *pp = (void*)r.cue;
                g_sbReplayed++;
                return r.hr;
            }
            if (g_sbMiss++ < 20)
                Log("    sound: replay started sound %u%s (start %u of %u; first run started %u) at frame %u", key,
                    pp ? "" : " (no cue)", j.cuePlayIdx, (unsigned)j.cuePlays.size(),
                    j.cuePlayIdx < j.cuePlays.size() ? j.cuePlays[j.cuePlayIdx].idx : 0xFFFF, g_frame);
            // The replay went a different way: play for real (counted in the log).
        }
        if (g_jmode == JMode::Resim && g_jold) {
            JLock lock;
            Journal& o = *g_jold;
            if (o.cuePlayUsed.size() != o.cuePlays.size()) o.cuePlayUsed.assign(o.cuePlays.size(), 0);
            if (o.cuePlayIdx < o.cuePlays.size() && o.cuePlays[o.cuePlayIdx].idx == key &&
                o.cuePlays[o.cuePlayIdx].wantCue == (pp != nullptr)) {
                o.cuePlayUsed[o.cuePlayIdx] = 1;
                auto r = o.cuePlays[o.cuePlayIdx++];
                if (pp) *pp = (void*)r.cue;
                g_jcur->cuePlays.push_back(r);
                return r.hr;
            }
        }
        t_inXact++;
        HRESULT hr = real(sb, idx, fl, off, pp);
        t_inXact--;
        if (g_jmode == JMode::Record || g_jmode == JMode::Resim) {
            JLock lock;
            g_jcur->cuePlays.push_back({key, pp ? (uint32_t)*pp : 0, hr, (uint8_t)(pp != nullptr)});
            if (g_jmode == JMode::Resim && pp && *pp) g_resimNewCues.push_back((uint32_t)*pp);
            g_sbRecorded++;
        }
        return hr;
    }
    return real(sb, idx, fl, off, pp);
}
static HRESULT WINAPI H_SbPrepare(void* sb, DWORD idx, DWORD fl, LONG off, void** pp) {
    return SbCommon(o_SbPrepare, sb, idx, fl, off, pp);
}
static HRESULT WINAPI H_SbPlay(void* sb, DWORD idx, DWORD fl, LONG off, void** pp) {
    return SbCommon(o_SbPlay, sb, idx, fl, off, pp);
}
static bool SafeRead32(uint32_t a, uint32_t* out);
static bool ModuleNameHas(uint32_t code, const wchar_t* part, wchar_t* name, size_t n) {
    HMODULE m = nullptr;
    name[0] = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)code, &m) || !m)
        return false;
    GetModuleFileNameW(m, name, (DWORD)n);
    for (wchar_t* q = name; *q; q++) *q = towlower(*q);
    return wcsstr(name, part) != nullptr;
}
static void HookSoundBank() {
    if (g_sbVt) return;
    for (uint32_t w : addr::G_SOUNDBANK_WRAPS) {
        uint32_t sb, vt, f4;
        wchar_t name[MAX_PATH];
        if (!SafeRead32(w, &sb) || !sb || !SafeRead32(sb, &vt) || !SafeRead32(vt + 16, &f4)) continue;
        if (!ModuleNameHas(f4, L"xact", name, MAX_PATH)) continue;
        uint32_t* tab = (uint32_t*)vt;
        DWORD old;
        if (!VirtualProtect(tab + 3, 8, PAGE_READWRITE, &old)) continue;
        o_SbPrepare = (SbPlay_t)tab[3];
        o_SbPlay = (SbPlay_t)tab[4];
        g_wt_SbPrepare = (void*)H_SbPrepare;
        g_wt_SbPlay = (void*)H_SbPlay;
        tab[3] = (uint32_t)S_SbPrepare;
        tab[4] = (uint32_t)S_SbPlay;
        VirtualProtect(tab + 3, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        g_sbVt = vt;
        Log("sound bank hooked (bank 0x%08X, function table 0x%08X)", sb, vt);
        return;
    }
    Log("sound bank NOT hooked (no sound bank found); replays may still start sounds");
}

// Find the cue function table from any live cue in the sound manager's
// tables, and put our four functions in front of the library's.
static bool SafeRead32(uint32_t a, uint32_t* out) {
    MEMORY_BASIC_INFORMATION mi{};
    if (!a || !VirtualQuery((void*)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    if (a + 4 > (uint32_t)mi.BaseAddress + mi.RegionSize) return false;
    *out = *(uint32_t*)a;
    return true;
}
static bool HookCues() {
    if (g_cueVt) return true;
    std::vector<uint32_t> cands;
    for (uint32_t pl = 0; pl < 2; pl++)
        for (uint32_t i = 0; i < 0xC98; i++)
            for (uint32_t k = 0; k < 2; k++) {
                uint32_t v = *(uint32_t*)(addr::G_CUE_TABLE + pl * 0x64C0 + i * 8 + k * 4);
                if (v) cands.push_back(v);
            }
    for (uint32_t g : addr::G_CUE_SINGLES) { uint32_t v = *(uint32_t*)g; if (v) cands.push_back(v); }
    for (uint32_t c : cands) {
        uint32_t vt, f0;
        if (!SafeRead32(c, &vt) || !SafeRead32(vt, &f0)) continue;
        HMODULE m = nullptr;
        wchar_t name[MAX_PATH] = L"";
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)f0, &m) || !m)
            continue;
        GetModuleFileNameW(m, name, MAX_PATH);
        for (wchar_t* q = name; *q; q++) *q = towlower(*q);
        if (!wcsstr(name, L"xact")) continue;
        uint32_t* tab = (uint32_t*)vt;
        DWORD old;
        if (!VirtualProtect(tab, 16, PAGE_READWRITE, &old)) continue;
        o_CuePlay = (CuePlay_t)tab[0];
        o_CueStop = (CueStop_t)tab[1];
        o_CueGetState = (CueGetState_t)tab[2];
        o_CueDestroy = (CueDestroy_t)tab[3];
        g_wt_CuePlay = (void*)H_CuePlay;
        g_wt_CueStop = (void*)H_CueStop;
        g_wt_CueGetState = (void*)H_CueGetState;
        g_wt_CueDestroy = (void*)H_CueDestroy;
        tab[0] = (uint32_t)S_CuePlay;
        tab[1] = (uint32_t)S_CueStop;
        tab[2] = (uint32_t)S_CueGetState;
        tab[3] = (uint32_t)S_CueDestroy;
        VirtualProtect(tab, 16, old, &old);
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        g_cueVt = vt;
        wchar_t* b = wcsrchr(name, L'\\');
        Log("sound cues hooked (cue 0x%08X, function table 0x%08X in %ls)", c, vt, b ? b + 1 : name);
        HookSoundBank();
        return true;
    }
    return false;
}

static void LogSoundStats() {
    Log("  sound: %u cue deletes postponed, %u replay deletes ignored, %u play/stop calls skipped in replays, %u state answers not matched%s",
        g_cueDeferred, g_cueReplayDestroys, g_cueReplayCalls, g_cueStateMiss, g_cueVt ? "" : " (cues were never hooked)");
    Log("  sound bank: %u sounds started in first runs, %u replays given the same cue, %u replays went a different way%s",
        g_sbRecorded, g_sbReplayed, g_sbMiss, g_sbVt ? "" : " (sound bank was never hooked)");
}

// ---- Files (build 29) -------------------------------------------------------
typedef HANDLE(WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL(WINAPI* CloseHandle_t)(HANDLE);
static CreateFileW_t o_CreateFileW;
static CreateFileA_t o_CreateFileA;
static CloseHandle_t o_CloseHandle;
static std::vector<uint32_t> g_fileDyn;      // handles opened by the game logic during the test, not really closed yet (g_jLock)
static std::vector<uint32_t> g_fileTouched;  // handles the game logic read during the test (g_jLock)
static uint32_t g_fileOpens = 0, g_fileReplayed = 0, g_fileMiss = 0, g_fileCloseDeferred = 0, g_fileClosed = 0;
static uint32_t g_fileReads = 0, g_fileReadsReplayed = 0, g_fileReadMiss = 0;
static inline bool VecHas(const std::vector<uint32_t>& v, uint32_t x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}
static inline void VecDel(std::vector<uint32_t>& v, uint32_t x) {
    auto it = std::find(v.begin(), v.end(), x);
    if (it != v.end()) v.erase(it);
}
static void CloseForReal(uint32_t h) {      // called with g_jLock held
    VecDel(g_fileDyn, h);
    VecDel(g_fileTouched, h);
    o_CloseHandle((HANDLE)h);
    g_fileClosed++;
}

static void ReleaseFrees(Journal& j) {      // really free what the game freed earlier
    JLock lock;
    for (uint32_t h : j.fileCloses) CloseForReal(h);
    j.fileCloses.clear();
    j.fileOpens.clear();
    j.fileIdx = 0;
    j.fileReads.clear();
    j.readIdx = 0;
    HANDLE heap = CrtHeap();
    for (uint32_t p : j.frees) {
        if (g_blocksActive.load(std::memory_order_relaxed)) {
            int b = FindBlock(p);
            if (b >= 0) g_blockDead[b] = 2;     // 2 = released by us, safe
        }
        g_own.Erase(p);
        o_HeapFree(heap, 0, (LPVOID)p);
        g_released++;
    }
    j.frees.clear();
    j.allocs.clear();
    for (uint32_t c : j.cueDestroys) CueRelease(c);
    j.cueDestroys.clear();
    j.cueStates.clear();
    j.cueIdx = 0;
    j.cuePlays.clear();
    j.cuePlayIdx = 0;
}


static inline LPVOID Own(HANDLE h, LPVOID p) {
    if (p && h == CrtHeap()) g_own.Insert((uint32_t)p);
    return p;
}
static LPVOID WINAPI H_HeapAlloc(HANDLE h, DWORD f, SIZE_T n) {
    if (!(Watching() && h == CrtHeap())) return Own(h, o_HeapAlloc(h, f, n));
    uint32_t caller = MallocCaller(GAME_RET(3));
    g_apiCount[A_HeapAlloc]++;
    g_sites[A_HeapAlloc].Hit(caller);
    LPVOID p = nullptr;
    if (g_jmode == JMode::Replay && g_jcur) {
        JLock lock;
        // Hand back the block the original run got, but only to the same
        // code asking for the same size. If the replay asks from somewhere
        // else (e.g. the sound system, which is not rewound), it gets fresh
        // memory, so two owners never share one block (test 7 crash).
        uint32_t site = AllocSite();
        auto& v = g_jcur->allocs;
        size_t k = g_jIdx;
        while (k < v.size() && (v[k].used || v[k].size != (uint32_t)n || v[k].caller != site)) k++;
        if (k < v.size()) {
            v[k].used = 1;
            while (g_jIdx < v.size() && v[g_jIdx].used) g_jIdx++;
            p = (LPVOID)v[k].ptr;
            if (f & HEAP_ZERO_MEMORY) memset(p, 0, n);
            g_dyn.push_back({(uint32_t)p, (uint32_t)n});
            return p;
        }
        g_allocMiss++;
        return Own(h, o_HeapAlloc(h, f, n));         // replay went a different way; just allocate
    }
    if (g_jmode == JMode::Resim && g_jcur && g_jold) {
        JLock lock;
        uint32_t site = AllocSite();
        auto& v = g_jold->allocs;
        size_t k = g_jIdx;
        while (k < v.size() && (v[k].used || v[k].size != (uint32_t)n || v[k].caller != site)) k++;
        if (k < v.size()) {
            v[k].used = 1;
            while (g_jIdx < v.size() && v[g_jIdx].used) g_jIdx++;
            p = (LPVOID)v[k].ptr;
            if (f & HEAP_ZERO_MEMORY) memset(p, 0, n);
        } else {
            p = Own(h, o_HeapAlloc(h, f, n));
        }
        if (p) {
            g_jcur->allocs.push_back({(uint32_t)p, (uint32_t)n, site, 0});
            g_dyn.push_back({(uint32_t)p, (uint32_t)n});
        }
        return p;
    }
    p = Own(h, o_HeapAlloc(h, f, n));
    if (p && g_jmode == JMode::Record && g_jcur) {
        JLock lock;
        g_jcur->allocs.push_back({(uint32_t)p, (uint32_t)n, AllocSite(), 0});
        g_dyn.push_back({(uint32_t)p, (uint32_t)n});
    }
    if (p && g_jmode == JMode::Off) {
        EnterCriticalSection(&g_liveLock);
        if (g_live.size() < 4096) g_live.push_back({(uint32_t)p, (uint32_t)n, caller, g_frame});
        LeaveCriticalSection(&g_liveLock);
    }
    return p;
}
static __thread uint32_t t_lastFree = 0, t_lastFreeSite = 0;
static BOOL WINAPI H_HeapFree(HANDLE h, DWORD f, LPVOID p) {
    if (p && h == CrtHeap()) {
        t_lastFree = (uint32_t)p;
        t_lastFreeSite = MallocCaller(GAME_RET(3));
        if (Watching()) {
            g_apiCount[A_HeapFree]++;
            g_sites[A_HeapFree].Hit(t_lastFreeSite);
            if ((g_jmode == JMode::Record || g_jmode == JMode::Resim) && g_jcur) {   // delay the real free
                JLock lock;
                if (!FreePending((uint32_t)p)) g_jcur->frees.push_back((uint32_t)p);
                DynRemove((uint32_t)p);
                return TRUE;
            }
            if (g_jmode == JMode::Replay) {             // already freed in the original run
                JLock lock;
                DynRemove((uint32_t)p);
                return TRUE;
            }
        }
        // Freed outside the update (drawing, end-of-frame code, other threads):
        // if it's a block the game got during the test, our saved states may
        // still contain it, so it must stay alive until no rewind can reach it.
        if (g_jActive.load()) {
            JLock lock;
            if (g_jOutside && DynContains((uint32_t)p)) {
                DynRemove((uint32_t)p);
                if (!FreePending((uint32_t)p)) g_jOutside->frees.push_back((uint32_t)p);
                return TRUE;
            }
        }
        NoteFree((uint32_t)p);      // before the real free, so we never touch freed memory
    }
    if (p && h == CrtHeap()) g_own.Erase((uint32_t)p);
    return o_HeapFree(h, f, p);
}
static LPVOID WINAPI H_HeapReAlloc(HANDLE h, DWORD f, LPVOID p, SIZE_T n) {
    if (p && h == CrtHeap()) {
        if (Watching()) {
            g_apiCount[A_HeapReAlloc]++;
            g_sites[A_HeapReAlloc].Hit(GAME_RET(4));
            if (g_jmode != JMode::Off) g_reallocs++;   // not handled yet; reported and test stops
        }
        NoteFree((uint32_t)p);
        g_own.Erase((uint32_t)p);
        LPVOID q = o_HeapReAlloc(h, f, p, n);
        g_own.Insert((uint32_t)(q ? q : p));   // on failure the old block stays
        return q;
    }
    return o_HeapReAlloc(h, f, p, n);
}

static void LogApiSites(const char* title, uint32_t frames) {
    Log("%s (%u frames)", title, frames);
    for (int a = 0; a < A_COUNT; a++) {
        Log("  %-24s %u calls during update", kApiNames[a], g_apiCount[a].load());
        for (auto& s : g_sites[a].e)
            if (s.ret) Log("      from 0x%08X  x%u", s.ret, s.count);
    }
    Log("  %-24s %u calls during update", "rand", g_randCallsInUpdate.load());
    EnterCriticalSection(&g_liveLock);
    Log("  heap blocks allocated during update and still alive: %u", (unsigned)g_live.size());
    // group by caller
    std::vector<LiveAlloc> v = g_live;
    LeaveCriticalSection(&g_liveLock);
    std::sort(v.begin(), v.end(), [](const LiveAlloc& a, const LiveAlloc& b) { return a.caller < b.caller; });
    for (size_t i = 0; i < v.size();) {
        size_t j = i; uint64_t bytes = 0;
        while (j < v.size() && v[j].caller == v[i].caller) bytes += v[j++].size;
        Log("      allocated from 0x%08X: %u blocks, %llu bytes (first at frame %u)", v[i].caller,
            (unsigned)(j - i), (unsigned long long)bytes, v[i].frame);
        i = j;
    }
}
static void ClearApiSites() {
    for (int a = 0; a < A_COUNT; a++) { g_sites[a].Clear(); g_apiCount[a] = 0; }
    g_randCallsInUpdate = 0;
    EnterCriticalSection(&g_liveLock);
    g_live.clear();
    LeaveCriticalSection(&g_liveLock);
}

// ---------------------------------------------------------------------------
// Profile: which chunks change, and when.
//   U = changed during the scene update
//   O = changed between updates (drawing, other code, other threads)
// Heap chunks are hashed in slices when the heap is large, so the game
// stays playable; each slice is still sampled many times.
// ---------------------------------------------------------------------------
static std::vector<uint32_t> g_hBefore, g_hAfter;
static std::vector<uint16_t> g_cU, g_cO;
static std::vector<uint8_t> g_cW;               // per chunk: who wrote state bytes in it (W_*)
// Build 34: which part of the fight update writes which memory. For every
// write fault into fight memory on the game thread, the "part" is the call
// site inside the fight update function 0x432F30 (it calls stage, characters,
// effects, HUD, ... one after another) found on the stack.
static std::vector<uint32_t> g_cWn;              // per chunk: write faults during the update
struct WPart { uint32_t region, site, faults, chunks; uint8_t cls; };
static WPart g_wPart[4096];
static uint32_t g_wPartN = 0;
static uint32_t g_wPartLost = 0;
static std::vector<uint32_t> g_cWtid;           // per chunk: last other thread that wrote it
static uint32_t g_profFrames = 0, g_heapSlices = 1;
static bool g_haveProfile = false;

static void HashPass(std::vector<uint32_t>& out, uint32_t slice) {
    for (uint32_t i = 0; i < g_firstHeapChunk; i++) out[i] = HashChunk(g_chunks[i]);
    HANDLE heap = CrtHeap();
    if (g_firstHeapChunk == g_chunks.size()) return;
    HeapLock(heap);    // nobody can free a block while we read it
    for (uint32_t i = g_firstHeapChunk + slice; i < g_chunks.size(); i += g_heapSlices)
        if (ChunkAlive(g_chunks[i])) out[i] = HashChunk(g_chunks[i]);
    HeapUnlock(heap);
}

static void ProfileStart() {
    g_cW.clear();
    BuildChunks();
    size_t n = g_chunks.size();
    g_hBefore.assign(n, 0);
    g_hAfter.assign(n, 0);
    g_cU.assign(n, 0);
    g_cO.assign(n, 0);
    g_heapSlices = (uint32_t)(g_heapBytes / (24u << 20)) + 1;   // ~24 MB of heap per frame
    g_profFrames = 0;
    ClearApiSites();
    g_mode = Mode::Profiling;
    Log("PROFILE start (%d frames, heap in %u slices). Play normally during a fight.", g_cfg.profileFrames, g_heapSlices);
}

static void WriteProfileFile() {
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%lsrollback_profile.txt", util::ModDir());
    FILE* f = _wfopen(path, L"w");
    uint32_t nU = 0, nO = 0, nB = 0, hU = 0, hO = 0, hB = 0;
    uint64_t hUbytes = 0;
    for (uint32_t i = 0; i < g_chunks.size(); i++) {
        bool u = g_cU[i] != 0, o = g_cO[i] != 0;
        if (i < g_firstHeapChunk) { nU += u; nO += o; nB += u && o; }
        else { hU += u; hO += o; hB += u && o; if (u) hUbytes += g_chunks[i].len; }
    }
    if (f) {
        fprintf(f, "Profile build 2 over %u frames.\n", g_profFrames);
        fprintf(f, "kind: U = changed during update, O = changed outside update, UO = both\n\n");
        fprintf(f, ".data pages (0x%08X..):\n", addr::DATA_START);
        uint32_t i = 0;
        auto kindOf = [](uint32_t k) { return (g_cU[k] ? 1 : 0) | (g_cO[k] ? 2 : 0); };
        while (i < g_firstHeapChunk) {
            int kind = kindOf(i);
            if (!kind) { i++; continue; }
            uint32_t j = i, maxU = 0, maxO = 0;
            while (j < g_firstHeapChunk && kindOf(j) == kind) {
                maxU = std::max<uint32_t>(maxU, g_cU[j]); maxO = std::max<uint32_t>(maxO, g_cO[j]); j++;
            }
            fprintf(f, "%-2s 0x%08X-0x%08X %5u pages  U=%u O=%u%s\n", kind == 1 ? "U" : kind == 2 ? "O" : "UO",
                    g_chunks[i].addr, g_chunks[j - 1].addr + g_chunks[j - 1].len, j - i, maxU, maxO,
                    InInputRanges(g_chunks[i].addr, addr::PAGE) ? "  (input)" : "");
            i = j;
        }
        fprintf(f, "\n.data totals: U=%u pages, O=%u, both=%u\n", nU, nO, nB);
        fprintf(f, "\nHeap: %u blocks, %.1f MB. Changed chunks: U=%u (%.2f MB), O=%u, both=%u\n",
                (unsigned)g_blocks.size(), g_heapBytes / 1048576.0, hU, hUbytes / 1048576.0, hO, hB);
        fprintf(f, "Heap blocks with changes (start, size, changed chunks U/O):\n");
        uint32_t listed = 0;
        for (uint32_t b = 0, c = g_firstHeapChunk; b < g_blocks.size(); b++) {
            uint32_t u = 0, o = 0;
            uint32_t end = g_blocks[b].start + g_blocks[b].size;
            for (; c < g_chunks.size() && g_chunks[c].addr < end; c++) { u += g_cU[c] != 0; o += g_cO[c] != 0; }
            if ((u || o) && listed++ < 400)
                fprintf(f, "  0x%08X %8u bytes  U=%u O=%u%s\n", g_blocks[b].start, g_blocks[b].size, u, o,
                        g_blockDead[b] ? " (freed)" : "");
        }
        if (listed > 400) fprintf(f, "  ... %u more\n", listed - 400);
        fclose(f);
    }
    Log("PROFILE done: %u frames. .data U=%u O=%u both=%u pages. Heap %.1f MB in %u blocks: U=%u chunks (%.2f MB), O=%u, both=%u",
        g_profFrames, nU, nO, nB, g_heapBytes / 1048576.0, (unsigned)g_blocks.size(), hU, hUbytes / 1048576.0, hO, hB);
    LogApiSites("Calls made by the game logic during update", g_profFrames);
}

// The game copies some uninitialised stack bytes into its state (test 11:
// five 4-byte fields that held whatever our own code left on the stack).
// Clearing the stack area the update will use makes those bytes the same in
// the first run and in every replay.
static void __attribute__((noinline)) ClearStackBelow() {
    volatile uint8_t pad[48 * 1024];
    memset((void*)pad, 0, sizeof pad);
    __asm__ __volatile__("" ::: "memory");
}
// Test 24: a few fields in the 108800-byte fight block got a value that was
// left in a CPU register by our own code (for example "1 - first frame" from
// the replay loop). Some game code reads a register it never set. So the
// update is now always entered with the same register values (all zero),
// and the caller's registers are kept in globals, not on the stack.
// Build 31: the update (and the input step) also always run at the same
// stack address. The game keeps some stack addresses in its fight data
// (test 30: bytes like F3 vs F7 in a pool, 1 KB apart), and the rollback
// calls the update from different depths of our own code than the first
// run did. g_fixedEsp is a spot a bit below where the game calls us each
// frame; when set, the call switches to it.
extern "C" {
uint32_t g_cuFn, g_cuEbx, g_cuEsi, g_cuEdi, g_cuEbp, g_cuEsp, g_fixedEsp;
uint32_t CallUpdateClean(void* scene);
}
__asm__(".text\n.p2align 4\n.globl _CallUpdateClean\n_CallUpdateClean:\n"
        "\tmovl 4(%esp), %ecx\n"          // scene (thiscall: in ecx)
        "\tmovl %ebx, _g_cuEbx\n"
        "\tmovl %esi, _g_cuEsi\n"
        "\tmovl %edi, _g_cuEdi\n"
        "\tmovl %ebp, _g_cuEbp\n"
        "\tmovl %esp, _g_cuEsp\n"
        "\tmovl _g_fixedEsp, %eax\n"
        "\ttestl %eax, %eax\n"
        "\tjz 1f\n"
        "\tmovl %eax, %esp\n"
        "1:\n"
        "\txorl %eax, %eax\n\txorl %edx, %edx\n\txorl %ebx, %ebx\n"
        "\txorl %esi, %esi\n\txorl %edi, %edi\n\txorl %ebp, %ebp\n"
        "\tcall *_g_cuFn\n"
        "\tmovl _g_cuEsp, %esp\n"
        "\tmovl _g_cuEbx, %ebx\n"
        "\tmovl _g_cuEsi, %esi\n"
        "\tmovl _g_cuEdi, %edi\n"
        "\tmovl _g_cuEbp, %ebp\n"
        "\txorl %edx, %edx\n\txorl %ecx, %ecx\n"
        "\tret\n");
static bool g_fixedBad = false;
// Zero the stack the call will use, at the fixed spot if we have one.
static inline void PrepareStack() {
    uint32_t here = (uint32_t)__builtin_frame_address(0);
    if (g_fixedEsp && !g_fixedBad && here > g_fixedEsp + 0x400) {
        memset((void*)(g_fixedEsp - 0xC000), 0, 0xC000);
        return;
    }
    if (g_fixedEsp && !g_fixedBad) {
        g_fixedBad = true;
        Log("stack: our code is deeper than the fixed spot (0x%08X vs 0x%08X); fixed stack turned off", here, g_fixedEsp);
        g_fixedEsp = 0;
    }
    ClearStackBelow();
}
static inline uint32_t RunUpdate(void* scene) {
    PrepareStack();
    g_cuFn = addr::FN_SCENE_UPDATE;
    g_inUpdate = true;
    uint32_t r = CallUpdateClean(scene);
    g_inUpdate = false;
    return r;
}
// The input step (cdecl, no arguments) through the same fixed stack.
static inline void RunInputStep() {
    PrepareStack();
    g_cuFn = addr::FN_INPUT;
    CallUpdateClean(nullptr);
}
// Called at the top of every hooked update: picks the fixed spot once and
// makes sure the stack pages down there exist (Windows grows a thread's
// stack one page at a time, in order).
static void __attribute__((noinline)) CommitStack128() {
    volatile uint8_t pad[128 * 1024];
    memset((void*)pad, 0, sizeof pad);
    __asm__ __volatile__("" ::: "memory");
}
static void PickFixedStack(uint32_t hookFrame) {
    if (g_fixedEsp || g_fixedBad) return;
    CommitStack128();
    g_fixedEsp = (hookFrame - 0x6000) & ~0xFu;
    Log("stack: update and input step run at fixed stack 0x%08X", g_fixedEsp);
}

static void DescribeAddr(uint32_t a, char* out, size_t n);

// ---------------------------------------------------------------------------
// Writer tracking (runs automatically for a few seconds after the F9 profile).
// Test 5 showed fight memory changing while the sync test was mid-rewind, so
// something other than the game logic writes it at the same time. To find
// out who, we make the fight memory pages read-only for a moment: the next
// write to such a page raises an exception, we note which thread and which
// game code did it, make the page writable again and let the write happen.
// A helper thread keeps re-arming the pages every few milliseconds.
// Nothing is changed in how the game behaves, it just runs slower meanwhile.
// ---------------------------------------------------------------------------
enum : uint8_t { W_UPDATE = 1, W_GAME_OUTSIDE = 2, W_THREAD = 4, W_FOREIGN = 8 };
// W_FOREIGN: written on the game thread by code outside the game's exe and
// outside ntdll (graphics driver, d3d9, sound, Steam overlay, this mod...).
static uint32_t g_ntdllLo = 0, g_ntdllHi = 0;
static inline bool ForeignCode(uint32_t eip) {
    if (eip >= 0x00401000 && eip < 0x0065C000) return false;      // the game's own code
    if (eip >= g_ntdllLo && eip < g_ntdllHi) return false;         // heap / memset helpers
    return true;
}
struct WSet {                                   // never freed: the exception handler may still read it
    std::vector<uint32_t> pages;                // sorted page addresses we arm
    std::vector<DWORD> prot;                    // their original protection
    std::vector<uint8_t> skip;                  // 1 = never arm again (a file read needed it)
    std::vector<std::pair<uint32_t, uint32_t>> heapU;   // (start, chunk index) of heap U chunks, sorted
};
static std::atomic<WSet*> g_wset{nullptr};
static std::atomic<bool> g_wArm{false};         // helper thread keeps arming pages
static std::atomic<bool> g_wRec{false};         // faults are being recorded
static std::atomic<int> g_wArming{0};           // helper thread is inside an arming pass
static uint32_t g_wFrames = 0;
static std::atomic<uint32_t> g_wFaults{0}, g_wReadRetries{0};

struct WEntry {
    uint32_t eip, tid, count, inState, sample;
    uint8_t cls;
    uint32_t stack[8];
};
static WEntry g_wTab[1024];
static std::atomic<uint32_t> g_wTabN{0};
static std::atomic_flag g_wSpin = ATOMIC_FLAG_INIT;
struct WThread { uint32_t tid, start, crtStart; };
static WThread g_wThreads[128];
static uint32_t g_wThreadN = 0;

typedef LONG(NTAPI* NtQIT_t)(HANDLE, int, PVOID, ULONG, PULONG);
static NtQIT_t p_NtQIT = nullptr;
typedef PVOID(WINAPI* FlsGetValue_t)(DWORD);
static FlsGetValue_t p_FlsGetValue = nullptr;

static int WFindPage(const WSet* s, uint32_t pg) {
    auto it = std::lower_bound(s->pages.begin(), s->pages.end(), pg);
    return (it != s->pages.end() && *it == pg) ? int(it - s->pages.begin()) : -1;
}
// Chunk index of a profile "U" chunk containing address a, or -1.
static int WStateChunk(const WSet* s, uint32_t a) {
    if (a >= addr::DATA_START && a < addr::DATA_START + addr::DATA_SIZE) {
        uint32_t i = (a - addr::DATA_START) / addr::PAGE;
        return (i < g_cU.size() && g_cU[i]) ? int(i) : -1;
    }
    auto it = std::upper_bound(s->heapU.begin(), s->heapU.end(), std::make_pair(a, 0xFFFFFFFFu));
    if (it == s->heapU.begin()) return -1;
    --it;
    const Chunk& c = g_chunks[it->second];
    return (a < c.addr + c.len) ? int(it->second) : -1;
}

static void WNoteThread(uint32_t tid) {     // called with g_wSpin held
    for (uint32_t i = 0; i < g_wThreadN; i++) if (g_wThreads[i].tid == tid) return;
    if (g_wThreadN >= 128) return;
    uint32_t start = 0, crt = 0;
    if (p_NtQIT) p_NtQIT(GetCurrentThread(), 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof start, nullptr);
    if (start == addr::FN_THREADSTARTEX) {               // created with the game's _beginthreadex: real function is in its CRT data
        uint32_t* ptd = p_FlsGetValue ? (uint32_t*)p_FlsGetValue(*reinterpret_cast<DWORD*>(addr::G_CRT_FLS_INDEX)) : nullptr;
        if (ptd) crt = ptd[0x54 / 4];
    }
    g_wThreads[g_wThreadN++] = {tid, start, crt};
}

static LONG CALLBACK WriterHandler(EXCEPTION_POINTERS* e) {
    EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    WSet* s = g_wset.load();
    if (!s) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t a = (uint32_t)r->ExceptionInformation[1];
    int pi = WFindPage(s, a & ~(addr::PAGE - 1));
    if (pi < 0) return EXCEPTION_CONTINUE_SEARCH;     // not one of ours: a real crash
    DWORD old;
    VirtualProtect((void*)s->pages[pi], addr::PAGE, s->prot[pi], &old);
    g_wFaults++;
    uint32_t tid = GetCurrentThreadId();
    uint8_t cls = tid != g_gameTid ? W_THREAD : g_inUpdate.load(std::memory_order_relaxed) ? W_UPDATE : W_GAME_OUTSIDE;
    if (!g_wRec.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_EXECUTION;   // late fault after the check ended
    int ci = WStateChunk(s, a);
    if (ci >= 0 && (size_t)ci < g_cW.size()) {
        g_cW[ci] |= cls;
        if (cls != W_THREAD && ForeignCode(e->ContextRecord->Eip)) g_cW[ci] |= W_FOREIGN;
        if (cls == W_THREAD) g_cWtid[ci] = tid;
    }
    uint32_t eip = e->ContextRecord->Eip;
    uint32_t part = 0;
    if (ci >= 0 && cls != W_THREAD) {
        // First return address inside the fight update (0x432F30..0x433817);
        // otherwise the first one in the scene update chain, else the deepest seen.
        uint32_t* sp = (uint32_t*)e->ContextRecord->Esp;
        uint32_t* top = (uint32_t*)__readfsdword(4);
        uint32_t last = 0, scene = 0;
        for (int i = 0; i < 4096 && sp + i < top; i++) {
            uint32_t v = sp[i];
            if (v < 0x401000 || v >= 0x65C000) continue;
            if (v > 0x432F30 && v <= 0x433817) { part = v; break; }
            if (!scene && v > 0x5AA000 && v < 0x5AA400) scene = v | 0x80000000u;
            last = v;
        }
        if (!part) part = scene ? scene : (last ? last | 0x40000000u : eip | 0x20000000u);
    }
    while (g_wSpin.test_and_set(std::memory_order_acquire)) {}
    if (ci >= 0 && cls != W_THREAD && (size_t)ci < g_cWn.size()) {
        const Chunk& c = g_chunks[ci];
        uint32_t region = c.block >= 0 ? 0x10000000u + (uint32_t)c.block : (uint32_t)ci;
        bool firstHere = g_cWn[ci]++ == 0;
        uint32_t k = 0;
        for (; k < g_wPartN; k++) if (g_wPart[k].region == region && g_wPart[k].site == part && g_wPart[k].cls == cls) break;
        if (k == g_wPartN) {
            if (g_wPartN < 4096) { g_wPart[g_wPartN] = {region, part, 0, 0, cls}; g_wPartN++; }
            else { g_wPartLost++; k = 4096; }
        }
        if (k < 4096) { g_wPart[k].faults++; if (firstHere) g_wPart[k].chunks++; }
    }
    if (cls == W_THREAD) WNoteThread(tid);
    uint32_t n = g_wTabN.load(), k = 0;
    uint32_t key = cls == W_THREAD ? tid : 0;
    for (; k < n; k++) if (g_wTab[k].eip == eip && g_wTab[k].tid == key && g_wTab[k].cls == cls) break;
    if (k == n && n < 1024) {
        WEntry& w = g_wTab[n];
        memset(&w, 0, sizeof w);
        w.eip = eip; w.tid = key; w.cls = cls; w.sample = a;
        uint32_t* sp = (uint32_t*)e->ContextRecord->Esp;
        uint32_t* top = (uint32_t*)__readfsdword(4);   // this thread's stack end (TIB StackBase)
        int m = 0;
        for (int i = 0; i < 400 && sp + i < top && m < 8; i++) {
            uint32_t v = sp[i];
            if (v >= 0x401000 && v < 0x65C000) w.stack[m++] = v;
        }
        g_wTabN = n + 1;
    }
    if (k < 1024 && k <= n) {
        g_wTab[k].count++;
        if (ci >= 0) { if (!g_wTab[k].inState) g_wTab[k].sample = a; g_wTab[k].inState++; }
    }
    g_wSpin.clear(std::memory_order_release);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void WArmAll() {
    WSet* s = g_wset.load();
    if (!s) return;
    g_wArming++;
    DWORD old;
    for (size_t i = 0; i < s->pages.size() && g_wArm.load(); i++)
        if (!s->skip[i]) VirtualProtect((void*)s->pages[i], addr::PAGE, PAGE_READONLY, &old);
    g_wArming--;
}
static void WDisarmAll() {
    g_wRec = false;
    g_wArm = false;
    WSet* s = g_wset.load();
    if (!s) return;
    for (int pass = 0; pass < 2; pass++) {      // twice, in case an arming pass was just starting
        while (g_wArming.load()) Sleep(1);
        DWORD old;
        for (size_t i = 0; i < s->pages.size(); i++) VirtualProtect((void*)s->pages[i], addr::PAGE, s->prot[i], &old);
        Sleep(20);
    }
}
static DWORD WINAPI WArmThread(LPVOID) {
    while (g_wArm.load()) { WArmAll(); Sleep(10); }
    return 0;
}

// File reads into an armed page fail instead of raising an exception; if
// that happens, free the pages of that buffer for good and read again.
typedef BOOL(WINAPI* ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static ReadFile_t o_ReadFile;
static bool HuntReleaseRange(uint32_t a, uint32_t n);

// ---- File hooks (build 29) --------------------------------------------------
static inline bool FileRec() { return Watching() && g_jcur && (g_jmode == JMode::Record || g_jmode == JMode::Resim); }
static inline bool FileRep() { return Watching() && g_jcur && g_jmode == JMode::Replay; }
static bool FileReplayOpen(HANDLE* out) {
    if (Watching() && g_jcur && g_jold && g_jmode == JMode::Resim) {
        JLock lock;
        Journal& o = *g_jold;
        if (o.fileOpenUsed.size() != o.fileOpens.size()) o.fileOpenUsed.assign(o.fileOpens.size(), 0);
        if (o.fileIdx < o.fileOpens.size()) {
            o.fileOpenUsed[o.fileIdx] = 1;
            *out = (HANDLE)o.fileOpens[o.fileIdx++];
            g_jcur->fileOpens.push_back((uint32_t)*out);
            return true;
        }
        return false;
    }
    if (!FileRep()) return false;
    JLock lock;
    Journal& j = *g_jcur;
    if (j.fileIdx < j.fileOpens.size()) {
        *out = (HANDLE)j.fileOpens[j.fileIdx++];
        g_fileReplayed++;
        return true;
    }
    g_fileMiss++;          // the replay opens a file the first run did not: just open it
    return false;
}
static HANDLE FileNoteOpen(HANDLE h) {
    if (FileRec()) {
        JLock lock;
        g_jcur->fileOpens.push_back((uint32_t)h);
        g_fileOpens++;
        if (h != INVALID_HANDLE_VALUE && h) g_fileDyn.push_back((uint32_t)h);
    }
    return h;
}
static HANDLE WINAPI H_CreateFileW(LPCWSTR n, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t) {
    HANDLE h;
    if (FileReplayOpen(&h)) return h;
    return FileNoteOpen(o_CreateFileW(n, a, sh, sa, cd, fl, t));
}
static HANDLE WINAPI H_CreateFileA(LPCSTR n, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t) {
    HANDLE h;
    if (FileReplayOpen(&h)) return h;
    return FileNoteOpen(o_CreateFileA(n, a, sh, sa, cd, fl, t));
}
static BOOL WINAPI H_CloseHandle(HANDLE h) {
    if (g_jActive.load()) {
        if (FileRec()) {                 // the game logic closes: really close it later
            JLock lock;
            g_jcur->fileCloses.push_back((uint32_t)h);
            g_fileCloseDeferred++;
            return TRUE;
        }
        if (FileRep()) return TRUE;      // already closed in the first run
        JLock lock;                      // other code closing a handle a saved state may still use
        if (g_jOutside && VecHas(g_fileDyn, (uint32_t)h) && !VecHas(g_jOutside->fileCloses, (uint32_t)h)) {
            g_jOutside->fileCloses.push_back((uint32_t)h);
            g_fileCloseDeferred++;
            return TRUE;
        }
    }
    return o_CloseHandle(h);
}
static int64_t FilePos(uint32_t h) {
    LARGE_INTEGER z, pos;
    z.QuadPart = 0;
    if (!SetFilePointerEx((HANDLE)h, z, &pos, FILE_CURRENT)) return -1;
    return pos.QuadPart;
}
static void FileBeforeRead(HANDLE h, LPOVERLAPPED ov) {
    if (ov) return;
    if (FileRec()) {
        if (g_jmode == JMode::Resim && g_jold) {
            JLock lock;
            Journal& o = *g_jold;
            if (o.readIdx < o.fileReads.size() && o.fileReads[o.readIdx].first == (uint32_t)h && o.fileReads[o.readIdx].second >= 0) {
                LARGE_INTEGER li;
                li.QuadPart = o.fileReads[o.readIdx].second;
                SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
            }
            if (o.readIdx < o.fileReads.size()) o.readIdx++;
        }
        int64_t p = FilePos((uint32_t)h);
        JLock lock;
        g_jcur->fileReads.push_back({(uint32_t)h, p});
        g_fileReads++;
        if (p >= 0 && !VecHas(g_fileTouched, (uint32_t)h)) g_fileTouched.push_back((uint32_t)h);
    } else if (FileRep()) {
        JLock lock;
        Journal& j = *g_jcur;
        if (j.readIdx < j.fileReads.size() && j.fileReads[j.readIdx].first == (uint32_t)h) {
            int64_t p = j.fileReads[j.readIdx].second;
            if (p >= 0) {
                LARGE_INTEGER li;
                li.QuadPart = p;
                SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
                g_fileReadsReplayed++;
            }
            j.readIdx++;
        } else {
            g_fileReadMiss++;
        }
    }
}
// The rewind must not move the files the game is reading: we note where
// each one stood before the rewind and put it back after the replays.
static std::vector<std::pair<uint32_t, int64_t>> g_filePosSaved;
static void FilePositionsSave() {
    JLock lock;
    g_filePosSaved.clear();
    for (uint32_t h : g_fileTouched) g_filePosSaved.push_back({h, FilePos(h)});
}
static void FilePositionsRestore() {
    JLock lock;
    for (auto& e : g_filePosSaved) {
        if (e.second < 0 || !VecHas(g_fileTouched, e.first)) continue;
        LARGE_INTEGER li;
        li.QuadPart = e.second;
        SetFilePointerEx((HANDLE)e.first, li, nullptr, FILE_BEGIN);
    }
}

static void DtTouchRange(uint32_t a, uint32_t n);
static BOOL WINAPI H_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    FileBeforeRead(h, ov);
    DtTouchRange((uint32_t)buf, n);
    BOOL ok = o_ReadFile(h, buf, n, got, ov);
    if (!ok && n && GetLastError() == ERROR_NOACCESS && HuntReleaseRange((uint32_t)buf, n)) {
        g_wReadRetries++;
        return o_ReadFile(h, buf, n, got, ov);
    }
    WSet* s = g_wset.load();
    if (ok || !s || GetLastError() != ERROR_NOACCESS || !n) return ok;
    bool hit = false;
    for (uint32_t pg = (uint32_t)buf & ~(addr::PAGE - 1); pg < (uint32_t)buf + n; pg += addr::PAGE) {
        int pi = WFindPage(s, pg);
        if (pi < 0) continue;
        s->skip[pi] = 1;
        DWORD old;
        VirtualProtect((void*)pg, addr::PAGE, s->prot[pi], &old);
        hit = true;
    }
    if (!hit) { SetLastError(ERROR_NOACCESS); return ok; }
    g_wReadRetries++;
    return o_ReadFile(h, buf, n, got, ov);
}

static bool WritersStart() {
    WSet* s = new WSet();
    std::vector<uint32_t> pages;
    // Watch .data pages the update changes, and every page of each heap
    // block the update changes (whole blocks are what the sync test restores).
    std::vector<uint8_t> uBlock(g_blocks.size(), 0);
    // Only heap blocks the game itself allocated are watched: the sync test
    // never restores any other block, and watching memory that belongs to
    // Windows or a driver can make their internal calls fail (tests 9, 12).
    uint32_t skippedForeign = 0;
    for (uint32_t i = g_firstHeapChunk; i < g_chunks.size(); i++) {
        uint32_t b = g_chunks[i].block;
        if (!g_cU[i] || uBlock[b] || g_blocks[b].size > (32u << 20)) continue;
        if (g_own.Has(g_blocks[b].start)) uBlock[b] = 1; else { uBlock[b] = 2; skippedForeign++; }
    }
    Log("  writer check skips %u heap blocks that are not the game's own memory", skippedForeign);
    for (uint32_t i = 0; i < g_chunks.size(); i++) {
        bool want = g_chunks[i].block < 0 ? g_cU[i] != 0 : uBlock[g_chunks[i].block] == 1;
        if (!want || !ChunkAlive(g_chunks[i])) continue;
        const Chunk& c = g_chunks[i];
        if (c.block >= 0) s->heapU.push_back({c.addr, i});
        if (c.block >= 0 && !g_cU[i]) {
            // Rest of a block the update uses. Only for big blocks (the game's
            // own pools), and only pages that lie fully inside the block:
            // small blocks and block edges can be shared with Windows code
            // that receives data from the system, and arming those made
            // Windows' own messaging fail in test 9.
            const Block& b = g_blocks[c.block];
            if (b.size < (64u << 10)) continue;
            for (uint32_t pg = c.addr & ~(addr::PAGE - 1); pg < c.addr + c.len; pg += addr::PAGE)
                if (pg >= b.start && pg + addr::PAGE <= b.start + b.size) pages.push_back(pg);
            continue;
        }
        for (uint32_t pg = c.addr & ~(addr::PAGE - 1); pg < c.addr + c.len; pg += addr::PAGE) pages.push_back(pg);
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    std::sort(s->heapU.begin(), s->heapU.end());
    for (uint32_t pg : pages) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery((void*)pg, &mi, sizeof mi) || mi.State != MEM_COMMIT) continue;
        DWORD p = mi.Protect & 0xFF;
        if (p != PAGE_READWRITE && p != PAGE_WRITECOPY) continue;   // only plain writable pages
        if (mi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) continue;
        s->pages.push_back(pg);
        s->prot.push_back(mi.Protect);
    }
    s->skip.assign(s->pages.size(), 0);
    if (s->pages.empty()) { Log("WRITER CHECK skipped: no fight memory pages found"); return false; }
    g_cW.assign(g_chunks.size(), 0);
    g_cWn.assign(g_chunks.size(), 0);
    g_wPartN = 0;
    g_wPartLost = 0;
    g_cWtid.assign(g_chunks.size(), 0);
    g_wTabN = 0;
    g_wThreadN = 0;
    g_wFaults = 0;
    g_wReadRetries = 0;
    g_wFrames = 0;
    g_wset = s;
    g_wRec = true;
    g_wArm = true;
    CreateThread(nullptr, 0, WArmThread, nullptr, 0, nullptr);
    g_mode = Mode::Writers;
    Log("WRITER CHECK start: %u pages for %d frames. Keep playing.", (unsigned)s->pages.size(), g_cfg.writerFrames);
    return true;
}

// Build 34: fight memory by region (heap block or .data page run) and by
// the part of the fight update that writes it.
static void WritePartsReport(FILE* f) {
    struct R { uint32_t region, faults, chunks, total; };
    std::vector<R> regs;
    auto regionOf = [](uint32_t i) { const Chunk& c = g_chunks[i]; return c.block >= 0 ? 0x10000000u + (uint32_t)c.block : i; };
    std::map<uint32_t, R> m;
    for (uint32_t i = 0; i < g_cWn.size(); i++) {
        bool inState = g_chunks[i].block >= 0 ? true : g_cU[i] != 0;
        if (!inState && !g_cWn[i]) continue;
        uint32_t r = regionOf(i);
        if (g_chunks[i].block >= 0 && !g_cWn[i]) {
            // count block size once, below
        }
        R& x = m[r];
        x.region = r;
        x.faults += g_cWn[i];
        if (g_cWn[i]) x.chunks++;
        if (g_chunks[i].block < 0) x.total++;
    }
    for (auto& kv : m) {
        R x = kv.second;
        if (x.region >= 0x10000000u) x.total = (g_blocks[x.region - 0x10000000u].size + addr::PAGE - 1) / addr::PAGE;
        if (!x.faults) continue;
        regs.push_back(x);
    }
    std::sort(regs.begin(), regs.end(), [](const R& a, const R& b) { return a.faults > b.faults; });
    uint32_t frames = g_wFrames ? g_wFrames : 1;
    uint64_t allFaults = 0;
    for (auto& x : regs) allFaults += x.faults;
    fprintf(f, "\nFIGHT MEMORY BY PART OF THE UPDATE (build 34). %u frames, %.1f pages written per frame on average (state pages first written in a frame)%s\n",
            g_wFrames, double(allFaults) / frames, g_wPartLost ? ", table full: some lost" : "");
    fprintf(f, "part = return address inside the fight update 0x432F30 (S:xxx = scene update, D:xxx = other caller, E:xxx = code address)\n");
    for (auto& x : regs) {
        if (x.region >= 0x10000000u) {
            const Block& b = g_blocks[x.region - 0x10000000u];
            fprintf(f, "REGION heap block 0x%08X %u bytes: %u of %u pages written, %.2f pages/frame\n", b.start, b.size, x.chunks, x.total, double(x.faults) / frames);
        } else {
            fprintf(f, "REGION .data page 0x%08X: %.2f writes/frame\n", g_chunks[x.region].addr & ~(addr::PAGE - 1), double(x.faults) / frames);
        }
        std::vector<const WPart*> ps;
        for (uint32_t k = 0; k < g_wPartN; k++) if (g_wPart[k].region == x.region) ps.push_back(&g_wPart[k]);
        std::sort(ps.begin(), ps.end(), [](const WPart* a, const WPart* b) { return a->faults > b->faults; });
        for (size_t j = 0; j < ps.size() && j < 6; j++) {
            uint32_t s2 = ps[j]->site;
            const char* pre = (s2 & 0x80000000u) ? "S:" : (s2 & 0x40000000u) ? "D:" : (s2 & 0x20000000u) ? "E:" : "";
            fprintf(f, "    %s%08X %-7s %8u faults %6u pages\n", pre, s2 & 0x1FFFFFFFu, ps[j]->cls == W_UPDATE ? "update" : "outside", ps[j]->faults, ps[j]->chunks);
        }
    }
    // Totals per part (all regions), so the stage part can be spotted.
    std::map<uint64_t, std::pair<uint32_t, uint32_t>> per;
    for (uint32_t k = 0; k < g_wPartN; k++) {
        auto& p2 = per[(uint64_t(g_wPart[k].site) << 8) | g_wPart[k].cls];
        p2.first += g_wPart[k].faults; p2.second += g_wPart[k].chunks;
    }
    fprintf(f, "\nPARTS (all regions):\n");
    for (auto& kv : per) {
        uint32_t s2 = uint32_t(kv.first >> 8);
        const char* pre = (s2 & 0x80000000u) ? "S:" : (s2 & 0x40000000u) ? "D:" : (s2 & 0x20000000u) ? "E:" : "";
        fprintf(f, "PART %s%08X %-7s %8u faults %6u pages\n", pre, s2 & 0x1FFFFFFFu, (kv.first & 0xFF) == W_UPDATE ? "update" : "outside", kv.second.first, kv.second.second);
    }
}

static void WritersReport() {
    WDisarmAll();
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%lsrollback_profile.txt", util::ModDir());
    FILE* f = _wfopen(path, L"a");
    uint32_t n = g_wTabN.load();
    std::vector<uint32_t> order(n);
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return g_wTab[a].count > g_wTab[b].count; });
    uint32_t chunksBy[8] = {};
    uint32_t foreignChunks = 0;
    for (uint32_t i = 0; i < g_cW.size(); i++) {
        if (g_cU[i]) chunksBy[g_cW[i] & 7]++;
        if (g_cW[i] & W_FOREIGN) foreignChunks++;
    }
    char where[160];
    auto blockOf = [](uint32_t a, char* out, size_t len) {
        auto it = std::upper_bound(g_blocks.begin(), g_blocks.end(), a, [](uint32_t v, const Block& b) { return v < b.start; });
        if (it != g_blocks.begin() && a < (it - 1)->start + (it - 1)->size)
            snprintf(out, len, " (heap block 0x%08X+0x%X)", (it - 1)->start, a - (it - 1)->start);
        else if (a >= addr::DATA_START && a < addr::DATA_START + addr::DATA_SIZE) snprintf(out, len, " (.data)");
        else out[0] = 0;
    };
    if (f) {
        fprintf(f, "\n\nWRITER CHECK over %u frames: %u page faults, %u file reads retried\n", g_wFrames,
                g_wFaults.load(), g_wReadRetries.load());
        fprintf(f, "Fight memory chunks (from the profile above) by who wrote them:\n");
        const char* names[8] = {"nobody seen", "update only", "game thread outside update only", "update + game thread outside",
                                "other threads only", "update + other threads", "outside + other threads", "all three"};
        for (int m = 0; m < 8; m++) fprintf(f, "  %-34s %u\n", names[m], chunksBy[m]);
        fprintf(f, "Heap chunks written on the game thread by code outside the game (driver, sound, ...): %u\n", foreignChunks);
        fprintf(f, "\nThreads other than the game thread that wrote watched pages:\n");
        for (uint32_t i = 0; i < g_wThreadN; i++) {
            DescribeAddr(g_wThreads[i].start, where, sizeof where);
            fprintf(f, "  thread %u start %s", g_wThreads[i].tid, where);
            if (g_wThreads[i].crtStart) { DescribeAddr(g_wThreads[i].crtStart, where, sizeof where); fprintf(f, " -> %s", where); }
            fprintf(f, "\n");
        }
        fprintf(f, "\nWriters (code address, who, writes, writes into fight memory, sample address, callers):\n");
        for (uint32_t k = 0; k < n && k < 150; k++) {
            const WEntry& w = g_wTab[order[k]];
            DescribeAddr(w.eip, where, sizeof where);
            char blk[64];
            blockOf(w.sample, blk, sizeof blk);
            fprintf(f, "  %-40s %-8s", where, w.cls == W_UPDATE ? "update" : w.cls == W_GAME_OUTSIDE ? "outside" : "thread");
            if (w.cls == W_THREAD) fprintf(f, " %5u", w.tid); else fprintf(f, "      ");
            fprintf(f, " %8u %8u  0x%08X%s  |", w.count, w.inState, w.sample, blk);
            for (int i = 0; i < 8 && w.stack[i]; i++) fprintf(f, " %08X", w.stack[i]);
            fprintf(f, "\n");
        }
        WritePartsReport(f);
        fprintf(f, "\nFight memory chunks written by other threads:\n");
        uint32_t listed = 0;
        for (uint32_t i = 0; i < g_cW.size() && listed < 200; i++) {
            if (!(g_cW[i] & W_THREAD)) continue;
            char blk[64];
            blockOf(g_chunks[i].addr, blk, sizeof blk);
            fprintf(f, "  0x%08X +%u%s  mask=%u  thread %u\n", g_chunks[i].addr, g_chunks[i].len, blk, g_cW[i], g_cWtid[i]);
            listed++;
        }
        fclose(f);
    }
    Log("WRITER CHECK done: %u frames, %u faults, %u writers. Fight chunks: update only %u, +outside %u, +other threads %u, other threads only %u",
        g_wFrames, g_wFaults.load(), n, chunksBy[W_UPDATE], chunksBy[W_UPDATE | W_GAME_OUTSIDE],
        chunksBy[5] + chunksBy[7], chunksBy[W_THREAD] + chunksBy[6]);
}

static void ProfileFinish() {
    g_haveProfile = true;
    g_mode = Mode::Idle;
    SetStatus("profile saved (%u frames)", g_profFrames);
}

static void WritersStop() {
    WritersReport();
    ProfileFinish();
}

static uint32_t WritersFrame(void* scene) {
    WArmAll();      // so the first write of this update to each page is seen too
    uint32_t r = RunUpdate(scene);
    g_wFrames++;
    SetStatus("PROFILING (writer check) %u/%d", g_wFrames, g_cfg.writerFrames);
    if ((int)g_wFrames >= g_cfg.writerFrames) WritersStop();
    return r;
}

static void ProfileStop() {
    WriteProfileFile();
    if (g_cfg.writerFrames > 0 && WritersStart()) return;
    ProfileFinish();
}

static uint32_t ProfileFrame(void* scene) {
    uint32_t slice = g_profFrames % g_heapSlices;
    HashPass(g_hBefore, slice);
    if (g_profFrames >= g_heapSlices) {
        for (uint32_t i = 0; i < g_chunks.size(); i++) {
            if (i >= g_firstHeapChunk && (i - g_firstHeapChunk) % g_heapSlices != slice) continue;
            if (g_hBefore[i] != g_hAfter[i] && g_cO[i] < 0xFFFF) g_cO[i]++;
        }
    }
    g_inUpdate = true;
    uint32_t r = OrigSceneUpdate(scene);
    g_inUpdate = false;

    HashPass(g_hAfter, slice);
    for (uint32_t i = 0; i < g_chunks.size(); i++) {
        if (i >= g_firstHeapChunk && (i - g_firstHeapChunk) % g_heapSlices != slice) continue;
        if (g_hBefore[i] != g_hAfter[i] && g_cU[i] < 0xFFFF) g_cU[i]++;
    }
    g_profFrames++;
    SetStatus("PROFILING %u/%d", g_profFrames, g_cfg.profileFrames);
    if ((int)g_profFrames >= g_cfg.profileFrames) ProfileStop();
    return r;
}

// ---------------------------------------------------------------------------
// Sync test
// ---------------------------------------------------------------------------
struct Snapshot {
    std::vector<uint8_t> mem;     // concatenated state chunks
    std::vector<uint8_t> input;   // concatenated INPUT_RANGES bytes
    uint32_t seed = 0;
    TimeRec clock;
    std::vector<AllocEv> dynList;  // blocks allocated during the test, alive in this state
    std::vector<uint8_t> dynMem;   // their contents
    // Changes made between the previous frame's update and this one by code
    // outside the update (sound manager tick, effects...): runs of
    // [u32 address][u32 length][bytes]. Replayed between replayed updates.
    std::vector<uint8_t> outDelta;
    bool deltaOk = false;
    // Build 29: this frame's controller input, so a replay can work the pads
    // out again from the inputs instead of copying the recorded result.
    struct InputRec {
        uint32_t frame = 0;
        bool valid = false;
        std::vector<uint8_t> pre;    // pad memory (INPUT_RANGES) right before the input step
        std::vector<uint8_t> post;   // ... right after it
        std::vector<uint8_t> dev;    // runs the device reads wrote: [u32 addr][u32 len][bytes]
        uint32_t in[2][2] = {};      // per player: the 8 bytes of buttons/directions
    } inp;
    std::vector<uint8_t> padDelta;   // pad memory changes by other code before the input step (runs)
    std::vector<uint8_t> postDelta;  // ... and between the input step and the update
    bool inOk = false, padOk = false;
    uint32_t frameNo = 0xFFFFFFFFu;  // which frame this slot holds (rollback)
    uint32_t resimAt = 0xFFFFFFFFu;  // last rollback that re-ran this frame (the frame it happened at)
    uint32_t dtLayout = 0;                // build 35/36: changed-memory tracking layout of the last save
    std::vector<uint32_t> ver;            // ... and the version of each chunk it holds
};
using InputRec = Snapshot::InputRec;
static std::vector<Journal> g_journals;     // heap journal of the last `depth` frames
static std::vector<uint32_t> g_state;        // indices into g_chunks we save/restore
static std::vector<uint32_t> g_stateBlocks;  // heap blocks the state lives in
static size_t g_stateBytes = 0;
static std::vector<Snapshot> g_ring;         // pre-update state of last `depth` frames
static Snapshot g_truth;                     // post-update state of the current frame
static uint32_t g_ringCount = 0;
static uint32_t g_truthFrame = 0xFFFFFFFFu;   // frame whose post-update state g_truth holds
static size_t g_truthBytes = 0;
static uint32_t g_deltaRuns = 0, g_deltaBytes = 0, g_deltaFrames = 0, g_deltaSkips = 0;

// Test 21: with a rewind of 2+ frames every frame differed, by values that
// code outside the update changes between two updates (sound timers counting
// down, effect fades). A replay only re-runs the updates, so those changes
// were missing. We now note what changed between the end of one update and
// the start of the next, and put the same changes back between replayed
// updates.
// ---------------------------------------------------------------------------
// Build 35/36: save and restore only the memory that changed.
// Every page of the saved state is made read-only. The first write to a page
// faults; we mark the state pieces (chunks) on it dirty and make the page
// writable again. Each chunk has a version number: a dirty chunk gets a new
// version at the next save. Each snapshot remembers which version of each
// chunk it holds, so a save copies only chunks whose version changed, and a
// restore writes back only chunks that differ from the snapshot (then the
// live chunk has the snapshot's version again; build 35 counted restored
// memory as changed, so after a while it copied almost everything again).
// Pages written almost every frame stay writable for a while ("hot") and
// count as dirty, which saves a fault per frame. Small chunks, input memory
// and heap pages shared with other memory are always copied.
// ---------------------------------------------------------------------------
namespace dt {
struct Page { uint32_t addr; DWORD prot; uint32_t first, count; };
static std::vector<Page> pages;            // sorted by address
static std::vector<uint32_t> pageChunks;   // state positions on each page
static std::vector<uint8_t> armed, pending, streak, hotLeft, dead;
static std::vector<uint32_t> lastArm;
static std::vector<uint32_t> unarmed;      // page indices not armed (capacity reserved: no allocation in the handler)
static std::vector<uint8_t> dirty, always; // per state position
static std::vector<uint32_t> cur;          // per state position: version of the live content
static std::vector<uint32_t> chunkPage;    // per state position: first page index
static std::vector<uint8_t> chunkPages;    // per state position: number of pages
static std::atomic_flag spin = ATOMIC_FLAG_INIT;
static uint32_t tick = 1;                  // boundaries so far
static uint32_t ver = 1;                   // last version handed out
static std::atomic<bool> active{false};
static uint32_t layout = 1;                // snapshots saved under another layout get a full copy
static size_t builtCount = 0, builtBytes = 0;
static bool handlerOn = false;
static uint64_t faults = 0, arms = 0, copied = 0, saves = 0, restores = 0, rcopied = 0, audits = 0, missed = 0, protFails = 0, hotNow = 0;
static uint32_t alwaysBytes = 0;
}
static inline void DtLock() { while (dt::spin.test_and_set(std::memory_order_acquire)) {} }
static inline void DtUnlock() { dt::spin.clear(std::memory_order_release); }
static int DtFind(uint32_t pg) {
    size_t lo = 0, hi = dt::pages.size();
    while (lo < hi) { size_t m = (lo + hi) / 2; if (dt::pages[m].addr < pg) lo = m + 1; else hi = m; }
    return (lo < dt::pages.size() && dt::pages[lo].addr == pg) ? int(lo) : -1;
}
static inline void DtDirtyPage(uint32_t i) {               // with the lock held
    const dt::Page& p = dt::pages[i];
    for (uint32_t k = 0; k < p.count; k++) dt::dirty[dt::pageChunks[p.first + k]] = 1;
}
static inline void DtUnarmPage(uint32_t i) {               // with the lock held
    DWORD old;
    VirtualProtect((void*)dt::pages[i].addr, addr::PAGE, dt::pages[i].prot, &old);
    if (dt::armed[i]) { dt::armed[i] = 0; dt::unarmed.push_back(i); }
}
static LONG CALLBACK DirtyHandler(EXCEPTION_POINTERS* e) {
    EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    uint32_t pg = (uint32_t)r->ExceptionInformation[1] & ~(addr::PAGE - 1);
    DtLock();
    int i = DtFind(pg);
    if (i < 0) { DtUnlock(); return EXCEPTION_CONTINUE_SEARCH; }
    if (!dt::active) {
        // Tracking was just switched off and the page put back: retry the write
        // if it is writable now, otherwise the fault is not ours.
        DtUnlock();
        MEMORY_BASIC_INFORMATION mi{};
        if (VirtualQuery((void*)pg, &mi, sizeof mi) && ((mi.Protect & 0xFF) == PAGE_READWRITE || (mi.Protect & 0xFF) == PAGE_WRITECOPY))
            return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    bool wasArmed = dt::armed[i] != 0;
    DtUnarmPage(i);
    if (wasArmed) {
        dt::faults++;
        // Written again right after being armed: after 2 such times the page
        // stays writable for 240 saves (it counts as changed meanwhile).
        if (dt::tick - dt::lastArm[i] <= 2) { if (dt::streak[i] < 255) dt::streak[i]++; } else dt::streak[i] = 1;
        if (dt::streak[i] >= 2) { dt::hotLeft[i] = 240; dt::streak[i] = 0; }
    }
    DtDirtyPage(i);
    DtUnlock();
    return EXCEPTION_CONTINUE_EXECUTION;
}
// Put every page back to how it was and stop tracking.
static void DtDisarm() {
    DtLock();
    for (uint32_t i = 0; i < dt::pages.size(); i++) if (dt::armed[i]) DtUnarmPage(i);
    dt::unarmed.clear();
    dt::active = false;
    dt::builtCount = 0;
    dt::layout++;
    DtUnlock();
}
static bool DtPageAlive(uint32_t i) {
    const dt::Page& p = dt::pages[i];
    for (uint32_t k = 0; k < p.count; k++) if (!ChunkAlive(g_chunks[g_state[dt::pageChunks[p.first + k]]])) return false;
    return true;
}
// Protect a sorted list of page indices, joining neighbours into one call.
static void DtProtectList(const uint32_t* list, size_t n) {   // with the lock held
    size_t k = 0;
    while (k < n) {
        size_t j = k + 1;
        while (j < n && dt::pages[list[j]].addr == dt::pages[list[j - 1]].addr + addr::PAGE && dt::pages[list[j]].prot == dt::pages[list[k]].prot) j++;
        DWORD old;
        if (!VirtualProtect((void*)dt::pages[list[k]].addr, (j - k) * addr::PAGE, PAGE_READONLY, &old)) {
            // Neighbouring pages from different memory regions: one by one.
            for (size_t q = k; q < j; q++)
                if (!VirtualProtect((void*)dt::pages[list[q]].addr, addr::PAGE, PAGE_READONLY, &old)) {
                    dt::protFails++;
                    dt::dead[list[q]] = 1;          // can't watch it: its chunks are always copied
                    const dt::Page& p = dt::pages[list[q]];
                    for (uint32_t c = 0; c < p.count; c++) dt::always[dt::pageChunks[p.first + c]] = 1;
                }
        }
        for (size_t q = k; q < j; q++) {
            uint32_t i = list[q];
            if (dt::dead[i]) continue;
            dt::armed[i] = 1;
            dt::lastArm[i] = dt::tick;
            dt::arms++;
        }
        k = j;
    }
}
// Called before each save: arm the pages written since the last time (except hot ones).
static void DtBoundary() {
    DtLock();
    dt::tick++;
    std::vector<uint32_t>& u = dt::unarmed;
    std::sort(u.begin(), u.end());
    size_t keep = 0, nArm = 0;
    static std::vector<uint32_t> toArm;
    toArm.resize(u.size());
    uint64_t hot = 0;
    for (size_t k = 0; k < u.size(); k++) {
        uint32_t i = u[k];
        if (dt::dead[i]) continue;
        if (dt::hotLeft[i] > 0) { dt::hotLeft[i]--; DtDirtyPage(i); u[keep++] = i; hot++; continue; }
        if (!DtPageAlive(i)) {          // a block the game freed: never arm it again
            dt::dead[i] = 1;
            const dt::Page& p = dt::pages[i];
            for (uint32_t q = 0; q < p.count; q++) dt::always[dt::pageChunks[p.first + q]] = 1;
            continue;
        }
        toArm[nArm++] = i;
    }
    u.resize(keep);
    DtProtectList(toArm.data(), nArm);
    dt::hotNow = hot;
    DtUnlock();
}
static inline bool DtChunkArmed(uint32_t pos) {   // with the lock held
    uint32_t i = dt::chunkPage[pos];
    for (uint32_t q = 0; q < dt::chunkPages[pos]; q++) if (!dt::armed[i + q] && !dt::pending[i + q]) return false;
    return true;
}
// Build the page list for the current state and arm every page.
static void DtBuild() {
    if (dt::active) DtDisarm();
    if (!g_cfg.dirtyTracking) return;
    if (!dt::handlerOn) { AddVectoredExceptionHandler(1, DirtyHandler); dt::handlerOn = true; }
    DtLock();
    size_t n = g_state.size();
    dt::always.assign(n, 1);
    dt::dirty.assign(n, 1);
    dt::cur.assign(n, 0);
    dt::chunkPage.assign(n, 0);
    dt::chunkPages.assign(n, 0);
    std::vector<std::pair<uint32_t, uint32_t>> pp;     // (page, position)
    std::vector<std::pair<uint32_t, DWORD>> protOf;
    dt::alwaysBytes = 0;
    for (uint32_t pos = 0; pos < n; pos++) {
        const Chunk& c = g_chunks[g_state[pos]];
        dt::cur[pos] = ++dt::ver;
        bool ok = c.len >= 1024 && ChunkAlive(c);
        uint32_t lo = c.addr & ~(addr::PAGE - 1), hi = (c.addr + c.len + addr::PAGE - 1) & ~(addr::PAGE - 1);
        if (ok && c.block >= 0) {
            const Block& b = g_blocks[c.block];
            if (b.size < (64u << 10) || lo < b.start || hi > b.start + b.size) ok = false;
        }
        if (ok && InInputRanges(c.addr, c.len)) ok = false;   // the input code writes these every frame anyway
        std::vector<std::pair<uint32_t, DWORD>> mine;
        for (uint32_t pg = lo; ok && pg < hi; pg += addr::PAGE) {
            MEMORY_BASIC_INFORMATION mi{};
            if (!VirtualQuery((void*)pg, &mi, sizeof mi) || mi.State != MEM_COMMIT) { ok = false; break; }
            DWORD p = mi.Protect & 0xFF;
            if ((p != PAGE_READWRITE && p != PAGE_WRITECOPY) || (mi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE))) { ok = false; break; }
            mine.push_back({pg, mi.Protect});
        }
        if (!ok) { dt::alwaysBytes += c.len; continue; }
        dt::always[pos] = 0;
        for (auto& m : mine) { pp.push_back({m.first, pos}); protOf.push_back(m); }
    }
    std::sort(pp.begin(), pp.end());
    std::sort(protOf.begin(), protOf.end());
    dt::pages.clear();
    dt::pageChunks.clear();
    for (size_t k = 0; k < pp.size();) {
        uint32_t pg = pp[k].first;
        DWORD prot = std::lower_bound(protOf.begin(), protOf.end(), std::make_pair(pg, (DWORD)0))->second;
        dt::Page P{pg, prot, (uint32_t)dt::pageChunks.size(), 0};
        for (; k < pp.size() && pp[k].first == pg; k++) { dt::pageChunks.push_back(pp[k].second); P.count++; }
        dt::pages.push_back(P);
    }
    size_t np = dt::pages.size();
    for (uint32_t i = 0; i < np; i++)
        for (uint32_t q = 0; q < dt::pages[i].count; q++) {
            uint32_t pos = dt::pageChunks[dt::pages[i].first + q];
            if (!dt::chunkPages[pos]) dt::chunkPage[pos] = i;
            dt::chunkPages[pos]++;
        }
    dt::armed.assign(np, 0);
    dt::pending.assign(np, 0);
    dt::streak.assign(np, 0);
    dt::hotLeft.assign(np, 0);
    dt::dead.assign(np, 0);
    dt::lastArm.assign(np, 0);
    dt::unarmed.clear();
    dt::unarmed.reserve(np + 16);
    for (uint32_t i = 0; i < np; i++) dt::unarmed.push_back(i);   // armed at the next boundary
    dt::layout++;
    dt::builtCount = n;
    dt::builtBytes = g_stateBytes;
    dt::active = true;
    DtUnlock();
    Log("  changed-memory tracking ON: %u pages watched, %u KB always copied (small pieces, shared heap pages, input)",
        (unsigned)np, dt::alwaysBytes / 1024);
}
static inline void DtEnsure() {
    if (!g_cfg.dirtyTracking) return;
    if (!dt::active || dt::builtCount != g_state.size() || dt::builtBytes != g_stateBytes) DtBuild();
}
// ReadFile and the like can't write into a read-only page (they fail instead
// of faulting), so make the buffer's pages writable first.
static void DtTouchRange(uint32_t a, uint32_t n) {
    if (!dt::active || !n) return;
    DtLock();
    for (uint32_t pg = a & ~(addr::PAGE - 1); pg < a + n; pg += addr::PAGE) {
        int i = DtFind(pg);
        if (i < 0) continue;
        if (dt::armed[i]) DtUnarmPage(i);
        DtDirtyPage(i);
    }
    DtUnlock();
}
// Snapshot versions: does chunk pos of snapshot s equal live memory?
static inline bool DtSameAs(const Snapshot& s, uint32_t pos) {
    return dt::active && s.dtLayout == dt::layout && pos < dt::cur.size() && pos < s.ver.size() &&
           !dt::always[pos] && !dt::dirty[pos] && s.ver[pos] == dt::cur[pos];
}

static uint32_t g_auditFirst = 0;
static void MakeOutDelta(Snapshot& sn) {
    sn.outDelta.clear();
    sn.deltaOk = false;
    if (g_truthFrame + 1 != g_frame || g_truthBytes != g_stateBytes) return;
    static uint32_t auditTick = 0;
    // Build 43: check the tracking on every frame of the first 120 frames of
    // each test (test 41b: later fights went out of sync within 8 frames,
    // before the first check every 60 frames).
    bool auditNow = g_auditFirst ? (g_auditFirst--, true) : (++auditTick % 60 == 0);
    if (dt::active && g_truth.dtLayout == dt::layout && g_truth.ver.size() == g_state.size() && auditNow) {
        // Check the tracking itself: pieces it says did not change must equal the truth.
        dt::audits++;
        const uint8_t* t = g_truth.mem.data();
        uint32_t bad = 0;
        for (uint32_t pos = 0; pos < g_state.size(); pos++) {
            const Chunk& c = g_chunks[g_state[pos]];
            if (DtSameAs(g_truth, pos) && ChunkAlive(c) && memcmp((void*)c.addr, t, c.len) != 0) {
                bad++;
                if (dt::missed < 10) Log("  changed-memory tracking MISSED a write: 0x%08X +%u", c.addr, c.len);
                dt::missed++;
            }
            t += c.len;
        }
        if (bad) { DtLock(); for (auto& x : dt::dirty) x = 1; DtUnlock(); }
    }
    const uint8_t* t = g_truth.mem.data();
    for (uint32_t pos = 0; pos < g_state.size(); pos++) {
        const Chunk& c = g_chunks[g_state[pos]];
        const uint8_t* live = reinterpret_cast<const uint8_t*>(c.addr);
        if (DtSameAs(g_truth, pos)) { t += c.len; continue; }
        if (ChunkAlive(c) && memcmp(live, t, c.len) != 0) {
            uint32_t i = 0;
            while (i < c.len) {
                if (live[i] == t[i]) { i++; continue; }
                // Build 30: exact runs only. Joining runs over a few equal bytes
                // was fine while replays matched the first run, but after a
                // rollback with other inputs those bytes can differ.
                uint32_t j = i + 1;
                while (j < c.len && live[j] != t[j]) j++;
                uint32_t a = c.addr + i, n = j - i;
                size_t o = sn.outDelta.size();
                sn.outDelta.resize(o + 8 + n);
                memcpy(&sn.outDelta[o], &a, 4);
                memcpy(&sn.outDelta[o + 4], &n, 4);
                memcpy(&sn.outDelta[o + 8], live + i, n);
                g_deltaRuns++;
                g_deltaBytes += n;
                i = j;
            }
        }
        t += c.len;
    }
    sn.deltaOk = true;
    g_deltaFrames++;
}
static void ApplyOutDelta(const Snapshot& sn) {
    const uint8_t* d = sn.outDelta.data();
    const uint8_t* end = d + sn.outDelta.size();
    while (d + 8 <= end) {
        uint32_t a, n;
        memcpy(&a, d, 4);
        memcpy(&n, d + 4, 4);
        memcpy((void*)a, d + 8, n);
        d += 8 + n;
    }
}
static uint32_t g_testFrames = 0, g_desyncs = 0, g_diffEvents = 0, g_skips = 0;

static bool StateBlockFreed() {
    for (uint32_t b : g_stateBlocks) if (g_blockDead[b] == 1) return true;
    return false;
}

static void CopyState(Snapshot& s, bool save) {
    DtEnsure();
    bool act = dt::active;
    size_t n = g_state.size();
    bool inc = act && s.dtLayout == dt::layout && s.ver.size() == n;
    if (save && act) DtBoundary();   // arm the pages written since the last save
    HANDLE heap = CrtHeap();
    if (!g_stateBlocks.empty()) HeapLock(heap);
    uint8_t* d = s.mem.data();
    uint64_t cop = 0;
    if (save) {
        if (act && !inc) s.ver.assign(n, 0);
        for (uint32_t pos = 0; pos < n; pos++) {
            const Chunk& c = g_chunks[g_state[pos]];
            if (ChunkAlive(c)) {
                bool need = true;
                if (act) {
                    if (dt::always[pos]) dt::cur[pos] = ++dt::ver;
                    else {
                        DtLock();
                        if (dt::dirty[pos]) { dt::cur[pos] = ++dt::ver; if (DtChunkArmed(pos)) dt::dirty[pos] = 0; }
                        DtUnlock();
                    }
                    need = s.ver[pos] != dt::cur[pos];
                    s.ver[pos] = dt::cur[pos];
                }
                if (need) { memcpy(d, (void*)c.addr, c.len); cop += c.len; }
            }
            d += c.len;
        }
        s.dtLayout = act ? dt::layout : 0;
        dt::saves++;
        dt::copied += cop;
    } else {
        static std::vector<uint32_t> rearm;
        rearm.clear();
        for (uint32_t pos = 0; pos < n; pos++) {
            const Chunk& c = g_chunks[g_state[pos]];
            if (ChunkAlive(c)) {
                bool need = !inc || dt::always[pos] || dt::dirty[pos] || s.ver[pos] != dt::cur[pos];
                if (need) {
                    bool tracked = false;
                    if (act && !dt::always[pos]) {
                        // Make the pages writable for our copy and arm them again right after,
                        // so the restored chunk counts as "same as the snapshot".
                        DtLock();
                        tracked = true;
                        uint32_t i = dt::chunkPage[pos];
                        for (uint32_t q = 0; q < dt::chunkPages[pos]; q++) {
                            uint32_t j = i + q;
                            if (dt::armed[j]) {
                                DWORD old;
                                VirtualProtect((void*)dt::pages[j].addr, addr::PAGE, dt::pages[j].prot, &old);
                                dt::armed[j] = 0;
                                dt::pending[j] = 1;
                                rearm.push_back(j);
                            } else if (!dt::pending[j]) tracked = false;   // written since the last save: stays untracked until then
                        }
                        DtUnlock();
                    }
                    memcpy((void*)c.addr, d, c.len);
                    cop += c.len;
                    if (act) {
                        dt::cur[pos] = inc ? s.ver[pos] : ++dt::ver;
                        if (!dt::always[pos]) dt::dirty[pos] = (tracked && inc) ? 0 : 1;
                    }
                }
            }
            d += c.len;
        }
        if (act && !rearm.empty()) {
            DtLock();
            for (uint32_t j : rearm) dt::pending[j] = 0;
            std::sort(rearm.begin(), rearm.end());
            DtProtectList(rearm.data(), rearm.size());
            DtUnlock();
        }
        dt::restores++;
        dt::rcopied += cop;
    }
    if (!g_stateBlocks.empty()) HeapUnlock(heap);
    // Blocks the game allocated during the test (they are never really freed
    // while a rewind could still reach them, so writing to them is safe).
    JLock lock;
    if (save) {
        s.dynList = g_dyn;
        size_t total = 0;
        for (auto& b : g_dyn) total += b.size;
        s.dynMem.resize(total);
        uint8_t* q = s.dynMem.data();
        for (auto& b : g_dyn) { memcpy(q, (void*)b.ptr, b.size); q += b.size; }
    } else {
        g_dyn = s.dynList;
        const uint8_t* q = s.dynMem.data();
        for (auto& b : s.dynList) { memcpy((void*)b.ptr, q, b.size); q += b.size; }
    }
    if (save) {
        d = s.input.data();
        for (auto& r : addr::INPUT_RANGES) { memcpy(d, (void*)r.start, r.end - r.start); d += r.end - r.start; }
        SeedInit();
        s.seed = t_seed;
    } else {
        t_seed = s.seed;
    }
}
static void __attribute__((unused)) LoadInput(const Snapshot& s) {
    const uint8_t* d = s.input.data();
    for (auto& r : addr::INPUT_RANGES) { memcpy((void*)r.start, d, r.end - r.start); d += r.end - r.start; }
}

// Compares the blocks allocated during the test (live) with the truth.
// Returns the number of blocks that differ (missing, extra, or other bytes).
static uint32_t DynDiff(bool log) {
    JLock lock;
    struct E { uint32_t ptr, size, off; };
    std::vector<E> t, l;
    uint32_t off = 0;
    for (auto& b : g_truth.dynList) { t.push_back({b.ptr, b.size, off}); off += b.size; }
    for (auto& b : g_dyn) l.push_back({b.ptr, b.size, 0});
    auto byPtr = [](const E& a, const E& b) { return a.ptr < b.ptr; };
    std::sort(t.begin(), t.end(), byPtr);
    std::sort(l.begin(), l.end(), byPtr);
    uint32_t bad = 0;
    size_t i = 0, j = 0;
    while (i < t.size() || j < l.size()) {
        if (j == l.size() || (i < t.size() && t[i].ptr < l[j].ptr)) {
            if (log && bad < 20) Log("    new heap block 0x%08X (%u bytes) missing after replay", t[i].ptr, t[i].size);
            bad++; i++;
        } else if (i == t.size() || l[j].ptr < t[i].ptr) {
            if (log && bad < 20) Log("    new heap block 0x%08X (%u bytes) exists only after replay", l[j].ptr, l[j].size);
            bad++; j++;
        } else {
            if (memcmp((void*)l[j].ptr, g_truth.dynMem.data() + t[i].off, t[i].size) != 0) {
                if (log && bad < 20) Log("    new heap block 0x%08X (%u bytes) has different contents", t[i].ptr, t[i].size);
                bad++;
            }
            i++; j++;
        }
    }
    return bad;
}

// Every spot that differed during the whole test, so one run tells us all of
// them (the detailed lines above only cover the first desyncs).
struct DiffStat { uint32_t count, first, last, len; int block; uint8_t ex[8], got[8]; };
static std::unordered_map<uint32_t, DiffStat> g_diffStats;
static uint32_t g_diffStatsDropped = 0;
static void NoteDiff(uint32_t a, int block, uint32_t len, const uint8_t* ex, const uint8_t* got, uint32_t frame) {
    auto it = g_diffStats.find(a);
    if (it == g_diffStats.end()) {
        if (g_diffStats.size() >= 4096) { g_diffStatsDropped++; return; }
        DiffStat d{};
        d.first = frame; d.block = block; d.len = len;
        memcpy(d.ex, ex, len < 8 ? len : 8);
        memcpy(d.got, got, len < 8 ? len : 8);
        it = g_diffStats.emplace(a, d).first;
    }
    it->second.count++;
    it->second.last = frame;
    if (len > it->second.len) it->second.len = len;
}
static void LogDiffStats(size_t top) {
    if (g_diffStats.empty()) { Log("  differing spots: none"); return; }
    std::vector<std::pair<uint32_t, DiffStat>> v(g_diffStats.begin(), g_diffStats.end());
    std::sort(v.begin(), v.end(), [](const std::pair<uint32_t, DiffStat>& a, const std::pair<uint32_t, DiffStat>& b) {
        return a.second.count != b.second.count ? a.second.count > b.second.count : a.first < b.first;
    });
    Log("  differing spots over the whole test: %u different places%s (most frequent first):",
        (unsigned)v.size(), g_diffStatsDropped ? " (list was full, some not counted)" : "");
    for (size_t i = 0; i < v.size() && i < top; i++) {
        const DiffStat& d = v[i].second;
        char where[96], ex[3 * 8 + 1] = "", got[3 * 8 + 1] = "";
        if (d.block >= 0 && d.block < (int)g_blocks.size())
            snprintf(where, sizeof where, "heap 0x%08X (block 0x%08X %u bytes +0x%X)", v[i].first, g_blocks[d.block].start,
                     g_blocks[d.block].size, v[i].first - g_blocks[d.block].start);
        else
            snprintf(where, sizeof where, "0x%08X", v[i].first);
        for (uint32_t k = 0; k < d.len && k < 8; k++) {
            sprintf(ex + strlen(ex), "%02X ", d.ex[k]);
            sprintf(got + strlen(got), "%02X ", d.got[k]);
        }
        Log("    %-56s +%-3u %6u times, frames %u-%u, e.g. expected %s| got %s", where, d.len, d.count, d.first, d.last, ex, got);
    }
}

static void ReportDiff(uint32_t frame) {
    // Details for the first desyncs, and for any desync after a quiet stretch
    // (test 19: the one late desync was not shown).
    static uint32_t lastDesyncTest = 0;
    bool detailed = (int)g_diffEvents < g_cfg.maxDiffEvents || g_testFrames - lastDesyncTest > 300 || g_testFrames < lastDesyncTest;
    lastDesyncTest = g_testFrames;
    g_diffEvents++;
    uint32_t ranges = 0, bytes = 0;
    const uint8_t* t = g_truth.mem.data();
    for (uint32_t ci : g_state) {
        const Chunk& c = g_chunks[ci];
        const uint8_t* live = reinterpret_cast<const uint8_t*>(c.addr);
        if (ChunkAlive(c) && memcmp(live, t, c.len) != 0) {
            uint32_t i = 0;
            while (i < c.len) {
                if (live[i] == t[i]) { i++; continue; }
                uint32_t j = i + 1;
                while (j < c.len && (live[j] != t[j] || (j + 1 < c.len && live[j + 1] != t[j + 1]))) j++;
                bytes += j - i;
                NoteDiff(c.addr + i, c.block, j - i, t + i, live + i, frame);
                if (detailed && ranges < 60) {
                    char ex[3 * 16 + 1] = "", ac[3 * 16 + 1] = "";
                    for (uint32_t k = i; k < j && k < i + 16; k++) {
                        sprintf(ex + strlen(ex), "%02X ", t[k]);
                        sprintf(ac + strlen(ac), "%02X ", live[k]);
                    }
                    if (c.block >= 0)
                        Log("    heap 0x%08X (block 0x%08X+0x%X) +%u  expected %s| got %s", c.addr + i,
                            g_blocks[c.block].start, c.addr + i - g_blocks[c.block].start, j - i, ex, ac);
                    else
                        Log("    0x%08X +%u  expected %s| got %s", c.addr + i, j - i, ex, ac);
                }
                ranges++;
                i = j;
            }
        }
        t += c.len;
    }
    bool seedBad = (t_seed != g_truth.seed);
    uint32_t dynBad = DynDiff(detailed);
    if (detailed || g_diffEvents % 60 == 1)
        Log("DESYNC frame %u depth %d: %u differing ranges, %u bytes, %u new heap blocks differ%s", frame, g_cfg.depth,
            ranges, bytes, dynBad, seedBad ? ", rand seed differs" : "");
}

// ---------------------------------------------------------------------------
// Build 49: finding the rest of the fight memory on the spot.
// Test 48 crashed after a super move that wrote memory the 25-second
// profile had never seen change. So there is no profile any more: the state
// starts from a fixed list (addr::FORCE_RANGES, the player data, the input
// memory, the known object pools and fight blocks) and every other piece of
// game memory is watched: all other .data pages, and the inside pages of the
// game's own heap blocks from 64 KB to 32 MB. Watched pages are read-only.
// The first time the game logic writes one during an update, the handler
// copies the page as it was just before that write (nothing wrote it since
// it was made read-only, so that copy is what the page held in every saved
// frame too). Before the next frame, the page (for a heap block: the whole
// block) joins the state, and the copy goes into every saved frame, so a
// rewind to before the write stays exact. Pages that other threads write are
// left out for good; pages the game thread writes outside the update are
// made read-only again a little later, up to kHuntBusy times.
// ---------------------------------------------------------------------------
struct HPage { uint32_t addr; DWORD prot; int32_t block; };
// Status: 0 watching, 1 added to the state, 2 left out (other thread), 3 not watched any more, 4 written by the update (added next frame)
struct HSet {
    std::vector<HPage> pages;                        // sorted by addr
    std::vector<std::atomic<uint8_t>> armed, status;
    std::vector<std::atomic<uint32_t>> upd, outside, thr;
    std::vector<uint32_t> firstEip, firstFrame, thrEip;
    explicit HSet(size_t n) : armed(n), status(n), upd(n), outside(n), thr(n), firstEip(n, 0), firstFrame(n, 0), thrEip(n, 0) {}
};
static std::atomic<HSet*> g_hset{nullptr};
static std::atomic<bool> g_hArm{false};
static std::atomic<int> g_hArming{0};
static std::atomic<uint32_t> g_hFaults{0};
static std::atomic_flag g_hSpin = ATOMIC_FLAG_INIT;
static uint32_t g_hAdded = 0, g_hAddedBlocks = 0, g_hShared = 0, g_hAddLogs = 0;
static uint64_t g_hWatchHeap = 0;
static bool g_hHandler = false;
static const uint32_t kHuntBusy = 60;         // writes outside the game logic before we stop watching a page
// Copies of pages taken right before their first write (filled by the handler).
static const uint32_t kPreCap = 2048;
static uint8_t* g_hPre = nullptr;
static uint32_t g_hPreAddr[kPreCap];
static std::atomic<uint32_t> g_hPreN{0};
static std::atomic<uint32_t> g_hPreLost{0};
static uint32_t g_hPreLostTotal = 0;
// Heap blocks added in earlier fights: blocks of the same size join the state
// from the start in later fights (sizes, at most 64).
static std::vector<uint32_t> g_heapKeptSizes;
static inline void HLock() { while (g_hSpin.test_and_set(std::memory_order_acquire)) {} }
static inline void HUnlock() { g_hSpin.clear(std::memory_order_release); }

static int HFind(const HSet* h, uint32_t pg) {
    size_t lo = 0, hi = h->pages.size();
    while (lo < hi) { size_t m = (lo + hi) / 2; if (h->pages[m].addr < pg) lo = m + 1; else hi = m; }
    return (lo < h->pages.size() && h->pages[lo].addr == pg) ? int(lo) : -1;
}
static LONG CALLBACK HuntHandler(EXCEPTION_POINTERS* e) {
    EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    HSet* h = g_hset.load();
    if (!h) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t pg = (uint32_t)r->ExceptionInformation[1] & ~(addr::PAGE - 1);
    int i = HFind(h, pg);
    if (i < 0) return EXCEPTION_CONTINUE_SEARCH;
    HLock();
    if (!h->armed[i].load()) {
        // Not armed by us (any more): if the page is writable now the write can
        // simply be tried again, otherwise the fault belongs to someone else.
        HUnlock();
        MEMORY_BASIC_INFORMATION mi{};
        if (VirtualQuery((void*)pg, &mi, sizeof mi) && ((mi.Protect & 0xFF) == PAGE_READWRITE || (mi.Protect & 0xFF) == PAGE_WRITECOPY))
            return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_hFaults++;
    bool game = GetCurrentThreadId() == g_gameTid;
    if (game && g_inUpdate.load(std::memory_order_relaxed)) {
        uint32_t k = g_hPreN.fetch_add(1);
        if (k < kPreCap && g_hPre) { memcpy(g_hPre + (size_t)k * addr::PAGE, (const void*)pg, addr::PAGE); g_hPreAddr[k] = pg; }
        else g_hPreLost++;
        if (h->upd[i]++ == 0) { h->firstEip[i] = e->ContextRecord->Eip; h->firstFrame[i] = g_frame; }
        h->status[i] = 4;
    } else if (game) {
        h->outside[i]++;
    } else {
        if (h->thr[i]++ == 0) h->thrEip[i] = e->ContextRecord->Eip;
        h->status[i] = 2;
    }
    DWORD old;
    VirtualProtect((void*)pg, addr::PAGE, h->pages[i].prot, &old);
    h->armed[i] = 0;
    HUnlock();
    return EXCEPTION_CONTINUE_EXECUTION;
}
// Arms pages again that the game thread wrote outside the update.
static DWORD WINAPI HArmThread(LPVOID) {
    while (g_hArm.load()) {
        HSet* h = g_hset.load();
        g_hArming++;
        DWORD old;
        for (size_t i = 0; h && i < h->pages.size() && g_hArm.load(); i++) {
            if (h->armed[i].load() || h->status[i].load() != 0) continue;
            HLock();
            if (!h->armed[i].load() && h->status[i].load() == 0) {
                if (h->outside[i].load() > kHuntBusy) h->status[i] = 3;
                else if (VirtualProtect((void*)h->pages[i].addr, addr::PAGE, PAGE_READONLY, &old)) h->armed[i] = 1;
                else h->status[i] = 3;
            }
            HUnlock();
        }
        g_hArming--;
        Sleep(10);
    }
    return 0;
}
// Stop watching a range for good (a file read into it, or the game frees the block).
static bool HuntReleaseRange(uint32_t a, uint32_t n) {
    HSet* h = g_hset.load();
    if (!h || !n) return false;
    bool hit = false;
    uint32_t pg = a & ~(addr::PAGE - 1);
    // first watched page at or after pg
    size_t lo = 0, hi = h->pages.size();
    while (lo < hi) { size_t m = (lo + hi) / 2; if (h->pages[m].addr < pg) lo = m + 1; else hi = m; }
    for (size_t i = lo; i < h->pages.size() && h->pages[i].addr < a + n; i++) {
        HLock();
        if (h->status[i] == 0 || h->status[i] == 4) h->status[i] = 3;
        if (h->armed[i]) {
            DWORD old;
            VirtualProtect((void*)h->pages[i].addr, addr::PAGE, h->pages[i].prot, &old);
            h->armed[i] = 0;
            hit = true;
        }
        HUnlock();
    }
    return hit;
}
static void HuntArmAll(HSet* h) {
    // Join neighbouring pages into one call; one by one if that fails.
    size_t k = 0, n = h->pages.size();
    uint32_t fails = 0;
    HLock();     // the handler must never see a read-only page that is not marked armed yet
    while (k < n) {
        size_t j = k + 1;
        while (j < n && h->pages[j].addr == h->pages[j - 1].addr + addr::PAGE && h->pages[j].prot == h->pages[k].prot &&
               h->pages[j].block == h->pages[k].block)
            j++;
        DWORD old;
        if (VirtualProtect((void*)h->pages[k].addr, (j - k) * addr::PAGE, PAGE_READONLY, &old)) {
            for (size_t q = k; q < j; q++) h->armed[q] = 1;
        } else {
            for (size_t q = k; q < j; q++) {
                if (VirtualProtect((void*)h->pages[q].addr, addr::PAGE, PAGE_READONLY, &old)) h->armed[q] = 1;
                else { h->status[q] = 3; fails++; }
            }
        }
        k = j;
    }
    HUnlock();
    if (fails) Log("  memory watch: %u pages could not be made read-only (not watched)", fails);
}
static void HuntStart() {
    g_hFaults = 0;
    g_hAdded = g_hAddedBlocks = g_hShared = g_hAddLogs = 0;
    g_hPreN = 0;
    g_hPreLost = 0;
    g_hPreLostTotal = 0;
    g_hWatchHeap = 0;
    if (!g_cfg.hunt) return;
    if (!g_hPre) g_hPre = (uint8_t*)VirtualAlloc(nullptr, (size_t)kPreCap * addr::PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    std::vector<uint8_t> inState(addr::DATA_PAGES, 0), blockIn(g_blocks.size(), 0);
    for (uint32_t ci : g_state) {
        const Chunk& c = g_chunks[ci];
        if (c.block < 0) { if (ci < addr::DATA_PAGES) inState[ci] = 1; }
        else blockIn[c.block] = 1;
    }
    std::vector<HPage> pages;
    uint32_t dataPages = 0;
    for (uint32_t i = 0; i < addr::DATA_PAGES && i < g_chunks.size(); i++) {
        if (inState[i] || g_chunks[i].block >= 0) continue;
        uint32_t pg = addr::DATA_START + i * addr::PAGE;
        if (pg == addr::PART_PAGE || NeverRestore(pg) || InInputRanges(pg, addr::PAGE)) continue;
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery((void*)pg, &mi, sizeof mi) || mi.State != MEM_COMMIT) continue;
        DWORD p = mi.Protect & 0xFF;
        if (p != PAGE_READWRITE && p != PAGE_WRITECOPY) continue;
        if (mi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) continue;
        pages.push_back({pg, mi.Protect, -1});
        dataPages++;
    }
    uint32_t heapBlocks = 0;
    const uint64_t heapCap = (uint64_t)g_cfg.watchHeapMB << 20;
    if (g_cfg.heap && g_cfg.heapRestore) {
        for (uint32_t b = 0; b < g_blocks.size(); b++) {
            const Block& B = g_blocks[b];
            if (blockIn[b] || g_blockDead[b] || B.size < (64u << 10) || B.size > (32u << 20)) continue;
            if (!g_own.Has(B.start)) continue;
            uint32_t lo = (B.start + addr::PAGE - 1) & ~(addr::PAGE - 1), hi = (B.start + B.size) & ~(addr::PAGE - 1);
            if (hi <= lo) continue;
            if (g_hWatchHeap + (hi - lo) > heapCap) continue;
            MEMORY_BASIC_INFORMATION mi{};
            if (!VirtualQuery((void*)lo, &mi, sizeof mi) || mi.State != MEM_COMMIT) continue;
            DWORD p = mi.Protect & 0xFF;
            if (p != PAGE_READWRITE || (mi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE))) continue;
            if ((uint32_t)mi.BaseAddress + mi.RegionSize < hi) continue;   // keep it simple: one memory region
            for (uint32_t pg = lo; pg < hi; pg += addr::PAGE) pages.push_back({pg, mi.Protect, (int32_t)b});
            g_hWatchHeap += hi - lo;
            heapBlocks++;
        }
    }
    std::sort(pages.begin(), pages.end(), [](const HPage& a, const HPage& b) { return a.addr < b.addr; });
    HSet* h = new HSet(pages.size());     // never freed: the handler may still read it
    h->pages = std::move(pages);
    for (size_t i = 0; i < h->pages.size(); i++) { h->armed[i] = 0; h->status[i] = 0; h->upd[i] = 0; h->outside[i] = 0; h->thr[i] = 0; }
    if (!g_hHandler) { AddVectoredExceptionHandler(1, HuntHandler); g_hHandler = true; }
    g_hset = h;
    DWORD t0 = GetTickCount();
    HuntArmAll(h);                 // now, before the first save: every saved frame is then covered
    g_hArm = true;
    CreateThread(nullptr, 0, HArmThread, nullptr, 0, nullptr);
    Log("  memory watch: %u more .data pages and %u heap blocks (%.1f MB) watched; the first write by the game logic adds them (%.0f ms to set up)",
        dataPages, heapBlocks, g_hWatchHeap / 1048576.0, (double)(GetTickCount() - t0));
}
// Put one chunk into the state; pre = its content in every saved frame.
static void StateAppend(uint32_t ci, const uint8_t* pre) {
    const Chunk& c = g_chunks[ci];
    g_state.push_back(ci);
    g_stateBytes += c.len;
    for (auto& s : g_ring) s.mem.insert(s.mem.end(), pre, pre + c.len);
    const uint8_t* live = reinterpret_cast<const uint8_t*>(c.addr);
    g_truth.mem.insert(g_truth.mem.end(), live, live + c.len);
}
// Called between frames on the game thread: adds what the update wrote.
// The saved frames stay valid, so nothing has to start over.
static bool HuntCollect() {
    HSet* h = g_hset.load();
    if (!h) return false;
    uint32_t n = g_hPreN.load();
    uint32_t lost = g_hPreLost.load();
    if (n == 0 && lost == 0) return false;
    if (n > kPreCap) n = kPreCap;
    // page -> copy index
    std::vector<std::pair<uint32_t, uint32_t>> pre;
    pre.reserve(n);
    for (uint32_t k = 0; k < n; k++) pre.push_back({g_hPreAddr[k], k});
    std::sort(pre.begin(), pre.end());
    auto copyOf = [&](uint32_t pg) -> const uint8_t* {
        auto it = std::lower_bound(pre.begin(), pre.end(), std::make_pair(pg, 0u));
        if (it != pre.end() && it->first == pg) return g_hPre + (size_t)it->second * addr::PAGE;
        return nullptr;
    };
    size_t truthWas = g_truthBytes, stateWas = g_stateBytes;
    uint32_t addedNow = 0, missing = 0;
    std::vector<uint8_t> buf;
    char who[96];
    HANDLE heap = CrtHeap();
    bool locked = false;
    for (size_t i = 0; i < h->pages.size(); i++) {
        if (h->status[i].load() != 4) continue;
        const HPage& P = h->pages[i];
        if (P.block < 0) {
            uint32_t ci = (P.addr - addr::DATA_START) / addr::PAGE;
            const uint8_t* src = copyOf(P.addr);
            if (!src) { src = reinterpret_cast<const uint8_t*>(P.addr); missing++; }
            StateAppend(ci, src);
            h->status[i] = 1;
            g_hAdded++;
            addedNow++;
            if (std::find(g_huntKept.begin(), g_huntKept.end(), P.addr) == g_huntKept.end()) g_huntKept.push_back(P.addr);
            if (g_hAddLogs < 60) {
                g_hAddLogs++;
                DescribeAddr(h->firstEip[i], who, sizeof who);
                Log("  memory watch: added .data page 0x%08X (first written by %s at frame %u)", P.addr, who, h->firstFrame[i]);
            }
            continue;
        }
        // A heap block: add all of it. Pages of the block not written yet are
        // still read-only, so their content now is their content in every saved frame.
        int32_t b = P.block;
        size_t first = i;
        while (first > 0 && h->pages[first - 1].block == b) first--;
        size_t last = i;
        while (last + 1 < h->pages.size() && h->pages[last + 1].block == b) last++;
        if (!locked && heap) { HeapLock(heap); locked = true; }
        bool alive = !g_blockDead[b];
        std::vector<uint32_t> thrPages;              // pages of the block another thread wrote: left out
        for (size_t q = first; q <= last; q++) if (h->status[q].load() == 2) thrPages.push_back(h->pages[q].addr);
        for (size_t q = first; q <= last; q++) {
            HLock();
            if (h->armed[q]) { DWORD old; VirtualProtect((void*)h->pages[q].addr, addr::PAGE, h->pages[q].prot, &old); h->armed[q] = 0; }
            if (h->status[q] != 2) h->status[q] = alive ? 1 : 3;
            HUnlock();
        }
        if (!alive) continue;
        const Block& B = g_blocks[b];
        uint32_t c0 = g_blockFirst[b];
        uint32_t leftOut = 0;
        for (uint32_t ci = c0; ci < g_chunks.size() && g_chunks[ci].block == b; ci++) {
            const Chunk& c = g_chunks[ci];
            bool thr = false;
            for (uint32_t tp : thrPages) if (c.addr < tp + addr::PAGE && tp < c.addr + c.len) thr = true;
            if (thr) { leftOut++; continue; }
            buf.assign(reinterpret_cast<const uint8_t*>(c.addr), reinterpret_cast<const uint8_t*>(c.addr) + c.len);
            for (uint32_t pg = c.addr & ~(addr::PAGE - 1); pg < c.addr + c.len; pg += addr::PAGE) {
                const uint8_t* src = copyOf(pg);
                if (!src) continue;
                uint32_t lo = std::max(pg, c.addr), hi = std::min(pg + addr::PAGE, c.addr + c.len);
                memcpy(buf.data() + (lo - c.addr), src + (lo - pg), hi - lo);
            }
            StateAppend(ci, buf.data());
        }
        g_stateBlocks.push_back((uint32_t)b);
        g_hAddedBlocks++;
        addedNow++;
        if (std::find(g_heapKeptSizes.begin(), g_heapKeptSizes.end(), B.size) == g_heapKeptSizes.end() && g_heapKeptSizes.size() < 64)
            g_heapKeptSizes.push_back(B.size);
        if (g_hAddLogs < 60) {
            g_hAddLogs++;
            DescribeAddr(h->firstEip[i], who, sizeof who);
            Log("  memory watch: added heap block 0x%08X (%u bytes; first written by %s at frame %u)%s", B.start, B.size, who, h->firstFrame[i],
                leftOut ? " - parts another thread writes are left out" : "");
        }
        i = last;
    }
    if (locked) HeapUnlock(heap);
    for (uint32_t k = 0; k < n; k++) g_hPreAddr[k] = 0;
    g_hPreN = 0;
    g_hPreLost = 0;
    if (lost) g_hPreLostTotal += lost;
    if (missing || lost) Log("  memory watch: %u pages added without their earlier content (%u copies did not fit); a rewind to before frame %u may differ there", missing, lost, g_frame);
    if (truthWas == stateWas) g_truthBytes = g_stateBytes;   // the true state of the last frame grew with it
    return addedNow != 0;
}
static void HuntStop() {
    HSet* h = g_hset.load();
    if (!h) return;
    HuntCollect();
    g_hArm = false;
    for (int pass = 0; pass < 2; pass++) {
        while (g_hArming.load()) Sleep(1);
        for (size_t i = 0; i < h->pages.size(); i++) {
            HLock();
            if (h->armed[i]) { DWORD old; VirtualProtect((void*)h->pages[i].addr, addr::PAGE, h->pages[i].prot, &old); h->armed[i] = 0; }
            HUnlock();
        }
        Sleep(20);
    }
    g_hset = nullptr;
    uint32_t busy = 0, shared = 0;
    for (size_t i = 0; i < h->pages.size(); i++) {
        if (h->status[i] == 3) busy++;
        if (h->status[i] == 2 && h->upd[i]) shared++;
    }
    Log("  memory watch: %u .data pages and %u heap blocks added, %u pages written by the game logic and other threads (left out), %u not watched any more, %u faults, %u copies did not fit",
        g_hAdded, g_hAddedBlocks, shared, busy, g_hFaults.load(), g_hPreLostTotal);
    char line[1200] = "";
    size_t n = 0;
    for (size_t i = 0; i < h->pages.size() && n + 12 < sizeof line; i++)
        if (h->status[i] == 1 && h->pages[i].block < 0) n += snprintf(line + n, sizeof line - n, " 0x%08X", h->pages[i].addr);
    if (n) Log("  memory watch: .data pages added:%s", line);
}

// ---------------------------------------------------------------------------
// Controller input (build 29, step 3a)
// While the sync test runs, the mod decides what every pad "pressed" each
// frame, through the same door the game's online mode uses for the other
// player: all 10 pads are switched to "external input", and the mod fills
// in each player's 8 bytes (for now, what that player's own controller
// reads). A replay then runs the game's own input code again from those
// bytes, with the device reads answered from the recording. Online, the
// remote player's bytes will come from the network instead.
// ---------------------------------------------------------------------------
static inline uint8_t* Pad(uint32_t i) { return reinterpret_cast<uint8_t*>(addr::PAD_TABLE + i * addr::PAD_STRIDE); }
static inline uint32_t& PadU32(uint32_t i, uint32_t off) { return *reinterpret_cast<uint32_t*>(Pad(i) + off); }
typedef uint8_t(__cdecl* PadOfPlayer_t)(uint8_t);
static inline uint32_t PadOfPlayer(int p) { return reinterpret_cast<PadOfPlayer_t>(addr::FN_PAD_OF_PLAYER)((uint8_t)p); }
typedef void(__cdecl* VoidFn)();

static uint32_t InputBytes() {
    uint32_t n = 0;
    for (auto& r : addr::INPUT_RANGES) n += r.end - r.start;
    return n;
}
static void CopyRanges(std::vector<uint8_t>& out) {
    out.resize(InputBytes());
    uint8_t* d = out.data();
    for (auto& r : addr::INPUT_RANGES) { memcpy(d, (void*)r.start, r.end - r.start); d += r.end - r.start; }
}
static void WriteRanges(const std::vector<uint8_t>& in) {
    const uint8_t* d = in.data();
    for (auto& r : addr::INPUT_RANGES) { memcpy((void*)r.start, d, r.end - r.start); d += r.end - r.start; }
}
// Byte runs where `now` differs from `before` (both laid out like INPUT_RANGES).
// `now` = nullptr compares with live memory.
static void RangeRuns(const std::vector<uint8_t>& before, const uint8_t* now, std::vector<uint8_t>& runs) {
    runs.clear();
    const uint8_t* b = before.data();
    for (auto& r : addr::INPUT_RANGES) {
        uint32_t len = r.end - r.start;
        const uint8_t* l = now ? now : reinterpret_cast<const uint8_t*>(r.start);
        uint32_t i = 0;
        while (i < len) {
            if (l[i] == b[i]) { i++; continue; }
            uint32_t j = i + 1;
            while (j < len && l[j] != b[j]) j++;
            uint32_t a = r.start + i, n = j - i;
            size_t o = runs.size();
            runs.resize(o + 8 + n);
            memcpy(&runs[o], &a, 4);
            memcpy(&runs[o + 4], &n, 4);
            memcpy(&runs[o + 8], l + i, n);
            i = j;
        }
        b += len;
        if (now) now += len;
    }
}
static void ApplyRuns(const std::vector<uint8_t>& runs) {
    const uint8_t* d = runs.data();
    const uint8_t* end = d + runs.size();
    while (d + 8 <= end) {
        uint32_t a, n;
        memcpy(&a, d, 4);
        memcpy(&n, d + 4, 4);
        memcpy((void*)a, d + 8, n);
        d += 8 + n;
    }
}

static InputRec g_pend;                       // input of the frame about to be updated
static int g_inPhase = 0;                     // 0 = game reads devices, 1 = first run, 2 = replay
static const InputRec* g_inReplay = nullptr;
static bool g_devDone = false;
static std::vector<uint8_t> g_devBefore;
static uint32_t g_inputMiss = 0, g_inputMissLogs = 0, g_inSkips = 0, g_inNoDev = 0;
static uint32_t g_inFrames = 0, g_inActive[2] = {0, 0};
static uint32_t g_inLastMap[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};

// Step 3c (network play, net_session.inc)
enum class NetAct { Free, Stall, Inject };
static bool g_netInject = false;     // this input step uses the network inputs
static bool g_netStall = false;      // this frame waits (no input step, no update)
static NetAct NetBeforeInput();
static void NetFill(uint32_t T);
static void NetAfterInput(bool injected);
static void NetSavePre(const std::vector<uint8_t>& pre);
static bool NetOn();
static int NetRemoteSide();
static void NetFpCapture(uint32_t k);
static void NetFeedRb();
static void NetRbStarting(uint32_t F);
static void NetUpdateStarts();
static void NetFrameEnd(void* scene);
static void UiAfterGameInput();     // build 57 (overlay.inc)

// ---- Rollback engine: input side (build 30) --------------------------------
// One player's input is treated as if it came over the network: the game
// gets a guess (that player's newest input that has "arrived"), and the real
// input "arrives" a few frames later from a fake network queue. When it
// differs from the guess, the engine rewinds to that frame and re-runs.
static bool g_rbWant = false, g_rbOn = false, g_rbStarted = false;
static const uint32_t RB_NONE = 0xFFFFFFFFu;
static uint32_t g_rbStart = 0;              // first frame a rewind can go back to
static uint32_t g_rbConf = 0;               // all frames before this have the remote player's real input
static uint32_t g_rbLast[2] = {0, 0};       // remote player's newest real input (the guess for later frames)
static int g_rbRemote = 0;                  // 0 = player 1, 1 = player 2
static uint32_t g_rbWrong = RB_NONE;        // earliest frame whose guess turned out wrong
struct FakePkt { uint32_t frame = RB_NONE, arrive = 0, v[2] = {0, 0}; bool delivered = false; };
struct ConfIn { uint32_t frame = RB_NONE, v[2] = {0, 0}; };
static FakePkt g_fq[256];
static ConfIn g_cf[256];
static uint32_t g_rbRng = 12345;
static uint32_t g_rbSent = 0, g_rbGuessRight = 0, g_rbGuessWrong = 0;
static uint32_t RbRand() { g_rbRng = g_rbRng * 1103515245u + 12345u; return (g_rbRng >> 16) & 0x7FFF; }
static void RbLiveInput(uint32_t F) {
    int r = g_rbRemote;
    FakePkt& q = g_fq[F % 256];
    if (q.frame != F) {                       // "send" the real input (once per frame, also when we wait)
        q.frame = F;
        q.v[0] = g_pend.in[r][0];
        q.v[1] = g_pend.in[r][1];
        q.delivered = false;
        // Arrival is counted in real frames (g_tick also counts frames the
        // game waits), not game frames: test 30 waited forever because the
        // game frame count stops while waiting.
        uint32_t a = (g_tick + 1) + (uint32_t)g_cfg.fakeLag + (g_cfg.fakeJitter ? RbRand() % (uint32_t)(g_cfg.fakeJitter + 1) : 0);
        if (g_cfg.fakeLoss > 0 && (int)(RbRand() % 100) < g_cfg.fakeLoss) a += 4;
        q.arrive = a;
        g_rbSent++;
    }
    const ConfIn& c = g_cf[F % 256];
    const uint32_t* use = c.frame == F ? c.v : g_rbLast;
    g_pend.in[r][0] = use[0];
    g_pend.in[r][1] = use[1];
}

// Build 54 (test 53): some game code (the assist set menu, the replay
// recorder) reads the pads' raw device buttons instead of the fed input, so
// online each PC saw its own physical controllers there: only the desktop's
// player 1 could pick an assist set, and only on the desktop. Online, the raw
// buttons of every pad are now set to the fed input too (players' pads) or
// nothing (other pads). The real device values are kept and put back right
// before the next device read, so reading the controllers works as before.
static uint32_t g_physRaw[addr::PAD_COUNT][2];
static bool g_rawOverridden = false;
static bool NetOn();
static void SetExternal(const uint32_t in[2][2]) {
    for (uint32_t i = 0; i < addr::PAD_COUNT; i++) {
        PadU32(i, addr::PAD_EXT0) = 0;
        PadU32(i, addr::PAD_EXT1) = 0;
    }
    bool raw = NetOn();
    if (raw && g_inPhase == 1) {
        for (uint32_t i = 0; i < addr::PAD_COUNT; i++) {
            g_physRaw[i][0] = PadU32(i, addr::PAD_RAW0);
            g_physRaw[i][1] = PadU32(i, addr::PAD_RAW1);
        }
        g_rawOverridden = true;
    }
    if (raw)
        for (uint32_t i = 0; i < addr::PAD_COUNT; i++) {
            PadU32(i, addr::PAD_RAW0) = 0;
            PadU32(i, addr::PAD_RAW1) = 0;
        }
    for (int p = 0; p < 2; p++) {
        uint32_t d = PadOfPlayer(p);
        if (d >= addr::PAD_COUNT) continue;
        PadU32(d, addr::PAD_EXT0) = in[p][0];
        PadU32(d, addr::PAD_EXT1) = in[p][1];
        if (raw) {
            PadU32(d, addr::PAD_RAW0) = in[p][0];
            PadU32(d, addr::PAD_RAW1) = in[p][1];
        }
    }
}
// Replaces the call to the device read (pads 0-7) inside the input code.
static void __cdecl H_DevRead() {
    if (g_inPhase == 2) return;                   // replay: answered in H_KbRead
    if (g_rawOverridden) {                        // build 54: the real device values back first
        for (uint32_t i = 0; i < addr::PAD_COUNT; i++) {
            PadU32(i, addr::PAD_RAW0) = g_physRaw[i][0];
            PadU32(i, addr::PAD_RAW1) = g_physRaw[i][1];
        }
        g_rawOverridden = false;
    }
    if (g_inPhase == 1) CopyRanges(g_devBefore);
    reinterpret_cast<VoidFn>(addr::FN_DEVREAD)();
}
// Replaces the call to the keyboard read, which comes right after it. This
// is the spot where the devices have been read and the "held / pressed"
// work has not started yet.
static void __cdecl H_KbRead() {
    if (g_inPhase == 2) {
        ApplyRuns(g_inReplay->dev);
        SetExternal(g_inReplay->in);
        g_devDone = true;
        return;
    }
    reinterpret_cast<VoidFn>(addr::FN_KBREAD)();
    if (g_inPhase != 1) return;
    RangeRuns(g_devBefore, nullptr, g_pend.dev);
    for (int p = 0; p < 2; p++) {
        uint32_t d = PadOfPlayer(p);
        g_pend.in[p][0] = d < addr::PAD_COUNT ? PadU32(d, addr::PAD_RAW0) : 0;
        g_pend.in[p][1] = d < addr::PAD_COUNT ? PadU32(d, addr::PAD_RAW1) : 0;
        if (d != g_inLastMap[p]) {
            Log("input: player %d now uses pad %u%s", p + 1, d, d >= addr::PAD_COUNT ? " (none: CPU or no controller)" : "");
            g_inLastMap[p] = d;
        }
    }
    if (g_netInject) NetFill(g_frame + 1);
    else if (g_rbOn) RbLiveInput(g_frame + 1);
    SetExternal(g_pend.in);
    g_devDone = true;
}
static void FlagAllPads() {
    for (uint32_t i = 0; i < addr::PAD_COUNT; i++) Pad(i)[addr::PAD_EXT_FLAG] = 1;
}
static void UnflagAllPads() {
    for (uint32_t i = 0; i < addr::PAD_COUNT; i++) Pad(i)[addr::PAD_EXT_FLAG] = 0;
}
// Replaces the frame loop's call to the input function.
static void __cdecl H_Input() {
    if (!OnGameThread()) {
        reinterpret_cast<VoidFn>(addr::FN_INPUT)();
        return;
    }
    NetAct na = NetBeforeInput();
    if (na == NetAct::Stall) {   // online: the other player's input for this frame isn't here yet
        g_netStall = true;
        NetAfterInput(false);
        return;
    }
    g_netStall = false;
    if (na == NetAct::Free && g_mode != Mode::SyncTest) {
        reinterpret_cast<VoidFn>(addr::FN_INPUT)();
        UiAfterGameInput();          // build 57: the overlay reads the controllers and keeps the game's input off while it has focus
        NetAfterInput(false);
        return;
    }
    g_pend.valid = false;
    CopyRanges(g_pend.pre);
    if (na == NetAct::Inject) NetSavePre(g_pend.pre);
    FlagAllPads();
    g_devDone = false;
    g_inPhase = 1;
    g_netInject = na == NetAct::Inject;
    RunInputStep();
    g_netInject = false;
    g_inPhase = 0;
    if (!g_devDone) {            // the game took another path (online mode?): leave the pads alone
        UnflagAllPads();
        g_inNoDev++;
        NetAfterInput(false);
        return;
    }
    CopyRanges(g_pend.post);
    g_pend.frame = g_frame + 1;  // the update that follows
    g_pend.valid = true;
    NetAfterInput(na == NetAct::Inject);
}
// Replay: work this frame's pads out again from the recorded inputs.
// resim = rollback with possibly other inputs: no check, and the result
// becomes this frame's new record.
static bool ReplayInput(Snapshot& sn, bool first, uint32_t frame, bool resim = false) {
    if (first) {
        WriteRanges(sn.inp.pre);
    } else {
        ApplyRuns(sn.padDelta);
        if (resim) CopyRanges(sn.inp.pre);
    }
    FlagAllPads();
    g_devDone = false;
    g_inReplay = &sn.inp;
    g_inPhase = 2;
    RunInputStep();
    g_inPhase = 0;
    g_inReplay = nullptr;
    if (!g_devDone) UnflagAllPads();
    if (resim) {
        CopyRanges(sn.inp.post);
        ApplyRuns(sn.postDelta);
        return g_devDone;
    }
    // Check: the pads must come out exactly as in the first run.
    std::vector<uint8_t> runs;
    RangeRuns(sn.inp.post, nullptr, runs);
    bool ok = runs.empty() && g_devDone;
    if (!ok) {
        g_inputMiss++;
        if (g_inputMissLogs < 20) {
            g_inputMissLogs++;
            Log("  INPUT REPLAY MISMATCH frame %u%s: %u bytes of pad memory differ", frame, g_devDone ? "" : " (input code took another path)",
                (unsigned)runs.size());
            const uint8_t* d = runs.data();
            const uint8_t* end = d + runs.size();
            for (int k = 0; k < 6 && d + 8 <= end; k++) {
                uint32_t a, n;
                memcpy(&a, d, 4);
                memcpy(&n, d + 4, 4);
                Log("    0x%08X +%u", a, n);
                d += 8 + n;
            }
        }
    }
    ApplyRuns(sn.postDelta);
    return ok;
}

// Save-cost measurement (build 29): how long saving one frame and a full
// rewind + replay take on this PC.
static double g_qpcMs = 0;
static inline double NowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (g_qpcMs == 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcMs = 1000.0 / (double)f.QuadPart; }
    return (double)t.QuadPart * g_qpcMs;
}
struct Timing { double sum = 0, max = 0; uint32_t n = 0; void Add(double v) { sum += v; n++; if (v > max) max = v; } };
static Timing g_tSave, g_tReplay, g_tUpdate;
static void LogTimings(const char* when) {
    Log("  timing (%s): save one frame %.2f ms avg / %.2f max; rewind + replay of %d frames %.2f ms avg / %.2f max; one normal update %.2f ms avg (%u frames)",
        when, g_tSave.n ? g_tSave.sum / g_tSave.n : 0.0, g_tSave.max, g_cfg.depth, g_tReplay.n ? g_tReplay.sum / g_tReplay.n : 0.0,
        g_tReplay.max, g_tUpdate.n ? g_tUpdate.sum / g_tUpdate.n : 0.0, g_tSave.n);
}

static void RbResetStats();
static void RbLogStats(const char* when);
// ---------------------------------------------------------------------------
// Build 37: hardware write watch on the pool fields that differ in tests 35-36
// (3.2 MB fight pool block +0x208, +0x220, +0x228: stack addresses written in
// replays only). Logs which code writes them, to find the uninitialised value.
// ---------------------------------------------------------------------------
struct HwHit { uint32_t eip, count, nzFirst, nzReplay, lastVal, field; uint32_t stack[10]; };
static HwHit g_hw[64];
static uint32_t g_hwN = 0, g_hwAddr[3] = {}, g_hwTotal = 0;
static bool g_hwOn = false, g_hwHandler = false;
static const DWORD HW_SET = 0xE0DE0037, HW_CLEAR = 0xE0DE0038;
static LONG CALLBACK HwHandler(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    CONTEXT* c = e->ContextRecord;
    if (code == HW_SET || code == HW_CLEAR) {
        bool on = code == HW_SET;
        c->Dr0 = on ? g_hwAddr[0] : 0;
        c->Dr1 = on ? g_hwAddr[1] : 0;
        c->Dr2 = on ? g_hwAddr[2] : 0;
        c->Dr3 = 0;
        // L0-L2 enabled, RW = 01 (write), LEN = 11 (4 bytes) for each.
        c->Dr7 = on ? (0x15u | (0xDu << 16) | (0xDu << 20) | (0xDu << 24)) : 0;
        c->Dr6 = 0;
        c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_SINGLE_STEP || !g_hwOn) return EXCEPTION_CONTINUE_SEARCH;
    DWORD dr6 = c->Dr6;
    if (!(dr6 & 7)) return EXCEPTION_CONTINUE_SEARCH;
    int f = (dr6 & 1) ? 0 : (dr6 & 2) ? 1 : 2;
    uint32_t val = *reinterpret_cast<uint32_t*>(g_hwAddr[f]);
    uint32_t eip = c->Eip;
    g_hwTotal++;
    uint32_t k = 0;
    for (; k < g_hwN; k++) if (g_hw[k].eip == eip && g_hw[k].field == (uint32_t)f) break;
    if (k == g_hwN && g_hwN < 64) {
        HwHit& h = g_hw[g_hwN++];
        memset(&h, 0, sizeof h);
        h.eip = eip; h.field = f;
        uint32_t* sp = (uint32_t*)c->Esp;
        uint32_t* top = (uint32_t*)__readfsdword(4);
        int m = 0;
        for (int i = 0; i < 2048 && sp + i < top && m < 10; i++) { uint32_t v = sp[i]; if (v >= 0x401000 && v < 0x65C000) h.stack[m++] = v; }
    }
    if (k < 64) {
        g_hw[k].count++;
        if (val) { g_hw[k].lastVal = val; if (g_jmode == JMode::Replay || g_jmode == JMode::Resim) g_hw[k].nzReplay++; else g_hw[k].nzFirst++; }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void HwStart() {
    if (!OnGameThread()) return;
    uint32_t blk = 0;
    for (uint32_t b : g_stateBlocks) if (g_blocks[b].size == 3270964) blk = g_blocks[b].start;
    if (!blk) { Log("  write watch: fight pool block not found"); return; }
    g_hwAddr[0] = blk + 0x208; g_hwAddr[1] = blk + 0x220; g_hwAddr[2] = blk + 0x228;
    if (!g_hwHandler) { AddVectoredExceptionHandler(1, HwHandler); g_hwHandler = true; }
    g_hwN = 0; g_hwTotal = 0;
    g_hwOn = true;
    RaiseException(HW_SET, 0, 0, nullptr);
    Log("  write watch on 0x%08X 0x%08X 0x%08X (fight pool +0x208 +0x220 +0x228)", g_hwAddr[0], g_hwAddr[1], g_hwAddr[2]);
}
static void HwStop() {
    if (!g_hwOn || !OnGameThread()) return;
    RaiseException(HW_CLEAR, 0, 0, nullptr);
    g_hwOn = false;
    Log("  write watch: %u writes", g_hwTotal);
    for (uint32_t k = 0; k < g_hwN; k++) {
        const HwHit& h = g_hw[k];
        char line[256];
        int n = snprintf(line, sizeof line, "    +0x%X written before %08X x%u (nonzero: %u in first runs, %u in replays; last %08X) |", (h.field == 0 ? 0x208u : h.field == 1 ? 0x220u : 0x228u), h.eip, h.count, h.nzFirst, h.nzReplay, h.lastVal);
        for (int i = 0; i < 10 && h.stack[i]; i++) n += snprintf(line + n, sizeof line - n, " %08X", h.stack[i]);
        Log("%s", line);
    }
}

static void DtLogStats() {
    if (!dt::saves) { Log("  changed memory: tracking %s, nothing saved", g_cfg.dirtyTracking ? "on" : "OFF"); return; }
    Log("  changed memory (tracking %s): %.0f KB copied per save (%llu saves), %.0f KB per restore (%llu), %llu page faults, %llu pages armed, %llu hot pages now, %llu protect failures, %llu checks of the tracking, %llu missed writes",
        dt::active ? "on" : "off", double(dt::copied) / dt::saves / 1024.0, (unsigned long long)dt::saves,
        dt::restores ? double(dt::rcopied) / dt::restores / 1024.0 : 0.0, (unsigned long long)dt::restores,
        (unsigned long long)dt::faults, (unsigned long long)dt::arms, (unsigned long long)dt::hotNow,
        (unsigned long long)dt::protFails, (unsigned long long)dt::audits, (unsigned long long)dt::missed);
}
// Build 49: the state at the start of a test, without a profile.
static void BuildFixedState() {
    g_state.clear();
    g_stateBlocks.clear();
    std::vector<uint8_t> useBlock(g_blocks.size(), 0);
    auto blockOf = [](uint32_t p) -> int {
        auto it = std::upper_bound(g_blocks.begin(), g_blocks.end(), p, [](uint32_t v, const Block& b) { return v < b.start; });
        if (it == g_blocks.begin()) return -1;
        --it;
        return p < it->start + it->size ? int(it - g_blocks.begin()) : -1;
    };
    uint32_t pools = 0, known = 0, carried = 0;
    if (g_cfg.heap && g_cfg.heapRestore) {
        for (uint32_t ptrAddr : addr::POOL_PTRS) {
            int b = blockOf(*reinterpret_cast<volatile uint32_t*>(ptrAddr));
            if (b < 0 || useBlock[b] || g_blocks[b].size > (32u << 20) || !g_own.Has(g_blocks[b].start)) continue;
            useBlock[b] = 1;
            pools++;
        }
        for (uint32_t b = 0; b < g_blocks.size(); b++) {
            if (useBlock[b]) continue;
            uint32_t n = g_blocks[b].size;
            bool k = std::find(std::begin(addr::FIGHT_BLOCK_SIZES), std::end(addr::FIGHT_BLOCK_SIZES), n) != std::end(addr::FIGHT_BLOCK_SIZES) ||
                     std::find(g_heapKeptSizes.begin(), g_heapKeptSizes.end(), n) != g_heapKeptSizes.end();
            if (!k || !g_own.Has(g_blocks[b].start)) continue;
            useBlock[b] = 1;
            known++;
        }
        JLock lock;
        for (auto& c : g_carry) {               // blocks from the previous test are rewound as new blocks already
            int b = FindBlock(c.ptr);
            if (b >= 0 && useBlock[b]) { useBlock[b] = 0; carried++; }
        }
    }
    uint32_t dataPages = 0;
    for (uint32_t i = 0; i < g_chunks.size(); i++) {
        const Chunk& c = g_chunks[i];
        if (c.block >= 0) {
            if (!useBlock[c.block] || !ChunkAlive(c)) continue;
            g_state.push_back(i);
            if (g_stateBlocks.empty() || g_stateBlocks.back() != (uint32_t)c.block) g_stateBlocks.push_back(c.block);
            continue;
        }
        bool isPart = c.addr == addr::PART_RANGE.start;
        bool fixed = false;
        for (auto& r : addr::FIXED_RANGES) if (c.addr < r.end && r.start < c.addr + c.len) fixed = true;
        if (!isPart && !fixed && !InInputRanges(c.addr, c.len) && !InForceRanges(c.addr, c.len)) continue;
        if (NeverRestore(c.addr)) continue;
        g_state.push_back(i);
        dataPages++;
    }
    g_stateBytes = 0;
    for (uint32_t i : g_state) g_stateBytes += g_chunks[i].len;
    Log("  state from the fixed list: %u .data pages (%u found in earlier fights), %u heap blocks (%u object pools, %u known fight blocks%s), %.2f MB",
        dataPages, (unsigned)g_huntKept.size(), (unsigned)g_stateBlocks.size(), pools, known,
        carried ? ", some left to the new-block tracking" : "", g_stateBytes / 1048576.0);
}

static bool SyncTestStart() {
    g_auditFirst = 120;
    DtDisarm();
    dt::faults = dt::arms = dt::copied = dt::saves = dt::audits = dt::missed = dt::restores = dt::rcopied = dt::protFails = 0;
    Log("  changed-memory tracking is %s (F6 switches it)", g_cfg.dirtyTracking ? "on" : "OFF");
    uint32_t contested = 0, never = 0, dead = 0, threaded = 0, threadBlocks = 0;
    bool haveW = false;
    if (!g_cfg.useProfile) {
        BuildChunks();
        BuildFixedState();
    } else {
    if (!g_haveProfile) {
        Log("SYNC TEST needs a profile from this game session first: press F9 during a fight.");
        SetStatus("no profile: press F9 in a fight first");
        return false;
    }
    g_state.clear();
    g_stateBlocks.clear();
    haveW = g_cW.size() == g_chunks.size();
    // Heap: the profile only samples each heap chunk now and then, so it
    // misses parts of a block that change rarely (test 6). So we take whole
    // blocks: every block in which the update changed anything, except
    // blocks that other threads (graphics driver, sound) also write.
    std::vector<uint8_t> useBlock(g_blocks.size(), 0), threadBlock(g_blocks.size(), 0);
    for (uint32_t i = g_firstHeapChunk; i < g_chunks.size(); i++) {
        int b = g_chunks[i].block;
        if (g_cU[i]) useBlock[b] = 1;
        if (haveW && (g_cW[i] & (W_THREAD | W_FOREIGN))) threadBlock[b] = 1;
    }
    uint32_t notOwn = 0, forced = 0;
    for (uint32_t b = 0; b < g_blocks.size(); b++) {
        // Test 27: a fight block was dropped whole because another thread wrote
        // its first bytes once, and the first replay crashed. Now only the
        // chunks another thread wrote are left out; the rest of the block stays.
        if (useBlock[b] && threadBlock[b] && g_cfg.skipThreadChunks) {
            threadBlocks++;
            Log("  heap block 0x%08X (%u bytes): some parts also written by other threads or non-game code; those parts are left out",
                g_blocks[b].start, g_blocks[b].size);
        }
        if (useBlock[b] && g_blocks[b].size > (32u << 20)) useBlock[b] = 0;
        if (useBlock[b] && !g_own.Has(g_blocks[b].start)) {
            useBlock[b] = 0;
            notOwn++;
            Log("  left out heap block 0x%08X (%u bytes): not the game's own memory", g_blocks[b].start, g_blocks[b].size);
        }
    }
    for (uint32_t ptrAddr : addr::POOL_PTRS) {
        uint32_t p = *reinterpret_cast<volatile uint32_t*>(ptrAddr);
        if (!p) continue;
        auto it = std::upper_bound(g_blocks.begin(), g_blocks.end(), p,
                                   [](uint32_t v, const Block& b) { return v < b.start; });
        if (it == g_blocks.begin()) continue;
        --it;
        if (p >= it->start + it->size) continue;
        uint32_t b = uint32_t(it - g_blocks.begin());
        if (useBlock[b]) continue;
        if (!g_own.Has(it->start) || it->size > (32u << 20)) continue;
        useBlock[b] = 1;
        Log("  object pool block 0x%08X (%u bytes, pointer at 0x%08X) added: the profile did not see it change", it->start, it->size, ptrAddr);
    }
    for (uint32_t i = 0; i < g_chunks.size(); i++) {
        const Chunk& c = g_chunks[i];
        bool isInput = c.block < 0 && InInputRanges(c.addr, c.len);
        bool isPart = c.block < 0 && c.addr == addr::PART_RANGE.start;
        bool isForce = c.block < 0 && (isPart || InForceRanges(c.addr, c.len));
        if (isForce && !g_cU[i]) forced++;
        if (c.block >= 0 && g_cfg.wholeBlocks) {
            if (!useBlock[c.block] || !g_cfg.heapRestore) continue;
            if (!ChunkAlive(c)) { dead++; continue; }
            if (haveW && g_cfg.skipThreadChunks && (g_cW[i] & (W_THREAD | W_FOREIGN))) {
                threaded++;
                Log("    left out 0x%08X +%u (block 0x%08X+0x%X)", c.addr, c.len, g_blocks[c.block].start, c.addr - g_blocks[c.block].start);
                continue;
            }
            g_state.push_back(i);
            if (g_stateBlocks.empty() || g_stateBlocks.back() != (uint32_t)c.block) g_stateBlocks.push_back(c.block);
            continue;
        }
        if (!g_cU[i] && !isInput && !isForce) continue;
        if (c.block >= 0 && !g_cfg.heapRestore) continue;
        if (c.block < 0 && NeverRestore(c.addr)) { never++; continue; }
        if (g_cO[i] && !isInput && !isForce && !g_cfg.includeContested) { contested++; continue; }
        if (!ChunkAlive(c)) { dead++; continue; }
        if (haveW && g_cfg.skipThreadChunks && (g_cW[i] & W_THREAD) && !isInput && !isPart) { threaded++; continue; }
        g_state.push_back(i);
        if (c.block >= 0 && (g_stateBlocks.empty() || g_stateBlocks.back() != (uint32_t)c.block))
            g_stateBlocks.push_back(c.block);
    }
    g_stateBytes = 0;
    for (uint32_t i : g_state) g_stateBytes += g_chunks[i].len;
    Log("  sound manager memory: %u extra pages added that the profile did not see change", forced);
    }
    for (uint32_t b : g_stateBlocks)
        Log("  heap block in state: 0x%08X, %u bytes", g_blocks[b].start, g_blocks[b].size);
    uint32_t inputBytes = 0;
    for (auto& r : addr::INPUT_RANGES) inputBytes += r.end - r.start;
    g_ring.assign(g_cfg.depth, Snapshot());
    {
        JLock lock;
        g_journals.assign(g_cfg.depth, Journal());
        g_dyn.clear();
        for (auto& b : g_carry) if (g_own.Has(b.ptr)) g_dyn.push_back(b);
        if (!g_carry.empty())
            Log("  kept %u heap blocks the game allocated during the previous test (%u no longer exist)",
                (unsigned)g_dyn.size(), (unsigned)(g_carry.size() - g_dyn.size()));
        g_carry.clear();
        g_jOutside = nullptr;
    }
    g_jActive = true;
    g_allocMiss = g_reallocs = g_released = 0;
    for (auto& s : g_ring) { s.mem.resize(g_stateBytes); s.input.resize(inputBytes); }
    g_truth.mem.resize(g_stateBytes);
    g_truth.input.resize(inputBytes);
    g_ringCount = 0;
    g_testFrames = g_desyncs = g_diffEvents = g_skips = 0;
    g_diffStats.clear();
    g_diffStatsDropped = 0;
    g_sbReplayed = g_sbMiss = g_sbRecorded = 0;
    g_deltaRuns = g_deltaBytes = g_deltaFrames = g_deltaSkips = 0;
    g_truthFrame = 0xFFFFFFFFu;
    g_cueStateMiss = g_cueReplayDestroys = g_cueReplayCalls = g_cueDeferred = 0;
    g_randMissFrames = g_randMissLogs = 0; g_randOutside = 0; g_randMissPairs.clear();
    g_clockMiss = 0;
    g_inputMiss = g_inputMissLogs = g_inSkips = g_inNoDev = g_inFrames = 0;
    g_inActive[0] = g_inActive[1] = 0;
    g_inLastMap[0] = g_inLastMap[1] = 0xFFFFFFFFu;
    g_pend.valid = false;
    g_fileOpens = g_fileReplayed = g_fileMiss = g_fileCloseDeferred = g_fileClosed = 0;
    g_fileReads = g_fileReadsReplayed = g_fileReadMiss = 0;
    g_tSave = Timing(); g_tReplay = Timing(); g_tUpdate = Timing();
    ClearApiSites();
    g_mode = Mode::SyncTest;
    g_jAll = &g_journals;
    g_rbOn = g_rbWant;
    if (g_rbOn) {
        g_rbStarted = false;
        g_rbWrong = RB_NONE;
        g_rbLast[0] = g_rbLast[1] = 0;
        g_rbRemote = NetOn() ? NetRemoteSide() : g_cfg.fakeRemote - 1;
        for (auto& q : g_fq) q = FakePkt();
        for (auto& c : g_cf) c = ConfIn();
        g_rbSent = g_rbGuessRight = g_rbGuessWrong = 0;
        RbResetStats();
        if (NetOn()) Log("ROLLBACK start (online): player %d's input comes over the network, rewinds up to %d frames, full replay check every %d frames (0 = off)",
                         g_rbRemote + 1, g_cfg.depth, g_cfg.onlineVerifyEvery);
        else Log("ROLLBACK TEST start: player %d's input arrives %d frames late (+0..%d random, %d%% extra late), rewinds up to %d frames, check every %d frames",
            g_cfg.fakeRemote, g_cfg.fakeLag, g_cfg.fakeJitter, g_cfg.fakeLoss, g_cfg.depth, g_cfg.verifyEvery);
    }
    HuntStart();     // build 49: also during rollback (the saved frames stay valid when memory is added)
    if (g_cfg.useProfile)
        Log("SYNC TEST start: depth %d, %u chunks (%.2f MB per snapshot, %u heap blocks). Left out: %u never-restore, %u contested, %u freed, %u written by other threads (%u heap blocks partly used by other threads or by non-game code)%s",
            g_cfg.depth, (unsigned)g_state.size(), g_stateBytes / 1048576.0, (unsigned)g_stateBlocks.size(), never, contested, dead,
            threaded, threadBlocks, haveW ? "" : " (no writer check this session)");
    else
        Log("SYNC TEST start: depth %d, %u chunks (%.2f MB per snapshot at the start, %u heap blocks); more is added when the game writes it",
            g_cfg.depth, (unsigned)g_state.size(), g_stateBytes / 1048576.0, (unsigned)g_stateBlocks.size());
    if (g_cfg.dirtyTracking < 0) HwStart();   // build 38: write watch off (it worked: see test 37)
    return true;
}

static void SyncTestStop(const char* why) {
    Log("SYNC TEST stop (%s): %u frames tested, %u desynced, %u frames skipped, %u clock values missing in replay",
        why, g_testFrames, g_desyncs, g_skips, g_clockMiss);
    LogApiSites("Calls made by the game logic during update (sync test, includes replays)", g_testFrames);
    g_mode = Mode::Idle;
    HwStop();
    if (g_rbOn) RbLogStats("end");
    DtLogStats();
    DtDisarm();
    g_rbOn = false;
    HuntStop();
    ClockReal();
    g_jmode = JMode::Off;
    g_jActive = false;
    {
        JLock lock;
        g_jOutside = nullptr;
    }
    for (auto& j : g_journals) ReleaseFrees(j);    // the game did free these; do it for real now
    {
        JLock lock;               // files: the delayed closes are done; stop tracking the rest
        g_fileDyn.clear();
        g_fileTouched.clear();
    }
    LogSoundStats();
    RandLogStats();
    Log("  input: %u frames with mod-fed input, player 1 pressed something on %u, player 2 on %u; %u replays worked the pads out differently; %u frames not tested (input missing); %u frames the input code took another path",
        g_inFrames, g_inActive[0], g_inActive[1], g_inputMiss, g_inSkips, g_inNoDev);
    Log("  files: %u opened by the game logic (%u handed back in replays, %u replay opens not in the first run), %u closes delayed, %u closed for real, %u reads (%u replayed at the same position, %u not matched)",
        g_fileOpens, g_fileReplayed, g_fileMiss, g_fileCloseDeferred, g_fileClosed, g_fileReads, g_fileReadsReplayed, g_fileReadMiss);
    LogTimings("whole test");
    if (g_cfg.depth > 1)
        Log("  changes outside the update: %u frames, %.1f pieces and %.0f bytes per frame on average, %u frames not tested (missing)",
            g_deltaFrames, g_deltaFrames ? double(g_deltaRuns) / g_deltaFrames : 0.0,
            g_deltaFrames ? double(g_deltaBytes) / g_deltaFrames : 0.0, g_deltaSkips);
    LogDiffStats(60);
    Log("  heap: %u delayed frees done, %u replay allocations went a different way, %u reallocs",
        g_released, g_allocMiss, g_reallocs);
    {
        JLock lock;
        g_journals.clear();
        // Test 22: after a depth change the new test started without the
        // blocks the game had allocated during the old one (a new round's
        // pool), so replays went wrong at once. Keep them for the next test.
        g_carry.clear();
        if (strcmp(why, "depth change") == 0) g_carry = g_dyn;
        g_dyn.clear();
        g_jAll = nullptr;
    }
    g_ring.clear(); g_ring.shrink_to_fit();
    g_truth = Snapshot();
    SetStatus("sync test off: %u frames, %u desyncs", g_testFrames, g_desyncs);
}

static uint32_t SyncTestFrame(void* scene) {
    const int D = g_cfg.depth;
    uint32_t slot = g_frame % D;
    uint32_t sceneVt = *reinterpret_cast<uint32_t*>(scene);

    if (StateBlockFreed()) {   // the game released memory we restore: stop, don't touch it
        SyncTestStop("game freed a heap block that holds fight data");
        return RunUpdate(scene);
    }

    if (!g_cueVt) HookCues();
    if (HuntCollect()) DtEnsure();   // build 49: memory the game wrote for the first time joins the state (saved frames stay valid)
                                     // build 53: and the changed-memory tracking grows with it (test 52 crash)
    // 0) Memory the game freed D frames ago can't be reached by a rewind any
    //    more: free it for real now. This slot is reused for this frame.
    g_stage = 1;
    ReleaseFrees(g_journals[slot]);
    {
        JLock lock;
        g_jOutside = &g_journals[slot];   // frees after this frame's update land here too
    }

    // 1) Save the state before this frame's update (input already applied),
    //    run the real update while recording clock values and heap use.
    g_stage = 2;
    double t0 = NowMs();
    if (D > 1) MakeOutDelta(g_ring[slot]);
    CopyState(g_ring[slot], true);
    g_tSave.Add(NowMs() - t0);
    {
        Snapshot& sn = g_ring[slot];
        sn.inOk = g_pend.valid && g_pend.frame == g_frame;
        sn.padOk = false;
        if (sn.inOk) {
            std::swap(sn.inp, g_pend);
            RangeRuns(sn.inp.post, sn.input.data(), sn.postDelta);   // other code between input and update
            if (g_truthFrame + 1 == g_frame && g_truth.input.size() == sn.inp.pre.size()) {
                RangeRuns(g_truth.input, sn.inp.pre.data(), sn.padDelta);
                sn.padOk = true;
            }
            g_inFrames++;
            for (int p = 0; p < 2; p++) if (sn.inp.in[p][0] || sn.inp.in[p][1]) g_inActive[p]++;
        }
        g_pend.valid = false;
    }
    g_stage = 3;
    ClockRecord(&g_ring[slot].clock);
    g_jcur = &g_journals[slot];
    static std::vector<std::vector<RandCall>> randRing;
    if (randRing.size() != (size_t)D) randRing.assign(D, {});
    randRing[slot].clear();
    g_randCur = &randRing[slot];
    g_randMode = 1;
    g_jmode = JMode::Record;
    double tu = NowMs();
    uint32_t r = RunUpdate(scene);
    g_tUpdate.Add(NowMs() - tu);
    g_jmode = JMode::Off;
    g_randMode = 0;
    ClockReal();
    if (g_ringCount < (uint32_t)D) g_ringCount++;
    // The true result of this frame (also the base for the next frame's
    // outside-the-update changes).
    g_stage = 4;
    CopyState(g_truth, true);
    g_truthFrame = g_frame;
    g_truthBytes = g_stateBytes;

    if (StateBlockFreed()) { SyncTestStop("game freed a heap block that holds fight data"); return r; }
    if (g_reallocs) { SyncTestStop("game resized a heap block (not supported yet)"); return r; }

    // Skip frames where the scene changes or asks to exit.
    void* sceneNow = *reinterpret_cast<void**>(addr::G_MAIN_SCENE);
    if (r != 0 || sceneNow != scene || *reinterpret_cast<uint32_t*>(scene) != sceneVt) {
        g_ringCount = 0; g_skips++;
        return r;
    }
    if (g_ringCount < (uint32_t)D) return r;
    uint32_t first = g_frame - (D - 1);
    for (uint32_t k = first + 1; k <= g_frame; k++)
        if (!g_ring[k % D].deltaOk) { g_deltaSkips++; return r; }
    for (uint32_t k = first; k <= g_frame; k++)
        if (!g_ring[k % D].inOk || (k != first && !g_ring[k % D].padOk)) { g_inSkips++; return r; }

    // 2) Rewind D frames and replay them. Each replayed frame first runs the
    //    game's input code again from the recorded inputs (build 29).
    g_stage = 5;
    double tr = NowMs();
    FilePositionsSave();
    CopyState(g_ring[first % D], false);
    g_stage = 6;
    std::vector<uint8_t> padNow;
    for (uint32_t k = first; k <= g_frame; k++) {
        if (k != first) {
            CopyRanges(padNow);              // pads as the replayed update left them
            ApplyOutDelta(g_ring[k % D]);    // other code between updates (this also wrote the old pad result)
            WriteRanges(padNow);
        }
        ReplayInput(g_ring[k % D], k == first, k);
        ClockReplay(&g_ring[k % D].clock);
        g_jcur = &g_journals[k % D];
        g_jIdx = 0;
        {
            JLock lock;
            for (auto& a : g_jcur->allocs) a.used = 0;
            g_jcur->cueIdx = 0;
            g_jcur->cuePlayIdx = 0;
        }
        g_randCur = &randRing[k % D];
        g_randIdx = 0; g_randBad = false; g_randFrame = k; g_randStep = k - first + 1;
        g_randMode = 2;
        g_jmode = JMode::Replay;
        RunUpdate(scene);
        g_jmode = JMode::Off;
        g_randMode = 0;
        if (!g_randBad && g_randIdx != g_randCur->size()) RandMiss(nullptr);
        ClockReal();
        if (StateBlockFreed()) {
            // Continue from the replayed state; restoring now could write into freed memory.
            SyncTestStop("game freed a heap block during a replay");
            return r;
        }
    }

    // 3) Compare with the truth, then continue from the truth either way.
    bool same = (t_seed == g_truth.seed);
    const uint8_t* t = g_truth.mem.data();
    for (uint32_t ci : g_state) {
        const Chunk& c = g_chunks[ci];
        if (same && ChunkAlive(c) && memcmp((void*)c.addr, t, c.len) != 0) same = false;
        t += c.len;
    }
    g_stage = 7;
    if (same && DynDiff(false) != 0) same = false;
    g_testFrames++;
    if (!same) { g_desyncs++; ReportDiff(g_frame); }
    g_stage = 8;
    CopyState(g_truth, false);
    FilePositionsRestore();
    g_tReplay.Add(NowMs() - tr);
    g_stage = 0;

    if (g_testFrames % 600 == 0)
        Log("sync test: %u frames tested, %u desynced", g_testFrames, g_desyncs);
    if (g_testFrames % 600 == 0) LogTimings("so far");
    if (g_testFrames % 3000 == 0) { LogSoundStats(); RandLogStats(); if (g_desyncs) LogDiffStats(25); }
    SetStatus("SYNC TEST depth %d | %u frames | %u desyncs | %u input", D, g_testFrames, g_desyncs, g_inputMiss);
    return r;
}


// ---------------------------------------------------------------------------
// Rollback engine (build 30, step 3b)
// Every frame: deliver the inputs that "arrived", save the frame, and if a
// guess for an earlier frame was wrong, rewind to that frame and re-run up
// to now with the real inputs (and fresh guesses after them). If the other
// player's input is so late that the frame it belongs to would drop out of
// the saved frames, the game waits a frame instead.
// ---------------------------------------------------------------------------
static uint32_t g_rbRollbacks = 0, g_rbResim = 0, g_rbMaxDepth = 0, g_rbStalls = 0, g_rbLate = 0;
static uint32_t g_rbVerifies = 0, g_rbVerifyBad = 0, g_rbOldFreed = 0, g_rbOldCues = 0, g_rbOldFiles = 0, g_rbFrames = 0;
static uint32_t g_rbDepthHist[9];
static uint32_t g_rbDetailLogs = 0;
static Timing g_tRb, g_tVerify;
static void RbResetStats() {
    g_rbRollbacks = g_rbResim = g_rbMaxDepth = g_rbStalls = g_rbLate = 0;
    g_rbVerifies = g_rbVerifyBad = g_rbOldFreed = g_rbOldCues = g_rbOldFiles = g_rbFrames = 0;
    g_rbDetailLogs = 0;
    memset(g_rbDepthHist, 0, sizeof g_rbDepthHist);
    g_tRb = Timing(); g_tVerify = Timing();
}
static void RbLogStats(const char* when) {
    Log("ROLLBACK TEST %s: %u frames, %u rollbacks (%u frames re-run, deepest %u), %u guesses right, %u wrong, %u waits, %u inputs too late to fix",
        when, g_rbFrames, g_rbRollbacks, g_rbResim, g_rbMaxDepth, g_rbGuessRight, g_rbGuessWrong, g_rbStalls, g_rbLate);
    Log("  rollback depth: 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u 8:%u", g_rbDepthHist[1], g_rbDepthHist[2], g_rbDepthHist[3],
        g_rbDepthHist[4], g_rbDepthHist[5], g_rbDepthHist[6], g_rbDepthHist[7], g_rbDepthHist[8]);
    Log("  checks: %u (rewind %d frames and replay with the final inputs), %u did NOT match", g_rbVerifies, g_cfg.depth, g_rbVerifyBad);
    Log("  released from abandoned runs: %u heap blocks, %u sound cues, %u files", g_rbOldFreed, g_rbOldCues, g_rbOldFiles);
    Log("  timing: rollback %.2f ms avg / %.2f max (%u); check %.2f ms avg / %.2f max; save %.2f ms avg; update %.2f ms avg",
        g_tRb.n ? g_tRb.sum / g_tRb.n : 0.0, g_tRb.max, g_tRb.n, g_tVerify.n ? g_tVerify.sum / g_tVerify.n : 0.0, g_tVerify.max,
        g_tSave.n ? g_tSave.sum / g_tSave.n : 0.0, g_tUpdate.n ? g_tUpdate.sum / g_tUpdate.n : 0.0);
    DtLogStats();
}

// Deliver fake-network inputs that have arrived by frame F; note wrong guesses.
static void RbDeliver(uint32_t F) {
    const int D = g_cfg.depth;
    int r = g_rbRemote;
    for (auto& q : g_fq) {
        if (q.frame == RB_NONE || q.delivered || q.arrive > g_tick) continue;
        q.delivered = true;
        ConfIn& c = g_cf[q.frame % 256];
        c.frame = q.frame;
        c.v[0] = q.v[0];
        c.v[1] = q.v[1];
        if (!g_rbStarted || q.frame < g_rbStart) continue;
        const uint32_t* used = nullptr;
        if (q.frame == F) {
            if (g_pend.valid && g_pend.frame == F) used = g_pend.in[r];
        } else if (q.frame + (uint32_t)D > F && g_ring[q.frame % D].frameNo == q.frame) {
            used = g_ring[q.frame % D].inp.in[r];
        } else {
            g_rbLate++;
            if (g_rbLate <= 10) Log("  rollback: input for frame %u arrived at frame %u, too late to rewind", q.frame, F);
        }
        if (!used) continue;
        if (used[0] == c.v[0] && used[1] == c.v[1]) {
            g_rbGuessRight++;
        } else {
            g_rbGuessWrong++;
            if (g_rbWrong == RB_NONE || q.frame < g_rbWrong) g_rbWrong = q.frame;
        }
    }
    while (g_cf[g_rbConf % 256].frame == g_rbConf) {
        g_rbLast[0] = g_cf[g_rbConf % 256].v[0];
        g_rbLast[1] = g_cf[g_rbConf % 256].v[1];
        g_rbConf++;
    }
}

static void RbAttachInput(Snapshot& sn, uint32_t F) {
    sn.frameNo = F;
    sn.resimAt = RB_NONE;
    sn.inOk = g_pend.valid && g_pend.frame == F;
    sn.padOk = false;
    if (sn.inOk) {
        std::swap(sn.inp, g_pend);
        RangeRuns(sn.inp.post, sn.input.data(), sn.postDelta);
        if (g_truthFrame + 1 == F && g_truth.input.size() == sn.inp.pre.size()) {
            RangeRuns(g_truth.input, sn.inp.pre.data(), sn.padDelta);
            sn.padOk = true;
        }
        g_inFrames++;
        for (int p = 0; p < 2; p++) if (sn.inp.in[p][0] || sn.inp.in[p][1]) g_inActive[p]++;
    }
    g_pend.valid = false;
}

static uint32_t RbRecordUpdate(void* scene, Snapshot& sn, Journal& j) {
    ClockRecord(&sn.clock);
    NetFpCapture(sn.frameNo);
    g_jcur = &j;
    g_jmode = JMode::Record;
    double tu = NowMs();
    uint32_t r = RunUpdate(scene);
    g_tUpdate.Add(NowMs() - tu);
    g_jmode = JMode::Off;
    ClockReal();
    return r;
}

// After a frame was re-run: release what only the abandoned run had.
static void RbReleaseOld(Journal& old) {
    JLock lock;
    HANDLE heap = CrtHeap();
    for (auto& a : old.allocs) {
        if (a.used || DynContains(a.ptr)) continue;
        if (g_jAll)
            for (auto& j : *g_jAll) { auto& f = j.frees; f.erase(std::remove(f.begin(), f.end(), a.ptr), f.end()); }
        g_own.Erase(a.ptr);
        o_HeapFree(heap, 0, (LPVOID)a.ptr);
        g_rbOldFreed++;
    }
    for (size_t i = 0; i < old.cuePlays.size(); i++) {
        uint32_t c = old.cuePlays[i].cue;
        bool used = i < old.cuePlayUsed.size() && old.cuePlayUsed[i];
        if (used || !c || CuePending(c)) continue;
        if (std::find(g_resimNewCues.begin(), g_resimNewCues.end(), c) != g_resimNewCues.end()) continue;
        o_CueDestroy((void*)c);
        g_rbOldCues++;
    }
    for (size_t i = 0; i < old.fileOpens.size(); i++) {
        uint32_t h = old.fileOpens[i];
        bool used = i < old.fileOpenUsed.size() && old.fileOpenUsed[i];
        if (used || !h || h == (uint32_t)INVALID_HANDLE_VALUE || !VecHas(g_fileDyn, h)) continue;
        if (g_jAll) for (auto& j : *g_jAll) VecDel(j.fileCloses, h);
        CloseForReal(h);
        g_rbOldFiles++;
    }
}

// Rewind to frame w and re-run up to and including frame F (the current one).
static uint32_t RbRollback(void* scene, uint32_t w, uint32_t F) {
    const int D = g_cfg.depth;
    double t = NowMs();
    g_stage = 5;
    {
        // Sound cues the abandoned run deleted in these frames are alive again
        // until the new run deletes them itself.
        JLock lock;
        for (uint32_t k = w; k < F; k++)
            for (uint32_t cd : g_journals[k % D].cueDestroys) VecDel(g_cuePending, cd);
    }
    CopyState(g_ring[w % D], false);
    std::vector<uint8_t> padNow;
    uint32_t r = 0;
    int rp = g_rbRemote;
    for (uint32_t k = w; k <= F; k++) {
        Snapshot& sn = g_ring[k % D];
        g_stage = 6;
        if (k != w) {
            CopyRanges(padNow);
            ApplyOutDelta(sn);
            WriteRanges(padNow);
        }
        const ConfIn& c = g_cf[k % 256];
        const uint32_t* use = c.frame == k ? c.v : g_rbLast;
        sn.inp.in[rp][0] = use[0];
        sn.inp.in[rp][1] = use[1];
        ReplayInput(sn, k == w, k, true);
        CopyState(sn, true);
        sn.resimAt = F;
        if (k == F) {
            r = RbRecordUpdate(scene, sn, g_journals[k % D]);
            break;
        }
        Journal old = std::move(g_journals[k % D]);
        g_journals[k % D] = Journal();
        {
            JLock lock;
            for (uint32_t cd : old.cueDestroys) VecDel(g_cuePending, cd);   // alive again in the new run
            for (auto& a : old.allocs) a.used = 0;
            old.cueIdx = old.cuePlayIdx = old.fileIdx = old.readIdx = 0;
            old.cuePlayUsed.assign(old.cuePlays.size(), 0);
            old.fileOpenUsed.assign(old.fileOpens.size(), 0);
            g_resimNewCues.clear();
        }
        TimeRec oldClock = sn.clock;
        ClockResim(&sn.clock, &oldClock);
        g_jold = &old;
        g_jcur = &g_journals[k % D];
        g_jIdx = 0;
        g_jmode = JMode::Resim;
        NetFpCapture(k);
        RunUpdate(scene);
        g_jmode = JMode::Off;
        ClockReal();
        g_jold = nullptr;
        RbReleaseOld(old);
        g_rbResim++;
    }
    uint32_t depth = F - w;
    g_rbRollbacks++;
    if (g_rbRollbacks <= 40) Log("  rollback at frame %u: back to frame %u (%u frames re-run)", F, w, depth);
    if (depth > g_rbMaxDepth) g_rbMaxDepth = depth;
    if (depth <= 8) g_rbDepthHist[depth]++;
    g_tRb.Add(NowMs() - t);
    return r;
}

// Check: rewind D frames and replay them exactly (inputs as finally used).
// If the rollbacks kept the saved frames and records consistent, this
// matches the current state byte for byte.
// Build 31: when a check fails, run it again and compare the state before
// every replayed frame with the saved state of that frame, to find the first
// frame where the replay and the saved frames part ways.
static bool RbCompareWith(const Snapshot& sn, uint32_t k) {
    const uint8_t* tp = sn.mem.data();
    uint32_t shown = 0, ranges = 0;
    bool same = (t_seed == sn.seed);
    if (!same) Log("    frame %u: random seed differs (saved %08X, replay %08X)", k, sn.seed, t_seed);
    for (uint32_t ci : g_state) {
        const Chunk& c = g_chunks[ci];
        if (ChunkAlive(c) && memcmp((void*)c.addr, tp, c.len) != 0) {
            same = false;
            const uint8_t* l = reinterpret_cast<const uint8_t*>(c.addr);
            for (uint32_t i = 0; i < c.len; i++) {
                if (l[i] == tp[i]) continue;
                ranges++;
                if (shown < 12) {
                    char where[96];
                    if (c.block >= 0) _snprintf(where, sizeof where, "heap 0x%08X (block 0x%08X+0x%X)", c.addr + i, g_blocks[c.block].start, c.addr + i - g_blocks[c.block].start);
                    else _snprintf(where, sizeof where, ".data 0x%08X", c.addr + i);
                    Log("    frame %u: %s saved %02X, replay %02X", k, where, tp[i], l[i]);
                    shown++;
                }
            }
        }
        tp += c.len;
    }
    if (!same) Log("    frame %u: %u bytes differ in total", k, ranges);
    return same;
}
static void RbVerifyDetail(void* scene, uint32_t F) {
    const int D = g_cfg.depth;
    uint32_t first = F - (D - 1);
    Log("  CHECK DETAIL for frame %u: frames %u..%u, re-run by rollbacks at:", F, first, F);
    for (uint32_t k = first; k <= F; k++) {
        const Snapshot& sn = g_ring[k % D];
        if (sn.resimAt != RB_NONE) Log("    frame %u re-run by the rollback at frame %u", k, sn.resimAt);
    }
    FilePositionsSave();
    CopyState(g_ring[first % D], false);
    std::vector<uint8_t> padNow;
    for (uint32_t k = first; k <= F; k++) {
        Snapshot& sn = g_ring[k % D];
        if (k != first) { CopyRanges(padNow); ApplyOutDelta(sn); WriteRanges(padNow); }
        ReplayInput(sn, k == first, k);
        if (k != first && !RbCompareWith(sn, k)) {
            Log("    -> first difference before frame %u's update (so frame %u's replay differs from the run that saved frame %u)", k, k - 1, k);
            break;
        }
        ClockReplay(&sn.clock);
        g_jcur = &g_journals[k % D];
        g_jIdx = 0;
        {
            JLock lock;
            for (auto& a : g_jcur->allocs) a.used = 0;
            g_jcur->cueIdx = 0; g_jcur->cuePlayIdx = 0; g_jcur->fileIdx = 0; g_jcur->readIdx = 0;
        }
        g_jmode = JMode::Replay;
        RunUpdate(scene);
        g_jmode = JMode::Off;
        ClockReal();
        if (k == F) Log("    no difference before any frame: only the last frame's update differs");
    }
    CopyState(g_truth, false);
    FilePositionsRestore();
}
static void RbVerify(void* scene, uint32_t F) {
    const int D = g_cfg.depth;
    uint32_t first = F - (D - 1);
    for (uint32_t k = first; k <= F; k++) {
        const Snapshot& sn = g_ring[k % D];
        if (sn.frameNo != k || !sn.inOk) return;
        if (k != first && (!sn.deltaOk || !sn.padOk)) return;
    }
    double t = NowMs();
    FilePositionsSave();
    CopyState(g_ring[first % D], false);
    std::vector<uint8_t> padNow;
    for (uint32_t k = first; k <= F; k++) {
        Snapshot& sn = g_ring[k % D];
        if (k != first) { CopyRanges(padNow); ApplyOutDelta(sn); WriteRanges(padNow); }
        ReplayInput(sn, k == first, k);
        ClockReplay(&sn.clock);
        g_jcur = &g_journals[k % D];
        g_jIdx = 0;
        {
            JLock lock;
            for (auto& a : g_jcur->allocs) a.used = 0;
            g_jcur->cueIdx = 0;
            g_jcur->cuePlayIdx = 0;
            g_jcur->fileIdx = 0;
            g_jcur->readIdx = 0;
        }
        g_jmode = JMode::Replay;
        RunUpdate(scene);
        g_jmode = JMode::Off;
        ClockReal();
    }
    bool same = (t_seed == g_truth.seed);
    const uint8_t* tp = g_truth.mem.data();
    for (uint32_t ci : g_state) {
        const Chunk& c = g_chunks[ci];
        if (same && ChunkAlive(c) && memcmp((void*)c.addr, tp, c.len) != 0) same = false;
        tp += c.len;
    }
    if (same && DynDiff(false) != 0) same = false;
    g_rbVerifies++;
    if (!same) { g_rbVerifyBad++; g_desyncs++; ReportDiff(F); }
    CopyState(g_truth, false);
    FilePositionsRestore();
    g_tVerify.Add(NowMs() - t);
    if (!same && g_rbDetailLogs < 6) { g_rbDetailLogs++; RbVerifyDetail(scene, F); }
}

static uint32_t RollbackFrame(void* scene) {
    const int D = g_cfg.depth;
    uint32_t F = g_frame;
    uint32_t slot = F % D;
    uint32_t sceneVt = *reinterpret_cast<uint32_t*>(scene);
    if (StateBlockFreed()) {
        SyncTestStop("game freed a heap block that holds fight data");
        return RunUpdate(scene);
    }
    if (!g_cueVt) HookCues();
    if (HuntCollect()) DtEnsure();   // build 49; build 53: tracking grows with the state (test 52 crash)
    if (NetOn()) NetFeedRb();
    RbDeliver(F);
    // Too late: saving frame F overwrites frame F-D, so a rollback from here
    // can reach back to F-D+1 at most. Every frame up to F-D+1 needs the
    // other player's input before we go on; wait (skip this update) until then.
    if (g_rbStarted && F + 1 >= g_rbStart + (uint32_t)D && g_rbConf <= F + 1 - (uint32_t)D) {
        if (g_pend.valid && g_pend.frame == F) WriteRanges(g_pend.pre);   // undo this frame's input step
        g_pend.valid = false;
        g_frame--;
        g_rbStalls++;
        SetStatus("ROLLBACK TEST | waiting for input");
        return 0;
    }
    g_stage = 1;
    ReleaseFrees(g_journals[slot]);
    {
        JLock lock;
        g_jOutside = &g_journals[slot];
    }
    g_stage = 2;
    double t0 = NowMs();
    MakeOutDelta(g_ring[slot]);
    CopyState(g_ring[slot], true);
    g_tSave.Add(NowMs() - t0);
    RbAttachInput(g_ring[slot], F);
    Snapshot& sn = g_ring[slot];
    if (!sn.inOk && g_rbStarted) {
        SyncTestStop("rollback test: input record missing");
        return RunUpdate(scene);
    }
    uint32_t r;
    bool started = g_rbStarted;
    if (!g_rbStarted && sn.inOk) {
        g_rbStarted = true;
        g_rbStart = F;
        g_rbConf = F;
        if (NetOn()) NetRbStarting(F);
        Log("  rollback: starting at frame %u", F);
    }
    uint32_t w = g_rbWrong;
    g_rbWrong = RB_NONE;
    if (started && w != RB_NONE) {
        if (w + (uint32_t)D <= F) { g_rbLate++; w = F - (D - 1); }
        if (w < g_rbStart) w = g_rbStart;
        r = RbRollback(scene, w, F);
    } else {
        r = RbRecordUpdate(scene, sn, g_journals[slot]);
    }
    g_stage = 4;
    CopyState(g_truth, true);
    g_truthFrame = F;
    g_truthBytes = g_stateBytes;
    g_rbFrames++;
    if (StateBlockFreed()) { SyncTestStop("game freed a heap block that holds fight data"); return r; }
    if (g_reallocs) { SyncTestStop("game resized a heap block (not supported yet)"); return r; }
    void* sceneNow = *reinterpret_cast<void**>(addr::G_MAIN_SCENE);
    if (r != 0 || sceneNow != scene || *reinterpret_cast<uint32_t*>(scene) != sceneVt) {
        SyncTestStop("fight ended");
        return r;
    }
    int verifyEvery = NetOn() ? g_cfg.onlineVerifyEvery : g_cfg.verifyEvery;
    if (verifyEvery > 0 && F >= g_rbStart + (uint32_t)D && F % (uint32_t)verifyEvery == 0) RbVerify(scene, F);
    g_stage = 0;
    if (g_rbFrames % 600 == 0) RbLogStats("so far");
    if (NetOn())
        SetStatus("%u rollbacks (max %u) | checks %u, %u bad", g_rbRollbacks, g_rbMaxDepth, g_rbVerifies, g_rbVerifyBad);
    else
        SetStatus("ROLLBACK TEST lag %d | %u rollbacks (max %u) | checks %u, %u bad | waits %u",
                  g_cfg.fakeLag, g_rbRollbacks, g_rbMaxDepth, g_rbVerifies, g_rbVerifyBad, g_rbStalls);
    return r;
}

// ---------------------------------------------------------------------------
// Hotkeys (polled on the game thread, only when the game window has focus)
// ---------------------------------------------------------------------------
static bool KeyPressed(int vk) {
    static bool down[256];
    bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool pressed = now && !down[vk];
    down[vk] = now;
    return pressed;
}

// Build 26: F8 runs the whole test on its own (profile, writer check, then
// the sync test at depth 1, 2, 4 and 8 for AUTO_FRAMES tested frames each),
// so only one key is needed at the start of a fight.
static int g_auto = 0;                 // 0 off, 1 profiling, 2 sync test running
static const uint32_t AUTO_FRAMES = 2400;
static void AutoTestTick() {
    if (!g_auto) return;
    if (g_auto == 3 && !g_cfg.useProfile) {   // build 49: no profile: straight to rollback
        if (g_mode != Mode::Idle) return;
        g_cfg.depth = 8;
        g_rbWant = true;
        SyncTestStart();
        g_rbWant = false;
        g_auto = g_mode == Mode::SyncTest ? 4 : 0;
        return;
    }
    if (g_auto == 3) {                 // F7: profile done -> short sync test with the hunt (finds rarely written memory)
        if (g_mode != Mode::Idle) return;
        if (!g_haveProfile) { Log("ROLLBACK TEST not started: no profile"); g_auto = 0; return; }
        g_cfg.depth = 2;
        Log("ROLLBACK TEST: profile done, 600 frames of sync test first (finds memory the profile missed)");
        SyncTestStart();
        g_auto = g_mode == Mode::SyncTest ? 5 : 0;
        return;
    }
    if (g_auto == 5) {
        if (g_mode != Mode::SyncTest) { Log("ROLLBACK TEST ended early (sync test stopped)"); g_auto = 0; return; }
        if (g_testFrames < 600) return;
        SyncTestStop("depth change");      // keeps the heap blocks the game made meanwhile
        g_cfg.depth = 8;
        g_rbWant = true;
        SyncTestStart();
        g_rbWant = false;
        g_auto = g_mode == Mode::SyncTest ? 4 : 0;
        return;
    }
    if (g_auto == 4) {
        if (g_mode != Mode::SyncTest) { Log("ROLLBACK TEST ended"); g_auto = 0; SetStatus("rollback test ended (F7 to start again)"); }
        return;
    }
    if (g_auto == 1) {
        if (g_mode != Mode::Idle) return;
        if (!g_haveProfile && g_cfg.useProfile) { Log("AUTO TEST ended: no profile"); g_auto = 0; return; }
        g_cfg.depth = 1;
        Log("AUTO TEST: sync test at depth 1");
        SyncTestStart();
        g_auto = g_mode == Mode::SyncTest ? 2 : 0;
        if (!g_auto) Log("AUTO TEST ended: sync test did not start");
        return;
    }
    if (g_mode != Mode::SyncTest) { Log("AUTO TEST ended early at depth %d (sync test stopped)", g_cfg.depth); g_auto = 0; return; }
    if (g_testFrames < AUTO_FRAMES) return;
    if (g_cfg.depth < 8) {
        SyncTestStop("depth change");
        g_cfg.depth *= 2;
        Log("depth set to %d", g_cfg.depth);
        SyncTestStart();
    } else {
        SyncTestStop("auto test done");
        Log("AUTO TEST done: all four depths tested");
        SetStatus("auto test done");
        g_auto = 0;
    }
}

#include "net_session.inc"
#include "lobby.inc"
#include "overlay.inc"

static void PollHotkeys() {
    HWND hwnd = *reinterpret_cast<HWND*>(addr::G_HWND);
    bool focused = hwnd && GetForegroundWindow() == hwnd;
    bool f9 = KeyPressed(VK_F9), f10 = KeyPressed(VK_F10), f11 = KeyPressed(VK_F11), f8 = KeyPressed(VK_F8);
    bool f7 = KeyPressed(VK_F7), f6 = KeyPressed(VK_F6);
    bool f4 = KeyPressed(VK_F4), f5 = KeyPressed(VK_F5);
    UiGameThreadTick(focused);
    if (!focused) return;
    if (f4) {
        if (N.st != NS_OFF) NetEnd("F4 pressed");
        else if (g_mode != Mode::Idle || g_auto) SetStatus("stop the test first (F7 / F8), then F4 to connect");
        else NetStart();
        return;
    }
    if (f5 && N.st == NS_OFF) NetPickNext();
    if (N.st != NS_OFF) f7 = f8 = f9 = f10 = f11 = false;   // online: the tests run on their own
    if (f6) {
        g_cfg.dirtyTracking = !g_cfg.dirtyTracking;
        if (!g_cfg.dirtyTracking && dt::active) { DtLogStats(); DtDisarm(); }
        Log("F6: changed-memory tracking now %s", g_cfg.dirtyTracking ? "ON" : "OFF");
        SetStatus("changed-memory tracking %s", g_cfg.dirtyTracking ? "ON" : "OFF");
    }
    if (f7) {
        if (g_auto == 3 || g_auto == 4 || g_auto == 5 || g_rbOn) {
            Log("ROLLBACK TEST stopped (F7)");
            g_auto = 0;
            if (g_mode == Mode::SyncTest) SyncTestStop("F7");
            else if (g_mode == Mode::Profiling) ProfileStop();
            else if (g_mode == Mode::Writers) WritersStop();
        } else if (g_mode == Mode::Idle && !g_auto) {
            Log("ROLLBACK TEST (F7): rollback with fake network lag");
            if (g_cfg.useProfile) { g_haveProfile = false; ProfileStart(); }
            g_auto = 3;
        }
        return;
    }
    if (f8) {
        if (g_auto) {
            Log("AUTO TEST cancelled (F8)");
            g_auto = 0;
            if (g_mode == Mode::SyncTest) SyncTestStop("F8");
        } else if (g_mode == Mode::Idle) {
            Log("AUTO TEST start (F8): depth 1, 2, 4, 8 for %u frames each", AUTO_FRAMES);
            if (g_cfg.useProfile) { g_haveProfile = false; ProfileStart(); }
            g_auto = 1;
        }
        return;
    }
    if (g_auto && (f9 || f10 || f11)) { Log("AUTO TEST cancelled (F9/F10/F11)"); g_auto = 0; }
    if (f9) {
        if (g_mode == Mode::Profiling) ProfileStop();
        else if (g_mode == Mode::Writers) WritersStop();
        else { if (g_mode == Mode::SyncTest) SyncTestStop("F9"); ProfileStart(); }
    }
    if (f10) {
        if (g_mode == Mode::SyncTest) SyncTestStop("F10");
        else if (g_mode == Mode::Idle) SyncTestStart();
    }
    if (f11 && (g_mode == Mode::Idle || g_mode == Mode::SyncTest)) {
        int d = g_cfg.depth;
        d = d >= 8 ? 1 : d * 2;
        bool wasRunning = g_mode == Mode::SyncTest;
        if (wasRunning) SyncTestStop("depth change");
        g_cfg.depth = d;
        Log("depth set to %d", d);
        SetStatus("depth %d", d);
        if (wasRunning) SyncTestStart();
    }
}

// ---------------------------------------------------------------------------
// The hooked update calls
// ---------------------------------------------------------------------------
static uint32_t g_lastSceneVt = 0;

static uint32_t __attribute__((thiscall)) HookUpdateMain(void* scene) {
    PickFixedStack((uint32_t)__builtin_frame_address(0));
    if (!g_gameTid) {
        g_gameTid = GetCurrentThreadId();
        Log("game thread id %lu", g_gameTid);
    }
    uint32_t vt = scene ? *reinterpret_cast<uint32_t*>(scene) : 0;
    if (vt != g_lastSceneVt) {
        Log("scene: %s (vtable 0x%08X)", addr::SceneName(vt), vt);
        g_lastSceneVt = vt;
        if (g_mode == Mode::SyncTest) SyncTestStop("scene changed");
        if (g_mode == Mode::Profiling) ProfileStop();
        if (g_mode == Mode::Writers) WritersStop();
    }
    {
        // Build 40: the real screen is the child of "game main" (see CurScreen).
        static uint32_t lastChildVt = 0, lastChild = 0;
        uint32_t cvt, cptr;
        CurScreen(&cvt, &cptr);
        if (cvt != lastChildVt || cptr != lastChild) {
            Log("screen: %s (vtable 0x%08X)", addr::SceneName(cvt), cvt);
            LatchLogicTime();
            lastChildVt = cvt;
            lastChild = cptr;
        }
    }
    PollHotkeys();
    AutoTestTick();
    if (g_netStall) {            // online: waiting for the other player's input
        g_tick++;
        NetScreenDiag(true);
        NetFrameEnd(nullptr);
        return 0;
    }
    g_frame++;
    g_tick++;
    NetUpdateStarts();
    if (scene && !(g_mode == Mode::SyncTest && g_rbOn)) NetFpCapture(g_frame);   // (rollback: captured inside)

    uint32_t r;
    if (scene && g_mode == Mode::Profiling) r = ProfileFrame(scene);
    else if (scene && g_mode == Mode::Writers) r = WritersFrame(scene);
    else if (scene && g_mode == Mode::SyncTest) r = g_rbOn ? RollbackFrame(scene) : SyncTestFrame(scene);
    else {
        g_childRan = false;
        r = RunUpdate(scene);
        NetScreenDiag(false);
    }
    NetFrameEnd(scene);
    return r;
}
// Build 51: "game main" calls the current screen's update here (it skips it on
// some frames, e.g. while loading); noted for the screen check log.
static uint32_t __attribute__((thiscall)) H_ChildUpdate(void* child) {
    g_childRan = child && !reinterpret_cast<uint8_t*>(child)[4];
    // Build 52: online, on a synced screen, the game's first call of the
    // screen's update after loading is held back and only marks this PC ready
    // (see NetBeforeInput). The screen's real first update is then frame 1,
    // the same frame on both PCs.
    if (g_childRan && NetOn() && N.syncedType && !N.ready) {
        N.ready = true;
        g_childRan = false;
        Log("net: screen %u (%s): loaded on this PC after %u frames, telling the other game", N.epoch, addr::SceneName(N.sceneVt), g_frame);
        return 0;
    }
    return OrigSceneUpdate(child);
}

static uint32_t __attribute__((thiscall)) HookUpdateOverlay(void* scene) {
    // An overlay scene (system / network screen) runs instead of the main
    // scene. Pause the sync test history so we never replay across it.
    if (g_mode == Mode::SyncTest) { g_ringCount = 0; g_skips++; }
    return OrigSceneUpdate(scene);
}

// ---------------------------------------------------------------------------
// Crash logging: if the game crashes, note where (helps me fix it).
// ---------------------------------------------------------------------------
static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    // Serious errors (0xC...), e.g. access violation, heap corruption.
    if ((code & 0xF0000000) == 0xC0000000) {
        static std::atomic<int> logged{0};
        if (logged++ < 5) {
            CONTEXT* c = e->ContextRecord;
            Log("EXCEPTION 0x%08lX at 0x%08X (thread %lu, mode %d, frame %u) addr=0x%08X eax=%08lX ecx=%08lX edx=%08lX ebx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX",
                code, (uint32_t)e->ExceptionRecord->ExceptionAddress, GetCurrentThreadId(), (int)g_mode, g_frame,
                e->ExceptionRecord->NumberParameters > 1 ? (uint32_t)e->ExceptionRecord->ExceptionInformation[1] : 0,
                c->Eax, c->Ecx, c->Edx, c->Ebx, c->Esi, c->Edi, c->Ebp, c->Esp);
            bool inDyn = false, inJournal = false;
            {
                uint32_t q = t_lastFree;
                for (auto& b : g_dyn) if (b.ptr == q) inDyn = true;
                for (auto& j : g_journals) {
                    for (auto& a : j.allocs) if (a.ptr == q) inJournal = true;
                    for (uint32_t fr : j.frees) if (fr == q) inJournal = true;
                }
            }
            Log("    sync step %d, last heap free on this thread 0x%08X from 0x%08X (in new-block list: %d, in journal: %d), replay allocations that went elsewhere: %u",
                g_stage, t_lastFree, t_lastFreeSite, inDyn, inJournal, g_allocMiss);
            LogSoundStats();
            // Return addresses on the stack, to see who called the crashing code.
            uint32_t* sp = reinterpret_cast<uint32_t*>(c->Esp);
            char buf[512] = "";
            int n = 0;
            for (int i = 0; i < 256 && n < 16; i++) {
                uint32_t v;
                if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, 4, nullptr)) break;
                if (v >= 0x401000 && v < 0x65C000) { sprintf(buf + strlen(buf), " %08X", v); n++; }
            }
            Log("    stack code addresses:%s", buf);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
static wchar_t g_origTitle[256] = L"";

// Freeze watchdog: if the game logic stops advancing for 3 seconds, write
// down where the game thread is stuck (and which step of ours it was in).
static void DescribeAddr(uint32_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)a, &m) && m) {
        GetModuleFileNameW(m, name, MAX_PATH);
        wchar_t* b = wcsrchr(name, L'\\');
        snprintf(out, n, "%08X(%ls+0x%X)", a, b ? b + 1 : name, a - (uint32_t)m);
    } else snprintf(out, n, "%08X", a);
}
static DWORD WINAPI WatchdogThread(LPVOID) {
    uint32_t last = 0, still = 0;
    bool reported = false;
    for (;;) {
        Sleep(500);
        uint32_t t = g_tick;
        if (t != last || g_mode == Mode::Idle || !g_gameTid) { last = t; still = 0; reported = false; continue; }
        if (++still < 6 || reported) continue;
        reported = true;
        Log("FREEZE: game logic has not advanced for 3 s (mode %d, sync test step %d, frame %u)", (int)g_mode, g_stage, g_frame);
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_gameTid);
        if (!th) { Log("  could not open game thread"); continue; }
        SuspendThread(th);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &c)) {
            char where[160];
            DescribeAddr(c.Eip, where, sizeof where);
            Log("  game thread at %s esp=%08lX", where, c.Esp);
            uint32_t* sp = reinterpret_cast<uint32_t*>(c.Esp);
            int n = 0;
            for (int i = 0; i < 1024 && n < 24; i++) {
                uint32_t v;
                if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, 4, nullptr)) break;
                HMODULE m = nullptr;
                if (v > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                      (LPCWSTR)v, &m) && m) {
                    DescribeAddr(v, where, sizeof where);
                    Log("    stack+%03X %s", i * 4, where);
                    n++;
                }
            }
        }
        ResumeThread(th);
        CloseHandle(th);
    }
}

static DWORD WINAPI TitleThread(LPVOID) {
    char last[512] = "";
    for (;;) {
        Sleep(250);
        HWND hwnd = *reinterpret_cast<HWND*>(addr::G_HWND);
        if (!hwnd || !IsWindow(hwnd)) continue;
        if (!g_origTitle[0]) GetWindowTextW(hwnd, g_origTitle, 256);
        char now[512];
        EnterCriticalSection(&g_statusLock);
        if (g_netStatus[0]) snprintf(now, sizeof now, "%s] [%s", g_netStatus, g_status);
        else snprintf(now, sizeof now, "%s", g_status);
        LeaveCriticalSection(&g_statusLock);
        if (strcmp(now, last) == 0) continue;
        strcpy(last, now);
        wchar_t title[768];
        _snwprintf(title, 768, L"%ls  [%hs]", g_origTitle, now);
        SendMessageTimeoutW(hwnd, WM_SETTEXT, 0, (LPARAM)title, SMTO_ABORTIFHUNG, 200, nullptr);
    }
}

// Build 48: SaveData.dat writer (cdecl, 3 args, returns an error code, 0 = ok).
// While this PC holds the other player's saved data the write is skipped.
typedef int(__cdecl* SaveWrite_t)(int, int, int);
static SaveWrite_t o_SaveWrite = nullptr;
static const uint32_t FN_SAVE_WRITE = 0x00517E40;
static const uint8_t EXPECT_SAVE_WRITE[] = {0x55, 0x8D, 0xAC, 0x24, 0x70, 0xFF, 0xFF, 0xFF};
static const uint32_t G_SAVE_ERR = 0x00C787C4;
extern "C" int __cdecl H_SaveWrite(int a, int b, int c) {
    if (g_saveBlock) {
        if (g_saveBlocked++ < 5) Log("net: the game wanted to write SaveData.dat; skipped (this PC uses player 1's saved data right now)");
        *(volatile uint32_t*)G_SAVE_ERR = 0;
        return 0;
    }
    return o_SaveWrite(a, b, c);
}
static bool HookSaveWrite() {
    if (!util::BytesMatch(FN_SAVE_WRITE, EXPECT_SAVE_WRITE, sizeof EXPECT_SAVE_WRITE)) { Log("SaveData writer: unexpected bytes, not hooked"); return false; }
    uint8_t* t = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return false;
    memcpy(t, EXPECT_SAVE_WRITE, 8);
    t[8] = 0xE9;
    int32_t rel = (int32_t)((FN_SAVE_WRITE + 8) - ((uint32_t)t + 13));
    memcpy(t + 9, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), t, 13);
    o_SaveWrite = (SaveWrite_t)t;
    return util::WriteRel32(FN_SAVE_WRITE, 0xE9, (void*)H_SaveWrite);
}

// Build 49: the same intro pick on both PCs (test 48: it is picked with rand
// during the loading screen, which runs a different number of frames on each
// PC). Right before the pick, both games set the same random numbers, made
// from the session seed and the screen count.
typedef uint32_t(__cdecl* IntroPick_t)(void*, void*);
static uint32_t g_introPicks = 0;
static uint32_t __cdecl H_IntroPick(void* p1, void* p2) {
    if (NetOn()) {
        uint32_t s = N.seed ^ 0x1A7E0000u ^ ((N.epoch + 1) * 0x9E3779B1u);
        t_seed = s;                    // rand in the update (this thread)
        t_seedInit = true;
        t_seedOut = s ^ 0x5EED1234u;   // ... and outside it
        t_seedOutInit = true;
        uint32_t* x = reinterpret_cast<uint32_t*>(addr::G_XORSHIFT);   // the game's own generator
        x[0] = s | 1;
        x[1] = s * 0x2545F491u + 1;
        x[2] = s ^ 0x6C078965u;
        x[3] = (s << 7) ^ 0x075BCD15u;
        if (g_introPicks++ < 8)
            Log("net: intro pick: same random numbers on both PCs (after screen %u, %s thread, in update: %s)", N.epoch,
                OnGameThread() ? "game" : "other", g_inUpdate.load() ? "yes" : "no");
    }
    return reinterpret_cast<IntroPick_t>(addr::FN_INTRO_PICK)(p1, p2);
}

static bool Install() {
    struct Check { uint32_t at; const uint8_t* bytes; size_t n; const char* what; };
    const Check checks[] = {
        {addr::FN_SCENE_UPDATE, addr::EXPECT_UPDATE, sizeof addr::EXPECT_UPDATE, "scene update"},
        {addr::CALL_UPDATE_MAIN, addr::EXPECT_CALL_MAIN, sizeof addr::EXPECT_CALL_MAIN, "main update call"},
        {addr::CALL_UPDATE_OVERLAY, addr::EXPECT_CALL_OVERLAY, sizeof addr::EXPECT_CALL_OVERLAY, "overlay update call"},
        {addr::FN_RAND, addr::EXPECT_RAND, sizeof addr::EXPECT_RAND, "rand"},
        {addr::FN_SRAND, addr::EXPECT_SRAND, sizeof addr::EXPECT_SRAND, "srand"},
        {addr::CALL_INPUT, addr::EXPECT_CALL_INPUT, sizeof addr::EXPECT_CALL_INPUT, "input call"},
        {addr::CALL_DEVREAD, addr::EXPECT_CALL_DEVREAD, sizeof addr::EXPECT_CALL_DEVREAD, "device read call"},
        {addr::CALL_KBREAD, addr::EXPECT_CALL_KBREAD, sizeof addr::EXPECT_CALL_KBREAD, "keyboard read call"},
        {addr::FN_PAD_OF_PLAYER, addr::EXPECT_PAD_OF_PLAYER, sizeof addr::EXPECT_PAD_OF_PLAYER, "pad of player"},
        {addr::FN_HIT_CHECK, addr::EXPECT_HIT_CHECK, sizeof addr::EXPECT_HIT_CHECK, "two-object check"},
        {addr::CALL_INTRO_PICK, addr::EXPECT_CALL_INTRO_PICK, sizeof addr::EXPECT_CALL_INTRO_PICK, "intro pick call"},
        {addr::CALL_CHILD_UPDATE, addr::EXPECT_CALL_CHILD_UPDATE, sizeof addr::EXPECT_CALL_CHILD_UPDATE, "screen update call"},
    };
    for (int tries = 0;; tries++) {   // wait up to 2 minutes for the DRM layer
        bool all = true;
        for (auto& c : checks) all = all && util::BytesMatch(c.at, c.bytes, c.n);
        if (all) break;
        if (tries > 12000) {
            for (auto& c : checks)
                if (!util::BytesMatch(c.at, c.bytes, c.n)) Log("Unexpected bytes at %s 0x%08X", c.what, c.at);
            Log("Game code does not match the version this mod was built for. Mod is NOT active.");
            SetStatus("wrong game version, mod inactive");
            return false;
        }
        Sleep(10);
    }
    Log("Game code verified. Installing hooks.");
    g_codeHash = CodeHash();
    g_wt_timeGetTime = (void*)H_timeGetTime;
    g_wt_QPC = (void*)H_QPC;
    g_wt_GetTickCount = (void*)H_GetTickCount;
    g_wt_HeapAlloc = (void*)H_HeapAlloc;
    g_wt_HeapFree = (void*)H_HeapFree;
    g_wt_HeapReAlloc = (void*)H_HeapReAlloc;
    o_timeGetTime = (timeGetTime_t)util::SwapIat(addr::IAT_timeGetTime, (void*)S_timeGetTime);
    o_QPC = (QPC_t)util::SwapIat(addr::IAT_QPC, (void*)S_QPC);
    o_GetTickCount = (GetTickCount_t)util::SwapIat(addr::IAT_GetTickCount, (void*)S_GetTickCount);
    o_HeapAlloc = (HeapAlloc_t)util::SwapIat(addr::IAT_HeapAlloc, (void*)S_HeapAlloc);
    o_HeapFree = (HeapFree_t)util::SwapIat(addr::IAT_HeapFree, (void*)S_HeapFree);
    o_HeapReAlloc = (HeapReAlloc_t)util::SwapIat(addr::IAT_HeapReAlloc, (void*)S_HeapReAlloc);
    o_ReadFile = (ReadFile_t)util::SwapIat(addr::IAT_ReadFile, (void*)H_ReadFile);
    o_GSTAFT = (GSTAFT_t)util::SwapIat(addr::IAT_GetSystemTimeAsFileTime, (void*)H_GSTAFT);
    g_wt_CreateFileW = (void*)H_CreateFileW;
    g_wt_CreateFileA = (void*)H_CreateFileA;
    g_wt_CloseHandle = (void*)H_CloseHandle;
    o_CreateFileW = (CreateFileW_t)util::SwapIat(addr::IAT_CreateFileW, (void*)S_CreateFileW);
    o_CreateFileA = (CreateFileA_t)util::SwapIat(addr::IAT_CreateFileA, (void*)S_CreateFileA);
    o_CloseHandle = (CloseHandle_t)util::SwapIat(addr::IAT_CloseHandle, (void*)S_CloseHandle);
    bool ok = (g_wt_Rand = (void*)ModRand, util::WriteRel32(addr::FN_RAND, 0xE9, (void*)S_Rand)) &&
              util::WriteRel32(addr::FN_SRAND, 0xE9, (void*)ModSrand) &&
              util::WriteRel32(addr::CALL_UPDATE_OVERLAY, 0xE8, (void*)HookUpdateOverlay) &&
              util::WriteRel32(addr::CALL_UPDATE_MAIN, 0xE8, (void*)HookUpdateMain) &&
              util::WriteRel32(addr::CALL_INPUT, 0xE8, (void*)H_Input) &&
              util::WriteRel32(addr::CALL_DEVREAD, 0xE8, (void*)H_DevRead) &&
              util::WriteRel32(addr::CALL_KBREAD, 0xE8, (void*)H_KbRead) &&
              util::WriteRel32(addr::FN_HIT_CHECK, 0xE9, (void*)S_HitCheckEntry) &&
              util::WriteRel32(addr::CALL_INTRO_PICK, 0xE8, (void*)H_IntroPick) &&
              util::WriteRel32(addr::CALL_CHILD_UPDATE, 0xE8, (void*)H_ChildUpdate) &&
              HookSaveWrite();
    Log(ok ? "Hooks installed. Hotkeys: F9 profile, F10 sync test, F11 depth, F8 automatic full test, F7 rollback test with fake lag, F4 connect / disconnect (online), F5 pick a Steam friend." : "Failed to install hooks!");
    SetStatus(ok ? "ready (F9 profile, F10 sync test, F11 depth)" : "hook install failed");
    return ok;
}

static DWORD WINAPI StartThread(LPVOID) {
    Log("Arcana Blood rollback mod, step 3 prototype, test build 59. Game pid %lu", GetCurrentProcessId());
    LoadSettings();
    Log("settings: profile_frames=%d depth=%d include_contested=%d heap=%d heap_restore=%d", g_cfg.profileFrames, g_cfg.depth,
        g_cfg.includeContested, g_cfg.heap, g_cfg.heapRestore);
    Log("  writer_frames=%d skip_thread_chunks=%d whole_blocks=%d hunt=%d", g_cfg.writerFrames, g_cfg.skipThreadChunks, g_cfg.wholeBlocks, g_cfg.hunt);
    LoadNetSettings();
    UiStart();                       // build 57: overlay (waits for the game's renderer on its own thread)
    static const char* modeNames[] = {"loopback test", "home network", "Steam"};
    NetStatus("offline - F4 to connect (%s)%s", modeNames[g_ncfg.mode], g_ncfg.mode == 2 ? ", F5 picks a friend" : "");
    p_NtQIT = (NtQIT_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        auto* dos = (IMAGE_DOS_HEADER*)nt;
        auto* nth = (IMAGE_NT_HEADERS*)((uint8_t*)nt + dos->e_lfanew);
        g_ntdllLo = (uint32_t)nt;
        g_ntdllHi = g_ntdllLo + nth->OptionalHeader.SizeOfImage;
    }
    p_FlsGetValue = (FlsGetValue_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FlsGetValue");
    AddVectoredExceptionHandler(1, WriterHandler);   // first in line: our own read-only pages
    AddVectoredExceptionHandler(0, CrashLogger);
    if (Install()) {
        CreateThread(nullptr, 0, TitleThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        util::SetModDir(self);
        InitializeCriticalSection(&g_statusLock);
        InitializeCriticalSection(&g_liveLock);
        InitializeCriticalSection(&g_jLock);
        InitializeCriticalSection(&g_own.lock);
        wchar_t log[MAX_PATH];
        _snwprintf(log, MAX_PATH, L"%lsrollback_log.txt", util::ModDir());
        util::LogOpen(log);
        CreateThread(nullptr, 0, StartThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
