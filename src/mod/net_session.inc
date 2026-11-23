// ===========================================================================
// Step 3c: playing over the network (included into mod.cpp)
// ===========================================================================
// How it works, in short:
//   - A session starts with F4. The two mods exchange a hello (versions,
//     random seed, button layout) and agree who is player 1.
//   - Every "synced" screen (character select, fight, result screens) starts
//     on both PCs at frame 1 with no input. From frame 2 on, each frame needs
//     both players' input for that frame (menus: wait for it; fights, once
//     rollback is running: guess it and fix it later). Loading screens run
//     freely, because their length depends on the PC.
//   - At the start of each synced screen both games set the same random seed
//     and the same button layouts and controller slots.
//   - In fights, every 30 frames both games compare a fingerprint of the two
//     characters' data (desync check).
// Loopback test: the "other player" is a small stand-in inside this process
// that answers like a real one would, through a fake connection with lag and
// lost packets. It copies your inputs as its own (mirror).
// ===========================================================================

enum : uint8_t { M_HELLO = 1, M_INPUT = 2, M_FP = 3, M_RAW = 4, M_PING = 5, M_PONG = 6, M_BYE = 7, M_SURV = 8, M_SPAGE = 9, M_SAVE = 10, M_SAVEACK = 11,
                 M_LOBBY = 12, M_START = 13, M_STARTACK = 14 };   // build 57: lobby.inc
static const uint32_t NET_MAGIC = 0x33425241;   // "ARB3"
static const uint32_t NET_PROTO = 2, NET_BUILD = MOD_BUILD;   // build 122: 2 = per-player input delay (delay in every input packet, clock sync aims for the delay difference)
// Build 57: lobby and match setup (lobby.inc) and the overlay (overlay.inc)
static void LbHandle(const uint8_t* p, int n);
static void LobbyTick();
static void LbReset();
static void LoopPeerLobby(const uint8_t* p, int n);
static void UiSessionStarted();
static char g_uiName[32] = "";          // player name shown to the other PC (set by the overlay)
static const uint32_t FP_BASE = 0x05130CD8;      // the two characters (2 x 0xA520 bytes, fixed address)
static const uint32_t FP_SIZE = 2 * 0xA520;
static const uint32_t FP_BLK = 1024;
static const uint32_t FP_NBLK = (FP_SIZE + FP_BLK - 1) / FP_BLK;   // 83
static const uint32_t FP_EVERY = 30;
static const uint32_t SCENE_BTL = 0x008C9868;
static const uint32_t G_PAD_CFG = 0x04DCAAC8;    // active controller layout, 2 players x 12 dwords
static const uint32_t G_SLOT_MAP = 0x051E3DA8;   // player/slot -> controller table (10 bytes)
static const uint32_t G_SLOT_SWAP = 0x04DC3235;  // swaps slots 0/1 and 8/9 when set

#pragma pack(push, 1)
struct NHdr { uint32_t magic; uint8_t type, flags; uint16_t epoch; };
struct NHello {
    NHdr h;
    uint32_t proto, build, codeHash, seed, nonce;
    uint8_t wantSide, delay, seenYou, pad;
    uint32_t padCfg[12];
    char name[32];
};
// Build 57: ahead = how many quarter frames the sender thinks it runs ahead of the receiver (clock sync)
// Build 122: delay1 = the sender's own input delay for this screen + 1 (0 = not known)
struct NInput { NHdr h; uint32_t sceneVt, first, ack, cur; uint8_t count; int8_t ahead; uint8_t delay1, pad; uint32_t in[32][2]; };
struct NFp { NHdr h; uint32_t frame, seed; uint16_t nblk, pad; uint32_t hash[FP_NBLK]; };
struct NRaw { NHdr h; uint32_t frame; uint16_t blk, len; uint32_t mask[8]; uint8_t data[FP_BLK]; };
struct NPing { NHdr h; uint32_t t; uint32_t us; };   // build 121: us = send time in microseconds (ping log); t as before
// Build 46: memory survey (page hashes of the game's .data, then the pages that differ)
struct NSurv { NHdr h; uint32_t first; uint16_t count, pad; uint32_t hash[256]; };
struct NSPage { NHdr h; uint32_t page; uint16_t part, pad; uint32_t mask[8]; uint8_t data[1024]; };
struct NSave { NHdr h; uint32_t total, crc; uint16_t chunk, nchunks; uint8_t data[1024]; };
struct NSaveAck { NHdr h; uint32_t total, crc; uint16_t have, nchunks; uint32_t bits[4]; };
#pragma pack(pop)

struct NetCfg {
    int mode = 0;                 // 0 loopback, 1 home network, 2 Steam
    int delay = 2;                // input delay in frames
    int wantSide = 0;             // 0 auto, 1, 2
    int rollback = 1;             // use rollback in fights (0 = always wait for input)
    char lanPeer[64] = "auto";
    int lanPort = 7845;
    uint64_t steamFriend = 0;
    int loopLag = 3, loopJitter = 1, loopLoss = 5, loopMirror = 1, loopTestDesync = 0;
    int loopDelay = -1;           // build 122: the stand-in's own input delay (-1 = the same as yours)
    int testLag = 0, testJitter = 0, testLoss = 0;   // build 122: TEST ONLY fake lag on a real connection (home network / Steam), this PC's sending side
    int timeoutMs = 8000;
    int survey = 0;               // build 46: compare memory with the other PC at each screen start
    int syncSave = 0;             // build 48: player 2 uses player 1's saved data while connected (build 128b: off, see LoadNetSettings)
    int clockSync = 1;            // build 57: 0 = old clock sync off (for comparing)
};
static NetCfg g_ncfg;
// Build 128 (security): the raw button bits a controller can have = every mask in the game's
// button table (0xC7FF28, 18 entries: d-pad, ABXY, shoulders, triggers, sticks, Back/Start).
// Inputs from the network keep only these, on both PCs alike (the sender masks its own too).
static uint32_t PadRawMask() {
    static uint32_t mask = 0;
    if (!mask) {
        uint32_t m = 0;
        for (int i = 0; i < 18; i++) m |= reinterpret_cast<const uint32_t*>(0x00C7FF28)[i];
        mask = m ? m : 0xFFFFFFFFu;
        Log("net: controller button bits %08X", mask);
    }
    return mask;
}

struct NetIn { uint32_t frame = RB_NONE; uint32_t v[2] = {0, 0}; };
struct FpLocal {
    uint32_t frame = RB_NONE, seed = 0;
    bool final = false, compared = false;
    uint32_t hash[FP_NBLK];
    uint32_t mask[FP_NBLK][8];
    std::vector<uint8_t> raw;
};
struct FpRemote { uint32_t frame = RB_NONE, seed = 0; uint32_t hash[FP_NBLK]; };

enum NetState { NS_OFF, NS_HELLO, NS_ON };
static struct NetS {
    NetState st = NS_OFF;
    net::Transport* tr = nullptr;
    bool loop = false;
    char peerName[96] = "";
    int side = 0;                       // 0 = we are player 1, 1 = player 2
    uint32_t seed = 0, nonce = 0, codeHash = 0;
    uint32_t localDev = 0;              // the controller the local player uses
    // the other game's hello
    bool gotHello = false;
    NHello rh{};
    // current screen
    uint16_t epoch = 0;                 // synced screens so far
    uint32_t sceneVt = 0;               // screen the input step last saw
    uint32_t scenePtr = 0;
    bool syncedType = false;            // that scene runs in step on both PCs
    bool synced = false;                // ... and the other game is in it too
    bool peerLeft = false;              // build 56: the other game has gone on to the next screen
    bool ready = false;                 // build 52: the game has finished loading this screen here (its own update is due)
    bool autoDone = false;              // rollback start requested for this fight
    // Build 49: input check log for the first part of each fight (assist menu, test 48)
    uint32_t diagN = 0, diagRaw0 = 0xFFFFFFFFu, diagRaw1 = 0xFFFFFFFFu, diagHeld0 = 0xFFFFFFFFu, diagHeld1 = 0xFFFFFFFFu;
    int diagFg = -1;
    bool noTrackRetry = false;          // build 43: check retried with tracking off in this fight
    int trackingWas = -1;               // tracking setting to put back at the next fight
    // the other game
    uint16_t rEpoch = 0;
    uint32_t rSceneVt = 0, rCur = 0, rAck = 1;
    // build 122: input delays for the current screen, locked at its start (a change in My
    // setup counts from the next screen); rDelay = the other player's, 0xFF until their packet says
    uint8_t myDelay = 2, rDelay = 0xFF;
    uint32_t timeStallsAtStart = 0;     // one-frame waits before this screen (log of the first 300 fight frames)
    NetIn rIn[256];
    uint32_t rContig = 2, rMax = 1;     // frames < rContig all known
    NetIn lIn[256];
    uint32_t lNewest = 1;
    // timing
    uint32_t lastRecv = 0, lastPing = 0, lastHello = 0, rtt = 0, startMs = 0;
    uint32_t lastAdjust = 0;
    bool stretching = false;            // build 123: clock sync is lengthening frames now
    uint32_t stretchFrames = 0;         // build 123: frames lengthened (whole session)
    int aheadF = 0;
    int aheadQ = 0;                     // build 57: smoothed frames ahead of the other game x4 (this PC's view)
    int rAheadQ = 0;                    // ... the other game's own view of the same
    int syncQ = 0;                      // both views together: > 0 = this PC runs ahead (x4)
    // input step bookkeeping
    uint32_t injected = 0;              // frame whose input we put in the pads
    bool injectedConsumed = true;
    std::vector<uint8_t> pre;
    // originals to put back when the session ends
    uint32_t origCfg[24];
    uint8_t origMap[10], origSwap = 0;
    bool saved = false;
    // fingerprints
    FpLocal fl[8];
    FpRemote fr[8];
    // stats
    uint32_t stalls = 0, timeStalls = 0, sent = 0, recvd = 0, desyncs = 0, fpOk = 0, fpFalse = 0, srandIgnored = 0;
    uint32_t badPackets = 0, otherPath = 0;
    // build 114: values that already differ at a fight's first check (frame 1, before anyone
    // pressed anything) are left over from loading on each PC, not from play. They stay the
    // same all fight, so they are left out of the check for the rest of that fight.
    uint32_t fpIgn[(FP_SIZE / 4 + 31) / 32] = {};
    uint32_t fpNoise = 0;
    uint32_t waited = 0, waitMs = 0, lastStallFrame = 0, seenDesyncs = 0;
    // build 111: switched away from a full-screen game (the game stops while it has no screen)
    volatile uint32_t peerAwaySince = 0;   // the other player's "I'm away" pings started (0 = not away)
    volatile uint32_t gameTick = 0;          // last time the game thread looked at the network
    char lastDesync[96] = "";
} N;

static char g_netStatus[200] = "";
static uint64_t g_netPick = 0;          // friend picked with F5
static char g_netPickName[64] = "";
// Build 112: a match from a room table (roommatch.inc): who and which side come from the
// seats, not from rollback.ini; the one-PC test player means the loopback test.
static struct RoomMatchCfg { bool on = false, loop = false; uint64_t peer = 0; int side = 0; int setLen = 0; } g_roomCfg;   // setLen: build 113, 0 = no limit
enum RmState : int { RM_IDLE = 0, RM_STARTING, RM_PLAYING };
static struct RoomMatchView {             // read by the room screen (roomui.inc)
    std::atomic<int> st{RM_IDLE};
    std::atomic<uint32_t> t0{0};          // GetTickCount when both were READY
    std::atomic<bool> connected{false};
} RMV;

static void NetStatus(const char* fmt, ...) {
    char buf[200];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_statusLock);
    strcpy(g_netStatus, buf);
    LeaveCriticalSection(&g_statusLock);
}

