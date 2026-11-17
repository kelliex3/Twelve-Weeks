#ifndef T2R_MATCHMAKER_PROTOCOL_H
#define T2R_MATCHMAKER_PROTOCOL_H

#include <stdint.h>

// INCREASE WITH EVERY RELEASE, NEVER DECREASE. The server refuses clients with a lower version.
#define MM_PROTOCOL_VERSION 28
// DEV matchmaker currently 22

// Message types.
enum {
    MM_QUEUE       = 1,   // client -> server : join/refresh the queue (MMQueue)
    MM_CANCEL      = 2,   // client -> server : leave queue/lobby (no body)
    MM_QUEUED      = 3,   // server -> client : you are in the pool / searching (no body)
    MM_MATCH_FOUND = 4,   // server -> client : matched (MMMatchFound + roster)
    MM_PING        = 5,   // client -> server : heartbeat (no body)
    MM_PONG        = 6,   // server -> client : heartbeat reply (no body)
    MM_GET_CONFIG  = 7,   // client -> server : request the map config (body: [u8 version])
    MM_CONFIG      = 8,   // server -> client : the map list (variable; see format below)
    MM_READY       = 9,   // client -> server : my match level finished loading (no body)
    MM_START       = 10,  // server -> client : everyone is loaded, begin the match (no body)
    MM_ABORT       = 11,  // server -> client : match aborted (a player dropped/hung) -- to menu (no body)
    MM_LOBBY       = 12,  // server -> client : pre-match lobby state (body: MMLobby)
    MM_REJECT      = 13,  // server -> client : refused (version / invalid token) -- to error (body: MMReject)
    MM_HELLO       = 14,  // client -> server : present at the MP menu, not (yet) searching (MMHello)
    MM_AUTH        = 15,  // client -> server : authenticate with the account token (MMAuth) -- FIRST message
    MM_AUTH_OK     = 16,  // server -> client : authenticated; here is your account username (MMAuthOk)
    MM_BROADCAST   = 17,  // server -> client : admin announcement; body is the message text (no struct)
    MM_RESULT      = 18,  // client(host) -> server : match outcome; body is 1 byte (see MM_WINNER_* below)
    MM_LEADERBOARD_REQ = 19,  // client -> server : request the top-wins leaderboard (no body)
    MM_LEADERBOARD     = 20,  // server -> client : leaderboard rows ([u8 count][count*MMLeaderEntry], rank order)
    MM_ONLINE_REQ      = 21,  // client -> server : request the online player count (no body)
    MM_ONLINE          = 22,  // server -> client : online player count (body: uint32, little-endian)
    MM_LOBBY_ROSTER    = 23,  // server -> client : who else is searching alongside you
                              // Broadcast to every member of a forming lobby whenever its
                              // membership changes, so the lobby list can show real names while
                              // still searching instead of a row of SEARCHING placeholders.
                              // Body: [u8 count] then per player [u8 nameLen][name bytes].
                              // The receiver's OWN name is included; the client renders itself in
                              // slot 0 from its local username and skips the first match on name.
    MM_TAMPER          = 24,
};

enum {
    MM_WINNER_SERAPHITES = 0,   // == MM_TEAM_SERAPHITES
    MM_WINNER_WLF        = 1,   // == MM_TEAM_WLF
    MM_WINNER_DRAW       = 2,
};

// admin announcement message
#define MM_BROADCAST_MAX 240

#pragma pack(push, 1)
struct MMReject {
    uint8_t reason;          // see MM_REJECT_* below
    uint8_t serverVersion;   // the server's MM_PROTOCOL_VERSION (for version mismatches)
};
#pragma pack(pop)
static_assert(sizeof(MMReject) == 2, "MMReject layout");
enum {
    MM_REJECT_VERSION       = 1,   // wire/protocol version mismatch
    MM_REJECT_INVALID_TOKEN = 2,   // token missing / unknown / account disabled -> "INVALID TOKEN"
};

#define MM_TOKEN_LEN 64   // account token: 64 lowercase hex chars (256-bit), NOT nul-terminated on the wire

#pragma pack(push, 1)
struct MMLobby {
    uint8_t phase;       // 0 = match found, 1 = map shown, 2 = countdown
    uint8_t countdown;
};
#pragma pack(pop)
static_assert(sizeof(MMLobby) == 2, "MMLobby layout");

// Team IDs
enum {
    MM_TEAM_SERAPHITES = 0,
    MM_TEAM_WLF        = 1,
};

static inline const char* MMTeamName(uint8_t team)
{
    switch (team) {
        case MM_TEAM_SERAPHITES: return "SERAPHITES";
        case MM_TEAM_WLF:        return "WLF";
        default:                 return "?";
    }
}

#define MM_NAME_LEN   32  // player name length
#define MM_MAX_SLOTS   8  // hard ceiling on roster size. The real limit on players is
                          // kMaxNetPlayers (net-protocol.hpp), bounded by this value.
#define MM_MAX_RELAYS  4  // hard ceiling on the relay pool the client pings / reports
#define MM_PING_UNREACHABLE 0xFFFF

