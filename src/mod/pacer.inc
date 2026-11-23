// ===========================================================================
// Build 123 (issue #5, steady frame rate): our own frame pacer.
//
// RE (2026-11-22, static analysis): the game's frame loop (0x620FD0) notes QPC at the
// start of each frame (0x621093), runs input/update/draw/present, then, if the dword at
// 0x051CACEC is 1, works out how long the frame took, Sleep()s the rest of a frame
// (whole milliseconds, rounded down) and spins until the frame time is reached
// (0x621364-0x621485). Every frame is timed from its own start, so whatever a frame
// runs over is lost for good: testers' logs show averages of 16.70-17.00 ms (58.8-59.9
// fps), different per PC, and our clock sync then pauses the faster PC for whole frames.
//
// We replace that block (one call + jump at 0x621364, put in from the game thread itself)
// with a fixed schedule: frame N is due at start + N/60 s. A late frame is made up on the
// next ones; more than 2 frames late (a load freeze, a wait for input) starts the schedule
// again from now instead of rushing. Waiting: a high-resolution waitable timer for most of
// it (Windows 10 1803+), then a short spin. The clock sync and the one-time delay offset
// lengthen frames through PaceStretch / PaceAddDelay instead of Sleep(), so the schedule
// knows about them.
// With v-sync on, Present already waits for the monitor: then we only wait when the frame
// would otherwise end more than 2 ms early (monitors above 60 Hz), so a 60 Hz monitor sets
// the pace and frames never wait for two refreshes because of us.
// [perf] pacer=0 in rollback.ini leaves the game's own limiter in place (for comparing).
// ===========================================================================
void PaceEndOfFrame();
namespace pace {
static const uint32_t AT = 0x00621364;
static const uint8_t EXPECT[] = {0x8B, 0x15, 0xEC, 0xAC, 0x1C, 0x05,   // mov edx, [0x051CACEC]
                                 0x89, 0x55, 0xBC,                     // mov [ebp-0x44], edx
                                 0x83, 0x7D, 0xBC, 0x01,               // cmp dword [ebp-0x44], 1
                                 0x0F, 0x85, 0x0E, 0x01, 0x00, 0x00};  // jne 0x621485
static const uint32_t AFTER = 0x00621485;
static const uint32_t G_LIMITER_ON = 0x051CACEC;
static const uint32_t G_FRAME_A = 0x008D7E50;   // double: the game's frame time for its Sleep
static const uint32_t G_FRAME_B = 0x008D7D84;   // float: the game's frame time for its spin
static const uint32_t G_FRAME_MS = 0x008D7EF0;  // double: seconds -> ms (1000)

static bool wanted = true, tried = false, active = false;
static double qpcMs = 0;                   // ms per QPC tick
static int64_t qpcFreq = 0;
static double next = 0;                    // when the current frame is due to end (ms)
static double period = 1000.0 / 60.0;
static double stretchNext = 0;             // clock sync: extra ms for the frame now running
static double addNext = 0;                 // one-time delay: extra ms for the frame now running
static HANDLE timer = nullptr;
static bool hiResTimer = false;
static volatile bool vsync = false;        // from the device (overlay.inc), updated after a display reset
static uint32_t limiterFlagSeen = 0xFFFFFFFFu;
static double lastEnd = 0;                 // build 124: when the previous frame's wait ended
static double lastBusy = 0;                // build 124: how long the last frame worked (game + mod, without our wait)

// statistics: whole run, and the current window (logged with the fight timing every 600 frames)
struct Win {
    uint32_t frames = 0, onTime = 0, late2 = 0, late8 = 0, late16 = 0, lateFrame = 0, resets = 0;
    uint32_t stretched = 0, waitedFull = 0;
    double stretchMs = 0, addedMs = 0, t0 = 0, worstLate = 0;
};
static Win win, run;

static inline double Now() {
    LARGE_INTEGER t;
    if (o_QPC) o_QPC(&t); else QueryPerformanceCounter(&t);
    return (double)t.QuadPart * qpcMs;
}

// Wait until time t (ms, our clock): the timer for all but the last ~1.2 ms, then spin.
static void WaitUntil(double t) {
    for (;;) {
        double left = t - Now();
        if (left <= 0) return;
        if (left > 1.6 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((left - (hiResTimer ? 1.0 : 1.6)) * 10000.0);   // 100 ns units, relative
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(timer, 100); continue; }
        }
        if (left > 1.6) { Sleep(1); continue; }
        YieldProcessor();
    }
}

