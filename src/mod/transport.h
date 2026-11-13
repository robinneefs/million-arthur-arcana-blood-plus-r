// Network transports for step 3c: how packets get to the other PC.
//
//   UDP   : test-only direct connection over the home network (LAN).
//   Steam : Steam peer-to-peer packets (SteamNetworking005), on a channel of
//           our own so they never mix with the game's own online packets.
//
// The loopback test lives in the session code (it needs to understand the
// packets to play the other side).
#pragma once
#include <cstdint>

namespace net {

struct Transport {
    virtual ~Transport() {}
    virtual bool Send(const void* p, int n) = 0;
    // Returns the packet size, or -1 when nothing is waiting.
    virtual int Recv(void* buf, int cap) = 0;
    virtual const char* Name() const = 0;
    // Called about once per frame (keeps a Steam session accepted, etc.).
    virtual void Tick() {}
    // A short description of who we are connected to, for the log.
    virtual void Describe(char* out, int n) const = 0;
    // UDP auto mode: from now on, talk only to whoever sent the last packet.
    virtual void LockPeer() {}
};

// peer = "auto" (find the other PC with a broadcast) or an IPv4 address.
Transport* MakeUdp(const char* peer, int port, char* err, int errLen);

// ---- Steam ----
bool SteamOpen(char* err, int errLen);     // find the game's Steam interfaces
struct Friend { uint64_t id; char name[64]; bool inGame; };
// Online friends, the ones playing this game first. Returns how many.
int SteamFriends(Friend* out, int max);
const char* SteamFriendName(uint64_t id);
Transport* MakeSteam(uint64_t peer, char* err, int errLen);

} // namespace net
