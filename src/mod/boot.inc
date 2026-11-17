// ===========================================================================
// Build 61: start the fight straight from our overlay (included into mod.cpp
// after lobby.inc).
// ===========================================================================
// When both PCs have the agreed match settings (lobby phase "match"), each
// game, while it shows the main menu, does what the game itself does for a
// Versus match, but skips character select:
//
//  1. Main menu: set the mode the way the title menu does for Versus
//     (0x04DC9650 = 2, 0x04DC9640 = 1, sides 0x04DC8A04 = 3) and tell
//     "game main" that both sides are now human (+0x0C = 3). "Game main"
//     then asks 0x5BB170 which screen comes next; for Versus that is
//     character select (3).
//  2. Our hook on that call (0x5BE29D) writes both players' setup blocks
//     (0x04DC9658 + p*0x200: +0x04 character, +0x08/0C/10 the 3 assists,
//     +0x14 colour, +0x30 stage, as character select would) and changes the
//     answer to the loading screen (0x24).
//  3. Loading screen: its assist set menu still runs. Both players' input is
//     replaced by an A press every 16 frames (the same on both PCs, it is a
//     synced screen), so the menu confirms by itself and the "VS" screen
//     moves on. When the menu has copied its preset into the setup blocks,
//     our 3 assists are written over it, before the fight reads them.
//  4. Rounds and timer: the game's options (0x04DCAD04 / 0x04DCAD08) are set
//     for the match and put back when the session ends.
// ===========================================================================

namespace boot {

enum State : uint8_t { B_IDLE = 0, B_WAIT_TITLE, B_SWITCHING, B_LOADING, B_FIGHT, B_AFTER };
static const char* const STATE_NAME[] = {"idle", "waiting for the main menu", "switching", "loading screen", "fight", "back to the main menu"};

static const uint32_t VT_TITLE = 0x008D2CBC, VT_LOADING = 0x008D2604;
static const uint32_t SETUP = 0x04DC9658, SETUP_STRIDE = 0x200;
static const uint32_t FIGHT_ASSISTS = 0x04B79560;  // fight setup copy, 3 per player (stride 0xC)
static const uint32_t OPT_ROUNDS = 0x04DCAD04, OPT_TIMER = 0x04DCAD08;
// Build 68: the game copies its options into a settings block (0x5D0F80, run
// when the options load or change): +0x08 timer (seconds, from the table at
// 0x7027E0), +0x14/+0x18/+0x1C rounds to win (option + 1). Test 67 set only
// the options, so the timer happened to work but the fight kept 2 rounds.
static const uint32_t GAME_SETTINGS = 0x051AFDC0, TIMER_TABLE = 0x007027E0;
static uint32_t setOld[4];
static const uint32_t CALL_NEXT_SCREEN = 0x005BE29D;  // game main -> 0x5BB170 (which screen after a side change)
static const uint8_t EXPECT_CALL_NEXT_SCREEN[] = {0xE8, 0xCE, 0xCE, 0xFF, 0xFF};
static const uint32_t FN_NEXT_SCREEN = 0x005BB170;
// Build 63: game main -> 0x5BB9B0, "the fight screen finished, which screen next"
static const uint32_t CALL_BTL_FINISHED = 0x005BE405;
static const uint8_t EXPECT_CALL_BTL_FINISHED[] = {0xE8, 0xA6, 0xD5, 0xFF, 0xFF};
static const uint32_t FN_BTL_FINISHED = 0x005BB9B0;
static const uint32_t SCREEN_TITLE = 4;

static State st = B_IDLE;
static MatchSettings s{};
static uint32_t bootedMatch = 0, stateFrames = 0, loadingFrames = 0;
static bool optSaved = false;
static uint32_t optRounds = 0, optTimer = 0;
static bool setSaved = false;
static int assistFixes = 0;
static bool hooked = false, hookedAfter = false;
// Build 107: the after-match screen runs inside the fight screen (fight phase 7, the
// game's own "Request rematch?" box, held unanswered while both choose), and a
// rematch restarts the fight in place (the game's own rematch, phase 8): no
// main menu, versus or loading screen in between.
static bool postFight = false;          // this fight reached phase 7 (after-match screen up)
static bool rmPending = false;          // rematch agreed: restart after frame rmAt
static uint32_t rmAt = 0;
static uint16_t rmYesEpoch = 0xFFFF;    // the restart's synced screen: frame 1 answers YES there
static bool rmYesArmed = false;
static bool restarting = false;         // build 107b: the game took the rematch, waiting for it to start again
static bool answerNo = false;           // Back to room: answer NO, the fight screen ends as before
static uint32_t winsAtStart[2] = {0, 0};
static const uint32_t G_WINS = 0x04B79364;
static const uint32_t G_RESULT = 0x04B79480;   // game's win count per player (+p*4), draws at 0x04B79360

static inline uint32_t& U32(uint32_t a) { return *reinterpret_cast<uint32_t*>(a); }
static void SetState(State n) {
    if (n != st) Log("boot: %s -> %s", STATE_NAME[st], STATE_NAME[n]);
    st = n;
    stateFrames = 0;
}

static void WriteAssists() {
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < 3; k++) {
            U32(SETUP + p * SETUP_STRIDE + 0x08 + k * 4) = s.assist[p][k];
            U32(FIGHT_ASSISTS + p * 0xC + k * 4) = s.assist[p][k];
        }
}
static bool AssistsOk() {
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < 3; k++)
            if (U32(SETUP + p * SETUP_STRIDE + 0x08 + k * 4) != s.assist[p][k] || U32(FIGHT_ASSISTS + p * 0xC + k * 4) != s.assist[p][k])
                return false;
    return true;
}

