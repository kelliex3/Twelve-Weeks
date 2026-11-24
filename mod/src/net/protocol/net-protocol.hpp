#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

#ifndef NET_PROTOCOL_HAS_STRINGID
using NetSid64 = uint64_t;
#endif

constexpr uint32_t kNetMagic = 0x4F523254;   // 'T2RO'
// was tools/features/slots8.py (8-player / 4v4 support). The shipped base binary was compiled
// with kMaxNetPlayers = 4, so slots8.py relocated the 4-entry tables and unrolled loops to 8.
// The source is already driven entirely by kMaxNetPlayers, so the port is just the constants:
// 8 net slots and the roster/protocol bump (0x29 -> 0x2A) that keeps a 4-slot build from
// parsing an 8-slot roster. Rebuild with -DTO1_SLOTS4=1 for a 4-player client.
#ifdef TO1_SLOTS4
constexpr int      kMaxNetPlayers = 4;
constexpr uint16_t kProtocolVersion = 41;
#else
constexpr int      kMaxNetPlayers = 8;
constexpr uint16_t kProtocolVersion = 42;
#endif
constexpr uint16_t kNetDefaultPort = 7777;
constexpr int      kMaxAnimCmdDwords = 1280;
constexpr int      kNetNameLen = 24;           // player display name
constexpr int      kNetLookLen = 40;           // look/model string ("marlene-player")
constexpr uint8_t  kInvalidSlot = 0xFF;
constexpr uint8_t  kBroadcastSlot = 0xFF;         // event target: everyone
constexpr float    kNetTimeoutSeconds = 5.0f;         // silence -> peer dropped
constexpr float    kHelloResendSeconds = 0.5f;         // client join retry
constexpr float    kJoinGiveUpSeconds = 10.0f;
constexpr float    kEventResendSeconds = 0.1f;         // reliable event retry
constexpr float    kEventExpireSeconds = 5.0f;         // give up resending after
constexpr int      kMaxPerks = 16;

enum class NetPacketType : uint8_t {
    Hello = 1,   // client -> host : "let me in" (HelloPayload)
    Welcome = 2,   // host -> client : "you are slot N" (RosterPayload, yourSlot set)
    Reject = 3,   // host -> client : version mismatch / server full (RejectPayload)
    Roster = 4,   // host -> all    : membership changed (RosterPayload)
    State = 5,   // both ways      : PlayerNetState (+ embedded reliable events)
    Goodbye = 6,   // either way     : clean disconnect
    AnimCmd = 7,   // both ways      : AnimCmdPayload -- the sender's captured AnimCmdList
};

enum class NetRejectReason : uint8_t {
    VersionMismatch = 1,
    ServerFull = 2,
};

#pragma pack(push, 1)

struct NetPacketHeader {
    uint32_t magic;        // kNetMagic
    uint16_t version;      // kProtocolVersion
    uint8_t  type;         // NetPacketType
    uint8_t  senderSlot;
    uint32_t sequence;
};
static_assert(sizeof(NetPacketHeader) == 12, "header layout");

struct HelloPayload {
    char name[kNetNameLen];
    char look[kNetLookLen];
};
static_assert(sizeof(HelloPayload) == 64, "HelloPayload layout");

constexpr uint8_t kTeamUnknown = 0xFF;

struct RosterEntry {
    uint8_t slot;
    uint8_t active;                 // 1 = occupied
    uint8_t team;                   // matchmaker team (0=Seraphites, 1=WLF; kTeamUnknown=unresolved).
    // Host-authoritative: the host stamps it from its complete match
    // roster, so no client depends on its own private matchmaker copy.
    char    name[kNetNameLen];
    char    look[kNetLookLen];
};
static_assert(sizeof(RosterEntry) == 67, "RosterEntry layout");

struct RosterPayload {
    uint8_t     yourSlot;           // only meaningful in Welcome; kInvalidSlot otherwise
    uint8_t     count;              // entries used
    RosterEntry players[kMaxNetPlayers];
};
static_assert(sizeof(RosterPayload) == 270, "RosterPayload layout (grows with kMaxNetPlayers)");

struct RejectPayload {
    uint8_t  reason;                // NetRejectReason
    uint16_t hostVersion;
};
static_assert(sizeof(RejectPayload) == 3, "RejectPayload layout");

