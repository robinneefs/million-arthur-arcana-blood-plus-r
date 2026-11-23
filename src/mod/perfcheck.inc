// ===========================================================================
// Build 124 (issue #5): the Performance check (Settings > PERFORMANCE > RUN CHECK).
// Included into overlay.inc after roommatch.inc; runs on the game thread (PcTick from
// UiGameThreadTick), the results are drawn by settingsui.inc.
//
// It is the one-PC test session (the loopback stand-in, nothing goes online) with fake
// lag of a far-away opponent (5 frames + up to 2, 5% lost packets) and input delay 1, so
// rollbacks of about 4 to 7 frames happen all the time, like on a bad connection. Your
// own My setup is used, first to 3 rounds with no timer. Player 1's input is a fixed
// pseudo-random button pattern (the stand-in copies it, late), so nobody has to play.
// After a 1 s warm-up, 30 s are measured:
//   - frames on time: how long each frame took from the last (a frame more than 2 ms
//     over 1/60 s is late), and the frame rate;
//   - hitches: frames over 2 frames long, and the worst one;
//   - rollback headroom: what one re-run frame costs on this PC and how much of a frame
//     is left over with the frame pacer's "busy" time, so how many frames this PC can
//     re-run in one frame. That gives the input delay tip per ping;
//   - hints from what the mod knows about the PC (pacer off, Windows timer, v-sync with
//     a monitor that isn't 60 Hz, built-in graphics, overlays, window in the background).
// Then the fight is ended like after a lost connection (straight to the main menu, no
// pop-up) and the main menu opens on Settings > PERFORMANCE with the results.
// ===========================================================================
namespace pc {
enum St : int { IDLE = 0, CONNECTING, WARMUP, MEASURE, ENDING };
static std::atomic<int> st{IDLE};
static std::atomic<bool> req{false}, showResult{false};
static const uint32_t WARM_FRAMES = 60, MEASURE_FRAMES = 1800;
static uint32_t startMs = 0, phaseFrames = 0, endMs = 0;
static NetCfg saved;
static bool savedOk = false;

// scripted input
static uint32_t rng = 0x1234567u, dir = 0, dirLeft = 0, btn = 0, btnLeft = 0, btnGap = 30;
static uint32_t Rand() { rng = rng * 1103515245u + 12345u; return (rng >> 16) & 0x7FFF; }

// measuring
static double prevT = 0;
static uint32_t frames = 0, late = 0, hitches = 0, busyN = 0, notFront = 0;
static double worstMs = 0, sumMs = 0, busySum = 0, t0 = 0;
static uint32_t rb0 = 0, resim0 = 0;
static double rbMs0 = 0;

struct Result {
    bool have = false, ok = false;
    char fail[160] = "";
    double onTimePct = 0, score = 0, fps = 0, worstMs = 0, perResimMs = 0, baseBusyMs = 0;
    uint32_t frames = 0, late = 0, hitches = 0, rollbacks = 0, resim = 0, deepest = 0;
    int headroom = -1;                  // frames this PC can re-run inside one frame (-1: not known)
    bool pacerOff = false, timerBad = false, vsyncOdd = false, igpu = false, otherGpu = false, notFront = false;
    int hz = 0;
    double sleep1 = 0;
    char gpu[128] = "", overlays[160] = "";
    char when[32] = "";
};
static Result R;                        // written on the game thread while st != IDLE, read when st == IDLE
}  // namespace pc

static bool PcActive() { return pc::st.load() != pc::IDLE; }
static bool PcBusy() { return pc::st.load() != pc::IDLE || pc::req.load(); }
static void PcRequest() { if (!PcBusy()) pc::req = true; }
static const pc::Result& PcResult() { return pc::R; }
static bool PcTakeShowResult() { return pc::showResult.exchange(false); }

// From NetStart, after the settings were read from rollback.ini: the check's own session.
static void PcOverrideNet() {
    using namespace pc;
    saved = g_ncfg;
    savedOk = true;
    g_ncfg.mode = 0;                    // one-PC test session (the stand-in lives in this game)
    g_ncfg.wantSide = 0;
    g_ncfg.rollback = 1;
    g_ncfg.delay = 1;
    g_ncfg.loopDelay = 1;
    g_ncfg.loopLag = 5;
    g_ncfg.loopJitter = 2;
    g_ncfg.loopLoss = 5;
    g_ncfg.loopMirror = true;
    g_ncfg.loopTestDesync = 0;
    g_ncfg.testLag = g_ncfg.testJitter = g_ncfg.testLoss = 0;
    Log("performance check: test session with fake lag 5 frames (+0..2), 5%% lost packets, input delay 1");
}

