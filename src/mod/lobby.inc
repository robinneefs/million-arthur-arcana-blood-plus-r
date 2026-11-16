// ===========================================================================
// Build 57: lobby and match setup over the network (included into mod.cpp,
// after net_session.inc)
// ===========================================================================
// Our own overlay (overlay.inc) replaces the game's menus before a match.
// This file is the part both PCs must agree on: who is ready, what each
// player picked, and the final match settings.
//
//   - Each PC sends its own side's whole state (M_LOBBY) every time it
//     changes, and repeats it until the other PC confirms that version (and
//     once a second as a heartbeat). A lost packet never matters, because
//     the next one carries everything again.
//   - Both PCs follow the same rules on the same two states, so they move
//     between the phases (lobby -> match setup -> match -> after the match)
//     together.
//   - Player 1 (host) makes the final settings (M_START): both sides' picks,
//     the stage, random choices resolved, input delay, rounds, a seed. Player
//     2 answers M_STARTACK with a checksum of what it got, so both logs show
//     the two PCs agree.
//
// Threads: the overlay draws on the game's render call, the network runs on
// the game thread. The overlay only changes "wishes" (ready, locked, picks,
// choice) under g_lobbyLock; the rules and all sending run on the game
// thread in LobbyTick().
// ===========================================================================

// Build 67: real names and ranges, from the game's own English text file
// (flashrom/text/textData_English.tdf: every byte minus 0x32, then UTF-16)
// and the files in flashrom/act. 13 playable characters (00-0C); 0D is the
// boss (Arcana Blood Corrupted Iai Arthur). Build 70: left out of the list
// again because a fight with her crashed during a rollback replay (test 69). 21 colours per character (palette slots
// 0-20, the game shows them as 01-21; the boss has only 1, test 67). 31 assist characters
// (act/partner 00-1E). Stages bg/stage00-0C still have no names: the game's
// Versus stage select offers 8 of these 13.
static const int LB_CHARS = 13, LB_CHARS_ALL = 14, LB_BOSS = 0x0D, LB_COLORS = 21, LB_BOSS_COLORS = 1,
                 LB_ASSISTS = 31, LB_STAGES = 8, LB_TIMERS = 4;
// Build 69/71: the game has 13 stage files (00-0C); 09, 0A, 0B and 0C are
// copies of stage 00 that draw the characters black (tests 65b, 68, 70), so
// they are left out. Picks hold a position in this list (Stage 01-10); the match settings
// hold the game's own stage id.
// Build 102: only the 8 stages of normal play (the ones with a picture in the game's stage
// select, MA10_CSEL 6-13); 08 is left out, also from Random.
static const uint8_t LB_STAGE_IDS[LB_STAGES] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
// Build 72: [ui] show_boss=1 in rollback.ini puts the boss back in the list, for the boss-crash test only.
static int g_showBoss = -1;
static inline int LbCharCount() {
    if (g_showBoss < 0) {
        wchar_t ini[MAX_PATH];
        _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
        g_showBoss = GetPrivateProfileIntW(L"ui", L"show_boss", 0, ini) ? 1 : 0;
    }
    return g_showBoss ? LB_CHARS_ALL : LB_CHARS;
}
static inline int LbColorCount(uint8_t chara) { return chara == LB_BOSS ? LB_BOSS_COLORS : LB_COLORS; }
static const char* const LB_CHAR_NAMES[LB_CHARS_ALL] = {
    "Twinblade Arthur", "Blade-Protector Arthur", "Thief Arthur", "Zechs Siegfried", "Hybrid Nimue",
    "Type II Bisclavret", "Void Eternal Flame", "Unique Snow White", "Otherworldly Riesz", "Iai Arthur",
    "Otherworldly Koume Sakiyama", "Wildcat Arthur", "Otherworldly Iori Yagami",
    "Corrupted Iai Arthur (boss)"};
static const char* const LB_ASSIST_NAMES[LB_ASSISTS] = {
    "Clone Techno-Smith Arthur", "Clone Magic Guild Arthur", "Clone Fay", "Clone Lif\xC3\xA9" "e", "Clone Elle",
    "Type I Lancelot", "Type I Gawain", "Type I Galahad", "Ruler Supreme Guinevere", "Type I Mordred",
    "Monarch Constantine", "Type II Peridod", "Clone Mercenary Arthur", "Clone Merchant Arthur", "Clone Diva Arthur",
    "Clone Scathach", "Clone Uathach", "Clone Aife", "Clone Cuchulainn", "Clone Pharsalia",
    "Summer Evaine", "Supporter Enide", "Melancholy Princess Kaguya", "Supporter Claire", "Type II Tor",
    "Type II Balin", "Unique Little Grey", "Foible Elle", "Clone Azia Kriemhild", "Otherworldly Hawkeye",
    "Otherworldly Akira Ono"};
