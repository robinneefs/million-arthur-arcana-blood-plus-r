// ===========================================================================
// Build 121: desync logging (thread "Fix real online desyncs", plan of 2026-11-22).
// The 18-19 Nov logs showed real desyncs but could not say where they came from:
// the fingerprint only covers the two characters and runs every 30 frames, the rand
// trace only covered frames 0-63, and ping was never written down. Here:
//  1) a random-number check of EVERY final frame (M_RCHK): seed at the start of the frame
//     and how many shared-seed calls it made. The first frame that differs is exact, and
//     both PCs write the rand trace (callers), the inputs and the rollback history around it.
//  2) a rollback history of the last 128 steps (what each step did: rollback, waits).
//  3) a memory snapshot of the whole fight state at an agreed frame after the first desync
//     (M_SNAP / M_SNAPH): both PCs swap a hash of every saved chunk (pointers left out),
//     log where they differ and keep the differing chunks in a desync_*.bin file
//     (tools/desync-compare.py compares the files of both PCs).
//  4) ping and lost pings every 10 s, and one summary line per fight.
// Nothing here changes the game: it only reads and logs.
// ===========================================================================
enum : uint8_t { M_RCHK = 15, M_SNAP = 16, M_SNAPH = 17 };
#pragma pack(push, 1)
struct NRchk { NHdr h; uint32_t first; uint8_t count, pad[3]; uint32_t seed[16]; uint16_t calls[16]; };
// kind 0: "let's take the snapshot at this frame"; kind 1: "taken, my lists have these layouts"
struct NSnap { NHdr h; uint32_t frame; uint8_t kind, pad[3]; uint32_t layout[2], count[2]; };
struct NSnapH { NHdr h; uint32_t frame; uint8_t list, pad; uint16_t count; uint32_t first; uint32_t hash[256]; };
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Ping (build 121): measured in microseconds (GetTickCount steps in ~16 ms), every 250 ms
// in a synced screen; min/avg/max and lost pings per 10 s window and per fight.
// ---------------------------------------------------------------------------
static uint32_t NowUs32() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (uint32_t)(t.QuadPart * 1000000 / f.QuadPart);
}
struct PingWin {
    uint32_t sent = 0, got = 0, minUs = 0xFFFFFFFFu, maxUs = 0;
    uint64_t sumUs = 0;
    void Add(uint32_t us) { got++; sumUs += us; if (us < minUs) minUs = us; if (us > maxUs) maxUs = us; }
    uint32_t AvgMs() const { return got ? (uint32_t)(sumUs / got / 1000) : 0; }
    uint32_t MinMs() const { return got ? minUs / 1000 : 0; }
    uint32_t MaxMs() const { return maxUs / 1000; }
    uint32_t LostPct() const { return sent ? (sent > got ? (sent - got) * 100 / sent : 0) : 0; }
};
static PingWin g_pingWin, g_pingFight;
static uint32_t g_pingLastUs = 0;
static void PingSent() { g_pingWin.sent++; g_pingFight.sent++; }
static void PingGot(uint32_t sentUs) {
    uint32_t us = NowUs32() - sentUs;
    if (us > 10000000u) return;            // a stale pong (or an old-style one)
    g_pingLastUs = us;
    g_pingWin.Add(us);
    g_pingFight.Add(us);
}

