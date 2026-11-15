#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

typedef int (*fn_Init)(void);
typedef void (*fn_Void)(void);
typedef void *(*fn_Iface)(void);
typedef const char *(*fn_GetPersonaName)(void *);
typedef uint64_t(*fn_GetSteamID) (void *);
typedef void (*fn_IdentSet)(void *ident, uint64_t id);
typedef uint64_t(*fn_IdentGet) (const void *ident);
typedef int (*fn_SendToUser)(void *self, const void *ident,
                             const void *data, uint32_t len, int flags,
                             int channel);
typedef int (*fn_RecvOnChannel)(void *self, int channel, void **msgs,
                                int max);
typedef int (*fn_AcceptSession)(void *self, const void *ident);
typedef int (*fn_CloseSession)(void *self, const void *ident);
typedef void (*fn_MsgRelease)(void *msg);
typedef void (*fn_InitRelay)(void *self);
typedef int (*fn_SetCfgInt)(void *self, int value, int32_t v);
typedef int (*fn_SessionInfo)(void *self, const void *ident, void *info,
                              void *status);
static fn_SessionInfo SessionInfo;
static int g_ice_off;
static int (*g_set_cfg)(void *, int, int);
// Ping locations (host choice): the server collects every helper's Steam ping location
// (0xF4 [string]), hands each helper the others' (0xF5 [u64 steamid][string]) and gets back
// the estimated ping from this machine (0xF6 [u64 steamid][i32 ms], -1 = unknown).
typedef float (*fn_GetLocalLoc)(void *self, void *loc);
typedef void (*fn_LocToStr)(void *self, const void *loc, char *buf,
                            int cb);
typedef bool (*fn_ParseLoc)(void *self, const char *str, void *loc);
typedef int (*fn_EstPing)(void *self, const void *loc);
static fn_GetLocalLoc GetLocalLoc;
static fn_LocToStr LocToStr;
static fn_ParseLoc ParseLoc;
static fn_EstPing EstPing;
static void *g_utils;

static fn_IdentSet IdentSet;
static fn_IdentGet IdentGet;
static fn_SendToUser SendToUser;
static fn_RecvOnChannel RecvOnChannel;
static fn_AcceptSession AcceptSession;
static fn_CloseSession CloseSession;
static fn_MsgRelease MsgRelease;
static void *g_msgs;            // ISteamNetworkingMessages*

#define SEND_FLAGS (1 | 32)     // UnreliableNoNagle | AutoRestartBrokenSession
#define CHANNEL 0
#define PUNCH 0xFF              // tiny message that opens the Steam session

static FILE *g_log;

// A Steam-licensed copy started directly exits at once and Steam starts it again
// (RestartAppIfNecessary), so the game can vanish for a few seconds right after launch.
// Treat the game as gone only after GAME_GONE_MS without it.
#define GAME_GONE_MS 60000
static DWORD g_game_seen;
// Linux external mode (--alive-file, started by t2r-linux.py through Proton in its own prefix): the game runs in another Wine prefix,
// so its process is not in our list; t2r-ext touches <game>/t2r-alive.txt every 5 s instead. Alive = written within GAME_GONE_MS.
static const char *g_alive_file;

// A Steam-licensed copy started directly exits at once and Steam starts it again
// (RestartAppIfNecessary), so the game can vanish for a few seconds right after launch.
// Treat the game as gone only after GAME_GONE_MS without it.
#define GAME_GONE_MS 60000
static DWORD g_game_seen;
// Linux external mode (--alive-file, started by t2r-linux.py through Proton in its own prefix): the game runs in another Wine prefix,
// so its process is not in our list; t2r-ext touches <game>/t2r-alive.txt every 5 s instead. Alive = written within GAME_GONE_MS.
static const char *g_alive_file;

// ---------------------------------------------------------------- match state
static struct {
    int active;
    uint32_t sid;
    uint8_t own;
    uint64_t peer[16];          // slot -> steamid (0 = none)
    int seen[16];               // got anything from that peer yet
} M;
static struct sockaddr_in g_game;       // the game's gameplay socket (from its hello)
static int g_have_game;
static uint64_t g_stats_out, g_stats_in;
static DWORD g_last_data;       // tick of the last gameplay packet from the game
static DWORD g_last_pkt;        // tick of the last datagram of any kind from the game (hello / data)
static int g_pkt_seen;          // the game talked to us at least once (was g_last_pkt != 0 via '| 1': that put the stamp up to 1 ms
                                           // ahead of GetTickCount, the unsigned age wrapped to ~4e9 and the pump flipped idle/active ~12x a second)
static int g_idle_mode;         // 1 = no game traffic for 10 s (single player / menus): slow wake, normal priority, timer period released

// Pump health (logged every 10 s): how long the loop went without running, the most datagrams that were waiting
// in the game's loopback socket / Steam's receive queue at once, the bytes pending in the socket buffer. A loop
// that stalls (CPU starved by the game, a throttled hidden process, a blocking connect) lets the 64 KB default
// loopback buffer overflow = silently dropped gameplay packets = "packet loss" that is not the network's.
static LARGE_INTEGER g_qpf;
static double g_max_gap_ms, g_max_drain_gs, g_max_drain_steam, g_loops;
static u_long g_max_pending;

// ---------------------------------------------------------------- transport
static int g_fake;              // --fake-id mode
static SOCKET g_fake_sock = INVALID_SOCKET;

