// Network transports for step 3c (see transport.h).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "transport.h"
#include "util.h"

using util::Log;

namespace net {

// ---------------------------------------------------------------------------
// UDP (home network test)
// ---------------------------------------------------------------------------
struct Udp : Transport {
    SOCKET s = INVALID_SOCKET;
    sockaddr_in peer{};
    bool haveAddr = false;     // we know where to send
    bool locked = false;       // only accept packets from `peer`
    bool broadcast = false;
    sockaddr_in last{};
    int port = 0;
    ~Udp() override { if (s != INVALID_SOCKET) closesocket(s); }
    bool Send(const void* p, int n) override {
        sockaddr_in to = peer;
        if (!haveAddr) {
            if (!broadcast) return false;
            to.sin_family = AF_INET;
            to.sin_port = htons((u_short)port);
            to.sin_addr.s_addr = INADDR_BROADCAST;
        }
        return sendto(s, (const char*)p, n, 0, (sockaddr*)&to, sizeof to) == n;
    }
    int Recv(void* buf, int cap) override {
        for (;;) {
            sockaddr_in from{};
            int fl = sizeof from;
            int r = recvfrom(s, (char*)buf, cap, 0, (sockaddr*)&from, &fl);
            if (r < 0) return -1;
            if (locked && (from.sin_addr.s_addr != peer.sin_addr.s_addr || from.sin_port != peer.sin_port)) continue;
            last = from;
            return r;
        }
    }
    void LockPeer() override {
        if (locked) return;
        peer = last;
        haveAddr = locked = true;
        char a[64];
        Describe(a, sizeof a);
        Log("net: home network: talking to %s from now on", a);
    }
    const char* Name() const override { return "home network (UDP)"; }
    void Describe(char* out, int n) const override {
        if (!haveAddr) { snprintf(out, n, "anyone on the home network (port %d)", port); return; }
        const uint8_t* a = (const uint8_t*)&peer.sin_addr.s_addr;
        snprintf(out, n, "%u.%u.%u.%u:%u", a[0], a[1], a[2], a[3], ntohs(peer.sin_port));
    }
};

Transport* MakeUdp(const char* peerText, int port, char* err, int errLen) {
    static bool wsa = false;
    if (!wsa) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) { snprintf(err, errLen, "Windows network start failed"); return nullptr; }
        wsa = true;
    }
    Udp* u = new Udp();
    u->port = port;
    u->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u->s == INVALID_SOCKET) { snprintf(err, errLen, "could not open a network socket (%d)", WSAGetLastError()); delete u; return nullptr; }
    BOOL yes = TRUE;
    setsockopt(u->s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof yes);
    // Build 47: big buffers (the memory survey sends bursts of packets; with
    // the 64 KB default the rest of a burst was dropped).
    int big = 4 << 20;
    setsockopt(u->s, SOL_SOCKET, SO_RCVBUF, (const char*)&big, sizeof big);
    setsockopt(u->s, SOL_SOCKET, SO_SNDBUF, (const char*)&big, sizeof big);
    u_long nb = 1;
    ioctlsocket(u->s, FIONBIO, &nb);
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_port = htons((u_short)port);
    me.sin_addr.s_addr = INADDR_ANY;
    if (bind(u->s, (sockaddr*)&me, sizeof me) != 0) {
        snprintf(err, errLen, "port %d is in use (is the game open twice?) (%d)", port, WSAGetLastError());
        delete u;
        return nullptr;
    }
    if (!peerText || !*peerText || _stricmp(peerText, "auto") == 0) {
        u->broadcast = true;
    } else {
        u->peer.sin_family = AF_INET;
        u->peer.sin_port = htons((u_short)port);
        if (inet_pton(AF_INET, peerText, &u->peer.sin_addr) != 1) {
            snprintf(err, errLen, "lan_peer \"%s\" is not an IP address like 192.168.1.20", peerText);
            delete u;
            return nullptr;
        }
        u->haveAddr = u->locked = true;
    }
    return u;
}

// ---------------------------------------------------------------------------
// Steam
// ---------------------------------------------------------------------------
// The game's steam_api.dll (Steamworks SDK of the SteamClient017 era). We
// call Steam's interfaces through their function tables, the way the SDK's
// C++ headers do. Classes returned by value come back through a hidden
// pointer (MSVC rule for member functions).
#define TC __attribute__((thiscall))
typedef void*(__cdecl* CreateInterface_t)(const char*);
typedef int(__cdecl* GetH_t)();
static void* g_client = nullptr;
static void* g_friends = nullptr;
static void* g_netw = nullptr;
static uint32_t g_scLo = 0, g_scHi = 0;     // steamclient.dll address range
static const int kChannel = 7;              // the game itself uses channel 0