// ---------------------------------------------------------------------------
// Rollback history: one record per step (main update) in rollback mode.
// ---------------------------------------------------------------------------
struct StepRec { uint32_t F = RB_NONE, rbTo = RB_NONE, conf = 0, recvd = 0, ms = 0; uint16_t flags = 0, rttMs = 0; };
static StepRec g_steps[128];
static uint32_t g_stepN = 0;
static uint32_t g_winRollbacks = 0, g_winDeepest = 0, g_winStalls = 0;   // this 10 s window
static void StepRecord() {
    static uint32_t lastStalls = 0, lastTime = 0;
    StepRec& r = g_steps[g_stepN++ % 128];
    r.F = g_frame;
    r.rbTo = g_stepRbTo;
    r.conf = g_rbConf;
    r.flags = (uint16_t)((g_rbStalls != lastStalls ? 1 : 0) | (N.timeStalls != lastTime ? 2 : 0));
    r.recvd = N.recvd;
    r.ms = GetTickCount();
    r.rttMs = (uint16_t)(g_pingLastUs / 1000);
    if (g_stepRbTo != RB_NONE && g_frame >= g_stepRbTo) {
        g_winRollbacks++;
        if (g_frame - g_stepRbTo > g_winDeepest) g_winDeepest = g_frame - g_stepRbTo;
    }
    if (r.flags & 1) g_winStalls++;
    g_stepRbTo = RB_NONE;
    lastStalls = g_rbStalls;
    lastTime = N.timeStalls;
}
static void StepHistDump(uint32_t fromFrame) {
    uint32_t n = g_stepN < 128 ? g_stepN : 128, start = g_stepN - n;
    uint32_t i = start;
    while (i < g_stepN && (g_steps[i % 128].F == RB_NONE || g_steps[i % 128].F < fromFrame)) i++;
    if (i == g_stepN) { Log("  rollback history: no steps from frame %u on", fromFrame); return; }
    Log("  rollback history (build 121, steps from frame %u on): frame (ms since the first line): other player's input known up to | what the step did", fromFrame);
    uint32_t t0 = g_steps[i % 128].ms, lastRecvd = i > start ? g_steps[(i - 1) % 128].recvd : g_steps[i % 128].recvd;
    for (; i < g_stepN; i++) {
        const StepRec& r = g_steps[i % 128];
        char buf[200];
        int m = snprintf(buf, sizeof buf, "    %u (+%u ms): input known to %u", r.F, r.ms - t0, r.conf ? r.conf - 1 : 0);
        if (r.rbTo != RB_NONE) m += snprintf(buf + m, sizeof buf - m, " | rolled back to %u (%u frames re-run)", r.rbTo, r.F - r.rbTo);
        if (r.flags & 1) m += snprintf(buf + m, sizeof buf - m, " | waited for the other player's input");
        if (r.flags & 2) m += snprintf(buf + m, sizeof buf - m, " | clock sync wait");
        snprintf(buf + m, sizeof buf - m, " | packets in +%u, ping %u ms", r.recvd - lastRecvd, r.rttMs);
        lastRecvd = r.recvd;
        Log("%s", buf);
    }
}