// Mid-match the host's gameplay stopped arriving at ONE client (helper: 'peer slot 0: state 3 ping 36 ms ... out 120 pkt/s, in 0 pkt/s', the other
// players played on), the Steam session stayed 'connected' (state 3) with the send buffer stuck at 32 KB (pending 32639 B, queue 60 ms) and then crawled
// at 2-3 pkt/s of Steam's own acks for 3 more minutes, across the player's leave and re-join ('gameplay packets in' frozen at 1332231 from 00:50:04 to
// the end; the re-join got nothing from the host). The helper never closed that ISteamNetworkingMessages session, so a wedged session stayed wedged for
// every later match, and nothing but the game's own 15 s host-migration timeout reacted. Per peer we now count what we sent / received / failed, and a
// session that carried gameplay and then went one-way (or never carried a packet although we send plenty) is closed and re-opened (see peer_watch).
static struct {
    uint64_t id;                // steamid this record belongs to (slot reuse by another player clears it)
    uint32_t rx_run;            // gameplay packets received from it since it last went silent for 25 s
    DWORD last_rx;              // tick of the last gameplay packet from it
    uint64_t tx_since_rx;       // gameplay bytes we sent it since the last one from it
    uint64_t tx_n, rx_n, fail_n;        // totals since the game's last hello: sent, received, sends Steam refused
    int last_res;               // last non-OK EResult of a send (25 = LimitExceeded: its buffer is full)
    DWORD last_reset;           // tick of the last close+reopen
    uint32_t resets, resets_nf; // closes (total / without it ever having flowed) since the game's last hello
    DWORD first_tx;             // tick of the first gameplay packet we sent it since the hello (0 = none yet)
    DWORD conn_since;           // tick the Steam session was first seen still connecting / finding a route (0 = connected / not seen)
    uint32_t resets_cn;         // rule E2 re-opens of a session stuck connecting
    int wait_logged;            // a re-open was skipped because the session is still connecting (logged once until the next re-open)
} P[16];
// A session reused from the match before was half-open after GAME-START: Steam said 'state 3,
// ping 36 ms' and carried its own keepalives ('in/out 1-3 pkt/s, pending 6 B') but not ONE gameplay packet crossed in either direction ('gameplay sent 20 got 0'
// on the client, 'sent 0 got 0' on the host), for the rest of the match and again on the mid-match re-join. Rules A / B above need >= 8 KB sent, which a
// stuck peer never reaches (20 packets), so nothing reopened it. New: C = we sent it gameplay since the hello and 2.5 s later nothing came back from it;
// D = both sides silent with this peer for 6 s since the hello while other peers flow (the host side of the same case); E = at a match change (new sid)
// every peer session of the old match is closed + re-opened up front, so a session is never carried from one match into the next.
// OFF by default since build 100: D fired on peers that were still loading and killed sessions
// seconds after they connected, a C reset stamped g_last_rx_any so D then saw 'other peers flow' and reset every peer, and E's simultaneous
// close on both ends left matches with zero gameplay. Opt-in: file t2r-peer-reset.txt next to the helper, or env T2R_PEER_RESET=1 (rules A / B stay).
static int g_no_reset = 1;
static DWORD g_hello_at;        // tick of the game's last (new) hello
static uint64_t g_tx_since_any_rx;      // gameplay bytes sent to anyone since the last gameplay packet from anyone
static DWORD g_last_rx_any;     // tick of that packet

// Direct melee path: the game's traffic is a star (clients talk to the host, which relays), so melee between two
// clients pays both legs. The ext (tools/ext/netdirect.c) opens its own loopback socket to this helper and sends ext messages straight to a peer:
//   ext -> helper [06]                         hello: the sender is the ext (address remembered), reply [07]
//   ext -> helper [05][dst slot][body]         -> Steam message [05][u32 sid][u8 own slot][body] to that peer (nothing else about it is kept)
//   Steam [05][sid][src][body] from peer id    -> checked (same match, src slot's steam id = the sender) -> ext [05][src][body]
// Peers with an older helper drop the unknown first byte; the ext only uses the path after a direct ping/pong proved both ends.
static struct sockaddr_in g_ext;
static int g_have_ext;
static uint64_t g_dir_out, g_dir_in, g_dir_bad;

// ---------------------------------------------------------------- server link
static SOCKET g_srv = INVALID_SOCKET;
// "server up" (the menu's status + FIND MATCH gate) is only claimed once the server really answered. A tunnel (playit) accepts the
// TCP connect with the daemon behind it down and closes at the first read: the old write_status(1) on connect() made the menu flap
// connected / not connected every reconnect (5 s). g_srv_ok = a frame arrived (daemon acks the registration with 0xF1), or - older daemon
// without the ack - the link has survived SRV_OK_MS after the registration.
#define SRV_OK_MS 4000
static int g_srv_ok;
static DWORD g_srv_at;

static uint8_t g_srvbuf[4096];
static int g_srvlen;
static int g_srvskip;           // bytes of an oversized server frame still to discard

static uint32_t g_prev_sid;     // sid of the match before this F2 (0 = none)

// A NAT dropped the bridge TCP socket 8.5 min into a running match. The reconnect below used to wait for 5 s without a gameplay
// packet (connect() blocks), which never happens while a match is running, so the helper stayed off the daemon for the rest of the match (the daemon counts a
// helper that is gone for 4 s as 'host helper gone'). The TCP connect now runs in a worker thread (resolve + non-blocking connect + select, 4 s); the main
// loop adopts the connected socket and registers it, so the P2P pump never blocks on the daemon.
static char g_cs_host[256], g_cs_port[16];
static SOCKET g_cs_new = INVALID_SOCKET;        // a connected, non-blocking socket handed from the worker to the main loop
static volatile LONG g_cs_busy;