static bool InSteamClient(uint32_t a) { return a >= g_scLo && a < g_scHi; }
static bool VtOk(void* obj, int entries) {
    if (!obj) return false;
    uint32_t vt = *(uint32_t*)obj;
    if (!InSteamClient(vt)) return false;
    for (int i = 0; i < entries; i++) if (!InSteamClient(((uint32_t*)vt)[i])) return false;
    return true;
}
template <typename F> static F Vf(void* obj, int i) { return (F)((void**)*(void***)obj)[i]; }

bool SteamOpen(char* err, int errLen) {
    if (g_netw) return true;
    HMODULE api = GetModuleHandleW(L"steam_api.dll");
    HMODULE sc = GetModuleHandleW(L"steamclient.dll");
    if (!api || !sc) { snprintf(err, errLen, "Steam is not loaded in the game"); return false; }
    {
        auto* dos = (IMAGE_DOS_HEADER*)sc;
        auto* nth = (IMAGE_NT_HEADERS*)((uint8_t*)sc + dos->e_lfanew);
        g_scLo = (uint32_t)sc;
        g_scHi = g_scLo + nth->OptionalHeader.SizeOfImage;
    }
    auto ci = (CreateInterface_t)GetProcAddress(api, "SteamInternal_CreateInterface");
    auto gu = (GetH_t)GetProcAddress(api, "SteamAPI_GetHSteamUser");
    auto gp = (GetH_t)GetProcAddress(api, "SteamAPI_GetHSteamPipe");
    if (!ci || !gu || !gp) { snprintf(err, errLen, "steam_api.dll is a version we don't know"); return false; }
    g_client = ci("SteamClient017");
    if (!VtOk(g_client, 17)) { snprintf(err, errLen, "SteamClient017 not available"); g_client = nullptr; return false; }
    int hu = gu(), hp = gp();
    typedef void*(TC * GetI_t)(void*, int, int, const char*);
    void* f = Vf<GetI_t>(g_client, 8)(g_client, hu, hp, "SteamFriends015");
    void* n = Vf<GetI_t>(g_client, 16)(g_client, hu, hp, "SteamNetworking005");
    if (!VtOk(f, 9)) { snprintf(err, errLen, "SteamFriends015 not available"); return false; }
    if (!VtOk(n, 8)) { snprintf(err, errLen, "SteamNetworking005 not available"); return false; }
    g_friends = f;
    g_netw = n;
    Log("net: Steam interfaces found (friends %p, networking %p)", f, n);
    return true;
}

struct FriendGameInfo { uint64_t gameId; uint32_t ip; uint16_t port, qport; uint64_t lobby; };
static const uint32_t kAppId = 989550;

int SteamFriends(Friend* out, int max) {
    if (!g_friends) return 0;
    typedef int(TC * Count_t)(void*, int);
    typedef uint64_t*(TC * ByIndex_t)(void*, uint64_t*, int, int);
    typedef int(TC * State_t)(void*, uint64_t);
    typedef const char*(TC * Name_t)(void*, uint64_t);
    typedef bool(TC * Played_t)(void*, uint64_t, FriendGameInfo*);
    const int kImmediate = 4;
    int n = Vf<Count_t>(g_friends, 3)(g_friends, kImmediate);
    int got = 0;
    for (int pass = 0; pass < 2; pass++) {          // friends in this game first
        for (int i = 0; i < n && got < max; i++) {
            uint64_t id = 0;
            Vf<ByIndex_t>(g_friends, 4)(g_friends, &id, i, kImmediate);
            if (!id) continue;
            if (Vf<State_t>(g_friends, 6)(g_friends, id) == 0) continue;   // offline
            FriendGameInfo gi{};
            bool playing = Vf<Played_t>(g_friends, 8)(g_friends, id, &gi) && (uint32_t)(gi.gameId & 0xFFFFFF) == kAppId;
            if (playing != (pass == 0)) continue;
            out[got].id = id;
            out[got].inGame = playing;
            const char* nm = Vf<Name_t>(g_friends, 7)(g_friends, id);
            snprintf(out[got].name, sizeof out[got].name, "%s", nm ? nm : "?");
            got++;
        }
    }
    return got;
}

// build 104: this player's Steam name ("" when Steam isn't there)
const char* SteamMyName() {
    char err[96];
    if (!g_friends && !SteamOpen(err, sizeof err)) return "";
    typedef const char*(TC * Name_t)(void*);
    const char* nm = Vf<Name_t>(g_friends, 0)(g_friends);   // ISteamFriends::GetPersonaName
    return nm ? nm : "";
}