struct PlayerNetState {
    float posX, posY, posZ;
    float rotX, rotY, rotZ, rotW;
    uint8_t isAiming;
    uint8_t isCrouched;
    uint8_t isProne;
    uint8_t isSprinting;
    uint8_t isJumping;
    uint8_t isFiring;
    uint8_t isReloading;
    uint8_t inMelee;
    uint8_t inDebugFly;
    uint8_t inBackpack;
    uint8_t isMnk;
    // Weapon-IK feather fade, 0..255 == 0..1. The engine's gate for two-handed weapon
    // grip IK: 0 while the weapon is stowed, ramps across draw/holster, 1 with it out.
    // Read off the local player's own arm controller (engine::GetWeaponIkFade) and
    // replayed onto the puppet as the weapon-ik-feather MASTER weight -- the puppet
    // cannot derive it itself (our replay replaces its anim-state layer walk wholesale,
    // so ND's own NPC emitter never runs and the IK is never applied).
    uint8_t weaponIk;
    uint8_t aimSideLeft;
    NetSid64 weaponId;
    NetSid64 meleeAction;
    uint32_t shotCount;
    float    health;
    float camPosX, camPosY, camPosZ;
    float camRotX, camRotY, camRotZ, camRotW;
    float    animSpine[20];
    uint16_t eventBytes;
    uint8_t  eventBuf[1024];
};
static_assert(offsetof(PlayerNetState, eventBuf) == 175, "PlayerNetState fixed-part layout");
static_assert(sizeof(PlayerNetState) == 1199, "PlayerNetState layout");

inline size_t NetStateWireSize(uint16_t eventBytes)
{
    return offsetof(PlayerNetState, eventBuf) + eventBytes;
}


constexpr int kMaxAnimCmdBytes = 4068;
struct AnimCmdPayload {
    uint32_t skelId; // sender's ArtItemSkeleton id (*(u32*)(skel+52))
    uint32_t sendMs;
    uint8_t  dictEpoch;
    uint16_t bits;
    uint8_t  data[kMaxAnimCmdBytes];
};
static_assert(offsetof(AnimCmdPayload, data) == 11, "AnimCmdPayload fixed-part layout");
static_assert(sizeof(AnimCmdPayload) == 4079, "AnimCmdPayload layout");

inline size_t NetAnimCmdWireSize(uint16_t bits)
{
    return offsetof(AnimCmdPayload, data) + ((static_cast<size_t>(bits) + 7u) / 8u);
}

constexpr size_t kMaxPacketSize = sizeof(NetPacketHeader) + sizeof(PlayerNetState);

constexpr size_t kPathMtuPayload = 1472;

enum NetEventType : uint8_t {
    NEVT_OPEN_DOOR,          // DoorOpenEvent
    NEVT_CHAT,               // string
    NEVT_THROWPROJECTILE,    // ProjectileThrowEvent
    NEVT_TAKE_DAMAGE,        // PlayerDamageEvent
    NEVT_PLAYER_DEATH,       // PlayerDeathEvent
    NEVT_PLAYER_MELEE_ATTACK,// melee action sid
    NEVT_MATCH_TIMER_SYNC,   // MatchStateSync
    NEVT_FIRE_SHOT,          // FireShotEvent
    NEVT_MATCH_OVER,         // MatchOverEvent
    NEVT_MARK_PLAYER,        // AimMarkEvent
    NEVT_VOX_LINE,           // VoxLineEvent
    NEVT_RESPAWN_REQUEST,
    NEVT_RESPAWN_GRANT,
    NEVT_PING,
    NEVT_SUSPICIOUS_PROCESS,
    NEVT_DEBUGGING,
    NEVT_PLAYER_EXECUTION,   // ExecutionEvent -- the move is starting, play your half
    NEVT_EXECUTION_REQUEST,  // ExecutionRequestEvent -- may I execute you?
    NEVT_EXECUTION_REPLY,    // ExecutionReplyEvent   -- yes / no

    // Ported from tools/ext/glass.c (MSG_GLASS): a broken window piece. Pinned to the ext's
    // own byte 0x7A so the two transports interoperate during the cutover.
    //   [u8 1][u64 pane id][u32 piece][u8 nargs][nargs x 16B event arg slots]   one break
    //   [u8 2][u8 n][n x ([u64 pane id][u32 piece])]                           panes broken here
    NEVT_GLASS = 0x7A,