static const char* const LB_TIMER_NAMES[LB_TIMERS] = {"30 seconds", "60 seconds", "99 seconds", "No time limit"};
static const uint8_t LB_RANDOM = 0xFF;          // "Random" in a pick
enum LbPhase : uint8_t { LP_LOBBY = 0, LP_SETUP = 1, LP_MATCH = 2, LP_POST = 3 };
enum LbChoice : uint8_t { LCH_NONE = 0, LCH_REMATCH = 1, LCH_SETUP = 2, LCH_LOBBY = 3, LCH_LEAVE = 4 };
static const char* const LB_PHASE_NAME[] = {"lobby", "match setup", "match", "after the match"};
static const char* const LB_CHOICE_NAME[] = {"nothing yet", "rematch", "back to match setup", "back to the lobby", "leave"};

#pragma pack(push, 1)
struct LbPicks { uint8_t chara = 0, color = 0, assist[3] = {0, 1, 2}, stage = LB_RANDOM; };
struct NLobby {
    NHdr h;
    uint32_t seq, ack;                  // version of this state / newest version of yours I have
    uint8_t phase, ready, locked, choice;
    LbPicks p;
    uint8_t delay, rounds, timer, pad1; // delay / rounds / timer: player 1's settings
    uint32_t matchNo;                   // the match the phase belongs to
    uint32_t choiceMatch;               // the match the choice was made after
    char name[32];
};
struct MatchSettings {
    uint32_t matchNo;
    uint8_t chara[2], color[2], assist[2][3];
    uint8_t stage, rounds, delay, timer;
    uint32_t seed;
};
struct NStart { NHdr h; MatchSettings s; };
struct NStartAck { NHdr h; uint32_t matchNo, sum; };
#pragma pack(pop)


struct LbSide {
    uint32_t seq = 0;
    uint8_t phase = LP_LOBBY;
    bool ready = false, locked = false;
    uint8_t choice = LCH_NONE;
    LbPicks p;
    uint8_t delay = 2, rounds = 2, timer = 2;
    uint32_t matchNo = 0;
    uint32_t choiceMatch = 0;           // the choice stays visible after it happened, so the other PC can follow
    char name[32] = "";
};
static CRITICAL_SECTION g_lobbyLock;
static struct Lobby {
    LbSide me, them;
    bool haveThem = false;
    uint32_t themAck = 0;               // newest version of ours the other PC has
    uint32_t lastSend = 0;
    // match
    bool haveSettings = false;
    MatchSettings s{};
    uint32_t sum = 0;
    int confirmed = 0;                  // player 1: 0 waiting, 1 same on both, -1 different
    uint32_t startAt = 0;               // player 1: countdown end (GetTickCount), 0 = none
    bool startNow = false;              // player 1 pressed Start
    bool leaveNow = false;              // "Leave" pressed (handled on the game thread)
    char note[160] = "";                // last thing that happened, for the overlay
    uint32_t noteAt = 0;
} LB;

static void LbNote(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(LB.note, sizeof LB.note, fmt, ap);
    va_end(ap);
    LB.noteAt = GetTickCount();
    Log("lobby: %s", LB.note);
}
// Call with g_lobbyLock held, after changing LB.me.
static inline void LbChanged() { LB.me.seq++; LB.lastSend = 0; }

