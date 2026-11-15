// ===========================================================================
// Build 72: what a crashing address is (called from the crash logger).
// ===========================================================================
static const char* kJmNames[] = {"off", "record", "replay", "re-run"};
static bool CdReadable(uint32_t a, uint32_t n) {
    MEMORY_BASIC_INFORMATION mi{};
    if (!VirtualQuery((void*)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    DWORD p = mi.Protect & 0xFF;
    if (mi.Protect & PAGE_GUARD) return false;
    if (p == PAGE_NOACCESS || p == 0) return false;
    return (uint32_t)mi.BaseAddress + mi.RegionSize >= a + n;
}
// Where in our saved state does address a lie (-1: not in it).
static int CdStatePos(uint32_t a) {
    for (uint32_t pos = 0; pos < g_state.size(); pos++) {
        const Chunk& c = g_chunks[g_state[pos]];
        if (a >= c.addr && a < c.addr + c.len) return (int)pos;
    }
    return -1;
}
static void CdDescribeValue(const char* name, uint32_t v) {
    if (v < 0x10000 || v >= 0x80000000u) return;
    char buf[320];
    int n = snprintf(buf, sizeof buf, "    %s=0x%08X:", name, v);
    MEMORY_BASIC_INFORMATION mi{};
    if (VirtualQuery((void*)v, &mi, sizeof mi))
        n += snprintf(buf + n, sizeof buf - n, " memory %s (region 0x%08X +0x%X, protect 0x%lX)",
                      mi.State == MEM_COMMIT ? "committed" : mi.State == MEM_RESERVE ? "RESERVED only (freed big block?)" : "FREE",
                      (uint32_t)mi.AllocationBase, (uint32_t)v - (uint32_t)mi.AllocationBase, mi.Protect);
    // fight-start heap block list
    auto it = std::upper_bound(g_blocks.begin(), g_blocks.end(), v, [](uint32_t x, const Block& b) { return x < b.start; });
    if (it != g_blocks.begin()) {
        --it;
        if (v < it->start + it->size) {
            uint32_t b = uint32_t(it - g_blocks.begin());
            bool inSt = std::find(g_stateBlocks.begin(), g_stateBlocks.end(), b) != g_stateBlocks.end();
            n += snprintf(buf + n, sizeof buf - n, "; in fight-start heap block 0x%08X (%u bytes, +0x%X, %s, %s)", it->start, it->size,
                          v - it->start, inSt ? "in saved state" : "NOT in saved state",
                          b < g_blockDead.size() && g_blockDead[b] ? (g_blockDead[b] == 2 ? "freed by us" : "freed by the game") : "alive");
        }
    }
    for (auto& d : g_dyn)
        if (v >= d.ptr && v < d.ptr + d.size) { n += snprintf(buf + n, sizeof buf - n, "; in block 0x%08X (%u bytes) allocated during the test, alive", d.ptr, d.size); break; }
    int pos = CdStatePos(v);
    n += snprintf(buf + n, sizeof buf - n, pos >= 0 ? "; address is IN the saved state" : "; address not in the saved state");
    Log("%s", buf);
    // heap history: newest first, events whose block holds v (short ring, then the long ring of big / other-heap / virtual events)
    for (int ring = 0; ring < 2; ring++) {
        HEv* R = ring ? g_hevBig : g_hev;
        uint32_t N = ring ? kHEvBigN : kHEvN;
        if (!R) continue;
        uint32_t total = ring ? g_hevBigN.load() : g_hevN.load(), cnt = total < N ? total : N, shown = 0;
        for (uint32_t k = 0; k < cnt && shown < 8; k++) {
            const HEv& e = R[(total - 1 - k) & (N - 1)];
            uint32_t sz = e.size ? e.size : 1;
            if (!(v >= e.ptr && v < e.ptr + sz)) continue;
            char who[96];
            DescribeAddr(e.caller, who, sizeof who);
            Log("      %s: frame %u %s 0x%08X (%u bytes) by %s, %s, %s thread%s, sync step %u", ring ? "long history" : "heap history", e.frame,
                e.op < sizeof kHevNames / sizeof *kHevNames ? kHevNames[e.op] : "?", e.ptr, e.size, who,
                e.jm < 4 ? kJmNames[e.jm] : "?", (e.flags & 1) ? "game" : "other", (e.flags & 2) ? ", in update" : ", outside update", e.stage);
            shown++;
        }
        if (!shown) Log("      %s: no event for this address in the last %u events", ring ? "long history" : "heap history", cnt);
    }
    // build 76: block lifetime table (no time limit)
    if (g_hlife) {
        uint32_t shown = 0, best = 0;
        for (uint32_t k = 0; k < kHLifeN && shown < 6; k++) {
            const HLife& L = g_hlife[k];
            if (!L.ptr) continue;
            uint32_t sz = L.size ? L.size : 1;
            if (!(v >= L.ptr && v < L.ptr + sz)) { if (L.ptr <= v && L.ptr > best) best = L.ptr; continue; }
            char wa[96], wf[96];
            DescribeAddr(L.aCaller, wa, sizeof wa);
            DescribeAddr(L.fCaller, wf, sizeof wf);
            Log("      block life: 0x%08X (%u bytes) %s at frame %d by %s (%s); %s%s at frame %u by %s (%s)", L.ptr, L.size,
                L.aOp < sizeof kHevNames / sizeof *kHevNames ? kHevNames[L.aOp] : "?", (int)L.aFrame, L.aOp ? wa : "-", L.aJm < 4 ? kJmNames[L.aJm] : "?",
                L.fOp ? "then " : "NOT freed", L.fOp && L.fOp < sizeof kHevNames / sizeof *kHevNames ? kHevNames[L.fOp] : "", L.fFrame, L.fOp ? wf : "-",
                L.fJm < 4 ? kJmNames[L.fJm] : "?");
            shown++;
        }
        if (!shown) {
            Log("      block life: no block holding this address was allocated or freed during the test (nearest start below: 0x%08X)", best);
            for (uint32_t k = 0; k < kHLifeN; k++) {
                const HLife& L = g_hlife[k];
                if (L.ptr != best || !best) continue;
                char wa[96]; DescribeAddr(L.aCaller, wa, sizeof wa);
                Log("        nearest: 0x%08X (%u bytes) %s at frame %d by %s, %s at frame %u", L.ptr, L.size,
                    L.aOp < sizeof kHevNames / sizeof *kHevNames ? kHevNames[L.aOp] : "?", (int)L.aFrame, wa,
                    L.fOp && L.fOp < sizeof kHevNames / sizeof *kHevNames ? kHevNames[L.fOp] : "not freed", L.fFrame);
            }
        }
    }
}
// Build 76: the value at address a (in the saved state at position pos) in every saved frame and the true state.
static void CdHolderHistory(uint32_t a, int pos) {
    size_t off = 0;
    for (int i = 0; i < pos; i++) off += g_chunks[g_state[i]].len;
    off += a - g_chunks[g_state[pos]].addr;
    char buf[512]; int n = snprintf(buf, sizeof buf, "        saved copies of 0x%08X (now frame %u, re-run frame %u):", a, g_frame, g_resimFrame);
    for (size_t k = 0; k < g_ring.size() && n < 440; k++) {
        const Snapshot& S = g_ring[k];
        if (S.frameNo == 0xFFFFFFFFu || S.mem.size() < off + 4) continue;
        uint32_t val; memcpy(&val, S.mem.data() + off, 4);
        n += snprintf(buf + n, sizeof buf - n, " f%u=%08X%s", S.frameNo, val, pos < (int)S.ver.size() ? "" : "?");
    }
    if (g_truth.mem.size() >= off + 4) { uint32_t val; memcpy(&val, g_truth.mem.data() + off, 4); n += snprintf(buf + n, sizeof buf - n, " true(f%u)=%08X", g_truthFrame, val); }
    Log("%s", buf);
}
// Where is pointer value v stored? Scans .data and the game's heap blocks.
static void CdFindHolders(uint32_t v) {
    if (v < 0x10000 || v >= 0x80000000u) return;
    uint32_t found = 0;
    auto scan = [&](uint32_t lo, uint32_t hi, const char* what) {
        for (uint32_t pg = lo & ~(addr::PAGE - 1); pg < hi && found < 12; pg += addr::PAGE) {
            if (!CdReadable(pg, addr::PAGE)) continue;
            const uint32_t* w = reinterpret_cast<const uint32_t*>(pg);
            for (uint32_t i = 0; i < addr::PAGE / 4 && found < 12; i++) {
                uint32_t a = pg + i * 4;
                if (a < lo || a >= hi || w[i] != v) continue;
                int pos = CdStatePos(a);
                Log("      pointer 0x%08X is stored at 0x%08X (%s) - %s", v, a, what, pos >= 0 ? "IN the saved state" : "NOT in the saved state");
                if (found < 3) {   // build 75: the object around it
                    uint32_t o = a >= 0x60 ? a - 0x60 : a;
                    if (CdReadable(o & ~(addr::PAGE - 1), addr::PAGE) && CdReadable((o + 0xC0) & ~(addr::PAGE - 1), addr::PAGE)) {
                        const uint32_t* q = reinterpret_cast<const uint32_t*>(o);
                        for (int r = 0; r < 12; r++)
                            Log("        %08X: %08X %08X %08X %08X", o + r * 16, q[r * 4], q[r * 4 + 1], q[r * 4 + 2], q[r * 4 + 3]);
                    }
                    auto it = std::upper_bound(g_blocks.begin(), g_blocks.end(), a, [](uint32_t x, const Block& b) { return x < b.start; });
                    if (it != g_blocks.begin()) { --it; if (a < it->start + it->size) Log("        (in heap block 0x%08X, %u bytes, +0x%X)", it->start, it->size, a - it->start); }
                    if (pos >= 0) CdHolderHistory(a, pos);
                }
                found++;
            }
        }
    };
    scan(addr::DATA_START, addr::DATA_START + addr::DATA_SIZE, ".data");
    for (auto& b : g_blocks) {
        if (found >= 12) break;
        if (b.size > (64u << 20)) continue;
        scan(b.start, b.start + b.size, "fight-start heap block");
    }
    for (auto& d : g_dyn) { if (found >= 12) break; scan(d.ptr, d.ptr + d.size, "block allocated during the test"); }
    if (!found) Log("      pointer 0x%08X: not found stored anywhere in .data or the game's heap blocks", v);
}
static void CrashDiag(EXCEPTION_POINTERS* e) {
    CONTEXT* c = e->ContextRecord;
    uint32_t fa = e->ExceptionRecord->NumberParameters > 1 ? (uint32_t)e->ExceptionRecord->ExceptionInformation[1] : 0;
    Log("  crash diagnosis (build 76): %u heap events recorded", g_hevN.load());
    BigReport("crash");   // build 82
    HwReport("crash");    // build 85
    CdDescribeValue("crash address", fa);
    const char* nm[] = {"eax", "ecx", "edx", "ebx", "esi", "edi", "ebp"};
    uint32_t rv[] = {(uint32_t)c->Eax, (uint32_t)c->Ecx, (uint32_t)c->Edx, (uint32_t)c->Ebx, (uint32_t)c->Esi, (uint32_t)c->Edi, (uint32_t)c->Ebp};
    for (int i = 0; i < 7; i++) {
        if (rv[i] == fa) continue;
        if (rv[i] >= (uint32_t)c->Esp - 0x10000 && rv[i] < (uint32_t)c->Esp + 0x10000) continue;   // stack
        CdDescribeValue(nm[i], rv[i]);
    }
    // The pointer the game followed: the register the address was computed from.
    for (int i = 0; i < 7; i++)
        if (rv[i] && fa >= rv[i] && fa - rv[i] < 0x1000) { Log("    who still holds %s (0x%08X):", nm[i], rv[i]); CdFindHolders(rv[i]); break; }
}