    // Ported from tools/ext/scopeglint.c (MSG_GLINT): [u8 on] -- that player is scoped in /
    // out. Pinned to the ext's own byte 0x91 so the two transports interoperate during the
    // cutover.
    NEVT_SCOPE_GLINT = 0x91,

    // Ported from tools/ext/backpack.c (MSG_BACKPACK): the TAB backpack (crafting / shop) as
    // other players see it. One byte carries three shapes, dispatched on data[0] (see
    // net/match/backpack.cpp):
    //   [u8 1][u8 phase][u8 bagoff][u8 seq][u32 ms][u16 enter][u16 leave]   the owner's phase
    //   [u8 0x54][u8 clip][u16 first][u8 n][u8 0][n x {u8 has, i16 x,y,z}]  spinec track chunk
    //   [u8 0x55][u8 clip][u16 first][u8 n][u8 0][n x {u8 has, i16 x,y,z,w}] root track chunk
    // Pinned to the ext's own byte 0x5B so the two transports interoperate during the cutover.
    NEVT_BACKPACK = 0x5B,

    // ---- reserved ext opcodes (melee + downed) ------------------------------------------
    // These are the ext's own peer-message bytes, pinned here at their EXACT values so the
    // two transports interoperate during the cutover (never renumber the low 0..18 events
    // above). A feature module owns the payload layout; the entries are reservations while
    // that module's wire bridge is still gated (see FEATURES.md / PHASE1.md).
    NEVT_DOWNED      = 0x43,   // ext MSG_DOWNED: [u8 state][u8 downer][u8 knockDir+1][u8 mix][u8 bleed+0.5][u8 epoch] (net/match/downed-state.hpp)
    NEVT_REVIVE      = 0x44,   // ext MSG_REVIVE: [u8 who] -- a teammate completed the revive
    // ext MSG_STUNNED 0x45: the sender's local stun lock started. The receiver stamps the
    // peer-stun rising edge that feeds the music "enemy stunned" (5.0) input. Pinned to the
    // ext's exact byte for cutover interop.
    NEVT_STUNNED     = 0x45,   // StunnedEvent
    NEVT_ARMOR_HIT   = 0x4A,   // ext MSG_ARMOR_HIT: [u8 victim][u8 piece][u8 broke][f32 absorbed][f32 left]
    NEVT_PSYNC       = 0x4B,   // ext MSG_PSYNC: paired-move lifecycle / ping / pong / ack
    NEVT_HIT_REL     = 0x6C,   // ext MSG_HIT_REL: reliable one-shot hits (net/match/hit-rel.hpp)
    NEVT_DODGE_SWING = 0x97,   // ext MSG_DODGE_SWING: a free melee swing was armed
    NEVT_DODGE_RES   = 0x98,   // ext MSG_DODGE_RES: a blow was dodged, play the miss
    NEVT_DODGE_PRESS = 0x99,   // ext MSG_DODGE_PRESS: dodge pressed (immune / dir / clip sid)
    NEVT_HITREACT2   = 0x9A,   // ext MSG_HITREACT2: [u8 flags][u64 clip][u16 cut ms]
    // ext MSG_KFWEAPON 0x94 (killfeed.c): [u8 victimSlot][u64 LE weaponSID]
    // [u8 1 = downing weapon]. Broadcast by the victim so a peer can resolve the
    // weapon icon for its kill / down row (net/ui/kill-feed-icons.cpp).
    NEVT_KFWEAPON    = 0x94,
    NEVT_MEX         = 0x9C,   // ext MSG_MEX: melee exchange window
    NEVT_MV3         = 0x9E,   // ext MSG_MV3: melee-v3 bubble / inputs / hash

    // ext MSG_SHOTRAY 0xAF: where the sender's shot actually flew (immersion.c whizz-bys).
    // Pinned to the ext's exact byte for cutover interop. NOTE: 0xAF is also a daemon tag inside
    // the 0xE3 report (NetDaemonOp::kAfkTrailer) -- a different wire, so there is no collision.
    NEVT_SHOTRAY     = 0xAF,   // ShotRayEvent

