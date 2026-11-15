// ===========================================================================
// Build 72 (step 5, part 1): where the time goes in a fight.
// Logging only, nothing here changes how the game runs.
//  - every fight frame is timed from the start of one frame to the next,
//    split into our parts (memory added, tracking rebuilt, save, game update
//    or rollback, true-state copy, check, network wait) and the rest of the
//    game's frame (drawing, waiting for the screen);
//  - every 600 frames: how many frames were slow and the 5 slowest in detail;
//    the first 10 fight frames are always shown;
//  - at the end: what each part of the saved state costs (size, pages written
//    per frame), to see what could be left out.
// ===========================================================================
namespace perf {
enum Part { P_COLLECT, P_ENSURE, P_SAVE, P_STEP, P_TRUTH, P_VERIFY, P_WAIT, P_COUNT };
static const char* kPartNames[P_COUNT] = {"memory added", "tracking rebuilt", "save", "game update/rollback", "true-state copy", "check", "network wait"};
struct Fr { uint32_t frame = 0; float total = 0, inHook = 0; float part[P_COUNT] = {}; uint32_t hFaults = 0, dFaults = 0, added = 0; bool stalled = false; };
static bool on = false;
static Fr cur, worst[5];
static uint32_t nWorst = 0, frames = 0, winFirst = 0, over20 = 0, over50 = 0, over100 = 0, shownFirst = 0;
static double hookStart = 0, prevStart = 0, winWorst = 0, winSum = 0;
static uint32_t lastHF = 0;
static uint64_t lastDF = 0;
static double partWin[P_COUNT] = {};
}
static inline void PerfAdd(perf::Part p, double ms) { if (perf::on) perf::cur.part[p] += (float)ms; }
static void PerfLogFrame(const perf::Fr& f, const char* what) {
    char buf[512];
    int n = snprintf(buf, sizeof buf, "  %s frame %u%s: %.1f ms = ours %.1f (", what, f.frame, f.stalled ? " (waited for input)" : "", f.total, f.inHook);
    bool any = false;
    for (int p = 0; p < perf::P_COUNT; p++)
        if (f.part[p] >= 0.05f) { n += snprintf(buf + n, sizeof buf - n, "%s%s %.1f", any ? ", " : "", perf::kPartNames[p], f.part[p]); any = true; }
    if (!any) n += snprintf(buf + n, sizeof buf - n, "nothing measurable");
    snprintf(buf + n, sizeof buf - n, ") + rest of the frame %.1f; faults: watch %u, tracking %u%s", f.total - f.inHook, f.hFaults, f.dFaults,
             f.added ? ", memory joined the state" : "");
    Log("%s", buf);
}
static void PerfReport(const char* when) {
    if (!perf::frames || perf::frames == perf::winFirst) return;
    uint32_t n = perf::frames - perf::winFirst;
    char parts[320];
    int k = 0;
    for (int p = 0; p < perf::P_COUNT; p++)
        k += snprintf(parts + k, sizeof parts - k, "%s%s %.2f", p ? ", " : "", perf::kPartNames[p], perf::partWin[p] / n);
    Log("perf (%s): fight frames %u-%u: %u over 20 ms, %u over 50 ms, %u over 100 ms; slowest %.1f ms, average %.2f ms; ours per frame: %s",
        when, perf::winFirst + 1, perf::frames, perf::over20, perf::over50, perf::over100, perf::winWorst, perf::winSum / n, parts);
    for (uint32_t i = 0; i < perf::nWorst; i++) if (perf::worst[i].total > 20) PerfLogFrame(perf::worst[i], "slow");
    perf::nWorst = 0;
    perf::winFirst = perf::frames;
    perf::over20 = perf::over50 = perf::over100 = 0;
    perf::winWorst = perf::winSum = 0;
    for (auto& v : perf::partWin) v = 0;
}
// Called at the start of every main update while a fight runs (sync test / rollback / online).
static void PerfFrameBegin() {
    double now = NowMs();
    bool running = g_mode == Mode::SyncTest;
    if (perf::on && perf::prevStart > 0) {
        perf::Fr& f = perf::cur;
        f.total = (float)(now - perf::prevStart);
        uint32_t hf = g_hFaults.load();
        f.hFaults = hf - perf::lastHF;
        perf::lastHF = hf;
        f.dFaults = (uint32_t)(dt::faults - perf::lastDF);
        perf::lastDF = dt::faults;
        perf::frames++;
        perf::winSum += f.total;
        if (f.total > perf::winWorst) perf::winWorst = f.total;
        if (f.total > 20) perf::over20++;
        if (f.total > 50) perf::over50++;
        if (f.total > 100) perf::over100++;
        for (int p = 0; p < perf::P_COUNT; p++) perf::partWin[p] += f.part[p];
        if (perf::shownFirst < 10) { perf::shownFirst++; PerfLogFrame(f, "first"); }
        // keep the 5 slowest of this window
        uint32_t at = perf::nWorst < 5 ? perf::nWorst++ : 5;
        if (at == 5) { uint32_t m = 0; for (uint32_t i = 1; i < 5; i++) if (perf::worst[i].total < perf::worst[m].total) m = i; if (perf::worst[m].total < f.total) at = m; }
        if (at < 5) perf::worst[at] = f;
        if (perf::frames % 600 == 0) PerfReport("so far");
    }
    if (running && !perf::on) {           // a fight's rollback / sync test just started
        perf::on = true;
        perf::frames = perf::winFirst = perf::nWorst = perf::shownFirst = 0;
        perf::over20 = perf::over50 = perf::over100 = 0;
        perf::winWorst = perf::winSum = 0;
        for (auto& v : perf::partWin) v = 0;
        perf::lastHF = g_hFaults.load();
        perf::lastDF = dt::faults;
    } else if (!running && perf::on) {
        PerfReport("end");
        perf::on = false;
    }
    perf::prevStart = perf::on ? now : 0;
    perf::hookStart = now;
    perf::cur = perf::Fr();
    perf::cur.frame = g_frame + 1;
}
static void PerfFrameEnd() {
    if (!perf::on) return;
    perf::cur.inHook = (float)(NowMs() - perf::hookStart);
    perf::cur.stalled = g_netStall;
}
// Scoped timer for one part.
struct PerfScope {
    perf::Part p; double t;
    explicit PerfScope(perf::Part p_) : p(p_), t(perf::on ? NowMs() : 0) {}
    ~PerfScope() { if (perf::on) PerfAdd(p, NowMs() - t); }
};

