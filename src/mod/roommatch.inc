// ===========================================================================
// Build 112: matches from the room's tables (included into overlay.inc).
//
// The room (Steam lobby) says who sits where and who pressed READY (member data,
// transport.cpp). When you and the player across the table are both READY, this
// PC starts the usual two-player session with that player (net_session.inc;
// 1P / 2P from the seats) and walks the old lobby phases by itself: ready with
// My setup as the picks, locked, and player 1 starts the match once the
// 3-second countdown is over. The fight boots as before (boot.inc).
//
// After the match, "Back to room" on both sides: the game goes back to its title
// screen, our main menu opens on the room, and a moment later the session ends
// quietly. Both stay seated; READY starts the next set.
//
// Runs on the game thread (RoomMatchTick from UiGameThreadTick). The room screen
// reads RMV (what to show at your table).
// ===========================================================================
// (RMV, what the room screen shows, is in net_session.inc next to g_roomCfg)
static const uint32_t RM_COUNTDOWN_MS = 3000, RM_CONNECT_MS = 25000, RM_END_AFTER_TITLE_MS = 1500;
static struct {
    int table = -1;
    uint64_t peer = 0;
    char peerName[48] = "";
    uint32_t titleAt = 0;                 // back on the title screen after the set (0 = not yet)
    bool started = false;                 // the first match of this session began
    char su[64] = "", sc[32] = "", play[4] = "";
} RM;
static bool RoomMatchOn() { return g_roomCfg.on; }
// quiet ends: no "connection lost" pop-up for a room match that never started a fight
// or that both players left for the room (DcOnEnd asks)
static bool RoomMatchQuietEnd() {
    if (!g_roomCfg.on) return false;
    EnterCriticalSection(&g_lobbyLock);
    bool fighting = LB.me.phase == LP_MATCH || LB.me.phase == LP_POST;
    LeaveCriticalSection(&g_lobbyLock);
    return !fighting || !BootInMatch();
}

static void RmPublish(const char* key, char* last, int cap, const char* v) {
    if (!strcmp(last, v)) return;
    snprintf(last, cap, "%s", v);
    net::RoomSetMyData(key, v);
}

static void RmEnd(const char* why, const char* chat) {
    if (N.st != NS_OFF) NetEnd(why);
    if (chat && chat[0]) net::RoomSystemLine(chat);
    Log("room: table match over (%s)", why);
    g_roomCfg = RoomMatchCfg();
    RM.table = -1; RM.peer = 0; RM.titleAt = 0; RM.started = false;
    RMV.st = RM_IDLE; RMV.connected = false;
}