static void WriteSetupBlocks() {
    for (int p = 0; p < 2; p++) {
        uint32_t b = SETUP + p * SETUP_STRIDE;
        U32(b + 0x00) = 1;
        U32(b + 0x04) = s.chara[p];
        U32(b + 0x14) = s.color[p];
        U32(b + 0x18) = 0;              // human (the CPU match had 1 here for player 2)
        U32(b + 0x1C) = 0;
        U32(b + 0x30) = s.stage;
        U32(b + 0x78) = 0x00010000;
        U32(b + 0x7C) = 0x00010000;
    }
    WriteAssists();
    Log("boot: setup written: P1 character %u colour %u assists %u/%u/%u, P2 character %u colour %u assists %u/%u/%u, stage %u",
        s.chara[0], s.color[0], s.assist[0][0], s.assist[0][1], s.assist[0][2],
        s.chara[1], s.color[1], s.assist[1][0], s.assist[1][1], s.assist[1][2], s.stage);
}

// Replaces "game main"'s call of 0x5BB170 (thiscall, 3 stack args, callee pops).
typedef void(__attribute__((thiscall)) * NextScreenFn)(void*, void*, int*, int);
static void __attribute__((thiscall)) H_NextScreen(void* self, void* blk, int* next, int a3) {
    reinterpret_cast<NextScreenFn>(FN_NEXT_SCREEN)(self, blk, next, a3);
    if (st != B_SWITCHING) return;
    Log("boot: the game chose screen %d after the side change", next ? *next : -1);
    if (next && *next == 3) {
        WriteSetupBlocks();
        *next = 0x24;                    // the loading screen instead of character select
        assistFixes = 0;
        loadingFrames = 0;
        SetState(B_LOADING);
    } else {
        Log("boot: not the expected character select (3); leaving the game's choice");
        SetState(B_IDLE);
    }
}

// Build 63: replaces game main's call of 0x5BB9B0 (thiscall, 3 stack args: block,
// next screen id, a flag byte). After one of our matches, the next screen is the main
// menu instead of character select / the winner screen, and the lobby moves on to
// "match over" so our overlay shows Rematch / Back to lobby there.
typedef void(__attribute__((thiscall)) * BtlFinishedFn)(void*, void*, int*, void*);
static void __attribute__((thiscall)) H_BtlFinished(void* self, void* blk, int* next, void* flag) {
    reinterpret_cast<BtlFinishedFn>(FN_BTL_FINISHED)(self, blk, next, flag);
    if (g_dcExit && next) {               // build 108: the connection was lost and the player pressed OK
        g_dcExit = false;
        Log("boot: the fight screen finished after the connection was lost; the game chose screen %d, going to the main menu", *next);
        *next = (int)SCREEN_TITLE;
        g_postOn = false;
        postFight = false;
        if (st != B_IDLE) SetState(B_IDLE);
        return;
    }
    if (st != B_FIGHT || !next) return;
    Log("boot: the fight screen finished; the game chose screen %d (setup +0: P1 %u, P2 %u); going to the main menu instead",
        *next, U32(SETUP), U32(SETUP + SETUP_STRIDE));
    *next = (int)SCREEN_TITLE;
    g_postOn = false;                    // build 107
    postFight = false;
    EnterCriticalSection(&g_lobbyLock);
    LbSide& me = LB.me;
    if (NetOn() && me.phase == LP_MATCH && me.matchNo == bootedMatch) {
        me.phase = LP_POST;
        me.choice = LCH_NONE;
        me.choiceMatch = 0;
        LbChanged();
        LbNote("match %u is over", me.matchNo);
    }
    LeaveCriticalSection(&g_lobbyLock);
    SetState(B_AFTER);
}