// From NetFill (game thread), in fights: player 1's input for the frame being filled.
static uint32_t PcScriptInput() {
    using namespace pc;
    static const uint32_t kDirs[8] = {0x0, 0x2, 0x4, 0x8, 0x2, 0x4, 0x6, 0xA};
    if (!dirLeft) { dir = kDirs[Rand() % 8]; dirLeft = 6 + Rand() % 24; }
    dirLeft--;
    if (btnLeft) btnLeft--;
    else if (btnGap) { btnGap--; btn = 0; }
    else {
        static const uint32_t kBtns[4] = {0x1000, 0x2000, 0x4000, 0x8000};
        btn = kBtns[Rand() % 4];
        btnLeft = 2 + Rand() % 3;
        btnGap = 4 + Rand() % 24;
    }
    return dir | btn;
}

static void PcFail(const char* why) {
    using namespace pc;
    R = Result();
    R.have = true;
    R.ok = false;
    snprintf(R.fail, sizeof R.fail, "%s", why);
    Log("performance check: could not finish: %s", why);
}

static void PcRestore() {
    using namespace pc;
    if (savedOk) { g_ncfg = saved; savedOk = false; }
}

static void PcFinish() {
    using namespace pc;
    Result r;
    r.have = r.ok = true;
    SYSTEMTIME lt; GetLocalTime(&lt);
    snprintf(r.when, sizeof r.when, "%02d:%02d", lt.wHour, lt.wMinute);
    double secs = (NowMs() - t0) / 1000.0;
    r.frames = frames;
    r.late = late;
    r.hitches = hitches;
    r.worstMs = worstMs;
    r.fps = secs > 0 ? frames / secs : 0;
    r.onTimePct = frames ? 100.0 * (frames - late) / frames : 0;
    // build 124c: a PC that runs a little under 60 fps loses frames for good (online, the other
    // game then waits for it): 1 point off per 0.01 fps below 59.98
    {
        double slow = 59.98 - r.fps;
        r.score = r.onTimePct - (slow > 0 ? slow * 100.0 : 0.0);
        if (r.score < 0) r.score = 0;
    }
    r.rollbacks = g_rbRollbacks - rb0;
    r.resim = g_rbResim - resim0;
    r.deepest = g_rbMaxDepth;
    double rbMs = g_tRb.sum - rbMs0;
    r.perResimMs = r.resim ? rbMs / r.resim : g_verifyPerFrameMs;
    if (busyN && r.perResimMs > 0) {
        double avgBusy = busySum / busyN;
        r.baseBusyMs = avgBusy - (frames ? rbMs / frames : 0);   // a frame's work without the rollbacks
        if (r.baseBusyMs < 0) r.baseBusyMs = 0;
        double left = 1000.0 / 60.0 - 1.0 - r.baseBusyMs;        // 1 ms kept free for Windows
        int h = left > 0 ? (int)(left / r.perResimMs) : 0;
        r.headroom = h > 20 ? 20 : h;
    }
    r.pacerOff = !PacerActive();
    r.sleep1 = pace::sleep1Ms;
    r.timerBad = pace::sleep1Ms > 4;
    r.hz = g_refreshHz;
    r.vsyncOdd = pace::vsync && g_refreshHz && (g_refreshHz < 59 || g_refreshHz > 61);
    r.igpu = g_gpuIntegratedOnly;
    r.otherGpu = g_gpuOtherFaster;
    r.notFront = notFront > 30;
    snprintf(r.gpu, sizeof r.gpu, "%s", g_gpuName);
    snprintf(r.overlays, sizeof r.overlays, "%s", g_overlays);
    R = r;
    Log("PERFORMANCE CHECK: score %.1f%%; %u frames in %.1f s = %.2f fps; %.1f%% on time (%u late), %u hitches over 2 frames (worst %.1f ms); "
        "%u rollbacks, %u frames re-run (deepest %u), %.2f ms per re-run frame, frame work without rollbacks %.2f ms -> headroom %d frames",
        r.score, frames, secs, r.fps, r.onTimePct, late, hitches, worstMs, r.rollbacks, r.resim, r.deepest, r.perResimMs, r.baseBusyMs, r.headroom);
    Log("  pacer %s, Sleep(1) %.2f ms, monitor %d Hz, v-sync %s, graphics \"%s\"%s%s, overlays: %s, window in the background %u frames",
        r.pacerOff ? "OFF" : "on", r.sleep1, r.hz, pace::vsync ? "on" : "off", r.gpu, r.igpu ? " (built-in)" : "", r.otherGpu ? ", a faster card is there" : "",
        r.overlays[0] ? r.overlays : "none seen", notFront);
}