static uint32_t LbSum(const MatchSettings& s) {
    uint32_t h = 0x811C9DC5u;
    const uint8_t* b = (const uint8_t*)&s;
    for (size_t i = 0; i < sizeof s; i++) h = (h ^ b[i]) * 0x01000193u;
    return h;
}
static const char* LbCharName(uint8_t c) {
    static char buf[4][24];
    static int k = 0;
    if (c == LB_RANDOM) return "Random";
    if (c < LB_CHARS_ALL) return LB_CHAR_NAMES[c];
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof buf[k], "Character %02X", c);
    return buf[k];
}
// s = position in LB_STAGE_IDS (a pick)
static const char* LbStageName(uint8_t s) {
    static char buf[4][24];
    static int k = 0;
    if (s == LB_RANDOM) return "Random";
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof buf[k], "Stage %02u", s + 1u);
    return buf[k];
}
// id = the game's stage id (match settings)
static const char* LbStageIdName(uint8_t id) {
    for (int i = 0; i < LB_STAGES; i++) if (LB_STAGE_IDS[i] == id) return LbStageName((uint8_t)i);
    static char buf[24];
    snprintf(buf, sizeof buf, "Stage id %02X", id);
    return buf;
}
static const char* LbColorName(uint8_t c) {
    static char buf[4][16];
    static int k = 0;
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof buf[k], "Colour %02u", c + 1u);
    return buf[k];
}
static const char* LbAssistName(uint8_t a) {
    static char buf[4][16];
    static int k = 0;
    if (a < LB_ASSISTS) return LB_ASSIST_NAMES[a];
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof buf[k], "Assist %02X", a);
    return buf[k];
}
static const char* LbTimerName(uint8_t t) { return LB_TIMER_NAMES[t < LB_TIMERS ? t : 2]; }

static void LbNoteStart() {
    const MatchSettings& m = LB.s;
    LbNote("match %u starts: %s (id %02X, colour %02u, knights %02X/%02X/%02X) vs %s (id %02X, colour %02u, knights %02X/%02X/%02X), %s (id %02X), rounds %u, %s, checksum %08X",
           m.matchNo, LbCharName(m.chara[0]), m.chara[0], m.color[0] + 1u, m.assist[0][0], m.assist[0][1], m.assist[0][2],
           LbCharName(m.chara[1]), m.chara[1], m.color[1] + 1u, m.assist[1][0], m.assist[1][1], m.assist[1][2],
           LbStageIdName(m.stage), m.stage, m.rounds, LbTimerName(m.timer), LB.sum);
}

static void LbReset() {
    EnterCriticalSection(&g_lobbyLock);
    uint32_t seq = LB.me.seq;
    LbPicks keep = LB.me.p;             // picks are remembered between sessions
    LB = Lobby();
    LB.me.seq = seq + 1;
    LB.me.p = keep;
    LB.me.delay = (uint8_t)g_ncfg.delay;
    {   // build 104: the name is always the Steam name (the typed name only without Steam)
        const char* sn = net::SteamMyName();
        if (sn[0]) snprintf(g_uiName, sizeof g_uiName, "%s", sn);
    }
    snprintf(LB.me.name, sizeof LB.me.name, "%s", g_uiName);
    LeaveCriticalSection(&g_lobbyLock);
}

static void LbSend() {
    NLobby m{};
    m.h = MakeHdr(M_LOBBY);
    m.seq = LB.me.seq;
    m.ack = LB.them.seq;
    m.phase = LB.me.phase;
    m.ready = LB.me.ready;
    m.locked = LB.me.locked;
    m.choice = LB.me.choice;
    m.p = LB.me.p;
    m.delay = LB.me.delay;
    m.rounds = LB.me.rounds;
    m.timer = LB.me.timer;
    m.matchNo = LB.me.matchNo;
    m.choiceMatch = LB.me.choiceMatch;
    memcpy(m.name, LB.me.name, sizeof m.name);
    NetSend(&m, sizeof m);
    LB.lastSend = GetTickCount();
}