// Build 64: the fight's own "Request rematch? YES / NO" box (fight phase 7, handled in
// 0x440D30 around 0x441F5A) asks two game functions, per player, whether the box was
// answered (0x596860) and what the answer was (0x596890; both 1 = rematch). During one
// of our matches those six calls say "answered: NO" straight away, so the fight ends
// as if a player said no and our after-fight hook takes over. Same answer on both PCs
// and in every rollback replay (it only depends on our match running).
static const uint32_t FN_DLG_DONE = 0x00596860, FN_DLG_ANSWER = 0x00596890;
static const uint32_t CALLS_DLG_DONE[] = {0x00441F8B, 0x00441F9D, 0x00442044};
static const uint32_t CALLS_DLG_ANSWER[] = {0x00441FAF, 0x00441FBE, 0x00442052};
static const uint32_t FIGHT_PHASE = 0x04B79340, FIGHT_STATE = 0x04B79490;
static bool skipBox = false;
static int boxSkips = 0;
static int __cdecl H_DlgDone(int p) {
    if (skipBox && st == B_FIGHT) {
        // build 107: "not answered" while both choose; answered on the restart's first frame
        // (rematch) or once Back to room was agreed. The same answer in every rollback replay
        // (rematch: a new synced screen; Back to room: rollback is switched off first).
        // build 107b (test 107: a faked YES left the game's boxes open, and the fight waited
        // for them): in the rematch's synced screen both players really press A on YES
        // (NetFill), and the game's own boxes answer
        if (rmYesArmed && rmYesEpoch == N.epoch) return reinterpret_cast<int(__cdecl*)(int)>(FN_DLG_DONE)(p);
        boxSkips++;
        return answerNo ? 1 : 0;
    }
    return reinterpret_cast<int(__cdecl*)(int)>(FN_DLG_DONE)(p);
}
static int __cdecl H_DlgAnswer(int p) {
    if (skipBox && st == B_FIGHT && !(rmYesArmed && rmYesEpoch == N.epoch)) return 0;
    return reinterpret_cast<int(__cdecl*)(int)>(FN_DLG_ANSWER)(p);
}
// Build 107b: the game's boxes (WINNER / LOSER, "Request rematch?") are not drawn while our
// after-match screen covers the fight. Replaces game main's call of the box drawing (0x596260).
static const uint32_t CALL_DLG_DRAW = 0x005BEF3F, FN_DLG_DRAW = 0x00596260;
static void __cdecl H_DlgDraw() {
    if (g_postOn.load() || (st == B_FIGHT && postFight)) return;
    // build 113 (test 112): boxes left over from the last fight (after Back to room) showed for
    // a moment over the next match's VS screen: none of the game's boxes while a match boots
    if (st == B_WAIT_TITLE || st == B_SWITCHING || st == B_LOADING || st == B_AFTER) {
        static int logs = 0;
        if (logs < 3 && st == B_LOADING) { logs++; Log("boot: the game's boxes are hidden on the loading screen"); }
        return;
    }
    reinterpret_cast<void(__cdecl*)()>(FN_DLG_DRAW)();
}
// Build 107c: the loading screen's Knight set menu (the game's preset picker, object
// 0x051EAA40) is not used by our matches (each player's 3 Knights are set in our setup).
// Loading state 1 calls its update (0x5E2D20, thiscall, 3 args) every frame and moves on
// once it says "done" (byte 0x051EAA72); the loading screen's draw calls its draw
// (0x5E3150). In our booted matches the update is not run and says "done" at once (no
// menu, no cursor or confirm sounds, no preset written over our Knights) and nothing is drawn.
static const uint32_t CALL_KS_UPDATE = 0x005C05BB, FN_KS_UPDATE = 0x005E2D20;
static const uint32_t CALL_KS_DRAW = 0x005C081F, FN_KS_DRAW = 0x005E3150;
static const uint32_t KS_DONE = 0x051EAA72;
static int ksSkips = 0;
static void __attribute__((thiscall)) H_KsUpdate(void* self, int a, int b, int c) {
    if (st == B_LOADING) {
        *reinterpret_cast<uint8_t*>(KS_DONE) = 1;
        if (ksSkips++ == 0) Log("boot: loading screen: the game's Knight set menu is skipped (our Knights are kept)");
        return;
    }
    reinterpret_cast<void(__attribute__((thiscall))*)(void*, int, int, int)>(FN_KS_UPDATE)(self, a, b, c);
}
static void __attribute__((thiscall)) H_KsDraw(void* self) {
    if (st == B_LOADING) return;
    reinterpret_cast<void(__attribute__((thiscall))*)(void*)>(FN_KS_DRAW)(self);
}
// Build 109b: no pause in online fights. The fight screen (CMLBtl update 0x5A9C80) calls
// its pause menu's tick (CPauseMenu at Btl +0x44, 0x4EB290, thiscall) at 0x5A9E48 every
// fighting frame (fight state 6); that tick opens the menu on Start / Esc. Online it is
// not called at all (same on both PCs, every replay), unless the menu is somehow open
// already (+0x1D), so it can still close.
static const uint32_t CALL_PAUSE_TICK = 0x005A9E48, FN_PAUSE_TICK = 0x004EB290;
static uint32_t pauseSkips = 0;
static void __attribute__((thiscall)) H_PauseTick(void* self) {
    if (NetOn() && !reinterpret_cast<uint8_t*>(self)[0x1D]) {
        if (pauseSkips++ == 0) Log("boot: online fight: the game's pause menu is off");
        return;
    }
    reinterpret_cast<void(__attribute__((thiscall))*)(void*)>(FN_PAUSE_TICK)(self);
}
static bool HookCall(uint32_t at, uint32_t fn, void* to) {
    uint8_t e[5] = {0xE8};
    int32_t rel = (int32_t)(fn - (at + 5));
    memcpy(e + 1, &rel, 4);
    return util::BytesMatch(at, e, 5) && util::WriteRel32(at, 0xE8, to);
}
static void InstallBoxSkip() {
    int ok = 0;
    for (uint32_t a : CALLS_DLG_DONE) {
        uint8_t e[5] = {0xE8};
        int32_t rel = (int32_t)(FN_DLG_DONE - (a + 5));
        memcpy(e + 1, &rel, 4);
        if (util::BytesMatch(a, e, 5) && util::WriteRel32(a, 0xE8, (void*)H_DlgDone)) ok++;
    }
    for (uint32_t a : CALLS_DLG_ANSWER) {
        uint8_t e[5] = {0xE8};
        int32_t rel = (int32_t)(FN_DLG_ANSWER - (a + 5));
        memcpy(e + 1, &rel, 4);
        if (util::BytesMatch(a, e, 5) && util::WriteRel32(a, 0xE8, (void*)H_DlgAnswer)) ok++;
    }
    skipBox = ok == 6;
    Log("boot: end-of-fight rematch box skip %s (%d of 6 calls)", skipBox ? "installed" : "OFF, unexpected bytes", ok);
}