static void RoomMatchTick(uint32_t vt) {
    static net::RoomView R;            // big: not on the stack
    static bool iniRead = false;
    if (!iniRead) {   // [dev] room_standin=1: a test player sits across from you in your own room
        iniRead = true;
        wchar_t ini[MAX_PATH];
        _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
        bool on = GetPrivateProfileIntW(L"dev", L"room_standin", 0, ini) != 0;
        net::RoomStandIn(on);
        if (on) Log("room: test player (stand-in) is on: it sits across from you and copies READY");
    }
    net::RoomGet(&R);
    bool inRoom = R.state == net::ROOM_IN && R.id;
    uint32_t now = GetTickCount();

    // ---- what this PC shows the room ----
    if (inRoom) {
        if (!SU.loaded) SuLoad();
        char v[64];
        snprintf(v, sizeof v, "%d,%d,%d,%d,%d,%d", SU.chara, SuColor(), SU.stage, SuInUse().a[0], SuInUse().a[1], SuInUse().a[2]);
        RmPublish("su", RM.su, sizeof RM.su, v);
    } else RM.su[0] = RM.sc[0] = RM.play[0] = 0;   // a new room gets everything again

    // ---- a match running from a table ----
    if (g_roomCfg.on) {
        bool fighting;
        uint32_t matchNo, w0, w1;
        EnterCriticalSection(&g_lobbyLock);
        LbSide& me = LB.me;
        if (N.st == NS_ON) {
            RMV.connected = true;
            if (!RM.started) {
                // walk the lobby phases by ourselves: ready with My setup, locked, start
                if (me.phase == LP_LOBBY && !me.ready) {
                    LeaveCriticalSection(&g_lobbyLock);
                    SuApply();                      // My setup -> this PC's picks
                    EnterCriticalSection(&g_lobbyLock);
                    me.ready = true;
                    me.rounds = (uint8_t)ImClamp(R.set.rounds, 1, 5);
                    me.timer = (uint8_t)(R.set.timer & 3);
                    LbChanged();
                }
                if (me.phase == LP_SETUP && !me.locked) { me.locked = true; LbChanged(); }
                if (N.side == 0 && me.phase == LP_SETUP && now - RMV.t0.load() >= RM_COUNTDOWN_MS) LB.startNow = true;
            }
            if (me.phase == LP_MATCH && !RM.started) { RM.started = true; RMV.st = RM_PLAYING; }
        }
        fighting = me.phase == LP_MATCH || me.phase == LP_POST;
        bool backToRoom = RM.started && me.phase == LP_LOBBY;
        matchNo = me.matchNo; w0 = LB.setWins[0]; w1 = LB.setWins[1];
        LeaveCriticalSection(&g_lobbyLock);

        if (N.st == NS_OFF) {   // ended on its own (other player left, connection lost, could not connect)
            RmPublish("play", RM.play, sizeof RM.play, "0");
            RmPublish("sc", RM.sc, sizeof RM.sc, "");
            char s[120];
            snprintf(s, sizeof s, RM.started ? "The match with %s ended" : "Could not start the match with %s", RM.peerName);
            if (backToRoom) s[0] = 0;          // the other game ended it first on the way back to the room
            if (!RM.started) net::RoomSetReady(false);
            RmEnd("session over", s);
            return;
        }
        if (RM.started) {
            char v[32];
            snprintf(v, sizeof v, "%u,%u,%u", matchNo, w0, w1);
            RmPublish("play", RM.play, sizeof RM.play, "1");
            RmPublish("sc", RM.sc, sizeof RM.sc, v);
            if (R.myReady) net::RoomSetReady(false);   // READY is used up; the next set needs a new one
        }
        // still getting there: the other player stood up, left or took READY back -> cancel
        if (!RM.started) {
            const net::RoomMember* p = nullptr;
            for (int i = 0; i < R.nMembers; i++) if (R.members[i].id == RM.peer) p = &R.members[i];
            // before the two games are connected both seats count; once connected only this
            // player's own (the other game ends the session itself, and its READY is used up
            // a little earlier than this one's when the match starts)
            bool self = inRoom && R.mySeat >= 0 && R.mySeat / 2 == RM.table && R.myReady;
            bool ok = self && (N.st == NS_ON || (p && p->seat == (R.mySeat ^ 1) && p->ready));
            if (!ok) { RmEnd("the match start was cancelled", "Match start cancelled"); return; }
            if (now - RMV.t0.load() > RM_CONNECT_MS && N.st != NS_ON) {
                char s[120];
                snprintf(s, sizeof s, "Could not connect to %s. Press READY to try again.", RM.peerName);
                net::RoomSetReady(false);
                RmEnd("could not connect in time", s);
                return;
            }
        }
        // both chose Back to room: once the game is back on its title screen (our main menu
        // covers it), end the session a moment later (both games get there together)
        if (backToRoom && !fighting && !BootInMatch()) {
            if (vt == VT_TITLE) {
                if (!RM.titleAt) RM.titleAt = now;
                else if (now - RM.titleAt >= RM_END_AFTER_TITLE_MS) {
                    RmPublish("play", RM.play, sizeof RM.play, "0");
                    RmPublish("sc", RM.sc, sizeof RM.sc, "");
                    RmEnd("back to the room", "");
                }
            }
        }
        if (!inRoom && !fighting && !BootInMatch()) RmEnd("you left the room", "");
        return;
    }

    // ---- not playing: start when both seats of my table are READY ----
    RMV.st = RM_IDLE;
    if (!inRoom || R.mySeat < 0 || !R.myReady || N.st != NS_OFF || g_mode != Mode::Idle || g_auto) return;
    const net::RoomMember* p = nullptr;
    for (int i = 0; i < R.nMembers; i++) if (R.members[i].seat == (R.mySeat ^ 1)) p = &R.members[i];
    if (!p || !p->ready || p->playing) return;
    g_roomCfg.on = true;
    g_roomCfg.loop = p->standin;
    g_roomCfg.peer = p->standin ? 0 : p->id;
    g_roomCfg.side = R.mySeat & 1;
    g_roomCfg.setLen = R.set.setLen;   // build 125: wins that end a set, first to N (0 = no limit)
    RM.table = R.mySeat / 2;
    RM.peer = p->id;
    RM.started = false;
    RM.titleAt = 0;
    snprintf(RM.peerName, sizeof RM.peerName, "%s", p->name);
    RMV.t0 = now;
    RMV.connected = false;
    RMV.st = RM_STARTING;
    Log("room: table %c: you (%dP) and %s (%dP) are both READY; starting the session", 'A' + RM.table, (R.mySeat & 1) + 1, p->name, (p->seat & 1) + 1);
    {
        snprintf(g_uiName, sizeof g_uiName, "%s", net::SteamMyName()[0] ? net::SteamMyName() : U.name);
        NetStart();
    }
    if (N.st == NS_OFF) {   // could not even start (no Steam ...)
        net::RoomSetReady(false);
        RmEnd("could not start", "Could not start the match");
    }
}
