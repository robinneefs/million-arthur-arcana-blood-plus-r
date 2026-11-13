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