// Build 101: while our main menu covers the game's title screen, the title screen
// doesn't run at all (no logos, intro movie, "press start", title music, attract demo).
// Game main runs the current screen through the call at 0x5BE272 (already ours:
// H_ChildUpdate in mod.cpp), which asks HoldsTitle first.
static bool holdOn = false;
static uint32_t heldFrames = 0;
static bool HoldsTitle(void* screen) {
    bool title = screen && *reinterpret_cast<uint32_t*>(screen) == VT_TITLE;
    if (holdOn && title && g_holdTitle) {
        if (heldFrames++ == 0) Log("boot: the game's title screen is on hold under our main menu");
        return true;
    }
    if (heldFrames) { Log("boot: the game's title screen runs again (held %u frames)", heldFrames); heldFrames = 0; }
    return false;
}

static bool Install() {
    InstallBoxSkip();
    {   // build 109b
        bool p = HookCall(CALL_PAUSE_TICK, FN_PAUSE_TICK, (void*)H_PauseTick);
        Log("boot: pause menu block for online fights %s", p ? "installed" : "OFF (unexpected bytes)");
    }
    {   // build 107c
        bool u = HookCall(CALL_KS_UPDATE, FN_KS_UPDATE, (void*)H_KsUpdate), d = HookCall(CALL_KS_DRAW, FN_KS_DRAW, (void*)H_KsDraw);
        Log("boot: Knight set menu on the loading screen: skip %s, hide %s", u ? "installed" : "OFF (unexpected bytes)", d ? "installed" : "OFF (unexpected bytes)");
    }
    {   // build 107b
        uint8_t e[5] = {0xE8};
        int32_t rel = (int32_t)(FN_DLG_DRAW - (CALL_DLG_DRAW + 5));
        memcpy(e + 1, &rel, 4);
        bool ok = util::BytesMatch(CALL_DLG_DRAW, e, 5) && util::WriteRel32(CALL_DLG_DRAW, 0xE8, (void*)H_DlgDraw);
        Log("boot: hiding the game's boxes under our after-match screen %s", ok ? "installed" : "OFF, unexpected bytes");
    }
    {
        wchar_t ini[MAX_PATH];
        _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
        holdOn = GetPrivateProfileIntW(L"ui", L"hold_title", 1, ini) != 0;
        Log("boot: title hold %s", holdOn ? "on (the game's title screen waits while our main menu is up)" : "OFF ([ui] hold_title=0)");
    }
    if (util::BytesMatch(CALL_BTL_FINISHED, EXPECT_CALL_BTL_FINISHED, sizeof EXPECT_CALL_BTL_FINISHED))
        hookedAfter = util::WriteRel32(CALL_BTL_FINISHED, 0xE8, (void*)H_BtlFinished);
    Log("boot: after-fight hook %s", hookedAfter ? "installed" : "NOT installed (unexpected bytes); the game's own after-match screens will run");
    if (!util::BytesMatch(CALL_NEXT_SCREEN, EXPECT_CALL_NEXT_SCREEN, sizeof EXPECT_CALL_NEXT_SCREEN)) {
        Log("boot: unexpected bytes at 0x%08X, starting fights from the overlay is OFF", CALL_NEXT_SCREEN);
        return false;
    }
    hooked = util::WriteRel32(CALL_NEXT_SCREEN, 0xE8, (void*)H_NextScreen);
    Log("boot: next-screen hook %s", hooked ? "installed" : "FAILED");
    return hooked;
}

