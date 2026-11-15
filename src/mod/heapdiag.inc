// ===========================================================================
// Build 72: heap history for crash diagnosis (included into mod.cpp)
// Test 69 crashed in a boss fight during a rollback: the game followed a
// pointer into freed memory. While the sync test / rollback runs, every heap
// allocation and free of the game's heap goes into a ring of the last 65536
// events, with what our journal did with it. On a crash the log then says,
// for the crashing address and every register: which block it lies in, who
// allocated and freed it and when, and where in memory that pointer is still
// stored (inside or outside the saved state).
// ===========================================================================
enum HevOp : uint8_t {
    HEV_ALLOC = 1,        // fresh allocation (game asked, heap gave)
    HEV_REUSE,            // replay / re-run got the block the first run got
    HEV_FREE_DELAY,       // freed by the update: real free delayed (journal)
    HEV_FREE_IGNORED,     // replay freed it again (already freed in the first run)
    HEV_FREE_DELAY_OUT,   // freed outside the update, block from during the test: delayed
    HEV_FREE_NOW,         // freed for real right away (block from before the test, or not watched)
    HEV_FREE_LATE,        // a delayed free done for real (no rewind can reach it any more)
    HEV_FREE_ABANDONED,   // released after a rollback (the new run did not ask for it)
    HEV_REALLOC,          // resized (old pointer)
    HEV_OTHER_ALLOC,      // build 75: allocation from another heap (not the game's main heap)
    HEV_OTHER_FREE,       // build 75: free on another heap
    HEV_VALLOC,           // build 75: VirtualAlloc by the game
    HEV_VFREE,            // build 75: VirtualFree by the game, done now
    HEV_VFREE_DEFER,      // build 75: VirtualFree / other-heap free held back until no rewind can reach it
    HEV_VFREE_LATE,       // build 75: a held-back free done for real
    HEV_VFREE_DROPPED,    // build 75: held-back free dropped (the run that freed it was rolled back)
};
static const char* kHevNames[] = {"?", "alloc", "alloc(reused)", "free(delayed)", "free(replay, ignored)",
                                  "free outside update(delayed)", "free NOW", "free(delayed, done)", "free(abandoned run)", "realloc",
                                  "alloc (other heap)", "free (other heap)", "VirtualAlloc", "VirtualFree NOW", "free held back", "held-back free done", "held-back free dropped"};
struct HEv { uint32_t ptr, size, caller, frame; uint8_t op, jm, flags, stage; };
static const uint32_t kHEvN = 1u << 16;
static HEv* g_hev = nullptr;
static std::atomic<uint32_t> g_hevN{0};
// Build 75: the boss crash (test 74a) pointed into memory with no event in the
// last 65536 (about 4 s of fight). Big blocks (64 KB+), other heaps and
// VirtualAlloc/VirtualFree also go into a second, long ring.
static const uint32_t kHEvBigN = 1u << 14;
static HEv* g_hevBig = nullptr;
static std::atomic<uint32_t> g_hevBigN{0};
// Build 76: test 75a's boss crash again pointed at a block with no event in
// either ring. Every block the game gets during the test also goes into a
// table keyed by address that keeps who allocated and who freed it, with no
// time limit (a newer block at the same address replaces the entry).
struct HLife { uint32_t ptr, size, aFrame, aCaller, fFrame, fCaller; uint8_t aOp, aJm, fOp, fJm; };
static const uint32_t kHLifeN = 1u << 18;
static HLife* g_hlife = nullptr;
static inline uint32_t HLifeSlot(uint32_t p) { return ((p >> 3) * 2654435761u) >> 14; }   // 18 bits
static inline void HLifeAdd(uint8_t op, uint32_t p, uint32_t size, uint32_t caller) {
    if (!g_hlife || !p) return;
    bool isAlloc = op == HEV_ALLOC || op == HEV_REUSE || op == HEV_OTHER_ALLOC || op == HEV_VALLOC;
    uint32_t h = HLifeSlot(p);
    if (isAlloc) {
        uint32_t pick = h;
        for (uint32_t i = 0; i < 8; i++) {
            HLife& L = g_hlife[(h + i) & (kHLifeN - 1)];
            if (L.ptr == p || L.ptr == 0) { pick = (h + i) & (kHLifeN - 1); break; }
            if (L.fOp && !g_hlife[pick].fOp) pick = (h + i) & (kHLifeN - 1);   // prefer replacing a freed entry
        }
        HLife& L = g_hlife[pick];
        L.ptr = p; L.size = size; L.aFrame = g_frame; L.aCaller = caller; L.aOp = op; L.aJm = (uint8_t)g_jmode;
        L.fFrame = 0; L.fCaller = 0; L.fOp = 0; L.fJm = 0;
        return;
    }
    for (uint32_t i = 0; i < 8; i++) {
        HLife& L = g_hlife[(h + i) & (kHLifeN - 1)];
        if (L.ptr != p) continue;
        if (op == HEV_FREE_IGNORED || op == HEV_VFREE_DROPPED) return;   // keep the real free
        if (!size && L.size) size = L.size;
        L.fFrame = g_frame; L.fCaller = caller; L.fOp = op; L.fJm = (uint8_t)g_jmode;
        return;
    }
    // freed but never seen allocated (from before the test): record the free alone
    HLife& L = g_hlife[h];
    if (L.ptr && !L.fOp) return;   // do not push out a live block
    L.ptr = p; L.size = size; L.aFrame = 0xFFFFFFFFu; L.aCaller = 0; L.aOp = 0; L.aJm = 0;
    L.fFrame = g_frame; L.fCaller = caller; L.fOp = op; L.fJm = (uint8_t)g_jmode;
}
static inline void HevAdd(uint8_t op, uint32_t p, uint32_t size, uint32_t caller) {
    if (!g_hev || !g_jActive.load(std::memory_order_relaxed)) return;
    uint32_t k = g_hevN.fetch_add(1, std::memory_order_relaxed) & (kHEvN - 1);
    HEv& e = g_hev[k];
    e.ptr = p; e.size = size; e.caller = caller; e.frame = g_frame;
    e.op = op; e.jm = (uint8_t)g_jmode; e.stage = (uint8_t)g_stage;
    e.flags = (OnGameThread() ? 1 : 0) | (g_inUpdate.load(std::memory_order_relaxed) ? 2 : 0);
    HLifeAdd(op, p, size, caller);
    if (g_hevBig && (size >= (64u << 10) || op >= HEV_OTHER_ALLOC)) {
        uint32_t b = g_hevBigN.fetch_add(1, std::memory_order_relaxed) & (kHEvBigN - 1);
        g_hevBig[b] = e;
    }
}
static inline uint32_t HevSize(HANDLE h, LPVOID p) {
    if (!g_hev || !g_jActive.load(std::memory_order_relaxed)) return 0;
    SIZE_T s = HeapSize(h, 0, p);
    return s == (SIZE_T)-1 ? 0 : (uint32_t)s;
}
static void HevInit() {
    if (!g_hev) g_hev = (HEv*)VirtualAlloc(nullptr, sizeof(HEv) * kHEvN, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_hlife) g_hlife = (HLife*)VirtualAlloc(nullptr, sizeof(HLife) * kHLifeN, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_hevBig) g_hevBig = (HEv*)VirtualAlloc(nullptr, sizeof(HEv) * kHEvBigN, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