// Packets in (game thread, from NetHandle).
static void LbHandle(const uint8_t* p, int n) {
    const NHdr* h = (const NHdr*)p;
    EnterCriticalSection(&g_lobbyLock);
    if (h->type == M_LOBBY && n >= (int)sizeof(NLobby)) {
        const NLobby* m = (const NLobby*)p;
        if (m->ack > LB.themAck && m->ack <= LB.me.seq) LB.themAck = m->ack;
        if (!LB.haveThem || m->seq > LB.them.seq) {
            bool first = !LB.haveThem;
            LbSide& t = LB.them;
            bool wasReady = t.ready, wasLocked = t.locked;
            t.seq = m->seq;
            t.phase = m->phase <= LP_POST ? m->phase : LP_LOBBY;
            t.ready = m->ready != 0;
            t.locked = m->locked != 0;
            t.choice = m->choice <= LCH_LEAVE ? m->choice : LCH_NONE;
            t.p = m->p;
            t.delay = m->delay;
            t.rounds = m->rounds;
            t.timer = m->timer < LB_TIMERS ? m->timer : 2;
            t.matchNo = m->matchNo;
            t.choiceMatch = m->choiceMatch;
            memcpy(t.name, m->name, sizeof t.name);
            t.name[sizeof t.name - 1] = 0;
            LB.haveThem = true;
            if (first) LbNote("%s joined the lobby", t.name[0] ? t.name : "the other player");
            else {
                if (t.ready != wasReady && t.phase == LP_LOBBY) LbNote("%s is %s", t.name, t.ready ? "ready" : "not ready");
                if (t.locked != wasLocked && t.phase == LP_SETUP && LB.me.phase == LP_SETUP) LbNote("%s %s", t.name, t.locked ? "locked in" : "is changing their picks");
            }
            LB.lastSend = 0;            // confirm it
        }
    } else if (h->type == M_START && n >= (int)sizeof(NStart) && N.side == 1) {
        const NStart* m = (const NStart*)p;
        if (!LB.haveSettings || m->s.matchNo != LB.s.matchNo) {
            LB.s = m->s;
            LB.sum = LbSum(LB.s);
            LB.haveSettings = true;
            LB.confirmed = 1;           // this side has player 1's settings by definition
            LB.me.phase = LP_MATCH;
            LB.me.matchNo = LB.s.matchNo;
            LB.me.choice = LCH_NONE;
            LbChanged();
            LbNoteStart();
        }
        NStartAck a{};
        a.h = MakeHdr(M_STARTACK);
        a.matchNo = LB.s.matchNo;
        a.sum = LB.sum;
        NetSend(&a, sizeof a);
    } else if (h->type == M_STARTACK && n >= (int)sizeof(NStartAck) && N.side == 0) {
        const NStartAck* m = (const NStartAck*)p;
        if (LB.haveSettings && m->matchNo == LB.s.matchNo && LB.confirmed == 0) {
            LB.confirmed = m->sum == LB.sum ? 1 : -1;
            if (LB.confirmed > 0) LbNote("the other PC has the same match settings (checksum %08X)", m->sum);
            else LbNote("SETTINGS DIFFER: the other PC has checksum %08X, this one %08X", m->sum, LB.sum);
        }
    }
    LeaveCriticalSection(&g_lobbyLock);
}

// Player 1: turn both sides' picks into the final match settings.
static void LbMakeSettings() {
    MatchSettings s{};
    s.matchNo = (LB.me.matchNo > LB.them.matchNo ? LB.me.matchNo : LB.them.matchNo) + 1;
    s.seed = N.seed ^ (s.matchNo * 0x9E3779B1u);
    uint32_t r = s.seed | 1;
    auto rnd = [&](uint32_t n) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return r % n; };
    const LbPicks* pk[2] = {&LB.me.p, &LB.them.p};   // player 1 is this PC
    for (int i = 0; i < 2; i++) {
        s.chara[i] = pk[i]->chara == LB_RANDOM || pk[i]->chara >= LbCharCount() ? (uint8_t)rnd(LB_CHARS) : pk[i]->chara;
        s.color[i] = pk[i]->color < LbColorCount(s.chara[i]) ? pk[i]->color : 0;   // never a colour the character doesn't have
        for (int k = 0; k < 3; k++) s.assist[i][k] = pk[i]->assist[k] < LB_ASSISTS ? pk[i]->assist[k] : 0;
    }
    uint8_t a = pk[0]->stage, b = pk[1]->stage;
    uint8_t st = a == b ? a : (rnd(2) ? a : b);       // each picks; the game chooses one of the two
    s.stage = LB_STAGE_IDS[st == LB_RANDOM || st >= LB_STAGES ? rnd(LB_STAGES) : st];
    s.rounds = LB.me.rounds;
    s.timer = LB.me.timer;
    s.delay = LB.me.delay;
    LB.s = s;
    LB.sum = LbSum(s);
    LB.haveSettings = true;
    LB.confirmed = 0;
}
static void LbSendStart() {
    NStart m{};
    m.h = MakeHdr(M_START);
    m.s = LB.s;
    NetSend(&m, sizeof m);
}

