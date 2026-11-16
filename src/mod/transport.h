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
const char* SteamMyName();            // build 104: own Steam name, "" without Steam
Transport* MakeSteam(uint64_t peer, char* err, int errLen);

// ---- Build 100: rooms (Steam lobbies) ----
// All Steam calls run on the game thread in RoomTick(); the overlay only asks
// (Room* requests) and reads a copy (RoomGet). Nothing here waits.
enum { ROOM_NONE, ROOM_CREATING, ROOM_JOINING, ROOM_IN, ROOM_FAILED };
static const int ROOM_MAX_MEMBERS = 8, ROOM_MAX_LIST = 40, ROOM_CHAT_LINES = 60;
struct RoomInfo { uint64_t id; char name[48]; char host[48]; int members, limit; };
struct RoomMember { uint64_t id; char name[48]; bool host, me; };
struct ChatLine { uint32_t seq; uint64_t from; char name[48]; char text[200]; bool system; };
struct RoomSettings { char name[48]; bool priv; int size, rounds, timer, setLen; };   // timer 0-3, setLen 0 = no limit
struct RoomInvite { uint64_t lobby, from; char name[48]; uint32_t ms; };
struct RoomView {
    bool steam;                    // Steam found
    int state;                     // ROOM_*
    char err[128];
    uint64_t id, owner, me;
    RoomSettings set;
    int nMembers;
    RoomMember members[ROOM_MAX_MEMBERS];
    bool searching;
    uint32_t searchSeq;            // goes up when a search finished
    int nRooms;
    RoomInfo rooms[ROOM_MAX_LIST];
    uint32_t chatSeq;              // seq of the newest chat line
    int nChat;
    ChatLine chat[ROOM_CHAT_LINES];
    bool haveInvite;
    RoomInvite invite;
    int nFriends;
    Friend friends[64];
};
void RoomTick(int build);          // game thread, every frame
void RoomGet(RoomView* out);       // a copy, any thread
void RoomCreate(const RoomSettings& s);
void RoomSearch();
void RoomJoin(uint64_t lobby);
void RoomLeave();
void RoomSay(const char* text);
void RoomInviteFriend(uint64_t friendId);
void RoomRefreshFriends();
void RoomDropInvite();
void RoomChange(const RoomSettings& s);   // host only: name stays, the rest is updated

} // namespace net