// The lobby has agreed settings for a new match: boot it.
static void Request(const MatchSettings& ms) {
    if (!hooked) return;
    if (ms.matchNo == bootedMatch && st != B_IDLE) return;
    s = ms;
    bootedMatch = ms.matchNo;
    SetState(B_WAIT_TITLE);
    Log("boot: match %u requested", ms.matchNo);
}

// Build 107: a fight starts (from the loading screen, or again in place after a rematch)
static void FightStarts() {
    HudResetMatch();                     // build 109: stutters count per match
    postFight = false;
    answerNo = false;
    rmPending = false;
    winsAtStart[0] = U32(G_WINS);
    winsAtStart[1] = U32(G_WINS + 4);
}
// Build 107: the fight's result phase: the after-match screen comes up over it
static void PostStarts() {
    postFight = true;
    uint32_t w0 = U32(G_WINS), w1 = U32(G_WINS + 4);
    // build 107b (test 107: the game's win counts stay 0 in Versus): the fight's result block
    // 0x04B79480: +0x88 draw (byte), +0x8C winner (0 / 1), as the result phase reads it
    bool draw = *reinterpret_cast<uint8_t*>(G_RESULT + 0x88) != 0;
    uint32_t wi = U32(G_RESULT + 0x8C);
    int winner = draw || wi > 1 ? -1 : (int)wi;
    EnterCriticalSection(&g_lobbyLock);
    LbSide& me = LB.me;
    if (winner >= 0) LB.setWins[winner]++;
    LB.lastWinner = (int8_t)winner;
    LB.postMatch = me.matchNo;
    if (me.phase == LP_MATCH && me.matchNo == bootedMatch) {
        me.phase = LP_POST;
        me.choice = LCH_NONE;
        me.choiceMatch = 0;
        LbChanged();
        LbNote("match %u is over", me.matchNo);
    }
    LB.postAt = GetTickCount();
    LeaveCriticalSection(&g_lobbyLock);
    g_postOn = true;
    Log("boot: fight frame %u: result phase: %s (result block: draw %d, winner %u; game counts %u/%u); set %u-%u; after-match screen up, rollback stays on",
        stateFrames, winner < 0 ? "draw" : winner == 0 ? "player 1 won" : "player 2 won", (int)draw, wi, w0, w1, LB.setWins[0], LB.setWins[1]);
}
// Build 107: can a rematch start in place now (both PCs still in this fight's result phase)?
static bool InPlaceOk() {
    uint32_t vt, ptr;
    CurScreen(&vt, &ptr);
    return hooked && skipBox && st == B_FIGHT && postFight && !answerNo && vt == SCENE_BTL && U32(FIGHT_PHASE) == 7 && NetOn() && N.synced;
}
// Lobby (game thread): the rematch is agreed; after frame `at` both PCs start the new fight
static void InPlaceArm(uint32_t matchNo, uint32_t at) {
    bootedMatch = matchNo;
    rmAt = at;
    rmPending = true;
    Log("boot: rematch (match %u) in place: the fight starts again after frame %u (now %u)", matchNo, at, g_frame);
    if (g_frame > at) Log("boot: warning: this PC is already %u frames past the agreed frame", g_frame - at);
}
// Frame rmAt is done on both PCs (same state): treat what comes next as a new synced
// screen, so both games wait for each other and start from frame 1 together (the
// netcode resets its frame count, inputs and rollback for it). Frame 1 answers YES.
static void RematchNow() {
    rmPending = false;
    if (g_frame != rmAt) Log("boot: warning: rematch restart at frame %u, agreed %u", g_frame, rmAt);
    rmYesEpoch = (uint16_t)(N.epoch + 1);
    rmYesArmed = true;
    N.sceneVt = 0xFFFFFFFFu;            // NetBeforeInput: "a new screen", next frame
    Log("boot: frame %u: restarting the fight in place (synced screen %u next)", g_frame, rmYesEpoch);
}
// Lobby (game thread): Back to room agreed: answer the game's box NO, the fight screen ends
static void AnswerNo() {
    if (st != B_FIGHT || answerNo) return;
    if (g_mode == Mode::SyncTest && g_rbOn) g_rbFrozen = true;   // the answer changes now: no rewinds past it
    answerNo = true;
    rmPending = false;
    Log("boot: back to the room: answering the rematch box NO (rollback off for the rest of this fight)");
}
// Build 107: zero both players' input in our match's result phase (the box is ours now).
// 107b: 2 = press A instead (the rematch's synced screen: YES on the game's own box)
static int ZeroInput() {
    if (st != B_FIGHT || !postFight) return 0;
    return rmYesArmed && rmYesEpoch == N.epoch ? 2 : 1;
}