// ---------------------------------------------------------------------------
// 1) per-frame random-number check
// ---------------------------------------------------------------------------
static struct Rchk {
    uint32_t rFrame[RT_N], rSeed[RT_N];
    uint16_t rCalls[RT_N];
    uint32_t rMax = 0, next = 1, sendTick = 0;
    uint32_t compared = 0, differed = 0, skipped = 0;
    uint32_t first = RB_NONE;            // first frame that differed on this screen
} RC;
static void RchkReset() {
    memset(RC.rFrame, 0xFF, sizeof RC.rFrame);
    RC.rMax = 0; RC.next = 1; RC.sendTick = 0;
    RC.compared = RC.differed = RC.skipped = 0;
    RC.first = RB_NONE;
}
// Frames up to this one have their final run here (all inputs real, any correction done).
static uint32_t RchkFinalUpTo() {
    if (!NetRbMode() || g_rbConf < 2) return RB_NONE;
    uint32_t last = g_rbConf - 1;
    if (last > g_frame) last = g_frame;
    if (g_rbWrong != RB_NONE && g_rbWrong <= last) {      // a correction still to come (the step waited)
        if (g_rbWrong < 2) return RB_NONE;
        last = g_rbWrong - 1;
    }
    return last;
}
static void RchkSend() {
    uint32_t last = RchkFinalUpTo();
    if (last == RB_NONE || (++RC.sendTick % 8)) return;
    NRchk m{};
    m.h = MakeHdr(M_RCHK);
    uint32_t first = last >= 16 ? last - 15 : 1, cnt = 0;
    for (uint32_t f = first; f <= last; f++) {
        if (!RtHas(f)) { if (!cnt) { first = f + 1; continue; } break; }
        m.seed[cnt] = RtSeed(f);
        m.calls[cnt] = (uint16_t)RtShared(f);
        cnt++;
    }
    if (!cnt) return;
    m.first = first;
    m.count = (uint8_t)cnt;
    NetSend(&m, sizeof m);
}
static void SnapPropose(const char* why);
static void RchkInputsLog(uint32_t from, uint32_t to) {
    Log("  inputs (build 121): frame: this player | other player (as this PC got it); the other PC's log has them the other way round");
    for (uint32_t k = from; k <= to; k++) {
        const NetIn& l = N.lIn[k % 256];
        const ConfIn& c = g_cf[k % 256];
        char a[24] = "?", b[24] = "?";
        if (l.frame == k) snprintf(a, sizeof a, "%08X %08X", l.v[0], l.v[1]);
        if (c.frame == k) snprintf(b, sizeof b, "%08X %08X", c.v[0], c.v[1]);
        Log("    %u: %s | %s", k, a, b);
    }
}
static void RchkFirstDiff(uint32_t f) {
    RC.first = f;
    uint32_t s = f % RT_N;
    NetDesync(f, "random numbers went differently in frame %u (build 121 check of every frame): this PC seed %08X, %u calls; other PC seed %08X, %u calls",
              f, RtSeed(f), RtShared(f), RC.rSeed[s], RC.rCalls[s]);
    if (RtSeed(f) == RC.rSeed[s])
        Log("net:   same seed at the start of frame %u: the game code in that frame called the random function a different number of times (%u frames compared before it, all the same)", f, RC.compared - 1);
    else
        Log("net:   the seed already differed at the start of frame %u (the frame before it was not compared: %u frames skipped so far)", f, RC.skipped);
    uint32_t last = RchkFinalUpTo();
    uint32_t from = f > 8 ? f - 8 : 1, to = (last != RB_NONE && f + 2 <= last) ? f + 2 : f;
    RandTraceDumpRange(from, to, "around the first difference; compare with the other PC's log");
    RchkInputsLog(from, f);
    StepHistDump(f > 16 ? f - 16 : 1);
    SnapPropose("random numbers");
}
static void RchkCompare() {
    uint32_t last = RchkFinalUpTo();
    if (last == RB_NONE) return;
    while (RC.next <= last && RC.next <= RC.rMax) {
        uint32_t f = RC.next, s = f % RT_N;
        if (RC.rFrame[s] != f) {
            if (RC.rMax > f + 16 || !RtHas(f)) { RC.skipped++; RC.next++; continue; }   // lost (it was sent twice)
            break;                                                                        // may still come
        }
        if (!RtHas(f)) { RC.skipped++; RC.next++; continue; }
        RC.compared++;
        if (RtSeed(f) != RC.rSeed[s] || (uint16_t)RtShared(f) != RC.rCalls[s]) {
            RC.differed++;
            if (RC.first == RB_NONE) RchkFirstDiff(f);
        }
        RC.next++;
    }
}
static void RchkHandle(const uint8_t* p, int n) {
    if (n < (int)sizeof(NRchk)) { N.badPackets++; return; }
    const NRchk* m = (const NRchk*)p;
    if (m->h.epoch != N.epoch || m->count > 16) return;
    for (uint32_t i = 0; i < m->count; i++) {
        uint32_t f = m->first + i, s = f % RT_N;
        RC.rFrame[s] = f;
        RC.rSeed[s] = m->seed[i];
        RC.rCalls[s] = m->calls[i];
        if (f > RC.rMax) RC.rMax = f;
    }
    RchkCompare();
}