// ---------------------------------------------------------------------------
// What each part of the saved state costs. dt counts, per watched page, at
// how many save points it had been written (or was kept writable as "hot").
// ---------------------------------------------------------------------------
namespace perf {
static std::unordered_map<uint32_t, uint32_t> pageWrites;   // page -> save points written (from earlier page lists)
static uint32_t ticksAtStart = 0;
}
static void PerfFlushPageWrites() {        // dt's counters into the map (before dt rebuilds its page list)
    for (size_t i = 0; i < dt::pages.size() && i < dt::writes.size(); i++)
        if (dt::writes[i]) perf::pageWrites[dt::pages[i].addr] += dt::writes[i];
}
static void PerfCostReport() {
    PerfFlushPageWrites();
    for (auto& w : dt::writes) w = 0;
    uint32_t ticks = dt::tick - perf::ticksAtStart;
    if (!ticks || g_state.empty()) { perf::pageWrites.clear(); return; }
    struct Part { uint32_t addr, bytes, pages; uint64_t writes; int32_t block; bool tracked; };
    struct Piece { int32_t block; uint32_t addr, len; bool tracked; };
    std::vector<Piece> pcs;
    pcs.reserve(g_state.size());
    for (uint32_t pos = 0; pos < g_state.size(); pos++) {
        const Chunk& c = g_chunks[g_state[pos]];
        pcs.push_back({c.block, c.addr, c.len, pos < dt::always.size() && !dt::always[pos]});
    }
    // build 73: sorted first (pieces added mid-fight sit at the end of the state)
    std::sort(pcs.begin(), pcs.end(), [](const Piece& a, const Piece& b) { return a.block != b.block ? a.block < b.block : a.addr < b.addr; });
    std::vector<Part> parts;
    for (const Piece& c : pcs) {
        bool join = !parts.empty() && parts.back().block == c.block &&
                    (c.block >= 0 || ((parts.back().addr + parts.back().bytes + addr::PAGE - 1) & ~(addr::PAGE - 1)) >= c.addr);
        if (!join) parts.push_back({c.addr, 0, 0, 0, c.block, false});
        Part& P = parts.back();
        if (c.tracked) P.tracked = true;
        if (c.addr + c.len - P.addr > P.bytes) P.bytes = c.addr + c.len - P.addr;
    }
    uint64_t totalBytes = 0, neverBytes = 0, alwaysBytes = 0;
    uint32_t never = 0;
    for (auto& P : parts) {
        uint32_t lo = P.addr & ~(addr::PAGE - 1), hi = (P.addr + P.bytes + addr::PAGE - 1) & ~(addr::PAGE - 1);
        P.pages = (hi - lo) / addr::PAGE;
        for (uint32_t pg = lo; pg < hi; pg += addr::PAGE) { auto it = perf::pageWrites.find(pg); if (it != perf::pageWrites.end()) P.writes += it->second; }
        totalBytes += P.bytes;
        if (!P.tracked) alwaysBytes += P.bytes;
        else if (!P.writes) { never++; neverBytes += P.bytes; }
    }
    std::sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) { return a.bytes > b.bytes; });
    Log("state cost (build 72): %u parts, %.1f MB, over %u save points; %u parts (%.1f MB) never written while watched; %.1f MB in small pieces copied every frame (not counted)",
        (unsigned)parts.size(), totalBytes / 1048576.0, ticks, never, neverBytes / 1048576.0, alwaysBytes / 1048576.0);
    for (size_t i = 0; i < parts.size() && i < 30; i++) {
        const auto& P = parts[i];
        if (P.block >= 0)
            Log("  heap block 0x%08X: %u KB, %u pages, written pages per frame %.2f%s", g_blocks[P.block].start, P.bytes / 1024, P.pages,
                (double)P.writes / ticks, !P.tracked ? " (copied every frame)" : P.writes ? "" : " (never written)");
        else
            Log("  .data 0x%08X-0x%08X: %u KB, %u pages, written pages per frame %.2f%s", P.addr, P.addr + P.bytes, P.bytes / 1024, P.pages,
                (double)P.writes / ticks, !P.tracked ? " (copied every frame)" : P.writes ? "" : " (never written)");
    }
    perf::pageWrites.clear();
}