// Every game frame, before the update (game thread).
static void Tick() {
    if (!hooked) return;
    stateFrames++;
    if (NetOn()) {                       // a new agreed match -> boot it
        EnterCriticalSection(&g_lobbyLock);
        bool go = LB.me.phase == LP_MATCH && LB.haveSettings && LB.s.matchNo == LB.me.matchNo &&
                  (N.side == 1 || LB.confirmed > 0) && LB.s.matchNo != bootedMatch &&
                  st != B_FIGHT;          // build 107: a fight still on screen ends first (its box is answered NO)
        MatchSettings ms = LB.s;
        LeaveCriticalSection(&g_lobbyLock);
        if (go) Request(ms);
    }
    uint32_t vt, ptr;
    CurScreen(&vt, &ptr);
    if (g_dcExit) {                      // build 108: end the fight after "connection lost" (Btl +0x28 = leave)
        static uint32_t btlFrames = 0;
        if (vt == SCENE_BTL && ptr) {
            if (++btlFrames >= 3 && !*reinterpret_cast<uint8_t*>(ptr + 0x28)) {
                *reinterpret_cast<uint8_t*>(ptr + 0x28) = 1;
                Log("boot: connection lost: asking the fight screen to end");
            }
        } else {
            btlFrames = 0;
            if (vt == VT_TITLE) { g_dcExit = false; Log("boot: connection lost: back on the title screen"); }
        }
    }
    switch (st) {
    case B_IDLE: break;
    case B_WAIT_TITLE:
        if (!NetOn()) { SetState(B_IDLE); break; }
        if (vt != VT_TITLE) {
            if (stateFrames % 600 == 1) Log("boot: waiting for the main menu (now on %s)", addr::SceneName(vt));
            break;
        }
        if (stateFrames < 10) break;     // let the title settle
        {
            uint32_t scene = U32(addr::G_MAIN_SCENE);
            if (!scene || U32(scene) != VT_GAMEMAIN) break;
            if (!optSaved) { optRounds = U32(OPT_ROUNDS); optTimer = U32(OPT_TIMER); optSaved = true; }
            U32(OPT_ROUNDS) = s.rounds >= 1 ? s.rounds - 1u : 0u;
            U32(OPT_TIMER) = s.timer;
            if (!setSaved) {
                setOld[0] = U32(GAME_SETTINGS + 0x08); setOld[1] = U32(GAME_SETTINGS + 0x14);
                setOld[2] = U32(GAME_SETTINGS + 0x18); setOld[3] = U32(GAME_SETTINGS + 0x1C);
                setSaved = true;
            }
            U32(GAME_SETTINGS + 0x08) = U32(TIMER_TABLE + 4 * (s.timer < 4 ? s.timer : 2));
            U32(GAME_SETTINGS + 0x14) = U32(GAME_SETTINGS + 0x18) = U32(GAME_SETTINGS + 0x1C) = U32(OPT_ROUNDS) + 1u;
            Log("boot: game settings: timer %u, rounds %u (were %u, %u)", U32(GAME_SETTINGS + 0x08), U32(GAME_SETTINGS + 0x14), setOld[0], setOld[1]);
            U32(0x04DC9650) = 2;         // Versus
            U32(0x04DC9640) = 1;
            U32(0x04DC8A04) = 3;         // both sides human
            Log("boot: main menu: Versus set (options rounds %u, timer %u; were %u, %u); game main sides %u/%u -> 3",
                U32(OPT_ROUNDS), U32(OPT_TIMER), optRounds, optTimer, U32(scene + 0x0C), U32(scene + 0x10));
            U32(scene + 0x0C) = 3;
            if (U32(scene + 0x10) == 3) U32(scene + 0x10) = 0;   // make sure the game sees a change
            SetState(B_SWITCHING);
        }
        break;
    case B_SWITCHING:
        if (stateFrames > 120) {
            Log("boot: the game did not switch screens within 2 s (still on %s); giving up", addr::SceneName(vt));
            SetState(B_IDLE);
        }
        break;
    case B_LOADING:
        if (vt == VT_LOADING) loadingFrames++;
        if (vt == SCENE_BTL) {
            Log("boot: fight starts after %u loading-screen frames; assists rewritten %d times", loadingFrames, assistFixes);
            if (!AssistsOk()) { WriteAssists(); Log("boot: assists were different at fight start, written again"); }
            SetState(B_FIGHT);
            FightStarts();
            ksSkips = 0;
            break;
        }
        if (vt == VT_LOADING && !AssistsOk()) {
            Log("boot: loading frame %u: the set menu wrote P1 %u/%u/%u P2 %u/%u/%u; writing ours again", loadingFrames,
                U32(SETUP + 0x08), U32(SETUP + 0x0C), U32(SETUP + 0x10),
                U32(SETUP + SETUP_STRIDE + 0x08), U32(SETUP + SETUP_STRIDE + 0x0C), U32(SETUP + SETUP_STRIDE + 0x10));
            WriteAssists();
            assistFixes++;
        }
        if (stateFrames > 60 * 60) { Log("boot: still not in the fight after 60 s (on %s)", addr::SceneName(vt)); SetState(B_IDLE); }
        break;
    case B_FIGHT:
        {   // fight phase changes, to check the box skip (build 64)
            static uint32_t lastPhase = 0xFFFFFFFF, lastState = 0xFFFFFFFF;
            if (stateFrames == 1) { lastPhase = lastState = 0xFFFFFFFF; boxSkips = 0; }
            uint32_t ph = U32(FIGHT_PHASE), fs = U32(FIGHT_STATE);
            // build 107: rollback stays on in the result phase (build 74 switched it off there):
            // the box answers no longer change in the middle of a synced screen, and both
            // players' input is zero from here on (NetFill), so the two PCs stay the same
            // while they choose and the rematch can start from the same state
            if (ph == 7 && !postFight && NetOn()) PostStarts();
            if (rmYesArmed && ph != 7 && ph < 0x100) {
                rmYesArmed = false;
                restarting = true;
                Log("boot: fight frame %u: the game took the rematch (phase %d)", stateFrames, (int)ph);
            }
            if (restarting && ph != 8) {   // build 107b: our screen (and the hidden boxes) stay until the game restarts
                restarting = false;
                Log("boot: fight frame %u: the fight starts again in place (phase %d)", stateFrames, (int)ph);
                g_postOn = false;
                g_ptrMapStale = true;      // build 119: the restart moved the characters' data (see net_session.inc)
                FightStarts();
            }
            if (rmPending && g_frame >= rmAt) RematchNow();
            if (ph != lastPhase || fs != lastState) {
                Log("boot: fight frame %u: phase %d, state %d (box answers given: %d)", stateFrames, (int)ph, (int)fs, boxSkips);
                lastPhase = ph;
                lastState = fs;
            }
        }
        // Build 65 (test 64b, LAN): the box was answered (phase 9) but the fight screen
        // kept waiting with the box on screen. Once the result is in (phase 9), tell the
        // fight screen it is finished (its own "done" flag, +0x28): its next update ends
        // the fight the normal way and our after-fight hook takes over.
        if (vt == SCENE_BTL && ptr && U32(FIGHT_PHASE) == 9 && skipBox) {
            uint8_t& done = *reinterpret_cast<uint8_t*>(ptr + 0x28);
            if (!done) {
                done = 1;
                static int logs = 0;
                if (logs++ < 20) Log("boot: fight frame %u: result is in, ending the fight screen", stateFrames);
            }
        }
        // Build 78 (test 76b): after a desync the laptop's fight never ended while the desktop's
        // did, so its overlay never came back. When the other PC has finished this match and
        // ours is still fighting 10 s later, end our fight screen too (no more rollbacks).
        {
            static uint32_t otherDoneAt = 0;
            if (stateFrames == 1) otherDoneAt = 0;
            if (NetOn() && LB.haveThem && LB.me.phase == LP_MATCH && !postFight && LB.them.phase == LP_POST && LB.them.matchNo == LB.me.matchNo && vt == SCENE_BTL && ptr) {
                if (!otherDoneAt) otherDoneAt = stateFrames;
                else if (stateFrames - otherDoneAt == 600) {
                    if (g_mode == Mode::SyncTest && g_rbOn) g_rbFrozen = true;
                    *reinterpret_cast<uint8_t*>(ptr + 0x28) = 1;
                    Log("boot: fight frame %u: the other PC finished this match 10 s ago but this fight is still on (out of sync?); ending the fight screen", stateFrames);
                    LbNote("the other PC's fight ended; ending this one too");
                }
            }
        }
        if (vt != SCENE_BTL && vt) {
            Log("boot: the fight screen ended (now %s) without our after-fight hook; the game's own after-match flow runs", addr::SceneName(vt));
            SetState(B_IDLE);
        }
        break;
    case B_AFTER:
        if (vt == VT_TITLE) {
            if (!U.visible.load()) { U.visible = true; }
            U.autoOpened = true;
            Log("boot: back on the main menu; overlay open with the after-match choices");
            SetState(B_IDLE);
        } else if (stateFrames > 60 * 30) {
            Log("boot: not back on the main menu after 30 s (on %s)", addr::SceneName(vt));
            SetState(B_IDLE);
        }
        break;
    }
}