// Windows 11 may ignore a process's timer precision when it thinks its window is hidden,
// and power saving may slow it: opt out of both (harmless where it doesn't exist).
static void NoThrottle() {
    struct PPTS { ULONG Version, ControlMask, StateMask; };
    typedef BOOL(WINAPI * SPI_t)(HANDLE, int, LPVOID, DWORD);
    auto spi = (SPI_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
    if (!spi) { Log("pacer: power-throttling opt-out not available on this Windows"); return; }
    PPTS s{1, 0x1 | 0x4, 0};   // EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION, state 0 = never throttle
    BOOL ok = spi(GetCurrentProcess(), 4 /*ProcessPowerThrottling*/, &s, sizeof s);
    if (!ok) { s.ControlMask = 0x1; ok = spi(GetCurrentProcess(), 4, &s, sizeof s); }   // older Windows 10: no timer flag
    Log("pacer: asked Windows not to throttle the game's timer and speed (%s)", ok ? "ok" : "not supported");
}

// How well this PC's Sleep(1) keeps time (once at start, logged; the Performance check uses it too).
static double sleep1Ms = 0;
static void MeasureSleep() {
    double sum = 0, worst = 0;
    for (int i = 0; i < 10; i++) {
        double a = Now();
        Sleep(1);
        double d = Now() - a;
        sum += d;
        if (d > worst) worst = d;
    }
    sleep1Ms = sum / 10;
    Log("pacer: Sleep(1) takes %.2f ms on average, %.2f ms at worst%s", sleep1Ms, worst,
        sleep1Ms > 4 ? " (Windows is ignoring the 1 ms timer: the game's own limiter loses frames here)" : "");
}

static void WinReset(Win& w) { w = Win(); w.t0 = Now(); }

// Called from the game thread (first main update): put our pacer in place of the game's limiter.
static void Install() {
    if (tried) return;
    tried = true;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq = f.QuadPart;
    qpcMs = 1000.0 / (double)f.QuadPart;
    wchar_t ini[MAX_PATH];
    _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
    wanted = GetPrivateProfileIntW(L"perf", L"pacer", 1, ini) != 0;
    double a = *(const double*)G_FRAME_A, ms = *(const double*)G_FRAME_MS;
    float b = *(const float*)G_FRAME_B;
    uint32_t flag = *(const uint32_t*)G_LIMITER_ON;
    limiterFlagSeen = flag;
    Log("pacer: the game's own limiter: %s (switch %u), sleeps to %.3f ms, waits to %.3f ms (x%.0f)", flag == 1 ? "on" : "OFF", flag, a * 1000, b * 1000.0, ms);
    MeasureSleep();
    NoThrottle();
    if (!wanted) { Log("pacer: off ([perf] pacer=0): the game's own limiter paces frames"); return; }
    if (!util::BytesMatch(AT, EXPECT, sizeof EXPECT)) { Log("pacer: unexpected bytes at 0x%08X, game's own limiter kept", AT); return; }
    timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    hiResTimer = timer != nullptr;
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    uint8_t code[10];
    code[0] = 0xE8;
    int32_t rel = (int32_t)((uint32_t)(void*)&::PaceEndOfFrame - (AT + 5));
    memcpy(code + 1, &rel, 4);
    code[5] = 0xE9;
    rel = (int32_t)(AFTER - (AT + 10));
    memcpy(code + 6, &rel, 4);
    if (!util::WriteCode(AT, code, sizeof code)) { Log("pacer: could not patch the limiter, game's own limiter kept"); return; }
    active = true;
    next = 0;
    WinReset(win);
    WinReset(run);
    Log("pacer: ON: fixed 60 fps schedule, %s timer", hiResTimer ? "high-resolution" : "normal (older Windows)");
}

static void WinLog(const Win& w, const char* what) {
    if (!w.frames) return;
    double secs = (Now() - w.t0) / 1000.0;
    Log("pacer (%s): %u frames in %.1f s = %.2f fps; ready in time %.1f%%, late <2 ms %u, 2-8 ms %u, 8-16 ms %u, a frame or more %u (worst %.1f ms), "
        "schedule restarted %u; clock sync stretched %u frames by %.0f ms in all, extra waits %.0f ms, full-frame waits %u%s",
        what, w.frames, secs, secs > 0 ? w.frames / secs : 0, 100.0 * w.onTime / w.frames, w.late2, w.late8, w.late16, w.lateFrame, w.worstLate,
        w.resets, w.stretched, w.stretchMs, w.addedMs, w.waitedFull, vsync ? "; v-sync on" : "");
}
}  // namespace pace