static void LoadNetSettings() {
    wchar_t ini[MAX_PATH];
    _snwprintf(ini, MAX_PATH, L"%lsrollback.ini", util::ModDir());
    wchar_t w[64];
    GetPrivateProfileStringW(L"net", L"mode", L"steam", w, 64, ini);   // build 106: Steam unless the ini says otherwise
    g_ncfg.mode = _wcsicmp(w, L"lan") == 0 ? 1 : _wcsicmp(w, L"steam") == 0 ? 2 : 0;
    g_ncfg.delay = GetPrivateProfileIntW(L"net", L"input_delay", 2, ini);
    g_ncfg.wantSide = GetPrivateProfileIntW(L"net", L"side", 0, ini);
    g_ncfg.rollback = GetPrivateProfileIntW(L"net", L"rollback", 1, ini);
    GetPrivateProfileStringW(L"net", L"lan_peer", L"auto", w, 64, ini);
    snprintf(g_ncfg.lanPeer, sizeof g_ncfg.lanPeer, "%ls", w);
    g_ncfg.lanPort = GetPrivateProfileIntW(L"net", L"lan_port", 7845, ini);
    GetPrivateProfileStringW(L"net", L"steam_friend", L"", w, 64, ini);
    g_ncfg.steamFriend = _wcstoui64(w, nullptr, 10);
    g_ncfg.loopLag = GetPrivateProfileIntW(L"net", L"loopback_lag_frames", 3, ini);
    g_ncfg.loopDelay = GetPrivateProfileIntW(L"net", L"loopback_delay", -1, ini);
    if (g_ncfg.loopDelay > 8) g_ncfg.loopDelay = 8;
    g_ncfg.loopJitter = GetPrivateProfileIntW(L"net", L"loopback_jitter_frames", 1, ini);
    g_ncfg.loopLoss = GetPrivateProfileIntW(L"net", L"loopback_loss_percent", 5, ini);
    GetPrivateProfileStringW(L"net", L"loopback_other_player", L"mirror", w, 64, ini);
    g_ncfg.loopMirror = _wcsicmp(w, L"idle") != 0;
    g_ncfg.loopTestDesync = GetPrivateProfileIntW(L"net", L"loopback_test_desync_frame", 0, ini);
    g_ncfg.testLag = GetPrivateProfileIntW(L"net", L"test_lag_frames", 0, ini);
    g_ncfg.testJitter = GetPrivateProfileIntW(L"net", L"test_jitter_frames", 0, ini);
    g_ncfg.testLoss = GetPrivateProfileIntW(L"net", L"test_loss_percent", 0, ini);
    if (g_ncfg.testLag < 0) g_ncfg.testLag = 0;
    if (g_ncfg.testLag > 30) g_ncfg.testLag = 30;
    if (g_ncfg.testJitter < 0) g_ncfg.testJitter = 0;
    if (g_ncfg.testJitter > 10) g_ncfg.testJitter = 10;
    if (g_ncfg.testLoss < 0) g_ncfg.testLoss = 0;
    if (g_ncfg.testLoss > 50) g_ncfg.testLoss = 50;
    if (g_ncfg.testLag || g_ncfg.testJitter || g_ncfg.testLoss)
        Log("net settings: TEST fake lag on real connections: packets this PC sends wait %d frames (+0..%d), %d%% are dropped",
            g_ncfg.testLag, g_ncfg.testJitter, g_ncfg.testLoss);
    g_ncfg.timeoutMs = GetPrivateProfileIntW(L"net", L"timeout_ms", 8000, ini);
    g_ncfg.survey = GetPrivateProfileIntW(L"net", L"memory_survey", 0, ini);   // build 49: off unless asked for (it stops the game for seconds)
    // build 128b (security): off. Player 2 copied player 1's save sections raw into the game, so
    // a hostile player 1 could put anything there. Since build 61 our own setup sets the fight's
    // characters, colours, Knights, stage, rounds and timer, which is what the sync was for.
    // same_save_data=1 brings it back, for comparison tests only.
    g_ncfg.syncSave = GetPrivateProfileIntW(L"net", L"same_save_data", 0, ini);
    g_ncfg.clockSync = GetPrivateProfileIntW(L"net", L"clock_sync", 1, ini);
    if (g_ncfg.delay < 0) g_ncfg.delay = 0;
    if (g_ncfg.delay > 8) g_ncfg.delay = 8;
    if (g_ncfg.wantSide != 1 && g_ncfg.wantSide != 2) g_ncfg.wantSide = 0;
    if (g_ncfg.loopLag < 0) g_ncfg.loopLag = 0;
    if (g_ncfg.loopLag > 30) g_ncfg.loopLag = 30;
    if (g_ncfg.loopJitter < 0) g_ncfg.loopJitter = 0;
    if (g_ncfg.timeoutMs < 3000) g_ncfg.timeoutMs = 3000;
    static const char* modes[] = {"loopback test", "home network", "Steam"};
    Log("net settings: mode=%s input_delay=%d side=%d rollback=%d lan_peer=%s lan_port=%d steam_friend=%llu",
        modes[g_ncfg.mode], g_ncfg.delay, g_ncfg.wantSide, g_ncfg.rollback, g_ncfg.lanPeer, g_ncfg.lanPort,
        (unsigned long long)g_ncfg.steamFriend);
    if (g_ncfg.mode == 0)
        Log("  loopback: lag %d frames (+0..%d), %d%% lost packets, other player %s, test desync at frame %d",
            g_ncfg.loopLag, g_ncfg.loopJitter, g_ncfg.loopLoss, g_ncfg.loopMirror ? "copies your input" : "idle",
            g_ncfg.loopTestDesync);
}

static bool NetOn() { return N.st == NS_ON; }
static int NetRemoteSide() { return 1 - N.side; }
static void NetSavePre(const std::vector<uint8_t>& pre) { N.pre = pre; }
static inline bool NetRbMode() { return g_mode == Mode::SyncTest && g_rbOn && g_rbStarted; }
// Build 121 (issue: freeze at fight start with input delay 0 on both PCs): frames that run
// in step (no rollback yet, the rollback-off rematch box, rollback turned off) wait for the
// other player's input of a frame BEFORE this PC makes its own input of that frame, so with
// delay 0 both games waited for each other forever. In step, the delay is at least 1.
static uint32_t ScreenDelay();
static uint32_t InputDelayNow() {
    uint32_t d = ScreenDelay();
    return d == 0 && !NetRbMode() ? 1u : d;
}
// Menus wait for every input, so they get a bit more input delay (smoother
// with lag, and nobody notices 4 frames in a menu). Fights use input_delay.
static uint32_t ScreenDelay() {
    uint32_t d = N.myDelay;              // build 122: this player's own delay, locked for the screen
    if (N.sceneVt == SCENE_BTL) return d;
    return d > 4 ? d : 4u;
}
// Build 122 (per-player delay): "your own delay protects your own screen". Your rollbacks are
// the connection minus YOUR delay, so the game with the lower delay runs ahead of the other by
// the difference. This is how many frames this PC aims to run ahead (negative = behind), in
// quarter frames. Fights only (menus and the loading screen run in step).
static int ClockTargetQ() {
    if (N.sceneVt != SCENE_BTL || N.rDelay > 8) return 0;
    return ((int)N.rDelay - (int)N.myDelay) * 4;
}
// The top scene is "game main" the whole time; the real screen (title,
// character select, fight, ...) is its child object at +0x28 (test 39).
static const uint32_t VT_GAMEMAIN = 0x008D24D4;
static void CurScreen(uint32_t* vtOut, uint32_t* ptrOut) {
    *vtOut = *ptrOut = 0;
    uint32_t scene = *reinterpret_cast<uint32_t*>(addr::G_MAIN_SCENE);
    if (!scene) return;
    uint32_t vt = *reinterpret_cast<uint32_t*>(scene);
    if (vt != VT_GAMEMAIN) { *vtOut = vt; *ptrOut = scene; return; }
    uint32_t child = *reinterpret_cast<uint32_t*>(scene + 0x28);
    if (!child) return;
    *vtOut = *reinterpret_cast<uint32_t*>(child);
    *ptrOut = child;
}
static bool IsSyncedScene(uint32_t vt) {
    // The loading screen runs the fight's own pre-fight part, including the assist
    // set menu (CMLLoading's update -> 0x4360C0 -> 0x431CA0 -> fight update 0x432F30
    // -> 0x610880), so it runs in step like the fight. Build 66: the game's own menus
    // (character select, winner screen ...) are no longer used online, so only these two.
    return vt == 0x008D2604 /*Loading*/ || vt == SCENE_BTL;
}
static bool NetIgnoreSrand() {
    if (NetOn() && N.syncedType) { N.srandIgnored++; return true; }
    return false;
}

// ---- Loopback test: a stand-in for the other game -------------------------
struct LPkt { uint32_t at; uint16_t n; uint8_t d[1100]; };
static std::vector<LPkt> g_loopOut, g_loopIn;
static uint32_t g_loopRng = 0xC0FFEE;
static uint32_t LoopRand() { g_loopRng = g_loopRng * 1103515245u + 12345u; return (g_loopRng >> 16) & 0x7FFF; }
static bool g_loopMute = false;   // build 108 test (see LoopRecv)
static void LoopQueue(std::vector<LPkt>& q, const void* p, int n) {
    if (n > 1100) return;
    if (g_ncfg.loopLoss > 0 && (int)(LoopRand() % 100) < g_ncfg.loopLoss) return;
    LPkt k;
    uint32_t frames = (uint32_t)g_ncfg.loopLag + (g_ncfg.loopJitter ? LoopRand() % (uint32_t)(g_ncfg.loopJitter + 1) : 0);
    k.at = GetTickCount() + frames * 50 / 3;     // frames of 1/60 s
    k.n = (uint16_t)n;
    memcpy(k.d, p, n);
    q.push_back(k);
}
// It plays like a second game that runs at the same time as yours: its
// player's input for a frame is decided when that game would sample it
// (its frame minus the input delay), and copies the newest input of yours
// it has seen by then (or nothing, with loopback_other_player=idle).
// Build 122: the stand-in's own input delay (loopback_delay=, -1 = yours), menus at least 4 like ours
static uint32_t LoopFightDelay() { return g_ncfg.loopDelay < 0 ? N.myDelay : (uint32_t)g_ncfg.loopDelay; }
static uint32_t LoopScreenDelay() {
    uint32_t d = LoopFightDelay();
    if (N.sceneVt == SCENE_BTL) return d;
    return d > 4 ? d : 4u;
}
static struct LoopPeer {
    uint16_t epoch = 0xFFFF;
    NetIn in[256];          // the frames it got from us
    uint32_t contig = 2;
    NetIn own[256];         // its own player's inputs
    uint32_t ownNewest = 1;
    uint32_t last[2] = {0, 0};
} g_lp;
static void LoopPeerHandle(const uint8_t* p, int n) {
    if (n < (int)sizeof(NHdr)) return;
    const NHdr* h = (const NHdr*)p;
    switch (h->type) {
    case M_HELLO: {
        NHello r = *(const NHello*)p;
        r.nonce = ((const NHello*)p)->nonce - 1;     // so you are player 1
        r.seed ^= 0x1234567u;
        r.wantSide = r.wantSide == 1 ? 2 : r.wantSide == 2 ? 1 : 0;
        r.seenYou = 1;
        snprintf(r.name, sizeof r.name, "loopback stand-in");
        LoopQueue(g_loopIn, &r, sizeof r);
        break;
    }
    case M_INPUT: {
        const NInput* m = (const NInput*)p;
        if (m->h.epoch != g_lp.epoch) {
            g_lp.epoch = m->h.epoch;
            for (auto& x : g_lp.in) x = NetIn();
            for (auto& x : g_lp.own) x = NetIn();
            g_lp.contig = 2;
            g_lp.ownNewest = 1;
            g_lp.last[0] = g_lp.last[1] = 0;
            for (uint32_t f = 2; f <= LoopScreenDelay(); f++) { g_lp.own[f].frame = f; g_lp.ownNewest = f; }
        }
        for (uint32_t i = 0; i < m->count; i++) {
            uint32_t f = m->first + i;
            NetIn& x = g_lp.in[f % 256];
            x.frame = f;
            x.v[0] = m->in[i][0];
            x.v[1] = m->in[i][1];
        }
        while (g_lp.in[g_lp.contig % 256].frame == g_lp.contig) {
            g_lp.last[0] = g_lp.in[g_lp.contig % 256].v[0];
            g_lp.last[1] = g_lp.in[g_lp.contig % 256].v[1];
            g_lp.contig++;
        }
        // where the stand-in's game would be now; build 122: in fights a stand-in with the higher
        // delay runs behind by the difference (and one with the lower delay ahead), like a real PC
        uint32_t sd = LoopScreenDelay();
        int offF = N.sceneVt == SCENE_BTL ? (int)sd - (int)N.myDelay : 0;
        int pf = (int)m->cur + g_ncfg.loopLag - offF;
        uint32_t peerFrame = pf < 1 ? 1u : (uint32_t)pf;
        if (m->sceneVt) {
            uint32_t target = peerFrame + (sd == 0 && !NetRbMode() ? 1u : sd);
            if (target > g_lp.ownNewest + 200) target = g_lp.ownNewest + 200;
            for (uint32_t f = g_lp.ownNewest + 1; f <= target; f++) {
                NetIn& o = g_lp.own[f % 256];
                o.frame = f;
                o.v[0] = g_ncfg.loopMirror ? g_lp.last[0] : 0;
                o.v[1] = g_ncfg.loopMirror ? g_lp.last[1] : 0;
                g_lp.ownNewest = f;
            }
        }
        NInput r{};
        r.h = m->h;
        r.sceneVt = m->sceneVt;
        r.ack = g_lp.contig - 1;
        r.cur = peerFrame;
        r.delay1 = (uint8_t)(LoopFightDelay() + 1);
        r.first = m->ack + 1 < 2 ? 2 : m->ack + 1;
        uint32_t cnt = 0;
        while (m->sceneVt && cnt < 32 && r.first + cnt <= g_lp.ownNewest) {
            const NetIn& x = g_lp.own[(r.first + cnt) % 256];
            r.in[cnt][0] = x.v[0];
            r.in[cnt][1] = x.v[1];
            cnt++;
        }
        r.count = (uint8_t)cnt;
        LoopQueue(g_loopIn, &r, (int)(offsetof(NInput, in) + cnt * 8));
        break;
    }
    case M_FP: {
        NFp r = *(const NFp*)p;
        if (g_ncfg.loopTestDesync > 0 && r.frame >= (uint32_t)g_ncfg.loopTestDesync) r.hash[5] ^= 1;
        LoopQueue(g_loopIn, &r, n);
        break;
    }
    case M_RAW: {
        NRaw r = *(const NRaw*)p;
        if (g_ncfg.loopTestDesync > 0 && r.frame >= (uint32_t)g_ncfg.loopTestDesync && r.blk == 5) {
            for (int i = 0; i < 256; i++)        // change the first dword neither side calls a pointer
                if (!(r.mask[i / 32] & (1u << (i % 32)))) { r.data[i * 4] ^= 1; break; }
        }
        LoopQueue(g_loopIn, &r, n);
        break;
    }
    case M_PING: {
        NPing r = *(const NPing*)p;
        r.h.type = M_PONG;
        LoopQueue(g_loopIn, &r, sizeof r);
        break;
    }
    case 15: {   // build 121: M_RCHK (desynclog.inc): the stand-in has the same random numbers (loopback_test_desync: not from that frame on)
        uint8_t b[512];
        if (n > (int)sizeof b) break;
        memcpy(b, p, n);
        uint32_t first; memcpy(&first, b + 8, 4);
        uint8_t cnt = b[12];
        for (uint32_t i = 0; i < cnt && i < 16; i++)
            if (g_ncfg.loopTestDesync > 0 && first + i >= (uint32_t)g_ncfg.loopTestDesync) b[16 + i * 4] ^= 1;
        LoopQueue(g_loopIn, b, n);
        break;
    }
    case 16: LoopQueue(g_loopIn, p, n); break;   // M_SNAP
    case 17: {                                    // M_SNAPH: with loopback_test_desync, chunk 3 of each list differs
        uint8_t b[1100];
        if (n > (int)sizeof b) break;
        memcpy(b, p, n);
        uint32_t first; memcpy(&first, b + 16, 4);
        if (g_ncfg.loopTestDesync > 0 && first == 0 && n >= 20 + 16) b[20 + 3 * 4] ^= 1;
        LoopQueue(g_loopIn, b, n);
        break;
    }
    default: LoopPeerLobby(p, n); break;
    }
}
static void LoopTick() {
    for (size_t i = 0; i < g_loopOut.size();) {
        if ((int32_t)(GetTickCount() - g_loopOut[i].at) >= 0) {
            LPkt k = g_loopOut[i];
            g_loopOut.erase(g_loopOut.begin() + i);
            LoopPeerHandle(k.d, k.n);
        } else i++;
    }
}
static int LoopRecv(void* buf, int cap) {
    // build 108 test: in the loopback test, holding Insert (game window in front) = the other
    // player's packets stop coming (hiccup banner, then the 8 s time-out)
    if (g_loopMute) { g_loopIn.clear(); return -1; }
    for (size_t i = 0; i < g_loopIn.size(); i++) {
        if ((int32_t)(GetTickCount() - g_loopIn[i].at) < 0) continue;
        int n = g_loopIn[i].n;
        if (n > cap) n = cap;
        memcpy(buf, g_loopIn[i].d, n);
        g_loopIn.erase(g_loopIn.begin() + i);
        return n;
    }
    return -1;
}