// Game thread, every frame (after RoomMatchTick).
static void PcTick(uint32_t vt, bool focused) {
    using namespace pc;
    uint32_t now = GetTickCount();
    if (req.exchange(false) && st == IDLE) {
        if (N.st != NS_OFF || g_mode != Mode::Idle || g_auto || g_roomCfg.on) {
            PcFail("A match or a test is still running. Finish it first.");
            showResult = true;
            return;
        }
        R = Result();
        rng = 0x1234567u; dir = dirLeft = btn = btnLeft = 0; btnGap = 30;
        frames = late = hitches = busyN = notFront = 0;
        worstMs = sumMs = busySum = 0; prevT = 0;
        st = CONNECTING;
        startMs = now;
        phaseFrames = 0;
        Log("performance check: starting");
        snprintf(g_uiName, sizeof g_uiName, "%s", net::SteamMyName()[0] ? net::SteamMyName() : U.name);
        NetStart();
        if (N.st == NS_OFF) { PcRestore(); PcFail("The test session could not start."); st = IDLE; showResult = true; }
        return;
    }
    if (st == IDLE) return;
    phaseFrames++;
    if (st == CONNECTING || st == WARMUP || st == MEASURE) {
        if (N.st == NS_OFF) {
            PcRestore();
            PcFail("The test session ended early.");
            st = IDLE; showResult = true;
            return;
        }
    }
    if (st == CONNECTING) {
        EnterCriticalSection(&g_lobbyLock);
        LbSide& me = LB.me;
        bool on = N.st == NS_ON;
        if (on && me.phase == LP_LOBBY && !me.ready) {
            LeaveCriticalSection(&g_lobbyLock);
            SuApply();                      // My setup -> this PC's picks
            EnterCriticalSection(&g_lobbyLock);
            me.ready = true;
            me.rounds = 3;
            me.timer = 3;                   // no time limit
            LbChanged();
        }
        if (on && me.phase == LP_SETUP && !me.locked) { me.locked = true; LbChanged(); }
        if (on && N.side == 0 && me.phase == LP_SETUP) LB.startNow = true;
        LeaveCriticalSection(&g_lobbyLock);
        if (vt == SCENE_BTL && NetRbMode()) { st = WARMUP; phaseFrames = 0; Log("performance check: fight running, warming up"); }
        else if (now - startMs > 40000) {
            NetEnd("performance check: the fight did not start");
            PcRestore();
            PcFail("The test fight did not start in time.");
            st = IDLE; showResult = true;
        }
        return;
    }
    if (st == WARMUP) {
        if (vt != SCENE_BTL) return;
        if (phaseFrames >= WARM_FRAMES) {
            st = MEASURE; phaseFrames = 0;
            t0 = prevT = NowMs();
            rb0 = g_rbRollbacks; resim0 = g_rbResim; rbMs0 = g_tRb.sum;
            Log("performance check: measuring for %u frames", MEASURE_FRAMES);
        }
        return;
    }
    if (st == MEASURE) {
        double t = NowMs(), dt = t - prevT;
        prevT = t;
        frames++;
        sumMs += dt;
        if (dt > 1000.0 / 60.0 + 2.0) late++;
        if (dt > 2000.0 / 60.0) hitches++;
        if (dt > worstMs) worstMs = dt;
        double b = PaceLastBusy();
        if (b >= 0) { busySum += b; busyN++; }
        if (!focused) notFront++;
        if (frames >= MEASURE_FRAMES || vt != SCENE_BTL) {
            if (vt != SCENE_BTL && frames < MEASURE_FRAMES / 2) {
                NetEnd("performance check: the fight ended early");
                PcRestore();
                PcFail("The test fight ended early.");
                st = IDLE; showResult = true;
                return;
            }
            PcFinish();
            NetEnd("performance check done");
            PcRestore();
            g_dcExit = true;                // the fight ends straight to the main menu (boot.inc)
            st = ENDING; endMs = now;
        }
        return;
    }
    if (st == ENDING) {
        if (vt == VT_TITLE || now - endMs > 15000) {
            if (vt != VT_TITLE) Log("performance check: the game did not get back to the title screen in time");
            st = IDLE;
            showResult = true;
            Log("performance check: done, results on Settings > PERFORMANCE");
        }
    }
}

