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
struct RoomMember {
    uint64_t id; char name[48]; bool host, me;
    // build 112: tables (Steam lobby member data, see transport.cpp)
    int seat;                      // -1 standing, else table * 2 + side (0 = 1P, 1 = 2P)
    bool ready, playing, standin;  // standin: the one-PC test player (not a Steam user)
    bool hasSetup;                 // their My setup is known:
    uint8_t chara, color, stage, assist[3];   // chara / stage 0xFF = random
    int game, wins[2];             // while playing: game number in the set, games won by 1P / 2P
};
struct ChatLine { uint32_t seq; uint64_t from; char name[48]; char text[200]; bool system; };
struct RoomSettings { char name[48]; bool priv; int size, rounds, timer, setLen; };   // timer 0-3, setLen 0 = no limit
struct RoomInvite { uint64_t lobby, from; char name[48]; uint32_t ms; };
struct RoomView {
    bool steam;                    // Steam found
    int state;                     // ROOM_*
    char err[128];
    uint64_t id, owner, me;
    RoomSettings set;
    char code[8];                  // build 114: private rooms: the code to join with ("" = none)
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
    int mySeat;                    // build 112: -1 standing
    bool myReady;
};
void RoomTick(int build);          // game thread, every frame
void SetGameHash(uint32_t h);      // build 119: the game code's hash (rooms check it when you join)
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
// ---- Build 112: tables and seats ----
static const uint64_t ROOM_STANDIN_ID = 1;   // the one-PC test player's id
void RoomSit(int seat);                      // -1 stands up (also drops READY)
void RoomSetReady(bool on);
void RoomSetMyData(const char* key, const char* val);   // other member data: "su" setup, "play", "sc" score
void RoomStandIn(bool on);                   // [dev] room_standin=1: a test player sits across from you
void RoomSystemLine(const char* text);       // a grey line in the room chat, only on this PC
// ---- Build 113 ----
void RoomKick(uint64_t member);
// ---- Build 114 ----
void RoomJoinCode(const char* code);         // find the private room with this code and join it              // host only: that player's game leaves the room and can't come back

} // namespace net