// ---------------------------------------------------------------------------
// 3) memory snapshot at the first desync
// ---------------------------------------------------------------------------
static const uint32_t SNAP_FILE_CAP = 2u << 20;   // differing chunks kept per file
static const int SNAP_FILES_KEPT = 2;
static struct SnapState {
    uint32_t want = RB_NONE;          // agreed frame: the earliest either PC proposed
    bool taken = false, sentInfo = false, compareStarted = false, done = false, rInfo = false;
    char why[48] = "";
    uint32_t sessionSnaps = 0;
    // taken here
    std::vector<uint8_t> mem;
    struct C { uint32_t addr, len, off, bstart, bsize; uint8_t list; uint32_t idx; };
    std::vector<C> ch;
    std::vector<uint32_t> hash[2];    // per list (0 = .data, 1 = heap), pointers left out
    uint32_t layout[2] = {0, 0};
    uint8_t map[65536 / 8];
    uint32_t exeLo = 0, exeHi = 0, dynBlocks = 0;
    std::atomic<int> work{0};         // 0 idle, 1 hashing, 2 hashed, 3 comparing, 4 compared
    uint32_t sendList = 0, sendNext = 0, sendRound = 0, sentAt = 0;
    // the other PC
    uint32_t rLayout[2] = {0, 0}, rCount[2] = {0, 0}, rFrame = RB_NONE;
    std::vector<uint32_t> rHash[2];
    std::vector<uint8_t> rGot[2];
    uint32_t rGotN = 0;
} SN;
static void SnapReset() {
    if (SN.work == 1 || SN.work == 3) { SN.want = RB_NONE; SN.done = true; return; }   // the worker still uses the data; no new one this screen
    SN.want = RB_NONE;
    SN.taken = SN.sentInfo = SN.compareStarted = SN.done = SN.rInfo = false;
    SN.why[0] = 0;
    SN.mem.clear(); SN.mem.shrink_to_fit();
    SN.ch.clear();
    for (int l = 0; l < 2; l++) { SN.hash[l].clear(); SN.rHash[l].clear(); SN.rGot[l].clear(); SN.rCount[l] = SN.rLayout[l] = 0; }
    SN.rFrame = RB_NONE;
    SN.rGotN = 0;
    SN.work = 0;
    SN.sendList = SN.sendNext = SN.sendRound = SN.sentAt = 0;
}
static void SnapSendProposal() {
    NSnap m{};
    m.h = MakeHdr(M_SNAP);
    m.frame = SN.want;
    m.kind = 0;
    for (int i = 0; i < 2; i++) NetSend(&m, sizeof m);
}
static void SnapPropose(const char* why) {
    if (SN.done || SN.taken || SN.want != RB_NONE) return;
    if (SN.sessionSnaps >= 4) return;
    SN.want = g_frame + 60;
    snprintf(SN.why, sizeof SN.why, "%s", why);
    Log("net: memory snapshot (build 121): asking the other PC to save the whole fight state of frame %u with this PC (first desync: %s)", SN.want, why);
    SnapSendProposal();
}
static inline bool SnapPtr(uint32_t v) {
    if (v < 0x10000) return false;
    if (v >= SN.exeLo && v < SN.exeHi) return false;
    uint32_t g = v >> 16;
    return (SN.map[g >> 3] >> (g & 7)) & 1;
}
static uint32_t SnapChunkHash(const SnapState::C& c) {
    uint32_t h = 0x811C9DC5u ^ c.len;
    const uint8_t* d = SN.mem.data() + c.off;
    for (uint32_t o = 0; o + 4 <= c.len; o += 4) {
        uint32_t v; memcpy(&v, d + o, 4);
        if (SnapPtr(v)) v = 0x9A11E7u;
        h = (h ^ v) * 0x01000193u;
    }
    return h;
}
static DWORD WINAPI SnapHashThread(void*) {
    uint32_t lay[2] = {0x811C9DC5u, 0x811C9DC5u};
    for (auto& c : SN.ch) {
        SN.hash[c.list].push_back(SnapChunkHash(c));
        uint32_t key = c.list == 0 ? c.addr : c.bsize;
        lay[c.list] = (lay[c.list] ^ key) * 0x01000193u;
        lay[c.list] = (lay[c.list] ^ c.len) * 0x01000193u;
    }
    SN.layout[0] = lay[0];
    SN.layout[1] = lay[1];
    SN.work = 2;
    return 0;
}
static void SnapTake() {
    const int D = g_cfg.depth;
    const Snapshot& sn = g_ring[SN.want % D];
    SN.taken = true;
    SN.sessionSnaps++;
    size_t total = 0;
    for (uint32_t ci : g_state) total += g_chunks[ci].len;
    if (sn.frameNo != SN.want || sn.mem.size() != total) {
        Log("net: memory snapshot: the saved frame %u is not usable here (slot holds frame %u, %u of %u bytes); no snapshot", SN.want, sn.frameNo, (unsigned)sn.mem.size(), (unsigned)total);
        SN.done = true;
        return;
    }
    double t0 = NowMs();
    SN.mem = sn.mem;
    SN.ch.clear();
    SN.ch.reserve(g_state.size());
    uint32_t off = 0, idx[2] = {0, 0};
    for (uint32_t ci : g_state) {
        const Chunk& c = g_chunks[ci];
        SnapState::C x;
        x.addr = c.addr; x.len = c.len; x.off = off;
        x.list = c.block >= 0 ? 1 : 0;
        x.bstart = c.block >= 0 ? g_blocks[c.block].start : 0;
        x.bsize = c.block >= 0 ? g_blocks[c.block].size : 0;
        x.idx = idx[x.list]++;
        SN.ch.push_back(x);
        off += c.len;
    }
    memcpy(SN.map, g_ptrMapCur.load(), sizeof SN.map);
    SN.exeLo = g_exeLo; SN.exeHi = g_exeHi;
    SN.dynBlocks = (uint32_t)sn.dynList.size();
    Log("net: memory snapshot of frame %u taken at frame %u: %.1f MB in %u chunks (%u .data, %u heap), copy took %.1f ms; %u blocks made during the fight are not in it",
        SN.want, g_frame, SN.mem.size() / 1048576.0, (unsigned)SN.ch.size(), idx[0], idx[1], NowMs() - t0, SN.dynBlocks);
    SN.work = 1;
    HANDLE t = CreateThread(nullptr, 0, SnapHashThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t); else SnapHashThread(nullptr);
}
static std::vector<const SnapState::C*> SnapIndex(int l) {
    std::vector<const SnapState::C*> v(SN.hash[l].size(), nullptr);
    for (auto& x : SN.ch) if (x.list == l && x.idx < v.size()) v[x.idx] = &x;
    return v;
}
static void SnapWriteFile(const std::vector<uint32_t>* diff) {
    // differing chunks: [u8 list][u32 idx][u32 addr][u32 len][u32 block start][u32 block size][len bytes][len/32 dwords pointer mask]
    std::string out;
    auto put = [&](const void* p, size_t n) { out.append((const char*)p, n); };
    char hdr[64] = {};
    snprintf(hdr, sizeof hdr, "ARSNAP01 build %u", MOD_BUILD);
    put(hdr, 32);
    uint32_t meta[8] = {(uint32_t)N.side + 1, N.epoch, SN.want, SN.layout[0], SN.layout[1], SN.rLayout[0], SN.rLayout[1], SN.exeHi};
    put(meta, sizeof meta);
    uint32_t kept = 0, skipped = 0;
    for (int l = 0; l < 2; l++) {
        auto byIdx = SnapIndex(l);
        for (uint32_t i : diff[l]) {
            const SnapState::C* c = i < byIdx.size() ? byIdx[i] : nullptr;
            if (!c) continue;
            if (out.size() + c->len * 2 > SNAP_FILE_CAP) { skipped++; continue; }
            uint8_t li = (uint8_t)l;
            put(&li, 1); put(&c->idx, 4); put(&c->addr, 4); put(&c->len, 4); put(&c->bstart, 4); put(&c->bsize, 4);
            put(SN.mem.data() + c->off, c->len);
            std::vector<uint32_t> mask((c->len / 4 + 31) / 32, 0);
            for (uint32_t o = 0, w = 0; o + 4 <= c->len; o += 4, w++) {
                uint32_t v; memcpy(&v, SN.mem.data() + c->off + o, 4);
                if (SnapPtr(v)) mask[w / 32] |= 1u << (w % 32);
            }
            put(mask.data(), mask.size() * 4);
            kept++;
        }
    }
    // keep the newest SNAP_FILES_KEPT - 1 old files plus this one
    std::vector<std::wstring> old;
    wchar_t pat[MAX_PATH];
    _snwprintf(pat, MAX_PATH, L"%lsdesync_*.bin", util::ModDir());
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do old.push_back(fd.cFileName); while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(old.begin(), old.end());      // names start with the date and time
    for (size_t i = 0; i + (SNAP_FILES_KEPT - 1) < old.size(); i++) {
        wchar_t p[MAX_PATH];
        _snwprintf(p, MAX_PATH, L"%ls%ls", util::ModDir(), old[i].c_str());
        DeleteFileW(p);
    }
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%lsdesync_%04d%02d%02d_%02d%02d%02d_P%d_f%u.bin", util::ModDir(), st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, N.side + 1, SN.want);
    FILE* f = _wfopen(path, L"wb");
    if (!f) { Log("net: memory snapshot: could not write the desync file"); return; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    const wchar_t* base = wcsrchr(path, L'\\');
    Log("net: memory snapshot: %u differing chunks written to %ls (%u KB)%s; Pack logs includes it", kept, base ? base + 1 : path,
        (unsigned)(out.size() / 1024), skipped ? " (the file is full: some chunks left out)" : "");
}
static DWORD WINAPI SnapCompareThread(void*) {
    std::vector<uint32_t> diff[2];
    static const char* names[2] = {".data", "heap"};
    for (int l = 0; l < 2; l++) {
        uint32_t n = (uint32_t)SN.hash[l].size();
        if (SN.rLayout[l] != SN.layout[l] || SN.rCount[l] != n) {
            Log("net: memory snapshot: the %s chunks are laid out differently on the two PCs (%u here, %u there); not compared", names[l], n, SN.rCount[l]);
            continue;
        }
        uint32_t missing = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (i >= SN.rGot[l].size() || !SN.rGot[l][i]) { missing++; continue; }
            if (SN.rHash[l][i] != SN.hash[l][i]) diff[l].push_back(i);
        }
        Log("net: memory snapshot of frame %u, %s: %u of %u chunks differ (pointers left out)%s", SN.want, names[l], (unsigned)diff[l].size(), n,
            missing ? " - some hashes from the other PC never came" : "");
        // where: consecutive chunks of one block (or of .data) as one line
        auto byIdx = SnapIndex(l);
        uint32_t lines = 0;
        for (size_t a = 0; a < diff[l].size() && lines < 40;) {
            const SnapState::C* c0 = byIdx[diff[l][a]];
            size_t b = a + 1;
            uint32_t endAddr = c0 ? c0->addr + c0->len : 0;
            while (b < diff[l].size() && diff[l][b] == diff[l][b - 1] + 1) {
                const SnapState::C* cb = byIdx[diff[l][b]];
                if (!cb || !c0 || cb->bstart != c0->bstart) break;
                endAddr = cb->addr + cb->len;
                b++;
            }
            if (c0) {
                if (l == 0) {
                    bool chars = c0->addr < FP_BASE + FP_SIZE && FP_BASE < endAddr;
                    Log("net:   .data 0x%08X-0x%08X (%u chunks)%s", c0->addr, endAddr - 1, (unsigned)(b - a), chars ? " (the characters)" : "");
                } else
                    Log("net:   heap block 0x%08X (%u bytes): +0x%X-+0x%X (%u chunks)", c0->bstart, c0->bsize, c0->addr - c0->bstart, endAddr - c0->bstart - 1, (unsigned)(b - a));
            }
            lines++;
            a = b;
        }
    }
    if (!diff[0].empty() || !diff[1].empty()) SnapWriteFile(diff);
    SN.work = 4;
    return 0;
}
static void SnapSendHashes() {
    // 4 packets a step, everything three times (no resend protocol: a lost one is counted as missing)
    for (int k = 0; k < 4 && SN.sendRound < 3; k++) {
        const std::vector<uint32_t>& hl = SN.hash[SN.sendList];
        if (SN.sendNext >= hl.size()) {
            SN.sendNext = 0;
            if (++SN.sendList == 2) { SN.sendList = 0; SN.sendRound++; }
            continue;
        }
        NSnapH m{};
        m.h = MakeHdr(M_SNAPH);
        m.frame = SN.want;
        m.list = (uint8_t)SN.sendList;
        m.first = SN.sendNext;
        uint32_t cnt = (uint32_t)hl.size() - SN.sendNext;
        if (cnt > 256) cnt = 256;
        m.count = (uint16_t)cnt;
        memcpy(m.hash, hl.data() + SN.sendNext, cnt * 4);
        NetSend(&m, (int)(offsetof(NSnapH, hash) + cnt * 4));
        SN.sendNext += cnt;
    }
    if (SN.sendRound >= 3 && !SN.sentAt) SN.sentAt = GetTickCount() | 1;
}
static void SnapTick() {
    if (SN.done && SN.work != 2) return;
    if (SN.want != RB_NONE && !SN.taken) {
        const int D = g_cfg.depth;
        uint32_t last = RchkFinalUpTo();
        if (g_frame + 1 >= (uint32_t)D && SN.want < g_frame + 1 - (uint32_t)D) {
            Log("net: memory snapshot: frame %u is no longer saved here (now at frame %u); no snapshot", SN.want, g_frame);
            SN.done = true;
            return;
        }
        if (last != RB_NONE && SN.want <= last) SnapTake();
        if (!SN.taken) {   // repeat the proposal now and then (it may have been lost)
            static uint32_t lastSent = 0;
            if (g_frame - lastSent >= 15) { lastSent = g_frame; SnapSendProposal(); }
        }
        return;
    }
    if (SN.work == 2) {
        if (!SN.sentInfo) {
            NSnap m{};
            m.h = MakeHdr(M_SNAP);
            m.frame = SN.want;
            m.kind = 1;
            for (int l = 0; l < 2; l++) { m.layout[l] = SN.layout[l]; m.count[l] = (uint32_t)SN.hash[l].size(); }
            for (int i = 0; i < 3; i++) NetSend(&m, sizeof m);
            SN.sentInfo = true;
        }
        SnapSendHashes();
        uint32_t need = (uint32_t)(SN.rCount[0] + SN.rCount[1]);
        bool all = SN.rInfo && SN.rGotN >= need;
        bool late = SN.sentAt && (int32_t)(GetTickCount() - SN.sentAt) > 3000;   // signed: sentAt may be 1 ms ahead (| 1)
        if (SN.sendRound >= 3 && (all || late) && !SN.compareStarted) {
            if (!SN.rInfo) { Log("net: memory snapshot: no answer from the other PC; nothing to compare"); SN.done = true; SN.work = 4; return; }
            if (SN.rFrame != SN.want) { Log("net: memory snapshot: the other PC saved frame %u, this PC frame %u; not compared", SN.rFrame, SN.want); SN.done = true; SN.work = 4; return; }
            SN.compareStarted = true;
            SN.done = true;
            SN.work = 3;
            HANDLE t = CreateThread(nullptr, 0, SnapCompareThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t); else SnapCompareThread(nullptr);
        }
    }
}
static void SnapHandle(const uint8_t* p, int n) {
    const NHdr* h = (const NHdr*)p;
    if (h->epoch != N.epoch) return;
    if (h->type == M_SNAP) {
        if (n < (int)sizeof(NSnap)) { N.badPackets++; return; }
        const NSnap* m = (const NSnap*)p;
        if (m->kind == 0) {
            if (SN.taken || SN.done) return;
            if (SN.want == RB_NONE || m->frame < SN.want) {
                if (SN.want == RB_NONE) {
                    if (SN.sessionSnaps >= 4) return;
                    snprintf(SN.why, sizeof SN.why, "asked by the other PC");
                    Log("net: memory snapshot (build 121): the other PC found a desync and asks to save the fight state of frame %u", m->frame);
                } else Log("net: memory snapshot: the other PC asked for frame %u (earlier than %u): using it", m->frame, SN.want);
                SN.want = m->frame;
            }
        } else if (m->kind == 1 && !SN.rInfo) {
            SN.rInfo = true;
            SN.rFrame = m->frame;
            for (int l = 0; l < 2; l++) {
                SN.rLayout[l] = m->layout[l];
                SN.rCount[l] = m->count[l] < 200000 ? m->count[l] : 0;
                SN.rHash[l].resize(SN.rCount[l]);
                SN.rGot[l].resize(SN.rCount[l], 0);
            }
        }
        return;
    }
    if (n < (int)offsetof(NSnapH, hash)) { N.badPackets++; return; }
    const NSnapH* m = (const NSnapH*)p;
    if (m->list > 1 || m->count > 256 || n < (int)(offsetof(NSnapH, hash) + m->count * 4)) { N.badPackets++; return; }
    if (!SN.rInfo || m->frame != SN.rFrame) return;   // the info packet comes first (sent 3 times)
    auto& hl = SN.rHash[m->list];
    auto& got = SN.rGot[m->list];
    for (uint32_t i = 0; i < m->count; i++) {
        uint32_t k = m->first + i;
        if (k >= hl.size()) break;
        hl[k] = m->hash[i];
        if (!got[k]) { got[k] = 1; SN.rGotN++; }
    }
}

