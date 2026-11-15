// ===========================================================================
// Build 75: memory the game gives back outside its main heap (included into mod.cpp)
// Test 74a (boss vs boss) crashed again like test 69: during a rollback re-run
// the game followed a pointer, kept in the saved state, into memory that was
// no longer there (reserved only, decommitted), and the heap history had no
// free for it. So it was given back some other way: another heap, or
// VirtualFree. Frees on the game's main heap are already held back until no
// rewind can reach them; now these are too (while a fight's rollback runs),
// and all of them are written to the long history for the crash report.
// A held-back free is tagged with the frame that made it: a rollback to that
// frame or earlier drops it (the new run frees again if it still does), and it
// is done for real once no rewind can reach that frame.
// ===========================================================================
typedef LPVOID(WINAPI* VirtualAlloc_t)(LPVOID, SIZE_T, DWORD, DWORD);
typedef BOOL(WINAPI* VirtualFree_t)(LPVOID, SIZE_T, DWORD);
static VirtualAlloc_t o_VirtualAlloc = nullptr;
static VirtualFree_t o_VirtualFree = nullptr;
struct HeldFree { uint32_t frame; uint8_t kind; HANDLE heap; uint32_t ptr, size; DWORD type; };   // kind 0 VirtualFree, 1 HeapFree (other heap)
static std::vector<HeldFree> g_held;
static CRITICAL_SECTION g_heldLock;
static bool g_heldLockInit = false;
static uint32_t g_resimFrame = 0;            // frame being re-run by a rollback (0 = none)
static uint32_t g_heldCount = 0, g_heldDone = 0, g_heldDropped = 0, g_heldCancelled = 0, g_vfNow = 0, g_otherFreeNow = 0;
static int g_deferOther = 1;                 // rollback.ini [rollback] hold_back_other_frees
struct HeldLock { HeldLock() { EnterCriticalSection(&g_heldLock); } ~HeldLock() { LeaveCriticalSection(&g_heldLock); } };
static inline bool HoldFrees() { return g_deferOther && g_jActive.load(std::memory_order_relaxed) && OnGameThread(); }
static inline uint32_t HeldTag() { return g_resimFrame ? g_resimFrame : g_frame; }
static void HeldDo(const HeldFree& h) {
    if (h.kind == 0) o_VirtualFree((LPVOID)h.ptr, h.type == MEM_RELEASE ? 0 : h.size, h.type);
    else o_HeapFree(h.heap, 0, (LPVOID)h.ptr);
}
// Frees made at frames that no rewind can reach any more: do them now.
static void HeldRelease(uint32_t safeUpTo, bool all) {
    if (!g_heldLockInit) return;
    std::vector<HeldFree> now;
    {
        HeldLock l;
        size_t k = 0;
        for (auto& h : g_held) { if (all || h.frame <= safeUpTo) now.push_back(h); else g_held[k++] = h; }
        g_held.resize(k);
    }
    for (auto& h : now) { HevAdd(HEV_VFREE_LATE, h.ptr, h.size, 0); HeldDo(h); g_heldDone++; }
}
// A rollback to frame w: frees made at frame w or later belong to the abandoned run.
static void HeldDropFrom(uint32_t w) {
    if (!g_heldLockInit) return;
    HeldLock l;
    size_t k = 0;
    for (auto& h : g_held) {
        if (h.frame >= w) { HevAdd(HEV_VFREE_DROPPED, h.ptr, h.size, 0); g_heldDropped++; continue; }
        g_held[k++] = h;
    }
    g_held.resize(k);
}
static LPVOID WINAPI H_VirtualAlloc(LPVOID a, SIZE_T n, DWORD type, DWORD prot) {
    // Asking for an exact range we are still holding back: give it back first
    // (fresh pages must read as zero, like after a real decommit).
    if (a && g_heldLockInit) {
        HeldLock l;
        size_t k = 0;
        for (auto& h : g_held) {
            uint32_t lo = h.ptr, hi = h.ptr + (h.size ? h.size : 1);
            if (h.kind == 0 && (uint32_t)a < hi && lo < (uint32_t)a + n) { HeldDo(h); g_heldCancelled++; continue; }
            g_held[k++] = h;
        }
        g_held.resize(k);
    }
    LPVOID p = o_VirtualAlloc(a, n, type, prot);
    if (p && OnGameThread()) HevAdd(HEV_VALLOC, (uint32_t)p, (uint32_t)n, GAME_RET(4));
    return p;
}
static BOOL WINAPI H_VirtualFree(LPVOID a, SIZE_T n, DWORD type) {
    uint32_t size = (uint32_t)n;
    if (type == MEM_RELEASE) {
        MEMORY_BASIC_INFORMATION mi{};
        size = 0;
        if (VirtualQuery(a, &mi, sizeof mi) && mi.AllocationBase == a) {   // whole reservation: add up its regions
            uint32_t p = (uint32_t)a;
            while (VirtualQuery((LPVOID)p, &mi, sizeof mi) && mi.AllocationBase == a) { size += (uint32_t)mi.RegionSize; p += (uint32_t)mi.RegionSize; }
        }
    }
    if (HoldFrees() && (type == MEM_RELEASE || type == MEM_DECOMMIT)) {
        if (g_jmode == JMode::Replay) { HevAdd(HEV_VFREE_DEFER, (uint32_t)a, size, GAME_RET(3)); return TRUE; }   // the first run already did
        HeldLock l;
        g_held.push_back({HeldTag(), 0, nullptr, (uint32_t)a, size, type});
        g_heldCount++;
        HevAdd(HEV_VFREE_DEFER, (uint32_t)a, size, GAME_RET(3));
        return TRUE;
    }
    if (OnGameThread()) { HevAdd(HEV_VFREE, (uint32_t)a, size, GAME_RET(3)); g_vfNow++; }
    return o_VirtualFree(a, n, type);
}
// Called by the HeapFree hook for heaps other than the game's main heap.
static bool HeldOtherHeapFree(HANDLE h, LPVOID p) {
    SIZE_T s = HeapSize(h, 0, p);
    uint32_t size = s == (SIZE_T)-1 ? 0 : (uint32_t)s;
    if (HoldFrees()) {
        if (g_jmode == JMode::Replay) { HevAdd(HEV_VFREE_DEFER, (uint32_t)p, size, 0); return true; }
        HeldLock l;
        g_held.push_back({HeldTag(), 1, h, (uint32_t)p, size, 0});
        g_heldCount++;
        HevAdd(HEV_VFREE_DEFER, (uint32_t)p, size, 0);
        return true;
    }
    if (g_jActive.load(std::memory_order_relaxed)) { HevAdd(HEV_OTHER_FREE, (uint32_t)p, size, 0); g_otherFreeNow++; }
    return false;
}
// Finds the game's import slot for a kernel32 function by its current value.
static uint32_t FindIatSlot(const char* name) {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    void* f = k ? (void*)GetProcAddress(k, name) : nullptr;
    if (!f) return 0;
    for (uint32_t a = 0x0065C000; a < 0x0065C800; a += 4)
        if (*reinterpret_cast<void**>(a) == f) return a;
    return 0;
}
static void HookVirtualMemory() {
    {
        wchar_t ini[MAX_PATH];
        _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
        g_deferOther = GetPrivateProfileIntW(L"rollback", L"hold_back_other_frees", 1, ini);
    }
    InitializeCriticalSection(&g_heldLock);
    g_heldLockInit = true;
    uint32_t sa = FindIatSlot("VirtualAlloc"), sf = FindIatSlot("VirtualFree");
    g_wt_VirtualAlloc = (void*)H_VirtualAlloc;
    g_wt_VirtualFree = (void*)H_VirtualFree;
    if (sa) o_VirtualAlloc = (VirtualAlloc_t)util::SwapIat(sa, (void*)S_VirtualAlloc);
    if (sf) o_VirtualFree = (VirtualFree_t)util::SwapIat(sf, (void*)S_VirtualFree);
    if (!o_VirtualAlloc) o_VirtualAlloc = VirtualAlloc;
    if (!o_VirtualFree) o_VirtualFree = VirtualFree;
    Log("memory hooks (build 75): VirtualAlloc %s (0x%08X), VirtualFree %s (0x%08X); other-heap and VirtualFree frees held back during rollback: %s",
        sa ? "hooked" : "not imported", sa, sf ? "hooked" : "not imported", sf, g_deferOther ? "yes" : "no");
}
static void HeldLogStats() {
    Log("  frees outside the main heap: %u held back (%u done later, %u dropped by rollbacks, %u given back early for a new allocation), %u VirtualFree and %u other-heap frees done at once",
        g_heldCount, g_heldDone, g_heldDropped, g_heldCancelled, g_vfNow, g_otherFreeNow);
    g_heldCount = g_heldDone = g_heldDropped = g_heldCancelled = g_vfNow = g_otherFreeNow = 0;
}