static bool PacerActive() { return pace::active; }
static double PaceLastBusy() { return pace::active ? pace::lastBusy : -1; }   // build 124 (Performance check)
// clock sync: make the frame now running longer by ms (no visible pause)
static void PaceStretch(double ms) {
    if (!pace::active) return;
    pace::stretchNext += ms;
    pace::win.stretched++; pace::run.stretched++;
    pace::win.stretchMs += ms; pace::run.stretchMs += ms;
}
// a deliberate one-time wait (delay difference at the start of a fight, a big clock gap)
static void PaceAddDelay(double ms, bool fullFrameWait) {
    if (!pace::active) return;
    pace::addNext += ms;
    pace::win.addedMs += ms; pace::run.addedMs += ms;
    if (fullFrameWait) { pace::win.waitedFull++; pace::run.waitedFull++; }
}
static void PaceWindowLog(const char* what) {      // with the fight timing (perf.inc)
    if (!pace::active) return;
    pace::WinLog(pace::win, what);
    pace::WinReset(pace::win);
}

// The game's frame loop calls this where its own limiter was (game thread, after Present).
void PaceEndOfFrame() {
    using namespace pace;
    double now = Now();
    lastBusy = lastEnd > 0 ? now - lastEnd : 0;
    struct Ended { ~Ended() { pace::lastEnd = pace::Now(); } } ended;   // build 124: every way out of here
    uint32_t flag = *(const uint32_t*)G_LIMITER_ON;
    if (flag != limiterFlagSeen) { Log("pacer: the game switched its own limiter %s (%u)", flag == 1 ? "on" : "off", flag); limiterFlagSeen = flag; }
    if (next == 0) next = now;                  // first frame: the schedule starts here
    double due = next + stretchNext + addNext;
    stretchNext = addNext = 0;
    double late = now - due;                    // > 0: this frame was not ready in time
    auto count = [&](Win& w) {
        w.frames++;
        if (late <= 0) w.onTime++;
        else if (late < 2) w.late2++;
        else if (late < 8) w.late8++;
        else if (late < period) w.late16++;
        else w.lateFrame++;
        if (late > w.worstLate) w.worstLate = late;
    };
    count(win);
    count(run);
    if (late > 2 * period) {                    // a freeze or a long wait: start again from now, don't rush
        win.resets++; run.resets++;
        next = now + period;
        return;
    }
    if (vsync) {
        // Present already waited for the monitor. Wait only if we'd be well early (above 60 Hz).
        if (due - now > 2.0) WaitUntil(due);
        double end = Now();
        // follow the monitor when it's close to 60 Hz, else keep our schedule
        next = (end - due > -2.0 && end - due < 2.0) ? end + period : due + period;
        if (next < end - period) next = end;   // never more than a frame of catching up
        return;
    }
    if (late < 0) WaitUntil(due);
    next = due + period;                        // fixed schedule: a late frame is made up on the next ones
}