#pragma pack(push, 1)

typedef struct {
    uint8_t  version;            // MM_PROTOCOL_VERSION
    uint64_t clientId;           // stable per-player id (0 = unknown/legacy)
    char     name[MM_NAME_LEN];  // display name
} MMHello;                       // 41 bytes (packed)
static_assert(sizeof(MMHello) == 41, "MMHello layout");

typedef struct {
    uint8_t version;             // MM_PROTOCOL_VERSION
    char    token[MM_TOKEN_LEN]; // 64 hex chars
} MMAuth;                        // 65 bytes (packed)
static_assert(sizeof(MMAuth) == 65, "MMAuth layout");

#define MM_HWID_BLOCK_VERSION   1
#define MM_HWID_MAX_COMPONENTS  16
#define MM_HWID_HASH_LEN        32

enum {
    MM_HWID_TAG_MACHINE_GUID = 1,
    MM_HWID_TAG_SMBIOS_UUID  = 2,
    MM_HWID_TAG_DISK_SERIAL  = 3,
    MM_HWID_TAG_MAC          = 4,
};

enum {
    MM_HWID_FLAG_HYPERVISOR = 0x01,
    MM_HWID_FLAG_INCOMPLETE = 0x02,
};

typedef struct {
    uint8_t blockVersion;
    uint8_t flags;
    uint8_t count;
} MMHwidBlock;
static_assert(sizeof(MMHwidBlock) == 3, "MMHwidBlock layout");

typedef struct {
    uint8_t tag;
    uint8_t hash[MM_HWID_HASH_LEN];
} MMHwidComponent;
static_assert(sizeof(MMHwidComponent) == 33, "MMHwidComponent layout");

typedef struct {
    char username[MM_NAME_LEN];  // server owned account username
} MMAuthOk;                      // 32 bytes (packed)
static_assert(sizeof(MMAuthOk) == 32, "MMAuthOk layout");

enum {
    MM_TAMPER_DEBUGGER = 1,
    MM_TAMPER_PROCESS  = 2,
};

#define MM_TAMPER_DETAIL_LEN 64

typedef struct {
    uint8_t  kind;
    uint16_t flags;
    uint32_t pid;
    char     detail[MM_TAMPER_DETAIL_LEN];
} MMTamper;
static_assert(sizeof(MMTamper) == 71, "MMTamper layout");

typedef struct {
    uint8_t  version;            // MM_PROTOCOL_VERSION
    uint64_t clientId;           // stable per-player id (0 = unknown/legacy)
    char     name[MM_NAME_LEN];  // display name, nul-padded
    uint8_t  mode;               // game mode id
    uint8_t  map;                // map id
    uint64_t primaryWeapon;      // SID hash of the chosen primary weapon (0 = server default)
    uint64_t secondaryWeapon;    // SID hash of the chosen secondary weapon (0 = server default)
    uint16_t relayPing[MM_MAX_RELAYS]; // measured RTT (ms) to each relay, indexed to the
                                       //   MM_CONFIG relay list; MM_PING_UNREACHABLE = unknown
} MMQueue;                       // 67 bytes (packed)
static_assert(sizeof(MMQueue) == 67, "MMQueue layout");

typedef struct {
    char     relayIp[16];        // relay IPv4, dotted, nul-padded ("1.2.3.4")
    uint16_t relayPort;          // relay UDP port
    uint32_t sessionId;          // this match's session id on the relay
    uint8_t  map;                // server-selected map id (the matchmaker owns the choice)
    uint8_t  yourSlot;           // your slot within the session (0 = host)
    uint8_t  playerCount;        // number of MMRosterEntry records that follow
    uint8_t  yourSpawn;          // your assigned spawn point, 1-indexed ("to1-spawn-point-<n>"); 0 = none
    uint8_t  yourTeam;           // your team for this match (0 = SERAPHITES, 1 = WLF)
} MMMatchFound;                  // 27 bytes
static_assert(sizeof(MMMatchFound) == 27, "MMMatchFound layout");

typedef struct {
    uint8_t  slot;               // slot index (0 = host)
    char     name[MM_NAME_LEN];  // that player's name, nul-padded
    uint64_t primaryWeapon;      // that player's chosen primary weapon SID (0 = server default)
    uint64_t secondaryWeapon;    // that player's chosen secondary weapon SID (0 = server default)
    uint8_t  team;               // that player's team (0 = SERAPHITES, 1 = WLF)
} MMRosterEntry;                 // 50 bytes
static_assert(sizeof(MMRosterEntry) == 50, "MMRosterEntry layout");

#define MM_LEADERBOARD_MAX 10  // top-N rows returned / rendered
typedef struct {
    char     name[MM_NAME_LEN];  // account username, nul-padded
    uint32_t wins;
    uint32_t losses;
    float winloss_ratio;
    uint32_t elo;
} MMLeaderEntry;                 // 48 bytes (packed)
static_assert(sizeof(MMLeaderEntry) == 48, "MMLeaderEntry layout");

#pragma pack(pop)

#endif
