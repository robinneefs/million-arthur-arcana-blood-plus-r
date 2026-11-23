// ===========================================================================
// Build 123c: guard for the "move data" pointer crash (0x4BBB9B).
//
// Each fighting object keeps at +0x44 a pointer to the record of the move frame it is
// in (0x70-byte records, from the record array new'd at character load, pointer kept at
// chardata+0xF0F0C; the game's lookup 0x49DFD0 returns the static "no record" 0x4B79E90
// when there is none). The setter is 0x4C2F10 (writes it at 0x4C2F43).
// Crash of test 123b (and the parked boss crash): during a rollback re-run the pointer
// was 0x38FF0440, exactly 64 KB below the right one (0x39000440), in memory that was never
// usable (the reserved padding in front of a big heap block). No allocator is involved.
//
// What this does:
//  1. the setter is logged in a small ring (frame, sync step, re-run frame, object, value,
//     move, move frame), so a bad value can be traced to the setter or to something else;
//  2. right before the read that crashed (0x4BBB90), the pointer is checked: if it is not
//     readable memory, the whole story is logged once and the pointer is set to the game's
//     own "no record" so the game carries on instead of crashing.
// ===========================================================================
namespace pg {
static const uint32_t SET_AT = 0x004C2F40;     // mov ecx,[ebp-4] ; mov [ecx+0x44],eax
static const uint8_t SET_EXPECT[] = {0x8B, 0x4D, 0xFC, 0x89, 0x41, 0x44};
static const uint32_t CHK_AT = 0x004BBB90;     // mov ecx,[eax+0x44] ; mov edx,4
static const uint8_t CHK_EXPECT[] = {0x8B, 0x48, 0x44, 0xBA, 0x04, 0x00, 0x00, 0x00};
static const uint32_t NO_RECORD = 0x04B79E90;

struct SetEv { uint32_t frame, resim, obj, val, move, mframe; uint8_t stage, bad; };
static const uint32_t kN = 512;
static SetEv ring[kN];
static uint32_t ringN = 0;
static uint32_t badSets = 0, badReads = 0, fixed = 0, loggedReads = 0;

// small cache of readable regions, cleared whenever the frame / step / re-run changes
struct Rg { uint32_t lo, hi; };
static Rg cache[8];
static uint32_t cacheN = 0, cacheKey = 0xFFFFFFFFu;

static bool Readable(uint32_t p) {
    if (p == NO_RECORD) return true;
    if (p < 0x10000 || p >= 0x7FFF0000u) return false;
    uint32_t key = g_frame * 131u + (uint32_t)g_stage * 7u + g_resimFrame;
    if (key != cacheKey) { cacheKey = key; cacheN = 0; }
    for (uint32_t i = 0; i < cacheN; i++)
        if (p >= cache[i].lo && p + 0x70 <= cache[i].hi) return true;
    MEMORY_BASIC_INFORMATION mi{};
    if (!VirtualQuery((void*)p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;   // (our write tracking changes protections, so only "committed" counts)
    uint32_t lo = (uint32_t)mi.BaseAddress, hi = lo + (uint32_t)mi.RegionSize;
    if (p + 0x70 > hi) return Readable(hi);     // the record runs into the next run of pages: check that too
    cache[cacheN < 8 ? cacheN++ : (cacheN = 1, 0)] = {lo, hi};
    return true;
}

static void Describe(const char* what, uint32_t v) {
    MEMORY_BASIC_INFORMATION mi{};
    if (VirtualQuery((void*)v, &mi, sizeof mi))
        Log("      %s 0x%08X: %s, allocation 0x%08X +0x%X, this run of pages 0x%08X..0x%08X", what, v,
            mi.State == MEM_COMMIT ? "committed" : mi.State == MEM_RESERVE ? "reserved only" : "free",
            (uint32_t)mi.AllocationBase, v - (uint32_t)mi.AllocationBase, (uint32_t)mi.BaseAddress, (uint32_t)mi.BaseAddress + (uint32_t)mi.RegionSize);
}
}  // namespace pg

extern "C" void __cdecl PgOnSet(uint32_t obj, uint32_t val) {
    using namespace pg;
    SetEv& e = ring[ringN++ & (kN - 1)];
    e.frame = g_frame; e.resim = g_resimFrame; e.obj = obj; e.val = val;
    e.move = *(const uint32_t*)(obj + 0x9C); e.mframe = *(const uint32_t*)(obj + 0xB0);
    e.stage = (uint8_t)g_stage;
    e.bad = !Readable(val);
    if (e.bad && badSets++ < 5)
        Log("move-data guard: the game itself set object 0x%08X's move record to 0x%08X (not readable) at frame %u, sync step %d, re-run frame %u, move %u frame %u",
            obj, val, g_frame, g_stage, g_resimFrame, e.move, e.mframe);
}

extern "C" uint32_t __cdecl PgCheck(uint32_t obj) {
    using namespace pg;
    uint32_t p = *(const uint32_t*)(obj + 0x44);
    if (Readable(p)) return p;
    badReads++;
    if (loggedReads++ < 3) {
        Log("MOVE-DATA GUARD: object 0x%08X has move record 0x%08X, which is not readable (the crash of 0x4BBB9B); frame %u, sync step %d, re-run frame %u, mode %d",
            obj, p, g_frame, g_stage, g_resimFrame, (int)g_mode);
        Describe("bad value", p);
        Log("      object: move %u, move frame %u", *(const uint32_t*)(obj + 0x9C), *(const uint32_t*)(obj + 0xB0));
        // candidates: the same value +-64 KB, in case it is one bit/borrow off
        for (int d = -1; d <= 1; d += 2) {
            uint32_t q = p + (uint32_t)(d * 0x10000);
            Log("      value %s 64 KB = 0x%08X is %s", d > 0 ? "plus" : "minus", q, Readable(q) ? "readable" : "not readable");
        }
        // what the setter wrote for this object lately (newest first)
        uint32_t shown = 0;
        for (uint32_t k = 0; k < kN && k < ringN && shown < 12; k++) {
            const SetEv& e = ring[(ringN - 1 - k) & (kN - 1)];
            if (e.obj != obj) continue;
            Log("      setter, %u back: frame %u step %u re-run %u wrote 0x%08X (move %u frame %u)%s", k, e.frame, e.stage, e.resim, e.val, e.move, e.mframe,
                e.val == p ? "  <- THIS value" : "");
            shown++;
        }
        bool fromSetter = false;
        for (uint32_t k = 0; k < kN && k < ringN; k++) if (ring[k].val == p) fromSetter = true;
        Log("      the bad value %s in the last %u setter writes", fromSetter ? "IS" : "is NOT", ringN < kN ? ringN : kN);
        int pos = CdStatePos(obj + 0x44);
        if (pos >= 0) CdHolderHistory(obj + 0x44, pos);
        else Log("      the pointer's place 0x%08X is not in the saved state", obj + 0x44);
    }
    *(uint32_t*)(obj + 0x44) = NO_RECORD;     // the game's own "no record": carry on without a crash
    fixed++;
    return NO_RECORD;
}

extern "C" void S_PgSet(); extern "C" void S_PgChk();
__asm__(".text\n.p2align 4\n.globl _S_PgSet\n_S_PgSet:\n"
        "\tmovl -4(%ebp), %ecx\n"
        "\tmovl %eax, 0x44(%ecx)\n"
        "\tpushal\n\tpushl %eax\n\tpushl %ecx\n\tcall _PgOnSet\n\taddl $8, %esp\n\tpopal\n"
        "\tret\n"
        ".p2align 4\n.globl _S_PgChk\n_S_PgChk:\n"
        "\tpushal\n\tpushl %eax\n\tcall _PgCheck\n\taddl $4, %esp\n"
        "\tmovl %eax, 24(%esp)\n"          // -> ecx after popal
        "\tpopal\n"
        "\tmovl $4, %edx\n"
        "\tret\n");

static void PgInstall() {
    using namespace pg;
    wchar_t ini[MAX_PATH];
    _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
    if (!GetPrivateProfileIntW(L"debug", L"move_data_guard", 1, ini)) { Log("move-data guard: off ([debug] move_data_guard=0)"); return; }
    bool a = util::BytesMatch(SET_AT, SET_EXPECT, sizeof SET_EXPECT), b = util::BytesMatch(CHK_AT, CHK_EXPECT, sizeof CHK_EXPECT);
    if (!a || !b) { Log("move-data guard: unexpected game code (%d %d), not installed", a, b); return; }
    uint8_t c1[6] = {0xE8, 0, 0, 0, 0, 0x90};
    int32_t r = (int32_t)((uint32_t)(void*)S_PgSet - (SET_AT + 5)); memcpy(c1 + 1, &r, 4);
    uint8_t c2[8] = {0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    r = (int32_t)((uint32_t)(void*)S_PgChk - (CHK_AT + 5)); memcpy(c2 + 1, &r, 4);
    bool ok = util::WriteCode(SET_AT, c1, sizeof c1) && util::WriteCode(CHK_AT, c2, sizeof c2);
    Log("move-data guard (build 123c): %s", ok ? "on (checks the move record pointer before 0x4BBB9B, logs the setter)" : "could not patch");
}