    // ext MSG_CHAT 0xC7 (chat.c). Pinned to the ext's exact byte. This is NOT the legacy
    // NEVT_CHAT (0x01, a plain string log event); chat's own payload is validated by
    // net/chat/chat-core.hpp before it reaches the line stores. A short line keeps the
    // ext's v1 layout; a line too long for the native event's 255-byte size byte uses the
    // native v2 fragment layout defined there.
    NEVT_CHAT_C7     = 0xC7,   // [u8 ver][u8 chan][u16 seq][u16 bytes][UTF-8 text]
};

// ---- other wire opcodes introduced by the ext -> mod port -------------------------------
// One source of truth for every byte that goes on a wire. Peer events use NetEventType above;
// the direct-melee helper, the daemon RPC services and the backpack payload versions live
// here so a feature module never has to pin its own copy of an ext byte. Values are the ext's
// exact numbers -- do not renumber them (they must interoperate during cutover).
enum class NetDirectOp : uint8_t {
    kData  = 0x05,   // direct peer payload (ext netdirect.c)
    kHello = 0x06,   // direct peer handshake
    kAck   = 0x07,   // direct peer handshake reply
};

enum class NetDaemonOp : uint8_t {
    kAfkTrailer     = 0xAF,   // 4-byte tag inside the 0xE3 report: [0xAF][u16 idle s][flags]
    kJipTeamSwap    = 0xA7,   // trailing [u8 n][n x (slot, team)] block in the 0xE5 reply
    // 2013 CLAN state / page / post-match report (tools/ext/t2r-ext.c, clan13.c).
    // Same server socket as every other daemon RPC here; pinned to the ext bytes so an
    // ext client and a mod client can talk to the same daemon during the cutover.
    kClanPageReq    = 0xB6,   // [64B token][u8 lang][u8 section][u32 seen id][u16 day][u8 cap=1]
    kClanPageRep    = 0xB7,   // "<section text>" (tab-separated rows, parsed by clan-page-core.hpp)
    kClanStateRead  = 0xE0,   // [64B token] -> kClanStateReply
    kClanStateReply = 0xE1,   // "fac=1 st=1 day=3 ..." (parsed by clan-state.hpp)
    kClanReport     = 0xE2,   // [64B token][u32 seq][u32 parts][u8 flags][u8 n][n x (u8 label,u16 count)]
    kClanChooseSide = 0xE4,   // [64B token][u8 side] -> kClanStateReply
    kNoticePoll     = 0xBA,
    kNoticeReply    = 0xBB,
    kRequeuePoll    = 0xBC,
    kRequeueReply   = 0xBD,
    kRankTable      = 0xBF,   // request and reply are the same byte
    kFriendsRequest = 0xD8,
    kFriendsReply   = 0xD9,
    kJipClockReport = 0xE3,
    kJipClockReply  = 0xE5,
};

// 0xD8 sub-ops used by the in-game chat (tools/social.py). The 0xD8/0xD9 op pair is
// already registered above; these are the exact sub-byte values from chat.c, kept here
// so no feature module pins its own copy of a wire number.
enum class NetDaemonChatSub : uint8_t {
    kSay   = 14,  // S_CHAT
    kPSay  = 15,  // CH_S_PSAY
    kPChat = 16,  // CH_S_PCHAT
    kMLog  = 17,  // CH_S_MLOG (server-side report-log copy)
    kLSay  = 18,  // CH_S_LSAY
    kLChat = 19,  // CH_S_LCHAT
};

enum class NetBackpackPayload : uint8_t {
    kPhase      = 1,    // MSG_BACKPACK phase broadcast (BP_VER 1)
    kSpinec     = 0x54, // recorded spine-c track chunk ("BPT1")
    kRoot       = 0x55, // recorded root track chunk ("BPR1")
};

struct ExecutionEvent {
    uint8_t  victimSlot;
    NetSid64 moveSID;
};
static_assert(sizeof(ExecutionEvent) == 9, "ExecutionEvent layout");

// A finisher is agreed BEFORE either machine commits to it. The attacker asks, the victim
// answers, and only an accepted request releases a puppet or starts a move. Without this both
// players could commit simultaneously and each end up holding a corpse for someone still alive
// -- reconcilable only after the fact, and only if both messages arrived.
struct ExecutionRequestEvent {
    uint8_t victimSlot;
};
static_assert(sizeof(ExecutionRequestEvent) == 1, "ExecutionRequestEvent layout");