// ---------------------------------------------------------------------------
// hooks for the rest of net_session.inc
// ---------------------------------------------------------------------------
static void DlScreenStart() {
    RchkReset();
    SnapReset();
    g_stepN = 0;
    for (auto& s : g_steps) s = StepRec();
}
// A fight starts (also again in place after a rematch): its own first difference, snapshot and summary.
static void DlFightStart() {
    g_dlFight++;
    RC.first = RB_NONE;
    RC.compared = RC.differed = RC.skipped = 0;
    SnapReset();
    g_pingFight = PingWin();
}
static void DlFrameEnd() {
    if (!NetRbMode()) return;
    StepRecord();
    RchkSend();
    RchkCompare();
    SnapTick();
}
// The 10 s line (next to the clock sync line): ping and lost pings, rollbacks in the window.
static void DlWindowLog() {
    Log("net:   last 10 s: ping %u ms (min %u, max %u), %u of %u pings lost; %u rollbacks (deepest %u), waited for input %u times; random numbers this fight so far: %u frames checked, %u differed",
        g_pingWin.AvgMs(), g_pingWin.MinMs(), g_pingWin.MaxMs(), g_pingWin.sent > g_pingWin.got ? g_pingWin.sent - g_pingWin.got : 0, g_pingWin.sent,
        g_winRollbacks, g_winDeepest, g_winStalls, RC.compared, RC.differed);
    g_pingWin = PingWin();
    g_winRollbacks = g_winDeepest = g_winStalls = 0;
}
static void NetFightSummary() {
    if (!NetOn()) return;
    Log("net: fight summary (build 121): ping %u ms (min %u, max %u), %u%% of pings lost; rollbacks %u (deepest %u), %u frames re-run, waited for input %u times; random numbers: %u frames checked, %u differed%s%s, %u not checked",
        g_pingFight.AvgMs(), g_pingFight.MinMs(), g_pingFight.MaxMs(), g_pingFight.LostPct(), g_rbRollbacks, g_rbMaxDepth, g_rbResim, g_rbStalls,
        RC.compared, RC.differed, RC.first != RB_NONE ? " (first at frame " : "", RC.first != RB_NONE ? (std::to_string(RC.first) + ")").c_str() : "", RC.skipped);
    g_pingFight = PingWin();
}