// Seconds left, for the banner over the fight (drawing thread)
static int PcSecondsLeft() {
    int s = pc::st.load();
    if (s == pc::WARMUP || s == pc::CONNECTING) return (int)(pc::MEASURE_FRAMES / 60) + 1;
    if (s == pc::MEASURE) { int left = (int)((pc::MEASURE_FRAMES - pc::frames) / 60) + 1; return left < 0 ? 0 : left; }
    return 0;
}

// Settings > PERFORMANCE: the results of the last check (drawing thread; settingsui.inc)
static void StPerfPanel(ImDrawList* dl, float X0, float Y0, float X1, float Y1, float pulse) {
    const MmPal& P = MmP();
    const pc::Result& r = PcResult();
    float x = X0 + 60, y = Y0 + 50;
    if (PcBusy()) {
        MmSpinner(dl, x + 14, y, 14, (float)ImGui::GetTime(), P.gold);
        MmText(dl, g_fUi, 26, x + 46, y, MmC(P.gold), "Starting the check...");
        return;
    }
    if (!r.have) {
        MmText(dl, g_fUi, 24, x, y, MmC(P.dim), "No check run yet on this start of the game.");
        MmText(dl, g_fUi, 22, x, y + 40, MmC(P.faint), "Run one after changing display or Windows settings to see if things got better.");
        return;
    }
    if (!r.ok) {
        char t[220];
        snprintf(t, sizeof t, "The check did not finish: %s", r.fail);
        MmText(dl, g_fUi, 24, x, y, MmC(P.gold), t);
        MmText(dl, g_fUi, 22, x, y + 40, MmC(P.faint), "Try again from the main menu, outside a room and with no match running.");
        return;
    }
    // ---- left: the score and the numbers ----
    char t[200];
    snprintf(t, sizeof t, "%.1f%%", r.score);
    MmRgb sc = r.score >= 98 ? MmRgb{90, 205, 100} : r.score >= 93 ? MmRgb{240, 190, 60} : MmRgb{225, 55, 40};
    MmText(dl, g_fUiBold, 64, x, y + 30, MmC(sc), t, 0, 0, 0, 0.5f);
    MmText(dl, g_fMenu, 14, x, y + 85, MmC(P.faint), "SCORE: FRAMES ON TIME AT FULL SPEED");
    bool slowFps = r.fps < 59.98;
    const char* verdict = r.score >= 98 ? "Smooth." : slowFps && r.onTimePct >= 98 ? "Steady, but a little under 60 fps." : r.score >= 93 ? "Mostly smooth, with some hitches." : "Not steady: see the hints.";
    MmText(dl, g_fUi, 24, x + 260, y + 30, MmC(P.text), verdict);
    float ly = y + 140;
    snprintf(t, sizeof t, "Speed: %.2f frames per second", r.fps);
    MmText(dl, g_fUi, 23, x, ly, slowFps ? MmC(P.gold) : MmC(P.text), t); ly += 38;
    snprintf(t, sizeof t, "Frames on time: %.1f%%", r.onTimePct);
    MmText(dl, g_fUi, 23, x, ly, MmC(P.text), t); ly += 38;
    if (r.hitches) snprintf(t, sizeof t, "Hitches: %u (the worst %.0f ms)", r.hitches, r.worstMs);
    else snprintf(t, sizeof t, "Hitches: none (the slowest frame %.0f ms)", r.worstMs);
    MmText(dl, g_fUi, 23, x, ly, MmC(P.text), t); ly += 38;
    snprintf(t, sizeof t, "Rollbacks: %u in 30 s, up to %u frames", r.rollbacks, r.deepest);
    MmText(dl, g_fUi, 23, x, ly, MmC(P.dim), t); ly += 38;
    if (r.perResimMs > 0) {
        snprintf(t, sizeof t, "One re-run frame takes %.2f ms", r.perResimMs);
        MmText(dl, g_fUi, 23, x, ly, MmC(P.dim), t); ly += 38;
    }
    snprintf(t, sizeof t, "Checked at %s. The result is in the log too (Pack logs).", r.when);
    MmText(dl, g_fUi, 20, x, Y1 - 36, MmC(P.faint), t);

    // ---- right: rollback headroom + delay tip, then hints ----
    float rx = X0 + 700, rw = X1 - rx - 50, ry = y;
    MmText(dl, g_fMenu, 14, rx, ry, MmC(P.faint), "INPUT DELAY TIP");
    ry += 40;
    if (r.headroom < 0) {
        MmText(dl, g_fUi, 22, rx, ry, MmC(P.dim), "Not measured (the frame pacer is off).");
        ry += 40;
    } else {
        int h = r.headroom;
        if (h >= 8) snprintf(t, sizeof t, "Your PC re-runs 8 or more frames within one frame: any input delay works.");
        else snprintf(t, sizeof t, "Your PC can re-run about %d frame%s within one frame.", h, h == 1 ? "" : "s");
        {
            char wl[3][200];
            int nl = MmWrap(g_fUi, 23, t, rw, wl, 3);
            for (int i = 0; i < nl; i++) { MmText(dl, g_fUi, 23, rx, ry, MmC(P.text), wl[i]); ry += 32; }
            ry += 6;
        }
        if (h < 8) {
            // rollback frames online ~ one-way trip in frames + 1, minus your input delay
            static const int pings[4] = {50, 100, 150, 200};
            char line[160]; int n = 0;
            n += snprintf(line + n, sizeof line - n, "Smooth with:");
            for (int i = 0; i < 4; i++) {
                int need = (int)ceil(pings[i] / 2.0 / (1000.0 / 60.0)) + 1 - h;
                if (need < 0) need = 0;
                n += snprintf(line + n, sizeof line - n, "%s ping %d ms: delay %d", i ? "," : "", pings[i], need);
            }
            char wl[3][200];
            int nl = MmWrap(g_fUi, 21, line, rw, wl, 3);
            for (int i = 0; i < nl; i++) { MmText(dl, g_fUi, 21, rx, ry, MmC(P.dim), wl[i]); ry += 30; }
        }
    }
    ry += 24;
    MmText(dl, g_fMenu, 14, rx, ry, MmC(P.faint), "WHAT COULD HELP");
    ry += 38;
    char hints[8][220]; int nh = 0;
    if (r.notFront) snprintf(hints[nh++], 220, "The game window was in the background during the check. Run it again with the game in front.");
    if (r.pacerOff) snprintf(hints[nh++], 220, "The frame pacer is off (pacer=0 in rollback.ini). Turn it back on (pacer=1) for a steady 60 fps.");
    if (slowFps && r.fps > 50) snprintf(hints[nh++], 220, "At %.2f fps this PC falls one frame behind about every %.0f s. Online the other game then has to wait for it.", r.fps, 1.0 / (60.0 - r.fps));
    if (r.timerBad) snprintf(hints[nh++], 220, "Windows' timer is coarse on this PC (a 1 ms sleep takes %.1f ms). Plug a laptop in and use a high performance power plan.", r.sleep1);
    if (r.vsyncOdd) snprintf(hints[nh++], 220, "V-Sync is on and the monitor runs at %d Hz. Turn V-Sync off under DISPLAY, or set the monitor to 60 Hz.", r.hz);
    if (r.igpu && r.otherGpu) snprintf(hints[nh++], 220, "The game runs on the built-in graphics (%s). Turn on \"Use the fast graphics card\" and restart the game.", r.gpu);
    else if (r.igpu) snprintf(hints[nh++], 220, "The game runs on built-in graphics (%s). A lower resolution under DISPLAY helps.", r.gpu);
    if (r.overlays[0]) snprintf(hints[nh++], 220, "Overlays are loaded (%s). If you see stutter, try without them.", r.overlays);
    if (r.onTimePct < 95 && nh == 0) snprintf(hints[nh++], 220, "Try Borderless under DISPLAY and close other programs while you play.");
    if (!nh) { MmText(dl, g_fUi, 22, rx, ry, MmC(P.dim), "Nothing found: this PC looks fine for the game."); return; }
    for (int k = 0; k < nh && ry < Y1 - 60; k++) {
        char wl[3][200];
        int nl = MmWrap(g_fUi, 21, hints[k], rw - 24, wl, 3);
        dl->AddCircleFilled(CP(rx + 6, ry), CV(4), MmC(P.gold, 0.9f), 12);
        for (int i = 0; i < nl; i++) { MmText(dl, g_fUi, 21, rx + 24, ry, MmC(P.text), wl[i]); ry += 29; }
        ry += 10;
    }
    (void)pulse;
}