// Build 122: TEST ONLY (test_lag_frames / test_jitter_frames / test_loss_percent in rollback.ini,
// all 0 normally): this PC's packets wait before they really go out, to test the clock sync with
// two real PCs on a home network. Packets keep their order; NetSend is also used by the away thread.
static std::vector<LPkt> g_testOut;
static CRITICAL_SECTION g_testLock;
static bool g_testLockInit = false;
static void TestFlush() {
    if (!N.tr || g_testOut.empty()) return;
    EnterCriticalSection(&g_testLock);
    size_t i = 0;
    uint32_t now = GetTickCount();
    while (i < g_testOut.size() && (int32_t)(now - g_testOut[i].at) >= 0) { N.tr->Send(g_testOut[i].d, g_testOut[i].n); i++; }
    if (i) g_testOut.erase(g_testOut.begin(), g_testOut.begin() + i);
    LeaveCriticalSection(&g_testLock);
}
static void NetSend(const void* p, int n) {
    N.sent++;
    if (N.loop) LoopQueue(g_loopOut, p, n);
    else if (N.tr && (g_ncfg.testLag || g_ncfg.testJitter || g_ncfg.testLoss) && n <= 1100) {
        if (!g_testLockInit) { InitializeCriticalSection(&g_testLock); g_testLockInit = true; }
        EnterCriticalSection(&g_testLock);
        bool drop = g_ncfg.testLoss > 0 && (int)(LoopRand() % 100) < g_ncfg.testLoss;
        if (!drop) {
            LPkt k;
            uint32_t frames = (uint32_t)g_ncfg.testLag + (g_ncfg.testJitter ? LoopRand() % (uint32_t)(g_ncfg.testJitter + 1) : 0);
            k.at = GetTickCount() + frames * 50 / 3;
            if (!g_testOut.empty() && (int32_t)(k.at - g_testOut.back().at) < 0) k.at = g_testOut.back().at;   // keep the order
            k.n = (uint16_t)n;
            memcpy(k.d, p, n);
            g_testOut.push_back(k);
        }
        LeaveCriticalSection(&g_testLock);
        TestFlush();
    }
    else if (N.tr) N.tr->Send(p, n);
}
static int NetRecvRaw(void* buf, int cap) {
    if (N.loop) return LoopRecv(buf, cap);
    TestFlush();
    return N.tr ? N.tr->Recv(buf, cap) : -1;
}
static NHdr MakeHdr(uint8_t type) { NHdr h; h.magic = NET_MAGIC; h.type = type; h.flags = 0; h.epoch = N.epoch; return h; }

static uint32_t g_codeHash = 0;      // game code before our patches (same on every PC)
static uint32_t CodeHash() {
    uint32_t h = 0x811C9DC5u;
    for (uint32_t a = 0x00401000; a < 0x0065C000; a += 4) h = (h ^ *(const uint32_t*)a) * 0x01000193u;
    return h;
}
static void SendHello() {
    NHello m{};
    m.h = MakeHdr(M_HELLO);
    m.proto = NET_PROTO;
    m.build = NET_BUILD;
    m.codeHash = N.codeHash;
    m.seed = N.seed;
    m.nonce = N.nonce;
    m.wantSide = (uint8_t)g_ncfg.wantSide;
    m.delay = (uint8_t)g_ncfg.delay;
    m.seenYou = N.gotHello ? 1 : 0;
    memcpy(m.padCfg, N.saved ? (const void*)N.origCfg : (const void*)G_PAD_CFG, 48);   // this PC's player 1 layout
    snprintf(m.name, sizeof m.name, "%s", g_uiName[0] ? g_uiName : "player");
    NetSend(&m, sizeof m);
    N.lastHello = GetTickCount();
}

// ---- Fingerprints (desync check) ------------------------------------------
// Pointers into the heap or other DLLs differ between PCs even when the
// fight is the same, so each PC hides values that look like such a pointer
// on its own PC. When a block still differs, both PCs send that block in
// full and compare it with what either PC called a pointer left out.
// 64 KB pieces of the address space holding heap / DLL memory. Build 77: built on a
// worker thread into the spare buffer, then swapped in (test 76b: walking the
// address space took 23 ms on the laptop every 600 frames, inside a frame).
static uint8_t g_ptrMapBuf[2][65536 / 8];
static std::atomic<uint8_t*> g_ptrMapCur{g_ptrMapBuf[0]};
static std::atomic<bool> g_ptrMapBusy{false};
static uint32_t g_exeLo = 0, g_exeHi = 0;
static uint32_t g_ptrMapAt = 0;
static void BuildPtrMapInto(uint8_t* m) {
    memset(m, 0, 65536 / 8);
    uint64_t a = 0x10000;
    MEMORY_BASIC_INFORMATION mi;
    while (a < 0xFFFF0000ull && VirtualQuery((LPCVOID)(uintptr_t)a, &mi, sizeof mi)) {
        uint64_t lo = (uintptr_t)mi.BaseAddress, hi = lo + mi.RegionSize;
        if (hi <= a) break;
        if (mi.State == MEM_COMMIT && !(lo >= g_exeLo && hi <= g_exeHi))
            for (uint64_t g = lo >> 16; g <= (hi - 1) >> 16 && g < 65536; g++) m[g >> 3] |= (uint8_t)(1u << (g & 7));
        a = hi;
    }
}
// One worker thread for the whole run (made the first time, outside a fight frame
// where possible), woken by an event; no thread is created during fights.
static HANDLE g_ptrMapEv = nullptr;
static DWORD WINAPI PtrMapThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_ptrMapEv, INFINITE);
        uint8_t* cur = g_ptrMapCur.load();
        uint8_t* spare = cur == g_ptrMapBuf[0] ? g_ptrMapBuf[1] : g_ptrMapBuf[0];
        BuildPtrMapInto(spare);
        g_ptrMapCur.store(spare);
        g_ptrMapBusy.store(false);
    }
    return 0;
}
static void BuildPtrMap(bool now = false) {
    if (g_exeHi == g_exeLo) {
        auto* dos = (IMAGE_DOS_HEADER*)GetModuleHandleW(nullptr);
        auto* nth = (IMAGE_NT_HEADERS*)((uint8_t*)dos + dos->e_lfanew);
        g_exeLo = (uint32_t)dos;
        g_exeHi = g_exeLo + nth->OptionalHeader.SizeOfImage;
    }
    g_ptrMapAt = g_tick;
    if (!g_ptrMapEv) {
        g_ptrMapEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = g_ptrMapEv ? CreateThread(nullptr, 64 << 10, PtrMapThread, nullptr, 0, nullptr) : nullptr;
        if (t) CloseHandle(t); else now = true;
    }
    if (now || !g_ptrMapEv) {
        if (g_ptrMapBusy.load()) return;
        uint8_t* cur = g_ptrMapCur.load();
        uint8_t* spare = cur == g_ptrMapBuf[0] ? g_ptrMapBuf[1] : g_ptrMapBuf[0];
        BuildPtrMapInto(spare);
        g_ptrMapCur.store(spare);
        return;
    }
    bool exp = false;
    if (!g_ptrMapBusy.compare_exchange_strong(exp, true)) return;
    SetEvent(g_ptrMapEv);
}
// Build 119 (test with Ditn's friend session, 2026-11-17): an in-place rematch restarts the
// fight about 50 frames into its synced screen, after the map above was made for frame 1. The
// restart puts the characters' data in newly committed memory, so for up to 10 s their
// pointers looked like values and counted as desyncs. Now the map is made again (on this
// thread, at once) right when the fight restarts, and once more before any difference counts.
static bool g_ptrMapStale = false;
static uint32_t g_ptrMapRecheckFrame = 0xFFFFFFFFu;
static void BuildPtrMapNow() {
    for (int i = 0; i < 100 && g_ptrMapBusy.load(); i++) Sleep(1);   // the worker is making one: let it finish
    BuildPtrMap(true);
}
static inline bool LooksLikePtr(uint32_t v) {
    if (v < 0x10000) return false;
    if (v >= g_exeLo && v < g_exeHi) return false;      // the game's own image: same address on every PC
    uint32_t g = v >> 16;
    return (g_ptrMapCur.load(std::memory_order_relaxed)[g >> 3] >> (g & 7)) & 1;
}
static void NetFpCapture(uint32_t k) {
    if (!NetOn() || !N.synced || N.sceneVt != SCENE_BTL || k % FP_EVERY != 1) return;
    if (g_exeHi == g_exeLo || k == 1) BuildPtrMap(true);   // first time, and fresh for each fight's first check (build 81: 80a false alarm at frame 1)
    else if (g_ptrMapStale) { BuildPtrMapNow(); Log("net: fight restarted in place: memory map made again (frame %u)", k); }
    else if (g_tick - g_ptrMapAt > 600) BuildPtrMap();
    g_ptrMapStale = false;
    FpLocal& L = N.fl[(k / FP_EVERY) % 8];
    L.frame = k;
    L.seed = t_seed;
    L.final = L.compared = false;
    L.raw.resize(FP_SIZE);
    memcpy(L.raw.data(), (const void*)FP_BASE, FP_SIZE);
    if (k == 1) { memset(N.fpIgn, 0, sizeof N.fpIgn); N.fpNoise = 0; }   // a new fight
    for (uint32_t b = 0; b < FP_NBLK; b++) {
        uint32_t lo = b * FP_BLK, hi = lo + FP_BLK > FP_SIZE ? FP_SIZE : lo + FP_BLK;
        uint32_t h = 0x811C9DC5u ^ b;
        memset(L.mask[b], 0, 32);
        for (uint32_t o = lo, i = 0; o + 4 <= hi; o += 4, i++) {
            uint32_t v;
            memcpy(&v, &L.raw[o], 4);
            if (LooksLikePtr(v)) { L.mask[b][i / 32] |= 1u << (i % 32); v = 0x9A11E7u; }
            else if ((N.fpIgn[(o / 4) / 32] >> ((o / 4) % 32)) & 1) v = 0x1D1E5u;
            h = (h ^ v) * 0x01000193u;
        }
        L.hash[b] = h;
    }
}
static void NetDesync(uint32_t frame, const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    N.desyncs++;
    snprintf(N.lastDesync, sizeof N.lastDesync, "frame %u", frame);
    Log("net: DESYNC in the fight (screen %u, frame %u): %s", N.epoch, frame, buf);
}
static void SendRaw(const FpLocal& L, uint32_t b) {
    NRaw m{};
    m.h = MakeHdr(M_RAW);
    m.frame = L.frame;
    m.blk = (uint16_t)b;
    uint32_t lo = b * FP_BLK, hi = lo + FP_BLK > FP_SIZE ? FP_SIZE : lo + FP_BLK;
    m.len = (uint16_t)(hi - lo);
    memcpy(m.mask, L.mask[b], 32);
    memcpy(m.data, &L.raw[lo], hi - lo);
    NetSend(&m, (int)(offsetof(NRaw, data) + m.len));
}
static void FpCompare(FpLocal& L) {
    FpRemote& R = N.fr[(L.frame / FP_EVERY) % 8];
    if (!L.final || L.compared || R.frame != L.frame) return;
    L.compared = true;
    bool seedBad = L.seed != R.seed;
    if (seedBad) {
        NetDesync(L.frame, "random number state differs (this PC %08X, other PC %08X)", L.seed, R.seed);
        if (N.desyncs > 10) return;    // build 77 (build 121: 10 instead of 3): the first times, also compare the memory to see what else differs
        // build 121: the rand trace is written by the per-frame check (RchkCompare), around the exact frame
    }
    uint32_t bad = 0;
    for (uint32_t b = 0; b < FP_NBLK; b++) {
        if (L.hash[b] == R.hash[b]) continue;
        if (bad < (L.frame == 1 ? FP_NBLK : 6)) SendRaw(L, b);
        bad++;
    }
    if (!bad) { if (!seedBad) N.fpOk++; }
    else if (N.fpFalse + N.desyncs < 30) Log("net: fingerprint of frame %u: %u of %u blocks differ, comparing them in full", L.frame, bad, FP_NBLK);
}
static void SnapPropose(const char* why);   // build 121 (desynclog.inc)
static uint32_t g_dlFight = 0;              // build 121: fights started this session (a new one also after an in-place rematch)
static void NetHandleRaw(const NRaw* m, int n) {
    if (n < (int)offsetof(NRaw, data) || m->blk >= FP_NBLK) return;
    FpLocal& L = N.fl[(m->frame / FP_EVERY) % 8];
    if (L.frame != m->frame || L.raw.size() != FP_SIZE) return;
    uint32_t lo = m->blk * FP_BLK, hi = lo + FP_BLK > FP_SIZE ? FP_SIZE : lo + FP_BLK;
    if (m->len != hi - lo || n < (int)(offsetof(NRaw, data) + m->len)) return;
    uint32_t diffs = 0, ptrs = 0;
    char detail[200] = "";
    // build 119: values that differ on a later frame are checked again against a fresh memory
    // map (made once per checked frame): a pointer into memory committed since the last map is
    // this PC's own address, not a desync. Each PC leaves out its own pointers this way.
    bool fresh = false;
    struct DiffV { uint32_t o, a, b; };
    std::vector<DiffV> all;
    for (int pass = 0; pass < 2; pass++) {
        diffs = ptrs = 0;
        detail[0] = 0;
        all.clear();
        for (uint32_t o = lo, i = 0; o + 4 <= hi; o += 4, i++) {
            uint32_t bit = 1u << (i % 32);
            if ((L.mask[m->blk][i / 32] | m->mask[i / 32]) & bit) continue;
            uint32_t w = o / 4;
            if ((N.fpIgn[w / 32] >> (w % 32)) & 1) continue;
            uint32_t a, b;
            memcpy(&a, &L.raw[o], 4);
            memcpy(&b, &m->data[o - lo], 4);
            if (a == b) continue;
            if (m->frame == 1) { N.fpIgn[w / 32] |= 1u << (w % 32); N.fpNoise++; diffs++; continue; }
            if (fresh && LooksLikePtr(a)) { ptrs++; continue; }
            if (diffs < 4 && N.desyncs < 10) {
                size_t len = strlen(detail);
                snprintf(detail + len, sizeof detail - len, " P%u+0x%X: %08X vs %08X;", o / 0xA520 + 1, o % 0xA520, a, b);
            }
            if (all.size() < 256) all.push_back({o, a, b});   // build 121: every value, for the first desync of a screen
            diffs++;
        }
        if (!diffs || m->frame == 1 || fresh) break;
        // build 121: the fresh map (a ~10 ms walk of the address space) only when a differing
        // value could be a pointer at all; after a real desync it ran every 30 frames (stutter)
        bool maybePtr = false;
        for (auto& d : all) if (d.a >= 0x10000 && !(d.a & 3) && d.a < 0xFFFF0000u) { maybePtr = true; break; }
        if (!maybePtr && diffs <= all.size()) break;
        // and at most once per 300 frames (a map from a few seconds ago still knows nearly all memory)
        if (g_ptrMapRecheckFrame != m->frame) {
            if (g_ptrMapRecheckFrame != 0xFFFFFFFFu && m->frame > g_ptrMapRecheckFrame && m->frame - g_ptrMapRecheckFrame < 300) {
                // reuse the map of that frame
            } else { g_ptrMapRecheckFrame = m->frame; BuildPtrMapNow(); }
        }
        fresh = true;
    }
    if (ptrs && N.fpFalse + N.desyncs < 40)
        Log("net:   frame %u block %u: %u values are pointers into memory the map did not have yet (fine)", m->frame, m->blk, ptrs);
    if (diffs && m->frame == 1) {   // left over from loading: not a desync (see fpIgn)
        Log("net:   frame 1 block %u: %u values differ from the start (left over from loading on each PC, not from play); left out for this fight", m->blk, diffs);
        return;
    }
    if (!diffs) {
        N.fpFalse++;
        if (N.fpFalse <= 20) Log("net:   frame %u block %u: same once pointers are left out (fine)", m->frame, m->blk);
        return;
    }
    NetDesync(m->frame, "%u values of the character data differ in block %u%s%s", diffs, m->blk, detail[0] ? ":" : "", detail);
    // build 121: the first character desync of each screen: every differing value of the block
    // (the 18-19 Nov logs had only the first 4, and none after 10 desyncs), then the memory snapshot
    static uint32_t fullFight = 0xFFFFFFFFu;
    static uint32_t fullBlocks = 0;
    if (fullFight != g_dlFight) { fullFight = g_dlFight; fullBlocks = 0; }
    if (fullBlocks++ < 4) {
        Log("net:   all differing values of frame %u block %u (this PC vs other PC):", m->frame, m->blk);
        for (size_t i = 0; i < all.size(); i += 6) {
            char buf[400]; int k = 0;
            for (size_t j = i; j < all.size() && j < i + 6; j++)
                k += snprintf(buf + k, sizeof buf - k, " P%u+0x%X: %08X vs %08X;", all[j].o / 0xA520 + 1, all[j].o % 0xA520, all[j].a, all[j].b);
            Log("net:    %s", buf);
        }
        if (diffs > all.size()) Log("net:     (%u more)", diffs - (unsigned)all.size());
    }
    SnapPropose("character data");
}
static void NetFpFinalize() {
    if (!NetOn()) return;
    bool rb = NetRbMode();
    for (auto& L : N.fl) {
        if (L.frame == RB_NONE || L.final) continue;
        if (rb && g_rbConf < L.frame) continue;      // an input before it is still a guess
        L.final = true;
        NFp m{};
        m.h = MakeHdr(M_FP);
        m.frame = L.frame;
        m.seed = L.seed;
        m.nblk = (uint16_t)FP_NBLK;
        memcpy(m.hash, L.hash, sizeof m.hash);
        NetSend(&m, sizeof m);
        FpCompare(L);
    }
}