const char* SteamFriendName(uint64_t id) {
    if (!g_friends) return "?";
    typedef const char*(TC * Name_t)(void*, uint64_t);
    const char* nm = Vf<Name_t>(g_friends, 7)(g_friends, id);
    return nm ? nm : "?";
}

struct SteamT : Transport {
    uint64_t peer = 0;
    uint32_t lastAccept = 0;
    bool Send(const void* p, int n) override {
        typedef bool(TC * Send_t)(void*, uint64_t, const void*, uint32_t, int, int);
        return Vf<Send_t>(g_netw, 0)(g_netw, peer, p, (uint32_t)n, 0 /*unreliable*/, kChannel);
    }
    int Recv(void* buf, int cap) override {
        typedef bool(TC * Avail_t)(void*, uint32_t*, int);
        typedef bool(TC * Read_t)(void*, void*, uint32_t, uint32_t*, uint64_t*, int);
        for (;;) {
            uint32_t size = 0;
            if (!Vf<Avail_t>(g_netw, 1)(g_netw, &size, kChannel)) return -1;
            static uint8_t big[4096];
            uint32_t got = 0;
            uint64_t from = 0;
            void* dst = size <= (uint32_t)cap ? buf : big;
            uint32_t dcap = size <= (uint32_t)cap ? (uint32_t)cap : sizeof big;
            if (!Vf<Read_t>(g_netw, 2)(g_netw, dst, dcap, &got, &from, kChannel)) return -1;
            if (from != peer || dst == big) continue;      // not our opponent, or too big: drop
            return (int)got;
        }
    }
    void Tick() override {
        uint32_t now = GetTickCount();
        if (now - lastAccept < 1000) return;
        lastAccept = now;
        typedef bool(TC * Accept_t)(void*, uint64_t);
        Vf<Accept_t>(g_netw, 3)(g_netw, peer);
    }
    const char* Name() const override { return "Steam"; }
    void Describe(char* out, int n) const override { snprintf(out, n, "%s (Steam ID %llu)", SteamFriendName(peer), (unsigned long long)peer); }
    ~SteamT() override {
        typedef bool(TC * Close_t)(void*, uint64_t, int);
        if (g_netw) Vf<Close_t>(g_netw, 5)(g_netw, peer, kChannel);
    }
};

Transport* MakeSteam(uint64_t peer, char* err, int errLen) {
    if (!SteamOpen(err, errLen)) return nullptr;
    if (!peer) { snprintf(err, errLen, "no friend picked"); return nullptr; }
    SteamT* t = new SteamT();
    t->peer = peer;
    t->Tick();
    return t;
}

} // namespace net