// Online, on the booted loading screen: both players "press A" every 16
// frames, so the assist set menu and the VS screen go on by themselves.
// T is the synced screen frame, the same on both PCs.
static void InputOverride(uint32_t T, uint32_t in[2][2]) {
    if (st != B_LOADING) return;
    uint32_t vt, ptr;
    CurScreen(&vt, &ptr);
    if (vt != VT_LOADING) return;
    uint32_t a = (T >= 30 && T % 16 < 2) ? 0x1000u : 0u;
    for (int p = 0; p < 2; p++) { in[p][0] = a; in[p][1] = 0; }
}

static void SessionEnd() {
    if (optSaved) {
        U32(OPT_ROUNDS) = optRounds;
        U32(OPT_TIMER) = optTimer;
        optSaved = false;
        Log("boot: options rounds/timer put back (%u, %u)", optRounds, optTimer);
    }
    if (setSaved) {
        U32(GAME_SETTINGS + 0x08) = setOld[0]; U32(GAME_SETTINGS + 0x14) = setOld[1];
        U32(GAME_SETTINGS + 0x18) = setOld[2]; U32(GAME_SETTINGS + 0x1C) = setOld[3];
        setSaved = false;
    }
    if (st != B_IDLE) SetState(B_IDLE);
    bootedMatch = 0;
    postFight = rmPending = rmYesArmed = answerNo = restarting = false;
    g_postOn = false;
}

} // namespace boot

static void BootSessionEnd() { boot::SessionEnd(); }
static bool BootInMatch() { return boot::st != boot::B_IDLE; }
static bool BootInPlaceOk() { return boot::InPlaceOk(); }
static void BootInPlaceArm(uint32_t matchNo, uint32_t at) { boot::InPlaceArm(matchNo, at); }
static int BootZeroInput() { return boot::ZeroInput(); }
static void BootAnswerNo() { boot::AnswerNo(); }
static void BootInputOverride(uint32_t T, uint32_t in[2][2]) { boot::InputOverride(T, in); }