static void NetPump();
static void NetEnd(const char* why);
struct SvPacer {
    LARGE_INTEGER f, t0;
    uint64_t bytes = 0;
    SvPacer() { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    void Sent(uint32_t n) {
        bytes += n;
        for (;;) {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            double sec = double(t.QuadPart - t0.QuadPart) / double(f.QuadPart);
            if (double(bytes) <= sec * 3.0e6) return;
            NetPump();
            SwitchToThread();
        }
    }
};

// ---- Build 48: same saved data on both PCs ------------------------------------
// Test 47: each game started the screens from its own SaveData.dat (cursor
// memory, unlocks, option values, play records), so the fights went apart
// from frame 1. The game keeps its save as 7 sections listed in a table at
// 0x051E3BD8 (pointer, size, handler); SaveData.dat is a copy of them. On
// connect player 1 sends its GS_INFO, RANKING, OPTION and GALLERY sections,
// player 2 keeps its own copy, uses player 1's while connected, and puts its
// own back when the session ends. While player 2 holds player 1's data the
// game is not allowed to write SaveData.dat on player 2's PC (0x517E40), so
// player 2's save file is never touched. Controller settings (KEY, KEYBOARD)
// already have their own handling; DEMO is left alone.
static const uint32_t SS_REG = 0x051E3BD8;          // 7 x {ptr, size, handler}
static const int SS_IDX[] = {0, 1, 2, 5};           // GS_INFO, RANKING, OPTION, GALLERY
static const char* const SS_NAME[] = {"GS_INFO", "RANKING", "OPTION", "KEY", "KEYBOARD", "GALLERY", "DEMO"};
static const uint32_t SS_CHUNK = 1024;
static struct SaveSync {
    bool active = false;          // this session syncs saves (not loopback, setting on)
    bool done = false;            // P1: player 2 has all of it; P2: applied
    bool failed = false;
    std::vector<uint8_t> blob;    // P1: what we send; P2: what came
    std::vector<uint8_t> own;     // P2: our own sections, put back at the end
    std::vector<uint8_t> got;     // P2: chunks received
    uint32_t total = 0, crc = 0, nchunks = 0;
    uint32_t ackBits[4] = {0, 0, 0, 0};
    uint32_t lastSend = 0, lastAck = 0, t0 = 0, resends = 0;
    bool applied = false;
} SS;
static volatile bool g_saveBlock = false;     // checked by the SaveData.dat writer hook
static uint32_t g_saveBlocked = 0;
static uint32_t SsCrc(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
static bool SsSection(int idx, uint32_t* ptr, uint32_t* size) {
    const uint32_t* e = (const uint32_t*)(SS_REG + idx * 12);
    *ptr = e[0];
    *size = e[1];
    return e[0] >= 0x008E8000 && e[0] + e[1] <= 0x051EE000 && e[1] > 0 && e[1] <= 0x10000;
}
static bool SsGather(std::vector<uint8_t>& out) {
    out.clear();
    for (int idx : SS_IDX) {
        uint32_t ptr, size;
        if (!SsSection(idx, &ptr, &size)) { Log("net: save section %s looks wrong (0x%08X, %u bytes)", SS_NAME[idx], ptr, size); return false; }
        out.insert(out.end(), (const uint8_t*)ptr, (const uint8_t*)ptr + size);
    }
    return true;
}
static void SsScatter(const std::vector<uint8_t>& in) {
    size_t at = 0;
    for (int idx : SS_IDX) {
        uint32_t ptr, size;
        SsSection(idx, &ptr, &size);
        memcpy((void*)ptr, &in[at], size);
        at += size;
    }
}
static void SsStart() {
    SS = SaveSync();
    if (N.loop || !g_ncfg.syncSave) { SS.done = true; return; }
    SS.active = true;
    SS.t0 = GetTickCount();
    std::vector<uint8_t> mine;
    if (!SsGather(mine)) { SS.failed = true; NetEnd("could not read this game's saved data"); return; }
    SS.total = (uint32_t)mine.size();
    SS.nchunks = (SS.total + SS_CHUNK - 1) / SS_CHUNK;
    if (SS.nchunks > 128) { SS.failed = true; NetEnd("saved data too big to send"); return; }
    if (N.side == 0) {
        SS.blob = mine;
        SS.crc = SsCrc(SS.blob.data(), SS.blob.size());
        Log("net: sending this PC's saved data to player 2 (%u bytes; GS_INFO, RANKING, OPTION, GALLERY)", SS.total);
    } else {
        SS.own = mine;
        SS.blob.assign(SS.total, 0);
        SS.got.assign(SS.nchunks, 0);
        Log("net: waiting for player 1's saved data (%u bytes). Your own save stays as it is and comes back when you disconnect.", SS.total);
    }
}
static void SsSendAck() {
    NSaveAck a{};
    a.h = MakeHdr(M_SAVEACK);
    a.total = SS.total;
    a.crc = SS.crc;
    a.nchunks = (uint16_t)SS.nchunks;
    uint32_t have = 0;
    for (uint32_t c = 0; c < SS.nchunks && c < 128; c++)
        if (SS.got.size() > c && SS.got[c]) { a.bits[c / 32] |= 1u << (c % 32); have++; }
    a.have = (uint16_t)(SS.applied ? 0xFFFF : have);
    NetSend(&a, sizeof a);
    SS.lastAck = GetTickCount();
}
static void SsApply() {
    if (SsCrc(SS.blob.data(), SS.blob.size()) != SS.crc) {
        Log("net: player 1's saved data came damaged, asking again");
        std::fill(SS.got.begin(), SS.got.end(), 0);
        return;
    }
    uint32_t diff = 0;
    for (size_t i = 0; i < SS.blob.size(); i++) diff += SS.blob[i] != SS.own[i];
    g_saveBlock = true;              // from now on the game may not write SaveData.dat here
    SsScatter(SS.blob);
    SS.applied = true;
    SS.done = true;
    Log("net: using player 1's saved data while connected (%u of %u bytes differed from yours, %u ms). SaveData.dat on this PC will not be written until you disconnect.",
        diff, SS.total, GetTickCount() - SS.t0);
}
static void SsRestore() {
    if (SS.applied && SS.own.size() == SS.total) {
        SsScatter(SS.own);
        Log("net: your own saved data is back in the game (%u save writes were skipped while connected)", g_saveBlocked);
    }
    SS.applied = false;
    g_saveBlock = false;
    g_saveBlocked = 0;
}
static void SsHandle(const uint8_t* p, int n) {
    if (!SS.active) return;
    const NHdr* h = (const NHdr*)p;
    if (h->type == M_SAVE && N.side == 1) {
        if (n < (int)offsetof(NSave, data)) { N.badPackets++; return; }
        const NSave* m = (const NSave*)p;
        if (m->total != SS.total || m->nchunks != SS.nchunks || m->chunk >= SS.nchunks) {
            if (!SS.failed) { SS.failed = true; Log("net: saved data sizes differ (theirs %u, ours %u)", m->total, SS.total); }
            NetEnd("the two games' saved data have different sizes");
            return;
        }
        if (SS.applied) { if (GetTickCount() - SS.lastAck > 100) SsSendAck(); return; }
        SS.crc = m->crc;
        uint32_t off = m->chunk * SS_CHUNK, len = std::min<uint32_t>(SS_CHUNK, SS.total - off);
        if (n < (int)(offsetof(NSave, data) + len)) { N.badPackets++; return; }
        memcpy(&SS.blob[off], m->data, len);
        SS.got[m->chunk] = 1;
        bool all = true;
        for (auto g : SS.got) all = all && g;
        if (all) SsApply();
        if (all || GetTickCount() - SS.lastAck > 50) SsSendAck();
    } else if (h->type == M_SAVEACK && N.side == 0) {
        if (n < (int)sizeof(NSaveAck)) { N.badPackets++; return; }
        const NSaveAck* a = (const NSaveAck*)p;
        if (a->total != SS.total) return;
        if (a->have == 0xFFFF) {
            if (!SS.done) Log("net: player 2 has this PC's saved data (%u ms, %u resends)", GetTickCount() - SS.t0, SS.resends);
            SS.done = true;
            return;
        }
        memcpy(SS.ackBits, a->bits, sizeof SS.ackBits);
    }
}
// Called every frame (and while waiting): player 1 sends the chunks player 2
// has not confirmed, player 2 repeats its confirmation now and then.
static void SsTick() {
    if (!SS.active || N.st != NS_ON) return;
    uint32_t now = GetTickCount();
    if (N.side == 0) {
        if (SS.done || now - SS.lastSend < 300) return;
        if (SS.lastSend) SS.resends++;
        SS.lastSend = now;
        SvPacer pace;
        for (uint32_t c = 0; c < SS.nchunks; c++) {
            if (SS.ackBits[c / 32] & (1u << (c % 32))) continue;
            NSave m{};
            m.h = MakeHdr(M_SAVE);
            m.total = SS.total;
            m.crc = SS.crc;
            m.chunk = (uint16_t)c;
            m.nchunks = (uint16_t)SS.nchunks;
            uint32_t off = c * SS_CHUNK, len = std::min<uint32_t>(SS_CHUNK, SS.total - off);
            memcpy(m.data, &SS.blob[off], len);
            NetSend(&m, (int)(offsetof(NSave, data) + len));
            pace.Sent(offsetof(NSave, data) + len);
        }
    } else if (SS.applied && now - SS.lastAck > 500 && now - SS.t0 < 10000) {
        SsSendAck();     // in case our "all there" got lost
    }
}

// ---- Session start / end ----------------------------------------------------
static void NetRestoreGame() {
    if (!N.saved) return;
    memcpy((void*)G_PAD_CFG, N.origCfg, sizeof N.origCfg);
    memcpy((void*)G_SLOT_MAP, N.origMap, 10);
    *(uint8_t*)G_SLOT_SWAP = N.origSwap;
    N.saved = false;
}
static void DcOnEnd(const char* why);   // build 108: the "connection lost" pop-up (overlay.inc)
static bool g_byeClosing = false;       // build 108: our BYE says "closed the game" (flags 1)
static const uint32_t NET_AWAY_MS = 30000;   // build 111: how long the other player may be switched away
static std::atomic<bool> g_closeReq{false};  // build 111: the window is closing (window thread asks the game thread)
// Build 111: in full screen the game stops completely while it has no screen (alt-tab: the
// frame loop sleeps while byte 0x051CACEA, "device lost", is set). This thread then keeps the
// session alive: an "I'm away" ping (flags 2) every 300 ms. Only sends; it never reads.
static DWORD WINAPI NetAwayThread(void*) {
    uint32_t sent = 0;
    for (;;) {
        Sleep(300);
        if (N.st != NS_ON || !N.tr && !N.loop) { sent = 0; continue; }
        uint32_t now = GetTickCount();
        bool lost = *reinterpret_cast<volatile uint8_t*>(0x051CACEA) != 0;
        if (!lost || now - N.gameTick < 600) { if (sent) { Log("net: game running again after %u away pings", sent); sent = 0; } continue; }
        if (N.loop) { sent++; continue; }       // (loopback: the stand-in lives in this game)
        NPing m{};
        m.h = MakeHdr(M_PING);
        m.h.flags = 2;
        m.t = now;
        N.tr->Send(&m, sizeof m);
        if (sent++ == 0) Log("net: this game has no screen (switched away); keeping the session alive");
    }
    return 0;
}
static void NetEnd(const char* why) {
    if (N.st == NS_OFF) return;
    DcOnEnd(why);
    Log("net: session ends (%s). Packets sent %u, received %u; waited for input %u times (%u ms), %u frames skipped, %u frames slowed to keep the clocks together",
        why, N.sent, N.recvd, N.waited, N.waitMs, N.stalls, N.timeStalls);
    Log("net:   desync check: %u fingerprints matched, %u matched once pointers were left out, %u DESYNCS; game srand calls ignored %u; %u bad packets; %u date reads by the game logic (fixed per screen)",
        N.fpOk, N.fpFalse, N.desyncs, N.srandIgnored, N.badPackets, g_logicTimeCalls);
    if (N.trackingWas >= 0) { g_cfg.dirtyTracking = N.trackingWas != 0; N.trackingWas = -1; }
    NHdr b = MakeHdr(M_BYE);
    b.flags = g_byeClosing ? 1 : 0;
    for (int i = 0; i < 3; i++) NetSend(&b, sizeof b);
    if (g_mode == Mode::SyncTest) SyncTestStop("network session ended");
    else if (g_mode == Mode::Profiling) ProfileStop();
    else if (g_mode == Mode::Writers) WritersStop();
    g_auto = 0;
    NetRestoreGame();
    BootSessionEnd();                     // build 61: our rounds/timer out of the options again (before the save data goes back)
    SsRestore();
    if (N.st == NS_ON) UnflagAllPads();
    if (N.tr && !g_testOut.empty()) {     // build 122 test fake lag: send what still waits (the goodbye too)
        EnterCriticalSection(&g_testLock);
        for (auto& k : g_testOut) N.tr->Send(k.d, k.n);
        g_testOut.clear();
        LeaveCriticalSection(&g_testLock);
    }
    if (N.tr) { delete N.tr; N.tr = nullptr; }
    g_loopOut.clear();
    g_loopIn.clear();
    N.st = NS_OFF;
    g_netStall = false;
    NetStatus("offline (%s) - F4 to connect", why);
}
#include "desynclog.inc"           // build 121
static void NetResetScreen() {
    N.myDelay = (uint8_t)(g_ncfg.delay < 0 ? 0 : g_ncfg.delay > 8 ? 8 : g_ncfg.delay);   // build 122: locked for this screen
    N.rDelay = 0xFF;
    N.timeStallsAtStart = N.timeStalls;
    DlScreenStart();                  // build 121
    for (auto& x : N.rIn) x = NetIn();
    for (auto& x : N.lIn) x = NetIn();
    N.rIn[1].frame = 1;                   // frame 1 of every synced screen: no input on both sides
    N.lIn[1].frame = 1;
    N.rContig = 2;
    N.rMax = 1;
    N.rAck = 1;
    N.lNewest = 1;
    for (uint32_t f = 2; f <= ScreenDelay(); f++) { N.lIn[f].frame = f; N.lNewest = f; }
    for (auto& L : N.fl) L = FpLocal();
    for (auto& R : N.fr) R = FpRemote();
    N.injected = 0;
    N.injectedConsumed = true;
    N.autoDone = false;
    N.aheadF = N.aheadQ = N.rAheadQ = N.syncQ = 0;
    N.lastAdjust = 0;
    N.stretching = false;
    N.diagN = 0;
    N.diagRaw0 = N.diagRaw1 = N.diagHeld0 = N.diagHeld1 = 0xFFFFFFFFu;
    N.diagFg = -1;
}
static void NetApplyLayouts() {
    // Both PCs: player 1 uses player 1's own layout, player 2 player 2's,
    // and the two players sit on controller slots 0 and 1.
    const uint32_t* mine = N.origCfg;                 // this PC's player 1 layout
    uint32_t* cfg = (uint32_t*)G_PAD_CFG;
    memcpy(cfg + 12 * N.side, mine, 48);
    memcpy(cfg + 12 * (1 - N.side), N.rh.padCfg, 48);
    uint8_t* map = (uint8_t*)G_SLOT_MAP;
    map[0] = 0;
    map[1] = 1;
    *(uint8_t*)G_SLOT_SWAP = 0;
}
static void NetBecomeOn() {
    N.st = NS_ON;
    const NHello& r = N.rh;
    if (g_ncfg.wantSide && r.wantSide && g_ncfg.wantSide == r.wantSide) {
        NetEnd("both games want to be the same player: change side= in rollback.ini");
        return;
    }
    if (g_ncfg.wantSide) N.side = g_ncfg.wantSide - 1;
    else if (r.wantSide) N.side = 2 - r.wantSide;
    else N.side = N.nonce > r.nonce ? 0 : 1;
    if (N.side == 1) N.seed = r.seed;            // player 1's seed is used
    if (!N.saved) {
        memcpy(N.origCfg, (const void*)G_PAD_CFG, sizeof N.origCfg);
        memcpy(N.origMap, (const void*)G_SLOT_MAP, 10);
        N.origSwap = *(const uint8_t*)G_SLOT_SWAP;
        N.saved = true;
    }
    uint32_t d = PadOfPlayer(0);
    N.localDev = d < addr::PAD_COUNT ? d : 0;
    N.epoch = 0;
    N.sceneVt = 0xFFFFFFFFu;            // treat the current screen as new
    N.rEpoch = 0;
    N.lastRecv = GetTickCount();
    NetResetScreen();
    char who[96];
    if (N.tr) N.tr->Describe(who, sizeof who); else snprintf(who, sizeof who, "%s", r.name);
    snprintf(N.peerName, sizeof N.peerName, "%s", who);
    Log("net: CONNECTED to %s (computer \"%s\"). You are player %d. Their input delay %u, yours %d. Your controller: slot %u. Seed %08X.",
        who, r.name, N.side + 1, r.delay, g_ncfg.delay, N.localDev, N.seed);
    if (N.localDev >= 8) Log("net: note: you seem to play on the keyboard. Online play needs a controller for now.");
    BuildPtrMap(true);     // build 77: makes the pointer-map worker thread now, not during a fight
    LbReset();
    UiSessionStarted();
    SsStart();
}
static bool PcActive();          // build 124 (perfcheck.inc)
static void PcOverrideNet();
static uint32_t PcScriptInput();
static void NetStart() {
    {   // build 111
        static bool awayThread = false;
        if (!awayThread) { awayThread = true; CreateThread(nullptr, 0, NetAwayThread, nullptr, 0, nullptr); }
    }
    N.peerAwaySince = 0;
    if (N.st != NS_OFF) return;
    LoadNetSettings();
    if (g_roomCfg.on) {
        g_ncfg.mode = g_roomCfg.loop ? 0 : 2;
        g_ncfg.wantSide = g_roomCfg.loop ? 0 : g_roomCfg.side + 1;   // the stand-in is always player 2
        Log("net: room table match: %s, you are player %d", g_roomCfg.loop ? "test player on this PC" : "Steam", g_roomCfg.loop ? 1 : g_roomCfg.side + 1);
    }
    if (PcActive()) PcOverrideNet();   // build 124: the Performance check's own one-PC session
    N = NetS();
    N.codeHash = g_codeHash;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    N.nonce = (uint32_t)q.LowPart ^ (GetCurrentProcessId() << 16) ^ GetTickCount();
    N.seed = N.nonce * 2654435761u + 0x2545F491u;
    char err[160] = "";
    if (g_ncfg.mode == 0) {
        N.loop = true;
        g_lp = LoopPeer();
        snprintf(N.peerName, sizeof N.peerName, "loopback stand-in");
    } else if (g_ncfg.mode == 1) {
        N.tr = net::MakeUdp(g_ncfg.lanPeer, g_ncfg.lanPort, err, sizeof err);
    } else {
        uint64_t who = g_roomCfg.on ? g_roomCfg.peer : g_netPick ? g_netPick : g_ncfg.steamFriend;
        if (!who && net::SteamOpen(err, sizeof err)) {
            net::Friend f[1];
            if (net::SteamFriends(f, 1) == 1 && f[0].inGame) who = f[0].id;
            else snprintf(err, sizeof err, "no friend is playing the game right now (F5 picks one)");
        }
        if (who) N.tr = net::MakeSteam(who, err, sizeof err);
    }
    if (!N.loop && !N.tr) {
        Log("net: could not start: %s", err);
        NetStatus("could not connect: %s", err);
        return;
    }
    if (N.tr) N.tr->Describe(N.peerName, sizeof N.peerName);
    N.st = NS_HELLO;
    N.startMs = N.lastRecv = GetTickCount();
    Log("net: connecting (%s) to %s", N.loop ? "loopback test" : N.tr->Name(), N.peerName);
    NetStatus("connecting to %s... (F4 cancels)", N.peerName);
    SendHello();
}

// ---- Packets in --------------------------------------------------------------
static void SvHandle(const uint8_t* p, int n);
static void SsHandle(const uint8_t* p, int n);
static void NetHandle(const uint8_t* p, int n) {
    if (n < (int)sizeof(NHdr)) { N.badPackets++; return; }
    const NHdr* h = (const NHdr*)p;
    if (h->magic != NET_MAGIC) { N.badPackets++; return; }
    if (h->type == M_HELLO) {
        if (n < (int)sizeof(NHello)) { N.badPackets++; return; }
        const NHello* m = (const NHello*)p;
        if (m->nonce == N.nonce) return;                     // our own broadcast
        N.recvd++;
        N.lastRecv = GetTickCount();
        if (N.tr) N.tr->LockPeer();
        if (N.gotHello && m->nonce != N.rh.nonce) {
            if (N.st == NS_ON) { NetEnd("the other game restarted"); return; }
        }
        if (m->proto != NET_PROTO || m->build != NET_BUILD) {
            Log("net: the other game runs mod build %u (protocol %u), this one %u (%u)", m->build, m->proto, NET_BUILD, NET_PROTO);
            NetEnd(m->build > NET_BUILD ? "the other player has a newer version of the mod: you need to update"
                                        : "the other player has an older version of the mod: they need to update");
            return;
        }
        if (m->codeHash != N.codeHash) {
            Log("net: the other game's code hash is %08X, this one %08X", m->codeHash, N.codeHash);
            NetEnd("the other player's game version is different");
            return;
        }
        bool first = !N.gotHello;
        if (first) {
            N.rh = *m;
            // build 128 (security): the other game's hello goes into game memory (button layout)
            // and picks the side, so only values the game has are kept
            N.rh.name[sizeof N.rh.name - 1] = 0;
            if (N.rh.wantSide > 2) { Log("net: the other game asked for side %u; ignored", N.rh.wantSide); N.rh.wantSide = 0; }
            bool layoutOk = true;
            for (int i = 0; i < 12; i++) layoutOk = layoutOk && N.rh.padCfg[i] <= 18;   // button numbers 0-17, 18 = none
            if (!layoutOk) {
                static const uint32_t kDefaultPad[12] = {4, 5, 6, 7, 11, 18, 10, 12, 18, 18, 13, 18};
                memcpy(N.rh.padCfg, kDefaultPad, sizeof kDefaultPad);
                Log("net: the other game's button layout has values the game doesn't know; using the default layout for them");
            }
        }
        N.gotHello = true;
        if (first || !m->seenYou || N.st == NS_ON) SendHello();   // (ON: our last hello may have been lost)
        if (N.st == NS_HELLO && m->seenYou) NetBecomeOn();
        return;
    }
    if (N.st != NS_ON) return;
    N.recvd++;
    N.lastRecv = GetTickCount();
    if (h->type == M_PING && (h->flags & 2)) {            // build 111: the other player's game has no screen right now
        if (!N.peerAwaySince) { N.peerAwaySince = N.lastRecv | 1; Log("net: the other player switched away from the game (full screen)"); }
    } else if (h->type != M_PONG && N.peerAwaySince) {
        Log("net: the other player is back after %u ms", N.lastRecv - N.peerAwaySince);
        N.peerAwaySince = 0;
    }
    switch (h->type) {
    case M_INPUT: {
        if (n < (int)offsetof(NInput, in)) { N.badPackets++; return; }
        const NInput* m = (const NInput*)p;
        if (n < (int)(offsetof(NInput, in) + m->count * 8) || m->count > 32) { N.badPackets++; return; }
        N.rEpoch = m->h.epoch;
        N.rSceneVt = m->sceneVt;
        if (m->h.epoch != N.epoch) return;
        N.rCur = m->cur;
        N.rAheadQ = m->ahead;
        if (m->delay1 && m->delay1 <= 9 && N.rDelay != m->delay1 - 1) {    // build 122
            N.rDelay = (uint8_t)(m->delay1 - 1);
            Log("net: screen %u: the other player's input delay is %u (yours %u)", N.epoch, N.rDelay, N.myDelay);
        }
        if (m->ack > N.rAck && m->ack <= N.lNewest) N.rAck = m->ack;
        for (uint32_t i = 0; i < m->count; i++) {
            uint32_t f = m->first + i;
            if (f < N.rContig || f >= N.rContig + 200) continue;
            NetIn& x = N.rIn[f % 256];
            if (x.frame == f) continue;
            x.frame = f;
            x.v[0] = m->in[i][0] & PadRawMask();   // build 128: only buttons a real controller has
            x.v[1] = m->in[i][1];
            if (f > N.rMax) N.rMax = f;
        }
        while (N.rIn[N.rContig % 256].frame == N.rContig) N.rContig++;
        break;
    }
    case M_FP: {
        if (n < (int)sizeof(NFp)) { N.badPackets++; return; }
        const NFp* m = (const NFp*)p;
        if (m->h.epoch != N.epoch || m->nblk != FP_NBLK) return;
        FpRemote& R = N.fr[(m->frame / FP_EVERY) % 8];
        R.frame = m->frame;
        R.seed = m->seed;
        memcpy(R.hash, m->hash, sizeof R.hash);
        FpCompare(N.fl[(m->frame / FP_EVERY) % 8]);
        break;
    }
    case M_RAW:
        if (h->epoch == N.epoch) NetHandleRaw((const NRaw*)p, n);
        break;
    case M_SURV:
    case M_SPAGE:
        if (g_ncfg.survey) SvHandle(p, n);   // build 128: a test tool; ignored unless turned on here
        break;
    case M_SAVE:
    case M_SAVEACK:
        SsHandle(p, n);
        break;
    case M_PING: {
        if (n < (int)sizeof(NPing)) return;
        NPing r = *(const NPing*)p;
        r.h.type = M_PONG;
        NetSend(&r, sizeof r);
        break;
    }
    case M_PONG: {
        if (n < (int)sizeof(NPing) || (h->flags & 2)) return;    // (an answer to an "I'm away" ping)
        N.rtt = GetTickCount() - ((const NPing*)p)->t;   // (what the clock sync uses, unchanged)
        PingGot(((const NPing*)p)->us);   // build 121: exact, for the log (desynclog.inc)
        break;
    }
    case M_RCHK:                          // build 121
        RchkHandle(p, n);
        break;
    case M_SNAP:
    case M_SNAPH:
        SnapHandle(p, n);
        break;
    case M_BYE:
        NetEnd(h->flags == 1 ? "the other player closed the game" : "the other player left");
        break;
    case M_LOBBY:
    case M_START:
    case M_STARTACK:
        LbHandle(p, n);
        break;
    default: N.badPackets++; break;
    }
}
static void NetPump() {
    if (N.tr) N.tr->Tick();
    if (N.loop) LoopTick();
    static uint8_t buf[2048];
    for (int i = 0; i < 64 && N.st != NS_OFF; i++) {
        int n = NetRecvRaw(buf, sizeof buf);
        if (n < 0) break;
        NetHandle(buf, n);
    }
}
static void NetSendInput() {
    if (N.st != NS_ON) return;
    NInput m{};
    m.h = MakeHdr(M_INPUT);
    m.sceneVt = N.syncedType && N.ready ? N.sceneVt : 0;   // build 52: only once loading is done here
    m.ack = N.rContig - 1;
    m.cur = g_frame;
    m.ahead = (int8_t)(N.aheadQ > 127 ? 127 : N.aheadQ < -127 ? -127 : N.aheadQ);
    m.delay1 = (uint8_t)(N.myDelay + 1);                   // build 122
    m.first = N.rAck + 1 < 2 ? 2 : N.rAck + 1;
    uint32_t cnt = 0;
    if (N.syncedType)
        while (cnt < 32 && m.first + cnt <= N.lNewest) {
            const NetIn& x = N.lIn[(m.first + cnt) % 256];
            m.in[cnt][0] = x.v[0];
            m.in[cnt][1] = x.v[1];
            cnt++;
        }
    m.count = (uint8_t)cnt;
    NetSend(&m, (int)(offsetof(NInput, in) + cnt * 8));
}

// ---- Per frame -----------------------------------------------------------------
static void NetUpdateStatus() {
    if (N.st != NS_ON) return;
    const char* where = !N.syncedType ? "loading" : !N.synced ? "waiting for the other game" : NetRbMode() ? "rollback" : "in step";
    char ds[64] = "";
    if (N.desyncs) snprintf(ds, sizeof ds, " | DESYNC x%u (%s)!", N.desyncs, N.lastDesync);
    char ah[48] = "";
    if (NetRbMode()) snprintf(ah, sizeof ah, " | ahead %+.1f", N.syncQ / 4.0);
    NetStatus("ONLINE P%d vs %s | ping %u ms | delay %u | %s%s | waited %u ms, skipped %u%s", N.side + 1, N.peerName, N.rtt, ScreenDelay(), where,
              ah, N.waitMs, N.stalls, ds);
}
// Build 41: wait for the other game right here (a few milliseconds,
// usually) instead of skipping the frame. A skipped frame still runs the
// game's code around the update (drawing, sound) once more, and test 40
// suggests that broke the sync check. Only if nothing comes for a while is
// the frame skipped, so the window keeps responding.
template <typename Ready> static bool NetWait(Ready ready, uint32_t maxMs) {
    if (ready()) return true;
    PerfScope ps(perf::P_WAIT);    // build 72
    uint32_t t0 = GetTickCount(), lastSend = t0;
    for (;;) {
        Sleep(1);
        NetPump();
        if (N.st != NS_ON) return false;
        if (ready()) { N.waitMs += GetTickCount() - t0; N.waited++; return true; }
        uint32_t now = GetTickCount();
        if (now - t0 >= maxMs) { N.waitMs += now - t0; return false; }
        if (now - lastSend >= 15) { NetSendInput(); lastSend = now; }
    }
}

// ---- Build 46: memory survey ----------------------------------------------------
// Test 46 (two PCs): each PC starts character select and the fight from its
// own saved settings (cursor memory, assists, colours), so the games differed
// from the first frame. At the start of each synced screen (frame 2, both
// games in step) both PCs hash every page of the game's .data, swap the
// hashes, then swap the pages that differ, and write the differences to
// rollback_memdiff.txt. Values that look like heap/DLL pointers are ignored.
static const uint32_t SV_LO = 0x008E8000, SV_HI = 0x051EE000;
static const uint32_t SV_PAGES = (SV_HI - SV_LO) / 0x1000;
static struct Survey {
    uint16_t epoch = 0xFFFF;
    std::vector<uint32_t> rh;          // remote hashes
    std::vector<uint8_t> rhGot;
    uint32_t rhCount = 0;
    std::vector<uint32_t> want;        // differing pages
    std::vector<int> wantIdx;          // page -> index in want, -1
    std::vector<uint8_t> rpage;        // remote copies (want.size() * 4096)
    std::vector<uint32_t> rmask;       // remote masks (want.size() * 32)
    std::vector<uint8_t> rpart;        // parts received (want.size() * 4)
    uint32_t partsGot = 0;
    bool active = false;
} SV;
static uint32_t SvHashPage(uint32_t pg) {
    const uint32_t* w = (const uint32_t*)(SV_LO + pg * 0x1000);
    uint32_t h = 0x811C9DC5u ^ pg;
    for (int i = 0; i < 1024; i++) {
        uint32_t v = w[i];
        if (LooksLikePtr(v)) v = 0x9A11E7u;
        h = (h ^ v) * 0x01000193u;
    }
    return h;
}
static void SvReset(uint16_t epoch) {
    SV.epoch = epoch;
    SV.rh.assign(SV_PAGES, 0);
    SV.rhGot.assign(SV_PAGES, 0);
    SV.rhCount = 0;
    SV.want.clear();
    SV.wantIdx.assign(SV_PAGES, -1);
    SV.rpage.clear(); SV.rmask.clear(); SV.rpart.clear();
    SV.partsGot = 0;
}
static void SvHandle(const uint8_t* p, int n) {
    const NHdr* h = (const NHdr*)p;
    if (h->epoch != N.epoch) return;
    if (SV.epoch != N.epoch) SvReset(N.epoch);
    if (h->type == M_SURV) {
        if (n < (int)offsetof(NSurv, hash)) return;
        const NSurv* m = (const NSurv*)p;
        if (m->first >= SV_PAGES || m->count > 256 || n < (int)(offsetof(NSurv, hash) + m->count * 4)) return;
        for (uint32_t i = 0; i < m->count && m->first + i < SV_PAGES; i++) {
            uint32_t pg = m->first + i;
            if (!SV.rhGot[pg]) { SV.rhGot[pg] = 1; SV.rhCount++; }
            SV.rh[pg] = m->hash[i];
        }
    } else if (h->type == M_SPAGE) {
        if (n < (int)sizeof(NSPage)) return;
        const NSPage* m = (const NSPage*)p;
        if (m->page >= SV_PAGES || m->part > 3) return;
        int k = SV.wantIdx[m->page];
        if (k < 0) return;
        if (SV.rpart[k * 4 + m->part]) return;
        SV.rpart[k * 4 + m->part] = 1;
        SV.partsGot++;
        memcpy(&SV.rpage[(size_t)k * 4096 + m->part * 1024], m->data, 1024);
        memcpy(&SV.rmask[(size_t)k * 32 + m->part * 8], m->mask, 32);
    }
}
static const char* SvLabel(uint32_t a) {
    if (a >= FP_BASE && a < FP_BASE + FP_SIZE) return a < FP_BASE + 0xA520 ? "P1 data" : "P2 data";
    if (a >= G_PAD_CFG && a < G_PAD_CFG + 96) return "controller layout";
    if (a >= G_SLOT_MAP && a < G_SLOT_MAP + 10) return "controller slots";
    for (auto& r : addr::INPUT_RANGES) if (a >= r.start && a < r.end) return "pad input";
    return "";
}
// Build 47: send at most ~3 MB/s (test 46b: bursts overflowed the send
// buffer over Wi-Fi, so only the first 64 packets of each burst arrived).
static void NetSurvey(const char* where) {
    if (N.loop || !g_ncfg.survey) return;
    uint32_t t0 = GetTickCount();
    if (SV.epoch != N.epoch) SvReset(N.epoch);
    BuildPtrMap();
    std::vector<uint32_t> lh(SV_PAGES);
    for (uint32_t pg = 0; pg < SV_PAGES; pg++) lh[pg] = SvHashPage(pg);
    auto sendHashes = [&] {
        SvPacer pace;
        for (uint32_t f = 0; f < SV_PAGES; f += 256) {
            NSurv m{};
            m.h = MakeHdr(M_SURV);
            m.first = f;
            m.count = (uint16_t)std::min<uint32_t>(256, SV_PAGES - f);
            memcpy(m.hash, &lh[f], m.count * 4);
            NetSend(&m, (int)(offsetof(NSurv, hash) + m.count * 4));
            pace.Sent(offsetof(NSurv, hash) + m.count * 4);
        }
    };
    sendHashes();
    uint32_t lastResend = GetTickCount();
    NetWait([&] {
        if (SV.rhCount == SV_PAGES) return true;
        if (GetTickCount() - lastResend > 1000) { sendHashes(); lastResend = GetTickCount(); }
        return false;
    }, 6000);
    if (SV.rhCount < SV_PAGES) { Log("net: memory survey (%s): only %u of %u page hashes came from the other PC", where, SV.rhCount, SV_PAGES); }
    for (uint32_t pg = 0; pg < SV_PAGES; pg++)
        if (SV.rhGot[pg] && SV.rh[pg] != lh[pg] && SV.want.size() < 1500) { SV.wantIdx[pg] = (int)SV.want.size(); SV.want.push_back(pg); }
    SV.rpage.assign(SV.want.size() * 4096, 0);
    SV.rmask.assign(SV.want.size() * 32, 0);
    SV.rpart.assign(SV.want.size() * 4, 0);
    // our copies of the differing pages (and which values we call pointers)
    std::vector<uint8_t> mine(SV.want.size() * 4096);
    std::vector<uint32_t> mmask(SV.want.size() * 32, 0);
    for (size_t k = 0; k < SV.want.size(); k++) {
        memcpy(&mine[k * 4096], (const void*)(SV_LO + SV.want[k] * 0x1000), 4096);
        const uint32_t* w = (const uint32_t*)&mine[k * 4096];
        for (int i = 0; i < 1024; i++) if (LooksLikePtr(w[i])) mmask[k * 32 + i / 32] |= 1u << (i % 32);
    }
    // Lost packets are not resent one by one: the whole set goes out twice
    // if parts are missing.
    auto sendPages = [&] {
        SvPacer pace;
        uint32_t sent = 0;
        for (size_t k = 0; k < SV.want.size(); k++)
            for (int part = 0; part < 4; part++) {
                NSPage m{};
                m.h = MakeHdr(M_SPAGE);
                m.page = SV.want[k];
                m.part = (uint16_t)part;
                memcpy(m.mask, &mmask[k * 32 + part * 8], 32);
                memcpy(m.data, &mine[k * 4096 + part * 1024], 1024);
                NetSend(&m, sizeof m);
                pace.Sent(sizeof m);
                sent++;
            }
    };
    uint32_t need = (uint32_t)SV.want.size() * 4;
    // Always two rounds (the other PC may have lost some of ours even when we
    // got all of theirs), a third only if ours are still incomplete.
    for (int round = 0; round < 3; round++) {
        if (round == 2 && SV.partsGot >= need) break;
        sendPages();
        NetWait([&] { return SV.partsGot >= need; }, round == 0 ? 4000 : 1500);
    }
    // Report
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%lsrollback_memdiff.txt", util::ModDir());
    FILE* f = _wfopen(path, N.epoch <= 1 ? L"w" : L"a");
    uint32_t pagesDiff = 0, vals = 0;
    if (f) fprintf(f, "==== screen %u (%s), this PC is player %d. %u of %u pages differ (pointer-like values ignored), %u of %u page copies came\n",
                   N.epoch, where, N.side + 1, (unsigned)SV.want.size(), SV_PAGES, SV.partsGot, need);
    for (size_t k = 0; k < SV.want.size(); k++) {
        const uint32_t* a = (const uint32_t*)&mine[k * 4096];
        const uint32_t* b = (const uint32_t*)&SV.rpage[k * 4096];
        uint32_t n = 0;
        for (int i = 0; i < 1024; i++) {
            if (!SV.rpart[k * 4 + i / 256]) continue;
            uint32_t bit = 1u << (i % 32);
            if ((mmask[k * 32 + i / 32] | SV.rmask[k * 32 + i / 32]) & bit) continue;
            if (a[i] == b[i]) continue;
            uint32_t addr = SV_LO + SV.want[k] * 0x1000 + i * 4;
            if (f && n < 48) {
                const char* lab = SvLabel(addr);
                if (lab[0] == 'P' && lab[1] != 'a') fprintf(f, "  0x%08X  this %08X  other %08X  (%s +0x%X)\n", addr, a[i], b[i], lab, (addr - FP_BASE) % 0xA520);
                else fprintf(f, "  0x%08X  this %08X  other %08X  %s\n", addr, a[i], b[i], lab);
            }
            n++;
        }
        if (n) { pagesDiff++; vals += n; if (f && n > 48) fprintf(f, "  ... page 0x%08X: %u values differ in total\n", SV_LO + SV.want[k] * 0x1000, n); }
    }
    if (f) fclose(f);
    Log("net: memory survey (%s): %u pages with %u differing values (not counting pointers), written to rollback_memdiff.txt (%u ms)",
        where, pagesDiff, vals, GetTickCount() - t0);
    N.lastRecv = GetTickCount();
}
// Build 50: the input state the game derived from earlier frames (held /
// previous flags of every pad, the menu key-repeat table) differs between
// the PCs when a synced screen starts; clear it on both.
static void ClearMenuInputState() {
    for (uint32_t i = 0; i < addr::PAD_COUNT; i++) memset(Pad(i) + 0x18, 0, 0x60 - 0x18);
    memset(reinterpret_cast<void*>(0x04DC3430), 0, 2 * 0x168);
}
static NetAct NetBeforeInput() {
    if (N.st == NS_OFF) return NetAct::Free;
    NetPump();
    uint32_t now = GetTickCount();
    if (N.st == NS_HELLO) {
        if (now - N.lastHello > 200) SendHello();
        return NetAct::Free;
    }
    if (N.st != NS_ON) return NetAct::Free;
    if (now - N.gameTick > 1000) N.lastRecv = now;  // build 111: this game had no screen (switched away); don't count that time
    N.gameTick = now;
    if (now - N.lastRecv > (uint32_t)g_ncfg.timeoutMs) { NetEnd("no packets from the other game for a while"); return NetAct::Free; }
    if (N.peerAwaySince && now - N.peerAwaySince > NET_AWAY_MS) { NetEnd("the other player switched away for too long"); return NetAct::Free; }
    // build 121: every 250 ms in a synced screen (ping and lost pings go into the log), else every second
    if (now - N.lastPing > (N.synced ? 250u : 1000u)) {
        NPing m{};
        m.h = MakeHdr(M_PING);
        m.t = now;
        m.us = NowUs32();
        NetSend(&m, sizeof m);
        if (N.synced) PingSent();
        N.lastPing = now;
    }
    LobbyTick();
    if (N.st != NS_ON) return NetAct::Free;
    SsTick();
    uint32_t vt, ptr;
    CurScreen(&vt, &ptr);
    // An input put in the pads last time that no update used (overlay screen,
    // or the rollback waited): take it back out, so the input step runs
    // exactly once per frame like on the other PC.
    if (!N.injectedConsumed && N.pre.size() == InputBytes()) WriteRanges(N.pre);
    N.injectedConsumed = true;
    if (vt != N.sceneVt || ptr != N.scenePtr) {
        if (g_mode == Mode::SyncTest) {
            if (g_rbOn && g_rbStarted && g_rbConf <= g_frame)
                Log("net: warning: the screen changed while %u frames still had guessed input", g_frame + 1 - g_rbConf);
            SyncTestStop("scene changed");
        } else if (g_mode == Mode::Profiling) ProfileStop();
        else if (g_mode == Mode::Writers) WritersStop();
        g_auto = 0;
        N.sceneVt = vt;
        N.scenePtr = ptr;
        N.syncedType = IsSyncedScene(vt);
        N.synced = false;
        N.ready = false;
        N.peerLeft = false;
        if (N.syncedType) {
            N.epoch++;
            NetResetScreen();
            g_frame = 0;
            t_seed = N.seed ^ (N.epoch * 0x9E3779B1u);
            t_seedInit = true;
            NetApplyLayouts();
            Log("net: screen %u (%s) starts, waiting for the other game to get there", N.epoch, addr::SceneName(vt));
        }
    }
    if (!N.syncedType) return NetAct::Free;
    // Build 52 (test 51): the game keeps a new screen's own update switched off
    // while it loads (137 frames on the laptop, 0 on the desktop). Until the
    // game calls that update here, the frames run freely and don't count;
    // the first call is held back (H_ChildUpdate) and marks this PC ready. Only
    // then does it tell the other PC "I'm on this screen", so frame 1 is the
    // screen's first real update on both PCs.
    if (!N.ready && g_frame >= 900) {                // build 55: safety net, never wait for this forever
        N.ready = true;
        Log("net: screen %u (%s): the game never ran the screen's own update in 900 frames; going on anyway", N.epoch, addr::SceneName(N.sceneVt));
    }
    if (!N.ready) return NetAct::Free;
    uint32_t T = g_frame + 1;
    if (!SS.done) {
        // Build 48: no synced screen runs before both games have the same saved data.
        NetWait([&] { SsTick(); return SS.done; }, 100);
        if (!SS.done) {
            if (GetTickCount() - SS.t0 > 15000) { NetEnd("the saved data did not get across"); return NetAct::Free; }
            N.stalls++;
            return NetAct::Stall;
        }
    }
    if (!N.synced) {
        NetWait([&] { return N.rEpoch == N.epoch && N.rSceneVt != 0; }, 100);
        if (N.rEpoch == N.epoch && N.rSceneVt == vt) {
            N.synced = true;
            g_frame = 0;                               // build 52: frames before this were loading only
            T = 1;
            Log("net: screen %u: both games are there (other game at frame %u)", N.epoch, N.rCur);
            NetSurvey(addr::SceneName(vt));
        } else if (N.rEpoch == N.epoch && N.rSceneVt && N.rSceneVt != vt) {
            Log("net: the other game is on screen %s, this one on %s", addr::SceneName(N.rSceneVt), addr::SceneName(vt));
            NetEnd("the two games went to different screens");
            return NetAct::Free;
        } else {
            N.stalls++;
            N.lastStallFrame = T;
            return NetAct::Stall;
        }
    }
    if (T == 1) {
        // Build 50: frame 1 only runs once both games are on this screen (test 49:
        // the PC that got to character select first ran a frame on its own), and
        // both start from the same menu input state: what each PC's pads held on
        // the screen before (held keys, key-repeat timers) is cleared.
        ClearMenuInputState();
        if (N.sceneVt == SCENE_BTL) {
            // Build 122: with different delays the lower-delay game runs ahead by the difference
            // (ClockTargetQ). Both start frame 1 together, so the higher-delay PC waits for the
            // difference once, here (both games were just waiting for each other anyway), instead
            // of the clock sync closing the gap later with one-frame waits you can see. The game
            // ahead runs those frames on our first frames' input, which were sent before (no input).
            int behind = N.rDelay <= 8 ? (int)N.myDelay - (int)N.rDelay : 0;
            char rd[16];
            if (N.rDelay <= 8) snprintf(rd, sizeof rd, "%u", N.rDelay); else snprintf(rd, sizeof rd, "not known yet");
            Log("net: fight screen %u: input delay yours %u, theirs %s; this PC aims to run %+d frames ahead%s",
                N.epoch, N.myDelay, rd, ClockTargetQ() / 4, behind > 0 ? "; waits for the difference once when rollback starts" : "");
            // build 123: the wait itself moved to NetRbStarting. In an in-place rematch the fight
            // first runs in step (the YES box) until rollback starts, which undid a wait made here.
        }
        return NetAct::Inject;                         // frame 1: no input on both PCs
    }
    if (!NetRbMode()) {
        // Build 56 (test 55): how long the loading screen lasts after the assist
        // set menu depends on how fast each PC loads the fight. The faster PC goes
        // on to the fight and sends no more input for this screen, so the other
        // one waited forever. Once the other game is on a later screen, finish
        // this one here without it (its choices on this screen are all in).
        auto peerAhead = [&] { return (uint16_t)(N.rEpoch - N.epoch - 1) < 0x8000; };
        if (N.peerLeft || peerAhead()) {
            if (!N.peerLeft) {
                N.peerLeft = true;
                Log("net: screen %u (%s): the other game went on to the next screen at our frame %u; finishing this one without it", N.epoch, addr::SceneName(N.sceneVt), T);
            }
            return NetAct::Free;
        }
        bool have = NetWait([&] { return N.rIn[T % 256].frame == T || peerAhead(); }, 250);
        if (!have || N.rIn[T % 256].frame != T) { if (peerAhead()) return NetAct::Stall; }
        if (!have) { N.stalls++; N.lastStallFrame = T; return NetAct::Stall; }
        return NetAct::Inject;
    }
    // Rollback: keep the two games' clocks together. If we keep running
    // ahead of the other game, wait a frame now and then.
    // Build 57 (test 56: the desktop ran about 7 frames ahead, so every
    // rollback was 7 deep): each PC works out how far ahead it runs (smoothed
    // over about half a second), sends that, and both use the average of the
    // two views (like GGPO). The PC that is ahead by a frame or more waits one
    // frame, as often as every 6 frames, until the gap is gone.
    {
        int rttQ = (int)(N.rtt * 240 / 1000);                       // round trip in quarter frames
        // build 122: measured against the target (the delay difference), not against 0
        int aheadNowQ = ((int)g_frame - (int)N.rCur) * 4 - rttQ / 2 - ClockTargetQ();
        N.aheadF += (aheadNowQ * 16 - N.aheadF) / 8;                // smoothing (x16 so nothing is lost to rounding)
        N.aheadQ = N.aheadF / 16;
        N.syncQ = (N.aheadQ - N.rAheadQ) / 2;
        // Build 73 (test 72a): each wait is a skipped frame (a 33 ms frame you can
        // see), and it fired 10-18 times per 10 s, mostly on noise. Now: nothing in
        // loopback (the stand-in runs in step with us by design); a gap under 1.5
        // frames is left alone; 1.5-2 frames: one wait at most every 30 frames;
        // 2 frames or more (after a hiccup): a wait every 2 frames until it is back.
        int gapQ = N.syncQ;
        // Build 123 (fixes 2 and 3): with our frame pacer, the PC that runs ahead makes its frames
        // a little longer instead of pausing a whole frame (0.25 ms per quarter frame of gap, at most
        // 1.5 ms = 9%: nobody sees it). Starts at half a frame of gap (was 1.5 frames: at equal delays
        // one PC sat a frame ahead and took all rollbacks), stops when the gap is gone. A whole-frame
        // wait is left only for gaps of 4 frames or more (after a freeze).
        if (PacerActive() && g_ncfg.clockSync && !N.loop) {
            if (gapQ >= 2) N.stretching = true;
            else if (gapQ <= 0) N.stretching = false;
            if (gapQ >= 16 && g_frame - N.lastAdjust >= 2) {
                N.lastAdjust = g_frame;
                N.timeStalls++;
                N.lastStallFrame = T;
                PaceAddDelay(1000.0 / 60.0, true);
            } else if (N.stretching) {
                double ms = gapQ * 0.25;
                PaceStretch(ms < 0.25 ? 0.25 : ms > 1.5 ? 1.5 : ms);
                N.stretchFrames++;
            }
            return NetAct::Inject;
        }
        uint32_t every = gapQ >= 8 ? 2 : gapQ >= 6 ? 30 : 0xFFFFFFFFu;
        if (g_ncfg.clockSync && !N.loop && every != 0xFFFFFFFFu && g_frame - N.lastAdjust >= every) {
            N.lastAdjust = g_frame;
            N.timeStalls++;
            N.lastStallFrame = T;
            { PerfScope ps(perf::P_WAIT); Sleep(16); }              // build 41: wait a frame's time instead of skipping
            return NetAct::Inject;
        }
    }
    return NetAct::Inject;
}
static bool g_childRan = false;              // the current screen's own update ran this frame
static uint32_t HeldMask(uint32_t pad);
// Called from the keyboard-read hook in the first run of frame T's input step.
static int BootZeroInput();    // boot.inc (build 107)
static std::atomic<uint32_t> g_postNav{0};   // build 107: this PC's controller on the after-match screen
// Build 114 (friend test 113: the friend's controller did nothing in his first fight; the
// session had taken the game's "player 1 controller", slot 0, while his pad was slot 4): with
// the controller on Automatic, a button pressed on another controller while this one has been
// quiet for half a second makes that one this player's controller.
static bool g_padFollow = true;
static uint32_t g_padQuiet = 0, g_padPrev[8];
static void NetPadFollow() {
    uint32_t d = N.localDev;
    bool quiet = d >= addr::PAD_COUNT || (PadU32(d, addr::PAD_RAW0) == 0 && PadU32(d, addr::PAD_RAW1) == 0);
    g_padQuiet = quiet ? g_padQuiet + 1 : 0;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t r = PadU32(i, addr::PAD_RAW0), fresh = r & ~g_padPrev[i] & ~0xFu;   // buttons, not directions
        g_padPrev[i] = r;
        if (g_padFollow && i != d && fresh && g_padQuiet >= 30) {
            Log("input: controller slot %u was quiet and slot %u pressed a button: this player now uses slot %u", d, i, i);
            N.localDev = i;
            g_padQuiet = 0;
            return;
        }
    }
}
static void NetFill(uint32_t T) {
    NetPadFollow();
    uint32_t d = N.localDev;
    uint32_t raw0 = d < addr::PAD_COUNT ? PadU32(d, addr::PAD_RAW0) : 0;
    uint32_t raw1 = d < addr::PAD_COUNT ? PadU32(d, addr::PAD_RAW1) : 0;
    if (int z = BootZeroInput()) {
        // Build 107: the after-match screen is up over the fight: this controller drives
        // our screen, and the game gets no input from either player (the same zeros on
        // both PCs, so the result phase runs the same while both choose).
        // 107b: in the rematch's own synced screen, this player presses A (YES on the
        // game's box) every 16 frames instead; it goes to the other PC like any input
        g_postNav = raw0;
        uint32_t s = T + InputDelayNow();
        raw0 = z == 2 && s % 16 < 2 ? 0x1000u : 0u;
        raw1 = 0;
    } else if (N.sceneVt == SCENE_BTL && PcActive()) {   // build 124: the Performance check plays by itself
        raw0 = PcScriptInput();
        raw1 = 0;
    }
    if (N.sceneVt == SCENE_BTL && T < 7200 && N.diagN < 150) {
        // Build 49: what this PC's controller reads in the first two minutes of each fight
        HWND hwnd = *reinterpret_cast<HWND*>(addr::G_HWND);
        int fg = hwnd && GetForegroundWindow() == hwnd ? 1 : 0;
        if (raw0 != N.diagRaw0 || raw1 != N.diagRaw1 || fg != N.diagFg) {
            N.diagN++;
            Log("input check: fight frame %u: this PC's controller (slot %u) reads %08X %08X, game window %s", T, d, raw0, raw1,
                fg ? "in front" : "NOT in front (DirectInput controllers then read nothing)");
            N.diagRaw0 = raw0;
            N.diagRaw1 = raw1;
            N.diagFg = fg;
        }
    }
    uint32_t s = T + InputDelayNow();
    // build 121: when the delay goes up (rollback mode -> in step), the frames in between get
    // this input too: a frame with no input would never be sent and the other game would wait forever
    if (s > N.lNewest) {
        uint32_t from = N.lNewest + 1 + 8 < s ? s - 8 : N.lNewest + 1;
        for (uint32_t f = from; f <= s; f++) {
            NetIn& x = N.lIn[f % 256];
            x.frame = f;
            x.v[0] = raw0 & PadRawMask();   // build 128: the same mask as the other player's input gets
            x.v[1] = raw1;
        }
        N.lNewest = s;
    }
    const NetIn& l = N.lIn[T % 256];
    const NetIn& r = N.rIn[T % 256];
    int me = N.side, other = 1 - N.side;
    g_pend.in[me][0] = l.frame == T ? l.v[0] : 0;
    g_pend.in[me][1] = l.frame == T ? l.v[1] : 0;
    if (r.frame == T) {
        g_pend.in[other][0] = r.v[0];
        g_pend.in[other][1] = r.v[1];
    } else {                                           // rollback: guess (the newest real input)
        g_pend.in[other][0] = g_rbLast[0];
        g_pend.in[other][1] = g_rbLast[1];
    }
}
static uint32_t HeldMask(uint32_t pad) {
    if (pad >= addr::PAD_COUNT) return 0;
    const uint8_t* held = Pad(pad) + 0x18;            // 18 "held" flags
    uint32_t m = 0;
    for (int i = 0; i < 18; i++) if (held[i]) m |= 1u << i;
    return m;
}
static void NetAfterInput(bool injected) {
    if (injected && N.sceneVt == SCENE_BTL && g_frame + 1 < 7200 && N.diagN < 150) {
        uint32_t d0 = PadOfPlayer(0), d1 = PadOfPlayer(1);
        uint32_t h0 = HeldMask(d0), h1 = HeldMask(d1);
        if (h0 != N.diagHeld0 || h1 != N.diagHeld1) {
            N.diagN++;
            Log("input check: fight frame %u: the game sees player 1 (pad %u) %05X, player 2 (pad %u) %05X; sent P1 %08X %08X, P2 %08X %08X",
                g_frame + 1, d0, h0, d1, h1, g_pend.in[0][0], g_pend.in[0][1], g_pend.in[1][0], g_pend.in[1][1]);
            N.diagHeld0 = h0;
            N.diagHeld1 = h1;
        }
    }
    if (injected) {
        N.injected = g_frame + 1;
        N.injectedConsumed = false;
    }
    NetSendInput();
}
// The update for frame g_frame is about to run (main scene).
static void NetUpdateStarts() {
    if (N.st != NS_ON) return;
    if (!N.injectedConsumed && N.injected == g_frame) N.injectedConsumed = true;
}
// Rollback: hand the other player's inputs that arrived to the engine.
static void NetFeedRb() {
    for (uint32_t f = g_rbConf; f <= N.rMax && f <= g_frame && f < g_rbConf + 250; f++) {   // (later frames: when we get there)
        const NetIn& x = N.rIn[f % 256];
        if (x.frame != f || g_cf[f % 256].frame == f) continue;
        FakePkt& q = g_fq[f % 256];
        if (q.frame == f) continue;
        q.frame = f;
        q.arrive = 0;
        q.v[0] = x.v[0];
        q.v[1] = x.v[1];
        q.delivered = false;
    }
}
static void NetRbStarting(uint32_t F) {
    // Build 123 (fix 3, friend test 22 Nov: 8-10 input waits + 13-36 one-frame clock waits in the
    // first 300 frames of every in-place rematch): the frames before rollback starts run in step,
    // so both games are at the same frame here. The PC with the higher delay now waits for the
    // delay difference once (it should run behind by that much), in every fight and rematch.
    if (N.sceneVt == SCENE_BTL && N.rDelay <= 8) {
        int behind = (int)N.myDelay - (int)N.rDelay;
        if (behind > 0) {
            Log("net: rollback starts at fight frame %u: this PC waits %d frame%s once (delays %u / %u)", F, behind, behind == 1 ? "" : "s", N.myDelay, N.rDelay);
            if (PacerActive()) PaceAddDelay(behind * 1000.0 / 60.0, false);
            else { PerfScope ps(perf::P_WAIT); Sleep((DWORD)(behind * 50 / 3)); }
        }
    }
    const NetIn& x = N.rIn[(F - 1) % 256];
    if (x.frame == F - 1) { g_rbLast[0] = x.v[0]; g_rbLast[1] = x.v[1]; }
    for (uint32_t f = 1; f < F && f + 250 > F; f++) {     // frames before the start are all real
        const NetIn& y = N.rIn[f % 256];
        if (y.frame != f) continue;
        g_cf[f % 256].frame = f;
        g_cf[f % 256].v[0] = y.v[0];
        g_cf[f % 256].v[1] = y.v[1];
    }
}
// End of a main update (also when we waited).
static void NetFrameEnd(void* scene) {
    if (N.st != NS_ON) return;
    NetFpFinalize();
    DlFrameEnd();                       // build 121
    if (N.synced && N.sceneVt == SCENE_BTL && g_frame == 300)   // build 122: how the start of the fight went
        Log("net: fight frame 300: %u one-frame waits so far in this fight; aim %+d frames ahead, now %+.2f from the aim (both views together)",
            N.timeStalls - N.timeStallsAtStart, ClockTargetQ() / 4, N.syncQ / 4.0);
    if (NetRbMode() && g_frame % 600 == 0) {
        Log("net: clock sync at frame %u: this PC %+.2f frames ahead, the other PC says %+.2f, together %+.2f; %u one-frame waits, %u stretched frames so far; rollbacks %u (deepest %u)",
            g_frame, N.aheadQ / 4.0, N.rAheadQ / 4.0, N.syncQ / 4.0, N.timeStalls, N.stretchFrames, g_rbRollbacks, g_rbMaxDepth);
        DlWindowLog();                  // build 121
    }
    // Build 41: the check before rollback found a difference. A replay that
    // went another way may have changed memory outside the saved state, so
    // stop at once (test 40 went on and crashed) and stay in step without
    // rollback for the rest of this fight.
    if (g_mode == Mode::SyncTest && !g_rbOn && g_desyncs > 0) {
        if (g_cfg.dirtyTracking && !N.noTrackRetry) {
            // Build 43: test 41b failed at once in every fight after the
            // first. Try once more with changed-memory tracking off, to tell
            // whether the tracking is the cause.
            Log("net: the check before rollback found a difference at frame %u; trying the check again with changed-memory tracking OFF", g_frame);
            SyncTestStop("difference found before rollback");
            N.noTrackRetry = true;
            N.trackingWas = 1;
            g_cfg.dirtyTracking = false;
            if (dt::active) DtDisarm();
            g_desyncs = 0;
            g_auto = 3;                  // next frame: short sync test again, then rollback
        } else {
            Log("net: the check before rollback found a difference at frame %u (tracking %s, last skipped frame: %u, waited %u times / %u ms so far); no rollback in this fight",
                g_frame, g_cfg.dirtyTracking ? "on" : "off", N.lastStallFrame, N.waited, N.waitMs);
            SyncTestStop("difference found before rollback");
            g_auto = 0;
        }
    }
    // Fights: start rollback (profile -> short sync test -> rollback) once the
    // two games are in step.
    if (scene && N.synced && N.sceneVt == SCENE_BTL && g_ncfg.rollback && !N.autoDone && g_mode == Mode::Idle && !g_auto && g_frame >= 2) {
        N.autoDone = true;
        N.noTrackRetry = false;
        if (N.trackingWas >= 0) { g_cfg.dirtyTracking = N.trackingWas != 0; N.trackingWas = -1; }
        if (g_cfg.useProfile) {
            Log("net: fight: starting the rollback setup (profile, short sync test, then rollback)");
            g_haveProfile = false;
            ProfileStart();
        } else {
            Log("net: fight: rollback starts now (fixed memory list + memory watch, no profile)");
        }
        g_auto = 3;
    }
    NetUpdateStatus();
}

// F5: pick the Steam friend to play with.
static int g_netPickIdx = -1;
static void NetPickNext() {
    if (g_ncfg.mode != 2) { NetStatus("F5 picks a Steam friend: set mode=steam in rollback.ini first"); return; }
    char err[160] = "";
    if (!net::SteamOpen(err, sizeof err)) { NetStatus("Steam: %s", err); Log("net: Steam: %s", err); return; }
    static net::Friend f[64];
    int n = net::SteamFriends(f, 64);
    if (!n) { NetStatus("no Steam friends online"); return; }
    g_netPickIdx = (g_netPickIdx + 1) % n;
    g_netPick = f[g_netPickIdx].id;
    snprintf(g_netPickName, sizeof g_netPickName, "%s", f[g_netPickIdx].name);
    NetStatus("friend %d of %d: %s%s - F4 connects, F5 next", g_netPickIdx + 1, n, g_netPickName,
              f[g_netPickIdx].inGame ? " (in this game)" : " (not in this game)");
    Log("net: F5 picked friend %s (%llu)%s", g_netPickName, (unsigned long long)g_netPick, f[g_netPickIdx].inGame ? ", in this game" : "");
}