struct ExecutionReplyEvent {
    uint8_t attackerSlot;
    uint8_t accepted;   // 0 = refused
};
static_assert(sizeof(ExecutionReplyEvent) == 2, "ExecutionReplyEvent layout");

struct AimMarkEvent {
    uint8_t markedSlot;
};
static_assert(sizeof(AimMarkEvent) == 1, "AimMarkEvent layout");

struct VoxLineEvent {
    uint8_t  speakerSlot;
    NetSid64 lineSID;
};
static_assert(sizeof(VoxLineEvent) == 9, "VoxLineEvent layout");

struct ProjectileThrowEvent {
    float    fromPos[3];
    float    toPos[3];
    float    vel[3];
    NetSid64 throwableSID;
};
static_assert(sizeof(ProjectileThrowEvent) == 44, "ProjectileThrowEvent layout");


struct MatchStateSync {
    uint8_t  timerRunning;
    float    timerRemaining;
    float    timerDuration;
    uint8_t  teamLives[2];
};
static_assert(sizeof(MatchStateSync) == 11, "MatchStateSync layout");

struct RespawnGrantEvent {
    NetSid64 spawnerSid; // which to1-spawn-point-N the host chose
    uint8_t  slot;
    uint8_t  team;
};
static_assert(sizeof(RespawnGrantEvent) == 10, "RespawnGrantEvent layout");

struct PlayerDamageEvent {
    NetSid64 weaponSID;
    NetSid64 weaponAmmoSID;
    float    damage;
    uint8_t  lethalClaim;
    uint8_t  headshot;
};
static_assert(sizeof(PlayerDamageEvent) == 22, "PlayerDamageEvent layout");

struct PlayerDeathEvent {
    NetSid64 weaponSID;
    uint8_t  headshotKill;
    uint8_t  victimSlot;
    uint8_t  killerSlot;
};
static_assert(sizeof(PlayerDeathEvent) == 11, "PlayerDeathEvent layout");

struct DoorOpenEvent {
    NetSid64 doorSid;
    NetSid64 stateSid;
};
static_assert(sizeof(DoorOpenEvent) == 16, "DoorOpenEvent layout");

struct FireShotEvent {
    float ori[4];
    float org[4];
    float launchVel[3];
};
static_assert(sizeof(FireShotEvent) == 44, "FireShotEvent layout");

// ext MSG_STUNNED payload is [u8 dir+1][u8 frame parity]; only byte 0 is read by the ext and
// by us. 0 = front, 1 = back, 2 = left, 3 = right (ext g_stunDir). `dir` here is already +1.
struct StunnedEvent {
    uint8_t dirPlusOne;
};
static_assert(sizeof(StunnedEvent) == 1, "StunnedEvent layout");

// ext MSG_SHOTRAY payload (17 bytes): [u8 reserved][f32 origin x3][i8 dir*127 x3][u8 len m].
// Kept byte-identical to the ext so the two transports can interoperate during cutover.
// `len == 0` means unknown; the ext receiver then uses 120 m.
struct ShotRayEvent {
    uint8_t reserved;
    float   origin[3];
    int8_t  dir[3];
    uint8_t lenM;
};
static_assert(sizeof(ShotRayEvent) == 17, "ShotRayEvent layout");

struct MatchOverEvent {
    int8_t winner;
};
static_assert(sizeof(MatchOverEvent) == 1, "MatchOverEvent layout");

struct PingEvent {
    uint32_t sendMs;
    uint16_t hostPingMs;
    uint8_t  reply;
};
static_assert(sizeof(PingEvent) == 7, "PingEvent layout");

struct DebuggingEvent {
    uint16_t flags;
};
static_assert(sizeof(DebuggingEvent) == 2, "DebuggingEvent layout");

constexpr int kNetProcessNameLen = 64;

struct SuspiciousProcessEvent {
    uint32_t pid;
    char     processName[kNetProcessNameLen];
};
static_assert(sizeof(SuspiciousProcessEvent) == 68, "SuspiciousProcessEvent layout");

#pragma pack(pop)