// ===========================================================================
// Build 100: rooms = Steam lobbies (SteamMatchMaking009, through the flat
// functions steam_api.dll exports, so no function-table guessing).
//   - Lobby data: arb=1 (ours, not the game's own lobbies), arb_build, name,
//     host, priv, rounds, timer, setlen. Only the owner (host) writes it.
//   - Asynchronous calls (create, search, join) are polled with
//     ISteamUtils::IsAPICallCompleted / GetAPICallResult.
//   - Chat messages and invites arrive as Steam callbacks; we register our own
//     small callback objects (laid out like the SDK's CCallbackBase: on
//     Windows the table holds Run(param, ioFailure, call) first, then
//     Run(param), then the size). Steam runs them inside the game's own
//     SteamAPI_RunCallbacks, on the game thread.
//   - Everything the overlay reads is copied into a RoomView under g_rlock.
// ===========================================================================
namespace net {
namespace {
typedef uint64_t SteamCall;
static void* g_mm = nullptr;      // ISteamMatchmaking
static void* g_utils = nullptr;   // ISteamUtils
static void* g_user = nullptr;    // ISteamUser
static uint64_t g_me = 0;
static bool g_roomsTried = false, g_roomsOk = false;
static CRITICAL_SECTION g_rlock;
static bool g_rlockInit = false;
struct RLock { RLock() { EnterCriticalSection(&g_rlock); } ~RLock() { LeaveCriticalSection(&g_rlock); } };

// flat functions (cdecl, first argument = the interface)
#define FL(name, ret, ...) typedef ret(__cdecl* name##_t)(__VA_ARGS__); static name##_t p##name = nullptr;
FL(CreateLobby, SteamCall, void*, int, int)
FL(RequestLobbyList, SteamCall, void*)
FL(AddStringFilter, void, void*, const char*, const char*, int)
FL(AddDistanceFilter, void, void*, int)
FL(AddResultCountFilter, void, void*, int)
FL(GetLobbyByIndex, uint64_t, void*, int)
FL(JoinLobby, SteamCall, void*, uint64_t)
FL(LeaveLobby, void, void*, uint64_t)
FL(InviteUserToLobby, bool, void*, uint64_t, uint64_t)
FL(GetNumLobbyMembers, int, void*, uint64_t)
FL(GetLobbyMemberByIndex, uint64_t, void*, uint64_t, int)
FL(GetLobbyData, const char*, void*, uint64_t, const char*)
FL(SetLobbyData, bool, void*, uint64_t, const char*, const char*)
FL(SendLobbyChatMsg, bool, void*, uint64_t, const void*, int)
FL(GetLobbyChatEntry, int, void*, uint64_t, int, uint64_t*, void*, int, int*)
FL(RequestLobbyData, bool, void*, uint64_t)
FL(GetLobbyMemberLimit, int, void*, uint64_t)
FL(SetLobbyMemberLimit, bool, void*, uint64_t, int)
FL(SetLobbyType, bool, void*, uint64_t, int)
FL(GetLobbyOwner, uint64_t, void*, uint64_t)
FL(IsAPICallCompleted, bool, void*, SteamCall, bool*)
FL(GetAPICallResult, bool, void*, SteamCall, void*, int, int, bool*)
FL(GetSteamID, uint64_t, void*)
FL(RegisterCallback, void, void*, int)
#undef FL

// requests from the overlay (under g_rlock)
static struct {
    bool create = false, search = false, leave = false, friends = false;
    uint64_t join = 0, invite = 0;
    RoomSettings set{}, change{};
    bool changeReq = false;
    char say[8][200];
    int nSay = 0;
} RQ;
// state (game thread; copied out under g_rlock)
static RoomView RV;
static SteamCall g_call = 0;
static int g_callKind = 0;           // 1 create, 2 search, 3 join
static uint32_t g_callStart = 0;
static uint64_t g_lastMembers[ROOM_MAX_MEMBERS];
static int g_nLast = 0;

static void ChatAdd(uint64_t from, const char* name, const char* text, bool system) {
    // caller holds g_rlock
    if (RV.nChat == ROOM_CHAT_LINES) { memmove(&RV.chat[0], &RV.chat[1], sizeof RV.chat[0] * (ROOM_CHAT_LINES - 1)); RV.nChat--; }
    ChatLine& c = RV.chat[RV.nChat++];
    c.seq = ++RV.chatSeq;
    c.from = from;
    c.system = system;
    snprintf(c.name, sizeof c.name, "%s", name ? name : "");
    snprintf(c.text, sizeof c.text, "%s", text ? text : "");
}

// ---- callbacks ----
struct CbObj;
typedef void (*CbFn)(const void* param);
struct CbObj { void** vt; uint8_t flags; int id; CbFn fn; int size; };
static void __attribute__((thiscall)) CbRunCall(CbObj* self, void* p, bool, SteamCall) { if (self->fn && p) self->fn(p); }
static void __attribute__((thiscall)) CbRun(CbObj* self, void* p) { if (self->fn && p) self->fn(p); }
static int __attribute__((thiscall)) CbSize(CbObj* self) { return self->size; }
static void* g_cbVt[3] = {(void*)CbRunCall, (void*)CbRun, (void*)CbSize};

#pragma pack(push, 8)
struct LobbyCreated { int result; uint64_t lobby; };                                   // 513
struct LobbyEnter { uint64_t lobby; uint32_t perms; bool locked; uint32_t response; };  // 504
struct LobbyMatchList { uint32_t count; };                                              // 510
struct LobbyChatMsg { uint64_t lobby, user; uint8_t type; uint32_t chatId; };           // 507
struct LobbyInviteCb { uint64_t user, lobby, gameId; };                                 // 503
struct JoinRequested { uint64_t lobby, user; };                                         // 333
struct LobbyKicked { uint64_t lobby, admin; uint8_t disconnected; };                    // 512
#pragma pack(pop)

static void OnChat(const void* pv) {
    const LobbyChatMsg* m = (const LobbyChatMsg*)pv;
    if (!RV.id || m->lobby != RV.id || !pGetLobbyChatEntry) return;
    char buf[512];
    uint64_t who = 0;
    int type = 0;
    int n = pGetLobbyChatEntry(g_mm, m->lobby, (int)m->chatId, &who, buf, sizeof buf - 1, &type);
    static int logged = 0;
    if (logged < 5) { logged++; Log("room: chat callback (id %u, %d bytes, type %d)", m->chatId, n, type); }
    if (n <= 0 || type != 1) return;   // 1 = chat message
    buf[n] = 0;
    char name[48];
    snprintf(name, sizeof name, "%s", SteamFriendName(who));
    RLock l;
    ChatAdd(who, name, buf, false);
}
static void OnInvite(const void* pv) {
    const LobbyInviteCb* m = (const LobbyInviteCb*)pv;
    Log("room: invite from %llu to room %llu", (unsigned long long)m->user, (unsigned long long)m->lobby);
    RLock l;
    RV.haveInvite = true;
    RV.invite.lobby = m->lobby;
    RV.invite.from = m->user;
    RV.invite.ms = GetTickCount();
    snprintf(RV.invite.name, sizeof RV.invite.name, "%s", SteamFriendName(m->user));
}
static void OnJoinRequested(const void* pv) {
    // the player accepted an invite (or picked "Join game") in the Steam overlay
    const JoinRequested* m = (const JoinRequested*)pv;
    Log("room: Steam asks to join room %llu (friend %llu)", (unsigned long long)m->lobby, (unsigned long long)m->user);
    RLock l;
    RQ.join = m->lobby;
    if (m->lobby != RV.id) { RV.state = ROOM_JOINING; RV.err[0] = 0; }
}
static void OnKicked(const void* pv) {
    const LobbyKicked* m = (const LobbyKicked*)pv;
    if (m->lobby != RV.id) return;
    Log("room: removed from the room by Steam");
    RLock l;
    RV.state = ROOM_FAILED;
    snprintf(RV.err, sizeof RV.err, "You were removed from the room");
    RV.id = 0;
}
static CbObj g_cbs[5];
static void Reg(int i, int id, CbFn fn, int size) {
    g_cbs[i] = CbObj{g_cbVt, 0, id, fn, size};
    pRegisterCallback(&g_cbs[i], id);
}

static uint32_t g_roomsTryMs = 0;
static bool g_roomsDead = false;
static bool RoomsOpen() {
    if (g_roomsOk) return true;
    if (g_roomsDead) return false;
    if (g_roomsTried && GetTickCount() - g_roomsTryMs < 5000) return false;   // Steam may come up later: try again every 5 s
    bool first = !g_roomsTried;
    g_roomsTried = true;
    g_roomsTryMs = GetTickCount();
    char err[160];
    if (!SteamOpen(err, sizeof err)) { if (first) Log("room: no Steam yet: %s", err); return false; }
    HMODULE api = GetModuleHandleW(L"steam_api.dll");
    auto gu = (GetH_t)GetProcAddress(api, "SteamAPI_GetHSteamUser");
    auto gp = (GetH_t)GetProcAddress(api, "SteamAPI_GetHSteamPipe");
    int hu = gu(), hp = gp();
    typedef void*(TC * GetI_t)(void*, int, int, const char*);
    typedef void*(TC * GetU_t)(void*, int, const char*);
    g_user = Vf<GetI_t>(g_client, 5)(g_client, hu, hp, "SteamUser019");
    g_utils = Vf<GetU_t>(g_client, 9)(g_client, hp, "SteamUtils009");
    g_mm = Vf<GetI_t>(g_client, 10)(g_client, hu, hp, "SteamMatchMaking009");
    bool ok = g_user && g_utils && g_mm;
#define GP(var, name) var = (decltype(var))GetProcAddress(api, name); ok = ok && var;
    GP(pCreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby")
    GP(pRequestLobbyList, "SteamAPI_ISteamMatchmaking_RequestLobbyList")
    GP(pAddStringFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter")
    GP(pAddDistanceFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter")
    GP(pAddResultCountFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter")
    GP(pGetLobbyByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex")
    GP(pJoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby")
    GP(pLeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby")
    GP(pInviteUserToLobby, "SteamAPI_ISteamMatchmaking_InviteUserToLobby")
    GP(pGetNumLobbyMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers")
    GP(pGetLobbyMemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex")
    GP(pGetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData")
    GP(pSetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData")
    GP(pSendLobbyChatMsg, "SteamAPI_ISteamMatchmaking_SendLobbyChatMsg")
    GP(pGetLobbyChatEntry, "SteamAPI_ISteamMatchmaking_GetLobbyChatEntry")
    GP(pRequestLobbyData, "SteamAPI_ISteamMatchmaking_RequestLobbyData")
    GP(pGetLobbyMemberLimit, "SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit")
    GP(pSetLobbyMemberLimit, "SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit")
    GP(pSetLobbyType, "SteamAPI_ISteamMatchmaking_SetLobbyType")
    GP(pGetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner")
    GP(pIsAPICallCompleted, "SteamAPI_ISteamUtils_IsAPICallCompleted")
    GP(pGetAPICallResult, "SteamAPI_ISteamUtils_GetAPICallResult")
    GP(pGetSteamID, "SteamAPI_ISteamUser_GetSteamID")
    GP(pRegisterCallback, "SteamAPI_RegisterCallback")
#undef GP
    if (!ok) { Log("room: Steam lobby functions not found (user %p utils %p matchmaking %p)", g_user, g_utils, g_mm); g_roomsDead = true; return false; }
    g_me = pGetSteamID(g_user);
    Reg(0, 507, OnChat, sizeof(LobbyChatMsg));
    Reg(1, 503, OnInvite, sizeof(LobbyInviteCb));
    Reg(2, 333, OnJoinRequested, sizeof(JoinRequested));
    Reg(3, 512, OnKicked, sizeof(LobbyKicked));
    Log("room: Steam lobbies ready (me %llu)", (unsigned long long)g_me);
    g_roomsOk = true;
    return true;
}

static void WriteRoomData(uint64_t lobby, const RoomSettings& s, int build) {
    char v[32];
    pSetLobbyData(g_mm, lobby, "arb", "1");
    snprintf(v, sizeof v, "%d", build);
    pSetLobbyData(g_mm, lobby, "arb_build", v);
    pSetLobbyData(g_mm, lobby, "name", s.name);
    pSetLobbyData(g_mm, lobby, "host", SteamFriendName(g_me));
    pSetLobbyData(g_mm, lobby, "priv", s.priv ? "1" : "0");
    snprintf(v, sizeof v, "%d", s.rounds); pSetLobbyData(g_mm, lobby, "rounds", v);
    snprintf(v, sizeof v, "%d", s.timer); pSetLobbyData(g_mm, lobby, "timer", v);
    snprintf(v, sizeof v, "%d", s.setLen); pSetLobbyData(g_mm, lobby, "setlen", v);
}
static int DataInt(uint64_t lobby, const char* k, int def) {
    const char* v = pGetLobbyData(g_mm, lobby, k);
    return v && *v ? atoi(v) : def;
}
static void ReadRoom(uint64_t lobby) {
    // caller holds g_rlock
    const char* nm = pGetLobbyData(g_mm, lobby, "name");
    snprintf(RV.set.name, sizeof RV.set.name, "%s", nm && *nm ? nm : "Room");
    RV.set.priv = DataInt(lobby, "priv", 0) != 0;
    RV.set.rounds = DataInt(lobby, "rounds", 2);
    RV.set.timer = DataInt(lobby, "timer", 2);
    RV.set.setLen = DataInt(lobby, "setlen", 0);
    RV.set.size = pGetLobbyMemberLimit(g_mm, lobby);
    RV.owner = pGetLobbyOwner(g_mm, lobby);
    int n = pGetNumLobbyMembers(g_mm, lobby);
    if (n > ROOM_MAX_MEMBERS) n = ROOM_MAX_MEMBERS;
    uint64_t now[ROOM_MAX_MEMBERS];
    for (int i = 0; i < n; i++) {
        uint64_t id = pGetLobbyMemberByIndex(g_mm, lobby, i);
        now[i] = id;
        RoomMember& m = RV.members[i];
        m.id = id;
        m.host = id == RV.owner;
        m.me = id == g_me;
        snprintf(m.name, sizeof m.name, "%s", SteamFriendName(id));
    }
    // who came and went, for the chat
    if (g_nLast > 0 || n > 0) {
        for (int i = 0; i < n; i++) {
            bool was = false;
            for (int j = 0; j < g_nLast; j++) was |= g_lastMembers[j] == now[i];
            if (!was && g_nLast > 0) { char s[96]; snprintf(s, sizeof s, "%s joined the room", RV.members[i].name); ChatAdd(now[i], "", s, true); }
        }
        for (int j = 0; j < g_nLast; j++) {
            bool still = false;
            for (int i = 0; i < n; i++) still |= g_lastMembers[j] == now[i];
            if (!still) { char s[96]; snprintf(s, sizeof s, "%s left the room", SteamFriendName(g_lastMembers[j])); ChatAdd(g_lastMembers[j], "", s, true); }
        }
    }
    memcpy(g_lastMembers, now, sizeof(uint64_t) * n);
    g_nLast = n;
    RV.nMembers = n;
}
static void EnterRoom(uint64_t lobby, int build, bool created) {
    // caller holds g_rlock
    RV.id = lobby;
    RV.state = ROOM_IN;
    RV.err[0] = 0;
    RV.nChat = 0;
    g_nLast = 0;
    if (created) WriteRoomData(lobby, RQ.set, build);
    ReadRoom(lobby);
    char s[96];
    snprintf(s, sizeof s, created ? "You opened the room" : "You joined the room");
    ChatAdd(g_me, "", s, true);
    Log("room: %s room %llu \"%s\" (%d of %d)", created ? "opened" : "joined", (unsigned long long)lobby, RV.set.name, RV.nMembers, RV.set.size);
}
static const char* EnterError(uint32_t r) {
    switch (r) {
    case 2: return "That room doesn't exist any more";
    case 3: return "You can't join that room";
    case 4: return "That room is full";
    case 6: return "You are banned from that room";
    case 7: return "Your Steam account is limited";
    default: return "Could not join the room";
    }
}
} // namespace

void RoomTick(int build) {
    if (!g_rlockInit) { InitializeCriticalSection(&g_rlock); g_rlockInit = true; }
    bool ok = RoomsOpen();
    RLock l;
    RV.steam = ok;
    RV.me = g_me;
    if (!ok) {   // build 101: a request made without Steam fails at once (the screens show it)
        if (RQ.create || RQ.join) {
            RQ.create = false; RQ.join = 0;
            RV.state = ROOM_FAILED;
            snprintf(RV.err, sizeof RV.err, "Steam isn't running");
        }
        RQ.leave = false;
        return;
    }
    // ---- start what was asked ----
    if (RQ.leave) {
        RQ.leave = false;
        if (RV.id) { pLeaveLobby(g_mm, RV.id); Log("room: left room %llu", (unsigned long long)RV.id); }
        RV.id = 0; RV.state = ROOM_NONE; RV.nMembers = 0; RV.nChat = 0; g_nLast = 0;
        if (g_callKind == 1 || g_callKind == 3) g_call = 0, g_callKind = 0;
    }
    if (!g_call && RQ.create) {
        RQ.create = false;
        if (RV.id) { pLeaveLobby(g_mm, RV.id); RV.id = 0; }
        int size = RQ.set.size < 2 ? 2 : RQ.set.size > 8 ? 8 : RQ.set.size;
        g_call = pCreateLobby(g_mm, RQ.set.priv ? 0 : 2, size);   // 0 private (invite only), 2 public
        g_callKind = 1; g_callStart = GetTickCount();
        RV.state = ROOM_CREATING; RV.err[0] = 0;
        Log("room: creating a %s room for %d", RQ.set.priv ? "private" : "public", size);
    }
    if (!g_call && RQ.join) {
        uint64_t id = RQ.join;
        RQ.join = 0;
        if (id != RV.id) {
            if (RV.id) { pLeaveLobby(g_mm, RV.id); RV.id = 0; }
            g_call = pJoinLobby(g_mm, id);
            g_callKind = 3; g_callStart = GetTickCount();
            RV.state = ROOM_JOINING; RV.err[0] = 0;
            RV.haveInvite = false;
            Log("room: joining room %llu", (unsigned long long)id);
        }
    }
    if (!g_call && RQ.search) {
        RQ.search = false;
        pAddStringFilter(g_mm, "arb", "1", 0);
        char v[16];
        snprintf(v, sizeof v, "%d", build);
        pAddStringFilter(g_mm, "arb_build", v, 0);
        pAddDistanceFilter(g_mm, 3);            // worldwide
        pAddResultCountFilter(g_mm, ROOM_MAX_LIST);
        g_call = pRequestLobbyList(g_mm);
        g_callKind = 2; g_callStart = GetTickCount();
        RV.searching = true;
    }
    if (RQ.invite && RV.id) {
        bool sent = pInviteUserToLobby(g_mm, RV.id, RQ.invite);
        char s[120];
        snprintf(s, sizeof s, sent ? "Invite sent to %s" : "Could not invite %s", SteamFriendName(RQ.invite));
        ChatAdd(g_me, "", s, true);
        Log("room: invite to %llu %s", (unsigned long long)RQ.invite, sent ? "sent" : "failed");
        RQ.invite = 0;
    }
    if (RQ.friends) { RQ.friends = false; RV.nFriends = SteamFriends(RV.friends, 64); }
    if (RV.id && RV.state == ROOM_IN) {
        for (int i = 0; i < RQ.nSay; i++) pSendLobbyChatMsg(g_mm, RV.id, RQ.say[i], (int)strlen(RQ.say[i]) + 1);
        RQ.nSay = 0;
        if (RQ.changeReq && RV.owner == g_me) {
            RoomSettings s = RQ.change;
            snprintf(s.name, sizeof s.name, "%s", RV.set.name);
            WriteRoomData(RV.id, s, build);
            if (s.size >= RV.nMembers && s.size >= 2 && s.size <= 8) pSetLobbyMemberLimit(g_mm, RV.id, s.size);
            pSetLobbyType(g_mm, RV.id, s.priv ? 0 : 2);
        }
        RQ.changeReq = false;
    } else RQ.nSay = 0;
    // ---- finish what is running ----
    if (g_call) {
        bool failed = false;
        if (pIsAPICallCompleted(g_utils, g_call, &failed)) {
            SteamCall c = g_call;
            int kind = g_callKind;
            g_call = 0; g_callKind = 0;
            if (kind == 1) {
                LobbyCreated r{};
                if (pGetAPICallResult(g_utils, c, &r, sizeof r, 513, &failed) && !failed && r.result == 1 && r.lobby) EnterRoom(r.lobby, build, true);
                else { RV.state = ROOM_FAILED; snprintf(RV.err, sizeof RV.err, "Steam could not open the room (code %d)", r.result); Log("room: create failed (%d)", r.result); }
            } else if (kind == 3) {
                LobbyEnter r{};
                if (pGetAPICallResult(g_utils, c, &r, sizeof r, 504, &failed) && !failed && r.response == 1 && r.lobby) {
                    if (DataInt(r.lobby, "arb", 0) != 1) { pLeaveLobby(g_mm, r.lobby); RV.state = ROOM_FAILED; snprintf(RV.err, sizeof RV.err, "That is not a room of this mod"); }
                    else if (DataInt(r.lobby, "arb_build", 0) != build) {
                        pLeaveLobby(g_mm, r.lobby);
                        RV.state = ROOM_FAILED;
                        snprintf(RV.err, sizeof RV.err, "That room uses build %d of the mod, you have build %d", DataInt(r.lobby, "arb_build", 0), build);
                    } else EnterRoom(r.lobby, build, false);
                } else { RV.state = ROOM_FAILED; snprintf(RV.err, sizeof RV.err, "%s", EnterError(r.response)); Log("room: join failed (%u)", r.response); }
            } else if (kind == 2) {
                LobbyMatchList r{};
                RV.nRooms = 0;
                if (pGetAPICallResult(g_utils, c, &r, sizeof r, 510, &failed) && !failed) {
                    for (uint32_t i = 0; i < r.count && RV.nRooms < ROOM_MAX_LIST; i++) {
                        uint64_t id = pGetLobbyByIndex(g_mm, (int)i);
                        if (!id) continue;
                        RoomInfo& ri = RV.rooms[RV.nRooms++];
                        ri.id = id;
                        const char* nm = pGetLobbyData(g_mm, id, "name");
                        const char* h = pGetLobbyData(g_mm, id, "host");
                        snprintf(ri.name, sizeof ri.name, "%s", nm && *nm ? nm : "Room");
                        snprintf(ri.host, sizeof ri.host, "%s", h && *h ? h : "?");
                        ri.members = pGetNumLobbyMembers(g_mm, id);
                        ri.limit = pGetLobbyMemberLimit(g_mm, id);
                    }
                }
                RV.searching = false;
                RV.searchSeq++;
                Log("room: search found %d room(s)", RV.nRooms);
            }
        } else if (GetTickCount() - g_callStart > 20000) {
            Log("room: Steam did not answer (kind %d)", g_callKind);
            if (g_callKind == 2) RV.searching = false, RV.searchSeq++;
            else { RV.state = ROOM_FAILED; snprintf(RV.err, sizeof RV.err, "Steam did not answer, try again"); }
            g_call = 0; g_callKind = 0;
        }
    }
    if (RV.id && RV.state == ROOM_IN) ReadRoom(RV.id);
}

void RoomGet(RoomView* out) {
    if (!g_rlockInit) { memset(out, 0, sizeof *out); return; }
    RLock l;
    *out = RV;
}
#define RQLOCK if (!g_rlockInit) return; RLock l;
// build 101: the asked-for state shows straight away (the screens never see a frame of "no room")
void RoomCreate(const RoomSettings& s) { RQLOCK RQ.set = s; RQ.create = true; RV.state = ROOM_CREATING; RV.err[0] = 0; }
void RoomSearch() { RQLOCK RQ.search = true; RV.searching = true; }
void RoomJoin(uint64_t lobby) { RQLOCK RQ.join = lobby; if (lobby != RV.id) { RV.state = ROOM_JOINING; RV.err[0] = 0; } }
void RoomLeave() { RQLOCK RQ.leave = true; RQ.create = false; RQ.join = 0; RV.state = ROOM_NONE; }
void RoomSay(const char* text) { RQLOCK if (RQ.nSay < 8 && text && *text) snprintf(RQ.say[RQ.nSay++], 200, "%s", text); }
void RoomInviteFriend(uint64_t f) { RQLOCK RQ.invite = f; }
void RoomRefreshFriends() { RQLOCK RQ.friends = true; }
void RoomDropInvite() { RQLOCK RV.haveInvite = false; }
void RoomChange(const RoomSettings& s) { RQLOCK RQ.change = s; RQ.changeReq = true; }
#undef RQLOCK
} // namespace net