static uint32_t g_lbLastStartSend = 0;
// Every frame on the game thread while connected.
static void LobbyTick() {
    if (N.st != NS_ON) return;
    bool leave = false;
    EnterCriticalSection(&g_lobbyLock);
    uint32_t now = GetTickCount();
    LbSide& me = LB.me;
    const LbSide& them = LB.them;
    bool host = N.side == 0;
    if (LB.leaveNow) leave = true;
    // ---- the rules (same on both PCs) ----
    if (LB.haveThem && !leave) {
        if (!host && them.delay != me.delay) { me.delay = them.delay; }      // player 1's settings
        if (!host && them.rounds != me.rounds) { me.rounds = them.rounds; }
        if (!host && them.timer != me.timer) { me.timer = them.timer; }
        switch (me.phase) {
        case LP_LOBBY:
            if (me.ready && them.ready && (them.phase == LP_LOBBY || them.phase == LP_SETUP)) {
                me.phase = LP_SETUP;
                me.locked = false;
                LbChanged();
                LbNote("both players are ready: match setup");
            }
            break;
        case LP_SETUP:
            if (them.phase == LP_LOBBY && !them.ready) {
                me.phase = LP_LOBBY;
                me.ready = me.locked = false;
                LB.startAt = 0;
                LbChanged();
                LbNote("%s went back to the lobby", them.name);
                break;
            }
            if (host) {
                bool both = me.locked && them.locked && them.phase == LP_SETUP;
                if (!both) { LB.startAt = 0; LB.startNow = false; }
                else if (!LB.startAt) LB.startAt = now + 3000;
                if (both && (LB.startNow || (int32_t)(now - LB.startAt) >= 0)) {
                    LbMakeSettings();
                    me.phase = LP_MATCH;
                    me.matchNo = LB.s.matchNo;
                    me.choice = LCH_NONE;
                    LB.startAt = 0;
                    LB.startNow = false;
                    LbChanged();
                    LbSendStart();
                    g_lbLastStartSend = now;
                    LbNoteStart();
                }
            }
            break;
        case LP_MATCH:
            // The fight ends through boot.inc (after-fight hook), or by hand with
            // "Stuck? End this match" in the overlay; the other side follows.
            if (them.phase == LP_POST && them.matchNo == me.matchNo) {
                me.phase = LP_POST;
                me.choice = LCH_NONE;
                me.choiceMatch = 0;
                LbChanged();
                LbNote("match %u is over", me.matchNo);
            }
            if (host && LB.confirmed == 0 && now - g_lbLastStartSend > 300) { LbSendStart(); g_lbLastStartSend = now; }
            break;
        case LP_POST:
            if (me.choice != LCH_NONE && me.choiceMatch == me.matchNo && them.choice == me.choice && them.choiceMatch == me.matchNo) {
                uint8_t c = me.choice;
                LbNote("both chose: %s", LB_CHOICE_NAME[c]);
                if (c == LCH_REMATCH) {
                    me.matchNo++;
                    LB.s.matchNo = me.matchNo;
                    LB.sum = LbSum(LB.s);
                    me.phase = LP_MATCH;
                    LbNote("rematch: match %u, same settings (checksum %08X)", me.matchNo, LB.sum);
                } else if (c == LCH_SETUP) {
                    me.phase = LP_SETUP;
                    me.locked = false;
                } else if (c == LCH_LOBBY) {
                    me.phase = LP_LOBBY;
                    me.ready = me.locked = false;
                } else if (c == LCH_LEAVE) {
                    leave = true;
                }
                LbChanged();                // (the choice stays in the messages until the next match ends)
            }
            break;
        }
    }
    // ---- send ----
    bool unconfirmed = LB.themAck < me.seq;
    if (LB.lastSend == 0 || now - LB.lastSend > (unconfirmed ? 100u : 1000u)) LbSend();
    LeaveCriticalSection(&g_lobbyLock);
    if (leave) NetEnd("you left the session");
}

// Loopback test: the stand-in copies your lobby state (ready, picks, lock,
// choices), so every screen can be tried on one PC. It is always player 2.
static void LoopPeerLobby(const uint8_t* p, int n) {
    const NHdr* h = (const NHdr*)p;
    if (h->type == M_LOBBY && n >= (int)sizeof(NLobby)) {
        NLobby r = *(const NLobby*)p;
        r.ack = r.seq;
        snprintf(r.name, sizeof r.name, "loopback stand-in");
        LoopQueue(g_loopIn, &r, sizeof r);
    } else if (h->type == M_START && n >= (int)sizeof(NStart)) {
        const NStart* m = (const NStart*)p;
        NStartAck a{};
        a.h = m->h;
        a.h.type = M_STARTACK;
        a.matchNo = m->s.matchNo;
        a.sum = LbSum(m->s);
        LoopQueue(g_loopIn, &a, sizeof a);
    }
}
