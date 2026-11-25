#!/usr/bin/env python3
"""
TLOU2MP Revival — master server daemon.
Reverse-engineered from winmm.dll (see project memory). Central coordinator:
auth, presence/online-count, loadout persistence, leaderboard, matchmaking.
Gameplay itself is P2P (Steam/Spacewar) — the daemon only coordinates.

Listens TCP+UDP on :7001 (redirect the client's hardcoded 168.119.115.162 here).

PROTOCOL  (framing both dirs: [u16 len LE][u8 opcode][payload], len=len(op+payload))
  TCP client 0x0f  = login (71B: [1c][64B token][01 00 07]) OR profile (346B:
                     token + ~200B opaque loadout blob + 32B name).
        reply      = 0x10(32B name) + 0x08(browser list + udp list) -> sets dc=2 (auth OK)
  TCP client 0x0e  = session start / find-match ([1c][8B nonce][32B name])
  TCP client 0x01  = host/join
  TCP client 0x05  = session heartbeat (~3s)
  UDP client 03+seq -> server 04+seq  (presence keepalive; also game-server check)
  SESSION server->client opcodes:
     03 clear list | 17 map/mode list | 04 player rec | 0c stats | 0a/0b flags
     10 name | 11 text | 16 id | 14 MATCH PEER TABLE (P2P handoff; layout TBD via 2-player capture)
  CLAN (t2r-ext's own short TCP connection, see clan.py / re/factions2013/CLAN-IMPL.md):
     0xD0 [64B token]                                   read custom name + tag
     0xD2 [64B token][u8 nlen][name][u8 tlen][tag]      set them (empty both = clear)  reply 0xD1 "ok=1 name=.. tag=.."
     0xE0 [64B token][optional 8B steamid, LE]          read clan state (the steamid only helps a token with none of its own; read only)
     0xE2 [64B token][u32 seq][u32 parts][u8 flags]     match over: parts earned (flags bit0 = ext
                                                        saw no match end = left early)
     0xE4 [64B token][u8 faction 1 WLF / 2 Seraphites]  CHOOSE YOUR SIDE
     reply 0xE1 = clan.state_text() (ASCII "k=v k=v ..."); op 0xE0 / 0xE2 replies may end with the SUPPLY PERKS of tools/clan_perks.py (re/clan-supply-boosters.md):
        pk=<n> pkc<i>=<1 med 2 ammo 3 mat 4 food 5 water> pkn<i>=<cap per match> pkg<i>=<0 m / 1 f> pkl<i>=<head 0..3> pkr<i>=<role> .. pkm<i>=<Name_with_underscores>
     0xE2 labels 31 / 32 = the perk uses of the match (a nibble per category: 31 med | ammo << 4 | mat << 8, 32 food | water << 4); the daemon draws them from the stock
  BOOSTERS (one-use, tools/clan.py; same kind of connection):
     0xE8 [64B token][optional 8B steamid, LE]          read booster counts / pick / next booster (same steamid rule)
     0xEE [64B token][u8 booster id 1..7, 0 = none]     pick the booster for the next match (used up at GAME-START)
     0xEF [64B token][u8 mission id]                    CHOOSE MISSION (only with no mission running)
     0xE2 may end with [u8 n] n x [u8 label id][u16 count]: the match's award counts = mission progress
        extended labels (clan saga): 21 times downed, 22 deaths, 23 best kill streak, 24 weapon class with the most downs (1 bow 2 revolver 3 shorty 4 hunting
        5 semi 6 burst 7 pistol 8 crossbow 9 melee 10 bomb 11 molotov); sent only when > 0; n <= 16 read
 0xE2 label 33 = supply BOTTLES picked up this match (Interrogation lockbox-phase death drops; clan supplies = bottles + trunc(parts * 0.02)) -> clan.apply_match(bottles=)
     reply 0xE9 = clan.meta_text() (ASCII "k=v k=v ...")
  IN-GAME CLAN PAGE: 0xB6 [64B token][u8 language][u8 section 1 roster / 2 journal / 3 raids / 4 status / 5 match stats of the raid whose day is in the u16 slot][opt u32 seen journal id][opt u16 seen raid day] -> reply 0xB7 text (tools/clan_ingame.py)
  RANK BADGE: 0xBF [64B token] -> reply 0xBF "n=<k> HHHHHHHH:rank:weeks ..." (tools/rankbadge.py: the rank of every recently seen player keyed by a hash of the shown name; the map vote state 0xEB also carries pk<i> / pw<i> per roster row)
  CLAN FEED (main menu notification line, same kind of connection): 0xB4 [64B token][u8 language, optional: 0 / absent = English, 1 = Polish]
    -> reply 0xB5 "n=<k>\n<id>|<text>\n.." (feed_op, <= 3 lines, UTF-8, the text rendered in the asked language)
  HOST REGION (host pick + migration; hostmig.region_pool): the host comes from the majority region of the match (EU / NA-east / NA-west / other,
     decoded from each helper's 0xF4 ping location "pop=ms,..."), then the worst-ping + stall rule picks inside it. T2R_HOST_REGION_PIN=
     "steamid:EU,..." forces that region while that Steam account is in the match (release/host/T2R-Host.ps1 defaults it to the owner's, EU).
  PERFORMANCE REPORT (always on, t2r-ext short connection, every 30 s; feeds the host pick + migration, see hostmig.PerfScores):
     0xCA [64B token][u8 ver=1][u8 stalls>=250ms][u8 stalls>=1000ms][u16 LE worst stall ms][u16 LE avg frame ms*10][u8 flags bit0 = VRAM over budget]
         (counts = in-match frame stalls of the client's last 5 minutes)         reply 0xCB "ok=1"
         old servers ignore it (no reply: the ext stops after 2 tries); old clients never send it = unknown = no penalty
  HOST MIGRATION (t2r-ext, same kind of short connection; T2R_HOST_MIGRATION=1, see hostmig.py):
     0xE6 [64B token][u8 flags][u8 epoch]   reply 0xE7 [enabled][host slot][epoch][alive mask][state]
  JOIN IN PROGRESS (on by default, T2R_JIP=0 = off, re/join-in-progress.md; t2r-ext short connection, polled every 2 s (host) / 3 s while in a match):
     0xE3 [64B token][u8 flags: bit0 clock valid, bit1 match over][f32 seconds left][f32 duration]
         (newer ext: + [u8 my team][u8 ally lives][u8 enemy lives], 0xFF = unknown: the late-join bonus)
     reply 0xE5 [enabled][n][n x 0x32 roster entries: [0] slot, [1..0x30] name, [0x31] team]
  PARTY (friends on the same team; t2r-ext short connection):
     0xDA [64B token][u8 sub][name]   sub 0 read, 1 invite <name>, 2 accept the invite from <name>, 3 leave/disband
     reply 0xDB = party_text() (ASCII "k=v k=v ...": n, m=members, inv=pending invites from)
  INTERROGATION (T2R_MODE=interrogation|rotate, t2r-ext short connection, see interrogation.py):
     0xDC [64B token][u8 sub][u16 seq][args]   sub 0 poll, 1 interrogated [victim slot],
                                               2 lockbox stage done [stage], 3 died [killer slot]
     reply 0xDD = INT.event() (ASCII "k=v k=v ..."; on=0 = not an Interrogation match)
  MAP VOTE (T2R_MAP_VOTE=1, t2r-ext's own short TCP connection, see MATCH FLOW below):
     0xEA [64B token]                                   read the vote state
     0xEC [64B token][u8 option 0..2]                   cast / change my vote
     reply 0xEB = vote_state_text() (ASCII "k=v k=v ...")
  MAP PACKS (T2R_MAP_PACKS=1, t2r-ext short connection, tools/map_packs.py, re/part1/map-packs.md):
     0xC6 [64B token][u8 n][n x u32 pack hash]           the client's installed map packs
     reply 0xC7 "on=1 acc=<ids> pm<index>=<level> ..."   (on=0: packs off on this server)
  ANTI-CHEAT (tools/anticheat.py, re/anticheat-1025.md; T2R_AC=0 = off): 0xCC flag events + demo wishes -> 0xCD, 0xCE demo upload -> 0xCF, 0xD4 player report -> 0xD5,
     0xD6 my last 3 matches' players -> 0xD7 (layouts at the ANTI-CHEAT block above clan_report). Never bans: tools/reports/review.py writes bans.txt.
  FRIENDS + PARTIES (tools/social.py): 0xD8 [64B token][u8 ver=1][u8 lang][u8 sub][UTF-8 arg] from the launcher's FRIENDS page -> 0xD9 JSON state (friends with presence,
     pending requests, party with leader, party invites). A party queues as one unit (MatchMaker.try_form), in game its events ride the notice poll (kind p).
  PART I MAPS preference: the 0xC8 request below may end with ONE flags byte (bit 0 = ALWAYS), see tools/map_packs.py; reply 0xC9 adds " p1=<0|1>".
  SP-SECTION MAPS (always on; T2R_SP_GATE=0 turns the gating off): 0xC8 [64B token][u32 LE mask] = which SP maps (bit k = list
     index 8 + k) the client's DLL contains; reply 0xC9 "on=1 sp=<mask hex>". A match's pool / vote offers an SP map only when
     every player reported it (old clients that never report: base maps only).
"""
import json, math, select, socket, threading, time, os, sqlite3, struct, secrets, collections, zlib, sys, re, contextlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# CLOCK JUMPS (soak audit): every timeout / TTL in this process (presence, queue, reaper, host migration, notices, rate limiters) reads time.time.
# An NTP step or a manual clock change of +5 min made every player look offline for a moment (all running matches were reaped as "every player
# offline", the queue emptied); -1 h froze them for an hour. `time` below is the standard module with ONE change: time() = the wall clock at
# daemon start + the monotonic seconds since, i.e. a wall-clock-looking value that never jumps. (Stored timestamps - DB last_seen, clan feed - stay
# real-looking; they only differ from the system clock by the steps made since the start.)
import time as _real_time
_CLOCK_T0 = _real_time.time() - _real_time.monotonic()
class _SteadyTime:
    def __getattr__(self, name):
        return getattr(_real_time, name)
    def time(self):
        return _CLOCK_T0 + _real_time.monotonic()
time = _SteadyTime()
def wall_time():
    """The REAL wall clock: for values that other programs compare with their own clock (status file ts read by the update server / admin site)."""
    return _real_time.time()
def notices_public():
    """The notice book's public list with endsAt moved from the steady clock it runs on to the real wall clock (the launcher / admin site compare it with Date.now(); after an NTP step the two differ)."""
    d = wall_time() - time.time()
    out = NOTICES.public()
    for x in out:
        if isinstance(x.get("endsAt"), (int, float)): x["endsAt"] += d
    return out
import names                                  # custom names + clan tags (tools/names.py)
import clan                                   # 2013 clan rules + clans table (tools/clan.py)
try:
    import clan_perks                         # clan supply perks (tools/clan_perks.py, re/clan-supply-boosters.md)
except Exception as _pkex:                    # noqa: BLE001 - perks are optional: a broken module never takes the clan down
    sys.stderr.write("clan_perks unavailable: %r" % (_pkex,) + chr(10))
    clan_perks = None
try:
    import clan_status                        # member status updates (tools/clan_status.py)
except Exception as _stex:                    # noqa: BLE001 - optional
    sys.stderr.write("clan_status unavailable: %r" % (_stex,) + chr(10))
    clan_status = None
try:
    import clan_story                         # clan members as people + the 2013-style clan feed (tools/clan_story.py); optional
except Exception as _ex:                      # noqa: BLE001 - the clan itself must keep working without the feed
    clan_story = None
try:
    import achievements                       # Steam-style achievements (tools/achievements.py, re/achievements-1117/list.md); optional
except Exception as _ex:                      # noqa: BLE001 - the clan must keep working without them
    sys.stderr.write("achievements unavailable: %r" % (_ex,) + chr(10))
    achievements = None
import hostmig                                # host migration decisions (tools/hostmig.py)
try:
    import clan_reset                         # admin "reset clan" + backup / restore (tools/clan_reset.py); optional
except Exception as _ex:                      # noqa: BLE001
    clan_reset = None
import hostcap                                # host upload capacity history (tools/hostcap.py, T2R_HOST_CAP, re/host-capacity-1019.md)
import modeq  # per-mode queues (tools/modeq.py, op 0xBC / 0xBD)
import requeue                                # the queue stays after a match (tools/requeue.py)
import lobbybook                              # stable search lobbies (tools/lobbybook.py)
import interrogation                          # Interrogation game mode (tools/interrogation.py, T2R_MODE)
import gunlimit                               # per-team gun limits (tools/gunlimit.py): 1 Tactical Shotgun per team
import social                                 # friends + parties (tools/social.py, op 0xD8/0xD9; re: header of social.py)
import notices                                # server notices to players: update available / planned restart / free text (tools/notices.py)
import map_packs                              # DLC map packs (tools/map_packs.py, T2R_MAP_PACKS, re/part1/map-packs.md)
import ipaddress
import private_lobby                          # private matches: lobby, code, rule overrides, no clan / stats (tools/private_lobby.py, op 0xF8 / 0xF9)
try:
    import ident                              # identity ids per player (Steam id + install + hardware + IP); optional, not part of this repository
except Exception as _ex:                      # noqa: BLE001 - matchmaking must keep working without it
    ident = None
try:
    import anticheat                          # server-side anti-cheat, flag system, demos, player reports (tools/anticheat.py); optional, T2R_AC=0 = off
except Exception as _ex:                      # noqa: BLE001 - matchmaking must keep working without it
    anticheat = None

HOST = os.environ.get("T2R_HOST", "0.0.0.0")   # 127.0.0.1 for local-only tests
PORT = int(os.environ.get("T2R_PORT", "7001"))
DB_PATH = os.environ.get("T2R_DB") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "t2r.db")   # T2R-Host.ps1: one DB that outlives the PT folders (clans, stats)
LOG_PATH = os.environ.get("T2R_LOG_PATH") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "daemon.log")   # (T2R_LOG_PATH: soak / test daemons keep their log out of the tools folder)
KEEPALIVE_TTL = 15.0                   # seconds since last UDP ping = still online
# The patched winmm net layer is statically sized for 4 player slots (peer table 0x180167cc0 x4, net players
# ctx+0x1c0+slot*0xa70 x4 right below ctx+0x2b80, per-slot anim state x4): a 5th slot overwrites the engine's own
# state. Until those tables are relocated the server never forms a match bigger than the engine holds
# (spec ran 4v4 by raising the matchmaking size only - default 8; T2R_ENGINE_SLOTS=4 caps it again).
ENGINE_SLOTS = int(os.environ.get("T2R_ENGINE_SLOTS", "8"))
MATCH_SIZE = min(int(os.environ.get("T2R_MATCH_SIZE", "8")), ENGINE_SLOTS)   # max players per match: up to a 4v4 once the engine holds 8
MIN_PLAYERS = int(os.environ.get("T2R_MIN_PLAYERS", str(min(2, MATCH_SIZE))))
FILL_WAIT = float(os.environ.get("T2R_FILL_WAIT", "15"))   # s without a new arrival before the wanted match size steps down by one player
# JOIN IN PROGRESS (re/join-in-progress.md; ON by default since (helps crashed players get back in); T2R_JIP=0 turns it off): a searching player is put into an already RUNNING match that
# is not full (fewer players than MATCH_SIZE) instead of waiting for a new one. Needs the matching t2r-ext (it reports the match
# clock over op 0xE3 and adds the joiner to the roster of the players already in the match).
JIP = os.environ.get("T2R_JIP", "1") == "1"
JIP_HOST_CAP = os.environ.get("T2R_JIP_HOST_CAP", "1") != "0"   # no join in progress past what the host's lag record can carry (0 = old)
# WAVE RESPAWN (default ON since, T2R_WAVE_RESPAWN=0 = off): every client's respawn countdown ends at the next wave of the shared match clock
# (5-15 s wait, 2013 Supply_Raid.wiki). The setting rides on the 0xE5 reply (jip_roster_op) as one trailing byte, so clients that poll in a match
# follow it; a client that never gets it (old server / no poll) runs the default (on).
WAVE_RESPAWN = os.environ.get("T2R_WAVE_RESPAWN", "1") != "0"
JIP_MIN_AGE = float(os.environ.get("T2R_JIP_MIN_AGE", "15"))   #: a joiner is not put into a match before it is this many seconds past its GAME-START (the 0xF2 re-push + roster merge + a Hello landed in the host's first seconds, while the peers' links are still settling; host 'des' lost its only enemy 5 s later)
JIP_MIN_AGE_REMNANT = float(os.environ.get("T2R_JIP_MIN_AGE_REMNANT", "120"))   # a match with one player left is not joined until this long after its countdown (load-timeout remnants collapse)
JIP_MIN_LEFT = float(os.environ.get("T2R_JIP_MIN_LEFT", "90"))    # a match with less than this many seconds on its clock is not joinable
LIVES_START = 20     # Supply Raid: reinforcements per team at the start (2013 wiki); the late-join multiplier's progress is measured against it
if os.environ.get("T2R_TEST_LIVES2", "") == "1":
    LIVES_START = 40  # test server: the ext doubles the team lives at the match start (flag 0x20, wave_byte)
JIP_CLOCK_TTL = float(os.environ.get("T2R_JIP_CLOCK_TTL", "12"))  # the newest clock report must be at most this old (hosts report every 2 s, clients every 3 s)
JIP_REUSE_S = float(os.environ.get("T2R_JIP_REUSE_S", "10"))      # a slot whose player left is handed to a joiner only after this long (the host drops the old peer after ~5 s)
JIP_PREFER_LEFT = float(os.environ.get("T2R_JIP_PREFER_LEFT", "180"))  # a running match with at least this many s left is preferred to forming a new small match from the searchers
JIP_ACK_STALE = float(os.environ.get("T2R_JIP_ACK_STALE", "12"))     # a player whose last roster poll is older than this when the joiner is put in is not waited for
SECOND_MATCH_MIN = int(os.environ.get("T2R_SECOND_MATCH_MIN", "6"))  # searchers needed to start a match while another one is forming / running (they queue otherwise)
# Live 23:11 (match 0c6a99b0, Overpass): a Linux client whose Steam helper was not running (the Linux bypass of the Steam-only gate let it
# queue) formed a 2-player match, so the match ran over the server relay. Join in progress then filled it to 8 with players who all had a helper:
# the whole lobby played through the relay at 375-1000 ms (psync peer rtt; direct 85-170 ms), melee pairs started late or not at all.
# A server-relay match takes join-in-progress players WITH a Steam helper only while it has fewer than RELAY_MATCH_MAX active players; past that
# they form / join a Steam P2P match. Players without a helper can still join it (they play over the relay anywhere). 0 = no limit (old behaviour).
RELAY_MATCH_MAX = int(os.environ.get("T2R_RELAY_MAX", "4"))

def has_helper(tok):
    """The player's Steam helper is registered with a gameplay port and a live connection (the same test as the Steam-only gate and push_peers)."""
    return bool(BRIDGES.steamid(tok) and BRIDGES.port(tok) and BRIDGES.conn(tok))
# QUEUE STAYS AFTER A MATCH (tools/requeue.py, ext requeue.c; T2R_REQUEUE=0 = off): every player still in a match at its end whose ext can
# search by itself (0xE3 flag 0x20 within REQUEUE_CAP_S of the end) gets a ticket; its ext searches again once its post-match screens are done, and the
# next lobby of those players waits until all of them search (or went offline / cancelled), at most REQUEUE_WAIT_MAX s after the match end.
REQUEUE = os.environ.get("T2R_REQUEUE", "1") != "0"
# 120 s: measured over 312 finished matches in reports-1109..1111 (ext logs, 60 fps frame stamps): end banner -> supply counter closed took
# p50 48 s, p75 76 s, p90 209 s (the screens wait for a press, no auto close). 120 s covers ~85 % of players; the slower ones are not waited for.
# (persistent lobby, approved defaults): 90 s cap - the ones still on the screens then keep their seat and join the next match in progress
REQUEUE_WAIT_MAX = float(os.environ.get("T2R_REQUEUE_WAIT_MAX", "90"))
REQUEUE_TICKET_S = float(os.environ.get("T2R_REQUEUE_TICKET_S", "300"))  # (p95 of the same measure: 258 s) after this the ext no longer searches by itself
REQUEUE_OFFLINE_S = float(os.environ.get("T2R_REQUEUE_OFFLINE_S", "30"))  # a ticket holder silent this long is gone (keepalive ~3 s, ext notice poll ~6 s; guess: 5 missed polls)
REQUEUE_CAP_S = 60.0     # a flag-0x20 clock report this recent at the match end = the ext searches by itself (reports come every 2-3 s in a match)
RQ_CAP = {}              # token -> time of its last 0xE3 report with flag 0x20
# PERSISTENT LOBBY ("have the lobby not disband after a match ends, seamless, wait for people to get out of the match summary, kinda make it
# work like cod lobbies"; tools/requeue.py ready_groups / formed / next_of). T2R_LOBBY_KEEP=0 = the old requeue (wait, then the queue re-picks freely).
#   - the group of a finished match forms its next match TOGETHER and first (the queue backfills the open seats) once everybody is back, or LOBBY_RELEASE_S
#     after its first member searched again, or REQUEUE_WAIT_MAX after the end - never earlier than LOBBY_MIN_GAP_S after the end (_free runs at 9 s: the
#     per-roster dictionaries of the finished match are gone by then, so a same-roster rematch never shares them);
#   - members still on the post-match screens keep their seats (the backfill and other joiners leave room) and join the new match in progress;
#   - lone searchers wait for a lobby that is waiting and has room for them (they fill its open seats) instead of forming their own match.
LOBBY_KEEP = os.environ.get("T2R_LOBBY_KEEP", "1") != "0"
LOBBY_RELEASE_S = float(os.environ.get("T2R_LOBBY_RELEASE_S", "30"))
LOBBY_MIN_GAP_S = float(os.environ.get("T2R_LOBBY_MIN_GAP_S", "10"))
# Stable lobbies (a player seen in the lobby stays until they back out): every public searcher sits in one lobby of at most
# MATCH_SIZE players; the searching page and the LOBBY chat show that lobby and a match forms from one whole lobby (tools/lobbybook.py).
LOBBY_BOOK = os.environ.get("T2R_LOBBY_BOOK", "1") != "0"
def book_on():
    """The stable lobbies are on: T2R_LOBBY_BOOK and the persistent lobby (T2R_LOBBY_KEEP: the post-match lobby is one of them)."""
    return LOBBY_BOOK and LOBBY_KEEP
# ANTI-AFK (ext afk.c): the ext counts the seconds without any input (keys, mouse, any pad) while alive in a running match and reports them on
# every 0xE3 clock report; at AFK_KICK_S the player is removed through the kick path (counts as leaving early, no requeue). 0 = off. The ext warns at 60 s.
AFK_KICK_S = float(os.environ.get("T2R_AFK_KICK_S", "90"))      # brief: warning ~60 s, removal ~90 s
AFK_KICKED = set()       # (token, mid) removed for inactivity (once per match)
# ping limit: a player whose Steam ping to the match host stays
# above PING_KICK_MS for PING_KICK_S is removed through the kick path (the ext shows the must-close kick box with the numbers). Not counted as leaving early
# (no clan penalty), no requeue ticket, join in progress keeps it out of that match. Never the host itself, never while most players are high to the host
# (then the host's link is the problem: host migration's business), not in the first PING_GRACE_S of a match, only fresh measurements (PING_FRESH_S).
# T2R_PING_KICK_MS=0 = off.
PING_KICK_MS = int(os.environ.get("T2R_PING_KICK_MS", "190"))
PING_KICK_S = float(os.environ.get("T2R_PING_KICK_S", "60"))
PING_GRACE_S = 45.0
PING_FRESH_S = 90.0
PING_HIGH = {}           # (token, mid) -> time its ping to the host was first seen above the limit (cleared when it drops below)
PING_KICKED = set()      # (token, mid) removed for ping (once per match)
# packet loss limit (a player at 140-165 ms to the host passed the ping limit but lost 44 % of his
# position packets on the way to everyone else: he teleported for the whole lobby, re/reports-1123): every ext adds the loss it saw per other
# player to its 30 s performance report (op 0xCA trailer 3: msg-7 packets received / lost per peer name). A player whose MEDIAN loss across
# LOSS_MIN_OBS or more observers stays at or above LOSS_KICK_PCT for LOSS_KICK_S is removed through the kick path (no penalty, keep_out), and
# his new searches are refused for LOSS_HOLD_S. Never while half or more of the judged players are lossy (the host's link / relay is the
# problem then), not in the first PING_GRACE_S, only observers that received LOSS_MIN_PKTS in the last LOSS_WINDOW_S.
# Numbers from the reports (59 reports, 271 players): typical median 0.0 %, p90 0.6 %; the teleporting player 44.5 %.
# T2R_LOSS_KICK_PCT=0 = off. T2R_LOSS_GATE=1 = log only (no removal, no hold).
# 30 % for 120 s, judged against the match: a player is removed only when the rest of the match
# is clean (median loss of the others under LOSS_BASE_PCT) and fewer than half of the judged players are at LOSS_KICK_PCT / 2 or more. In one
# match five of eight players sat at 17-22 % (a lossy host / route for all) and the one at 23-34 % was removed.
LOSS_KICK_PCT = float(os.environ.get("T2R_LOSS_KICK_PCT", "30"))
LOSS_KICK_S = float(os.environ.get("T2R_LOSS_KICK_S", "120"))
LOSS_BASE_PCT = float(os.environ.get("T2R_LOSS_BASE_PCT", "8"))
LOSS_GATE = int(os.environ.get("T2R_LOSS_GATE", "2"))
LOSS_HOLD_S = float(os.environ.get("T2R_LOSS_HOLD_S", "300"))
LOSS_REPEAT_S = float(os.environ.get("T2R_LOSS_REPEAT_S", "7200"))   # the search hold only for a 2nd removal within this (the 1st: back to searching at once)
LOSS_MIN_OBS = 2
LOSS_MIN_PKTS = 300       # msg 7 runs at 30-60/s per peer: one 30 s report window holds ~900-1800
LOSS_WINDOW_S = 75.0      # two 30 s reports
LOSS = {}                 # (mid, observer, subject) -> deque of (time, received, lost)
LOSS_HIGH = {}            # (token, mid) -> time its median loss was first seen at or above the limit
LOSS_KICKED = set()       # (token, mid) removed for loss (once per match)
LOSS_HOLD = {}            # steam id (or token when unknown) -> (time removed, loss %)
LOSS_STRIKES = {}         # steam id (or token) -> [times removed for loss] (LOSS_REPEAT_S)
LOSS_FLAGS = {}           # (mid, token) -> (its msg-7 sends are paced, its host upload budget is on) from its last report
# same-region matchmaking: with more searchers than one match holds,
# the searchers of the lead region (most searchers; tie = the earliest one's) go first, queue order kept inside each group; a joiner prefers a
# running match whose host is in its own region. Regions = EU / NA-east / NA-west from the Steam ping location ('other' never counts as same).
# A searcher waiting MM_REGION_WAIT_S or more keeps its queue place. T2R_MM_REGION=0 = off.
MM_REGION_ON = os.environ.get("T2R_MM_REGION", "1") != "0"
MM_REGION_WAIT_S = float(os.environ.get("T2R_MM_REGION_WAIT_S", "90"))
# ping gate (match 72784a06
# formed a Polish player with two South Americans at 254 ms to the host, then let three more join at 238-280 ms; all four were ping-removed after 60 s):
#  - formation: searchers are only matched together when one host candidate has all of them at or under PING_KICK_MS (hostmig.ping_fit);
#    the others stay in the queue;
#  - host pick: a host candidate with nobody over PING_KICK_MS wins over one that would get somebody removed;
#  - join in progress: a joiner over PING_KICK_MS to the host never joins, also when most players are high to the host (jip_ping_block).
# Unknown pings are not judged. T2R_MM_PING_GATE=0 = off (all three).
MM_PING_GATE = os.environ.get("T2R_MM_PING_GATE", "1") != "0"
QUEUE_HOLD_MAX = float(os.environ.get("T2R_QUEUE_HOLD_MAX", "75"))  # s the oldest searcher may sit in the second-match queue hold; then the searchers start their own match (0 = hold forever)
PARTY_HOLD_S = float(os.environ.get("T2R_PARTY_HOLD_S", "120"))   # s a ready party short of players holds the solos back from a tiny match of their own (0 = off)
MAX_WAIT = float(os.environ.get("T2R_MAX_WAIT", "150"))              # s the oldest searcher may wait for a fuller lobby: then the match forms with everybody searching (JIP fills it later)
# re/queue-fill-1019.md: T2R_FILL_RULE=classic (default: the rule above, unchanged) | fast (replayed on 377 real searches: 30-55 % fewer
# give-ups, median wait 69 s -> 24 s). fast = step-down every FAST_STEP s, oldest searcher waits at most FAST_MAX_WAIT s, and when NOBODY ELSE is online
# who could still arrive (online - searching - in a match <= 0) the lobby forms after FAST_ALONE s without an arrival. A file `fill_rule.txt`
# (one word) next to daemon.py overrides the env (re-read every 5 s, no restart). T2R_FILL_WAIT / T2R_MAX_WAIT keep driving `classic`.
FILL_RULE = os.environ.get("T2R_FILL_RULE", "fast").strip().lower()      # 2 searchers waited far too long under classic (8 -> 2 = 90 s of quiet, max 150 s)
FAST_STEP = float(os.environ.get("T2R_FAST_STEP", "8"))
FAST_MAX_WAIT = float(os.environ.get("T2R_FAST_MAX_WAIT", "40"))
FAST_ALONE = float(os.environ.get("T2R_FAST_ALONE", "15"))
_FILL_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fill_rule.txt")
_fill_cache = [0.0, None]
def fill_rule():
    """Current fill rule name ('classic' | 'fast'); the file next to daemon.py wins over T2R_FILL_RULE."""
    now = time.time()
    if now - _fill_cache[0] >= 5.0:
        _fill_cache[0] = now; v = None
        try:
            with open(_FILL_FILE, "r", encoding="utf-8", errors="replace") as f:
                v = f.read().strip().lower() or None
        except Exception:
            v = None
        _fill_cache[1] = v
    return _fill_cache[1] or FILL_RULE
JIP_ACK_MAX = float(os.environ.get("T2R_JIP_ACK_MAX", "25"))      # GAME-START of a joiner waits this long (max) for the others' roster refresh
# A player becomes matchable only this long after (re)entering the queue: a match start that
# lands while the client is still switching to its searching page (right after backing out of
# a found match / being sent back to searching) dropped it to the title screen (
# "find match boots to the boat main menu after backing out"; live log: leave -> re-search ->
# START-MATCH within 1 s). matchmaker_loop forms the match a moment later.
QUEUE_SETTLE = float(os.environ.get("T2R_QUEUE_SETTLE", "3"))
# A queued player is matchable only while its last matchmaking poll (searching-page heartbeat, 0x05 ~3 s / 0x15 ~1.5 s)
# is at most this old; older = not (or no longer) searching. Still queued until QUEUE_TTL so a late poll revives it.
QUEUE_FRESH = float(os.environ.get("T2R_QUEUE_FRESH", "5"))
# 1 = old behaviour: answer the MP-menu announce (0x0f+0x0e) with ...+0x0d, closing the socket.
# Default 0 holds it open (re/mm-boot.md: the 0x0d raced FIND MATCH and booted to the title).
ANNOUNCE_CLOSE = os.environ.get("T2R_ANNOUNCE_CLOSE", "0") == "1"

# ---------------------------------------------------------------- logging
# daemon.log is rotated: at start the previous log becomes daemon.log.1 (.1 -> .2 ... up to LOG_KEEP files), and a
# running log that passes LOG_MAX_BYTES rolls over the same way, so a long-running host never fills the disk and a
# restart (crash loop, new build) never throws away the log of the match a tester is reporting. Raw TCP hex dumps are
# cut to LOG_HEX bytes. Besides the protocol lines the daemon writes compact EVT lines (join / queue / match formed /
# loaded / left / timeout / ended / reaped, with the reason) that are enough to reconstruct a tester's match.
LOG_MAX_BYTES = int(os.environ.get("T2R_LOG_MAX_BYTES", str(8 * 1024 * 1024)))
LOG_KEEP = int(os.environ.get("T2R_LOG_KEEP", "5"))
LOG_HEX = int(os.environ.get("T2R_LOG_HEX", "48"))
_q = collections.deque(maxlen=50000)
_CTRL = {i: " " for i in range(32)}
_CTRL.update({0x7f: " ", 0x2028: " ", 0x2029: " ", 0x85: " "})
def clean_str(s, n=64):
    """A player-supplied string made safe to log / store / relay: control characters (newlines!) become spaces, cut to n characters."""
    if isinstance(s, bytes):
        s = s.decode("utf-8", "replace")
    return str(s).translate(_CTRL).strip()[:n]
# Per-category rate limit (soak): a line printed per packet / per poll (TCP recv hex dumps of non-poll frames, GAMEPLAY / UDP / RELAY lines of a
# source-port spray, HANDLER ERR of a fuzzer) used to fill the 8 MB log in minutes and rotate away the match a tester reports. The category is the first
# argument ("TCP", "UDP", "RELAY-DATA", ...; EVT lines: "EVT <kind>"). Each category may burst LOG_BURST lines, then LOG_PER_S lines per second; the
# rest is counted and summarised in ONE "LOG-SUPPRESSED" line per category and second window (STATUS lines are never limited).
LOG_BURST = int(os.environ.get("T2R_LOG_BURST", "200"))
LOG_PER_S = float(os.environ.get("T2R_LOG_PER_S", "40"))
_LOG_UNLIMITED = frozenset(("STATUS", "LOG-SUPPRESSED", "daemon", "DB"))
_log_cat = {}                  # category -> [tokens, last refill (monotonic), suppressed count, last summary]
_log_lock = threading.Lock()
def _log_allow(cat):
    """True when a line of this category may be written now (token bucket per category; at most 256 categories are tracked)."""
    now = time.monotonic()
    with _log_lock:
        b = _log_cat.get(cat)
        if b is None:
            if len(_log_cat) > 256:
                return True
            b = _log_cat[cat] = [float(LOG_BURST), now, 0, now]
        b[0] = min(float(LOG_BURST), b[0] + (now - b[1]) * LOG_PER_S); b[1] = now
        if b[0] >= 1.0:
            b[0] -= 1.0
            return True
        b[2] += 1
        if now - b[3] >= 5.0 and b[2]:
            n, b[2], b[3] = b[2], 0, now
            _q.append(f"[{time.strftime('%H:%M:%S')}] LOG-SUPPRESSED {cat} {n} lines in the last 5 s (> {LOG_PER_S:.0f} per second)")
        return False
def log(*a):
    if a:
        cat = str(a[0])[:24] if a[0] != "EVT" or len(a) < 2 else "EVT " + str(a[1])[:20]
        if cat not in _LOG_UNLIMITED and not _log_allow(cat):
            return
    _q.append(f"[{time.strftime('%H:%M:%S')}] " + " ".join(str(x) for x in a).translate(_CTRL))
def evt(kind, *a):
    """One reconstruction line: 'EVT <kind> ...' (grep EVT daemon.log*)."""
    log("EVT", kind, *a)
def hexcut(b, n=None):
    n = LOG_HEX if n is None else n
    return b[:n].hex(" ") + (" ...(+%dB)" % (len(b) - n) if len(b) > n else "")
def rotate_log(path=None, keep=None):
    """daemon.log -> daemon.log.1 -> .2 ... (the oldest beyond `keep` is deleted). Never raises."""
    path = path or LOG_PATH
    keep = LOG_KEEP if keep is None else keep
    try:
        for i in range(keep, 0, -1):
            src = path if i == 1 else "%s.%d" % (path, i - 1)
            dst = "%s.%d" % (path, i)
            if os.path.exists(src):
                if os.path.exists(dst):
                    os.remove(dst)
                os.replace(src, dst)
    except OSError:
        pass
def _flusher():
    rotate_log()
    with open(LOG_PATH, "w") as _f:
        _f.write(f"# daemon {time.ctime()}\n")
    size = 0
    while True:
        time.sleep(0.5)
        if not _q:
            continue
        try:
            while _q:
                with open(LOG_PATH, "a", encoding="utf-8", errors="replace") as f:     # (PT60 a non-cp1252 player name raised UnicodeEncodeError, not OSError - the flusher thread died and the log stopped for good)
                    while _q:
                        line = _q.popleft() + "\n"
                        f.write(line); size += len(line.encode("utf-8", "replace"))
                        if size > LOG_MAX_BYTES:        # checked per line: a 50000-line backlog used to overshoot the cap by megabytes before the next check
                            break
                if size > LOG_MAX_BYTES:
                    rotate_log()
                    with open(LOG_PATH, "w") as _f:
                        _f.write(f"# daemon log continued {time.ctime()}\n")
                    size = 0
        except Exception:
            pass

# ---------------------------------------------------------------- framing
def msg(opcode, payload=b""):
    body = bytes([opcode]) + payload
    return len(body).to_bytes(2, "little") + body

def frames(buf):
    """Yield (opcode, payload, consumed) for each complete [u16 len][op][payload]."""
    i = 0
    while len(buf) - i >= 2:
        ln = int.from_bytes(buf[i:i+2], "little")
        if ln == 0 or len(buf) - i < ln + 2:
            break
        op = buf[i+2]
        payload = buf[i+3:i+2+ln]
        yield op, payload
        i += 2 + ln

_SF_CACHE = {"t": 0.0, "key": None, "ents": []}
SF_CACHE_S = float(os.environ.get("T2R_SEARCH_CACHE_S", "1.0"))
def _search_mode(tok):
    """The queue mode a searcher is in (its party leader's pick); every searcher shares it when the server forces one."""
    try:
        return MODEQ.get(tok, PARTIES.leader(tok))
    except Exception:                              # noqa: BLE001 - the list never breaks on the mode lookup
        return 0

def _search_entries(mode=None):
    """[(token, shown name bytes, team label bytes)] of the queue, cached SF_CACHE_S s: every searching client polls every 1.5 s and each poll
    used to cost a name lookup + skill query per queued player (3 DB connections each), i.e. O(clients x queue) database work per second."""
    q = MM.queued()
    if mode is not None and not modeq.forced():    # per-mode queues: a searcher only sees the players of its own mode
        q = [t for t in q if modeq.ModeQ.compat(_search_mode(t), mode)]
    key = (mode, tuple(q), _name_ver[0], len(PARTIES.of), tuple(sorted(PARTIES.leaders.items())))
    now = time.monotonic()
    c = _SF_CACHE
    if c["key"] == key and now - c["t"] < SF_CACHE_S:
        return c["ents"]
    t0, t1 = split_teams(q, {t: PARTIES.pid(t) for t in q if PARTIES.pid(t)}, player_skill(q) if os.environ.get("T2R_TEAM_BALANCE", "1") != "0" else None, top_players(q), rule_groups(q))
    lab = {t: " [T1]" for t in t0}
    lab.update({t: " [T2]" for t in t1})
    for t in q:                                    # a party shows on the searching page: " [T1] (party)", its leader " (party lead)"
        if PARTIES.pid(t) and len(PARTIES.members(t)) > 1:
            lab[t] = lab.get(t, "") + (" (party lead)" if PARTIES.is_leader(t) else " (party)")
    ents = [(t, (get_name(t) or "PLAYER").encode("utf-8", "ignore"), lab.get(t, "").encode()) for t in q]
    c.update(t=now, key=key, ents=ents)
    return ents

_LOBBY_ENT_CACHE = {}   # per-lobby searching-page rows. Its own name: it was _LB_CACHE, the same global as the leaderboard cache below, so its clear() wiped the leaderboard's keys (KeyError 'top' on every leaderboard request and menu announce)
def _lobby_entries(lid):
    """(LOBBY_BOOK) [(token, shown name bytes, team label bytes)] of one lobby (searching_frame lists only the viewer's lobby), cached like
    _search_entries."""
    with MM.lock:
        q = MM.book.members(lid)
    key = (lid, tuple(q), _name_ver[0], len(PARTIES.of), tuple(sorted(PARTIES.leaders.items())))
    now = time.monotonic()
    c = _LOBBY_ENT_CACHE.get(lid)
    if c and c["key"] == key and now - c["t"] < SF_CACHE_S:
        return c["ents"]
    t0, t1 = split_teams(q, {t: PARTIES.pid(t) for t in q if PARTIES.pid(t)}, player_skill(q) if os.environ.get("T2R_TEAM_BALANCE", "1") != "0" else None, top_players(q), rule_groups(q))
    lab = {t: " [T1]" for t in t0}
    lab.update({t: " [T2]" for t in t1})
    for t in q:
        if PARTIES.pid(t) and len(PARTIES.members(t)) > 1:
            lab[t] = lab.get(t, "") + (" (party lead)" if PARTIES.is_leader(t) else " (party)")
    ents = [(t, (get_name(t) or "PLAYER").encode("utf-8", "ignore"), lab.get(t, "").encode()) for t in q]
    if len(_LOBBY_ENT_CACHE) > 256:
        _LOBBY_ENT_CACHE.clear()
    _LOBBY_ENT_CACHE[lid] = {"t": now, "key": key, "ents": ents}
    return ents

def party_header(tok):
    """'PARTY n/4' row (n = party size, 4 = the team cap) for a token in a party of 2+; None otherwise."""
    if not tok or not PARTIES.pid(tok) or len(PARTIES.members(tok)) < 2:
        return None
    return b"PARTY %d/%d" % (len(PARTIES.members(tok)), PARTIES.MAX)

def party_lobby_names(tok):
    """The lobby list of a token on the MP menu (not searching): the party header + every party mate (a solo player: nothing). Sent as a bare
    op 0x17 (winmm just refills its name vector, whatever the session state: re/decomp_s3.c case 0x17) so the party shows before FIND MATCH."""
    hd = party_header(tok)
    if not hd:
        return []
    out = []                                       # (no 'PARTY n/4' header row)
    for t in PARTIES.members(tok):
        if t != tok:
            out.append((get_name(t) or "PLAYER").encode("utf-8", "ignore")[:24] + (b" (party lead)" if PARTIES.is_leader(t) else b" (party)"))
    return out[:20]

def lobby_list_frame(names):
    return msg(0x17, bytes([len(names)]) + b"".join(bytes([len(n)]) + n for n in names) + bytes(1))

def lobby_marks(tok):
    """ the token's lobby group: (members searching again, members still on the post-match
    screens) or None (no open ticket / T2R_LOBBY_KEEP=0 / T2R_REQUEUE=0). searching_frame marks the rows ' (lobby)' / ' (summary)'; t2r-ext cuts the
    suffix off and draws READY / IN SUMMARY in the row's status slot."""
    if not (REQUEUE and LOBBY_KEEP and tok):
        return None
    if book_on():
        # the rows of a post-match lobby: READY = back from that match, IN SUMMARY = still on its screens (seat kept), NEW = everybody else
        try:
            now = time.time()
            with MM.lock:
                lb = MM.book.get(MM.book.lobby_of(tok))
                if lb is None or lb.src is None:
                    return None
                mem = list(lb.members)
                back = [t for t in mem if MM.rq.mid_of(t) == lb.src]
                post = [t for t in MM.rq.tokens() if t not in MM.book.of and t not in MM.queue and MM.rq.mid_of(t) == lb.src and MM.rq.has(t)
                        and not MM._in_match(t)]
            return back, post
        except Exception as ex:                            # noqa: BLE001 - the rows never break the search
            log("LOBBY rows failed", repr(ex))
            return None
    try:
        with MM.lock:
            qd = list(MM.queue)
        now = time.time()
        return MM.rq.members(tok, qd, lambda t: silence(t, now), MM.in_match, now)
    except Exception as ex:                                # noqa: BLE001 - the rows never break the search
        log("LOBBY rows failed", repr(ex))
        return None

def searching_frame(self_tok=None):
    """Searching state for the client: 0x03 (state 3, also clears the lobby name list) then
    0x17 [u8 n][n x (u8 len, name)] = the players queued right now. The matchmaking screen
    shows its own name in row 0 (the 0x10 auth name) and then the list, skipping only an entry
    that strcmp-equals its own name (winmm FUN_180065760). The team label (" [T1]") broke that
 match, so the searcher saw itself twice ("Alpha" + "Alpha [T1]"): the
    recipient's own entry is left out here (by token, and by its own shown name), and every
    token / name is listed once (re/mm-boot.md)."""
    lid = None
    if book_on() and self_tok:
        with MM.lock:
            lid = MM.book.lobby_of(self_tok)
    ents = _lobby_entries(lid) if lid else _search_entries(_search_mode(self_tok) if self_tok else None)
    # the " [T1]" / " [T2]" text after a searcher's name is gone for the player - t2r-ext strips it from the row and draws a blue / red tab instead.
    # The marker is RELATIVE to the viewer here: [T1] = our team, [T2] = the enemy (an old ext without the tabs still shows a readable label).
    mine = next((lab for t, _b, lab in ents if t == self_tok and (b"[T1]" in lab or b"[T2]" in lab)), None) if self_tok else None
    if mine is not None:
        m1 = b"[T1]" in mine
        ents = [(t, b, lab.replace(b"[T1]", b"[X]").replace(b"[T2]", b"[Y]").replace(b"[X]", b"[T1]" if m1 else b"[T2]").replace(b"[Y]", b"[T2]" if m1 else b"[T1]")) for t, b, lab in ents]
    own = (get_name(self_tok) or "").encode("utf-8", "ignore")[:31] if self_tok else None
    lm = lobby_marks(self_tok)                     # the lobby group's rows: READY (searching again) / IN SUMMARY (still on the post-match screens)
    lm_back = set(lm[0]) if lm else set()
    names, seen_tok, seen_nm = [], set(), set()
    for t, base, lab in ents:
        if t == self_tok or (own and base[:31] == own) or t in seen_tok:
            continue
        n = base[:24] + lab + ((b" (lobby)" if t in lm_back else b" (new)") if lm else b"")      # (NEW: a searcher from the queue, the backfill)
        if n in seen_nm:
            continue
        seen_tok.add(t); seen_nm.add(n)
        names.append(n)
    for t in (lm[1] if lm else []):
        if t != self_tok and t not in seen_tok:
            n = (get_name(t) or "PLAYER").encode("utf-8", "ignore")[:24] + b" (summary)"
            if n not in seen_nm:
                seen_tok.add(t); seen_nm.add(n); names.append(n)
    if self_tok and PARTIES.pid(self_tok):         # party mates that are not searching yet are listed too (waiting for FIND MATCH)
        qs = {t for t, _, _ in ents}
        for t in PARTIES.members(self_tok):
            if t != self_tok and t not in qs:
                n = (get_name(t) or "PLAYER").encode("utf-8", "ignore")[:24] + (b" (party lead - not searching)" if PARTIES.is_leader(t) else b" (party - not searching)")
                if n not in seen_nm:
                    seen_nm.add(n); names.append(n)
        # ("one of the rows says PARTY 2/4?? like as if thats a player"): no header row - the FRIENDS strip under the list shows PARTY n/4
    # YOUR OWN TEAM is always on top, the enemy below (row 0 is us, then our team mates, then the enemy, then the not-searching party mates): a stable sort on the
    # relative marker, so the client's blue rows (our team) come first and its red rows (the enemy) last - whatever the order of the queue
    names.sort(key=lambda n: 0 if b" [T1]" in n else 1 if b" [T2]" in n else 2)
    names = names[:20]
    body = bytes([len(names)]) + b"".join(bytes([len(n)]) + n for n in names)
    return msg(0x03) + (msg(0x17, body + bytes(1)) if names else b"")

def lp(s):  # length-prefixed byte string
    return bytes([len(s)]) + s

# ---------------------------------------------------------------- storage
class RequestAbandoned(BaseException):
    """Raised by `with _dblock` when the request this thread serves passed its deadline (the client gave up on it: t2r_rpc waits <= 5 s).
    A BaseException on purpose: the handlers' own `except Exception` (feed_latest, rank_of_token ...) would otherwise turn it into an
    empty reply and CACHE that. tcp_conn catches it and closes the connection."""

_req = threading.local()             # .deadline = monotonic time after which this thread's request is no longer worth serving (0 = none)
ABANDONED = [0]                      # requests dropped at a _dblock wait (STATUS line)

class _DeadlineLock:
    """threading.Lock that a request thread with a deadline (tcp_conn, short read polls) waits on only until that deadline.
 overload: 417 threads of clients that had long given up (CLOSE-WAIT) still queued on _dblock, ran their request, then failed at send;
    the clients' retries kept the queue full until the 512 connection cap refused everyone."""
    def __init__(self):
        self._l = threading.Lock()
    def acquire(self, blocking=True, timeout=-1):
        return self._l.acquire(blocking, timeout)
    def release(self):
        self._l.release()
    def locked(self):
        return self._l.locked()
    def __enter__(self):
        dl = getattr(_req, "deadline", 0.0)
        if not dl:
            self._l.acquire()
            return self
        left = dl - time.monotonic()
        if left <= 0 or not self._l.acquire(timeout=left):
            ABANDONED[0] += 1
            raise RequestAbandoned()
        return self
    def __exit__(self, *exc):
        self._l.release()
        return False

_dblock = _DeadlineLock()
_schema_ok = {}      # DB path -> (inode, size>0) the base schema was verified for (soak: every call re-ran 3 CREATE TABLE + PRAGMA table_info; get_name opened 3 connections)
def _db_ident(path):
    try:
        st = os.stat(path)
        return (getattr(st, "st_ino", 0), st.st_size > 0)
    except OSError:
        return None
def db():
    c = sqlite3.connect(DB_PATH, timeout=10)
    c.execute("PRAGMA synchronous=FULL")        # explicit: every commit is fsynced (journal + db) - a power cut cannot half-write a clan save
    ident = _db_ident(DB_PATH)
    if ident is not None and _schema_ok.get(DB_PATH) == ident:
        return c
    c.execute("""CREATE TABLE IF NOT EXISTS players(
        token TEXT PRIMARY KEY, name TEXT, loadout BLOB,
        kills INT DEFAULT 0, deaths INT DEFAULT 0, wins INT DEFAULT 0,
        matches INT DEFAULT 0, steamid INTEGER DEFAULT 0, last_seen REAL)""")
    # tolerate an older DB created before the steamid column existed
    cols = [r[1] for r in c.execute("PRAGMA table_info(players)").fetchall()]
    if "steamid" not in cols:
        c.execute("ALTER TABLE players ADD COLUMN steamid INTEGER DEFAULT 0")
    c.execute("""CREATE TABLE IF NOT EXISTS hwids(
        hash TEXT, token TEXT, steamid INTEGER DEFAULT 0, name TEXT, first_seen REAL, last_seen REAL, PRIMARY KEY(hash, token))""")
    if "parts_total" not in cols:      # team balance: parts earned per finished match = the skill signal (kills are not recorded)
        c.execute("ALTER TABLE players ADD COLUMN parts_total INTEGER DEFAULT 0")
        c.execute("ALTER TABLE players ADD COLUMN parts_n INTEGER DEFAULT 0")
    if "draws" not in cols:            #: losses were matches - wins, so a draw (0x12 winner 2) was a loss
        c.execute("ALTER TABLE players ADD COLUMN draws INTEGER DEFAULT 0")
    # hot lookups that were full scans: token_steamid / ban checks / name_taken read players by steamid, hwid checks read hwids by token / steamid
    c.execute("CREATE INDEX IF NOT EXISTS players_steamid ON players(steamid)")
    c.execute("CREATE INDEX IF NOT EXISTS hwids_token ON hwids(token)")
    c.execute("CREATE INDEX IF NOT EXISTS hwids_steamid ON hwids(steamid)")
    c.commit()
    _schema_ok[DB_PATH] = _db_ident(DB_PATH)
    return c

@contextlib.contextmanager
def dbc():
    """`with _dblock, dbc() as c:` = db() that commits on success, rolls back on an error and ALWAYS closes the connection (sqlite3's own
    `with conn:` only commits: the connection lived until the garbage collector got it, kept alive by any traceback that referenced it)."""
    c = db()
    try:
        yield c
        c.commit()
    except BaseException:
        try: c.rollback()
        except sqlite3.Error: pass
        raise
    finally:
        c.close()

# Lock-free reads (overload): the hot lookups (shown name, Steam id, known token, rank) are single SELECTs. They used to wait for the
# global _dblock behind every writer, the backup and the other readers, and opened a new connection each time. sqlite itself keeps a reader
# consistent (rollback journal: a SELECT sees the last commit; it only waits out a writer's commit, in the busy handler). So they run on a
# small pool of read-only connections without _dblock. T2R_DB_RO_READS=0 = the old locked path.
DB_RO_READS = os.environ.get("T2R_DB_RO_READS", "1") != "0"
_ro_pool = []                        # idle read-only connections (check_same_thread=False: one thread at a time uses each)
_ro_pool_lock = threading.Lock()
RO_POOL_MAX = 8
RO_READS = [0, 0]                    # [lock-free reads, fallbacks to the locked path]

def _ro_get():
    """-> (connection, file identity). A pooled connection opened on another file (t2r.db replaced: db_guard restore, a test) is closed and
    never reused: on Linux it would go on reading the old, unlinked file."""
    ident = _db_ident(DB_PATH)
    if ident is None or not ident[1]:
        raise sqlite3.OperationalError("no database file yet")
    while True:
        with _ro_pool_lock:
            e = _ro_pool.pop() if _ro_pool else None
        if e is None:
            break
        if e[1] == ident:
            return e
        e[0].close()
    path = os.path.abspath(DB_PATH).replace("\\", "/")
    return sqlite3.connect("file:%s?mode=ro" % path, uri=True, timeout=10, check_same_thread=False), ident

def _ro_put(e):
    with _ro_pool_lock:
        if len(_ro_pool) < RO_POOL_MAX:
            _ro_pool.append(e)
            return
    e[0].close()

def ro_close_all():
    """Close the idle read-only connections (before t2r.db is replaced: Windows cannot rename an open file)."""
    with _ro_pool_lock:
        idle = list(_ro_pool)
        del _ro_pool[:]
    for c, _i in idle:
        try: c.close()
        except sqlite3.Error: pass

def db_read(fn):
    """fn(connection) -> value, for a pure read (SELECTs only). Runs on a pooled read-only connection without _dblock; any sqlite error there
    (no DB file yet, a table not created yet, a write by mistake) falls back to the old `with _dblock, dbc()` path, so a read never fails
    where it used to work."""
    if DB_RO_READS:
        e = None
        try:
            e = _ro_get()
            v = fn(e[0])
            RO_READS[0] += 1
            _ro_put(e)
            return v
        except sqlite3.Error:
            if e is not None:
                try: e[0].close()
                except sqlite3.Error: pass
            RO_READS[1] += 1
    with _dblock, dbc() as c:
        return fn(c)

# Shown-name / Steam-id cache: get_name() is called for every queued player on every searching poll (searching_frame: N searchers x N polls
# per 1.5 s) and cost 3 DB connections + ~8 statements each. Entries live NAME_CACHE_S seconds and are dropped at once by every writer
# (set_name / set_steam / the in-game rename) through name_dirty().
NAME_CACHE_S = float(os.environ.get("T2R_NAME_CACHE_S", "3"))
_name_cache = {}      # token -> (expires monotonic, generation, value, names key)
_steam_cache = {}
_name_gen = [0]       # global generation: name_dirty() with no token drops every entry
_name_ver = [0]       # bumped by every name_dirty() (keys of caches built from many names, e.g. _search_entries)
_tok_gen = {}         # token -> its own generation: name_dirty(token) drops only that player's entries
# overload: name_dirty used to drop the WHOLE cache, and set_steam runs on every Steam helper registration, so each login made every
# online player's next name lookup go to the DB at once (thousands of _dblock waits). Now a writer drops only the tokens it changed.
def _gen_of(token):
    return (_name_gen[0], _tok_gen.get(token, 0))
def name_dirty(token=None, key=None):
    """Drop cached names. token: that player's entries only. key: also every token whose custom name lives under this names key ("s:<steamid>"
    or a token id) - a rename under a Steam key changes the shown name of every install of that account. Neither: everything (old behaviour)."""
    _name_ver[0] += 1
    if token is None and key is None:
        _name_gen[0] += 1
        return
    toks = set()
    if token:
        toks.add(token)
    if key:
        for t, e in list(_name_cache.items()):
            if e[3] == key or names.token_id(t) == key:
                toks.add(t)
    if len(_tok_gen) > 8192:                       # bounded: start over with a global drop
        _tok_gen.clear()
        _name_gen[0] += 1
        return
    for t in toks:
        _tok_gen[t] = _tok_gen.get(t, 0) + 1
def _cache_get(cache, token):
    e = cache.get(token)
    if e is not None and e[1] == _gen_of(token) and time.monotonic() < e[0]:
        return True, e[2]
    return False, None
def _cache_put(cache, token, value, gen=None, key=None):
    """gen = _gen_of(token) taken BEFORE the DB read: a writer that commits after that makes the entry invalid, so a reader can never cache a
    value it read before a rename / Steam registration."""
    if len(cache) > 4096:
        cache.clear()
    cache[token] = (time.monotonic() + NAME_CACHE_S, _gen_of(token) if gen is None else gen, value, key)

# Power-loss safety: sqlite commits are journaled + fsynced, but a power cut can still leave the file zeroed / truncated by the
# filesystem (git refs were zeroed the same way). So: a verified rolling backup (sqlite backup API -> temp file ->
# fsync -> atomic replace, written only if the copy passes integrity_check, so a damaged db never overwrites a good backup),
# and a start-up check that restores it when t2r.db is empty or corrupt. (Clans, names, loadouts, stats all live in t2r.db.)
BAK_PATH = DB_PATH + ".bak"

# A backup of a large DB on a small server (1 vCPU / 1 GB) took minutes when it ran every 5 min with a full integrity_check: requests waited
# on the lock, the connection cap filled and players lost the server. Now every 30 min, and the copy is checked with quick_check
# (same page / record checks, without the index cross-check that read the whole file again).
DB_BACKUP_S = float(os.environ.get("T2R_DB_BACKUP_S", "1800"))

def _db_ok(path, quick=False):
    try:
        if os.path.getsize(path) == 0:
            return False
        c = sqlite3.connect(path)
        try:
            return c.execute("PRAGMA quick_check" if quick else "PRAGMA integrity_check").fetchone()[0] == "ok"
        finally:
            c.close()
    except (OSError, sqlite3.Error):
        return False

def db_guard():
    """At start: restore t2r.db from the backup when it is missing its data (zeroed / corrupt). Returns a status string."""
    if not os.path.exists(DB_PATH):
        return "no db yet"
    if _db_ok(DB_PATH, quick=True):
        return "ok"
    if _db_ok(BAK_PATH, quick=True):
        ro_close_all()
        bad = DB_PATH + ".corrupt-%d" % int(time.time())
        os.replace(DB_PATH, bad)
        import shutil
        shutil.copyfile(BAK_PATH, DB_PATH + ".restore")
        with open(DB_PATH + ".restore", "rb+") as f:
            os.fsync(f.fileno())
        os.replace(DB_PATH + ".restore", DB_PATH)
        return "CORRUPT - restored from the backup (damaged file kept as %s)" % os.path.basename(bad)
    return "CORRUPT and no usable backup"

def db_backup():
    """Rolling backup of t2r.db (write temp + fsync + atomic rename). True when a new verified backup is in place."""
    tmp = BAK_PATH + ".tmp"
    try:
        # the integrity check of the LIVE file used to run here, under _dblock (seconds on fra1 with a 66 MB DB, every 5 min: every
        # request waited). The copy is checked instead (below, outside the lock): the backup API copies the pages as they are, so a damaged
        # source gives a damaged copy and the good backup is kept just the same. Only the page copy holds the lock (no commit in the middle).
        if os.path.getsize(DB_PATH) == 0:        # never copy an emptied db over the good backup
            return False
        with _dblock:
            src = sqlite3.connect(DB_PATH, timeout=10)
            try:
                dst = sqlite3.connect(tmp)
                try:
                    src.backup(dst)
                finally:
                    dst.close()
            finally:
                src.close()
        if not _db_ok(tmp, quick=True):
            os.remove(tmp)
            return False
        with open(tmp, "rb+") as f:
            os.fsync(f.fileno())
        os.replace(tmp, BAK_PATH)
        return True
    except (OSError, sqlite3.Error) as e:
        log("DB backup failed", repr(e))
        return False

DB_MAINT_S = float(os.environ.get("T2R_DB_MAINT_S", str(24 * 3600)))
_db_maint_last = [0.0]
def db_maintenance(force=False):
    """Space + planner upkeep of the long-running DB: PRAGMA optimize (refresh the planner statistics of the new indexes) and a VACUUM when more than
    a quarter of the file is free pages (deleted clan feed rows / players leave holes the rollback-journal DB never gives back). Runs under the DB lock
    (the file is a few MB: milliseconds) and so never in the middle of a backup or a write. Returns a short status text."""
    try:
        with _dblock:
            c = sqlite3.connect(DB_PATH, timeout=10)
            try:
                c.execute("PRAGMA optimize")
                pages = c.execute("PRAGMA page_count").fetchone()[0]
                free = c.execute("PRAGMA freelist_count").fetchone()[0]
                if pages and (force or (pages >= 256 and free * 4 > pages)):
                    c.execute("VACUUM")
                    return "vacuumed (%d of %d pages were free)" % (free, pages)
                return "ok (%d of %d pages free)" % (free, pages)
            finally:
                c.close()
    except (sqlite3.Error, OSError) as e:
        return "failed: %r" % (e,)

def db_maintenance_tick(now=None):
    now = time.time() if now is None else now
    if not _db_maint_last[0]:
        _db_maint_last[0] = now - DB_MAINT_S + 3600.0      # first pass an hour after the start, then daily
        return None
    if now - _db_maint_last[0] < DB_MAINT_S:
        return None
    _db_maint_last[0] = now
    r = db_maintenance()
    log("DB maintenance:", r)
    return r

def save_loadout(token, blob):
    with _dblock, dbc() as c:
        c.execute("INSERT INTO players(token,loadout,last_seen) VALUES(?,?,?) "
                  "ON CONFLICT(token) DO UPDATE SET loadout=excluded.loadout,last_seen=excluded.last_seen",
                  (token, blob, time.time()))

def set_name(token, name):
    name_dirty(token)
    with _dblock, dbc() as c:
        c.execute("INSERT INTO players(token,name,last_seen) VALUES(?,?,?) "
                  "ON CONFLICT(token) DO UPDATE SET name=excluded.name,last_seen=excluded.last_seen",
                  (token, name, time.time()))
    name_dirty(token)                              # (again after the commit: a reader between the first call and the write cached the old value)

def set_steam(token, steamid, name):
    name_dirty(token)
    with _dblock, dbc() as c:
        c.execute("INSERT INTO players(token,name,steamid,last_seen) VALUES(?,?,?,?) "
                  "ON CONFLICT(token) DO UPDATE SET name=excluded.name,"
                  "steamid=excluded.steamid,last_seen=excluded.last_seen",
                  (token, name, steamid, time.time()))
    name_dirty(token)                              # (only this token's players row changed: its name / Steam id / names key)

NAME_TAG = b"T2N:"

def make_token(name):
    """64-hex token carrying a player name: hex(b"T2N:" + name + b"\0" + random).
    The client only accepts hex (it stops at the first non-hex char) and never sends
    its own name, so the name rides inside the token."""
    raw = NAME_TAG + name.encode("utf-8")[:15] + b"\0"
    return (raw + secrets.token_bytes(32 - len(raw))).hex()

def token_name(token):
    """Name embedded by make_token(), else None."""
    try:
        raw = bytes.fromhex(token)
    except (ValueError, TypeError):
        return None
    if raw.startswith(NAME_TAG):
        return raw[len(NAME_TAG):].split(b"\0")[0].decode("utf-8", "ignore")[:15] or None
    return None

def tag(token):
    """Short log label for a token: embedded name if any, else hex prefix."""
    return token_name(token) or (token or "")[:8]

def token_steamid(token):
    """Steam id the bridge reported for this token (0 = none known)."""
    if not token:
        return 0
    hit, v = _cache_get(_steam_cache, token)
    if hit:
        return v
    gen = _gen_of(token)
    r = db_read(lambda c: c.execute("SELECT steamid FROM players WHERE token=?", (token,)).fetchone())
    v = int(r[0]) if r and r[0] else 0
    _cache_put(_steam_cache, token, v, gen)
    return v

# Bans: one entry per line in bans.txt next to the database (outlives the PT folders): a Steam id (7656...), a
# player name (case-insensitive; a name entry also bans every Steam account that ever used that name) or "hwid:<hex>" (a hardware
# id the client reported, see "HWID bans"). '#' = comment. Re-read when the file changes.
# "ip:<address>" or "ip:<network/prefix>" (IPv4 or IPv6, e.g.
# ip:203.0.113.7 or ip:203.0.113.0/24) refuses every connection from that address / range, whatever the account; "pid:P-XXXXXXXX"
# bans a whole identity (ident.py: every Steam id, install and hardware id tied to it, and any id merged into it later).
BAN_PATH = os.path.join(os.path.dirname(os.path.abspath(DB_PATH)), "bans.txt")
BAN_MSG = b"You are banned from the server."          # English form; the live path sends ban_note(token) (the token's language)
def ban_note(token):
    # a per-ban reason ("# ... reason: <text>" on the bans.txt line) is shown after the banned note
    if not token:
        return BAN_MSG
    base = srv_note("banned", token)
    rs = _bans.get("reason") or {}
    why = None
    try:
        sid = BRIDGES.steamid(token) or token_steamid(token)
        why = rs.get(str(sid)) if sid else None
        if not why:
            with _dblock, dbc() as c:
                r = c.execute("SELECT name, steamid FROM players WHERE token=?", (token,)).fetchone()
            if r:
                why = rs.get(str(r[1])) or rs.get((r[0] or "").lower())
    except Exception as ex:
        log("BANS reason lookup failed", ex)
    return (base + b" " + why.encode("utf-8", "replace"))[:0xf0] if why else base
_bans = {"mtime": None, "ids": set(), "names": set(), "hw": set(), "tok": set(), "ip": [], "pid": set(), "reason": {}}

def _bans_load():
    try:
        m = os.path.getmtime(BAN_PATH)
    except OSError:
        _bans.update(mtime=None, ids=set(), names=set(), hw=set(), tok=set(), ip=[], pid=set())
        return
    if m == _bans["mtime"]:
        return
    ids, nms, hw, tk = set(), set(), set(), set()
    ipn, pids = [], set()
    reason = {}
    for line in open(BAN_PATH, encoding="utf-8", errors="ignore"):
        e = line.split("#", 1)[0].strip()
        if not e:
            continue
        cm = line.split("#", 1)[1] if "#" in line else ""
        if "reason:" in cm.lower():
            reason[e if e.isdigit() else e.lower()] = cm[cm.lower().index("reason:") + 7:].strip()
        if e.lower().startswith("token:"):             # anti-cheat review (tools/reports/review.py): the 64-hex name token of a banned install
            t = e[6:].strip().lower()
            if len(t) == 64 and all(ch in "0123456789abcdef" for ch in t):
                tk.add(t)
            continue
        if e.lower().startswith("ip:"):
            try:
                ipn.append(ipaddress.ip_network(e[3:].strip(), strict=False))
            except ValueError:
                log("BANS bad ip entry", e)
            continue
        if e.lower().startswith("pid:"):
            pids.add(e[4:].strip().upper())
            continue
        if e.lower().startswith("hwid:"):
            h = e[5:].strip().lower()
            if len(h) >= HWID_HEX and all(ch in "0123456789abcdef" for ch in h):
                hw.add(h[:HWID_HEX])
            continue
        (ids if e.isdigit() and len(e) >= 15 else nms).add(e if e.isdigit() else e.lower())
    if nms:
        try:
            with _dblock, dbc() as c:
                for n in nms:
                    ids |= {str(r[0]) for r in c.execute("SELECT steamid FROM players WHERE lower(name)=? AND steamid", (n,))}
        except Exception as ex:
            log("BANS name lookup failed", ex)
    if nms and reason:
        try:
            with _dblock, dbc() as c:
                for n in nms:
                    if n in reason:
                        for r in c.execute("SELECT steamid FROM players WHERE lower(name)=? AND steamid", (n,)):
                            reason.setdefault(str(r[0]), reason[n])
        except Exception as ex:
            log("BANS reason name lookup failed", ex)
    _bans.update(mtime=m, ids=ids, names=nms, hw=hw, tok=tk, ip=ipn, pid=pids, reason=reason)
    log("BANS loaded", len(ids), "steam id(s),", len(nms), "name(s),", len(hw), "hwid(s),", len(tk), "token(s),", len(ipn), "ip / range(s),",
        len(pids), "identity id(s) from", BAN_PATH)

# --------------------------------------------------- HWID bans
# The client (t2r-ext hwid.c; spec's loader/hwid.cpp sources: MachineGuid, SMBIOS UUID, a fixed disk serial) reports on its own short
# connection:  client -> server  0xB2 [64B token][u8 ver=1][u8 mask b0 guid b1 smbios b2 disk][12 B per set bit]   reply 0xB3 "ok=1"
# Each 12 B component is kept as 24 hex chars in the hwids table with the token / Steam id / name it came with. A ban entry (Steam
# id, name, hwid) bans every hardware id ever seen with that account, and a banned hardware id bans every account that ever
# reported it (one hop: the other hardware ids of those accounts are not followed). Clients that never report (older) are only
# checked by Steam id / name.
OP_HWID, OP_HWID_REPLY = 0xB2, 0xB3
HWID_HEX = 24

# PT60 pre-mortem: one shared component used to ban every account that reported it, but cloned Windows images share MachineGuid,
# OEM boards ship placeholder SMBIOS UUIDs / disk serials ("To Be Filled By O.E.M.", "0000...", "Default string"), and some SSD batches
# share a serial string. Now (a) known placeholder values (hashed exactly as the client does: sha256("T2R-HWID1" + tag + data)[:12]) are
# ignored on both sides of the comparison, (b) an account that is banned only through ANOTHER account's hardware needs >= HWID_MIN_MATCH
# matching components (an explicit "hwid:<hex>" entry in bans.txt still bans on its own).
HWID_MIN_MATCH = 2
def _hw_placeholders():
    import hashlib
    def h(tag, data):
        return hashlib.sha256(b"T2R-HWID1" + bytes([tag]) + data).digest()[:12].hex()
    out = set()
    for s in ("", "To Be Filled By O.E.M.", "To be filled by O.E.M.", "Default string", "Default String", "None", "none", "Unknown", "unknown",
              "N/A", "NA", "Not Specified", "Not Applicable", "System Serial Number", "SerialNumber", "Serial Number", "123456789", "1234567890",
              "0123456789", "00000000", "0000000000", "0", "000000000000", "0000_0000_0000_0000", "0000_0000_0000_0000.", "XXXXXXXX",
              "xxxxxxxx", "INVALID", "Invalid", "OEM", "O.E.M.", "ChangeMe", "default", "Default", "VMware Virtual disk", "QEMU HARDDISK"):
        out.add(h(3, s.encode()))
    for n in (8, 16, 20, 32, 40):
        out.add(h(3, b"0" * n)); out.add(h(3, b"F" * n)); out.add(h(3, b"f" * n)); out.add(h(3, b" " * n))
    for u in (bytes(16), b"\xff" * 16, bytes.fromhex("00020003000400050006000700080009"), bytes.fromhex("03000200040005000006000700080009"),
              bytes.fromhex("11111111111111111111111111111111"), bytes.fromhex("0123456789abcdef0123456789abcdef"),
              bytes.fromhex("fedcba9876543210fedcba9876543210")):
        out.add(h(2, u))
    return out
HWID_PLACEHOLDERS = _hw_placeholders()

def parse_hwid_report(pl):
    """0xB2 payload -> (token, [hex component, ...]) or None when malformed."""
    if len(pl) < 66 or pl[64] != 1:
        return None
    tok = pl[0:64].decode("latin1", "ignore")
    if not all(ch in "0123456789abcdefABCDEF" for ch in tok):
        return None
    mask = pl[65]
    if mask & ~7:
        return None
    n = bin(mask).count("1")
    if mask == 0 or len(pl) < 66 + 12 * n:
        return None
    return tok, [pl[66 + 12 * i:78 + 12 * i].hex() for i in range(n)]

def hwid_record(token, hashes):
    sid = BRIDGES.steamid(token) or token_steamid(token)
    nm = get_name(token) or ""
    now = time.time()
    with _dblock, dbc() as c:
        for h in hashes:
            c.execute("INSERT INTO hwids(hash,token,steamid,name,first_seen,last_seen) VALUES(?,?,?,?,?,?) "
                      "ON CONFLICT(hash,token) DO UPDATE SET steamid=CASE WHEN excluded.steamid THEN excluded.steamid ELSE steamid END,"
                      "name=excluded.name,last_seen=excluded.last_seen", (h, token, sid, nm, now, now))

def hwid_op(pl, ip=None):
    r = parse_hwid_report(pl)
    if not r:
        return msg(OP_HWID_REPLY, b"ok=0")
    tok, hashes = r
    try:
        hwid_record(tok, hashes)
    except Exception as e:
        log("HWID record failed", repr(e))
    log("HWID", tag(tok), len(hashes), "component(s)", " ".join(h[:6] for h in hashes))
    ident_note(tok, hwids=hashes, ip=ip)
    return msg(OP_HWID_REPLY, b"ok=1")

def banned_hwids():
    """Explicit hwid entries + every hardware id ever seen with a banned Steam id / name / (token of those)."""
    _bans_load()
    e, d = banned_hwid_sets()
    return e | d

def banned_hwid_sets():
    """(explicit hwid: entries, hardware ids derived from banned accounts), placeholders removed from both."""
    _bans_load()
    hw = set(_bans["hw"]) - HWID_PLACEHOLDERS
    derived = set()
    ids = {int(i) for i in _bans["ids"]}
    if not ids and not _bans["names"]:
        return hw, derived
    with _dblock, dbc() as c:
        toks = set()
        for i in ids:
            toks |= {r[0] for r in c.execute("SELECT token FROM players WHERE steamid=?", (i,))}
            derived |= {r[0] for r in c.execute("SELECT hash FROM hwids WHERE steamid=?", (i,))}
        for n in _bans["names"]:
            toks |= {r[0] for r in c.execute("SELECT token FROM players WHERE lower(name)=?", (n,))}
            toks |= {r[0] for r in c.execute("SELECT token FROM hwids WHERE lower(name)=?", (n,))}
        for t in toks:
            derived |= {r[0] for r in c.execute("SELECT hash FROM hwids WHERE token=?", (t,))}
    return hw, derived - HWID_PLACEHOLDERS - hw

def token_hwids(token, sid=0):
    with _dblock, dbc() as c:
        h = {r[0] for r in c.execute("SELECT hash FROM hwids WHERE token=?", (token,))}
        if sid:
            h |= {r[0] for r in c.execute("SELECT hash FROM hwids WHERE steamid=?", (sid,))}
    return h

# ---------------------------------------------------------------- identities (ident.py, optional)
_IDENT_SEEN = {}        # (token, steamid, ip, hwids) -> time of the last write (one write per IDENT_EVERY_S per combination)
IDENT_EVERY_S = 600.0

def ident_note(token, steamid=0, hwids=(), ip=None):
    """Records what was seen together (ident.link) and logs the identity id when it is new, changes or joins another. Never raises."""
    if ident is None or not token or len(token) != 64:
        return None
    if ip and (ip.startswith("127.") or ip == "::1"):
        ip = None
    key = (token, int(steamid or 0), ip, tuple(sorted(hwids)))
    now = time.time()
    if now - _IDENT_SEEN.get(key, 0) < IDENT_EVERY_S:
        return None
    _IDENT_SEEN[key] = now
    ident_prune(now)
    if len(_IDENT_SEEN) > 20000:
        for k in [k for k, t in _IDENT_SEEN.items() if now - t > IDENT_EVERY_S][:10000]:
            _IDENT_SEEN.pop(k, None)
    try:
        sid = int(steamid or 0) or (BRIDGES.steamid(token) or token_steamid(token) or 0)
        with _dblock, dbc() as c:
            pid, merged, created = ident.link(c, token=token, steamid=sid, hwids=hwids, ip=ip, placeholders=HWID_PLACEHOLDERS, now=now)
        if pid:
            log("IDENT", pid, tag(token), "steam", sid or "-", "ip", ip or "-", "hwid", len(hwids),
                "(new identity)" if created else "", ("joined " + " ".join(merged)) if merged else "")
        return pid
    except Exception as ex:
        log("IDENT failed", repr(ex))
        return None

_IDENT_PRUNE = [0.0]

def ident_prune(now=None):
    """Once a day: IP addresses not seen for T2R_IP_KEEP_DAYS (180) are forgotten."""
    now = now or time.time()
    if ident is None or now - _IDENT_PRUNE[0] < 86400:
        return
    _IDENT_PRUNE[0] = now
    try:
        with _dblock, dbc() as c:
            n = ident.prune_ips(c, now=now)
        if n:
            log("IDENT forgot", n, "ip address row(s) older than", ident.IP_KEEP_DAYS, "days")
    except Exception as ex:
        log("IDENT prune failed", repr(ex))

def ident_backfill():
    """One time: every account recorded before ident.py (players + hwids) gets its identity, 200 accounts per lock hold."""
    if ident is None:
        return
    try:
        with _dblock, dbc() as c:
            ident.ensure_schema(c)
            c.execute("CREATE TABLE IF NOT EXISTS ident_meta(k TEXT PRIMARY KEY, v TEXT)")
            r = c.execute("SELECT v FROM ident_meta WHERE k='backfill'").fetchone()
        after = r[0] if r else ""
        if after == "done":
            return
        n = ids = 0
        while True:
            with _dblock, dbc() as c:
                toks = ident.backfill_tokens(c, after)
                for t in toks:
                    pid, merged, created = ident.backfill_one(c, t, HWID_PLACEHOLDERS)
                    ids += bool(created)
                    if merged:
                        log("IDENT backfill", pid, tag(t), "joined", " ".join(merged))
                after = toks[-1] if toks else "done"
                c.execute("INSERT OR REPLACE INTO ident_meta(k, v) VALUES('backfill', ?)", (after,))
            n += len(toks)
            if not toks:
                break
            time.sleep(0.05)
        log("IDENT backfill done:", n, "account(s),", ids, "new identity id(s)")
    except Exception as ex:
        log("IDENT backfill failed", repr(ex))

def ident_pid(token, steamid=0):
    if ident is None:
        return None
    try:
        sid = int(steamid or 0) or (BRIDGES.steamid(token) or token_steamid(token) or 0)
        with _dblock, dbc() as c:
            return ident.pid_of(c, token=token, steamid=sid)
    except Exception as ex:
        log("IDENT lookup failed", repr(ex))
        return None

def ip_banned(ip):
    """The bans.txt ip: entry that covers this address, or None."""
    _bans_load()
    if not ip or not _bans["ip"]:
        return None
    try:
        a = ipaddress.ip_address(ip.split("%", 1)[0])
    except ValueError:
        return None
    if getattr(a, "ipv4_mapped", None):
        a = a.ipv4_mapped
    for n in _bans["ip"]:
        if a.version == n.version and a in n:
            return n
    return None

def is_banned(token, name=None, ip=None):
    _bans_load()
    if ip and ip_banned(ip):
        return True
    if not _bans["ids"] and not _bans["names"] and not _bans["hw"] and not _bans["tok"] and not _bans["pid"]:
        return False
    if token and token.lower() in _bans["tok"]:
        return True
    if token and _bans["pid"] and ident is not None:
        try:
            with _dblock, dbc() as c:
                live = {ident.resolve(c, p) for p in _bans["pid"]}
                mine = ident.pid_of(c, token=token, steamid=BRIDGES.steamid(token) or token_steamid(token) or 0)
            if mine and mine in live:
                return True
        except Exception as ex:
            log("BANS identity check failed", repr(ex))
    sid = BRIDGES.steamid(token) or token_steamid(token)
    if sid and str(sid) in _bans["ids"]:
        return True
    nm = (get_name(token) or name or "").lower()
    if nm and nm in _bans["names"]:
        return True
    try:
        mine = token_hwids(token, sid) - HWID_PLACEHOLDERS
        if not mine:
            return False
        explicit, derived = banned_hwid_sets()
        return bool(mine & explicit) or len(mine & derived) >= HWID_MIN_MATCH
    except Exception as ex:
        log("BANS hwid check failed", repr(ex))
        return False

def name_key(token):
    """Key of a player's custom name: the Steam account when known ("s:<steamid>": the name follows the player to every install / new
 token; token ids are NOT unique - a shipped token was shared by 5 players), else the token id."""
    sid = token_steamid(token)
    return ("s:%d" % sid) if sid else names.token_id(token)

def custom_name(token):
    """(name, tag) the player set in-game (MULTIPLAYER > OPTIONS > CHANGE NAME), keyed by Steam account (else token id)."""
    if not token or len(token) != 64:
        return "", ""
    key = name_key(token)
    def rd(c):
        cn, ct = names.load(c, key)
        if not cn and key.startswith("s:"):           # a name saved before the Steam keying: the token id's
            cn, ct = names.load(c, names.token_id(token))
        return cn, ct
    return db_read(rd)

def safe_name(nm):
    """A shown name never carries '<' / '>'. winmm's text routines read '<' as a markup tag start (<color=> / <scale>): a Steam name
 with angle brackets crashed every client that drew it (nametag, kill feed: exe+123bb4d frame allocator full,, 8 crashes in
    one evening). Clients copy names from the daemon (0x10 own name, lobby / match roster), so it is cleaned here."""
    if not nm or ("<" not in nm and ">" not in nm):
        return nm
    return nm.replace("<", "").replace(">", "").strip() or "PLAYER"

def get_name(token):
    """Shown name: custom name (with [TAG]) > Steam name (bridge/DB) > T2N setup name."""
    if not token:
        return None
    hit, v = _cache_get(_name_cache, token)
    if hit:
        return v
    gen = _gen_of(token)                           # (before the read: see _cache_put)
    v = safe_name(_get_name(token))
    _cache_put(_name_cache, token, v, gen, name_key(token) if len(token) == 64 else None)
    return v

def _get_name(token):
    cn, ctag = custom_name(token)
    if cn:
        return names.display(cn, ctag)
    tn = token_name(token)
    if tn:
        return tn
    r = db_read(lambda c: c.execute("SELECT name FROM players WHERE token=?", (token,)).fetchone())
    return r[0] if r and r[0] else None

def get_loadout(token):
    with _dblock, dbc() as c:
        r = c.execute("SELECT loadout FROM players WHERE token=?", (token,)).fetchone()
        return r[0] if r and r[0] else None

def leaderboard(top=10):
    # one row per player: the Steam account when known (every install / token of one player adds up - the same names
    # showed many times), else the token id (bytes 20..31: a rename changes the T2N token but keeps stats); shown with the name of
    # the newest token. Rows that would still show the same name twice are merged too.
    with _dblock, dbc() as c:
        rows = c.execute("SELECT MAX(last_seen),token,name,SUM(kills) k,SUM(deaths),SUM(wins) w,SUM(matches) - SUM(draws) FROM players "
                         "WHERE length(token) = 64 AND (matches > 0 OR token LIKE '54324e3a%') "
                         "GROUP BY CASE WHEN steamid > 0 THEN 's' || steamid ELSE substr(token,41,24) END").fetchall()
    best = {}
    for r in rows:
        nm = get_name(r[1]) or r[2] or "PLAYER"
        k = nm.lower()
        if k in best:
            b = best[k]
            best[k] = (b[0], nm, b[2] + (r[3] or 0), b[3] + (r[4] or 0), b[4] + (r[5] or 0), b[5] + (r[6] or 0))
        else:
            best[k] = (r[1], nm, r[3] or 0, r[4] or 0, r[5] or 0, r[6] or 0)
    out = sorted(best.values(), key=lambda x: (-x[4], -x[2]))
    return out[:top]

def name_taken(c, key, name):
    """Another player already shows this name (custom name of another key, or the Steam name of another account)."""
    n = (name or "").strip().lower()
    if not n:
        return False
    names.init(c)
    sid = int(key[2:]) if key.startswith("s:") else -1
    own = {key} | {names.token_id(r[0]) for r in c.execute("SELECT token FROM players WHERE steamid=?", (sid,))}   # the account's own rows
    if any(r[0] not in own for r in c.execute("SELECT token_id FROM names WHERE lower(name)=?", (n,))):
        return True
    return bool(c.execute("SELECT 1 FROM players WHERE lower(name)=? AND steamid>0 AND steamid<>?", (n, sid)).fetchone())

def add_matches(tokens):
    with _dblock, dbc() as c:
        for t in tokens:
            c.execute("INSERT INTO players(token,name,matches,last_seen) VALUES(?,?,1,?) "
                      "ON CONFLICT(token) DO UPDATE SET matches=matches+1,last_seen=excluded.last_seen",
                      (t, token_name(t), time.time()))


# ---------------------------------------------------------------- Elo (in-game leaderboard + website)
# The old leaderboard "elo" was 1000 + 25 x (wins - losses): it ignored who you played, never moved for a draw and clamped at 0.
# Now: one rating per account (Steam id, else the install token id - the leaderboard's grouping), updated on every match result (host 0x12)
# as a team Elo: each side's strength = its players' mean rating, expected score E = 1 / (1 + 10^((other - own) / 400)), result S = 1 win /
# 0.5 draw / 0 loss, change K x (S - E) for every player of the side, K = 40 for an account's first 20 rated matches, then 24.
# Leavers (record_result skip) are not rated. An account without a rating is seeded from its record so far: the rating its win rate implies
# against the average player (the pool's win rate = 1000; losses include abandoned matches, so the pool wins < 50 %), with the win rate pulled
# toward the pool's by ELO_PRIOR matches (11 matches at 64 % is not proof).
# Shown rating (leaderboards, website) = Elo - ELO_UNSURE / sqrt(matches + 1): "L played 11 matches, match count should be looked
# at too" - a rating from few matches is unsure, so it ranks lower until the matches back it up (11 matches -58, 100 matches -20).
ELO_BASE, ELO_K_NEW, ELO_K, ELO_NEW_N = 1000.0, 40.0, 24.0, 20
ELO_PRIOR, ELO_UNSURE = 40.0, 200.0
_elo_pool = {"t": 0.0, "p": 0.4}


def elo_pool_rate(c):
    """Win rate of all matches booked (cached 10 min); the seed's 1000 point."""
    if time.time() - _elo_pool["t"] > 600:
        w, m = c.execute("SELECT SUM(wins), SUM(matches) FROM players").fetchone() or (0, 0)
        if m:
            _elo_pool["p"] = min(0.9, max(0.1, (w or 0) / m))
        _elo_pool["t"] = time.time()
    return _elo_pool["p"]


def elo_shown(e, matches):
    return e - ELO_UNSURE / math.sqrt(max(0, matches) + 1)


def elo_key(c, token):
    r = c.execute("SELECT steamid FROM players WHERE token=?", (token,)).fetchone()
    sid = int(r[0]) if r and r[0] else 0
    return ("s%d" % sid if sid else "t" + token[40:64].lower()), sid


def elo_seed(c, key, sid, token, record=None):
    q = ("SELECT SUM(wins), SUM(matches), SUM(draws) FROM players WHERE steamid=?", (sid,)) if sid else \
        ("SELECT SUM(wins), SUM(matches), SUM(draws) FROM players WHERE substr(token,41,24)=?", (token[40:64],))
    if record:
        w, l, d = record
    else:
        w, m, d = c.execute(*q).fetchone() or (0, 0, 0)
        w, m, d = w or 0, m or 0, d or 0
        l = max(0, m - w - d)
    p0 = elo_pool_rate(c)
    m = w + l + d
    wr = (w + 0.5 * d + ELO_PRIOR * p0) / (m + ELO_PRIOR)
    return ELO_BASE + 400.0 * math.log10((wr / (1.0 - wr)) / (p0 / (1.0 - p0)))


def elo_ensure(c):
    c.execute("CREATE TABLE IF NOT EXISTS ratings(key TEXT PRIMARY KEY, elo REAL, n INT DEFAULT 0, updated REAL)")


def elo_get(c, token, record=None):
    """(key, rating, rated matches) of the token's account; seeds the row when it has none (from `record` = (wins, losses, draws) when given)."""
    elo_ensure(c)
    key, sid = elo_key(c, token)
    r = c.execute("SELECT elo, n FROM ratings WHERE key=?", (key,)).fetchone()
    if r:
        return key, float(r[0]), int(r[1] or 0)
    e = elo_seed(c, key, sid, token, record)
    c.execute("INSERT OR IGNORE INTO ratings(key, elo, n, updated) VALUES(?,?,0,?)", (key, e, time.time()))
    return key, e, 0


# Match performance: the team result still decides the sign, the player's own match
# score scales its size. score = the parts the match paid the player (the game's own scoreboard: downs, executions, revives, gifts, ...; match_stats.parts,
# booked with the player's own end-of-match report), else 2 x kills + downs + revives - deaths. p = score / the mean score of the match's rated players.
# A gain is x clamp(0.5 + 0.5 p) and a loss x clamp(1.5 - 0.5 p), both within [ELO_PERF_LO, ELO_PERF_HI]: the match's best player gains up to 1.5 x on a win and
# loses down to 0.5 x on a loss, its weakest the other way; a player without a report (or a match without any) gets x 1. The reports reach the server
# after the host's result, so the rating waits ELO_PERF_WAIT_S for them (record_result). T2R_ELO_NO_PERF=1 = team result only, at once.
ELO_PERF_LO, ELO_PERF_HI, ELO_PERF_WAIT_S = 0.5, 1.5, 45.0


def elo_perf(c, mid, tokens):
    """{token: p} for the tokens with stats in match `mid` (p = own score / the mean score), {} when nothing usable."""
    if not mid or not tokens:
        return {}
    try:
        rows = c.execute("SELECT token, parts, kills, downs, deaths, revives FROM match_stats WHERE mid=?", (str(mid),)).fetchall()
    except Exception:                                               # noqa: BLE001 - no table yet
        return {}
    want = set(tokens)
    by_parts, by_counts = {}, {}
    for tok, parts, k, dn, de, rv in rows:
        if tok not in want:
            continue
        if parts is not None and parts >= 0:
            by_parts[tok] = float(parts)
        if k is not None and k >= 0:
            by_counts[tok] = max(0.0, 2.0 * k + max(dn or 0, 0) + max(rv or 0, 0) - max(de or 0, 0))
    sc = by_parts if len(by_parts) >= 2 else by_counts               # one measure for the whole match
    if len(sc) < 2:
        return {}
    mean = sum(sc.values()) / len(sc)
    if mean <= 0:
        return {}
    return {t: v / mean for t, v in sc.items()}


def elo_update(players, winner, skip=(), mid=None):
    """Called by record_result with the same arguments (winner 0 / 1 = that team, 2 = draw)."""
    if winner not in (0, 1, 2):
        return
    try:
        with _dblock, dbc() as c:
            side = {0: {}, 1: {}}
            for i, t in enumerate(players):
                if not t or len(t) != 64 or t in skip:
                    continue
                if not c.execute("SELECT 1 FROM players WHERE token=?", (t,)).fetchone():
                    continue
                key, e, n = elo_get(c, t)
                side[team_of(i)].setdefault(key, (e, n, t))   # one account twice in a match (two installs) counts once
            if not side[0] or not side[1]:
                return
            perf = {} if os.environ.get("T2R_ELO_NO_PERF") == "1" else elo_perf(c, mid, [v[2] for k in (0, 1) for v in side[k].values()])
            mean = {k: sum(v[0] for v in side[k].values()) / len(side[k]) for k in (0, 1)}
            notes = []
            for k in (0, 1):
                exp = 1.0 / (1.0 + 10.0 ** ((mean[1 - k] - mean[k]) / 400.0))
                res = 0.5 if winner == 2 else (1.0 if winner == k else 0.0)
                for key, (e, n, t) in side[k].items():
                    kf = ELO_K_NEW if n < ELO_NEW_N else ELO_K
                    d = kf * (res - exp)
                    p = perf.get(t)
                    if p is not None:
                        w = (0.5 + 0.5 * p) if d > 0 else (1.5 - 0.5 * p)
                        w = min(ELO_PERF_HI, max(ELO_PERF_LO, w))
                        d *= w
                        notes.append("%s p%.2f x%.2f %+.1f" % (tag(t), p, w, d))
                    c.execute("UPDATE ratings SET elo=?, n=n+1, updated=? WHERE key=?", (e + d, time.time(), key))
            log("ELO match", mid, "winner", winner, "team means %.0f / %.0f" % (mean[0], mean[1]), "rated", len(side[0]) + len(side[1]),
                "perf", "; ".join(notes) if notes else "none (no stats: team result only)")
    except Exception as ex:                                        # noqa: BLE001
        log("ELO update failed", ex)


def elo_of_tokens(tokens, records=None):
    """{token: rounded shown rating} for the in-game leaderboard (seeds unrated accounts); records = {token: (wins, losses, draws)}."""
    out = {}
    try:
        with _dblock, dbc() as c:
            for t in tokens:
                rec = (records or {}).get(t)
                e = elo_get(c, t, rec)[1]
                m = sum(rec) if rec else (c.execute("SELECT matches FROM players WHERE token=?", (t,)).fetchone() or (0,))[0] or 0
                out[t] = int(round(elo_shown(e, m)))
    except Exception as ex:                                        # noqa: BLE001
        log("ELO read failed", ex)
    return out

def record_result(players, winner, skip=(), mid=None):
    """Winning team from op 0x12 (2 = a draw); players[i] is on team_of(i). Losses = matches - wins - draws.
 `skip` = tokens that left the match before its end: they get no win / draw (pre-mortem: the whole list, leavers and
    host-migration placeholders included, was credited - a player who quit in minute 2 of a won match got the win, and the skill
    rating that balances the teams reads wins)."""
    if mid and PRIV.is_private_mid(mid):
        log("RESULT private match", mid, "winner", winner, "- no wins / skill rating booked")
        return
    if os.environ.get("T2R_ELO_NO_PERF") == "1" or not mid:
        elo_update(players, winner, skip, mid)
    else:                                                          # the players' own stats reports come after the host's result (elo_perf)
        tm = threading.Timer(ELO_PERF_WAIT_S, elo_update, (list(players), winner, set(skip), mid))
        tm.daemon = True
        tm.start()
    with _dblock, dbc() as c:
        for i, t in enumerate(players):
            if t in skip:
                continue
            if winner == 2:
                c.execute("UPDATE players SET draws = draws + 1 WHERE token = ?", (t,))
            elif team_in(mid, i, t) == winner:
                c.execute("UPDATE players SET wins = wins + 1 WHERE token = ?", (t,))

# ---------------------------------------------------------------- clan (2013 12 weeks)
# A day is resolved from the player's own report (op 0xE2, parts the ext booked for its slot)
# once per match id. Match ids: CLAN_MATCH[token] is set when the match really starts (all
# loaded, GAME-START), CLAN_ENDED[mid] when the host's 0x12 arrives. A report before the end
# waits CLAN_GRACE s for the 0x12; still no end then = the player left early.
CLAN_MATCH = {}      # token -> mid of its last started match
CLAN_TEAM = {}       # mid -> {token: team} at the match start (the player stats of the RAIDS page: clan_ingame.stats_report)
CLAN_MAP = {}        # mid -> display name of the map the match was started on (the clan chronicle's "where it happened")
CLAN_ENDED = {}      # mid -> time the host reported the end
CLAN_PENDING = {}    # token -> (mid, parts, flags)
CLAN_CREDITED = set()  # (token, mid) whose clan day was resolved: a match gives a player ONE day, even when it left early and came back into it through join in progress (fuzz seed 98: left-early day + normal day for one match)
CLAN_QUITTERS = set()  # (token, mid): the host that left a match we then ended for the others (abort award): its own day stays 'left early'
CLAN_GRACE = float(os.environ.get("T2R_CLAN_GRACE", "20"))
# (the host left, migration waited, the other player -
# who could not see anyone - quit and got a left-early day: 1 died, 5 hungry, 2 sick). A player who leaves a running match within
# CLAN_EXCUSE_S after someone ELSE left / crashed it, or while its host is lost and migration has not taken over, is excused:
# no left-early day (parts earned still count, a deficit is forgiven; nothing earned = no day at all).
CLAN_EXCUSE_S = float(os.environ.get("T2R_CLAN_EXCUSE_S", "90"))
# ("we need to improve forgiving timeouts/crashes"): ONE rule - a left-early clan day only for a deliberate leave. The daemon-side reason of every
# departure is known in leave_running_match (the op that took the player out): the client's own leave op (0x02 from the pause menu / "searching again")
# is deliberate, everything the network, a crash, a load timeout or the server did (session lost, timed out, never loaded, admin kick) is not. Even a
# deliberate leave is excused in the first CLAN_START_GRACE_S s of the match (nobody reached the host / bad start) and in the last CLAN_END_GRACE_S s of
# the round (decided). A player who comes back into the match (join in progress / re-attach) loses the excuse and keeps playing for full credit: the first
# report waits (CLAN_REJOIN_MAX_S) and merges with the one at the end.
HOSTMIG_NOCAND_S = float(os.environ.get("T2R_HOSTMIG_NOCAND_S", "45"))   # host lost, no player asked to take over for this long: the match is a ghost, end it (it blocked join in progress forever)
CLAN_START_GRACE_S = float(os.environ.get("T2R_CLAN_START_GRACE", "45"))
CLAN_END_GRACE_S = float(os.environ.get("T2R_CLAN_END_GRACE", "30"))
CLAN_REJOIN_MAX_S = float(os.environ.get("T2R_CLAN_REJOIN_MAX", "900"))
# T2R_CLAN_NO_PENALTY=1 excuses every leave (no left-early day).
CLAN_NO_PENALTY = os.environ.get("T2R_CLAN_NO_PENALTY", "0") not in ("", "0")
CLAN_DELIBERATE_WHY = ("left (0x02)", "searching again")     #
CLAN_EXCUSE_WHY = {}   # (token, mid) -> why the departure was excused (the log line of clan_finalize)
# (token, mid) removed by an admin kick or the ping limit. Its clan day for that match never books a
# deficit: 0 parts = no day at all, parts > 0 = the parts count and the deficit is forgiven - even when the match ended normally afterwards (an admin kick
# of the last opponent ends it with an abort award, CLAN_ENDED, and the kicked player's 0-part report used to book a full starving day).
CLAN_KICKED = {}       # (token, mid) -> why (log text)
# (match 87ab7bc6: both others left in the first 30 s, the abort award
# ended the match for the one player left, whose 0-part report booked a normal day: not counted, but 2 died, 48 hungry, 19 sick). A player the match was
# ended FOR by abort_award (everyone else left / the host left with no migration / host lost with no successor) is booked like a kicked player:
# 0 parts = no day at all, parts > 0 = the parts count and the deficit is forgiven. The host that quit on purpose keeps its left-early day (CLAN_QUITTERS).
CLAN_ABORTED = {}      # (token, mid) -> why the match was ended for it (log text)
CLAN_LEFT_AT = {}      # (token, mid) -> time of its departure from the running match (a 'back in the match' needs a departure first)
CLAN_REJOINED = {}     # (token, mid) -> time the first deferral happened: the player is back in the match, its first report waits for the next one
CLAN_LAST_DEPART = {}  # mid -> (time, token) of the latest departure from the running match
CLAN_EXCUSED = set()   # (token, mid)
CLAN_LINK_EXCUSES = {}  # token -> [times]: 'my link was lost' excuses granted (client-reported flag 4, cannot be proven: capped so pulling the cable is not a free way out)
CLAN_LINK_EXCUSE_MAX, CLAN_LINK_EXCUSE_WINDOW = 2, 86400.0
# Closing the game window during a match (Alt+F4, the X) must not save the clan from its penalty. It
# dropped the session like a crash, so the leave was excused. The ext now sends op 0xBC sub 3 just before its window closes in a match
# (CLAN_CLOSING): a session lost within CLAN_CLOSING_S (20 s) of it is the player's own leave. A kill the ext cannot see (Task Manager, pulled cable) still
# looks like a crash: those excuses are capped per player (CLAN_LOST_EXCUSE_MAX in CLAN_LOST_EXCUSE_WINDOW s), the rest count as left early.
CLAN_CLOSING = {}      # token -> (time, mid) of its 'my window is closing' notice
CLAN_CLOSE_DROPS = set()   # (token, mid): its session dropped within CLAN_CLOSING_S of that notice - the verdict comes SESSION_GRACE_S later
CLAN_CLOSING_S = float(os.environ.get("T2R_CLAN_CLOSING_S", "20"))   # (short: a close the game asks to confirm and the player cancels must not turn a later crash into a leave)
CLAN_LOST_EXCUSES = {}  # token -> [times]: crash / lost-connection excuses granted
CLAN_LOST_EXCUSE_MAX = int(os.environ.get("T2R_CLAN_LOST_EXCUSE_MAX", "3"))
CLAN_LOST_EXCUSE_WINDOW = float(os.environ.get("T2R_CLAN_LOST_EXCUSE_WINDOW", "86400"))
_clanlock = threading.Lock()

def clan_key(c, token):
    """Key of a player's clan: the Steam account when known ("s:<steamid>"), else the install token id. clan progress reset
    with every update - the token id is per install and every playtest is a fresh client folder (a new token), so each update made a new
    clan row. First use of the Steam key adopts the account's furthest-along clan among its old tokens (alive first, then day, then
    most recently played), boosters included."""
    tid = clan.token_id(token)
    r = c.execute("SELECT steamid FROM players WHERE token=?", (token,)).fetchone()
    sid = int(r[0]) if r and r[0] else 0
    if not sid:
        return tid
    key = "s:%d" % sid
    clan.ensure(c)
    if c.execute("SELECT 1 FROM clans WHERE token_id=?", (key,)).fetchone():
        return key
    olds = sorted({clan.token_id(t) for (t,) in c.execute("SELECT token FROM players WHERE steamid=?", (sid,))} | {tid})
    best = None
    for tok, status, day, upd in c.execute("SELECT token_id, status, day, updated FROM clans WHERE token_id IN (%s)" % ",".join("?" * len(olds)), olds):
        k = (status == clan.ALIVE, day or 0, upd or 0)
        if best is None or k > best[0]:
            best = (k, tok)
    if best:
        cols = [r[1] for r in c.execute("PRAGMA table_info(clans)") if r[1] != "token_id"]
        c.execute("INSERT INTO clans(token_id,%s) SELECT ?,%s FROM clans WHERE token_id=?" % (",".join(cols), ",".join(cols)), (key, best[1]))
        try:
            c.execute("INSERT INTO boosters(token_id,bid,n) SELECT ?,bid,n FROM boosters WHERE token_id=? ON CONFLICT(token_id,bid) DO NOTHING", (key, best[1]))
        except sqlite3.OperationalError:
            pass
        log("CLAN key", key, "adopted clan of install", best[1], "(alive %d, day %d) from %d install(s)" % (best[0][0], best[0][1], len(olds)))
    return key

def clan_read_key(c, token, sid_hint=0):
    """READ-ONLY clan key for the launcher's reads (0xE0 / 0xE8 with an optional 8-byte steamid after the token; a fresh install's token
    never joined a match, so the daemon has no steamid for it and its clan reads as empty although the account's clan lives under "s:<steamid>").
    The hint is used ONLY when the token itself has no steamid in the players table; it never links the token to that account and never creates or
    adopts a clan row: if no "s:<id>" row exists the answer is "no clan". Without a hint (old clients) the key is exactly clan_key()."""
    r = c.execute("SELECT steamid FROM players WHERE token=?", (token,)).fetchone()
    if r and r[0]:
        return clan_key(c, token)
    if sid_hint > 0:
        return "s:%d" % sid_hint
    return clan_key(c, token)

def clan_state(token, sid_hint=0):
    with _dblock, dbc() as c:
        if sid_hint:
            return clan.load(c, clan_read_key(c, token, sid_hint))        # load() never inserts: an unknown key reads as no clan
        return clan.load(c, clan_key(c, token))

def clan_side(token, faction):
    with _dblock, dbc() as c:
        st, ok = clan.choose_side(c, clan_key(c, token), faction)
        if ok and clan_story:
            try:
                clan_story.on_new_run(c, clan_key(c, token), faction)
            except Exception as ex:                  # noqa: BLE001 - the feed never breaks the clan
                log("CLAN story (new run) failed", ex)
    log("CLAN side", tag(token), clan.FACTION_NAMES.get(faction, faction), "ok" if ok else "refused (clan alive)")
    if ok and achievements:
        try:
            with _dblock, dbc() as c:
                ach_log(token, achievements.book_side(c, clan_key(c, token)))
        except Exception as ex:                      # noqa: BLE001
            log("ACH side failed", repr(ex))
    return st

CLAN_WIN = {}        # (token, mid) -> True win / False loss / None draw of the token's team, booked with the host's 0x12 (record_result); read once by clan_match_facts
CLAN_WEAPONS = ("", "bow", "revolver", "shorty", "hunting", "semi", "burst", "pistol", "crossbow", "melee", "bomb", "molotov")     # optional extended label 24 -> clan_saga_data.WEAPONS key


def clan_match_facts(token, mid, r, parts, counts):
    """What the clan chronicle / saga may tell about the player's match (tools/clan_saga.py): everything the daemon already receives, nothing new on the wire.
    map = where the match was started; kills / execs / revives / headshots / marked = the award counts of op 0xE2 (label ids = mission ids 1..5, only when the client
    sent its count tail); win = the host's 0x12 winner against the player's team; the extended labels 21 times downed, 22 deaths, 23 best kill streak, 24 weapon with
    the most downs (CLAN_WEAPONS index) are read when a client sends them (the ext does not yet: absent = the saga leaves them out)."""
    cnt = dict(counts or {})
    m = {"map": CLAN_MAP[mid], "supplies": int(r.get("supplies", 0)), "parts": int(parts), "kills": int(cnt.get(1, 0)), "mid": mid}
    if cnt:
        for lab, key in ((2, "execs"), (3, "revives"), (4, "headshots"), (5, "marked"), (21, "downs"), (22, "deaths"), (23, "streak")):
            if lab in cnt or lab <= 5:
                m[key] = int(cnt.get(lab, 0))
        w = int(cnt.get(24, 0))
        if 0 < w < len(CLAN_WEAPONS):
            m["weapon"] = CLAN_WEAPONS[w]
    res = CLAN_WIN.pop((token, mid), "none")
    if res in (True, False):
        m["win"] = res
    return m


CLAN_MOVED = {}      # (token, mid) -> time: the admin moved it to the other team (admin_move_team); its leave report is held for the rest of the match
CLAN_REBOOK = set()    # (token, mid): booked once, then the player joined the SAME running match again (join in progress): its next report books a second day
CLAN_DONE = {}       # token -> mid of the last match whose report was resolved (the ext re-sends op 0xE2 until it gets a reply: no double booking)


def book_career_stats(token, counts):
    """Career totals for the website profile (tools/web/web.py): the award counts of each booked match report (op 0xE2 labels, after the
    anti-cheat clamp). 1 kills, 2 executions, 3 revives, 4 headshots, 5 marked, 21 times downed, 22 deaths, 23 best kill streak (career best),
    24 weapon class with the most downs that match (CLAN_WEAPONS index) -> pweapon counts the matches each weapon class led. Booked once per
    match (clan_finalize calls it after the duplicate checks)."""
    cnt = dict(counts or {})
    try:
        with _dblock, dbc() as c:
            c.execute("""CREATE TABLE IF NOT EXISTS pstats(token TEXT PRIMARY KEY, kills INT DEFAULT 0, execs INT DEFAULT 0, revives INT DEFAULT 0,
                headshots INT DEFAULT 0, marked INT DEFAULT 0, downs INT DEFAULT 0, deaths INT DEFAULT 0, streak INT DEFAULT 0,
                reports INT DEFAULT 0, updated REAL)""")
            c.execute("CREATE TABLE IF NOT EXISTS pweapon(token TEXT, weapon INT, n INT DEFAULT 0, PRIMARY KEY(token, weapon))")
            v = [int(cnt.get(k, 0)) for k in (1, 2, 3, 4, 5, 21, 22)]
            st = int(cnt.get(23, 0))
            c.execute("INSERT INTO pstats(token,kills,execs,revives,headshots,marked,downs,deaths,streak,reports,updated) VALUES(?,?,?,?,?,?,?,?,?,1,?) "
                      "ON CONFLICT(token) DO UPDATE SET kills=kills+excluded.kills, execs=execs+excluded.execs, revives=revives+excluded.revives, "
                      "headshots=headshots+excluded.headshots, marked=marked+excluded.marked, downs=downs+excluded.downs, deaths=deaths+excluded.deaths, "
                      "streak=MAX(streak, excluded.streak), reports=reports+1, updated=excluded.updated", (token, *v, st, time.time()))
            w = int(cnt.get(24, 0))
            if 0 < w < len(CLAN_WEAPONS):
                c.execute("INSERT INTO pweapon(token,weapon,n) VALUES(?,?,1) ON CONFLICT(token,weapon) DO UPDATE SET n=n+1", (token, w))
    except Exception as ex:                                        # noqa: BLE001
        log("STATS career record failed", ex)


# ---- achievements (tools/achievements.py) --------------------------------------------------------------------------------------------
# A match has two reports: the clan report 0xE2 (counts clamped by the anti-cheat, books the day) and the achievement report 0xA9 (all the ext's
# counters). Either may come first and both are re-sent. ACH_DONE[(token, mid)] = what that match already credited, per label; ACH_PEND holds a
# 0xA9 that came before the day was booked; ACH_DAY marks a booked day (its 0xE2 counts, result) so a later 0xA9 books on top of it.
ACH_DONE, ACH_PEND, ACH_DAY = {}, {}, {}
ACH_E2_ONLY = (1, 2, 3, 4, 5, 6, 21, 22, 23, 24, 33)      # labels the clamped 0xE2 report is the authority for (a 0xA9 value never overrides them)


def _ach_trim(d, n=3000):
    while len(d) > n:
        d.pop(next(iter(d)))


def ach_log(token, got):
    if got:
        log("ACH unlocked", tag(token), ", ".join("%s (%s)" % (a, achievements.BY_ID[a]["title"]) for a in got))
        evt("ach", tag(token), got)


def ach_merge(e2, a9):
    m = {int(k): int(v) for k, v in (e2 or {}).items()}
    for k, v in (a9 or {}).items():
        k = int(k)
        if k in ACH_E2_ONLY:
            continue
        m[k] = max(m.get(k, 0), int(v))
    return m


def ach_book_day(token, mid, counts, win, left, late, before, after, r):
    """The clan day of (token, mid) was booked: credit the match (0xE2 counts + a 0xA9 that came first) and the clan day."""
    try:
        with _dblock, dbc() as c:
            key = clan_key(c, token)
            done = ACH_DONE.setdefault((token, mid), {})
            first = (token, mid) not in ACH_DAY
            ACH_DAY[(token, mid)] = {"e2": dict(counts or {}), "win": win, "left": bool(left), "late": bool(late)}
            achievements.settle(c, key, counts, done, ACH_E2_ONLY)   # live reports (op 0xAB) above the clamped counts are taken back to them
            got = achievements.book_match(c, key, ach_merge(counts, ACH_PEND.pop((token, mid), None)), done, win=win, left=left, late=late, first=first)
            if r:
                got += [a for a in achievements.book_clan(c, key, before, after, r) if a not in got]
        _ach_trim(ACH_DONE)
        _ach_trim(ACH_DAY)
        ach_log(token, got)
    except Exception as ex:                                      # noqa: BLE001 - achievements never break the clan day
        log("ACH day failed", repr(ex))


def ach_report_op(pl):
    """Op 0xA9 [64B token][u32 seq][u8 n][n x (u8 label, u16 count)] (the ext's achievement counters of the match it just left) -> reply 0xA9 =
    the player's achievement text (achievements.wire_text). Booked on top of the match's clan day, or held until the day books."""
    if len(pl) < 69:
        return None
    token = pl[:64].decode("latin1", "ignore")
    if not all(ch in "0123456789abcdefABCDEF" for ch in token):
        return None
    a9 = {}
    for k in range(min(pl[68], 80)):
        o = 69 + 3 * k
        if len(pl) >= o + 3:
            a9[pl[o]] = struct.unpack_from("<H", pl, o + 1)[0]
    mid = CLAN_MATCH.get(token) or CLAN_LAST_MATCH.get(token)
    if not CLAN_MATCH.get(token) and PRIV.is_private_mid(PRIV_LAST_MID.get(token)):
        log("ACH report from a private match", tag(token), "- not booked (private matches have no clan / stats effects)")
        return ach_read(token, 0, 0xA9)      # (CLAN_LAST_MATCH is the public match before it: its day must not get these counters)
    got = []
    try:
        if mid and (AC is None or AC.rate_ok(token)):
            day = ACH_DAY.get((token, mid))
            if day is None:
                old = ACH_PEND.get((token, mid)) or {}
                ACH_PEND[(token, mid)] = {k: max(a9.get(k, 0), old.get(k, 0)) for k in set(a9) | set(old)}
                _ach_trim(ACH_PEND)
                log("ACH report", tag(token), "mid", mid, "held until the day books:", a9)
            else:
                with _dblock, dbc() as c:
                    got = achievements.book_match(c, clan_key(c, token), ach_merge(day["e2"], a9), ACH_DONE.setdefault((token, mid), {}),
                                                  win=day["win"], left=day["left"], late=day["late"], first=False)
                log("ACH report", tag(token), "mid", mid, "booked:", a9)
        elif not mid:
            log("ACH report without a match", tag(token))
        else:
            log("ACH report refused: report flood", tag(token))
        ach_log(token, got)
    except Exception as ex:                                      # noqa: BLE001
        log("ACH report failed", repr(ex))
    return ach_read(token, 0, 0xA9)


ACH_LIVE_AT = {}                                             # token -> time of its last booked live report
ACH_LIVE_GAP = 2.0                                           # s between two booked live reports of a token (more = the table only)


def ach_live_op(pl):
    """Op 0xAB [64B token][u8 n][n x (u8 label, u16 count)] (an achievement shows the moment it is earned): the ext's counters of
    the match it is in, sent when its table says a goal was just crossed (achieve.c ach_live_tick) -> booked at once (achievements.book_live,
    the same per-match `done` as the match end, so nothing counts twice) -> reply 0xAB = the table, which carries the unlock to the card.
    Only while the match runs (CLAN_MATCH, its day not booked yet); at most one booking per ACH_LIVE_GAP per token."""
    if len(pl) < 65:
        return None
    token = pl[:64].decode("latin1", "ignore")
    if not all(ch in "0123456789abcdefABCDEF" for ch in token):
        return None
    cnt = {}
    for k in range(min(pl[64], 80)):
        o = 65 + 3 * k
        if len(pl) >= o + 3:
            cnt[pl[o]] = struct.unpack_from("<H", pl, o + 1)[0]
    mid = CLAN_MATCH.get(token)
    now = time.time()
    got = []
    try:
        if mid and (token, mid) not in ACH_DAY and now - ACH_LIVE_AT.get(token, 0.0) >= ACH_LIVE_GAP:
            ACH_LIVE_AT[token] = now
            _ach_trim(ACH_LIVE_AT)
            with _dblock, dbc() as c:
                got = achievements.book_live(c, clan_key(c, token), cnt, ACH_DONE.setdefault((token, mid), {}))
            _ach_trim(ACH_DONE)
            log("ACH live", tag(token), "mid", mid, "booked:", cnt)
            ach_log(token, got)
    except Exception as ex:                                      # noqa: BLE001
        log("ACH live failed", repr(ex))
    return ach_read(token, 0, 0xAB)


def ach_read(token, sid_hint=0, op=0xAA):
    try:
        import clan_ingame
        with _dblock, dbc() as c:
            key = clan_read_key(c, token, sid_hint)
            row = clan.load(c, key) or {}
            txt = achievements.wire_text(c, key, int(row.get("days_total") or 0) // 7)
        txt += "LIVE 1\n"                                     # (this daemon takes op 0xAB; a line without '|' fields, skipped by every reader)
    except Exception as ex:                                      # noqa: BLE001
        log("ACH read failed", repr(ex))
        txt = ""
    return msg(op, txt.encode("utf-8"))


def ach_read_op(pl):
    """Op 0xAA [64B token][optional 8B steamid hint, the launcher's] -> reply 0xAA (achievements.wire_text). Read only."""
    if len(pl) < 64:
        return None
    token = pl[:64].decode("latin1", "ignore")
    if not all(ch in "0123456789abcdefABCDEF" for ch in token):
        return None
    return ach_read(token, int.from_bytes(pl[64:72], "little") if len(pl) >= 72 else 0)


def clan_finalize(token):
    """Resolve the token's pending report (if any): match end seen = normal day, else left early."""
    with _clanlock:
        p = CLAN_PENDING.pop(token, None)
    if not p:
        return
    mid, parts, flags, counts = p
    mvt = CLAN_MOVED.get((token, mid))
    if mvt and not (flags & 2):
        # ("doesnt delete their progress"): a team move is no leave. The report of the first half waits while the player is back in
        # the match (or its move is still open); the report at the end adds to it. Never re-entered: booked as an excused day (no penalty).
        # (3 players were moved, the host quit 5 s later): the match END used to skip this hold, so the movers got a normal 0-supply day
        # (hungry + sick). A mover is excused whether the match ended or not; the hold itself only while the match still runs.
        back_in = MM.in_match(token) and MM.mid_of(token) == mid
        if mid not in CLAN_ENDED and (back_in or time.time() - mvt < 200):
            with _clanlock:
                CLAN_PENDING.setdefault(token, p)
            CLAN_DUE[token] = time.time() + 10
            return
        CLAN_EXCUSED.add((token, mid))
    CLAN_MOVED.pop((token, mid), None)
    if not (flags & 2):
        t0 = CLAN_REJOINED.get((token, mid))
        with MM.lock:
            _dep = token in MM.departed.get(mid, ())
        if (token, mid) in CLAN_LEFT_AT and MM.in_match(token) and MM.mid_of(token) == mid and not _dep and (t0 is None or time.time() - t0 < CLAN_REJOIN_MAX_S):
            # left, then came back into the same match (join in progress / re-attach): it plays on for full credit - the excuse is void, the verdict
            # waits for its next leave or the end report, which adds to this one (clan_report)
            if t0 is None:
                CLAN_REJOINED[(token, mid)] = time.time()
                log("CLAN rejoin", tag(token), "mid", mid, "is back in the match - report held, full credit (the next leave / the end decides)")
            CLAN_EXCUSED.discard((token, mid))
            CLAN_EXCUSE_WHY.pop((token, mid), None)
            with _clanlock:
                CLAN_PENDING.setdefault(token, p)
            CLAN_DUE[token] = time.time() + 10
            return
    CLAN_DONE[token] = mid                            # this match id is booked (a re-sent report is acknowledged, not booked again: clan_report)
    if (token, mid) in CLAN_CREDITED:
        log("CLAN day", tag(token), "mid", mid, "already credited for this match (it left, came back and reported again) - ignored")
        return
    # the server saw the host's 0x12, or the client's own game ended the match (flags bit 1, winmm's
    # match end ran there): a normal day. Otherwise the player left early.
    left = (mid not in CLAN_ENDED and not (flags & 2)) or (token, mid) in CLAN_QUITTERS
    if not left and not (flags & 2) and (token, mid) in CLAN_EXCUSED and CLAN_LEFT_AT.get((token, mid), 1e18) <= CLAN_ENDED.get(mid, 0) + 2:
        # FKZB: an excused leave (host lost) was itself the last departure, so the abort award ended the match in the same second and
        # CLAN_ENDED was set before this report resolved -> a normal 0-supply day (21 hungry, 9 sick). A player who left at or before the end
        # (and whose own game never ran the match end) left; its excuse applies below.
        left = True
    if left and (flags & 4) and (token, mid) not in CLAN_QUITTERS and (token, mid) not in CLAN_EXCUSED:
        # (the flag is the client's own word - pulling the cable sets it too: at most CLAN_LINK_EXCUSE_MAX per day and token, after that it is a normal early leave)
        _now = time.time()
        _ex = [t for t in CLAN_LINK_EXCUSES.get(token, []) if _now - t < CLAN_LINK_EXCUSE_WINDOW]
        if len(_ex) >= CLAN_LINK_EXCUSE_MAX:
            CLAN_LINK_EXCUSES[token] = _ex
            log("CLAN excuse", tag(token), "mid", mid, "link-lost flag refused - %d such excuses in the last day already" % len(_ex))
            flags &= ~4
    if left and (flags & 4) and (token, mid) not in CLAN_QUITTERS and (token, mid) not in CLAN_EXCUSED:
        CLAN_LINK_EXCUSES.setdefault(token, []).append(time.time())
        # ("booted for no reason, clan said left match early"): the player's own link died (flag bit 2 of the 0xE2 report: the ext saw
        # its inbound path dead for seconds / the host quiet until the session closed) - the game then showed "Connection to host lost" and sent
        # the leave itself, which the server cannot tell from a back-out. A dropped link is no leave: same excuse as a host that was lost.
        CLAN_EXCUSED.add((token, mid))
        log("CLAN excuse", tag(token), "mid", mid, "its own link was lost (report flag 4) - no left-early penalty")
    if left and (token, mid) not in CLAN_QUITTERS and time.time() - RESTORED_T.get(mid, -1e9) < RESTORE_EXCUSE_S:
        # (PT92 live): a daemon restart closed every session socket; the clients (that do not re-attach) left the match and reported flags 0 -> six
        # clans booked a left-early day with zero supplies. A report that arrives within RESTORE_EXCUSE_S of a restored match's re-adoption is ambiguous
        # (restart or deliberate leave): excused - the parts count, the deficit is forgiven, no penalty.
        CLAN_EXCUSED.add((token, mid))
        log("CLAN excuse", tag(token), "mid", mid, "report within %.0f s of the daemon restart that restored this match - no left-early penalty" % (time.time() - RESTORED_T[mid]))
    forgive_excused = False
    if left and CLAN_NO_PENALTY:                                   # a server without clan penalties (test server) - every early leave is excused
        if int(parts) <= 0:
            log("CLAN day", tag(token), "mid", mid, "left early - no clan penalties on this server: nothing earned, no day, no penalty")
            return
        left, forgive_excused = False, True
        log("CLAN day", tag(token), "mid", mid, "left early - no clan penalties on this server: parts count, deficit forgiven")
    if (token, mid) in CLAN_KICKED:
        if int(parts) <= 0:
            log("CLAN day", tag(token), "mid", mid, "kicked (%s) - nothing earned, no day, no penalty" % CLAN_KICKED[(token, mid)])
            return
        left, forgive_excused = False, True
        log("CLAN day", tag(token), "mid", mid, "kicked (%s) - parts count, deficit forgiven" % CLAN_KICKED[(token, mid)])
    if (token, mid) in CLAN_ABORTED and (token, mid) not in CLAN_QUITTERS and (token, mid) not in CLAN_KICKED:
        if int(parts) <= 0:
            log("CLAN day", tag(token), "mid", mid, "match ended for it (%s) - nothing earned, no day, no penalty" % CLAN_ABORTED[(token, mid)])
            return
        left, forgive_excused = False, True
        log("CLAN day", tag(token), "mid", mid, "match ended for it (%s) - parts count, deficit forgiven" % CLAN_ABORTED[(token, mid)])
    if left and (token, mid) in CLAN_EXCUSED and (token, mid) not in CLAN_QUITTERS:
        if int(parts) <= 0:
            log("CLAN day", tag(token), "mid", mid, "excused (%s) - nothing earned, no day, no penalty" % CLAN_EXCUSE_WHY.get((token, mid), "left after another player left / crashed"))
            return
        left, forgive_excused = False, True
        log("CLAN day", tag(token), "mid", mid, "excused (%s) - parts count, deficit forgiven" % CLAN_EXCUSE_WHY.get((token, mid), "left after another player left / crashed"))
    try:
        with _dblock, dbc() as c:
            _tid = clan_key(c, token)
            dup = clan.load(c, _tid).get("last_mid") == mid      # this match was already booked (a replayed report / READOPT snapshot)
            prev = clan.snap_get(c, _tid, mid)                   # the rows before the first booking of this match + its numbers (persisted)
    except Exception:                                              # noqa: BLE001
        dup, prev, _tid = False, None, None
    rebook = bool(dup and prev and prev.get("stint") and ((token, mid) in CLAN_REBOOK or prev.get("rejoined")))
    if dup and not rebook:
        log("CLAN day", tag(token), "mid", mid, "already booked - ignored (no second parts record)")
        return
    book_career_stats(token, counts)                              # website career totals (booked once per match)
    late, mult0 = 0, 1.0
    if rebook:
        # ("No word from the camp"): the player left this match, joined it again and played on. The report of the second stint REPLACES the first
        # booking (leave + rejoin must not farm days): the rows the first booking changed are put back and the whole match (both stints summed) books once.
        st1 = prev["stint"]
        counts2, merged = dict(counts or {}), {int(k): v for k, v in st1["counts"].items()}
        for k, v in counts2.items():
            if k == 23:                                              # best kill streak: the better stint
                merged[k] = max(merged.get(k, 0), v)
            elif k == 24:                                            # weapon class with the most downs: the first stint's, else this one's
                merged[k] = merged.get(k, 0) or v
            else:
                merged[k] = merged.get(k, 0) + v
        parts, counts = max(0, min(int(parts) + int(st1["parts"]), 100000)), merged
        late, mult0 = int(st1.get("late", 0)), float(st1.get("mult", 1.0))
        forgive_excused = forgive_excused or bool(st1.get("forgive"))
        MM.late_mult_take(token, mid); MM.late_bonus_take(token, mid)      # (the first booking used them; its bonus / multiplier is reused below)
        with _dblock, dbc() as c:
            clan.snap_restore(c, _tid, token, prev)
        log("CLAN day", tag(token), "mid", mid, "second stint: the first booking is undone, the whole match (%d parts) books once" % parts)
    elif _tid is not None:
        try:
            with _dblock, dbc() as c:
                clan.snap_take(c, _tid, mid, token)
        except Exception as ex:                                      # noqa: BLE001 - a snapshot failure never blocks a day
            log("CLAN snapshot failed", repr(ex))
    raw_parts, raw_counts = int(parts), dict(counts or {})
    if not left:
        try:
            with _dblock, dbc() as c:
                c.execute("UPDATE players SET parts_total = parts_total + ?, parts_n = parts_n + 1 WHERE token = ?", (int(parts), token))
        except Exception as ex:
            log("SKILL parts record failed", ex)
    CLAN_CREDITED.add((token, mid))
    try:                                                          # the per-match player stats of the clan page's RAIDS tab (F / Triangle-Y): this player's own award counts
        import clan_ingame
        nm = get_name(token) or token_name(token) or "?"            # (outside _dblock: get_name takes it - deadlock, the clan server went silent)
        with _dblock, dbc() as c:
            clan_ingame.stats_report(c, mid, token, nm, CLAN_TEAM.get(mid, {}).get(token, -1), counts, left)
            clan_ingame.stats_parts(c, mid, token, parts)
    except Exception as ex:                                       # noqa: BLE001 - only the stats page; the clan day never depends on it
        log("CLAN match stats failed", repr(ex))
    mult = mult0 if rebook else MM.late_mult_take(token, mid)         # 2013 d68: a late joiner's supplies x min(3, 1 / (1 - progress)) ...
    if not left:
        late = late if rebook else MM.late_bonus_take(token, mid)   # 2013 late join: 100 parts per reinforcement the team was down (+200 when outnumbered)
        if late:
            parts += late
            log("CLAN late-join bonus", tag(token), "+%d parts" % late)
    if mult > 1.0 and not left:
        log("CLAN late-join multiplier", tag(token), "x%.2f (a day above x%.1f does not count, a deficit is forgiven)" % (mult, clan.DAY_MULT_LIMIT))
    _ach_win = CLAN_WIN.get((token, mid), "none")                  # (clan_match_facts pops it below)
    _ach_late = bool(late) or mult > 1.0
    with _dblock, dbc() as c:
        _ach_before = clan.load(c, clan_key(c, token)) if achievements else None
        st, r = clan.apply_match(c, clan_key(c, token), mid, parts, left, counts, mult=mult if not left else 1.0,
                                 forgive=(mult > 1.0 and not left) or forgive_excused,
                                 bottles=0 if left else max(0, min(int((counts or {}).get(33, 0)), 200)))      # label 33 = supply bottles picked up (Interrogation lockbox-phase drops); a left match: ProcessClanDay(0, 0)
        CLAN_REBOOK.discard((token, mid))
        try:                                                         # this booking's raw numbers, for a later stint of the same match
            clan.snap_update(c, clan_key(c, token), mid, rejoined=False,
                             stint={"parts": raw_parts, "counts": {str(k): v for k, v in raw_counts.items()}, "late": 0 if left else late,
                                    "mult": mult, "forgive": bool(forgive_excused or (mult > 1.0 and not left))})
        except Exception as ex:                                      # noqa: BLE001
            log("CLAN snapshot meta failed", repr(ex))
        if r and not left and CLAN_MAP.get(mid):
            # what the clan chronicle may tell about the match (tools/clan_chronicle.py): the map, the supplies / parts of the day and the Downed Enemy count the
            # client counted (op 0xE2 label 1). Deaths / times downed are not reported by the client: absent, the chronicle uses kill-only lines.
            r["match"] = clan_match_facts(token, mid, r, parts, counts)
        if r and clan_story:
            try:
                clan_story.on_day(c, clan_key(c, token), st, r)
            except Exception as ex:                  # noqa: BLE001 - the feed never breaks the clan
                log("CLAN story (day) failed", ex)
        if r and clan_perks:                         # supply perks: credit the day's goods, draw the perk uses the ext reported, write the saga line
            try:
                drawn = clan_perks.on_day(c, clan_key(c, token), st, r, counts, r.get("match"))
                if drawn:
                    log("clanperk: drawn from the stock", tag(token), "mid", mid, drawn)
            except Exception as ex:                  # noqa: BLE001
                log("CLAN perks (day) failed", ex)
    log("CLAN day", tag(token), "mid", mid, "parts", parts, "left" if left else "", "->", r, "state", clan.state_text(st))
    if achievements:
        ach_book_day(token, mid, counts, _ach_win if _ach_win in (True, False) else None, left, _ach_late, _ach_before, st, r)

_clan_sched = [False]
CLAN_DUE = {}        # token -> time its pending report is finalised when no match end arrived

def clan_scheduler():
    while True:
        time.sleep(0.25)
        now = time.time()
        for t in [t for t, due in list(CLAN_DUE.items()) if due <= now]:
            CLAN_DUE.pop(t, None)
            try: clan_finalize(t)
            except Exception as e: log("CLAN scheduler", repr(e))

# --------------------------------------------------------------- anti-cheat (tools/anticheat.py has the rules and the storage)
#   0xCC [64B token][u8 ver=1][u8 n] n x [u8 kind][u8 suspect slot][u16 t_s][f32 v1][f32 v2]   flag events from the ext, reply 0xCD "ok=1 want=<case>:<mid>,.."
#   0xCE [64B token][u32 case][u8 part][u8 parts][u32 age_s][u32 span_s][chunk <= 60000 B]       demo upload,              reply 0xCF "ok=1 st=<ok|done|reason>"
#   0xD4 [64B token][u8 ver=1][u8 reasons][u8 slot][8 ASCII hex mid][u16 tlen][text]             player report (launcher), reply 0xD5 "ok=1 code=R12" / "ok=0 err=.."
#   0xD6 [64B token]                                                                             my last 3 matches' players,   reply 0xD7 "n=<m>\nm|<mid>|<age s>\np|<mid>|<slot>|<steamid>|<name>.."
# Nothing here bans: a ban is a human decision (tools/reports/review.py writes bans.txt; is_banned enforces it at login).
OP_AC_EVENTS, OP_AC_EVENTS_REPLY, OP_AC_DEMO, OP_AC_DEMO_REPLY = 0xCC, 0xCD, 0xCE, 0xCF
OP_AC_REPORT, OP_AC_REPORT_REPLY, OP_AC_RECENT, OP_AC_RECENT_REPLY = 0xD4, 0xD5, 0xD6, 0xD7
AC_OPS = frozenset((OP_AC_EVENTS, OP_AC_DEMO, OP_AC_REPORT, OP_AC_RECENT))
AC = None
if anticheat is not None and os.environ.get("T2R_AC", "1") != "0":
    try:
        AC = anticheat.AntiCheat(lambda: os.environ.get("T2R_AC_DIR") or os.path.join(os.path.dirname(os.path.abspath(DB_PATH)), "review"), now=time.time, log=lambda *a: log(*a))
    except Exception as _ex:                       # noqa: BLE001
        AC = None
        print("anti-cheat store unavailable:", repr(_ex))

def ac_server_event(token, mid, kind, w, evidence):
    """A finding of the server itself about `token` in `mid` (no reporter): scored, may open a case. Never raises."""
    if AC is None or not token or not mid:
        return
    try:
        AC.add_event(mid, None, token, kind, w, 0.0, 0, evidence)
        log("AC finding", tag(token), "mid", mid, anticheat.kind_name(kind), "-", evidence)
    except Exception as e:                         # noqa: BLE001
        log("AC finding failed", repr(e))

def ac_token_ok(pl):
    """The 64-hex token of an anti-cheat op (lower case), else None. These ops create nothing for a token that was not in a match: every write is gated on the match roster."""
    if len(pl) < 64:
        return None
    t = pl[:64].decode("latin1", "ignore")
    return t.lower() if is_token(t) else None

def ac_events_op(pl):
    t = ac_token_ok(pl)
    if t is None or AC is None or len(pl) < 66 or pl[64] != 1:
        return None
    mid = CLAN_MATCH.get(t)
    n = min(pl[65], 16, (len(pl) - 66) // 12)
    if mid:
        for i in range(n):
            kind, slot, ts, v1, v2 = struct.unpack_from("<BBHff", pl, 66 + 12 * i)
            rows = [r for r in AC.roster(mid) if r[0] == slot]
            if rows:
                AC.add_event(mid, t, rows[0][1], kind, v1, v2, ts)
        if n:
            log("AC events", tag(t), "mid", mid, n, "event(s)")
    want = ",".join("%d:%s" % w for w in AC.wanted(t))
    return msg(OP_AC_EVENTS_REPLY, ("ok=1 want=%s" % want).encode())

def ac_demo_op(pl):
    t = ac_token_ok(pl)
    if t is None or AC is None or len(pl) < 78:
        return None
    case_id, part, parts, age_s, span_s = struct.unpack_from("<IBBII", pl, 64)
    st = AC.add_demo_part(t, case_id, part, parts, min(age_s, 86400 * 3), min(span_s, 3600), pl[78:])
    return msg(OP_AC_DEMO_REPLY, ("ok=%d st=%s" % (1 if st in ("ok", "done") else 0, st)).encode())

def ac_report_op(pl):
    t = ac_token_ok(pl)
    if t is None or AC is None or len(pl) < 77 or pl[64] != 1:
        return None
    reasons, slot = pl[65], pl[66]
    mid = pl[67:75].decode("ascii", "ignore").lower()
    tl = struct.unpack_from("<H", pl, 75)[0]
    text = pl[77:77 + min(tl, 600)].decode("utf-8", "replace")
    if not re.fullmatch(r"[0-9a-f]{8}", mid):
        return msg(OP_AC_REPORT_REPLY, b"ok=0 err=bad match")
    ok, code = AC.add_report(t, mid, slot, reasons, text)
    log("AC player report", tag(t), "mid", mid, "slot", slot, "reasons", reasons, "->", ok, code)
    return msg(OP_AC_REPORT_REPLY, (("ok=1 code=%s" % code) if ok else ("ok=0 err=%s" % code)).encode())

def ac_recent_op(pl):
    t = ac_token_ok(pl)
    if t is None or AC is None:
        return None
    now = time.time()
    lines = []
    ms = AC.recent_matches(t, 3)
    for mid, ts, players in ms:
        lines.append("m|%s|%d" % (mid, max(0, int(now - ts))))
        for slot, sid, nm, me in players:
            if not me:
                lines.append("p|%s|%d|%d|%s" % (mid, slot, sid, (nm or "PLAYER").replace("|", "/").replace("\n", " ")))
    return msg(OP_AC_RECENT_REPLY, ("n=%d\n" % len(ms) + "\n".join(lines)).encode("utf-8")[:60000])

_ac_last_maint = [0.0]
def ac_tick():
    """Every status tick: cross-check finished matches' reports, deliver warnings, hourly housekeeping."""
    if AC is None:
        return
    try:                                           # (status_loop runs gc_tick and the rest after this: a broken review folder must never stop them)
        for tok, kind, w, ev, mid in AC.tick():
            ac_server_event(tok, mid, kind, w, ev)
        for wid, tok, text in AC.pending_warns():
            c = SESSIONS.conn(tok)
            if c and not MM.in_match(tok):
                try:
                    c.sendall(msg(0x11, srv_note("warn", tok, t=text)))
                    AC.warn_shown(wid)
                except OSError:
                    pass
        if time.time() - _ac_last_maint[0] > 3600:
            _ac_last_maint[0] = time.time()
            AC.maintenance()
    except Exception as e:                         # noqa: BLE001
        log("AC tick failed", repr(e))

def clan_report(token, parts, flags, counts=None):
    """Op 0xE2. Returns the state to reply with (before the day resolves if it has to wait)."""
    readopt_claim(token)
    mid = CLAN_MATCH.get(token)
    if AC is not None and not AC.rate_ok(token):
        log("AC clan report refused: report flood", tag(token))
        return clan_state(token)
    if not mid and PRIV.is_private_mid(PRIV_LAST_MID.get(token)):
        log("CLAN report from a private match", tag(token), "parts", parts, "- not booked (private matches have no clan effects)")
        return clan_state(token)
    if not mid:
        log("CLAN report without a started match", tag(token), "parts", parts)
        if AC is not None:
            ac_server_event(token, CLAN_LAST_MATCH.get(token), anticheat.S_ORPHAN, 0.5, "clan report with no started match")
        return clan_state(token)
    if CLAN_DONE.get(token) == mid:                   # a re-send: the client never saw our reply to the first report (tunnel drop / restart) - answer only
        log("CLAN report re-sent for an already booked match", tag(token), "mid", mid, "- acknowledged, not booked again")
        return clan_state(token)
    parts = max(0, min(int(parts), 100000))
    counts = dict(counts or {})
    if AC is not None:
        # anti-cheat: clamp parts / counts to what the match length allows; findings are scored once per distinct report (the ext re-sends)
        fresh = AC.first_time(token, mid, parts, counts)
        parts, counts, issues = AC.check_report(token, mid, parts, counts, flags)
        if fresh:
            issues = issues + AC.note_report(token, mid, parts, counts)
            for kind, w, ev in issues:
                ac_server_event(token, mid, kind, w, ev)
    with _clanlock:
        old = CLAN_PENDING.get(token)
        if old and old[0] == mid and ((token, mid) in CLAN_MOVED or (token, mid) in CLAN_REJOINED) and old[1:] != (parts, flags, counts):   # admin team move / a leave-and-return: the first half's report + this one = one day (an identical re-send is no second half)
            parts += old[1]
            for k, v in (old[3] or {}).items():
                if k == 23:                                   # best kill streak: the better half
                    counts[k] = max(counts.get(k, 0), v)
                elif k == 24:                                 # weapon class with the most downs: this half's, else the first half's
                    counts[k] = counts.get(k, 0) or v
                else:
                    counts[k] = counts.get(k, 0) + v
            log("CLAN team move", tag(token), "mid", mid, "parts %d (first team) + %d = %d" % (old[1], parts - old[1], parts))
        CLAN_PENDING[token] = (mid, parts, flags, counts)
    if not (flags & 2) and MM.mid_of(token) == mid:
        # the client's final report for the match while the daemon still has it in the match (a daemon restart ate its leave op): its game is out
        # of it - a leave (restart excuse in clan_finalize still applies), so a match whose players all reported ends instead of living on as a ghost that blocks JIP
        log("CLAN report from a player still listed in match", mid, tag(token), "- its game left it: departing")
        leave_running_match(token, "left (0x02) without a leave op, final report")
    if flags & 2:
        end_report_note(token, mid)
    if mid in CLAN_ENDED or (flags & 2):
        clan_finalize(token)
    else:
        CLAN_DUE[token] = time.time() + CLAN_GRACE      # one scheduler thread (clan_scheduler), not a Timer thread per report
        with _clanlock:
            if not _clan_sched[0]:
                _clan_sched[0] = True
                threading.Thread(target=supervised(clan_scheduler, "clan-scheduler"), daemon=True).start()
    return clan_state(token)

# Match end WITHOUT the host's op 0x12 (re/match-end-fallback-1019.md): a host whose link is down at the end (in-match link hold) or that crashed
# at the end never sends the result, so CLAN_ENDED / MM.end / INT.end / GL.end never ran. Every client's op 0xE2 carries flags bit 1 = "my own game ran
# the match end" - when all players still in the match said so, the daemon closes the match itself (match_end_via_reports, called by the reaper and by a
# timer after the first such report). The reports carry no winner: no win / draw is booked (winner unknown); matches were counted at GAME-START.
MEND_GRACE = float(os.environ.get("T2R_MEND_GRACE", "8"))      # s the host's 0x12 gets after the first end-report (a late 0x12 is still credited)
CLAN_REPORTED_END = {}   # mid -> {token: time} of the 0xE2 reports with flags bit 1
END_BY_REPORTS = set()   # mids closed by match_end_via_reports (idempotence)
RESULT_BOOKED = set()    # mids whose winner (0x12) was booked in record_result (a second 0x12 books nothing)
_end_timers = set()

def end_report_note(token, mid):
    """A client reported that its own game ended match `mid`: remember it, and give the host's 0x12 MEND_GRACE s before the daemon closes by itself."""
    with _clanlock:
        first = mid not in CLAN_REPORTED_END
        CLAN_REPORTED_END.setdefault(mid, {}).setdefault(token, time.time())
        if not first or mid in _end_timers or mid in CLAN_ENDED:
            return
        _end_timers.add(mid)
    def _go(m=mid):
        try:
            match_end_via_reports(m)
        except Exception as e:                      # noqa: BLE001
            log("MATCH-END via client reports ERR", repr(e))
        finally:
            _end_timers.discard(m)
    t = threading.Timer(MEND_GRACE + 0.5, _go)
    t.daemon = True
    t.start()

def match_end_via_reports(mid, now=None):
    """Close the live match `mid` as op 0x12 would (clan day, MM.end, INT.end, GL.end, freed 9 s later) when every player still in it reported the end
    (0xE2 flags bit 1) and the host's 0x12 did not come within MEND_GRACE s. A player that did not report counts as 'still playing' unless it is silent
    (> OFFLINE_GRACE: crashed host / gone). Never closes: an ended / unstarted match, one with a join-in-progress joiner still loading, one nobody reported
    for. Idempotent. Returns True when it closed the match."""
    now = time.time() if now is None else now
    if not mid or mid in END_BY_REPORTS or mid in RESULT_BOOKED:      # (RESULT_BOOKED: a 0x12 is mid-close on another thread - its closers run after the booking, before MM.ended is set)
        return False
    with _clanlock:
        rep = dict(CLAN_REPORTED_END.get(mid, {}))
    if not rep:
        return False
    with MM.lock:
        if mid in MM.ended or mid not in MM.started or mid not in MM.matches:
            return False                      # the host's result (or an abort) already closed it / it never went live
        players = list(MM.matches[mid])
        dep = set(MM.departed.get(mid, ()))
        joining = [t for t, (m, _) in MM.joining.items() if m == mid]
    if joining:
        return False                          # a joiner that has not started yet: the match is still being played
    active = [p for p in players if p not in dep]
    reported = [p for p in active if p in rep]
    if not reported or now - min(rep[p] for p in reported) < MEND_GRACE:
        return False
    playing = [p for p in active if p not in rep and silence(p, now) <= OFFLINE_GRACE]
    if playing:
        return False                          # somebody has not seen the end yet (or plays on alone after a migration)
    END_BY_REPORTS.add(mid)
    clan_match_ended(players, mid)
    loaded_clear_match(mid, players)
    MM.end(mid)
    INT.end(mid, 0xFF)
    GL.end(mid)
    def _free(p0=players[0], m=mid, pl=list(players)):
        MM.drop(m)
        HM.end(m)
        forget_match_state(pl)
        evt("freed", m, [tag(p) for p in pl])
    threading.Timer(9.0, _free).start()
    evt("match-end-reports", mid, "no host result; reports from", [tag(p) for p in reported], "silent/gone", [tag(p) for p in active if p not in rep])
    log("MATCH-END via client reports", mid, "players", [tag(p) for p in players], "reported", [tag(p) for p in reported],
        "- no 0x12 from the host, result unknown: no win / draw credited")
    return True

CLAN_LAST_MATCH = {}   # token -> last mid it was in (survives CLAN_MATCH; orphan-report evidence only)
PRIV_LAST_MID = {}   # token -> mid of its last private match: its clan report is answered, never booked

def clan_match_started(players, mid):
    try:
        # a join-in-progress start passes [joiner] alone - map_index([joiner]) has no MATCH_MAP entry, so it ROTATED a fresh map, logged it
        # ("MAP to1-capitol-shops ['lav']" in a Hospital match) and wrote that wrong name over the whole match's clan map. The match's own list is the key.
        CLAN_MAP[mid] = MAPS[map_index(MM.players_of_mid(mid) or list(players))][0]
        if len(CLAN_MAP) > 400:
            CLAN_MAP.pop(next(iter(CLAN_MAP)))
    except Exception as ex:                                        # noqa: BLE001 - only flavour for the clan feed
        log("CLAN map name failed", repr(ex))
    if PRIV.is_private_mid(mid):
        # private match: no clan day, no booster used, no clan feed / chronicle; the anti-cheat roster below still runs
        for p in players:
            PRIV_LAST_MID[p] = mid
            CLAN_MATCH.pop(p, None)
        log("CLAN private match", mid, "- no clan effects for", [tag(p) for p in players])
    else:
        CLAN_TEAM[mid] = {p: team_in(mid, i, p) for i, p in enumerate(players)}
        if len(CLAN_TEAM) > 400:
            CLAN_TEAM.pop(next(iter(CLAN_TEAM)))
        for p in players:
            CLAN_MATCH[p] = mid
            CLAN_LAST_MATCH[p] = mid
            PRIV_LAST_MID.pop(p, None)
            boost_start(p)
    if AC is not None:
        try:
            slots = MM.players_of_mid(mid) or list(players)
            AC.match_started(mid, slots, players, {p: get_name(p) for p in players}, {p: (BRIDGES.steamid(p) or token_steamid(p)) for p in players})
        except Exception as e:                      # noqa: BLE001
            log("AC roster record failed", repr(e))

def clan_match_ended(players, mid):
    for p in players:
        BOOST_ACTIVE.pop(p, None)
    if not mid:
        return
    if AC is not None:
        AC.match_ended(mid)
    CLAN_ENDED[mid] = time.time()
    for p in players:
        if CLAN_PENDING.get(p, (None,))[0] == mid:
            clan_finalize(p)
    old = [m for m, t in CLAN_ENDED.items() if time.time() - t > 3600]
    for m in old:
        CLAN_ENDED.pop(m, None)

# One-use boosters (tools/clan.py boosters table): the booster page's pick (op 0xEE) is remembered per token
# and used up when the match really starts (GAME-START), so a cancelled match costs nothing. The ext applies
# the effect itself; BOOST_ACTIVE (reply field ba) is what the server consumed for the running match.
BOOST_PICK = {}      # token -> booster id picked for the next match
BOOST_ACTIVE = {}    # token -> booster id in use in the running match

def boost_start(token):
    BOOST_ACTIVE.pop(token, None)
    bid = BOOST_PICK.pop(token, 0)
    if not bid:
        return
    with _dblock, dbc() as c:
        ok = clan.booster_consume(c, clan_key(c, token), bid)
    if ok:
        BOOST_ACTIVE[token] = bid
    log("BOOSTER", tag(token), clan.BOOSTERS.get(bid, bid), "used" if ok else "not owned")

def meta_op(op, pl):
    """Ops 0xE8 read / 0xEE [64B token][u8 booster id, 0 = none] pick / 0xEF [64B token][u8 mission id] choose
    -> reply 0xE9 (clan.meta_text)."""
    if len(pl) < 64:
        return None
    token = pl[:64].decode("latin1", "ignore")
    if not all(ch in "0123456789abcdefABCDEF" for ch in token):
        return None
    sid_hint = int.from_bytes(pl[64:72], "little") if op == 0xE8 and len(pl) >= 72 else 0       # optional steamid of the launcher's Steam account (read only)
    with _dblock, dbc() as c:
        tid = clan_read_key(c, token, sid_hint) if sid_hint else clan_key(c, token)
        if op == 0xEE:
            if len(pl) < 65:
                return None
            bid = pl[64]
            if bid and clan.booster_counts(c, tid).get(bid, 0) <= 0:
                bid = 0                                  # not owned: Nothing
            if bid:
                BOOST_PICK[token] = bid
            else:
                BOOST_PICK.pop(token, None)
            log("BOOSTER pick", tag(token), clan.BOOSTERS.get(bid, "nothing"))
        elif op == 0xEF:
            if len(pl) < 65:
                return None
            _, ok = clan.mission_choose(c, tid, pl[64])
            log("MISSION choose", tag(token), clan.MISSIONS.get(pl[64], ("?",))[0], "ok" if ok else "refused")
        elif op != 0xE8:
            return None
        text = clan.meta_text(c, tid, BOOST_PICK.get(token, 0), BOOST_ACTIVE.get(token, 0))
    text += " mf=%d" % flow_extra_secs()           # seconds after CHOOSE LOADOUT before the level start (ext timer)
    return msg(0xE9, text.encode())

FEED_SEEN = {}                     # token -> ids already sent to the main menu widget (newest unseen first, then the rest)

FEED_LINES = 6                     # lines per op 0xB4 reply (t2r-ext FEED_MAX)

def feed_latest(token, limit=FEED_LINES, lang="en"):
    """Clan feed lines for the main menu widget (op 0xB4): [(id, text), ...] <= limit, each one line (<= 118 chars: tools/clan_story.py
    live_for_widget). Stored events first (newest unseen; the cycle starts over once all recent ones were shown), then live ticker lines
    generated from the clan's real state up to `limit`, so a player with a clan always gets `limit` lines. [] without a clan.
    lang "en" | "pl": the language the lines are rendered in (the ids are the same in both, so a language switch keeps the cycle)."""
    if not clan_story:
        return []
    try:
        with _dblock, dbc() as c:
            tid = clan_key(c, token)
            seen = FEED_SEEN.setdefault(token, set())
            return clan_story.live_for_widget(c, tid, seen, limit, lang=lang)
    except Exception as ex:          # noqa: BLE001
        log("CLAN feed failed", ex)
        return []

_FEED_CACHE = {}                   # (token, lang, slot) -> (time.monotonic(), reply bytes): feed_op, 20 s

def feed_op(pl):
    """Op 0xB4 [64B token][u8 language] -> reply 0xB5 "n=<k>\\n<id>|<text>\\n..." (<= 6 lines (FEED_LINES), text <= 150 bytes of UTF-8, no '|' / newline inside;
 the whole reply must stay under the client's 1024 byte buffer). The optional language byte (the game's TEXT language, sent by t2r-ext)
    is 0 = English (also what a client that sends none gets), 1 = Polish; an unknown value is English. A message written before the language-neutral
    storage has no Polish form and comes back in English."""
    if len(pl) < 64:
        return None
    token = pl[:64].decode("latin1", "ignore")
    if not all(ch in "0123456789abcdefABCDEF" for ch in token):
        return None
    lines = []
    lang = clan_story.PLN.norm_lang(pl[64:65]) if (clan_story and len(pl) > 64) else "en"
    if len(pl) > 64:
        note_lang(token, pl[64:65])
    # (hotfix: the reply is a pure function of clan, 60 s time slot and language (clan_story.menu_lines); building it was 38 % of the
    #  daemon's CPU on fra1 with 20 players, so friends / clan replies missed the clients' 3.5 s timeout. Cached per token + language + slot, 20 s.)
    ck = (token, lang, clan_story.slot_of() if clan_story else 0)
    hit = _FEED_CACHE.get(ck)
    if hit and time.monotonic() - hit[0] < 20.0:
        return hit[1]
    if len(_FEED_CACHE) > 4000:
        _FEED_CACHE.clear()
    for mid, text in feed_latest(token, lang=lang)[:FEED_LINES]:
        t = str(text).replace("\n", " ").replace("|", "/").encode("utf-8")[:150].decode("utf-8", "ignore")
        lines.append("%s|%s" % (str(mid).replace("|", "").replace("\n", "")[:23], t))
    rep = msg(0xB5, ("n=%d\n" % len(lines) + "\n".join(lines)).encode("utf-8"))
    _FEED_CACHE[ck] = (time.monotonic(), rep)
    return rep

_RANK_TOK = {}        # token -> (time.monotonic(), symbol, weeks): the rank of a player behind a token, 60 s

def rank_of_token(token):
    """(symbol 0..9, weeks 0..999) of the player behind a daemon token (his clan row, clan_ingame.player_rank); (0, 0) when unknown."""
    now = time.monotonic()
    hit = _RANK_TOK.get(token)
    if hit and now - hit[0] < 15.0:
        return hit[1], hit[2]
    rk = wk = 0
    try:
        import clan_ingame
        rk, wk = db_read(lambda c: clan_ingame.player_rank(c, token))
    except Exception as ex:          # noqa: BLE001 - the rank is decoration
        log("RANK failed", repr(ex))
    if len(_RANK_TOK) > 2000:
        _RANK_TOK.clear()
    _RANK_TOK[token] = (now, rk, wk)
    return rk, wk

_ROLES = None

def role_of_steamid(sid):
    """tools/roles.py role code (0 none, 1 OG, 2 admin, 3 dev, 4 lead dev) of a Steam64 id; lists live next to the DB, re-read on change."""
    global _ROLES
    try:
        if _ROLES is None:
            import roles
            _ROLES = roles.Roles(os.path.dirname(os.path.abspath(DB_PATH)))
        return _ROLES.role_of(sid)
    except Exception as ex:          # noqa: BLE001 - the role is decoration
        log("ROLE failed", repr(ex))
        return 0

def rank_table_op(pl):
    """Op 0xBF [64B token] -> reply 0xBF (tools/rankbadge.py: one entry per recently seen player, keyed by a hash of the shown name)."""
    if len(pl) < 64:
        return None
    token = pl[:64].decode("latin1", "ignore")
    if not all(ch in "0123456789abcdefABCDEF" for ch in token):
        return None
    import rankbadge
    rankbadge.TABLE_TTL = 30.0           # (hotfix: 3 s re-ran a name + rank lookup per listed player every 3 s, 17 % of the daemon's CPU)

    def fetch():
        with _dblock, dbc() as c:
            rows = c.execute("SELECT token, steamid FROM players ORDER BY last_seen DESC LIMIT ?", (rankbadge.LIMIT,)).fetchall()
        return [(get_name(t) or "", *rank_of_token(t), role_of_steamid(sid)) for t, sid in rows]
    return msg(0xBF, rankbadge.table_text(fetch).encode("ascii", "ignore"))

def clan13_op(pl):
    """Op 0xB6 [64B token][u8 language][u8 section 1 roster / 2 journal / 3 raids / 4 status / 5 match stats of the raid on day <the u16 slot>][optional u32 LE seen journal id][optional u16 LE seen raid day] -> reply 0xB7 (UTF-8 text,
    tools/clan_ingame.py: the data of the in-game CLAN page, to1/clan13). Read only. An empty reply text = no clan / no story yet."""
    if len(pl) < 66:
        return None
    token = pl[:64].decode("latin1", "ignore")
    if not all(ch in "0123456789abcdefABCDEF" for ch in token):
        return None
    lang = clan_story.PLN.norm_lang(pl[64:65]) if clan_story else "en"
    note_lang(token, pl[64:65])
    section = pl[65]
    seen_id = int.from_bytes(pl[66:70], "little") if len(pl) >= 70 else 0
    seen_day = int.from_bytes(pl[70:72], "little") if len(pl) >= 72 else 0
    big = len(pl) >= 73 and pl[72] & 1               # PT92+ clients read 64 KB; older ones a 24 KB buffer (a bigger reply is dropped whole = blank JOURNAL tab)
    try:
        import clan_ingame
        with _dblock, dbc() as c:
            tid = clan_key(c, token)
            if clan_story:
                clan_story.ensure_roster(c, tid)          # a clan with no story yet (a fresh / reset one) gets its roster from the counts (additive, no feed row)
            text = clan_ingame.build(c, tid, lang, section, seen_id, seen_day, token=token.lower())
    except Exception as ex:          # noqa: BLE001
        log("CLAN13 failed", repr(ex))
        text = ""
    raw = text.encode("utf-8")
    cap = 60000 if big else 24000
    if len(raw) > cap:
        cut = raw.rfind(b"\n", 0, cap)               # whole rows only
        raw = raw[:cut + 1] if cut > 0 else raw[:cap]
    return msg(0xB7, raw)

def names_op(op, pl):
    """Ops 0xD0 read / 0xD2 set [64B token][u8 nlen][name][u8 tlen][tag] -> reply 0xD1 (names.state_text)."""
    if len(pl) < 64:
        return None
    token = pl[:64].decode("latin1", "ignore")
    if not all(ch in "0123456789abcdefABCDEF" for ch in token):
        return None
    tid = name_key(token)
    err = None
    if op == 0xD2:
        try:
            nl = pl[64]
            nm = pl[65:65 + nl].decode("utf-8")
            tl = pl[65 + nl]
            tg = pl[66 + nl:66 + nl + tl].decode("utf-8")
            if len(pl) < 66 + nl + tl:
                raise ValueError
        except (IndexError, ValueError, UnicodeDecodeError):
            return msg(0xD1, names.state_text("", "", "bad request").encode())
        with _dblock, dbc() as c:
            n2, t2, err = names.save(c, tid, nm, tg, taken=lambda n: name_taken(c, tid, n))
        name_dirty(token, key=tid)                    # the shown-name cache must not keep the old name (every token under this names key)
        if not err:
            log("NAME set", tag(token), "->", names.display(n2, t2) if n2 else "(cleared)")
            PRES.touch(token, get_name(token) or "PLAYER")
    with _dblock, dbc() as c:
        cn, ct = names.load(c, tid)
    return msg(0xD1, names.state_text(cn, ct, err).encode())

def clan_op(op, pl):
    """Ops 0xE0 / 0xE2 / 0xE4 -> reply frame (0xE1), or None for a bad payload."""
    if len(pl) < 64:
        return None
    token = pl[:64].decode("latin1", "ignore")
    if not all(ch in "0123456789abcdefABCDEF" for ch in token):
        return None
    if op == 0xE0:
        st = clan_state(token, int.from_bytes(pl[64:72], "little") if len(pl) >= 72 else 0)
    elif op == 0xE2 and len(pl) >= 73:
        seq, parts, flags = struct.unpack_from("<IIB", pl, 64)
        log("CLAN report", tag(token), "seq", seq, "parts", parts, "flags", flags)
        counts = {}                                   # optional tail: [u8 n] n x [u8 label id][u16 count] (missions)
        if len(pl) > 73:
            for k in range(min(pl[73], 16)):
                if len(pl) >= 74 + 3 * k + 3:
                    counts[pl[74 + 3 * k]] = struct.unpack_from("<H", pl, 75 + 3 * k)[0]
        st = clan_report(token, parts, flags, counts)
    elif op == 0xE4 and len(pl) >= 65:
        st = clan_side(token, pl[64])
    else:
        return None
    txt = clan.state_text(st)
    # the asker's own role (tools/roles.py) for its own rank badge colour / nametag; k=v, before the name tails. EVERY 0xE1 reply carries it: the ext's
    # clan_parse rebuilds the whole state from each reply (a missing key = 0), so the choose-side reply (0xE4) without it dropped the lead dev's pink badge and
    # rim for the rest of the session (live 186: everyone picked a side after the release reset).
    if op in (0xE0, 0xE2, 0xE4):
        try:
            with _dblock, dbc() as c:
                r = c.execute("SELECT steamid FROM players WHERE token=?", (token,)).fetchone()
            sid = (r[0] if r else 0) or (int.from_bytes(pl[64:72], "little") if op == 0xE0 and len(pl) >= 72 else 0)
            ro = role_of_steamid(sid)
            if ro:
                txt += " ro=%d" % ro
        except Exception as ex:                       # noqa: BLE001 - the role is decoration
            log("ROLE (state) failed", ex)
    if op in (0xE0, 0xE2) and clan_perks:             # the active supply perks + the credited member ride at the END of the state text (old ext: stops at the first name; old daemon: no keys)
        try:
            with _dblock, dbc() as c:
                key = clan_read_key(c, token, int.from_bytes(pl[64:72], "little") if op == 0xE0 and len(pl) >= 72 else 0)
                story = clan_story._load(c, key) if clan_story else None
                tail = clan_perks.wire_text(clan_perks.active(st, story), clan_perks.loss_streak(story))
            if tail:
                txt += " " + tail
                if op == 0xE0 and token not in CLAN_PERK_LOGGED:
                    CLAN_PERK_LOGGED[token] = 1
                    log("clanperk: active for", tag(token), tail)
        except Exception as ex:                       # noqa: BLE001
            log("CLAN perks (state) failed", ex)
    if op in (0xE0, 0xE2) and clan_status:            # member status updates (hungry / sad / sick / dead since last seen), after the perk tail: ints first, names last (tools/clan_status.py)
        try:
            with _dblock, dbc() as c:
                story = clan_story._load(c, clan_read_key(c, token, int.from_bytes(pl[64:72], "little") if op == 0xE0 and len(pl) >= 72 else 0)) if clan_story else None
            tail = clan_status.wire_text(story)
            if tail:
                txt += " " + tail
        except Exception as ex:                       # noqa: BLE001
            log("CLAN status (state) failed", ex)
    return msg(0xE1, txt.encode())

CLAN_PERK_LOGGED = {}   # token -> marker: the first perk state reply of a session is logged once
_LB_CACHE = {"t": 0.0, "top": None, "v": b""}
LB_CACHE_S = float(os.environ.get("T2R_LB_CACHE_S", "5"))
def leaderboard_table(top=10):
    """Cached for LB_CACHE_S s: op 0x13 is unauthenticated and the query costs ~60 ms (a get_name per row)."""
    now = time.time()
    c = _LB_CACHE
    if c["top"] == top and now - c["t"] < LB_CACHE_S:
        return c["v"]
    v = _leaderboard_table(top)
    c.update(t=now, top=top, v=v)
    return v

def _leaderboard_table(top=10):
    """0x14 payload = [u8 count][count x 0x30]: [32B name][u32 wins][u32 losses]
    [f32 ratio][u32 elo] (UI rows anim-group.table.row%d_onlineid/_wins/_losses/
    _ratio/_elo, read via FUN_1800590a0). Requested by client op 0x13."""
    rows = leaderboard(10 ** 6)
    ELO_R = elo_of_tokens([r[0] for r in rows], {r[0]: (r[4] or 0, max(0, (r[5] or 0) - (r[4] or 0)), 0) for r in rows})
    rows = sorted(rows, key=lambda r: -ELO_R.get(r[0], 1000))[:top]
    body = bytes([len(rows)])
    for tok, name, kills, deaths, wins, matches in rows:
        losses = max(0, (matches or 0) - (wins or 0))
        rec = bytearray(0x30)
        nm = (token_name(tok) or name or "PLAYER").encode("latin1", "ignore")[:31]
        rec[0:len(nm)] = nm
        ratio = (wins or 0) / losses if losses else float(wins or 0)
        elo = max(0, ELO_R.get(tok, 1000))                             # team Elo (elo_update); u32: never negative
        struct.pack_into("<IIfI", rec, 0x20, wins or 0, losses, ratio, elo)
        body += bytes(rec)
    return body + b"\x00"                               # +1 so len > count*0x30

# ---------------------------------------------------------------- presence
class Presence:
    def __init__(self):
        self.lock = threading.Lock()
        self.udp_seen = {}     # udp addr -> last ping time
        self.players = {}      # token -> {name, addr, last, loadout}
    UDP_SEEN_MAX = 2048
    def ping(self, addr):
        now = time.time()
        with self.lock:
            self.udp_seen[addr] = now
            if len(self.udp_seen) > self.UDP_SEEN_MAX:        # (a source-port spray grew this for ever; nothing reads it but the count)
                for a in [a for a, t in self.udp_seen.items() if now - t > 4 * KEEPALIVE_TTL]:
                    del self.udp_seen[a]
                if len(self.udp_seen) > self.UDP_SEEN_MAX:
                    for a in sorted(self.udp_seen, key=self.udp_seen.get)[:len(self.udp_seen) // 2]:
                        del self.udp_seen[a]
    def prune(self, max_age, keep=()):
        """Drop players whose last sign of life is older than max_age seconds (never those in `keep`: queued / in a match). Returns the number dropped."""
        now = time.time()
        with self.lock:
            old = [t for t, p in self.players.items() if now - p.get("last", 0) > max_age and t not in keep]
            for t in old:
                del self.players[t]
            for a in [a for a, t in self.udp_seen.items() if now - t > 4 * KEEPALIVE_TTL]:
                del self.udp_seen[a]
        return len(old)
    def touch(self, token, name):
        with self.lock:
            self.players.setdefault(token, {})["name"] = safe_name(name)
            self.players[token]["last"] = time.time()
    def touch_known(self, token):
        """Refresh `last` of a player that is already present (never creates an entry: the token comes from an unauthenticated poll)."""
        with self.lock:
            if token in self.players:
                self.players[token]["last"] = time.time()
    def drop(self, token):
        with self.lock:
            self.players.pop(token, None)
    def name_of(self, token):
        with self.lock:
            return self.players.get(token, {}).get("name", "PLAYER")
    def online(self):
        now = time.time()
        with self.lock:
            return [p for p in self.players.values() if now - p.get("last", 0) < KEEPALIVE_TTL]
    def count(self):
        return len(self.online())

PRES = Presence()
PEERS = {}     # player-IP -> latest UDP (ip,port) — for the P2P relay between matched peers (bounded: PEERS_MAX, oldest out)
PEERS_MAX = 2048
def peer_seen(ip, addr):
    PEERS.pop(ip, None)
    PEERS[ip] = addr
    if len(PEERS) > PEERS_MAX:
        try:
            for k in list(PEERS)[:len(PEERS) // 4]:
                PEERS.pop(k, None)
        except RuntimeError:
            pass
LOADED = set() # tokens that sent op 0x09 (level loaded/ready) for their current match
LOADED_T = {}  # token -> time of its last op 0x09 (the reaper drops a non-loader a bounded time after the others loaded)
RELAY = {}     # (match sid, slot) -> gameplay socket addr (relay routing table)
RELAY_TS = {}
UDP_ERRS = [0]
RELAY_STATS = collections.Counter()   # (sid, src, dst) -> packets
RELAY_TYPES = collections.Counter()   # (src, dst, T2RO type, size) -> packets
UDP_STATS = collections.Counter()   # (ip, first_byte) -> count, flushed by status_loop

# --------------------------------------------------- per-connection outbound queue
SENDQ_MAX_BYTES = int(os.environ.get("T2R_SENDQ_BYTES", str(256 * 1024)))   # queued, not yet accepted by the kernel
SENDQ_MAX_FRAMES = int(os.environ.get("T2R_SENDQ_FRAMES", "512"))
SENDQ_STALL_S = float(os.environ.get("T2R_SENDQ_STALL_S", "15"))            # no byte accepted by the peer for this long -> drop it
SENDQ_LINGER_S = 5.0                                                          # close(): flush what is queued for at most this long
TCP_SNDBUF = int(os.environ.get("T2R_TCP_SNDBUF", "0"))                      # 0 = OS default (tests set a few KB)
SENDQ_STATS = collections.Counter()                                           # dropped / killed

class OutConn:
    """A TCP connection whose WRITES are owned by the connection, not by the calling thread.
    sendall() never blocks: it sends inline when the kernel buffer takes everything (the normal case, no extra
    thread), otherwise the rest is queued and a lazily started writer thread drains it. The match-start /
    ping-location / countdown pushes reach other players' sockets from the matchmaker or another client's handler
    thread, and a peer that stopped reading used to hold that thread for the 120 s socket timeout.
    The queue is bounded (SENDQ_MAX_BYTES / _FRAMES): on overflow the oldest `drop=True` frames (state pushes a newer
    one supersedes: countdown steps, searching frame, ping location, notes) are discarded first; when that does not make
    room, or when the peer accepts no byte for SENDQ_STALL_S, the connection is closed (the client reconnects).
    Everything else (recv, settimeout, setsockopt, ...) is the plain socket. The wire bytes are unchanged."""
    def __init__(self, sock):
        self._sock = sock
        self._lock = threading.Lock()
        self._q = collections.deque()      # [bytes, droppable]
        self._qbytes = 0
        self._writer = False               # a writer thread is running
        self._dead = False
        self._closing = False
        self._closed = False
    def __getattr__(self, name):           # recv, settimeout, setsockopt, getpeername ...
        return getattr(self._sock, name)
    def fileno(self): return self._sock.fileno()
    def queued(self):
        with self._lock: return self._qbytes
    def _kill(self, why):
        """Mark dead and shut the socket down so a recv() in the handler thread returns; the handler's close() frees it."""
        with self._lock:
            if self._dead: return
            self._dead = True
            self._q.clear(); self._qbytes = 0
        SENDQ_STATS["killed"] += 1
        log("SENDQ", "closing connection:", why)
        try: self._sock.shutdown(socket.SHUT_RDWR)
        except OSError: pass
    def sendall(self, data, drop=False):
        if not data: return
        data = bytes(data)
        with self._lock:
            if self._dead or self._closing:
                raise BrokenPipeError("connection closed")
            if not self._q and not self._writer:
                # fast path: nothing waiting, so write directly, but only what the kernel takes without blocking
                try:
                    _, w, _ = select.select([], [self._sock], [], 0)
                    sent = self._sock.send(data) if w else 0
                except (BlockingIOError, socket.timeout, InterruptedError):
                    sent = 0
                except (OSError, ValueError) as e:
                    self._dead = True
                    raise BrokenPipeError("send failed: %r" % (e,))
                if sent >= len(data): return
                data = data[sent:]
                drop = False                  # a half-sent frame must complete
            self._q.append([data, drop]); self._qbytes += len(data)
            if self._qbytes > SENDQ_MAX_BYTES or len(self._q) > SENDQ_MAX_FRAMES:
                self._shed()
            overflow = self._qbytes > SENDQ_MAX_BYTES or len(self._q) > SENDQ_MAX_FRAMES
            start = not self._writer and not overflow
            if start: self._writer = True
        if overflow:
            self._kill("send queue overflow (%d B / %d frames)" % (self._qbytes, len(self._q)))
            raise BrokenPipeError("send queue overflow")
        if start:
            try:
                threading.Thread(target=self._run, daemon=True).start()
            except Exception as e:
                with self._lock: self._writer = False
                self._kill("cannot start writer thread %r" % (e,))
                raise BrokenPipeError("no writer thread")
    def _shed(self):
        """lock held: discard the oldest droppable frames (never the head one, it may be half sent) until the queue fits."""
        items = list(self._q)
        nbytes = sum(len(i[0]) for i in items); n = len(items)
        out = items[:1]
        for it in items[1:]:
            if it[1] and (nbytes > SENDQ_MAX_BYTES or n > SENDQ_MAX_FRAMES):
                nbytes -= len(it[0]); n -= 1; SENDQ_STATS["dropped"] += 1
            else:
                out.append(it)
        self._q = collections.deque(out); self._qbytes = nbytes
    def _run(self):
        """Writer thread: drains the queue and ends when it is empty (a later overflow starts a new one)."""
        last_progress = time.monotonic(); linger_end = None
        released = False                   # _writer already cleared under the lock (no window where a sender sees a writer that is leaving)
        try:
            while True:
                with self._lock:
                    if self._dead or not self._q:
                        self._writer = False; released = True
                        return
                    if self._closing and linger_end is None:
                        linger_end = time.monotonic() + SENDQ_LINGER_S
                    buf = self._q[0][0]
                now = time.monotonic()
                if now - last_progress > SENDQ_STALL_S:
                    self._kill("peer not reading for %.0f s (%d B queued)" % (SENDQ_STALL_S, self._qbytes)); return
                if linger_end is not None and now > linger_end:
                    self._kill("close(): queue not flushed in %.0f s" % SENDQ_LINGER_S); return
                try:
                    _, w, _ = select.select([], [self._sock], [], 0.5)
                    if not w: continue
                    n = self._sock.send(buf)
                except (BlockingIOError, socket.timeout, InterruptedError):
                    continue
                except (OSError, ValueError) as e:
                    self._kill("send failed %r" % (e,)); return
                if n:
                    last_progress = time.monotonic()
                    with self._lock:
                        if self._q and self._q[0][0] is buf:
                            if n >= len(buf): self._q.popleft()
                            else: self._q[0][0] = buf[n:]
                            self._qbytes -= n
        finally:
            with self._lock:
                if not released: self._writer = False     # (after a release a new writer may already own the flag)
                done = self._closing
            # close() while a later writer is running leaves the close to that writer; with none left nobody else would
            if done and self._writer_gone(): self._really_close()
    def _writer_gone(self):
        with self._lock: return not self._writer
    def _really_close(self):
        with self._lock:
            if self._closed: return
            self._closed = True
        try: self._sock.close()
        except OSError: pass
    def close(self):
        """Close once what is queued is flushed (bounded by SENDQ_LINGER_S); at once when nothing is queued."""
        with self._lock:
            self._closing = True
            if self._writer: return                 # the writer closes the socket when it ends
        self._really_close()

def push(c, data, drop=False):
    """Cross-thread push to another player's connection: droppable state frames are shed first on overflow (plain sockets / test fakes: sendall)."""
    if isinstance(c, OutConn): c.sendall(data, drop=drop)
    else: c.sendall(data)

def wrap_conn(c):
    return OutConn(c) if isinstance(c, socket.socket) else c

# --------------------------------------------------- steam bridges (Spacewar 480)
class Bridges:
    """Registry of connected Steam helpers (client steam/t2r-steam.exe), keyed by mod token.
    Each holds the player's SteamID64, the helper's local gameplay UDP port (0 = the
    helper can't carry gameplay) and the live TCP conn for the 0xF2 match push."""
    def __init__(self):
        self.lock = threading.Lock()
        self.by_token = {}     # token -> {"conn": sock, "steamid": int, "port": int, "ip": str, "name": str}
        self.by_sid = {}       # steamid -> token (ping_report: ~400 reports / s at 100 helpers scanned every helper per report)
    def register(self, token, conn, steamid, port=0, ip=None, name=None):
        with self.lock:
            old = self.by_token.get(token)
            if old and self.by_sid.get(old["steamid"]) == token:
                del self.by_sid[old["steamid"]]
            self.by_token[token] = {"conn": conn, "steamid": steamid, "port": port, "ip": ip, "name": name}
            self.by_sid[steamid] = token
    def links(self):
        """{token: (conn, steamid)} of every connected helper, one lock pass (the ping fan-out took the lock 3 times per other helper)."""
        with self.lock:
            return {t: (b["conn"], b["steamid"]) for t, b in self.by_token.items()}
    def tokens_for_ip(self, ip):
        """Tokens of every helper connected from this IP (for the optional install check)."""
        with self.lock:
            return [t for t, b in self.by_token.items() if b.get("ip") == ip]
    def name_for_ip(self, ip):
        """Steam name of the only helper connected from this IP, else None (none, or
        several players behind one address -> ambiguous)."""
        with self.lock:
            names = {b["name"] for b in self.by_token.values() if b.get("ip") == ip and b.get("name")}
        return names.pop() if len(names) == 1 else None
    def port(self, token):
        with self.lock:
            b = self.by_token.get(token)
            return b["port"] if b else 0
    def unregister(self, token, conn=None):
        """Drop the registration; with `conn` only when it is still THAT connection (a helper that
        reconnected replaced the entry - its old socket closing must not unregister the new one).
        Returns True when something was removed."""
        with self.lock:
            b = self.by_token.get(token)
            if b is None or (conn is not None and b["conn"] is not conn):
                return False
            del self.by_token[token]
            if self.by_sid.get(b["steamid"]) == token:
                del self.by_sid[b["steamid"]]
            return True
    def steamid(self, token):
        with self.lock:
            b = self.by_token.get(token)
            return b["steamid"] if b else 0
    def conn(self, token):
        with self.lock:
            b = self.by_token.get(token)
            return b["conn"] if b else None

BRIDGES = Bridges()
NAMELESS_BY_IP = {}    # IP -> last game token without an embedded T2N name (started outside Play.bat)

def adopt_bridge_name(token, ip):
    """A game token without a T2N name (game started outside Play.bat, or from another
    install) would show as PLAYER. If exactly one Steam helper is connected from the same
    IP, use its Steam name for that token."""
    if not token or token_name(token) or len(token) != 64:
        return
    NAMELESS_BY_IP[ip] = token
    nm = BRIDGES.name_for_ip(ip)
    if nm and get_name(token) != safe_name(nm):
        set_name(token, nm)
        log("NAME-FROM-BRIDGE", ip, tag(token), "->", nm)

def no_helper_note(token, ip):
    """(note key, helper name) for a search refused for want of a Steam helper.
    A nameless login token while a named Steam helper is connected from the same IP: the game and the helper did not log
    in with the same token. winmm sends a hash substitute when its game-data check flags the install (changed base
 archives, extra .psarc, other mods), so "open Steam" was the wrong advice."""
    helper = None if token_name(token) else BRIDGES.name_for_ip(ip)
    return ("files_changed", helper) if helper else ("steam_off", None)

# --------------------------------------------------- install check (optional module, not part of this tree)
def install_refusal(token, ip, wait_s=0.0):
    """None = this login / search goes on; else (reason for the log, op 0x11 note bytes). No install check in this tree: always None."""
    return None

# --------------------------------------------------- held matchmaking sessions
class Sessions:
    """Held (persistent) matchmaking TCP connections, keyed by token, so the
    daemon can push the match-start opcodes to BOTH players when a match forms."""
    def __init__(self):
        self.lock = threading.Lock()
        self.by_token = {}     # token -> conn
    def register(self, token, conn):
        with self.lock:
            self.by_token[token] = conn
    def unregister(self, token, conn=None):
        with self.lock:
            if token in self.by_token and (conn is None or self.by_token[token] is conn):
                del self.by_token[token]
    def conn(self, token):
        with self.lock:
            return self.by_token.get(token)

SESSIONS = Sessions()

# ---- session socket lost in a running match (PT89 live test): a NAT / router dropped a player's menu+match TCP socket 8.5 min into a match.
# The match itself runs over Steam P2P and was fine, but winmm leaves a running match when its session socket dies. The ext now re-opens the
# socket and logs in again (login 0x0f + hello 0x0e, same token); the daemon recognises that as a RE-ATTACH of a player whose match is running:
# the socket is registered as the player's session again (match id / team untouched: the match record never left), no match-start is re-sent.
# A player whose socket was lost and who does not come back within SESSION_GRACE_S is removed from the match like a timed-out one.
SESSION_GRACE_S = float(os.environ.get("T2R_SESSION_GRACE", "60"))
SESS_LOST = {}                         # token -> (time.time() the session conn was lost, match id)

def clan_closed_book(token, mid):
    """A closed game sends no match report, and a clan day is booked only from one: book the left-early day (0 parts) here. Finalised
    after the re-attach window + 20 s, so the leave verdict (leave_running_match) and a match end come first. A start / end grace or update excuse
    still applies: no day then (like any excused leave)."""
    if CLAN_MATCH.get(token) != mid or CLAN_DONE.get(token) == mid:
        return
    ex = clan_leave_excuse(token, mid, "session lost")
    if ex is not None:
        log("CLAN closed", tag(token), "mid", mid, "- excused (%s): no day" % ex)
        return
    CLAN_QUITTERS.add((token, mid))
    with _clanlock:
        CLAN_PENDING.setdefault(token, (mid, 0, 0, {}))
    CLAN_DUE[token] = time.time() + SESSION_GRACE_S + 20
    log("CLAN closed", tag(token), "mid", mid, "- no report comes from a closed game: left-early day (0 parts) booked in %.0f s" % (SESSION_GRACE_S + 20))

def note_session_lost(token, conn):
    """A registered session conn of `token` closed. In a launched, not ended match: remember it for SESSION_GRACE_S (re-attach window)."""
    try:
        if SESSIONS.conn(token) is not conn:
            return False                                   # replaced by a newer conn already (re-attached): this close is the stale one
        mid = MM.mid_of(token)
        if mid is None:
            return False
        with MM.lock:
            running = mid in MM.launched and mid not in MM.ended and token not in MM.departed.get(mid, ())
        if not running:
            return False
        SESS_LOST[token] = (time.time(), mid)
        cl = CLAN_CLOSING.get(token)
        if cl and time.time() - cl[0] <= CLAN_CLOSING_S and cl[1] in (None, mid):
            # the leave verdict comes after the re-attach window (SESSION_GRACE_S), long after CLAN_CLOSING_S: decide 'closed' now
            if len(CLAN_CLOSE_DROPS) > 4000:
                CLAN_CLOSE_DROPS.clear()
            CLAN_CLOSE_DROPS.add((token, mid))
            log("CLAN closed", tag(token), "mid", mid, "- its session dropped %.1f s after its window-close notice: its own leave" % (time.time() - cl[0]))
            clan_closed_book(token, mid)
        evt("session-lost", mid, tag(token), "session socket lost in a running match; waiting %.0f s for it to re-attach" % SESSION_GRACE_S)
        return True
    except Exception as e:
        log("SESSION-LOST ERR", repr(e))
        return False

def session_reattach(conn, addr, st):
    """A login+hello (0x0f+0x0e, no find-match) from a token that is in a launched, not ended match: the client re-opened its session socket
    (ext 'session reattach'). True when handled (the reply went out)."""
    tok_ = st.get("token")
    if not tok_ or len(tok_) != 64 or not MM.in_match(tok_):
        return False
    mid = MM.mid_of(tok_)
    with MM.lock:
        running = mid is not None and mid in MM.launched and mid not in MM.ended and tok_ not in MM.departed.get(mid, ())
    if not running:
        return False
    lost = SESS_LOST.get(tok_)
    old = SESSIONS.conn(tok_)
    if not (lost and lost[1] == mid and time.time() - lost[0] <= SESSION_GRACE_S + 15) and not (old is not None and old is not conn):
        return False                                       # nothing was lost: an ordinary menu announce
    shown = get_name(tok_) or st["name"]
    PRES.touch(tok_, shown)
    SESSIONS.register(tok_, conn)
    st["session"] = True; st["menu"] = True; st["authed"] = True; st["sent_name"] = shown; st["started"] = mid
    SESS_LOST.pop(tok_, None)
    conn.sendall(auth_reply_open(shown, msg(0x16, struct.pack("<I", max(1, PRES.count())))))
    log("REATTACH", addr, tag(tok_), "match", mid, "session socket back after %.1f s" % (time.time() - lost[0]) if lost else "(replaced a half-open socket)")
    evt("session-back", mid, tag(tok_), "session socket re-attached after %.1f s" % (time.time() - lost[0]) if lost else "(replaced a half-open socket)")
    if lost and lost[1] == mid:
        reattach_missed_start(tok_, mid, lost[0], conn)
    return True

REATTACH_START_DELAY_S = 2.0

def reattach_missed_start(tok_, mid, lost_at, conn):
    """A session socket that died on the loading screen missed the match's GAME-START (0x0a): it went out to the dead socket, the socket
    came back seconds later and no 0x0a ever came, so the client waited on the loading screen until the player closed the game. A start
    that went out while the socket was down is sent again to the re-attached socket. Only then: a client already in the match must not get a second 0x0a. Sent after
    REATTACH_START_DELAY_S, once the ext has set winmm's session state back to 4 (300 ms after the connect)."""
    js = JIP_STARTED.get(tok_)
    with MM.lock:
        if js and js[0] == mid:
            t_start = js[1]                                  # a joiner: its own GAME-START
        elif tok_ in MM.joining:
            t_start = None                                   # a joiner whose own GAME-START has not gone out yet: maybe_game_start sends it
        else:
            t_start = MM.started_t.get(mid) if mid in MM.started else None
    if t_start is None or t_start < lost_at:
        return False

    def _go():
        with MM.lock:
            running = mid in MM.launched and mid not in MM.ended and tok_ not in MM.departed.get(mid, ())
        if not running or MM.mid_of(tok_) != mid or SESSIONS.conn(tok_) is not conn:
            log("REATTACH start: not re-sent to", tag(tok_), "match", mid, "(left / ended / socket replaced)")
            return
        try:
            conn.sendall(msg(0x0a, b"\x01"))
        except OSError as e:
            log("REATTACH start: send to", tag(tok_), "failed", repr(e))
            return
        log("REATTACH start: GAME-START 0x0a re-sent to", tag(tok_), "match", mid,
            "(it went out %.1f s after the socket was lost)" % (t_start - lost_at))
        evt("session-back-start", mid, tag(tok_), "GAME-START re-sent: it went out while the session socket was down")

    threading.Timer(REATTACH_START_DELAY_S, _go).start()
    return True

# --------------------------------------------------- client / server version check
# A client of another build (different winmm / t2r-ext / pak) than the server plays "weirdly": wrong map list, wrong
# opcodes, desyncs. The release's version.txt (build_release.py: the git stamp) ships in both the client and the host
# folder; the launcher (Play.ps1) sends its version before it starts the game:
#   client -> server  0xB0 [u8 vlen][version][optional 64-hex token]
#   server -> client  0xB1 "ok=1|0 server=<v> client=<v> msg=<text to the end>"
# A mismatch is refused in the launcher with a clear message, and the daemon also refuses FIND MATCH for a token whose
# reported version differs (an in-game note, op 0x11) - for clients started some other way after Play.bat reported.
# No version.txt next to daemon.py (a dev tree) or T2R_VERSION= (empty) = no enforcement. A client that never reported
# (old launcher) is let in unless T2R_REQUIRE_VERSION=1.
OP_VERSION, OP_VERSION_REPLY = 0xB0, 0xB1

def _read_server_version():
    v = os.environ.get("T2R_VERSION")
    if v is not None:
        return v.strip()
    try:
        return open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "version.txt"), encoding="utf-8").read().strip()
    except OSError:
        return ""

SERVER_VERSION = _read_server_version()
REQUIRE_VERSION = os.environ.get("T2R_REQUIRE_VERSION", "0") == "1"
CLIENT_VERSIONS = {}      # token -> version string the launcher reported
# a server restart lost every running game's version (the game reports it once at launch, op 0xB0) and refused them all
# with "did not report its version". The table is kept next to the database and loaded at start (a game that updated re-reports).
VERSIONS_PATH = os.path.join(os.path.dirname(os.path.abspath(DB_PATH)), "client_versions.json")
try:
    with open(VERSIONS_PATH, "r", encoding="utf-8") as _f:
        CLIENT_VERSIONS.update({str(k): str(v) for k, v in json.load(_f).items()})
except Exception:
    pass

# Linux players ("asdf" on the full build got STEAM NOT CONNECTED): t2r-linux.py has no Steam helper (t2r-steam.exe needs
# Windows), so the Steam-only gate refused them. Its version report ends with the byte 'L' after the token; those tokens may queue
# without a helper and their matches run over the server relay (LINUX.txt). Kept next to the database like the versions.
LINUX_PATH = os.path.join(os.path.dirname(os.path.abspath(DB_PATH)), "linux_clients.json")
LINUX_CLIENTS = set()
try:
    with open(LINUX_PATH, "r", encoding="utf-8") as _f:
        LINUX_CLIENTS.update(str(t) for t in json.load(_f))
except Exception:
    pass

def _save_linux_clients():
    try:
        tmp = LINUX_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(sorted(LINUX_CLIENTS), f)
        os.replace(tmp, LINUX_PATH)
    except Exception as e:
        log("LINUX table not saved:", e)

def _save_client_versions():
    try:
        tmp = VERSIONS_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(CLIENT_VERSIONS, f)
        os.replace(tmp, VERSIONS_PATH)
    except Exception as e:
        log("VERSION table not saved:", e)
_VER_OK = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-+")

def parse_version_request(pl):
    """0xB0 payload -> (version, token or None), or None when malformed."""
    if not pl or pl[0] == 0 or len(pl) < 1 + pl[0]:
        return None
    ver = pl[1:1 + pl[0]].decode("latin1", "ignore")
    if not ver or len(ver) > 40 or any(ch not in _VER_OK for ch in ver):
        return None
    rest = pl[1 + pl[0]:1 + pl[0] + 64].decode("latin1", "ignore")
    tok = rest if len(rest) == 64 and all(ch in "0123456789abcdefABCDEF" for ch in rest) else None
    return ver, tok

# ---------------------------------------------------------------- per-player language for every server -> player text
# The server used to send one string to everybody ("English / Polish" in one note). Now each text is rendered for the token's LAST KNOWN game
# language: the game's TEXT language arrives in ops 0xB4 / 0xB6 / 0xBA (u8, 0 en / 1 pl) and the launcher's UI language in the same 0xBA poll.
# Only Polish is stored (absent = English, the default for an unknown token); kept next to client_versions.json and loaded at start.
LANGS_PATH = os.path.join(os.path.dirname(os.path.abspath(DB_PATH)), "client_langs.json")
CLIENT_LANGS = {}         # token -> "pl" (English tokens have no entry)
CLIENT_LANGS_MAX = 2000
try:
    with open(LANGS_PATH, "r", encoding="utf-8") as _f:
        CLIENT_LANGS.update({str(k): "pl" for k, v in json.load(_f).items() if v == "pl"})
except Exception:
    pass

def norm_lang(x):
    """A language in any form (None / b'' / 0 = English, 1 / b'\\x01' / "pl" / "pl-PL" = Polish) -> "en" | "pl"."""
    if isinstance(x, (bytes, bytearray)):
        x = x[0] if x else 0
    if isinstance(x, int):
        return "pl" if x == 1 else "en"
    s = str(x or "").strip().lower().replace("_", "-")
    return "pl" if s in ("1", "pl", "pol", "polish") or s.startswith("pl-") else "en"

def lang_of(token):
    """The language texts for this token are rendered in: its last known one, English when unknown."""
    return CLIENT_LANGS.get(token, "en")

def note_lang(token, lang):
    """Remember the language a client just told us (ops 0xB4 / 0xB6 / 0xBA). Unknown identities are not recorded (a flood of random tokens must not grow the table)."""
    if not isinstance(token, str) or len(token) != 64 or any(ch not in "0123456789abcdefABCDEF" for ch in token):
        return
    lang = norm_lang(lang)
    if lang == CLIENT_LANGS.get(token, "en"):
        return
    if token not in CLIENT_LANGS and token not in CLIENT_VERSIONS and not token_known(token):
        return
    if lang == "pl":
        if token not in CLIENT_LANGS and len(CLIENT_LANGS) >= CLIENT_LANGS_MAX:
            CLIENT_LANGS.pop(next(iter(CLIENT_LANGS)), None)           # oldest entry out
        CLIENT_LANGS[token] = "pl"
    else:
        CLIENT_LANGS.pop(token, None)
    _save_soon()

def _save_client_langs():
    try:
        tmp = LANGS_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(CLIENT_LANGS, f)
        os.replace(tmp, LANGS_PATH)
    except Exception as e:
        log("LANG table not saved:", e)

# Every text the daemon pushes to a player, English + Polish. The Polish forms keep the wording of tools/loc/pl/04-matchflow.txt (the game's own
# translation of the fixed notes) and carry the real diacritics: the game's notice / note path renders UTF-8 Polish (tools/ext/notice.c, loc tables).
SRV_TEXT = {
    "update_refusal": ("A new update is available - restart the launcher to update.",
                       "Dostępna nowa aktualizacja - uruchom ponownie launcher, aby ją zainstalować."),
    "update_available": ("New update available - restart after finishing the match.",
                         "Dostępna nowa aktualizacja - uruchom grę ponownie po zakończeniu meczu."),
    "ver_mismatch": ("Version mismatch: your game is build {a} but this server runs {b}. Get the current download from your host and start it with Play.bat.",
                     "Niezgodność wersji: twoja gra to kompilacja {a}, a serwer działa na {b}. Pobierz aktualną wersję od hosta i uruchom ją przez Play.bat."),
    "ver_mismatch_linux": ("Version mismatch: your game is build {a} but this server runs {b}. Press UPDATE in the Linux launcher (or get the current "
                           "download and run t2r-linux.py again), then start the game from Steam.",
                           "Niezgodność wersji: twoja gra to kompilacja {a}, a serwer działa na {b}. Naciśnij AKTUALIZUJ w launcherze na Linuksa (lub pobierz "
                           "aktualną wersję i uruchom ponownie t2r-linux.py), a potem uruchom grę ze Steama."),
    "ver_missing": ("Your game did not report its version. Start it with the current Play.bat from your host.",
                    "Twoja gra nie zgłosiła swojej wersji. Uruchom ją aktualnym plikiem Play.bat od hosta."),
    "ver_malformed": ("Malformed version request.", "Nieprawidłowe żądanie wersji."),
    "banned": ("You are banned from the server.", "Masz zakaz wstępu na serwer."),
    "steam_off": ("Steam is not connected: open Steam, then start the game with Play.bat",
                  "Steam nie jest połączony: uruchom Steam, a potem grę przez Play.bat"),
    # the game's login token is winmm's substitute (its game-data check flagged the install), so it never matches the Steam helper
    "files_changed": ("Your game files do not match Steam: verify the game files in Steam and remove other mods, then press PLAY in the launcher.",
                      "Twoje pliki gry nie zgadzają się ze Steamem: zweryfikuj pliki gry w Steamie i usuń inne mody, a potem naciśnij GRAJ w launcherze."),
    "files_edited": ("Your Twelve Weeks files are not the official build ({f}). Open the launcher, press Verify / Repair in Settings > Files, then PLAY.",
                     "Twoje pliki Twelve Weeks nie są oficjalną wersją ({f}). Otwórz launcher, naciśnij Verify / Repair w Settings > Files, a potem PLAY."),
    "files_unchecked": ("Twelve Weeks could not check your game files (old or changed t2r-ext.dll). Press PLAY in the launcher to update.",
                        "Twelve Weeks nie mógł sprawdzić twoich plików gry (stary lub zmieniony t2r-ext.dll). Naciśnij PLAY w launcherze, żeby zaktualizować."),
    "kicked": ("You got kicked from the match.", "Wyrzucono cię z meczu."),
    "kicked_why": ('You got kicked from the match. Reason: "{r}"', 'Wyrzucono cię z meczu. Powód: "{r}"'),
    # anti-AFK (the English text starts like the kick text: the ext shows it as the server toast on the menu, msg_is_kick)
    "afk_kicked": ("You got kicked from the match: no input for {s} seconds (AFK).", "Wyrzucono cię z meczu: brak aktywności przez {s} s (AFK)."),
    # ping limit (the ext's kick box reads the three numbers: ping, seconds, limit)
    "ping_kicked": ("You got kicked from the match: your ping to the host was {ms} ms for over {s} seconds (limit {lim} ms).",
                    "Wyrzucono cię z meczu: twój ping do hosta wynosił {ms} ms przez ponad {s} s (limit {lim} ms)."),
    "loss_kicked": ("You got kicked from the match: your connection lost {pct}% of its packets for over {s} seconds (limit {lim}%), the second time in a short while. Use a cable, close downloads and try again in {m} min.",
                    "Wyrzucono cię z meczu: twoje połączenie gubiło {pct}% pakietów przez ponad {s} s (limit {lim}%). Drugi raz w krótkim czasie. Użyj kabla, zamknij pobieranie i spróbuj ponownie za {m} min."),
    "loss_kicked_free": ("You got kicked from the match: your connection lost {pct}% of its packets for over {s} seconds (limit {lim}%). A cable or closing downloads helps. You can search again now.",
                         "Wyrzucono cię z meczu: twoje połączenie gubiło {pct}% pakietów przez ponad {s} s (limit {lim}%). Pomoże kabel albo zamknięcie pobierania. Możesz od razu szukać ponownie."),
    "loss_hold": ("Your connection lost {pct}% of its packets in your last match. You can search again in {m} min.",
                  "Twoje połączenie gubiło {pct}% pakietów w ostatnim meczu. Możesz szukać ponownie za {m} min."),
    # the admin team move - the client recognises this text (ext msg_is_move: "TEAMS:" prefix) and searches again by itself, straight back in on the other team
    "moved": ("TEAMS: you were moved to the other team - rejoining the match", "DRUŻYNY: przeniesiono cię do drugiej drużyny - wracasz do meczu"),
    "warn": ("Warning from the Twelve Weeks team: {t}", "Ostrzeżenie od zespołu Twelve Weeks: {t}"),
    "restart_in": ("Server restarting in {m} min. Finish your match - your progress is saved.",
                   "Restart serwera za {m} min. Dokończ mecz - twój postęp jest zapisany."),
    "restart_in_why": ("Server restarting in {m} min - {r}", "Restart serwera za {m} min - {r}"),
    "restart_cancel": ("Planned server restart cancelled.", "Planowany restart serwera został odwołany."),
    "shutdown": ("Server is shutting down. Matches in progress may end.", "Serwer się wyłącza. Trwające mecze mogą się zakończyć."),
    "jip_ended": ("The match ended while you were loading - searching again", "Mecz skończył się w trakcie wczytywania - szukam ponownie"),
    "host_lost": ("The host was lost while the map loaded - searching again", "Host zniknął w trakcie wczytywania mapy - szukam ponownie"),
    "dropped": ("A player dropped out - searching again", "Jeden z graczy wypadł - szukam ponownie"),
    "load_failed": ("Loading failed - the match was called off", "Wczytywanie nie powiodło się - mecz został odwołany"),
    "load_long": ("Loading took too long - the match started without you", "Wczytywanie trwało za długo - mecz zaczął się bez ciebie"),
    "load_long_off": ("Loading took too long - the match was called off", "Wczytywanie trwało za długo - mecz został odwołany"),
    "load_named": ("{n} could not load the map - the match was called off", "{n} nie mógł wczytać mapy - mecz został odwołany"),
}

def srv_text(key, lang="en", **kw):
    """One text of SRV_TEXT in a language ("en" | "pl" | anything norm_lang takes); {placeholders} filled from kw (values are never re-parsed)."""
    en, pl = SRV_TEXT[key]
    t = pl if norm_lang(lang) == "pl" else en
    return t.format(**kw) if kw else t

def srv_note(key, token, **kw):
    """The op 0x11 note bytes of a text for this token's language."""
    return srv_text(key, lang_of(token), **kw).encode("utf-8", "replace")[:0xf0]

UPDATE_REFUSAL_NOTE = srv_text("update_refusal")       # English form (kept for callers / tests); the live path renders per token: srv_text("update_refusal", lang_of(tok))

def version_mismatch_text(client_ver, linux=False, lang="en"):
    # (asdf's Linux client sat on build 73817f49 after the host moved to 5f499ac9; the note said "Play.bat", which Linux has not got)
    return srv_text("ver_mismatch_linux" if linux else "ver_mismatch", lang, a=client_ver, b=SERVER_VERSION)

def version_problem(token):
    """None = this token may play; else the note to show it (in the token's language)."""
    if not SERVER_VERSION:
        return None
    cv = CLIENT_VERSIONS.get(token)
    if cv is None:
        return srv_text("ver_missing", lang_of(token)) if REQUIRE_VERSION else None
    return None if cv == SERVER_VERSION else version_mismatch_text(cv, token in LINUX_CLIENTS, lang_of(token))

def version_refusal(token):
    """The note a NEW search of this token is refused with, else None. Grace (build 88 hotfix): a client of the previous build
    that is already IN a match (loading or running) is left alone - it finishes the match and is only refused when it searches again."""
    vp = version_problem(token)
    if not vp:
        return None
    try:
        # only a match still loading counts: a search from a match that already started drops that match (the stale-match rule in the
        # MATCHMAKE handler), so it is a NEW search and refused (iIVANL_90 on build 88 queued on the build 89 host that way)
        if MM.in_match(token) and not (token in LOADED or MM.match_age(token) > 120):
            return None
    except Exception:
        pass
    return srv_text("update_refusal", lang_of(token))

# The table sat at its cap of 2000 entries and every new player pushed the OLDEST entry out. A launch with an unchanged version did
# not move its entry, so the oldest entries were the regulars, evicted while their game ran; their next search was refused
# "(version) None" until they restarted the game.
# Now: least recently launched goes first (version_op moves a token to the newest end), a token with a live session or in the
# search / a match is never evicted, and the cap is 10000 (the per-address new-identity limit still guards against token floods).
CLIENT_VERSIONS_MAX = 10000
LINUX_CLIENTS_MAX = 500
def _versions_evict_one():
    """Drop the least recently launched token that is not online (no session socket, not searching, not in a match)."""
    for t in list(CLIENT_VERSIONS)[:64]:
        try:
            online = SESSIONS.conn(t) is not None or t in MM.queue or MM.in_match(t)     # (version_op runs without MM.lock: in_match takes it)
        except Exception:                           # noqa: BLE001 - an eviction check never breaks a version report
            online = False
        if not online:
            CLIENT_VERSIONS.pop(t, None)
            return t
    t = next(iter(CLIENT_VERSIONS), None)            # 64 oldest all online: the oldest goes (the table never grows past the cap)
    CLIENT_VERSIONS.pop(t, None)
    return t
_save_due = [0.0]
def _save_soon():
    """Debounced: the version / Linux tables are written at most every 5 s by the status loop (used to be one whole-file rewrite per request)."""
    _save_due[0] = _save_due[0] or time.time() + 5.0
def _save_tables_if_due(now=None):
    now = time.time() if now is None else now
    if _save_due[0] and now >= _save_due[0]:
        _save_due[0] = 0.0
        _save_client_versions(); _save_linux_clients(); _save_client_langs()

def version_op(pl, addr=None):
    """Op 0xB0 -> the 0xB1 reply frame (a malformed request is answered ok=0 as well, never an exception)."""
    r = parse_version_request(pl)
    srv = SERVER_VERSION or "dev"
    if not r:
        return msg(OP_VERSION_REPLY, ("ok=0 server=%s client=? msg=%s" % (srv, srv_text("ver_malformed"))).encode())
    ver, tok = r
    if tok and tok not in CLIENT_VERSIONS and tok not in LINUX_CLIENTS and addr is not None and not token_known(tok) and not NEW_TOKENS.allow(addr[0]):
        log("VERSION", addr, "refused to record: too many new identities from this address")      # (a flood of random tokens grew client_versions.json)
        tok = None
    if tok and len(pl) >= 1 + pl[0] + 65 and pl[1 + pl[0] + 64:1 + pl[0] + 65] == b"L" and tok not in LINUX_CLIENTS and len(LINUX_CLIENTS) < LINUX_CLIENTS_MAX:
        LINUX_CLIENTS.add(tok)
        _save_soon()
        evt("client-linux", tag(tok), "- may queue without a Steam helper (server relay)")
    if tok:
        if CLIENT_VERSIONS.get(tok) != ver:
            evt("client-version", tag(tok), ver, "server", srv)
            if tok not in CLIENT_VERSIONS and len(CLIENT_VERSIONS) >= CLIENT_VERSIONS_MAX:
                _versions_evict_one()
            CLIENT_VERSIONS.pop(tok, None)
            CLIENT_VERSIONS[tok] = ver
            _save_soon()
        elif next(reversed(CLIENT_VERSIONS), None) != tok:
            # An unchanged version used to stay where it was, so the table's "oldest" entries were the longest-known
            # players, not the least recently seen. Each launch now moves the token to the newest end (the json keeps the order).
            CLIENT_VERSIONS[tok] = CLIENT_VERSIONS.pop(tok)
            _save_soon()
    ok = (not SERVER_VERSION) or ver == SERVER_VERSION
    if not ok:
        log("VERSION mismatch: client", ver, "server", SERVER_VERSION, tag(tok) if tok else "")
    text = "ok=%d server=%s client=%s msg=%s" % (1 if ok else 0, srv, ver, "" if ok else version_mismatch_text(ver, False, lang_of(tok) if tok else "en"))
    return msg(OP_VERSION_REPLY, text.encode("utf-8", "ignore")[:400])

# PVP maps = the 0x08 "browser" list. Legit match start (page-state 3, 0x0c phase 2 +
# countdown 0) FNV-hashes record[ctx+0x18].s3 and starts that level under task node
# "to1-levels" (0x69e1bd6853680065, test-tasks.bin). s3 MUST be the level name.
# Record strings: s1 = map name label, s2 = map image (UI texture path, same style as
# the .rdata HUD paths), s3 = level name.
MAPS = [  # (display name - Title Case, image key, level name under to1-levels)
    ("Forest", "forest", "to1-forest"),
    ("Gas Station", "gas-station", "to1-gas-station"),
    ("School", "school", "to1-school"),
    ("Shipyard", "shipyard", "to1-shipyard"),
    ("Resort", "resort", "to1-resort"),
    ("Seraphite Camp", "seraphite-camp", "to1-seraphite-camp"),
    ("Houses", "houses", "to1-jesse-houses"),
    # No Return's hospital: the mod's placeholder task slot to1-custom-map-1, repointed by
    # build_dll (dc_data_patches.patch_hospital_map). Image: generated map-pvp-hospital (build_dll).
    ("Hospital", "hospital", "to1-custom-map-1"),
]

# PvP maps made from single-player sections (tools/maps/sp_maps.json, re/maps/technical.md). Always listed
# (fixed indices 8.. = t2r-ext MF_MAPS) but only in the pool when the DLL was built with them: the server's
# T2R_SP_MAPS must equal the build's (hardware,motel / all). A client whose DLL lacks a map can't start it.
SP_MAPS = [  # (display name, image key, level name, key in sp_maps.json)
    ("Hardware Store", "hardware", "to1-hardware", "hardware"),
    ("Motel", "motel", "to1-motel", "motel"),
    ("Hillcrest", "hillcrest-stores", "to1-hillcrest-stores", "hillcrest-stores"),
    ("TV Station", "tv-station", "to1-tv-station", "tv-station"),
    ("Train Station", "train-station", "to1-train-station", "train-station"),   # 12 (took Hardware's level-set slot)
    ("Downtown", "downtown", "to1-downtown", "downtown"),   # 13
    ("Lodge", "lodge", "to1-lodge", "lodge"),   # 14: RETIRED (not needed; sp_maps.json "retired"): index reserved, never in a pool / build
    ("Jackson Suburbs", "jackson-suburbs", "to1-jackson-suburbs", "jackson-suburbs"),   # 15
    ("Suburbs", "outpost", "to1-outpost", "outpost"),   # 16
    ("Bookstore (Part I)", "p1-bookstore", "to1-p1-bookstore", "p1-bookstore"),   # 17: TLOU Part I levels (tools/part1/p1_levelset.py)
    ("Hillside", "wilds-houses", "to1-wilds-houses", "wilds-houses"),   # 18: Ellie's infected wilds, the hillside before the Rattlers' snare trap
    ("Capitol Hill Shops", "capitol-shops", "to1-capitol-shops", "capitol-shops"),   # 19: Capitol Hill Olive St: donut shop + market + bank, upper floors (re/maps/capitol-shops.md)
    ("Convention Center", "into-the-city", "to1-into-the-city", "into-the-city"),   # 20: Seattle Day 3, the L-shaped street after the Theatre (re/maps/into-the-city.md); in the pool since PT54
    ("Overpass", "overpass", "to1-overpass", "overpass"),   # 21: Abby's Seattle Day 3 highway overpass (Scar Island), the No Return Overpass defense art (re/maps/overpass.md);
    ("Chase", "chase", "to1-chase", "chase"),   # 22: Abby's Seattle Day 3 sniper fight, the indoor part after the parking garage (footbridge, cruise terminal, restaurant; re/maps/chase.md); queue
    ("Supermarket", "jackson-market", "to1-jackson-market", "jackson-market"),   # 23: Jackson patrol supermarket + lot + courtyard, the winter map (re/maps/jackson-market.md);
    ("Plaza (Part I)", "p1-plaza", "to1-p1-plaza", "p1-plaza"),   # 24: TLOU Part I Financial District plaza (re/part1/plaza.md);, DLC like the Bookstore
]
def _sp_retired():
    """Keys that are NOT part of an "all" build = sp_maps.json "build": false (one source of truth with the build; the release host
 ships only daemon.py without tools/maps, so the fallback is the list as of: Hardware, Motel, Hillcrest and TV Station retired)."""
    try:
        import json
        spec = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "maps", "sp_maps.json"), encoding="utf-8"))["maps"]
        return {m["key"] for m in spec if not m.get("build")}
    except (OSError, ValueError, KeyError):
        return {"hardware", "motel", "hillcrest-stores", "tv-station", "lodge"}   # live 170: p1-bookstore ships (was retired in this fallback)
# Largest team size (players per team) each map is offered for - ONE table for the pool and the vote. A match of n
# players per team gets the maps with MAX_TEAM >= n only. Base maps are set here; the SP-section maps come from sp_maps.json
# ("max_team", default 4) with the fallback below when the release host ships daemon.py without tools/maps.
BASE_MAX_TEAM = {
    "to1-gas-station": 2,        # works, but 4v4 is chaos
    "to1-school": 2,
    "to1-seraphite-camp": 4,     # every map 4v4 except Gas Station / School
    "to1-custom-map-1": 4,       # Hospital
    "to1-resort": 4, "to1-jesse-houses": 4, "to1-shipyard": 4,
    "to1-forest": 4,             # UNKNOWN: left at 4, to be decided after a playtest
}
SP_MAX_TEAM_FALLBACK = {"to1-train-station": 4, "to1-downtown": 4, "to1-jackson-suburbs": 4, "to1-outpost": 4,
                         "to1-wilds-houses": 4, "to1-capitol-shops": 4, "to1-into-the-city": 4, "to1-overpass": 4, "to1-chase": 4, "to1-jackson-market": 4, "to1-p1-plaza": 4}


def max_team_table():
    """{level name: max team size}: BASE_MAX_TEAM + the SP-section maps (sp_maps.json "max_team", default 4)"""
    t = dict(BASE_MAX_TEAM)
    try:
        import json
        spec = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "maps", "sp_maps.json"), encoding="utf-8"))["maps"]
        t.update({m["level"]: int(m.get("max_team") or 4) for m in spec})
    except (OSError, ValueError, KeyError):
        t.update(SP_MAX_TEAM_FALLBACK)
    return t


sp_max_team = max_team_table          # (old name)

def team_size(players=()):
    """players per team of a match with these tokens (even split, rounded up); nobody known = 4 (a full 4v4)"""
    return (len(players) + 1) // 2 if players else 4

# No Return arenas built from the SP pipeline but regular pool maps: always built (tools/maps/sp_maps.json
# "always") and always offered, whatever T2R_SP_MAPS says
ALWAYS_MAPS = {"train-station"}


def sp_maps_on():
    """Level names of the SP-section maps enabled by T2R_SP_MAPS ("all" / "1" = every one, or keys)."""
    env = os.environ.get("T2R_SP_MAPS", "").strip()
    always = {lvl for _, _, lvl, key in SP_MAPS if key in ALWAYS_MAPS}
    if not env or env in ("0", "none"):
        return always
    keys = {k.strip() for k in env.split(",")} | ALWAYS_MAPS
    retired = _sp_retired()
    return {lvl for _, _, lvl, key in SP_MAPS if (env in ("all", "1") and key not in retired) or key in keys}

if sp_maps_on():        # listed (all of them, fixed indices) only on a server that enables any: live lists stay as before
    MAPS += [m[:3] for m in SP_MAPS]

# Respawn points per level (to1-spawn-point-1..n). Sent as the map record's last byte: winmm
# FUN_18004bfe0 picks respawns among 1..n (0 = its default 8). Levels with added respawns
# (tools/spawns/levels.py add) must be listed, or the new points are never used.
RESPAWNS = {"to1-forest": 16, "to1-gas-station": 11, "to1-school": 11, "to1-shipyard": 12, "to1-resort": 12,
            "to1-seraphite-camp": 10, "to1-jesse-houses": 14,
            # Hospital = the Ellie ground-floor set (sp_maps.json hospital-ellie, 16 respawns); T2R_HOSPITAL_OLD=1 (same value as the build) = the old NoR set, 13
            "to1-custom-map-1": 13 if os.environ.get("T2R_HOSPITAL_OLD") == "1" else 16,
            # SP-section maps: 13 = the Hospital template's count (tools/maps/sp_maps.py pak); update after placement
            "to1-hardware": 13, "to1-motel": 13, "to1-hillcrest-stores": 13, "to1-tv-station": 13,
            "to1-train-station": 15, "to1-downtown": 20, "to1-lodge": 13, "to1-jackson-suburbs": 12, "to1-outpost": 12,
            "to1-p1-bookstore": 14, "to1-wilds-houses": 12, "to1-capitol-shops": 13, "to1-into-the-city": 12,
            "to1-overpass": 17, "to1-chase": 13, "to1-jackson-market": 12, "to1-p1-plaza": 14}


def _sp_respawn_counts():
    """ SP-section maps whose pak carries generated respawns (tools/maps/respawn_fill.py): the count = the respawns
    (data/<key>-spawns.json) + the generated ones (data/<key>-respawns-gen.json), the order the pak build writes them. Without the data
    folder (a deployed daemon) the table above stays."""
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "maps", "data")
    for _n, _i, lvl, key in SP_MAPS:
        gp, sp = os.path.join(d, f"{key}-respawns-gen.json"), os.path.join(d, f"{key}-spawns.json")
        if not (os.path.exists(gp) and os.path.exists(sp)):
            continue
        try:
            n = len(json.load(open(sp))["respawns"]) + len(json.load(open(gp))["respawns"])
        except (OSError, ValueError, KeyError):
            continue
        RESPAWNS[lvl] = min(n, 255)


_sp_respawn_counts()

# DLC map packs (tools/map_packs.py, re/part1/map-packs.md): with T2R_MAP_PACKS=1 the registry's maps follow at the fixed
# indices PACK_BASE.. (the list is padded with the SP-section maps first, which stay out of the pool unless T2R_SP_MAPS
# enables them). A pack map joins a match's pool / vote only when EVERY player of the match reported its pack (0xC6).
# No registry entries (or the flag off) = no change at all.
PACKS = map_packs.Packs(map_packs.load_registry() if map_packs.enabled() else [])
SPSUP = map_packs.SpSupport([m[2] for m in SP_MAPS])      # which SP-section maps each client's DLL has (0xC8)
log("SPMAPS", "gating", "ON (a map is offered only when every player reported it, 0xC8)" if map_packs.sp_gating()
    else "OFF (T2R_SP_GATE=0: enabled SP maps are offered without any report)", "| enabled:", sorted(sp_maps_on()) or "none")


# PART I MAPS preference (OPTIONS > MULTIPLAYER, 0xC8 flags byte): ALWAYS players queue only with players who own the Part I maps (any
# preference), and a match with an ALWAYS player always has a Part I map among its vote options. DEFAULT = the behaviour before the option
# (a Part I map is in the vote pool when every player of the match owns it). T2R_P1_PREF=0 turns
# the whole rule off on a server; it is also off while no Part I map is in this server's pool (nothing to be "always" about).
MATCH_P1 = {}                 # match id -> True when it was formed as an owners match with an ALWAYS player


def p1_pref_on():
    if os.environ.get("T2R_P1_PREF", "1") == "0" or not map_packs.sp_gating():
        return False
    want = [x.strip() for x in os.environ.get("T2R_MAP_POOL", "").split(",") if x.strip()]
    on = sp_maps_on() | set(want)
    return any(map_packs.P1_MARK in lvl and lvl in on for _n, _i, lvl, _k in SP_MAPS)


def p1_always(token, leader=None):
    """Is this searcher (its party leader when it has one) counted as PART I MAPS = ALWAYS?"""
    try:
        return p1_pref_on() and SPSUP.p1_always(leader or token)
    except Exception:                                              # noqa: BLE001 - never breaks the queue
        return False


def p1_owner(token, leader=None):
    """Owns the Part I maps (its own report), or searches with an ALWAYS party leader (the party stays together)."""
    try:
        return p1_pref_on() and (SPSUP.p1_owner(token) or p1_always(token, leader))
    except Exception:                                              # noqa: BLE001 - never breaks the queue
        return False


def p1_fits(token, mid, leader=None):
    """May `token` join the running match `mid`? An owners match with an ALWAYS player takes owners only; another match takes everyone
    but ALWAYS players (unknown match = yes)."""
    if not p1_pref_on() or mid not in MATCH_P1:
        return True
    return p1_owner(token, leader) if MATCH_P1[mid] else not p1_always(token, leader)


def p1_match(players):
    """True when one of these players is ALWAYS (the vote then offers a Part I map, when the pool has one every player owns)."""
    ps = [p for p in players if not (isinstance(p, str) and p.startswith("f0" * 30))]
    return bool(ps) and p1_pref_on() and any(SPSUP.p1_always(p) for p in ps)


if PACKS.maps():
    for _m in SP_MAPS:
        if len(MAPS) >= map_packs.PACK_BASE:
            break
        if _m[:3] not in MAPS:
            MAPS.append(_m[:3])
    assert len(MAPS) == map_packs.PACK_BASE, "map list must reach PACK_BASE before the pack maps"
    for _name, _img, _lvl, _resp in PACKS.maps():
        MAPS.append((_name, _img, _lvl))
        RESPAWNS[_lvl] = _resp

BROKEN_MAPS = set()   # Seraphite Camp back (the crash looked random)

MATCH_MAP = {}            # frozenset(match tokens) -> map index (same for every player)

_POOL_LOGGED = set()


def _interrogation_players(players):
    """is the (formed) match of these players an Interrogation match? (server-forced T2R_MODE, or the players' queue mode picks it)"""
    try:
        if INT.mode_env in ("int", "interrogation"):
            return True
        ps = set(players)
        if not ps:
            return False
        for mid, pl in list(MM.matches.items()):
            if ps & set(pl):
                return MODEQ.match_mode(mid) == "interrogation"
    except NameError:       # (map_pool before the mode objects exist)
        pass
    return False


def map_pool(players=(), support=None):
    """Levels in play: T2R_MAP_POOL="to1-forest,to1-school,..." (default: all MAPS). Pack maps only when every
    one of `players` has the pack (none without players)."""
    # PT60 a host-migration
    # placeholder token (hostmig.placeholder, a slot whose player came back elsewhere) has no SP-map / pack report, so every SP / pack map failed
    # the "every player has it" tests - placeholders are not players
    ph = ("f0" * 30)
    players = [p for p in players if not (isinstance(p, str) and p.startswith(ph))]
    # (two joiners refused from a running Into the City match after a daemon restart: the players already in it had not re-sent their
    # SP-map / pack reports): `support` = whose packs / SP maps must be checked (a joiner: only itself - the others are already playing that map)
    sup = players if support is None else [p for p in support if not (isinstance(p, str) and p.startswith(ph))]
    want = [x.strip() for x in os.environ.get("T2R_MAP_POOL", "").split(",") if x.strip()]
    # default: every map not in BROKEN_MAPS (Seraphite Camp's old crash, exe+0x13d5671 in the
    # vegetation grid query, is guarded in the ext since - see re/crash-sites.md)
    # SP-section maps only when this server's T2R_SP_MAPS enables them (or T2R_MAP_POOL names them)
    sp_off = {m[2] for m in SP_MAPS} - sp_maps_on()
    # maps taken out of the vote / rotation (T2R_MAP_EXCLUDE="to1-school,to1-gas-station"; T2R_MAP_POOL still wins when set)
    sp_off = sp_off | {x.strip() for x in os.environ.get("T2R_MAP_EXCLUDE", "").split(",") if x.strip()}
    # team-size filter (max_team_table): only maps whose max team size fits the match (no players known = a full 4v4)
    n = team_size(players)
    big = {lvl for lvl, mt in max_team_table().items() if mt < n} if os.environ.get("T2R_MAX_TEAM", "1") != "0" else set()   # T2R_MAX_TEAM=0: filter off (tests)
    idx = [i for i, m in enumerate(MAPS) if (m[2] in want if want else m[2] not in BROKEN_MAPS | sp_off)
           and m[2] not in big and PACKS.allowed(m[2], sup) and SPSUP.allowed(m[2], sup)]
    if not idx:      # never an empty pool: the maps that fit this team size, else every non-SP / non-pack map
        idx = [i for i, m in enumerate(MAPS) if m[2] not in sp_off | big and not PACKS.is_pack_map(m[2]) and not SPSUP.is_sp(m[2])]
    if not idx:
        idx = [i for i, m in enumerate(MAPS) if m[2] not in sp_off and not PACKS.is_pack_map(m[2]) and not SPSUP.is_sp(m[2])]
    dropped = sorted(m[2] for m in MAPS if m[2] in big and m[2] not in sp_off)
    # an Interrogation match only gets maps that have lockbox spots (tools/interrogation_lockboxes.py); none left = unfiltered (start then falls back to Supply Raid)
    if _interrogation_players(players):
        lb = INT.lockboxes
        keep = [i for i in idx if i in lb] if lb else idx
        if keep and len(keep) != len(idx):
            log("POOL", "Interrogation match: maps without lockboxes dropped:", sorted(MAPS[i][2] for i in idx if i not in lb))
            idx = keep
    if (n, tuple(dropped), len(idx)) not in _POOL_LOGGED:
        _POOL_LOGGED.add((n, tuple(dropped), len(idx)))
        log("POOL", f"team size {n}: {len(idx)} maps, dropped by max_team: {', '.join(dropped) or 'none'}")
    return idx

_rotation = [secrets.randbelow(len(MAPS))]   # rotation starts at a random map each server run

def map_index(players=()):
    """T2R_MAP = "rotate" (default: next map of the pool each match), "random" (random
    pick from the pool, never the same map twice in a row) or one level name."""
    key = frozenset(players)
    if key in MATCH_MAP:
        return MATCH_MAP[key]
    want = os.environ.get("T2R_MAP", "rotate")
    pool = map_pool(players)
    if want == "rotate":
        idx = pool[_rotation[0] % len(pool)]
        _rotation[0] += 1
    elif want == "random":
        choices = [i for i in pool if i != _rotation[0]] or pool
        idx = choices[secrets.randbelow(len(choices))]
        _rotation[0] = idx
    else:
        idx = next((i for i, m in enumerate(MAPS) if m[2] == want and PACKS.allowed(m[2], players) and SPSUP.allowed(m[2], players)), pool[0])
    if key:
        MATCH_MAP[key] = idx
        log("MAP", MAPS[idx][2], [tag(p) for p in players])
    return idx

# ---------------------------------------------------------------- MATCH FLOW (2013 find match)
# T2R_MAP_VOTE=1 (the test launcher and Start Server.bat set it; the daemon alone defaults to off):
# between "match found" and the spawn the 2013 flow runs as the client's lobby seconds (0x0c phase 2,
# n = code):  5..1 STARTING MATCH IN n  ->  MAP VOTE (160 + n, VOTE_SECS..1)  ->  CHOOSE LOADOUT
# (100 + n)  ->  ONE-USE BOOSTER (130 + n)  ->  [LOADING (200 + n), T2R_LOADING_SECS > 0 only]  ->  1
# (the ext's match horn holds the level start ~5 s here)  ->  0 = level start.
# the old CHOOSE LOADOUT window (T2R_COUNTDOWN, 10 s) is split in half - 5 s loadout + 5 s
# booster pick - so the flow after the vote takes the same time as before (flow_stage_secs).
# T2R_MATCH_FLOW=1 = the loadout / booster / loading stages without the vote (fallback if the vote misbehaves).
# The vote offers VOTE_OPTIONS maps of the pool; the ext polls 0xEA and sends 0xEC. The 0x04 match record
# goes out first with the first option as a placeholder; when the vote closes the winner replaces it
# (MATCH_MAP), the match record is SENT AGAIN with the winning map right before the next 0x0c step (same
# segment: the 0x04 handler resets the phase, the 0x0c sets it back at once), and "w=" in the vote state
# lets the ext write it into the lobby object's map byte too (belt and braces: a client whose vote polls
# failed still loads the right level).
VOTE_OPTIONS = 3
VOTE_SECS = int(os.environ.get("T2R_VOTE_SECS", "15"))
BOOSTER_SECS = int(os.environ["T2R_BOOSTER_SECS"]) if os.environ.get("T2R_BOOSTER_SECS") else None   # None = the other half
LOADING_SECS = int(os.environ.get("T2R_LOADING_SECS", "0"))   # 0 = no LOADING stage (booster -> horn -> level)
VOTES = {}                # frozenset(match tokens) -> {"opts": [map idx], "votes": {token: option}, "winner": map idx or -1}
_vote_lock = threading.Lock()

def map_vote_on():
    return os.environ.get("T2R_MAP_VOTE", "") == "1"

def match_flow_on():
    """The 2013 stages after the lobby (loadout half / booster half [/ loading]) - with the vote, or alone."""
    return map_vote_on() or os.environ.get("T2R_MATCH_FLOW", "") == "1"

def flow_stage_secs(secs=None):
    """(loadout, booster, loading) seconds of the flow for a CHOOSE LOADOUT window of `secs` (T2R_COUNTDOWN):
    the window is halved, the booster pick gets the other half (T2R_BOOSTER_SECS overrides)."""
    if secs is None:
        secs = int(os.environ.get("T2R_COUNTDOWN", "10"))
    lo = max(1, secs // 2)
    bo = BOOSTER_SECS if BOOSTER_SECS is not None else max(1, secs - lo)
    return lo, max(1, bo), max(0, LOADING_SECS)

def flow_extra_secs():
    """Seconds between the end of the CHOOSE LOADOUT stage and the level start (booster + loading + the final
    '1' step); 0 without the flow. Sent to the ext (meta reply "mf=") so its MATCH STARTS IN timer is right."""
    if not match_flow_on():
        return 0
    _, bo, ld = flow_stage_secs()
    return bo + ld + 1

# a 2-in-1 chance per option slot against each other map of the pool: the ported maps are favoured a bit (Train Station;
# - Hillside, Capitol Hill Shops, Convention Center)
VOTE_WEIGHT = {"to1-train-station": 2, "to1-wilds-houses": 2, "to1-capitol-shops": 2, "to1-into-the-city": 2, "to1-overpass": 2, "to1-chase": 2, "to1-jackson-market": 2, "to1-p1-plaza": 2}


VOTE_RECENT = []       # map idx of the last votes' winners (newest last) -
VOTE_SHOWN = {}        # map idx -> how many of the last votes offered it (decays: each vote halves the rest)


def vote_options(n=VOTE_OPTIONS, players=()):
    """n distinct maps of the pool. the vote kept offering the same few maps - Train Station took the guaranteed "ported map" slot every
    time (the only favoured map in the PT82-84 pool) and nothing tracked what was played, so a 6-8 map pool showed the same three. Now: the last
    winners stay out while the pool has enough other maps, the favoured slot goes to a favoured map only if it did not just win, and every pick's
    tickets fall with how often the map was offered lately (VOTE_SHOWN), so the whole pool comes round."""
    pool = map_pool(players)
    if os.environ.get("T2R_VOTE_ALL", "") == "1":
        # test server: the whole pool, in pool order
        # (the ext pages its 3 tiles over the list, MF_GRID = 20 cells)
        return list(pool)[:20]
    recent = VOTE_RECENT[-max(1, min(3, len(pool) - n)):] if len(pool) > n else []
    cand = [i for i in pool if i not in recent] if len(pool) - len(set(recent) & set(pool)) >= n else list(pool)
    out = []
    # one slot always offers this map while it is in the pool (T2R_VOTE_ALWAYS=<map name>[,<map>...], e.g. to1-jackson-market; empty = off)
    for nm in [x for x in os.environ.get("T2R_VOTE_ALWAYS", "").split(",") if x]:
        hit = [i for i in pool if MAPS[i][2] == nm and i not in out]
        if hit and len(out) < n:
            out.append(hit[0])
            if hit[0] in cand:
                cand.remove(hit[0])
    # (PT54: a vote of Hospital / School / Gas Station): one slot goes to a favoured (ported) map when the pool has one (not the last winner)
    fav = [i for i in cand if VOTE_WEIGHT.get(MAPS[i][2], 1) > 1 and VOTE_SHOWN.get(i, 0) < 1.5]
    if fav and n > 0:
        pick = fav[secrets.randbelow(len(fav))]
        out.append(pick)
        cand.remove(pick)
    while cand and len(out) < n:
        # tickets: favoured maps x2, every map fewer the more it was offered lately (12 base tickets: shown 0 -> 12, 1 -> 6, 2 -> 4)
        tickets = [i for i in cand for _ in range(max(1, int(12 * VOTE_WEIGHT.get(MAPS[i][2], 1) / (1.0 + VOTE_SHOWN.get(i, 0)))))]
        pick = tickets[secrets.randbelow(len(tickets))]
        out.append(pick)
        cand.remove(pick)
    # PART I MAPS = ALWAYS (every player of the match): at least one option is a Part I map of the pool
    if out and p1_match(players):
        p1 = [i for i in pool if map_packs.P1_MARK in MAPS[i][2]]
        if p1 and not any(i in p1 for i in out):
            out[-1] = p1[secrets.randbelow(len(p1))]
    if os.environ.get("T2R_VOTE_ALWAYS"):
        pass   # the always-offered maps keep the FIRST slots in T2R_VOTE_ALWAYS order (Bookstore first, prominently); was a random slot
    for k in list(VOTE_SHOWN):
        VOTE_SHOWN[k] *= 0.5
    for i in out:
        VOTE_SHOWN[i] = VOTE_SHOWN.get(i, 0) + 1.0
    return out

def vote_start(players):
    """Open the vote of a formed match; the placeholder map (first option) goes into the match record."""
    key = frozenset(players)
    opts = vote_options(players=players)
    with _vote_lock:
        VOTES[key] = {"opts": opts, "votes": {}, "winner": -1}
        MATCH_MAP[key] = opts[0]
    log("VOTE open", [MAPS[i][2] for i in opts], [tag(p) for p in players])
    return opts

def vote_cast(token, option):
    """Cast or change a vote (option = 0..n-1). True when counted."""
    pl = MM.players_of(token) or []
    with _vote_lock:
        v = VOTES.get(frozenset(pl))
        if not v or v["winner"] >= 0 or not (0 <= option < len(v["opts"])):
            return False
        v["votes"][token] = option
    log("VOTE", tag(token), "->", MAPS[v["opts"][option]][2])
    return True

def vote_tally(v):
    c = [0] * len(v["opts"])
    for o in v["votes"].values():
        if 0 <= o < len(c):
            c[o] += 1
    return c

def vote_close(players):
    """Pick the winner (most votes, a tie or no votes at random) and make it the match's map."""
    key = frozenset(players)
    with _vote_lock:
        v = VOTES.get(key)
        if not v:
            return None
        if v["winner"] < 0:
            c = vote_tally(v)
            best = [k for k, n in enumerate(c) if n == max(c)]
            v["winner"] = v["opts"][best[secrets.randbelow(len(best))]]
            MATCH_MAP[key] = v["winner"]
            VOTE_RECENT.append(v["winner"]); del VOTE_RECENT[:-6]
            log("VOTE closed", MAPS[v["winner"]][2], c, [tag(p) for p in players])
        return v["winner"]

def vote_state_text(token):
    """The 0xEB reply: st=0 no vote, 1 voting, 2 decided; m0.. option map index, v0.. votes, my = my option
    (-1 none), w = winning map index (-1 open), pc = players, p0.. names (spaces as _)."""
    pl = MM.players_of(token) or []
    with _vote_lock:
        v = VOTES.get(frozenset(pl))
        if not v:
            return "st=0"
        c = vote_tally(v)
        t = ["st=%d" % (2 if v["winner"] >= 0 else 1), "n=%d" % len(v["opts"])]
        t += ["m%d=%d v%d=%d" % (k, m, k, c[k]) for k, m in enumerate(v["opts"])]
        t += ["my=%d" % v["votes"].get(token, -1), "w=%d" % v["winner"], "pc=%d" % len(pl)]
        for k, p in enumerate(pl[:8]):
            nm = "".join(ch if ch.isalnum() or ch in "-." else "_" for ch in (get_name(p) or "PLAYER %d" % (k + 1)))[:20]
            t.append("p%d=%s" % (k, nm))
            t.append("pv%d=%d" % (k, 1 if p in v["votes"] else 0))
            t.append("po%d=%d" % (k, v["votes"].get(p, -1)))      # the roster row names the map this player voted for (older clients ignore it)
            rkw = rank_of_token(p)                   # the rank badge of the roster row (exact: the name above is cut / sanitised)
            t.append("pk%d=%d" % (k, rkw[0]))
            t.append("pw%d=%d" % (k, rkw[1]))
    return " ".join(t)

def vote_op(op, pl):
    """Ops 0xEA / 0xEC -> reply frame (0xEB), or None for a bad payload."""
    if len(pl) < 64:
        return None
    token = pl[:64].decode("latin1", "ignore")
    if not all(ch in "0123456789abcdefABCDEF" for ch in token):
        return None
    if op == 0xEC:
        if len(pl) < 65:
            return None
        vote_cast(token, pl[64])
    elif op != 0xEA:
        return None
    return msg(0xEB, vote_state_text(token).encode())

def sp_support_op(pl):
    """Op 0xC8 [64B token][u32 LE mask] -> reply 0xC9 (map_packs.SpSupport.reply_text)."""
    r = map_packs.parse_sp_report(pl)
    if not r:
        return None
    token, mask = r
    if not map_packs.sp_gating():
        return msg(map_packs.SP_OP_REPLY, b"on=0")
    flags = map_packs.parse_sp_flags(pl)
    SPSUP.report(token, mask, flags)
    log("SPMAPS", tag(token), "%04x" % mask, [m[2] for k, m in enumerate(SP_MAPS) if (mask >> k) & 1],
        "| PART I MAPS: %s" % ("ALWAYS" if SPSUP.p1_always(token) else "DEFAULT"))
    return msg(map_packs.SP_OP_REPLY, SPSUP.reply_text(token).encode())

def map_packs_op(pl):
    """Op 0xC6 [64B token][u8 n][n x u32 hash] -> reply 0xC7 (map_packs.Packs.reply_text), on=0 with the flag off."""
    r = map_packs.parse_report(pl)
    if not r:
        return None
    token, hashes = r
    if not map_packs.enabled():
        return msg(map_packs.OP_REPLY, b"on=0")
    ids = PACKS.report(token, hashes)
    log("PACKS", tag(token), ["%08x" % h for h in hashes], "->", sorted(ids))
    return msg(map_packs.OP_REPLY, PACKS.reply_text(token, MAPS).encode())

def countdown_steps(pre=5, secs=10, vote=False, flow=None):
    """The 0x0c steps (phase, n) of one match start; see MATCH FLOW above. flow = the 2013 stages after the
    lobby (default: on whenever the vote is, or T2R_MATCH_FLOW=1)."""
    if flow is None:
        flow = vote or match_flow_on()
    steps = [(1, 0)] + [(2, n) for n in range(pre, 0, -1)]
    if vote:
        steps += [(2, 160 + n) for n in range(VOTE_SECS, 0, -1)]
    if flow:
        lo, bo, ld = flow_stage_secs(secs)
        steps += [(2, 100 + n) for n in range(lo, 0, -1)]
        steps += [(2, 130 + n) for n in range(bo, 0, -1)]
        steps += [(2, 200 + n) for n in range(ld, 0, -1)]
    else:
        steps += [(2, 100 + n) for n in range(secs, 1, -1)]
    return steps + [(2, 1), (2, 0)]

def countdown_seconds(pre=None, secs=None):
    """Length of one whole match-start countdown in seconds (the 2 s pause before it included)."""
    if pre is None:
        pre = int(os.environ.get("T2R_PRE_COUNTDOWN", "5"))
    if secs is None:
        secs = int(os.environ.get("T2R_COUNTDOWN", "10"))
    return 2.0 + len(countdown_steps(pre, secs, map_vote_on()))

SID_KEEP = {}   # frozenset(match tokens) -> session id kept from before a join-in-progress grew / changed the player list

def match_sid(players):
    """Shared non-zero u32 match session id (same for every player in the match). It is a hash of the player list, so a
    join-in-progress (jip_join) pins the id the running players already hold (their 0x04 record, the relay hello and the Steam
    helpers' 0xF2 table carry it) under the new list's key."""
    kept = SID_KEEP.get(frozenset(players))
    if kept:
        return kept
    return (zlib.crc32("".join(players).encode()) & 0xFFFFFFFF) or 1

def roster_team(slot):
    """Team written into roster records (0x04 match record, 0xE5 jip roster reply). TEST ONLY: T2R_TEST_ONE_TEAM puts everyone on team 0."""
    return 0 if os.environ.get("T2R_TEST_ONE_TEAM") else team_of(slot)

def team_of(slot):
    return slot % 2                         # alternate teams: slot 0,2 -> team 0; 1,3 -> team 1

# In-match team swap (admin_move_team): the admin moves a player to the other team WITHOUT a kick. Its slot (= its P2P peer, its net
# record) stays; only its TEAM changes. mid -> {token: team}. A token keeps its entry for the rest of the match (moved back = its parity team
# again, still listed, so a client that applied the first move also applies the way back). The override belongs to the TOKEN: a slot that a
# leaver freed and a joiner took gets the joiner's parity team. Read by every team decision about a running match (team_in).
TEAM_SWAP = {}
TS_CAP_S = 30        # a flag-0x10 poll this recent = the ext is in the match and applies the swap (polls come every 2-3 s)
TS_CAP = {}          # token -> time its t2r-ext last said it applies the 0xE5 team-swap block (0xE3 flag 0x10); older exts fall back to kick + rejoin

def team_in(mid, slot, tok=None):
    """Team of `slot` in match `mid`: the admin's in-match swap for the token in that slot, else the slot parity."""
    sw = TEAM_SWAP.get(mid) if mid is not None else None
    if sw:
        if tok is None:
            pl = MM.matches.get(mid) or ()
            tok = pl[slot] if 0 <= slot < len(pl) else None
        if tok in sw:
            return sw[tok]
    return team_of(slot)

def roster_team_in(mid, slot, tok=None):
    """roster_team with the in-match swap (T2R_TEST_ONE_TEAM still wins)."""
    return 0 if os.environ.get("T2R_TEST_ONE_TEAM") else team_in(mid, slot, tok)

def build_player_record(slot=0, sid=0, map_idx=0, roster=(), relay=None, team_fn=None):
    """0x04 match record (server->client), handler 0x180057e48. Payload offsets:
      +0x00 [15]  gameplay relay IP string (FUN_180060000 does inet_addr(ctx+0))
      +0x10 u16   relay port                         -> ctx+0x10
      +0x12 u32   match session id (relay hello)     -> ctx+0x14
      +0x16 u8    map index into the 0x08 map list   -> ctx+0x18
      +0x17 u8    LOCAL slot: 0 host, >0 client      -> ctx+0x19 (net path 0x18005afe0 / 0x18005b170)
      +0x18 u8    roster count                       -> ctx+0x1a
      +0x19 u8                                       -> ctx+0x1b (passed to FUN_1800336f0)
      +0x1a u8    LOCAL team (0/1)                   -> ctx+0x1c
      +0x1b ...   roster entries, 0x32 bytes each    -> ctx+0x1d + i*0x32
                  [0] player slot/id, [0x31] team. Spawn chooser FUN_1800336f0 picks
                  "to1-t<team>-intro-spawn-point-<n>", n = 1 + teammates with lower id.
    Entries are only parsed while the frame is long enough (handler length gates)."""
    n = max(1, len(roster))
    p = bytearray(0x1b + n * 0x32 + 8)
    if relay:                                                      # Steam P2P: the local helper
        ip, port = relay[0].encode(), relay[1]
    else:
        ip = os.environ.get("T2R_PUB_IP", "209.25.141.16").encode()   # gameplay relay IP
        port = int(os.environ.get("T2R_PUB_UDP_PORT", "15389"))       # gameplay relay port
    p[0:len(ip)] = ip
    struct.pack_into("<H", p, 0x10, port)
    struct.pack_into("<I", p, 0x12, sid)
    p[0x16] = map_idx
    p[0x17] = slot
    p[0x18] = n
    rteam = team_fn or roster_team       # team_fn: the running match's teams with the admin's in-match swaps (match_start_frame)
    p[0x1a] = rteam(slot)
    if os.environ.get("T2R_TEST_SAME_SPAWN") or os.environ.get("T2R_TEST_ONE_TEAM"):   # TEST ONLY: team-0 for everyone
        p[0x1a] = 0
    for i, (rslot, name) in enumerate(roster):
        e = 0x1b + i * 0x32
        p[e] = rslot
        nm = name.encode("latin1", "ignore")[:0x2e]
        p[e + 1:e + 1 + len(nm)] = nm
        p[e + 0x31] = rteam(rslot)   # TEST: one team
    return bytes(p)

def build_peer_table(players):
    """0x14 scoreboard/peer table: [u8 count][count x 0x30 record].
    record = [32B name null-pad][u32@0x20][u32@0x24][f32@0x28][u32@0x2c] (name+stats).
    payload length must be > count*0x30. Signals both players present."""
    n = min(len(players), 10)
    body = bytes([n])
    for tok in players[:n]:
        nm = (get_name(tok) or "PLAYER").encode("latin1", "ignore")[:31]
        rec = bytearray(0x30)
        rec[0:len(nm)] = nm
        body += bytes(rec)
    return body + b"\x00"                              # +1 so len > count*0x30

def match_start_frame(tok, players=None):
    """match-start bytes: 0x04 match record (slot 0 = host) + optional 0x11 notification.
    The 0x0c countdown (countdown()) follows. (0x16 = online count, 0x14 = leaderboard.)"""
    players = players or [tok]
    slot = players.index(tok) if tok in players else 0
    # ("after it found a match one of the players was PLAYER 6"): a host-migration placeholder (hostmig.placeholder, f0f0...) holds a
    # slot nobody plays - it is not a roster entry (the joiner's lobby / scoreboard showed it as 'PLAYER n')
    roster = [(i, get_name(t) or f"PLAYER {i + 1}") for i, t in enumerate(players) if not (isinstance(t, str) and t.startswith("f0" * 30))]
    relay = ("127.0.0.1", BRIDGES.port(tok)) if MATCH_P2P.get(frozenset(players)) else None
    mid = MM.mid_of(tok) if tok in players else None
    tf = (lambda sl: roster_team_in(mid, sl, players[sl] if 0 <= sl < len(players) else None)) if mid is not None and TEAM_SWAP.get(mid) else None
    rec = build_player_record(slot=slot, sid=match_sid(players), map_idx=map_index(players),
                              roster=roster, relay=relay, team_fn=tf)
    frame = msg(0x04, rec)
    # 0x11 = notification text (shown via FUN_18000bc00), NOT a level loader.
    note = os.environ.get("T2R_NOTE", "")
    if note:
        frame += msg(0x11, note.encode()[:0xf0])
    return frame

# Load timing: per match (frozenset of players) the stage marks, one summary line at GAME-START.
TIMING = {}

def stage_name(phase, n):
    if phase == 1: return "match found"
    if n == 0: return "level start"
    if n == 1: return "horn"
    if n >= 200: return "loading"
    if n >= 160: return "vote"
    if n >= 130: return "booster"
    if n >= 100: return "loadout"
    return "pre-countdown"

def timing_mark(players, what):
    t = TIMING.get(frozenset(players))
    if t is None: return None
    now = time.time()
    if what not in t["marks"]:
        t["marks"][what] = now
        log("load timing:", what, "at +%.1f s" % (now - t["start"]))
    return t

def timing_summary(players):
    t = TIMING.pop(frozenset(players), None)
    if not t: return
    m, gs = t["marks"], time.time()
    ls = m.get("level start", gs)
    first = min(t["loaded"].values()) if t["loaded"] else gs
    stages = ", ".join("%s +%.1f" % (k, v - t["start"]) for k, v in sorted(m.items(), key=lambda kv: kv[1]))
    loaded = ", ".join("%s +%.1f" % (k, v - ls) for k, v in t["loaded"].items())
    log("load timing: flow %.1f s, level load %.1f s, wait for others %.1f s (total %.1f s) | stages: %s | loaded after level start: %s"
        % (ls - t["start"], first - ls, gs - first, gs - t["start"], stages, loaded))

def start_match(players):
    """Push the match-start to every player's held session conn (best-effort; the
    reliable delivery is on the live keepalive conn in handle_batch)."""
    TIMING[frozenset(players)] = {"start": time.time(), "marks": {}, "loaded": {}}
    # rework: ready flags are NOT reset per round any more - a party that just
    # played stays ready, so the leader's next FIND MATCH takes everyone along (the per-round reset made every member press READY UP again after each match,
    # and the leader's press was refused until then). A member opts out with UNREADY or by backing out of the party's search.
    pings = {"%s<>%s" % (tag(a), tag(b)): pair_ping(a, b) for i, a in enumerate(players) for b in players[i + 1:]
             if pair_ping(a, b) is not None}
    try:
        PING_FORMED[MM.mid_of(players[0])] = {frozenset((a, b)): _pair_raw(a, b) for i, a in enumerate(players) for b in players[i + 1:] if _pair_raw(a, b) is not None}
        while len(PING_FORMED) > 256:
            PING_FORMED.pop(next(iter(PING_FORMED)))
    except Exception as e:                                   # noqa: BLE001 - a record for the kick learning never breaks the start
        log("PING-FORMED ERR", repr(e))
    evt("match-formed", MM.mid_of(players[0]), "players", [tag(t) for t in players], "host", tag(players[0]),
        "net", "steam-p2p" if MATCH_P2P.get(frozenset(players)) else "server-relay", "ping-ms", pings or "unknown",
        "client-versions", {tag(t): CLIENT_VERSIONS.get(t, "?") for t in players})
    log("load timing: START-MATCH sent", [tag(t) for t in players])
    MATCH_MAP.pop(frozenset(players), None)              # pick this match's map afresh
    VOTES.pop(frozenset(players), None)
    pmid = MM.mid_of(players[0]) if players else None
    if pmid and PRIV.is_private_mid(pmid):
        # private match: the lobby's map, a random pool map, or a vote over the lobby's pool
        idx, opts = priv_pick_map(pmid, players)
        key = frozenset(players)
        with _vote_lock:
            if opts:
                VOTES[key] = {"opts": opts, "votes": {}, "winner": -1}
            MATCH_MAP[key] = idx
        log("MAP (private)", MAPS[idx][2], "vote over %d maps" % len(opts) if opts else "chosen / random", [tag(p) for p in players])
    elif map_vote_on():
        vote_start(players)                              # placeholder map now, the winner at the vote's end
    map_index(players)
    for tok in players:
        LOADED.discard(tok)                              # fresh match: clear any old loaded flag
        conn = SESSIONS.conn(tok)
        if not conn:
            log("START-MATCH", tag(tok), "no session conn (will push on keepalive)"); continue
        try:
            conn.sendall(match_start_frame(tok, players))
            log("START-MATCH", tag(tok), "sent on session conn")
        except Exception as e:
            log("START-MATCH ERR", tag(tok), e)
    if not os.environ.get("T2R_NO_COUNTDOWN"):
        threading.Thread(target=countdown, args=(list(players),), daemon=True).start()

def countdown(players):
    """Thread entry: _countdown_run, but an exception logs and aborts cleanly (the reaper then frees the stalled match)."""
    try:
        _countdown_run(players)
    except Exception as e:
        log("COUNTDOWN-ERR", repr(e), [tag(t) for t in players])

def _countdown_run(players):
    """Drive the client's match page-state (0x189bc0) like the original server:
    op 0x0c [phase][seconds] -> ctx+0x1b4 / ctx+0x1b8.
      phase 1          -> page 1->2 (MATCH FOUND)
      phase 2, n > 0   -> page 3, "STARTING MATCH IN n"
      phase 2, n == 0  -> LEGIT start: client starts level MAPS[ctx+0x18] under
                          "to1-levels", closes the menu, sub-state 2->3.
    Must arrive AFTER the 0x04 record (0x04 resets 0x1b4=-1, 0x1b8=0)."""
    # MATCH FOUND -> "STARTING MATCH IN 5..1" -> [MAP VOTE 160+n] -> CHOOSE LOADOUT 100+n ->
    # [ONE-USE BOOSTER 130+n] -> level start (countdown_steps / MATCH FLOW above). The client's
    # t2r-ext opens its pages while the seconds are >= 100; the last second is a plain "1" again
    # (the ext fades its page out behind the match horn and holds the local start ~5 s), then 0 = start.
    pre = int(os.environ.get("T2R_PRE_COUNTDOWN", "5"))
    secs = int(os.environ.get("T2R_COUNTDOWN", "10"))   # CHOOSE LOADOUT window
    time.sleep(2.0)                              # let 0x04 land (start + keepalive re-push)
    steps = countdown_steps(pre, secs, frozenset(players) in VOTES)
    for t in players: CD_GOT.discard(t)
    in_vote = False
    for phase, n in steps:
        if MM.players_of(players[0]) != players:
            log("COUNTDOWN aborted (match cancelled)", [tag(t) for t in players])
            VOTES.pop(frozenset(players), None)
            return
        resend = False
        if phase == 2 and 160 <= n < 200:
            in_vote = True
        elif in_vote:
            in_vote = False
            vote_close(players)                          # the vote window is over
            resend = True                                # the match record again, now with the winning map
        timing_mark(players, stage_name(phase, n))
        if phase == 2 and n == 0:
            MM.mark_launched(players)
        for tok in players:
            c = SESSIONS.conn(tok)
            if c:
                try:
                    # the re-sent 0x04 (phase reset) and this step's 0x0c (phase 2 again) go in ONE segment
                    head = match_start_frame(tok, players) if resend else b""
                    push(c, head + msg(0x0c, bytes([phase, n])), drop=not head)
                    if phase == 2 and n == 0: CD_GOT.add(tok)
                except OSError: pass
        if resend:
            log("COUNTDOWN", "match record re-sent with the voted map", MAPS[map_index(players)][2], [tag(t) for t in players])
        log("COUNTDOWN", f"phase={phase} n={n}", [tag(t) for t in players])
        time.sleep(1.0)

CD_GOT = set()   # tokens that received the final countdown step (level start) for their match

def late_countdown(tok, conn):
    """A player whose start went out late (no session conn when the match formed) missed
    the shared countdown: run a short one on the conn that just delivered the start."""
    def run():
        time.sleep(1.0)
        for phase, n in [(1, 0), (2, 3), (2, 2), (2, 1), (2, 0)]:
            if tok in LOADED or not MM.in_match(tok): return
            try: push(conn, msg(0x0c, bytes([phase, n])), drop=True)
            except OSError: return
            time.sleep(1.0)
        CD_GOT.add(tok)
        log("COUNTDOWN late", tag(tok))
    if tok not in CD_GOT and tok not in MM.joining:        # (a join-in-progress player has its own countdown: jip_countdown)
        threading.Thread(target=run, daemon=True).start()

MATCH_P2P = {}   # frozenset(players) -> True when gameplay rides Steam P2P

# --------------------------------------------------- host choice by Steam ping estimates
# Each helper reports its Steam ping location (0xF4); the server forwards it to the other
# helpers (0xF5 [u64 steamid][location]) and they answer with their estimated ping to it
# (0xF6 [u64 steamid][i32 ms]). The match host is the player whose worst ping to the others
# is lowest (then lowest total), so nobody sits at 300 ms while the host has 0.
PING_LOC = {}    # token -> ping location string (the last ACCEPTED one: forwarded to the others, decodes the region)
PING_AT = {}     # (from token, to token) -> time of the last report (REPICK-CHECK logs the age: are in-match RTTs fresh?)
LOC_FWD = {}     # token -> time its location was last forwarded (an unchanged location is re-forwarded every 60 s: in-match RTT refresh)
PING = {}        # (from token, to token) -> deque of the last ms reports (pair_ping = median)
# (a player on a VPN): his location jumped NA -> EU -> NA-west with every data centre at ~1500 ms; each spiked sample was
# taken at face value (region flip for the host pick, the spiked location forwarded to every helper, one report = the pair ping).
# Now: a location whose best data centre is far above the player's usual (> 2x and > +80 ms) is a spike and ignored; 3 spikes in a
# row (~90 s) = the connection really changed (VPN switch) and it is accepted. The region changes only after 2 samples agree; a
# region change clears that player's pair pings (stale). Pair pings are the median of the last 5 reports per direction.
LOC_HIST = {}    # token -> deque of best-ms of accepted locations
LOC_SPIKES = {}  # token -> consecutive spiked samples
REGION = {}      # token -> stable region
REGION_CAND = {} # token -> (region, count) waiting to confirm a change

def mm_region(token):
    """Matchmaking region of a token: the stable region, else the decode of its last ping location; None = unknown."""
    try:
        import hostmig as _hm
        return REGION.get(token) or _hm.region_of(PING_LOC.get(token))
    except Exception:                                          # noqa: BLE001
        return None

LOC_MAX = 900                                              # the helper reads < 1100 B; a real Steam location string is a few hundred
LOC_OK = __import__("re").compile(r"^[A-Za-z0-9=+,:/_. -]+$")

def _loc_best(loc):
    import re as _re
    ms = [int(m) for _p, m in _re.findall(r"([a-z][a-z0-9]{2,4})=(\d+)", (loc or "").lower())]
    return min(ms) if ms else None

# The cap was 4096 directional pairs and a full table dropped its first 1024 keys in INSERTION order. At evening load (90 online,
# 65 helpers = 4160 pairs) the table was always full, so pairs refreshed every few seconds were dropped at random and a known ping turned
# unknown: a joiner refused at 233 ms joined the same match 5 s later and was removed 60 s in (70 refused-then-joined, 38 ping-kicked
# in one day). Now: a bigger cap, offline pairs out first, then the least recently reported; and PING_LAST below.
PING_MAX = int(os.environ.get("T2R_PING_MAX", "16384"))
# The last pair ping per unordered pair {a, b} = (ms, time), written with every report. The gates read it when PING has no entry (evicted,
# or reset); the table cap never touches it. Pruned by age (PING_GATE_AGE_S) in gc_state; _drop_pings clears a player's pairs
# (connection / region change = the old pings say nothing).
PING_LAST = {}
def _drop_pings(token):
    for k in [k for k in PING if token in k]:
        PING.pop(k, None); PING_AT.pop(k, None)
    for k in [k for k in PING_LAST if token in k]:
        PING_LAST.pop(k, None)

def _ping_evict(online, n):
    """Drop n entries of the full PING table: pairs with a player who is not online first, then the least recently reported."""
    order = sorted(list(PING), key=lambda k: (k[0] in online and k[1] in online, PING_AT.get(k, 0.0)))
    for k in order[:n]:
        PING.pop(k, None); PING_AT.pop(k, None)

# Ping mesh scope (with ~100 players the server was at 100 % CPU; 394 pair reports / s in and 191 location forwards / s out were 90 % of the daemon's messages).
# Every location went to EVERY online helper, so every helper estimated (and reported) its ping to every other online player, menu idlers
# included: N^2. Pair pings are read only for players who can share a match: searchers (host pick, ping gate, lobby fits, JIP cap) and the
# players of one running match (repick, net stats). Now a location goes only there: a searcher <-> every searcher and every in-match
# player, an in-match player <-> its matchmates and the searchers. A player who starts searching swaps locations with all of those at once
# (mesh_activate, at most once per MESH_ACTIVATE_S), so its pair pings still come in within a second or two.
# T2R_PING_MESH=0 = the old all-to-all.
PING_MESH = os.environ.get("T2R_PING_MESH", "1") != "0"
MESH_ACTIVATE_S = 60.0
MESH_ON = {}     # token -> time of its last full swap as a searcher

def mesh_peers(token, view=None):
    """Tokens whose helpers need this player's location (and whose locations it needs); None = everyone (T2R_PING_MESH=0)."""
    if not PING_MESH:
        return None
    q, live = view if view is not None else MM.mesh_view()
    if token in q:
        out = q | set(live)
    elif token in live:
        mid = live[token]
        out = {t for t, m in live.items() if m == mid} | q
    else:
        return set()
    out.discard(token)
    return out

def _mesh_send(links, token, peers, loc, both):
    """Send token's location to its peers' helpers (both=True: and each peer's location to token's helper)."""
    me = links.get(token)
    if not me:
        return
    for other, oloc in list(PING_LOC.items()):
        if other == token or (peers is not None and other not in peers):
            continue
        ol = links.get(other)
        if not ol:
            continue
        try:
            push(ol[0], msg(0xF5, me[1].to_bytes(8, "little") + loc.encode("latin1")), drop=True)
            if both:
                push(me[0], msg(0xF5, ol[1].to_bytes(8, "little") + oloc.encode("latin1")), drop=True)
        except Exception as e:
            log("PING-LOC ERR", tag(token), e)

def mesh_activate(token):
    """A player started searching: swap locations with every searcher and in-match player now (not at their next 60 s refresh)."""
    if not PING_MESH or not token:
        return
    now = time.time()
    if now - MESH_ON.get(token, 0) < MESH_ACTIVATE_S:
        return
    loc = PING_LOC.get(token)
    if not loc:
        return
    MESH_ON[token] = now
    try:
        _mesh_send(BRIDGES.links(), token, mesh_peers(token), loc, True)
    except Exception as e:                                 # noqa: BLE001 - never breaks the search
        log("PING-MESH ERR", tag(token), repr(e))

def ping_location(token, loc):
    if not loc:
        return
    if PING_LOC.get(token) == loc:
        # an unchanged location used to be dropped, so the others never re-estimated: pair pings stayed at the pre-match snapshot
        if time.time() - LOC_FWD.get(token, 0) >= 60 and BRIDGES.conn(token):
            LOC_FWD[token] = time.time()
            _mesh_send(BRIDGES.links(), token, mesh_peers(token), loc, False)
        return
    best = _loc_best(loc)
    hist = LOC_HIST.setdefault(token, collections.deque(maxlen=8))
    if best is not None and hist:
        med = sorted(hist)[len(hist) // 2]
        if best > max(2 * med, med + 80):
            n = LOC_SPIKES.get(token, 0) + 1
            LOC_SPIKES[token] = n
            if n < 3:
                log("PING-LOC", tag(token), "spike ignored: best %d ms vs usual %d ms (%d in a row)" % (best, med, n), "loc", loc[:80])
                return
            log("PING-LOC", tag(token), "connection changed (3 spikes in a row): best %d ms, usual was %d ms - accepted, pair pings reset" % (best, med))
            hist.clear()
            _drop_pings(token)
    LOC_SPIKES[token] = 0
    if best is not None:
        hist.append(best)
    PING_LOC[token] = loc
    LOC_FWD[token] = time.time()
    try:
        import hostmig as _hm
        r = _hm.region_of(loc)
        old = REGION.get(token)
        if r and old is None:
            REGION[token] = r
        elif r and r != old:
            cr, cn = REGION_CAND.get(token, (None, 0))
            cn = cn + 1 if cr == r else 1
            REGION_CAND[token] = (r, cn)
            if cn >= 2:
                REGION[token] = r; REGION_CAND.pop(token, None); _drop_pings(token)
                log("PING-LOC", tag(token), "region changed %s -> %s (2 samples agree) - pair pings reset" % (old, r))
        else:
            REGION_CAND.pop(token, None)
        log("PING-LOC", tag(token), "region", REGION.get(token), "(sample %s)" % r, "loc", loc[:120])   # the region decode was written from the format docs - log real strings
    except Exception as e:
        log("PING-LOC", tag(token), "loc", loc[:120], "region decode error", e)
    _mesh_send(BRIDGES.links(), token, mesh_peers(token), loc, True)

def ping_report(token, steamid, ms):
    with BRIDGES.lock:
        other = BRIDGES.by_sid.get(steamid)
        other = other if other in BRIDGES.by_token else None
        online = set(BRIDGES.by_token) if other and len(PING) >= PING_MAX and (token, other) not in PING else None
    if other and ms >= 0:
        if online is not None:     # bounded by a cap: offline pairs out first, then the least recently reported
            _ping_evict(online, max(1, PING_MAX // 8))
        PING.setdefault((token, other), collections.deque(maxlen=5)).append(ms)
        now = time.time()
        PING_AT[(token, other)] = now
        PING_LAST[frozenset((token, other))] = (_pair_raw(token, other), now)

def _ping_med(d):
    if not d:
        return None
    if isinstance(d, (int, float)):          # a single measurement (tests, old callers) = its own median
        return d
    v = sorted(d)
    return v[len(v) // 2]

# ping fixes (41 high-ping removals in one night, re/reports-1123):
#  - PING_MATCH_MS: the formation gate, the host pick and join in progress judge at this lower limit; the removal stays at PING_KICK_MS / PING_KICK_S
#    (players matched at 188 ms jittered to 192 and were removed). T2R_PING_MATCH_MS=190 = the old behaviour, 0 = the kick limit.
#  - PING_WAIT_S: a lobby with an unknown pair ping waits this long (per searcher, from the first pass it was ready with one) for the pings; after
#    that the players whose ping to the chosen host is still unknown stay in the queue. T2R_PING_WAIT_S=0 = off (unknown = not judged).
#  - PING_MEMORY_S: a player removed for ping at M ms to host H remembers (M, H) for this long; the gates read max(estimate, M) for the pair.
#    0 = off. PING_CORR_S: a player removed twice within it with a measured ping 40+ ms over the estimate the match was formed with gets the
#    median excess added to his estimated pair pings for this long. 0 = off. In memory only.
#  - the search note: a searcher the ping gate left out for PING_NOTE_AFTER_S gets one "no players within N ms" line per PING_NOTE_EVERY_S.
PING_MATCH_MS = int(os.environ.get("T2R_PING_MATCH_MS", "175"))
PING_WAIT_S = float(os.environ.get("T2R_PING_WAIT_S", "10"))
PING_MEMORY_S = float(os.environ.get("T2R_PING_MEMORY_S", "21600"))
PING_CORR_S = float(os.environ.get("T2R_PING_CORR_S", "43200"))
PING_CORR_EXCESS_MS = 40
PING_CORR_KICKS = 2
PING_NOTE_AFTER_S = 60.0
PING_NOTE_EVERY_S = 120.0
MEASURED_PAIR = {}       # (a, b) -> (measured ms, time): both orders, set when the ping removal took a player off a host
PING_EXCESS = {}         # token -> [(measured - formation estimate, time)] of its removals
PING_CORR = {}           # token -> (ms added to its estimated pair pings, until)
PING_FORMED = {}         # mid -> {frozenset((a, b)): the raw estimate at formation}
PING_WAIT_SINCE = {}     # token -> first time it was ready in a lobby with an unknown pair ping
PING_NOTE_AT = {}        # token -> time of the last "no players within the limit" note

def ping_match_ms():
    """The limit the formation gate, the host pick and join in progress judge at (PING_MATCH_MS, else the kick limit)."""
    return PING_MATCH_MS if 0 < PING_MATCH_MS else PING_KICK_MS

def _pair_raw(a, b):
    v = [x for x in (_ping_med(PING.get((a, b))), _ping_med(PING.get((b, a)))) if x is not None]
    return max(v) if v else None

def _corr_of(t, now):
    c = PING_CORR.get(t)
    if not c:
        return 0
    if c[1] <= now:
        PING_CORR.pop(t, None)
        return 0
    return c[0]

def _measured_of(a, b, now):
    m = MEASURED_PAIR.get((a, b))
    if not m:
        return None
    if now - m[1] > PING_MEMORY_S:
        MEASURED_PAIR.pop((a, b), None); MEASURED_PAIR.pop((b, a), None)
        return None
    return m[0]

def pair_ping(a, b, raw=False, now=None):
    """The ping the gates judge for a pair: the Steam estimate (median of the last reports, worse direction) + the per-player correction of a player
    the estimate under-called twice (PING_CORR), at least the ping measured when the pair was last removed for ping (MEASURED_PAIR). raw=True: the
    estimate alone (the in-match removal timer reads it, as the removal is about the real ping)."""
    est = _pair_raw(a, b)
    if raw or (not MEASURED_PAIR and not PING_CORR):
        return est
    now = time.time() if now is None else now
    if est is not None:
        est += max(_corr_of(a, now), _corr_of(b, now))
    if PING_MEMORY_S > 0:
        m = _measured_of(a, b, now)
        if m is not None and (est is None or m > est):
            est = m
    return est

def ping_kick_learn(mid, p, host, ms, now=None):
    """The ping removal took `p` off `host` at a measured `ms`: remember the pair and, when the match was formed with an estimate 40+ ms under
    that, the player's excess (two within PING_CORR_S = a correction on all his estimated pairs)."""
    now = time.time() if now is None else now
    if PING_MEMORY_S > 0:
        MEASURED_PAIR[(p, host)] = MEASURED_PAIR[(host, p)] = (ms, now)
        if len(MEASURED_PAIR) > 2000:
            for k in [k for k, v in MEASURED_PAIR.items() if now - v[1] > PING_MEMORY_S] or list(MEASURED_PAIR)[:500]:
                MEASURED_PAIR.pop(k, None)
        log("ping-learn:", tag(p), "<>", tag(host), "measured", ms, "ms - the gates judge this pair at least that for", int(PING_MEMORY_S), "s")
    if PING_CORR_S > 0:
        est = (PING_FORMED.get(mid) or {}).get(frozenset((p, host)))
        if est is not None and ms - est > PING_CORR_EXCESS_MS:
            ex = PING_EXCESS.setdefault(p, [])
            ex[:] = [e for e in ex if now - e[1] <= PING_CORR_S] + [(ms - est, now)]
            if len(ex) >= PING_CORR_KICKS:
                v = sorted(e[0] for e in ex)
                med = v[len(v) // 2] if len(v) % 2 else (v[len(v) // 2 - 1] + v[len(v) // 2]) / 2.0
                PING_CORR[p] = (int(med), now + PING_CORR_S)
                log("ping-learn:", tag(p), "estimate under-called %d removals (excess %s ms) - +%d ms on his estimated pings for %d s" % (len(ex), v, int(med), int(PING_CORR_S)))
            else:
                log("ping-learn:", tag(p), "measured %d ms, formation estimate %d ms (excess %d ms) - noted (%d of %d for a correction)" % (ms, est, ms - est, len(ex), PING_CORR_KICKS))
        if len(PING_EXCESS) > 2000:
            for k in [k for k, v in PING_EXCESS.items() if not v or now - v[-1][1] > PING_CORR_S]:
                PING_EXCESS.pop(k, None)

PERF = hostmig.PerfScores(now=time.time)   # token -> latest performance report (op 0xCA)

def perf_op(pl):
    """Op 0xCA -> reply 0xCB, or None for a bad payload."""
    r = hostmig.parse_perf(pl)
    if not r:
        return None
    token, rep = r
    PERF.report(token, rep, BRIDGES.steamid(token) or 0)
    if hostcap.MODE >= 1:
        try:
            hostcap_note(token, hostcap.parse_cap(pl))
        except Exception as e:                       # the capacity history must never break the performance report
            log("HOSTCAP ERR", repr(e))
    if hostcap.MODE >= 1:
        try:
            oneway_note(token, hostcap.parse_oneway(pl))
        except Exception as e:                       # observation only, never breaks the report
            log("ONEWAY ERR", repr(e))
    try:
        loss_note(token, parse_loss(pl))
    except Exception as e:                           # the loss record must never break the performance report
        log("LOSS ERR", repr(e))
    if rep["n250"] or rep["vram"]:
        log("PERF", tag(token), "stalls>=250ms", rep["n250"], ">=1s", rep["n1000"], "worst", rep["worst"], "ms avg frame", rep["avg_ms"], "ms",
            "vram-over" if rep["vram"] else "", "-> host penalty", hostmig.perf_penalty(rep), "ms")
    return msg(hostmig.OP_PERF_REPLY, b"ok=1")

def oneway_note(token, ow):
    """Alarm of an ext that sent for seconds and received nothing (re/oneway-p2p-1019.md). Logged + counted per token; NO action: the engine drops the
    session within ~6 s, a transport switch needs the Steam helper (see the note). `ow` None = no alarm."""
    if not ow:
        return
    mid, active = MM.active_of(token)
    snap = HM.snapshot(mid) if mid else None
    role, slot, players = "no match", "?", []
    if snap:
        t0, players, alive, hslot = snap
        slot = players.index(token) if token in players else "?"
        role = "HOST" if slot == hslot else "client"
    n, hn = HOSTCAP.oneway_note(token, mid, role == "HOST")
    p2p = MATCH_P2P.get(frozenset(players)) if players else None
    log("ONEWAY", tag(token), role, "slot", slot, "match", mid, "net", "steam-p2p" if p2p else ("server-relay" if p2p is not None else "?"),
        "- sent %d KB and got nothing for %d s (%d datagrams in %d s of the match);" % (ow["tx_kb"], ow["quiet_s"], ow["rx_pkts"], ow["in_match_s"]),
        "alarms so far", n, "as host", hn, "others:", [tag(p) for p in players if p != token])
    evt("oneway", mid, tag(token), role, "quiet-s", ow["quiet_s"], "rx", ow["rx_pkts"], "tx-kb", ow["tx_kb"], "n", n)

def perf_penalty(token):
    return PERF.penalty(token, BRIDGES.steamid(token) or 0)

# Host upload capacity (T2R_HOST_CAP 0 off / 1 record + log only / 2 live in the start pick + re-pick (default since)): tools/hostcap.py, re/host-capacity-1019.md
HOSTCAP = hostcap.HostCap(lambda: db(), now=time.time, log=log)

def hostcap_note(token, cap):
    """Optional capacity trailer of a 0xCA report (None = old ext): the host's tx raises its proven capacity, a client's relayed loss can charge the host."""
    if not cap:
        return
    mid, active = MM.active_of(token)
    snap = HM.snapshot(mid) if mid else None
    if not snap:
        return
    t0, players, alive, hslot = snap
    if not (0 <= hslot < len(players)):
        return
    host = players[hslot]
    if token == host:
        HOSTCAP.host_report(token, len(alive), cap["avg_kb"], cap["peak_kb"], mid)
        log("HOSTCAP", tag(token), "hosting", len(alive), "players: tx avg", cap["avg_kb"], "KB/s peak", cap["peak_kb"], "KB/s (need", hostcap.need_kb(len(alive)), ")")
    elif HOSTCAP.client_report(mid, host, token, cap["loss10"]):
        log("HOSTCAP", tag(host), "LAGGED as host: client", tag(token), "msg-7 loss", cap["loss10"] / 10.0, "% twice in a row, host last tx",
            (HOSTCAP.get(host) or {}).get("last_kb"), "KB/s")

# No-host list (a player on a VPN whose ping jumps to ~1500 ms must never host): no_host.txt next to the database (outlives
# the PT folders, re-read when it changes), one Steam id (7656...) or player name per line, '#' = comment. Those players are never
# picked as host (match start, 2-player matches, host migration) while anyone else is in the match.
NO_HOST_PATH = os.path.join(os.path.dirname(os.path.abspath(DB_PATH)), "no_host.txt")
_NO_HOST = {"mtime": None, "ids": set(), "names": set()}

def no_host(token):
    try:
        mt = os.path.getmtime(NO_HOST_PATH)
    except OSError:
        mt = None
    if mt != _NO_HOST["mtime"]:
        ids, names = set(), set()
        if mt is not None:
            try:
                for ln in open(NO_HOST_PATH, encoding="utf-8", errors="ignore"):
                    ln = ln.split("#", 1)[0].strip()
                    if ln.isdigit():
                        ids.add(int(ln))
                    elif ln:
                        names.add(ln.lower())
            except OSError:
                pass
        _NO_HOST.update(mtime=mt, ids=ids, names=names)
        log("NO-HOST", "loaded %d steam id(s), %d name(s) from %s" % (len(ids), len(names), NO_HOST_PATH))
    sid = BRIDGES.steamid(token)
    nm = (get_name(token) or "").lower()
    return bool((sid and sid in _NO_HOST["ids"]) or (nm and nm in _NO_HOST["names"]))

def host_allowed(tokens):
    """tokens without the no-host players (all of them when nobody else is left)."""
    ok = [t for t in tokens if not no_host(t)]
    return ok if ok else list(tokens)

def host_pool(tokens):
    """Host candidates by region: the pinned player's region (T2R_HOST_REGION_PIN) / the majority region (hostmig.region_pool).
    No-host players are left out first."""
    allowed = host_allowed(tokens)
    if len(allowed) < len(tokens):
        log("NO-HOST", "not host candidates:", [tag(t) for t in tokens if t not in allowed])
    everyone = list(tokens)                      # (no-host players still count for the majority region and the worst-case ping)
    tokens = allowed
    return hostmig.region_pool(tokens, lambda t: REGION.get(t) or hostmig.region_of(PING_LOC.get(t)), lambda t: BRIDGES.steamid(t) or 0, pair_ping,
                               everyone=everyone)

def best_host_order(players):
    """players with the best host moved to slot 0 (slot 0 = host); unchanged when any
    candidate's pings are unknown."""
    def first_allowed(pl):          # no ping pick: the first allowed player hosts (no_host.txt)
        a = host_allowed(pl)
        out = list(pl)
        if out and out[0] not in a:
            i = out.index(a[0]); out[0], out[i] = out[i], out[0]
            log("NO-HOST", tag(pl[0]), "may not host -", tag(out[0]), "hosts")
        return out
    def likely_joiners(pl):         # online players in no live match (post-match screens, menus): the usual join-in-progress arrivals
        try:
            busy = set()
            for mid, mp in list(MM.matches.items()):    # (team_order runs under MM.lock: read the dicts, never take the lock)
                if mid in MM.ended:
                    continue
                dep = MM.departed.get(mid, ())
                busy.update(t for t in mp if t not in dep)
            return [t for t in PRES_TOKENS() if t not in pl and t not in busy]
        except Exception as e:
            log("HOST-PICK joiners ERR", repr(e))
            return []
    def cap_first(pl):              # no ping pick: capacity alone (T2R_HOST_CAP=2) - a player that lagged as host never hosts before one that did not
        out = first_allowed(pl)
        if hostcap.MODE < 2 or len(out) < 2:
            return out
        # (match 01660bc0): 2 players = one link, the ping cannot choose between them - but a third player often joins in progress
        # seconds later (Sylvanas queued the second the match formed). Online players in no match whose pings to both are known count as
        # likely joiners: the host is scored on the pings to them too (worst, then total) before capacity decides as below.
        if len(out) == 2 and pair_ping(out[0], out[1]) is not None:
            try:
                a = host_allowed(out)
                extra = [t for t in likely_joiners(out) if all(pair_ping(t, c) is not None for c in out)]
                if extra and len(a) == 2:
                    view = out + extra
                    keyed = [(c, hostmig.host_key([pair_ping(c, o) for o in view if o != c], perf_penalty(c))) for c in a]
                    alt, lines = hostcap.pick(keyed, max(len(view), 3), HOSTCAP.verdict, label=tag)
                    log("HOST-PICK", "2 players + likely joiners", [tag(t) for t in extra], "|", "; ".join(lines), "->", tag(alt))
                    if alt and alt != out[0]:
                        i = out.index(alt); out[0], out[i] = out[i], out[0]
                    return out
            except Exception as e:
                log("HOST-PICK joiners ERR", repr(e))
        try:
            n = max(len(out), 3)                # a 2-player match grows by join in progress: judge for at least 3
            a = host_allowed(out)
            vs = {t: HOSTCAP.verdict(t, n) for t in a}
            best = min(a, key=lambda t: (vs[t][0], out.index(t)))
            if best != out[0] and vs[best][0] < vs.get(out[0], (1,))[0]:
                log("HOST-PICK", "cap:", tag(out[0]), vs[out[0]][1], "->", tag(best), vs[best][1], "hosts (no ping pick)")
                i = out.index(best); out[0], out[i] = out[i], out[0]
        except Exception as e:
            log("HOST-PICK cap ERR", repr(e))
        return out
    if os.environ.get("T2R_NO_HOST_PICK"):
        return first_allowed(players)
    if len(players) < 3:
        return cap_first(players)              # 2 players: both directions are the same link
    best = None
    cands = []
    for h in players:
        if None in [pair_ping(h, o) for o in players if o != h]:
            log("HOST-PICK", "ping unknown, keeping queue order")
            return cap_first(players)
    pool, rinfo = host_pool(players)
    log("HOST-PICK", "region:", rinfo)
    if MM_PING_GATE and PING_KICK_MS > 0:          # (MM_PING_GATE) the ping limit beats the region rule and capacity: never a host that gets a player removed when another would not
        fit = [h for h in host_allowed(players) if max(pair_ping(h, o) for o in players if o != h) <= ping_match_ms()]
        if fit:
            fpool = [h for h in pool if h in fit] or fit
            if fpool != pool:
                log("HOST-PICK", "ping limit %d ms: candidates" % ping_match_ms(), [tag(h) for h in fpool], "instead of", [tag(h) for h in pool])
                pool = fpool
    for h in pool:
        pings = [pair_ping(h, o) for o in players if o != h]
        pen = perf_penalty(h)
        key = hostmig.host_key(pings, pen)
        cands.append((tag(h), max(pings), sum(pings), PERF.get(h, BRIDGES.steamid(h) or 0), pen))
        if best is None or key < best[0]:
            best = (key, h)
    h = best[1]
    if hostcap.MODE >= 1:
        try:
            keyed = [(c, hostmig.host_key([pair_ping(c, o) for o in players if o != c], perf_penalty(c))) for c in pool]
            alt, lines = hostcap.pick(keyed, len(players), HOSTCAP.verdict, label=tag)
            log("HOST-PICK", "cap:", "; ".join(lines), "->", ("keeps %s" % tag(h)) if alt == h else ("would pick %s instead of %s" % (tag(alt), tag(h))),
                "(LIVE)" if hostcap.MODE >= 2 else "(log only)")
            if hostcap.MODE >= 2 and alt and alt != h:
                h = alt
        except Exception as e:
            log("HOST-PICK cap ERR", repr(e))
    if hostcap.MODE >= 1:                              # one-way inbound record (re/oneway-p2p-1019.md): LOG ONLY, the pick ignores it
        try:
            sus = [(tag(c), HOSTCAP.oneway_get(c)) for c in pool]
            sus = [(nm, v) for nm, v in sus if v[0]]
            if sus:
                log("HOST-PICK", "one-way inbound alarms (30 d, alarms/as host):", ", ".join("%s %d/%d" % (nm, v[0], v[1]) for nm, v in sus),
                    "(log only)", "- picked", tag(h))
        except Exception as e:
            log("HOST-PICK oneway ERR", repr(e))
    out = list(players)
    i = out.index(h)
    out[0], out[i] = out[i], out[0]
    for nm, w, tot, rep, pen in cands:
        log("HOST-PICK", "candidate", nm, "worst", w, "ms total", tot, "ms",
            ("stalls>=250ms %d >=1s %d worst %d ms" % (rep["n250"], rep["n1000"], rep["worst"])) if rep else "stalls unknown",
            "penalty", pen, "ms ->", w + pen)
    log("HOST-PICK", tag(h), "worst", best[0][0], "ms (incl. penalty) total", best[0][1], "ms")
    return out

def push_peers(players):
    """Decide Steam P2P vs server relay for this match and tell the helpers.
    P2P only when EVERY player has a Steam helper with a gameplay port (T2R_P2P=0 turns
    it off). Each helper gets 0xF2 [u32 sid][u8 own slot][u8 n][n x (u8 slot, u64 id)];
    the game's 0x04 record then points its relay socket at 127.0.0.1:<helper port>."""
    key = frozenset(players)
    ok = os.environ.get("T2R_P2P", "1") != "0" and all(
        BRIDGES.steamid(t) and BRIDGES.port(t) and BRIDGES.conn(t) for t in players)
    if ok and len({BRIDGES.steamid(t) for t in players}) < len(players):
        ok = False                                     # same Steam account twice: can't address
    MATCH_P2P[key] = ok
    log("MATCH-NET", "steam-p2p" if ok else "server-relay", [tag(t) for t in players])
    missing = [tag(t) for t in players if not (BRIDGES.steamid(t) and BRIDGES.port(t) and BRIDGES.conn(t))]
    if missing:
        log("MATCH-NET", "no Steam helper registered for", missing)
    if not ok:
        return
    sid = match_sid(players)
    table = bytes([len(players)]) + b"".join(
        bytes([i]) + BRIDGES.steamid(t).to_bytes(8, "little") for i, t in enumerate(players))
    failed = []
    for i, tok in enumerate(players):
        try:
            BRIDGES.conn(tok).sendall(msg(0xF2, sid.to_bytes(4, "little") + bytes([i]) + table))
        except Exception as e:
            log("PUSH-PEER ERR", tag(tok), e)
            failed.append(tag(tok))
    if failed:
        # pre-mortem: a helper whose link died between the check above and the push never gets its slot table - the match went on as
        # 'Steam P2P', that player's game pointed at a helper that knows nobody and sat on 'host silent' for ever. The start record is built
        # after this (start_match), so the whole match can still use the server relay.
        MATCH_P2P[key] = False
        log("MATCH-NET", "0xF2 slot table not delivered to", failed, "- server relay for this match")

PARTY_GAP_S = float(os.environ.get("T2R_PARTY_GAP", "15"))   # the leader's own auto-cancel + re-queue gap (same 15 s the matchmaker treats as "not a new arrival")

def party_lead_searching(m):
    """Party rule: a READY mate always goes with its leader's search. The leader counts as searching while it is
    queued, in a match, or out of the queue for under PARTY_GAP_S - the client cancels and re-queues its search about every 40 s, and a mate whose
 follow-the-leader FIND MATCH fell into that gap was refused (8a6a4014, its page stayed up as a fake search, the party never queued)."""
    if m in MM.queue or MM.in_match(m):
        return True
    left = MM.left.get(m)
    return bool(left) and time.time() - left[0] < PARTY_GAP_S


# ---------------------------------------------------------------- matchmaking
_JIP_HV = {}          # (host token, players) -> (time, (rank, text)): the join check runs for every searcher x running match on each poll
JIP_HV_S = 10.0
def jip_host_verdict(host, n):
    """HOSTCAP.verdict(host, n), cached JIP_HV_S seconds; (1, error) when the lookup fails (never blocks a join on an error)."""
    now = time.time()
    e = _JIP_HV.get((host, n))
    if e and now - e[0] < JIP_HV_S:
        return e[1]
    try:
        v = HOSTCAP.verdict(host, n)
    except Exception as ex:                    # noqa: BLE001
        v = (1, "lookup failed %r" % (ex,))
    if len(_JIP_HV) > 2000:
        _JIP_HV.clear()
    _JIP_HV[(host, n)] = (now, v)
    return v

class MatchMaker:
    """Queue -> match. A match starts when MATCH_SIZE players are queued, or when at
    least MIN_PLAYERS are queued and nobody new has joined for FILL_WAIT seconds (twice
    that with an odd count, so a third player waits for a fourth). Each new arrival
    restarts the countdown; a client's own auto-cancel + re-queue is not an arrival.
    Only players still online (presence TTL) count, so quitters never form ghosts."""
    QUEUE_TTL = 20.0          # a searching client polls (0x05/0x15) on its matchmaking conn every
                              # ~1.5 s; no poll for this long = it stopped searching (backed out,
                              # crashed, conn died). The old fixed 300 s from queue time dropped
                              # players still searching and kept ones that had left (ghost matches).
    def __init__(self):
        self.lock = threading.Lock()
        self.queue = {}        # token -> time queued (only tokens CURRENTLY queuing)
        self.seen = {}         # token -> last matchmaking poll
        self.matches = {}      # match_id -> [tokens]
        self.formed = {}       # match_id -> time formed (loading grace for leaves)
        self.left = {}         # token -> (time it left the queue, time it had queued)
        self.settle = {}       # token -> time it becomes matchable (QUEUE_SETTLE after (re)queueing)
        self.qtime = {}        # match_id -> {token: time it had queued} (cancel -> back in line)
        self.launched = set()  # match_ids whose countdown reached the level start
        self.departed = {}     # match_id -> tokens that left the running match (host migration keeps it)
        self.ended = set()     # match_ids whose result arrived (scoreboard hold)
        self.ended_at = {}     # match_id -> time the result arrived (the reaper frees a match whose _free timer was lost)
        self.timed_out = set() # match_ids whose load timeout was handled
        self.started = set()   # match_ids whose GAME-START (0x0a) went out
        self.started_t = {}    # match_id -> time of that GAME-START
        self.last_arrival = 0.0
        self._hold_since = 0.0 # try_form: when it first held a formation for players still settling (bounded)
        self.rq = requeue.Requeue(REQUEUE_WAIT_MAX, REQUEUE_TICKET_S, REQUEUE_OFFLINE_S, now=time.time,   # tickets of the players of finished matches
                                  release_s=LOBBY_RELEASE_S if LOBBY_KEEP else 0.0, min_gap=LOBBY_MIN_GAP_S if LOBBY_KEEP else 0.0)
        self._rq_go = []       # (persistent lobby) the released groups of the current try_form pass: [(mid, searching, still on the screens)]
        self._rq_logged = 0.0
        self.book = lobbybook.LobbyBook(MATCH_SIZE, log=log)   # (LOBBY_BOOK) the search lobbies
        # join in progress (T2R_JIP=1, re/join-in-progress.md)
        self.departed_vol = set()  # tokens that left their running match on purpose (clean op 0x02): not pulled back into it
        self.departed_t = {}   # token -> time it left its running match (a slot is handed on only after JIP_REUSE_S)
        self.clock = {}        # match_id -> (seconds left, duration, ended, time of the report): the t2r-ext clock report (op 0xE3)
        self.polled = {}       # match_id -> {token: time of its last 0xE3 poll}
        self.joining = {}      # token -> (match_id, time it was put into the running match) until its GAME-START went out
        self.lives = {}        # match_id -> (team 0 lives, team 1 lives, time): reinforcements from the players' op 0xE3 (late-join bonus)
        self.late_bonus = {}   # token -> (match_id, parts): 2013 late-join bonus owed to a joiner, paid with its clan day
        self.move_team = {}    # admin live team switch: token -> (match_id, team wanted, until) - its next search re-enters that match on that team
        self.late_mult = {}    # token -> (match_id, multiplier): 2013 d68 = min(3, 1 / (1 - progress)) of a joiner; its supplies x this, deficit forgiven
    def gc(self, keep, now=None):
        """Forget the per-token records of players that are gone (not in `keep`, not queued, not in a match): seen / left / settle / departed_* /
        late_* grew by one entry per token ever seen. Returns the number of entries dropped."""
        now = time.time() if now is None else now
        n = 0
        with self.lock:
            live = set(self.queue)
            for pl in self.matches.values():
                live.update(pl)
            live |= set(keep)
            for d in (self.seen, self.settle, self.departed_t, self.late_bonus, self.late_mult, self.joining):
                for t in [t for t in list(d) if t not in live]:
                    del d[t]; n += 1
            for t in [t for t, v in list(self.left.items()) if t not in live or now - v[0] > 600]:
                del self.left[t]; n += 1
            for t in [t for t in list(self.departed_vol) if t not in live]:
                self.departed_vol.discard(t); n += 1
        return n
    def enqueue(self, token):
        try:
            PRIV.on_enqueue(token)                         # a lobby member searching publicly leaves its private lobby
        except Exception as ex:                            # noqa: BLE001
            log("PRIVATE enqueue ERR", repr(ex))
        with self.lock:
            if self._in_match(token):
                return None, None                          # already in a match; don't re-queue
            now = time.time()
            self.seen[token] = now
            if token not in self.queue:
                self.settle[token] = now + QUEUE_SETTLE
                left, since = self.left.get(token, (0, now))
                if now - left > 15:                        # a new arrival, not a re-queue
                    self.last_arrival = now
                    since = now
                self.queue[token] = since
                evt("queue", tag(token), "queued: %d waiting" % len(self.queue))
        rq_mid = self.rq.searching(token) if REQUEUE else None
        if rq_mid:
            requeue_keep_mode(token, rq_mid)
            log("requeue:", tag(token), "searching again after match", rq_mid)
        return self.try_form()
    def try_form(self):
        now = time.time()
        online = {t for t in PRES_TOKENS()}
        with self.lock:
            gone = [t for t in self.queue if now - self.seen.get(t, 0) >= self.QUEUE_TTL or t not in online]
            for t in gone:
                log("QUEUE drop (no matchmaking poll / offline)", tag(t))
                evt("queue-drop", tag(t), ("no poll for %.0f s" % (now - self.seen.get(t, 0))) if t in online else "offline")
                del self.queue[t]
                if book_on():
                    self.book.away(t, now)
            # only players whose client is searching RIGHT NOW: a fresh poll (searching page heartbeat) within QUEUE_FRESH s.
            # After a countdown cancel the others were put back in the queue with 'seen = now' although their UI may have
            # dropped to the menu (A re-queued and was matched with a B that was not searching = empty match).
            ready = sorted((t for t in self.queue if self.settle.get(t, 0) <= now and now - self.seen.get(t, 0) <= QUEUE_FRESH),
                           key=self.queue.get)
            ready = self._rq_filter(ready, now, form=True)           # the players of a finished match wait for each other
            # private matches (tools/private_lobby.py): lobby members never form or join a public match; a launching lobby forms
            # its own match from its members as soon as they all search (or LAUNCH_WAIT_S after START with the ones that do)
            try:
                ready, priv_ready = PRIV.split_ready(ready)
                for lb, have in PRIV.launches(priv_ready):
                    return self._form_private(lb, have)
            except Exception as ex:                                   # noqa: BLE001 - never breaks the public queue
                log("PRIVATE form failed", repr(ex))
            # parties: a party queues as ONE unit - its members are matchable only when every one of them searches right now (the leader's
            # FIND MATCH asks the others to follow: SOCIAL.on_queue); one that is still missing holds the whole party back
            held = {}
            try:
                ready, held = social.held_parties(ready, PARTIES)
            except Exception as ex:                                   # noqa: BLE001 - parties never break the queue
                log("PARTY filter failed", repr(ex))
            if book_on():
                try:
                    return self._book_form(ready, held, now, online)
                except Exception as ex:                               # noqa: BLE001 - a book bug falls back to the old queue for this pass
                    log("LOBBY-BOOK ERR (old queue this pass)", repr(ex))
            return self._form_pick(ready, held, now, online, None)

    # ------------------------------------------------------------ stable lobbies (LOBBY_BOOK, tools/lobbybook.py)
    def _book_single_unit(self, lb):
        """(lock held) The lobby is one player or one party (the only lobbies offered a join in progress)."""
        if lb is None or not lb.members:
            return True
        if len(lb.members) == 1:
            return True
        p = PARTIES.pid(lb.members[0])
        return bool(p) and all(PARTIES.pid(t) == p for t in lb.members)

    def _book_unit(self, token):
        try:
            if PARTIES.pid(token) and len(PARTIES.members(token)) > 1:
                return list(PARTIES.members(token))
        except NameError:
            pass
        return [token]

    def _book_fits(self, members, newcomers):
        """(lock held) May `newcomers` share a lobby with `members`: one game mode, PART I MAPS = ALWAYS only with owners, and the ping gate keeps
        everybody with one host (unknown pings pass here; the start waits for them, PING_WAIT_S)."""
        allp = list(members) + [t for t in newcomers if t not in members]
        if not modeq.forced():
            ms = {m for m in (_search_mode(t) for t in allp) if m != modeq.ANY}
            if len(ms) > 1:
                return False
        if p1_pref_on():
            ld = PARTIES.leader
            if any(p1_always(t, ld(t)) for t in allp) and not all(p1_always(t, ld(t)) or p1_owner(t, ld(t)) for t in allp):
                return False
        if MM_PING_GATE and PING_KICK_MS > 0 and len(allp) >= 2:
            now = time.time()
            _kept, dropped, _h, _i = hostmig.ping_fit(allp, lambda a, b: _ping_gate(a, b, now), ping_match_ms(), units_fn=PARTIES.members,
                                                      host_ok=lambda t: not no_host(t))
            if dropped:
                return False
        return True

    def _book_cap(self, lb, incoming=()):
        """(lock held) Seats of lobby `lb` for `incoming`: MATCH_SIZE minus the seats kept for its post-match members still on the screens and
        for the party mates of its members that are not searching yet."""
        inc = set(incoming)
        res = set()
        if lb.src is not None and REQUEUE and LOBBY_KEEP:
            for t in self.rq.tokens():
                if (t not in inc and t not in self.book.of and t not in self.queue and self.rq.mid_of(t) == lb.src and self.rq.has(t)
                        and not self._in_match(t)):
                    res.add(t)
        for t in lb.members:
            for m in self._book_unit(t):
                if m not in inc and m not in self.book.of and m != t:
                    res.add(m)
        return MATCH_SIZE - len(res)

    def _book_jip_hold(self, token, unit, now):
        """(lock held) A lone player / one party that a running match can take now, or a match that starts soon will (room, its mode, ping),
        stays in its own lobby meanwhile (nobody is merged with it, so nobody sees it go): jip_pass takes it. Never past QUEUE_HOLD_MAX."""
        if not JIP:
            return False
        if QUEUE_HOLD_MAX > 0 and now - min(self.queue.get(u, now) for u in unit if u in self.queue or u == token) >= QUEUE_HOLD_MAX:
            return False
        need = len(unit)
        if self._jip_pick(token, [], JIP_PREFER_LEFT, need):
            return True
        for mid, pl in self.matches.items():
            if mid in self.ended or not pl:
                continue
            # forming (lobby / countdown / loading), or started so recently that the join waits JIP_MIN_AGE
            young = mid not in self.started or (mid in self.started_t and now - self.started_t[mid] < JIP_MIN_AGE + 5)
            if not young or now - self.formed.get(mid, now) >= 180:
                continue
            if not MODEQ.fits(token, mid, PARTIES.leader(token)) or not p1_fits(token, mid, PARTIES.leader(token)):
                continue
            act = [t for t in pl if t not in self.departed.get(mid, ())]
            if len(act) + need > MATCH_SIZE:
                continue
            if MM_PING_GATE and jip_ping_block(token, mid, pl[0], act, set(self.joining), now) is not None:
                continue
            return True
        return False

    def _book_form(self, ready, held, now, online):
        """(lock held) LOBBY_BOOK pass: lobbies follow their matches, members away too long leave, every searcher not in a lobby is placed,
        lobbies that fit are merged; then ONE whole lobby (biggest first) may form a match (_form_pick with lb)."""
        bk = self.book
        bk.sync_matches(lambda m: m in self.matches, lambda m: m in self.started or m in self.ended, list(self.queue), now)
        bk.expire(now)
        try:
            pub = PRIV.split_ready(list(self.queue))[0]
        except Exception:                                            # noqa: BLE001
            pub = list(self.queue)
        tick = set(self.rq.tokens()) if (REQUEUE and LOBBY_KEEP) else set()
        full = len(pub) >= MATCH_SIZE                                 # a full match can form from the searchers: no join-in-progress holds
        for t in sorted(pub, key=lambda x: self.queue.get(x, now)):
            if bk.lobby_of(t):
                bk.back(t)
                continue
            src = None
            if t in tick:
                nm = self.rq.next_of(t)
                if nm and nm in self.matches and nm not in self.ended:
                    continue                                          # its lobby formed its next match meanwhile: it joins that one (jip_pass)
                src = self.rq.mid_of(t)
            unit = self._book_unit(t)
            solo = len(unit) == 1 or all(u in self.queue for u in unit)
            solo = solo and not full and self._book_jip_hold(t, unit, now)
            lid, why = bk.place(t, unit, self._book_fits, self._book_cap, now, src=src, solo=solo, at=self.queue.get(t, now))
            log("LOBBY-BOOK:", tag(t), why, lid, [tag(x) for x in bk.members(lid)], "(held for a join in progress)" if solo else "")
        for lb in bk.open_lobbies():
            if lb.hold and (full or not (self._book_single_unit(lb) and self._book_jip_hold(lb.members[0], self._book_unit(lb.members[0]), now))):
                lb.hold = False
        for a, b in bk.merge_pass(self._book_fits, self._book_cap, now):
            log("LOBBY-BOOK: lobby", b, "merged into", a, [tag(x) for x in bk.members(a)])
        rset = set(ready)
        for lb in bk.launch_order():
            mem = list(lb.members)
            if not mem or lb.away or lb.hold or now < lb.wait_until or any(t not in rset for t in mem):
                continue
            sub = sorted(mem, key=lambda t: self.queue.get(t, now))
            mid, players = self._form_pick(sub, held, now, online, lb)
            if mid:
                bk.launched(lb.lid, mid)
                return mid, players
        return None, None

    def _form_pick(self, ready, held, now, online, lb):
        """(lock held) The match of the matchable `ready` tokens (the whole queue, or with LOBBY_BOOK one lobby `lb` that must go whole)."""
        if True:
            # per-mode queues: one match forms from the players of ONE mode (a party searches in its leader's mode; T2R_MODE forces)
            grp_mode = None
            try:
                ready, grp_mode = MODEQ.group(ready, PARTIES.leader)
            except Exception as ex:                                   # noqa: BLE001 - never breaks the queue
                log("MODEQ group failed", repr(ex))
            # PART I MAPS preference: an ALWAYS player's match forms from Part I owners only (a party searches with its leader's pick)
            grp_p1 = False
            try:
                if p1_pref_on() and ready:
                    ready, grp_p1 = map_packs.group_p1(ready, lambda t: p1_always(t, PARTIES.leader(t)), lambda t: p1_owner(t, PARTIES.leader(t)))
            except Exception as ex:                                   # noqa: BLE001 - never breaks the queue
                log("P1 group failed", repr(ex))
            if not ready:
                self._log_party_hold(held, now)
                return None, None
            # PERSISTENT LOBBY (LOBBY_KEEP): a released group (tools/requeue.py ready_groups) forms first and together, at once, with the
            # queue backfilling its open seats; its members still on the post-match screens keep their seats (lob_keep) and join in progress
            lob, lob_keep, lob_mid = [], 0, None
            if LOBBY_KEEP:
                rset = set(ready)
                for _gm, _srch, _post in self._rq_go:
                    _have = [t for t in ready if t in set(_srch)]
                    if len(_have) >= MIN_PLAYERS and all(t in rset for t in _srch):
                        lob, lob_keep, lob_mid = _have, len(_post), _gm
                        break
            # (PT): first wait for a full 4v4 (MATCH_SIZE); every FILL_WAIT s without a new arrival the wanted size steps
            # down by one player (4v4 -> 3v4 -> 3v3 -> ... -> MIN_PLAYERS). Odd counts play uneven (3v4); the map pool then only offers
            # maps whose max_team fits the bigger team (team_size / max_team_table).
            quiet = now - (lb.last_join if lb is not None else self.last_arrival)    # (LOBBY_BOOK) the lobby's own last arrival
            want = max(MIN_PLAYERS, MATCH_SIZE - int(quiet // FILL_WAIT)) if FILL_WAIT > 0 else MIN_PLAYERS
            if fill_rule() == "fast":
                want, _why = self.fast_want(ready, online, quiet, now)
            elif MAX_WAIT > 0 and ready and now - self.queue.get(ready[0], now) >= MAX_WAIT:
                want = MIN_PLAYERS           # PT55 00:54-00:58: arrivals trickled in, each reset the quiet timer - the first searcher waited 4.4 min
            _want0 = want
            if lob:
                want = MIN_PLAYERS           # (persistent lobby) the lobby goes now: it waited for its players already
            if JIP and self.matches and len(ready) < MATCH_SIZE and not lob and (lb is None or self._book_single_unit(lb)):
                # a running match with room and enough time left beats a new small match (PT55 02:13 formed a 2-player match while a 6-player one
                # ran; users): the players it can take stay out of the new match, jip_pass puts them in
                rs = set(ready)
                keep = []
                for t in ready:
                    need = 1 + sum(1 for m in self._jip_mates(t) if m in rs)
                    if not self._jip_pick(t, [], JIP_PREFER_LEFT, need):
                        keep.append(t)
                ready = keep
                if not ready:
                    return None, None
            # (live: a 7-player match and a separate 2-player one): while a match is forming or running, searchers it cannot
            # take now QUEUE for it (jip_pass puts them in when a slot frees, or the next lobby forms when it ends) instead of starting their own
            # small match; a second match forms only when the searchers alone fill SECOND_MATCH_MIN slots. 0 = old behaviour.
            # (2 searchers, 5 searchers): the hold had no end - 5 searchers waited 6 min (02:31:16-02:37:41) for a full match
            # that could not take them. Searchers held longer than QUEUE_HOLD_MAX s (the oldest one, MIN_PLAYERS present) form their own match.
            _held_long = bool(ready) and len(ready) >= MIN_PLAYERS and QUEUE_HOLD_MAX > 0 and now - min(self.queue.get(t, now) for t in ready) >= QUEUE_HOLD_MAX
            if SECOND_MATCH_MIN > 0 and len(ready) < SECOND_MATCH_MIN and not lob and (lb is None or self._book_single_unit(lb)):
                def _live(mid, pl):
                    if mid in self.ended or all(t in self.departed.get(mid, ()) for t in pl):
                        return False
                    ck = self.clock.get(mid)
                    if mid not in self.started:                       # forming: lobby / countdown / loading
                        # (MM_PING_GATE) searchers that are all over the ping limit to its host (slot 0, best_host_order) can never join it: no hold,
                        # else the ones the gate kept out of it would wait here until it started (up to 3 min) instead of forming their own match
                        if MM_PING_GATE and pl and all(jip_ping_block(t, mid, pl[0], pl, set(self.joining), now) is not None for t in ready):
                            return False
                        return now - self.formed.get(mid, now) < 180
                    # live: 4 searchers held for 2-player 4e2fc74b; at its GAME-START it had no clock report yet, the hold dropped and
                    # the 4 formed their own match the same second. A match that just started (no clock yet, formed < 300 s ago) still holds.
                    if not ck and now - self.formed.get(mid, 0) < 300:
                        pass                                          # (the map-fit check below still applies)
                    elif not (bool(ck) and now - ck[3] <= JIP_CLOCK_TTL and not ck[2]):    # running with a fresh clock (a stale record never blocks)
                        return False
                    # live: 3-4 searchers held for a running School match whose map can never take one more player (max team) -
                    # a deadlock until it ended. A running match only holds the queue when its map fits one more of them.
                    # live: a player searched INTERROGATION, the only running match was another mode - held for it with no way in.
                    # A running match only holds the queue when at least one searcher may join it (its mode, an admin team-move ticket for another match).
                    if mid in self.started and not any(MODEQ.fits(t, mid, PARTIES.leader(t)) and p1_fits(t, mid, PARTIES.leader(t)) and (self.move_team.get(t) or (mid,))[0] == mid for t in ready):
                        return False
                    cm = MATCH_MAP.get(frozenset(pl))
                    act = [t for t in pl if t not in self.departed.get(mid, ())]
                    # a running match whose host every searcher is over the ping limit to never takes them (jip_ping_block): it does not hold them
                    if mid in self.started:
                        hmi = HM.host_info(mid) if HM.enabled else None
                        hs = hmi["host"] if hmi else 0
                        if 0 <= hs < len(pl) and all(jip_ping_block(t, mid, pl[hs], act, set(self.joining), now) is not None for t in ready):
                            return False
                    # (RELAY_MATCH_MAX) a server-relay match at its limit does not hold searchers that all have a Steam helper: jip_pick refuses them
                    if (RELAY_MATCH_MAX > 0 and mid in self.started and MATCH_P2P.get(frozenset(pl)) is False and len(act) >= RELAY_MATCH_MAX
                            and all(has_helper(t) for t in ready)):
                        return False
                    # (a party of 3 waited forever for a 2-player Capitol Hill Shops match: 5 players are not in that map's pool):
                    # the fit is checked for the biggest searching party (it joins together), not for one player
                    return cm is None or cm in map_pool(act + grp)
                grp = ready[:1]
                for t in ready:
                    g = [t] + [m for m in self._jip_mates(t) if m in ready]
                    if len(g) > len(grp):
                        grp = g
                live = [mid for mid, pl in self.matches.items() if _live(mid, pl)]
                # live: QUEUE_HOLD_MAX ran out while 4-player ffc36d28 was still LOADING (room for 8, formed
                # 60 s before); the 3 formed their own 3-player match 24 s before ffc36d28 started, where join in progress would have put them.
                # Past QUEUE_HOLD_MAX the hold stays only for a match that is still forming (lobby / countdown / loading, < 180 s by _live) and has
                # room for the biggest searching party: it starts soon and jip_pass then takes them. A running match no longer holds them then.
                if _held_long:
                    live = [mid for mid in live if mid not in self.started
                            and sum(1 for t in self.matches[mid] if t not in self.departed.get(mid, ())) + len(grp) <= MATCH_SIZE]
                if live:
                    if now - getattr(self, '_qhold_logged', 0.0) >= 20.0:
                        self._qhold_logged = now
                        log("QUEUE-HOLD", [tag(t) for t in ready], "wait for match(es)", live,
                            "(a second match needs %d searchers, have %d)" % (SECOND_MATCH_MIN, len(ready)))
                    return None, None
            # pre-mortem (PT79 07:31:37): a player who backed out of a countdown (or queued 1-3 s ago) is in the queue but still inside
            # its QUEUE_SETTLE window, so the match formed WITHOUT him (3 players, host 304 ms away) and he joined it 1 s after GAME-START as a
            # late joiner: the whole lobby starts a player short and the roster / 0xF2 table is rebuilt in the first seconds. Hold the formation
            # for the few seconds those players need to become matchable (at most QUEUE_SETTLE + 2 s since the first hold, never when the lobby
            # is full anyway).
            settling = [t for t in self.queue if self.settle.get(t, 0) > now and t not in ready] if lb is None else []
            if settling and len(ready) < MATCH_SIZE and len(ready) >= want:
                if not self._hold_since:
                    self._hold_since = now
                if now - self._hold_since <= QUEUE_SETTLE + 2.0:
                    return None, None
            else:
                self._hold_since = 0.0
            # PART I MAPS priority: more searchers than one match holds -> the Part I owners are
            # taken first (queue order kept inside each group), so the maps can be in the vote (every player of the match must own them)
            try:
                if p1_pref_on() and len(ready) > MATCH_SIZE:
                    pri = sorted(ready, key=lambda t: 0 if p1_owner(t, PARTIES.leader(t)) else 1)
                    if pri != ready:
                        log("P1 priority: Part I owners first", sum(1 for t in ready if p1_owner(t, PARTIES.leader(t))), "of", len(ready), "searchers")
                        ready = pri
            except Exception as ex:                                   # noqa: BLE001 - never breaks the queue
                log("P1 priority failed", repr(ex))
            # SAME-REGION priority (after the Part I sort so region comes first and Part I second): see MM_REGION_ON
            try:
                if MM_REGION_ON and len(ready) > MATCH_SIZE:
                    pri, lead = hostmig.region_first(ready, mm_region, members_fn=PARTIES.members,
                                                     waited_fn=lambda t: now - self.queue.get(t, now), max_wait=MM_REGION_WAIT_S)
                    if lead and pri != ready:
                        log("REGION priority: %s searchers first" % lead, sum(1 for t in ready if mm_region(t) == lead), "of", len(ready), "searchers",
                            "(" + ", ".join("%s=%s" % (tag(t), mm_region(t)) for t in ready) + ")")
                        ready = pri
            except Exception as ex:                                   # noqa: BLE001 - never breaks the queue
                log("REGION priority failed", repr(ex))
            if lob:                                                   # (lobby) its members first: the ping gate keeps them when it can
                ready = lob + [t for t in ready if t not in lob]
            # PING GATE (see MM_PING_GATE): only searchers within the ping limit of one common host are matched together; the rest
            # stay in the queue for a lobby they fit (a later pass, a join in progress)
            if MM_PING_GATE and PING_KICK_MS > 0 and len(ready) >= 2:
                try:
                    _lim = ping_match_ms()
                    _pg = lambda a, b: _ping_gate(a, b, now)
                    kept, dropped, gh, ginfo = hostmig.ping_fit(ready, _pg, _lim, units_fn=PARTIES.members, host_ok=lambda t: not no_host(t))
                    ping_left_out_note(dropped, now)
                    # unknown pair pings: wait PING_WAIT_S for them, then leave out the players whose ping to the host is still unknown
                    hold, unk = ping_wait_judge(kept, gh, _pg, now, PARTIES.members, host_ok=lambda t: not no_host(t))
                    if hold and len(kept) >= max(2, want):
                        if now - getattr(self, '_pwait_logged', 0.0) >= 20.0:
                            self._pwait_logged = now
                            log("PING-WAIT", "pair pings unknown among", [tag(t) for t in kept], "- holding the formation up to %d s for them" % PING_WAIT_S)
                        return None, None
                    if unk:
                        if now - getattr(self, '_pwait_out_logged', 0.0) >= 20.0:
                            self._pwait_out_logged = now
                            log("PING-GATE", "ping to the host still unknown after %d s - left in the queue:" % PING_WAIT_S, [tag(t) for t in unk], "| kept", [tag(t) for t in kept if t not in unk])
                        kept, _d2, gh, ginfo = hostmig.ping_fit([t for t in kept if t not in unk], _pg, _lim, units_fn=PARTIES.members, host_ok=lambda t: not no_host(t))
                        dropped = list(dropped) + _d2
                    if dropped:
                        if now - getattr(self, '_pgate_logged', 0.0) >= 20.0:
                            self._pgate_logged = now
                            log("PING-GATE", ginfo, "- not matched with them (over %d ms to that host):" % _lim,
                                ", ".join("%s %s ms" % (tag(t), _ping_gate(gh, t, now)) for t in dropped), "| kept", [tag(t) for t in kept])
                    if dropped or unk:
                        if lb is not None:
                            # (LOBBY_BOOK) the ones the gate leaves out leave the lobby (they were placed with the pings known then): the lobby is
                            # the kept players now
                            for t in [t for t in ready if t not in kept]:
                                self.book.remove(t, "ping gate at the start (over %d ms to every host, or still unknown)" % _lim, now, lobbybook.BACKOUT_BLOCK_S)
                        ready = kept
                except Exception as ex:                               # noqa: BLE001 - never breaks the queue
                    log("PING-GATE failed", repr(ex))
            if lob:                                                   # (lobby) its members first, the queue backfills; seats kept for the slow ones
                lob = [t for t in lob if t in ready]                  # (a member the ping gate left out is not forced back in)
                if len(lob) < MIN_PLAYERS:
                    lob, lob_keep, want = [], 0, _want0
                else:
                    ready = lob + [t for t in ready if t not in lob]
            _size = max(MIN_PLAYERS, MATCH_SIZE - lob_keep) if lob else MATCH_SIZE
            players = social.pick_units(ready, PARTIES, _size) if len(ready) >= want else []     # whole parties only, 2p - 1 players for a party of p
            if lb is not None and players and set(players) != set(ready):
                # (LOBBY_BOOK) the whole lobby or nothing: a party that needs more players (2p - 1) waits with its lobby for them
                if now - getattr(self, '_book_short_logged', 0.0) >= 20.0:
                    self._book_short_logged = now
                    log("LOBBY-BOOK: lobby", lb.lid, "waits: not every member fits one match yet (parties need 2p - 1 players)", [tag(t) for t in ready])
                return None, None
            if lob and players and not all(t in players for t in lob):
                log("LOBBY:", lob_mid, "could not keep every member together (parties / size) - formed as picked:", [tag(t) for t in players])
            # live (party of 4 + two solos): the solos formed a 1v1 and the ready party (needs 2p - 1 = 7 players) was left behind with no
            # word why. A ready party that is short of players holds the solos back for PARTY_HOLD_S s (from its oldest member's queue time) so
            # that the arrivals can fill it; it and its members get a note saying how many players are missing
            short = {}
            try:
                short = social.party_shortfall(ready, PARTIES, MATCH_SIZE, players) if len(ready) >= 2 else {}
            except Exception as ex:                                   # noqa: BLE001
                log("PARTY shortfall failed", repr(ex))
            if short and now - getattr(self, '_pneed_noted', 0.0) >= 30.0:
                self._pneed_noted = now
                for _pid, (_u, _miss) in short.items():
                    for _m in PARTIES.members(_u[0]):
                        try: SOCIAL.note(_m, "pneed", str(_miss))
                        except Exception: pass                       # noqa: BLE001 - a note never breaks the queue
            if short and players and PARTY_HOLD_S > 0 and not lob:
                age = max(now - min(self.queue.get(t, now) for u, _ in short.values() for t in u), 0.0)
                if age < PARTY_HOLD_S:
                    if now - getattr(self, '_phold_logged', 0.0) >= 20.0:
                        self._phold_logged = now
                        log("PARTY-HOLD", [tag(t) for t in players], "wait for party", {pid: ([tag(t) for t in u], "needs %d more" % m) for pid, (u, m) in short.items()},
                            "(solos held %.0f of %.0f s)" % (age, PARTY_HOLD_S))
                    return None, None
            if players and len(players) >= want:
                if len(players) > 2 and len(players) % 2 and os.environ.get("T2R_EVEN_ONLY"):   # (old rule: even teams only)
                    players = players[:-1]
                players = team_order(players)
                mid = secrets.token_hex(4)
                self.qtime[mid] = {t: self.queue[t] for t in players}
                for t in players: del self.queue[t]
                try:
                    _gs = self.rq.formed(mid, players) if REQUEUE else set()   # (lobby) the slow members of these groups join this match
                except Exception as ex:                               # noqa: BLE001
                    _gs = set(); log("LOBBY: formed ERR", repr(ex))
                for t in players: self.rq.drop(t)
                self.matches[mid] = players
                self.formed[mid] = time.time()
                self._hold_since = 0.0
                if lob or _gs:
                    log("LOBBY: next match", mid, "of the lobby of match(es)", sorted(_gs), "members", [tag(t) for t in lob],
                        "| backfill", [tag(t) for t in players if t not in lob], "| seats kept for", lob_keep, "still on the post-match screens")
                MODEQ.formed(mid, grp_mode)
                if p1_pref_on():
                    MATCH_P1[mid] = grp_p1
                    while len(MATCH_P1) > 256:
                        MATCH_P1.pop(next(iter(MATCH_P1)))
                log("MATCH formed", mid, [tag(p) for p in players], grp_mode or "", "(PART I MAPS: ALWAYS)" if grp_p1 else "")
                return mid, players
            # ('cant load into a match'): a lone searcher sat 5.5 min in the queue and the log said nothing about WHY no match formed.
            # Every 20 s while somebody waits: who is ready, the size wanted now and when it steps down, who is still settling / not polling.
            if now - getattr(self, '_wait_logged', 0.0) >= 20.0:
                self._wait_logged = now
                _stp = FAST_STEP if fill_rule() == "fast" and FAST_STEP > 0 else FILL_WAIT
                nxt = (_stp - quiet % _stp) if _stp > 0 and want > MIN_PLAYERS else 0.0
                log("QUEUE-WAIT", "rule", fill_rule(), "ready", [tag(t) for t in ready], "want", want, "(min %d, size %d)" % (MIN_PLAYERS, MATCH_SIZE),
                    "quiet %.0f s" % quiet, ("next step-down in %.0f s" % nxt) if nxt else "at the minimum",
                    "| queued not ready:", [tag(t) for t in self.queue if t not in ready],
                    *(["| party waits:"] + [("party of %d %s needs %d players, %d searching (%d more)" % (len(u), [tag(t) for t in u], social.party_need(len(u)), len(ready), m))
                                         for u, m in short.values()] if short else []))
            self._log_party_hold(held, now)
        return None, None
    def _form_private(self, lb, have):
        """(lock held) Form the private match of lobby `lb` from the searching members `have`. Returns (mid, players)."""
        if lb.rules.get("teams", 0) == 0:
            players = team_order(have)
        else:
            t0, t1 = PRIV.teams_for(lb, have, lambda pl: (pl[0::2], pl[1::2]))
            bo = best_host_order(have) if len(have) >= 2 else list(have)
            big = t0 if len(t0) >= len(t1) else t1
            host = next((h for h in bo if h in big), None)
            players = order_slots(t0, t1, host)
        mid = secrets.token_hex(4)
        self.qtime[mid] = {t: self.queue[t] for t in players}
        for t in players:
            del self.queue[t]
            self.rq.drop(t)
        self.matches[mid] = players
        self.formed[mid] = time.time()
        self._hold_since = 0.0
        mode = lb.rules.get("mode", 0)
        mname = modeq.MODES[mode] if 0 <= mode < modeq.NMODES else modeq.MODES[0]
        MODEQ.formed(mid, mname)
        PRIV.bound(lb, mid, players)
        log("MATCH formed (private)", mid, lb.code, [tag(p) for p in players], mname)
        return mid, players
    def _rq_filter(self, ready, now, form=False):
        """(lock held) `ready` without the searchers whose finished match still has players on its post-match screens (tools/requeue.py).
        form=True (try_form only, never jip_ready): also the persistent lobby holds (LOBBY_KEEP) - lone searchers that fill a waiting lobby, and
        the slow members of a lobby whose next match is in its countdown (they join it in progress, jip_pass) - and self._rq_go is filled."""
        self._rq_go = []
        if not REQUEUE:
            return ready
        try:
            held, waiting, gone = self.rq.held(list(self.queue), lambda t: silence(t, now), self._in_match, now)
            if form and LOBBY_KEEP:
                self._rq_go = self.rq.ready_groups(list(self.queue), lambda t: silence(t, now), self._in_match, now)
        except Exception as ex:                                       # noqa: BLE001 - the requeue never breaks the queue
            log("requeue: ERR", repr(ex))
            return ready
        for t, why in gone:
            log("requeue:", tag(t), "ticket ended:", why)
        if form and LOBBY_KEEP:
            try:
                held = set(held) | self._lobby_holds(ready, held, waiting, now)
            except Exception as ex:                                   # noqa: BLE001 - the lobby never breaks the queue
                log("LOBBY: hold ERR", repr(ex))
        if not held:
            return ready
        if now - self._rq_logged >= 20.0:
            self._rq_logged = now
            log("requeue: HOLD", [tag(t) for t in ready if t in held], "wait for the post-match screens of", {m: [tag(t) for t in w] for m, w in waiting.items()},
                "(at most %.0f s after the match end)" % REQUEUE_WAIT_MAX)
        return [t for t in ready if t not in held]
    def _lobby_holds(self, ready, held, waiting, now):
        """(lock held) Persistent lobby holds for try_form (see LOBBY_KEEP). Returns the extra tokens to hold."""
        extra = set()
        tick = set(self.rq.tokens())
        # the slow members of a lobby that formed its next match: they join it in progress (jip_pass), they never form another match meanwhile
        for t in ready:
            nm = self.rq.next_of(t) if t in tick else None
            if nm and nm in self.matches and nm not in self.ended and nm not in self.started:
                extra.add(t)
        # lone searchers (no ticket) wait for a waiting lobby that has room for all of them: they fill its open seats when it goes
        others = [t for t in ready if t not in held and t not in tick and t not in extra]
        if others and waiting and not book_on():         # (LOBBY_BOOK places them into the lobby itself)
            for gmid, post in waiting.items():
                size = len(post) + sum(1 for t in held if self.rq.mid_of(t) == gmid)
                if size + len(others) <= MATCH_SIZE:
                    extra.update(others)
                    if now - getattr(self, '_lobby_fill_logged', 0.0) >= 20.0:
                        self._lobby_fill_logged = now
                        log("LOBBY: hold", [tag(t) for t in others], "to fill the open seats of the lobby of match", gmid,
                            "(%d members, still on the post-match screens: %s)" % (size, [tag(t) for t in post]))
                    break
        return extra
    def rq_open(self, mid, now=None):
        """(lock held) The match `mid` ended: tickets for the players still in it whose ext searches by itself (RQ_CAP). A party member gets one only
        when its leader is in it too (the leader's search takes the members along). Returns (tickets, players left without one)."""
        now = time.time() if now is None else now
        dep = self.departed.get(mid, ())
        act = [t for t in self.matches.get(mid, ()) if t not in dep and is_token(t) and not t.startswith("f0" * 30)]
        cap = [t for t in act if now - RQ_CAP.get(t, 0.0) <= REQUEUE_CAP_S]
        try:
            cap = [t for t in cap if not PARTIES.pid(t) or PARTIES.leader(t) in cap]
        except NameError:
            pass
        return self.rq.open(mid, cap, now), [t for t in act if t not in cap]
    def _log_party_hold(self, held, now):
        """Every 20 s while a party waits for a member that is not searching (a clear line for the log / the admin)."""
        if held and now - getattr(self, '_party_logged', 0.0) >= 20.0:
            self._party_logged = now
            log("PARTY-WAIT", {pid: [tag(t) for t in miss] for pid, miss in held.items()}, "(every member must be searching)")
    def fast_want(self, ready, online, quiet, now):
        """T2R_FILL_RULE=fast (caller holds the lock). Returns (wanted size, reason). Step-down every FAST_STEP s of quiet; the oldest ready
        searcher waits at most FAST_MAX_WAIT s; with nobody else online who could still arrive the lobby forms FAST_ALONE s after the last arrival."""
        step = FAST_STEP if FAST_STEP > 0 else FILL_WAIT
        want = max(MIN_PLAYERS, MATCH_SIZE - int(quiet // step)) if step > 0 else MIN_PLAYERS
        why = "step"
        inm = {t for mid, pl in self.matches.items() if mid not in self.ended for t in pl if t not in self.departed.get(mid, ())}
        cand = len(online - set(self.queue) - inm)            # online, not searching, not in a running match: may still queue
        if ready and FAST_MAX_WAIT > 0 and now - self.queue.get(ready[0], now) >= FAST_MAX_WAIT:
            want, why = MIN_PLAYERS, "max-wait"
        elif cand <= 0 and quiet >= FAST_ALONE:
            want, why = MIN_PLAYERS, "nobody-else-online"
        return want, why
    def mesh_view(self):
        """(searching tokens, {token: mid} of players in a live match), one lock pass: the ping mesh scope (mesh_peers)."""
        with self.lock:
            q = set(self.queue) | set(self.joining)
            live = {}
            for mid, pl in self.matches.items():
                if mid in self.ended:
                    continue
                dep = self.departed.get(mid, ())
                for t in pl:
                    if t not in dep:
                        live[t] = mid
            return q, live
    def _in_match(self, token):
        return any(token in pl and token not in self.departed.get(mid, ()) and mid not in self.ended
                   for mid, pl in self.matches.items())
    def end(self, mid):
        """The match's result is in (scoreboard showing): its players may queue again at once,
        the record stays until _free (a re-queue inside the scoreboard hold used to get the
        finished match's start re-sent = booted to the menu)."""
        tk = None
        with self.lock:
            if mid in self.matches:
                first = mid not in self.ended
                self.ended.add(mid)
                PRIV.match_over(mid)                       # the owner may start the next private match
                self.ended_at.setdefault(mid, time.time())
                if first and REQUEUE:
                    try:
                        tk = self.rq_open(mid)
                    except Exception as ex:                           # noqa: BLE001 - the requeue never breaks a match end
                        log("requeue: ERR at the match end", repr(ex))
        if tk:
            log("requeue: match", mid, "over - tickets (search again after the post-match screens, the lobby waits for them):", [tag(t) for t in tk[0]],
                "| no ticket (old ext / party member without its leader):", [tag(t) for t in tk[1]])
    def _forget(self, mid):
        """Remove every record of a match (caller holds the lock)."""
        PRIV.match_over(mid)                               # its private lobby is back in the lobby state
        self.matches.pop(mid, None)
        self.qtime.pop(mid, None); self.formed.pop(mid, None); self.launched.discard(mid)
        self.departed.pop(mid, None); self.ended.discard(mid); self.ended_at.pop(mid, None)
        self.timed_out.discard(mid); self.started.discard(mid); self.started_t.pop(mid, None)
        self.clock.pop(mid, None); self.polled.pop(mid, None); self.lives.pop(mid, None)
        for t in [t for t, (m, _) in self.joining.items() if m == mid]: del self.joining[t]
    def snapshot(self):
        """[(mid, players, formed, launched, ended, ended_at, departed set)] copies for the reaper."""
        with self.lock:
            return [(mid, list(pl), self.formed.get(mid, 0.0), mid in self.launched, mid in self.ended,
                     self.ended_at.get(mid, 0.0), set(self.departed.get(mid, ()))) for mid, pl in self.matches.items()]
    def abort(self, mid, only=None):
        """A match that never got going (countdown stalled / host lost while loading): everyone (or only the tokens in `only`) back into the
        queue at their old place. Returns the players put back or None."""
        with self.lock:
            players = self.matches.get(mid)
            if players is None:
                return None
            qt = self.qtime.get(mid, {})
            now = time.time()
            self._forget(mid)
            PRIV.aborted(mid)                              # a private lobby launches again with its players
            if only is not None:
                players = [t for t in players if t in only]
            for t in players:
                self.queue[t] = qt.get(t, now)
                self.seen[t] = now - QUEUE_FRESH - 0.001   # not fresh until it polls again
                self.settle[t] = now + QUEUE_SETTLE
            return list(players)
    def in_match(self, token):
        with self.lock:
            return self._in_match(token)
    def players_of(self, token):
        with self.lock:
            mid = self._mid_locked(token)          # newest live match first (a live match beats a finished one)
            return list(self.matches[mid]) if mid is not None else None
    def drop(self, mid):
        """Remove one finished match record (not any newer match its players queued into)."""
        with self.lock:
            if mid in self.matches:
                self._forget(mid)
    def loading(self, token, grace=5.0):
        """True while the token's match formed < grace s ago and has not started (not all
        players LOADED). Clients auto-cancel their search after ~40 s and re-queue; a cancel
        that lands while the match start is on its way must not tear the match down. Kept
        short: a player backing out after that really leaves (the match is cancelled)."""
        with self.lock:
            mid = self._mid_locked(token)
            if mid is not None:
                pl = self.matches[mid]
                return (time.time() - self.formed.get(mid, 0) < grace
                        and not all(p in LOADED for p in pl))
        return False

    def countdown_over(self, token):
        """True once the shared countdown of the token's match has run (its whole length: pre-countdown, vote,
        loadout / booster stages - the old fixed 'pre + T2R_COUNTDOWN + 4' re-ran a short countdown on a
        re-queued client while the others were still voting)."""
        with self.lock:
            mid = self._mid_locked(token)
            if mid is not None:
                return time.time() - self.formed.get(mid, 0) > countdown_seconds() + 2.0
        return False
    def match_age(self, token):
        """Seconds since the token's match formed (0 if not in a match)."""
        with self.lock:
            mid = self._mid_locked(token)
            if mid is not None:
                return time.time() - self.formed.get(mid, time.time())
        return 0.0
    def queued(self):
        with self.lock:
            return sorted(self.queue, key=self.queue.get)[:MATCH_SIZE]
    def players_of_mid(self, mid):
        with self.lock:
            pl = self.matches.get(mid)
            return list(pl) if pl else None
    def active_of(self, token):
        """(mid, players still in the match) of the token's live match, else (None, [])."""
        with self.lock:
            for mid, pl in self.matches.items():
                if token in pl and token not in self.departed.get(mid, ()) and mid not in self.ended:
                    return mid, [p for p in pl if p not in self.departed.get(mid, ())]
        return None, []
    def _mid_locked(self, token):
        """The token's CURRENT match (lock held): newest first, never one it departed (timed out / crashed out while the others play on),
 a live one before a finished one. PT 48: the old lookups took the first match listing the token - a player timed out of
        a running match and matched again was resolved to the OLD match (its countdown long over: late start, 'loaded' into the old
        match's world with its old players while the new opponent waited on another map)."""
        for skip_ended in (True, False):
            for mid in reversed(list(self.matches)):
                if token in self.matches[mid] and token not in self.departed.get(mid, ()) and not (skip_ended and mid in self.ended):
                    return mid
        return None
    def mid_of(self, token):
        with self.lock:
            return self._mid_locked(token)
    def all_loaded(self, token):
        with self.lock:
            mid = self._mid_locked(token)
            pl = self.matches[mid] if mid is not None else None
        return bool(pl) and all(p in LOADED for p in pl)
    def mark_launched(self, players):
        with self.lock:
            for mid, pl in self.matches.items():
                if pl == players:
                    self.launched.add(mid)
    def cancel(self, token):
        """Back-out before the level start: the match is called off and everyone else goes
        back into the queue at their old place (the leaver keeps theirs for a quick
        re-queue). Returns the others, or None when the token's match already launched."""
        with self.lock:
            mid = self._mid_locked(token)
            if mid is None or mid in self.launched:
                return None
            players = self.matches.pop(mid)
            qt = self.qtime.pop(mid, {})
            self.formed.pop(mid, None)
            now = time.time()
            others = [t for t in players if t != token]
            for t in others:
                self.queue[t] = qt.get(t, now)
                self.seen[t] = now - QUEUE_FRESH - 0.001  # NOT fresh: matchable only after its next searching-page poll proves it still searches
                self.settle[t] = now + QUEUE_SETTLE      # their page goes back to searching first
            self.left[token] = (now, qt.get(token, now))
            self.last_arrival = now                    # give the leaver time to come back
            if book_on():
                self.book.cancelled(mid, token, now)   # the others keep their lobby; the leaver is out of it
            return others
    def poll(self, token):
        with self.lock:
            self.seen[token] = time.time()
    def dequeue(self, token):
        """The client left the searching page (op 0x02 outside a match): out of the queue now. True when it was queued."""
        with self.lock:
            since = self.queue.pop(token, None)
            if since is not None:
                self.left[token] = (time.time(), since)
                self.settle.pop(token, None)
                if book_on():
                    self.book.away(token)              # its seat waits lobbybook.GRACE_S for a search again (page reset / reconnect)
            if self.rq.drop_search(token):              # (not the 0x02 session close right after MATCH-END - that keeps the 'post' ticket)
                log("requeue:", tag(token), "ticket dropped (left the search)")
            return since is not None
    def leave(self, token):
        with self.lock:
            self.rq.drop(token)
            if book_on():
                self.book.remove(token, "left")
            since = self.queue.pop(token, None)
            if since is not None:
                self.left[token] = (time.time(), since)
            for mid in [m for m, pl in self.matches.items() if token in pl]:
                self._forget(mid)
    def depart(self, token, voluntary=False):
        """token left its running match but the others play on (host migration): it is free to
        queue again, the match record stays for the rest (slots = list index, results at the
        end). The last one out removes the match. Returns the match id or None."""
        with self.lock:
            mid = next((m for m, pl in self.matches.items()
                        if token in pl and token not in self.departed.get(m, ())), None)
            if mid is None:
                return None
            gone = self.departed.setdefault(mid, set())
            gone.add(token)
            self.departed_t[token] = time.time()
            (self.departed_vol.add if voluntary else self.departed_vol.discard)(token)
            self.joining.pop(token, None)
            if all(t in gone for t in self.matches[mid]):
                self._forget(mid)
            return mid

    # ------------------------------------------------------------ join in progress
    def jip_clock(self, token, rem, dur, ended):
        """The t2r-ext of a player in a running match reports the match clock (op 0xE3). The newest report of any player of the
        match counts (the clock is replicated to every client once a second). Returns the match id or None."""
        with self.lock:
            mid = self._mid_locked(token)
            if mid is None:
                return None
            now = time.time()
            self.clock[mid] = (rem, dur, bool(ended), now)
            self.polled.setdefault(mid, {})[token] = now
            return mid
    def jip_ready(self):
        """Searching tokens that are matchable right now (same filter as try_form), oldest first."""
        now = time.time()
        with self.lock:
            ready = sorted((t for t in self.queue if self.settle.get(t, 0) <= now and now - self.seen.get(t, 0) <= QUEUE_FRESH),
                           key=self.queue.get)
            ready = self._rq_filter(ready, now)                      # (a held searcher is not put into a running match either)
            if book_on():                                           # a lobby of more than one unit stays together: never pulled apart by a join
                ready = [t for t in ready if not self.book.lobby_of(t) or self._book_single_unit(self.book.get(self.book.lobby_of(t)))]
        try:
            return social.held_parties(ready, PARTIES)[0]            # a party joins a running match together or not at all
        except Exception:                                            # noqa: BLE001
            return ready
    def jip_acked(self, token):
        """True once every other active player of the joiner's match polled (op 0xE3) after the joiner was put in, i.e. their roster
        now lists the joiner (or JIP_ACK_MAX passed). The joiner's GAME-START (its Hello to the host) waits for this: a host that
        takes the Hello before it knows the joiner's team gives the new player an unknown team."""
        with self.lock:
            j = self.joining.get(token)
            if not j:
                return True
            mid, t0 = j
            if time.time() - t0 > JIP_ACK_MAX:
                return True
            pl = self.matches.get(mid, [])
            dep = self.departed.get(mid, ())
            seen = self.polled.get(mid, {})
            others = [p for p in pl if p != token and p not in dep and p not in self.joining]     # (another joiner in flight cannot poll yet: PT55 02:42 waited 5 s for it)
            return all(seen.get(p, 0) >= t0 or (seen.get(p, 0) and t0 - seen[p] > JIP_ACK_STALE) for p in others)   # (a player that stopped polling long ago never acks)
    def jip_done(self, token):
        """The joiner's GAME-START went out: it is a normal player of the match from now on. Returns (mid, t0) or None."""
        with self.lock:
            return self.joining.pop(token, None)
    def jip_age(self, token):
        """Seconds since `token` was put into a running match (0 when it is not joining one)."""
        with self.lock:
            j = self.joining.get(token)
            return time.time() - j[1] if j else 0.0
    def jip_joiners(self, mid):
        with self.lock:
            return [t for t, (m, _) in self.joining.items() if m == mid]
    def _jip_mates(self, token):
        """The token's party mates (never itself). PARTIES is defined below the class; a module without it has no parties."""
        try:
            return [m for m in PARTIES.members(token) if m != token] if PARTIES.pid(token) else []
        except NameError:
            return []
    def _jip_pick(self, token, why_not, min_left=None, need=1, only_mid=None, team=None):
        """The match / slot `token` would be put into, without changing anything (caller holds the lock): (score, mid, slot) or None.
        `need` = how many players of the token's party join together (room for all of them on one team is required for the first one);
        `only_mid` restricts the choice to one match (the next party member follows the first), `team` = the team (slot parity) the first member
 got: the follower takes a slot of that team whatever the balance (Truth followed alph onto the other team). See jip_join for the rules."""
        now = time.time()
        min_left = JIP_MIN_LEFT if min_left is None else min_left
        mates = self._jip_mates(token)
        best = None
        for mid, pl in self.matches.items():
            if only_mid is not None and mid != only_mid:
                continue
            if mid not in self.started or mid in self.ended or mid not in self.launched:
                continue
            if token not in pl and not MODEQ.fits(token, mid, PARTIES.leader(token)):
                why_not.append("%s: another game mode" % mid); continue
            if token not in pl and not p1_fits(token, mid, PARTIES.leader(token)):
                why_not.append("%s: other PART I MAPS preference" % mid); continue
            if token not in pl and not PRIV.jip_ok(token, mid):
                why_not.append("%s: private match / private lobby" % mid); continue
            dep = self.departed.get(mid, ())
            active = [t for t in pl if t not in dep]
            age = now - self.started_t.get(mid, 0)
            if token not in pl and JIP_MIN_AGE > 0 and mid in self.started_t and age < JIP_MIN_AGE:
                why_not.append("%s: only %.0f s past GAME-START (< %.0f s)" % (mid, age, JIP_MIN_AGE)); continue
            # it crashed / left this match and searches again: back into its own slot. PT54: a freeze that dropped the game
            # to the menu sent a CLEAN leave (0x02) and the player was never let back in ("left on purpose" rule) - whoever queues
            # again while their match runs goes back to it
            mv = self.move_team.get(token)
            if mv and (mv[2] < now or mv[0] not in self.matches or mv[0] in self.ended):
                # the move's match is gone (ended / removed): the ticket only meant "back into that match", it must not keep the player
                # out of every other match for its 15 minutes (two players held in QUEUE-HOLD for a running match they could never join)
                self.move_team.pop(token, None); mv = None
            if mv and mv[0] != mid:
                continue                                       # moved by the admin: only back into that match
            back = token in pl and not mv
            if back and team is not None and team_in(mid, pl.index(token), token) != team:
                back = False                                   # a party follower goes to the team of the member that joined first
            hmi = HM.host_info(mid) if HM.enabled else None
            hslot = hmi["host"] if hmi else 0
            if hmi and (hmi["pending"] or hmi["given_up"]):
                why_not.append("%s: host migration in progress" % mid); continue
            if hmi and hmi.get("alone"):
                # fd7cb037: the host moved to the only player left, whose ext ended the match alone ('Everyone else left the match');
                # a joiner (coming back to its slot) loaded into the ended match and sat on its scoreboard
                why_not.append("%s: host moved to the last player left (the match ended on its side)" % mid); continue
            # after a host migration the host is another slot: the joiner's Hello goes there (its ext learns the host from the
            # server's 0xE7 reply on its first poll, the same state the survivors got). The crashed old host comes back as a
            # normal client in a FREE slot - slot 0 is the host role in the match record, never a client slot
            rejoin_host = bool(back and hslot != 0 and pl.index(token) == 0)
            if back and pl[hslot] == token:
                why_not.append("%s: was its host" % mid); continue
            if rejoin_host:
                back = False
            if mid in INT.matches and os.environ.get("T2R_JIP_INT", "1") != "1":       # Interrogation matches are joinable (T2R_JIP_INT=0 = not)
                why_not.append("%s: Interrogation match" % mid); continue
            if not back and len(active) <= 1 and now - self.formed.get(mid, 0) < countdown_seconds() + JIP_MIN_AGE_REMNANT:
                why_not.append("%s: only %d player left and the match is young (a load-timeout remnant)" % (mid, len(active))); continue
            if pl[hslot] in dep:
                why_not.append("%s: host gone" % mid); continue
            if not back or (token, mid) in PING_KICKED:
                pms = jip_ping_block(token, mid, pl[hslot], active, set(self.joining), now)
                if pms is not None:
                    why_not.append("%s: %d ms to the host, over the %d ms ping limit (it would be removed)" % (mid, pms, ping_match_ms() if MM_PING_GATE else PING_KICK_MS)); continue
            if need > 1 and only_mid is None:
                # A party joins a running match only when every searching member passes its ping limit; otherwise it waits for another match.
                # (Checking only the leader sent the leader in alone when a mate was too far from the host, and split the party across two matches.)
                lim = ping_match_ms() if MM_PING_GATE else PING_KICK_MS
                bad = [(m, b) for m, b in ((m, jip_ping_block(m, mid, pl[hslot], active, set(self.joining), now))
                                           for m in mates if m in self.queue and m not in pl) if b is not None]
                if bad:
                    why_not.append("%s: party mate %s %d ms to the host, over the %d ms ping limit (the party stays together)" % (mid, tag(bad[0][0]), bad[0][1], lim)); continue
            ck = self.clock.get(mid)
            if not ck or now - ck[3] > JIP_CLOCK_TTL:
                # (say how stale - PT79 had 'no fresh clock report' for minutes in running 6-player matches; the digits are ignored by the log dedupe)
                why_not.append("%s: no fresh clock report (%s; the players' ext stopped reporting or is an old build)" % (
                    mid, "never reported" if not ck else "newest %.0f s old" % (now - ck[3]))); continue
            left = ck[0] - (now - ck[3])
            if ck[2] or ck[1] <= 0 or left < min_left:
                why_not.append("%s: %.0f s left / ended=%s" % (mid, left, ck[2])); continue
            # ("prevent people from joining in late when its overtime"): overtime / sudden death = a team is out of lives (its last players fight
            # with no respawns). The held overtime clock (OT_FLOOR 30 s < JIP_MIN_LEFT) kept joiners out only while that clock report was fresh; the lives
            # report decides on its own now. Not in Interrogation (no lives there). A player back into its own slot is held too (a free respawn otherwise).
            lv = self.lives.get(mid)
            if mid not in INT.matches and lv and now - lv[2] <= 3 * JIP_CLOCK_TTL and min(lv[0], lv[1]) <= 0:
                why_not.append("%s: overtime (team lives %d / %d)" % (mid, lv[0], lv[1])); continue
            if not back and LOBBY_KEEP and REQUEUE:
                # (persistent lobby) seats kept for the lobby members still on their post-match screens: only they take them
                _kept = [k for k in self.rq.kept(mid) if k != token and k not in pl]
                if _kept and len(active) + need + len(_kept) > MATCH_SIZE:
                    why_not.append("%s: %d seat(s) kept for its lobby members still on the post-match screens" % (mid, len(_kept))); continue
            if len(active) + (1 if back else need) > MATCH_SIZE:         # (fuzz seed 2: a leaver coming back counts as one more player - its slot is departed, so it is not in `active`; the match was full already: 7 v MATCH_SIZE 6)
                continue                                           # full (for this many players)
            if not back and JIP_HOST_CAP and hostcap.MODE >= 2:
                # A host that lagged before with fewer players got filled to 8 by joins in progress: pings of several seconds for everyone, no re-pick
                # target. A join is refused when the host's record says it lagged below what one more player needs (verdict rank 2).
                rk, txt = jip_host_verdict(pl[hslot], len(active) + need)
                if rk >= 2:
                    why_not.append("%s: host %s cannot carry %d players (%s)" % (mid, tag(pl[hslot]), len(active) + need, txt)); continue
            key = frozenset(pl)
            if (RELAY_MATCH_MAX > 0 and not back and MATCH_P2P.get(key) is False and has_helper(token)
                    and len(active) + need > RELAY_MATCH_MAX):
                why_not.append("%s: server-relay match with %d players (limit %d for players with a Steam helper: they play Steam P2P)" % (
                    mid, len(active), RELAY_MATCH_MAX)); continue
            p2p = bool(MATCH_P2P.get(key))
            if p2p and not (BRIDGES.steamid(token) and BRIDGES.port(token) and BRIDGES.conn(token)
                            and BRIDGES.steamid(token) not in {BRIDGES.steamid(t) for t in active}):
                why_not.append("%s: Steam P2P match, joiner has no usable helper" % mid); continue
            cur_map = MATCH_MAP.get(key)
            if cur_map is None:
                continue
            cnt = [0, 0]
            for i, t in enumerate(pl):
                if t not in dep: cnt[team_in(mid, i, t)] += 1      # (an admin in-match swap counts on its new team)
            if back and not mates:
                # live: a player came back into its old slot (team 0) while team 0 already had 2 and team 1 had 1 = a 3 v 1.
                # A returning player counts like any joiner: when its old team is the bigger one it takes the smaller team (a free / new slot
                # of that parity); its old slot is left behind as gone (jip_join)
                own = team_in(mid, pl.index(token), token)
                free_other = any(i != 0 and i % 2 != own and t in dep and now - self.departed_t.get(t, 0) >= JIP_REUSE_S for i, t in enumerate(pl))                     or any(i % 2 != own for i in range(len(pl), ENGINE_SLOTS))
                if cnt[own] > cnt[1 - own] and free_other and now - self.departed_t.get(token, 0) >= JIP_REUSE_S:
                    back = False
            if back and not mv:
                try:
                    teams_of = {t: team_in(mid, i, t) for i, t in enumerate(pl) if t not in dep and t != token}
                    rt, rnote = rule_team_for(token, teams_of, rule_groups(list(teams_of) + [token]))
                    if rt is not None and rt != team_in(mid, pl.index(token), token) and rnote.startswith("together"):
                        back = False                           # its together mate is on the other team: join that one instead
                except Exception:
                    pass
            if back:                                           # its own slot, same team; the host dropped the old peer after ~5 s
                if now - self.departed_t.get(token, 0) < JIP_REUSE_S:
                    why_not.append("%s: left < %.0f s ago" % (mid, JIP_REUSE_S)); continue
                if cur_map not in map_pool(active + [token], support=[token]):
                    why_not.append("%s: map %s not in the pool for this joiner" % (mid, MAPS[cur_map][2])); continue
                score = (3, 0, 0, ck[0])                          # its own match before any other
                if best is None or score > best[0]:
                    best = (score, mid, pl.index(token))
                continue
            if rejoin_host and now - self.departed_t.get(token, 0) < JIP_REUSE_S:
                why_not.append("%s: left < %.0f s ago" % (mid, JIP_REUSE_S)); continue
            free = [i for i, t in enumerate(pl) if i != 0 and t in dep and now - self.departed_t.get(t, 0) >= JIP_REUSE_S]
            # parties: a member joins the team of the friends already playing (up to 2 players of imbalance are accepted for that); a
            # party that joins together takes the smaller team that has room for all of them; a lone joiner takes the smaller team
            anchor = sorted({team_in(mid, i, t) for i, t in enumerate(pl) if t in mates and t not in dep})
            # (Interrogation 2 v 1: a joiner went to the 2-team = 3 v 1): a LONE joiner never ends up more than ONE player ahead -
            # the smaller team first, friends / admin 'apart' rules included (a party of several keeps the 2-player allowance; 'together' stays hard)
            lim = 1 if need == 1 else 2
            want, forced = None, False
            if mv:
                want, forced = mv[1], True                     # the admin's team, whatever the balance
            elif team is not None:
                want, forced = team, True                      # a party follower: the team of the member that joined first, no balance check
            elif anchor:
                a = min(anchor, key=lambda x: cnt[x])
                if cnt[a] + need - cnt[1 - a] <= lim:
                    want, forced = a, True
            if want is None and not mv and need == 1:
                # live: a player (together with one player, apart from another) was put on that player's team 0: admin rules decide the team
                # of a joiner too (together > apart > balance); a rule that cannot be met (no slot / > 2 imbalance) falls back to the balance
                try:
                    teams_of = {t: team_in(mid, i, t) for i, t in enumerate(pl) if t not in dep}
                    rt, rnote = rule_team_for(token, teams_of, rule_groups(list(teams_of) + [token]))
                except Exception:
                    rt, rnote = None, ""
                if rt is not None:
                    has_slot = any(i % 2 == rt for i in free) or (len(pl) < ENGINE_SLOTS and len(pl) % 2 == rt)
                    if (cnt[rt] + 1 - cnt[1 - rt] <= lim or rnote.startswith("together")) and has_slot:   # together is hard: any imbalance
                        want, forced = rt, False
                        log("JIP", mid, "joiner %s -> team %d by admin rule (%s)" % (tag(token), rt, rnote))
                    else:
                        log("JIP", mid, "admin rule for %s wants team %d but %s - balance instead" % (tag(token), rt, "no free slot" if not has_slot else "it would be %d v %d" % (cnt[0] + (rt == 0), cnt[1] + (rt == 1))))
                elif rnote:
                    log("JIP", mid, "admin rules for %s cannot be met (%s) - balance instead" % (tag(token), rnote))
            if want is None and not mv:
                # a mid / late joiner always goes to the LOSING team (fewer reinforcements left, players' op 0xE3, < 30 s old) -
                # as long as that leaves at most 2 players of imbalance and the team has a slot; otherwise the smaller team as before
                lv = self.lives.get(mid)
                if lv and now - lv[2] < 30 and lv[0] != lv[1]:
                    lose = 0 if lv[0] < lv[1] else 1
                    # (live: the rule picked a team with no free slot for a lone joiner - the join failed every second, nobody could
                    # join; a slot must exist on that team: a free (departed) index of its parity, or the next index when it has that parity)
                    has_slot = any(i % 2 == lose for i in free) or (len(pl) < ENGINE_SLOTS and (len(pl) % 2 == lose or need > 1))
                    # (2 v 2 at 19 v 20 lives sent a joiner to 3 v 2, a leaver then made it 3 v 1. Only a team that is
                    # really behind (3+ lives) and ends at most one player up takes the joiner; otherwise the smaller team.)
                    if abs(lv[0] - lv[1]) >= 3 and cnt[lose] + need - cnt[1 - lose] <= max(1, need) and has_slot:
                        want, forced = lose, need > 1
                        log("JIP", mid, "joiner -> losing team %d (lives %d v %d, players %d v %d)" % (lose, lv[0], lv[1], cnt[0], cnt[1]))
            if want is None and need == 1 and cnt[0] == cnt[1] and os.environ.get("T2R_TEAM_BALANCE", "1") != "0":
                # (one-sided matches) a lone joiner into an even match took whichever team the next slot index has; joins in progress outnumber
                # formed matches about 2.5 to 1, so most players reach a match this way. It now takes the team with the lower rating sum
                # (player_skill) when that team has a slot.
                try:
                    tin = [(team_in(mid, i, t), t) for i, t in enumerate(pl) if t not in dep]
                    sk = player_skill([t for _k, t in tin] + [token])
                    tot = [sum(sk[t] for k, t in tin if k == side) for side in (0, 1)]
                    if abs(tot[0] - tot[1]) > 1e-6:
                        weak = 0 if tot[0] < tot[1] else 1
                        if any(i % 2 == weak for i in free) or (len(pl) < ENGINE_SLOTS and len(pl) % 2 == weak):
                            want = weak
                            log("JIP", mid, "joiner %s (%.2f) -> weaker team %d (rating %.2f v %.2f, players %d v %d)" % (tag(token), sk[token], weak, tot[0], tot[1], cnt[0], cnt[1]))
                except Exception as ex:                              # noqa: BLE001
                    log("JIP", mid, "weaker-team pick skipped:", repr(ex))
            if want is None:
                want = 0 if cnt[0] < cnt[1] else 1 if cnt[1] < cnt[0] else None     # the smaller team; None = even, either
                if need > 1:
                    forced = True
                    if want is None:                                # even: the team that has the room
                        room = [sum(1 for i in free if i % 2 == k) + sum(1 for i in range(len(pl), ENGINE_SLOTS) if i % 2 == k) for k in (0, 1)]
                        want = 0 if room[0] >= room[1] else 1
                    if cnt[want] + need - cnt[1 - want] > max(1, need):
                        why_not.append("%s: a party of %d would unbalance the teams (%d v %d)" % (mid, need, cnt[0], cnt[1])); continue
            reuse = [i for i in free if want is None or i % 2 == want]
            if forced or TEAM_SWAP.get(mid):   # (a hole below a higher index is filled with a gone placeholder; after an in-match swap the next index can have the wrong parity for the smaller team)
                new_idx = [i for i in range(len(pl), ENGINE_SLOTS) if want is None or i % 2 == want]
            else:        # a lone joiner only appends the next index
                new_idx = [len(pl)] if len(pl) < ENGINE_SLOTS and (want is None or len(pl) % 2 == want) else []
            if len(reuse) + len(new_idx) < need or not (reuse or new_idx):
                why_not.append("%s: no free slot on the smaller team (%d v %d)" % (mid, cnt[0], cnt[1])); continue
            holes_free = [i for i in reuse if pl[i] == hostmig.placeholder(i)]    # a placeholder never had a peer: refill it before anything else
            slot = holes_free[0] if holes_free else new_idx[0] if (new_idx and new_idx[0] == len(pl)) else (reuse[0] if reuse else new_idx[0])
            new_pl = list(pl)
            if rejoin_host: new_pl[0] = hostmig.placeholder(0)
            holes = []
            while len(new_pl) < slot:
                holes.append(hostmig.placeholder(len(new_pl))); new_pl.append(holes[-1])
            if slot == len(new_pl): new_pl.append(token)
            else: new_pl[slot] = token
            new_active = [t for t in new_pl if (t not in dep and t not in holes) or t == token]
            if cur_map not in map_pool(new_active, support=[token]):
                why_not.append("%s: map %s not in the pool for %d players / this joiner" % (mid, MAPS[cur_map][2], len(new_active))); continue
            # the old host: its own match first; a party mate: the friends' match; others: fullest, then most time left
            # (same region: among the others a match whose host is in the joiner's region comes first, then fullest, then most time left)
            tier = 3 if rejoin_host else 2 if (anchor and forced) else 1
            same = 0
            if tier == 1 and MM_REGION_ON:
                try:
                    jr = mm_region(token)
                    same = 1 if jr in hostmig.MM_REGIONS and jr == mm_region(pl[hslot]) else 0
                except Exception:                              # noqa: BLE001
                    same = 0
            score = (tier, same, len(active) if tier == 1 else 0, ck[0])
            if best is None or score > best[0]:
                best = (score, mid, slot)
        return best
    def jip_can_join(self, token, min_left=None, need=1):
        """Dry run of jip_join: True when `token` could be put into a running match right now (try_form: players that a running match
        with room and time left can take do not form a small new match). min_left overrides JIP_MIN_LEFT."""
        with self.lock:
            if token not in self.queue or self._in_match(token):
                return False
            return self._jip_pick(token, [], min_left, need) is not None
    def jip_join(self, token, why_not=None, need=1, only_mid=None, team=None):
        """Put the searching `token` into a running match that has room. Returns (mid, slot, players, old_key) or None
        (why_not: a list that receives the reasons). Rules, all of them must hold:
 - the match is launched + GAME-START went out, its result is not in (Interrogation matches too since, T2R_JIP_INT=0 = not)
          - its host is present (after a host migration the joiner's ext follows the server's 0xE7 reply to the new host; a migration still
            pending / given up refuses; the crashed old host returns as a client in a free slot, never slot 0), and Steam P2P / relay mode fits the joiner
          - its clock was reported fresh (<= JIP_CLOCK_TTL s ago) with >= JIP_MIN_LEFT s left and not ended
          - fewer than MATCH_SIZE active players; a slot is free (a never used index, or one whose player left >= JIP_REUSE_S ago)
            whose team (slot % 2) is the smaller one - never makes the teams uneven by more than one
          - the match's map is in the pool for the new head count (max team size, packs, SP map support of the joiner)
          - parties: a member joins the team of the friends already in the match (up to 2 players of imbalance); `need` > 1 = this many
            party members join one after the other (the first one needs room for all of them on one team; the index skipped on the other
            team becomes a gone placeholder that the next joiner of that team takes); `only_mid` = the friends' match
        The fullest eligible match wins (fewest free slots: the players already waiting for company get it). The joiner is
        written into the match's player list in place (slot = list index, as everywhere), the per-match dictionaries keyed by the
        player set are moved to the new key, and the session id stays what the running players hold."""
        why_not = why_not if why_not is not None else []
        now = time.time()
        with self.lock:
            if token not in self.queue or self._in_match(token):
                return None
            best = self._jip_pick(token, why_not, None, need, only_mid, team)
            if best is None:
                return None
            _, mid, slot = best
            pl = self.matches[mid]
            old_key = frozenset(pl)
            old_sid = match_sid(pl)
            dep = self.departed.setdefault(mid, set())
            mv = self.move_team.pop(token, None)
            moved = bool(mv) or (token in pl and pl.index(token) != slot)     # admin team switch, or a balance switch of a returning player
            if token in pl:                                          # back into the match it left (crash / leave / admin team move): its ext keeps the life's kit
                JIP_BACK[token] = (mid, now)
            else:
                JIP_BACK.pop(token, None)
            if moved and token in pl and pl.index(token) != slot:   # team switch: its old slot (other team) is left behind as gone
                oi = pl.index(token)
                if oi != 0:
                    ph = hostmig.placeholder(oi)
                    pl[oi] = ph; dep.add(ph); self.departed_t[ph] = self.departed_t.get(token, now)
                    dep.discard(token)
                if token in TEAM_SWAP.get(mid, {}):
                    TEAM_SWAP[mid].pop(token, None)                  # a new slot: its parity is its team now (an in-match swap belonged to the old slot)
                log("TEAMS move%s: %s re-enters %s in slot %d (team %d), was slot %d" % ("" if mv else " (balance)", tag(token), mid, slot, slot % 2, oi))
            if pl and pl[0] == token and slot != 0:                  # the crashed old host returns in a new slot: slot 0 becomes a gone placeholder
                ph = hostmig.placeholder(0)
                dep.add(ph)
                self.departed_t[ph] = self.departed_t.get(token, 0)
                pl[0] = ph
                dep.discard(token)
            while len(pl) < slot:                                    # a hole below the joiner's index (party on one team): gone placeholders
                ph = hostmig.placeholder(len(pl))
                pl.append(ph); dep.add(ph); self.departed_t[ph] = 0
            # late-join bonus (2013 scoring.md: 100 parts per reinforcement the team is down + 200 when the enemy has one more player),
            # priced before the joiner is in the list; paid with its clan day (late_bonus_take)
            team = team_in(mid, slot, token) if token in pl and pl.index(token) == slot else slot % 2
            n_ally = sum(1 for i, t in enumerate(pl) if t not in dep and team_in(mid, i, t) == team and t != token)
            n_enemy = sum(1 for i, t in enumerate(pl) if t not in dep and team_in(mid, i, t) != team)
            lv = self.lives.get(mid)
            # PS4 v1.11 ss-comp-tdm-mode (lambda 0x13a0, event of a late joiner, stat 0x405271b1 LATEJOIN_POINTS x 100 parts): the points are
            # round(minutes elapsed - 1) [= (900 - match-timer) / 60 - 1] + 2 / 4 / 6 when the enemy team has 1 / 2 / 3 more players; paid when > 0
            # (the older lives-based rule, 100 per reinforcement down, came from the PS3 decode and is not in the v1.11 script)
            ck = self.clock.get(mid)
            down = 0
            if ck and not ck[2]:
                elapsed = ck[1] - (ck[0] - (now - ck[3]))
                down = int(math.floor(elapsed / 60.0 - 1.0 + 0.5))
            down += {1: 2, 2: 4, 3: 6}.get(n_enemy - n_ally, 0)
            bonus = 100 * down if down > 0 else 0
            if slot == len(pl):
                pl.append(token)
            else:
                old = pl[slot]
                pl[slot] = token
                dep.discard(old)
                dep.discard(token)                                   # (a rejoin into its own slot: old == token)
            new_key = frozenset(pl)
            if mid in INT.matches:                                   # the joiner's team in the Interrogation record (intel / lockbox / stage reports)
                INT.roster(mid, pl, lambda i, _m=mid, _p=list(pl): team_in(_m, i, _p[i]))
            SID_KEEP[new_key] = old_sid                              # the running players' session id stays
            for d in (MATCH_MAP, MATCH_P2P, VOTES, TIMING):
                if old_key in d and old_key != new_key:
                    d[new_key] = d.pop(old_key)
            since = self.queue.pop(token)
            self.qtime.setdefault(mid, {})[token] = since
            if book_on():
                self.book.remove(token, "joined the running match %s" % mid, now)
            self.joining[token] = (mid, now)
            self.polled.get(mid, {}).pop(token, None)
            self.departed_t.pop(token, None)
            if old_key != new_key and not moved and lv and now - lv[2] <= 3 * JIP_CLOCK_TTL and min(lv[0], lv[1]) < LIVES_START:
                # PS4 v1.11 FUN_003ff560(.., 0): progress = 1 - min(lives of both teams) / lives at the start; the joiner's supplies are scaled
                # by min(3, 1 / (1 - progress)) = start / min lives (a team already out of lives = the cap 3)
                self.late_mult[token] = (mid, min(3.0, LIVES_START / max(1, min(lv[0], lv[1]))))
            else:
                self.late_mult.pop(token, None)
            if bonus and old_key != new_key and not moved:              # (a player back in its own slot is no late joiner; nor one the admin moved)
                self.late_bonus[token] = (mid, bonus)
                log("jip: late-join bonus", tag(token), bonus, "parts (%d late-join points, %d v %d players)" % (down, n_ally, n_enemy))
            else:
                self.late_bonus.pop(token, None)
            return mid, slot, list(pl), old_key
    def jip_lives(self, token, myteam, ally, enemy):
        """Team lives (reinforcements) from a player's op 0xE3 (viewer relative: ally = the reporter's own team). 0xFF = unknown."""
        if myteam not in (0, 1) or ally > 250 or enemy > 250:
            return
        with self.lock:
            mid = self._mid_locked(token)
            if mid is None:
                return
            l = [0, 0]
            l[myteam] = ally; l[1 - myteam] = enemy
            self.lives[mid] = (l[0], l[1], time.time())
    def late_mult_take(self, token, mid):
        """The late-join supply multiplier of `token` for the match `mid` (once); 1.0 = no late join."""
        with self.lock:
            b = self.late_mult.get(token)
            if b and b[0] == mid:
                del self.late_mult[token]
                return b[1]
        return 1.0
    def late_bonus_take(self, token, mid):
        """The late-join bonus parts owed to `token` for the match `mid` (once)."""
        with self.lock:
            b = self.late_bonus.get(token)
            if b and b[0] == mid:
                del self.late_bonus[token]
                return b[1]
        return 0


# ---------------------------------------------------------------- parties + team balancing
def token_by_name(name):
    """Online player whose shown name matches (case-insensitive)."""
    if isinstance(name, bytes):
        name = name.decode("utf-8", "ignore")
    name = name.strip().lower()
    if not name:
        return None
    for t in PRES_TOKENS():
        if (get_name(t) or "").lower() == name:
            return t
    return None

# Team balance rating (one-sided matches): the teams are balanced by K/D. Measured on 941 matches over three days (match_stats): the team with the higher mean of the old rating
# (smoothed win rate + parts per match) got more kills in 63 % of the matches, the higher mean smoothed K/D (K + 10) / (D + 10) in 70 %;
# a K/D gap in the top quarter made a stomp (loser under half the winner's kills) 35 % likely, the bottom quarter 18 %. The rating is now the
# account's K/D over its last SKILL_MATCHES matches, mapped onto the old scale (same mean 0.63 and spread 0.12 over the 414 players with 5+
# matches: K/D mean 1.11, spread 0.49), so the top-player bonus, HOST_SWAP_SKILL and the admin view keep their meaning.
SKILL_KD_PRIOR = 10.0                  # kills and deaths added to the totals: a new account sits at K/D 1.0
SKILL_MATCHES = 100
SKILL_A, SKILL_B = 0.36, 0.245         # rating = SKILL_A + SKILL_B x K/D  (0.63 + (kd - 1.11) x 0.12 / 0.49)
SKILL_NEW = SKILL_A + SKILL_B * 1.0
SKILL_CACHE_S = 300.0
_skill_cache = {}                      # token -> (time, rating)
_skill_index = [False]


def skill_of_kd(kills, deaths):
    kd = (max(0, kills) + SKILL_KD_PRIOR) / (max(0, deaths) + SKILL_KD_PRIOR)
    return SKILL_A + SKILL_B * kd


def player_skill(tokens):
    """Team balance rating per token from the account's last SKILL_MATCHES matches with kill stats (every install of one Steam account adds up),
    see SKILL_KD_PRIOR above. New players sit at K/D 1.0. Cached SKILL_CACHE_S s per token."""
    now = time.time()
    out = {}
    need = []
    for t in tokens:
        e = _skill_cache.get(t)
        if e and now - e[0] < SKILL_CACHE_S:
            out[t] = e[1]
        else:
            need.append(t)
    if need:
        try:
            with _dblock, dbc() as c:
                if not _skill_index[0]:
                    c.execute("CREATE INDEX IF NOT EXISTS match_stats_token_ts ON match_stats(token, ts)")
                    _skill_index[0] = True
                for t in need:
                    r = c.execute("SELECT steamid FROM players WHERE token=?", (t,)).fetchone()
                    sid = r[0] if r and r[0] else 0
                    toks = [x for (x,) in c.execute("SELECT token FROM players WHERE steamid=?", (sid,)).fetchall()] if sid else []
                    if t not in toks:
                        toks.append(t)
                    rows = c.execute("SELECT kills, deaths FROM match_stats WHERE token IN (%s) AND kills >= 0 ORDER BY ts DESC LIMIT ?"
                                     % ",".join("?" * len(toks)), toks + [SKILL_MATCHES]).fetchall()
                    v = skill_of_kd(sum(k or 0 for k, _d in rows), sum(max(d or 0, 0) for _k, d in rows))
                    out[t] = v
                    _skill_cache[t] = (now, v)
        except Exception as ex:
            log("SKILL lookup failed", ex)
    if len(_skill_cache) > 20000:
        for k in [k for k, e in list(_skill_cache.items()) if now - e[0] >= SKILL_CACHE_S]:
            _skill_cache.pop(k, None)
    return {t: out.get(t, SKILL_NEW) for t in tokens}

TOP_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "top_players.txt")
_top_cache = {"mt": None, "keys": frozenset()}
def top_players(tokens):
    """. The rating
    (win rate + parts) under-rates players who carry a team, so top_players.txt next to this file names them by hand: one shown
    name (any case) or Steam id per line, # comments. Read again whenever the file changes. Returns the set of tokens that are top."""
    try:
        mt = os.path.getmtime(TOP_FILE)
    except OSError:
        return set()
    if mt != _top_cache["mt"]:
        keys = set()
        try:
            for ln in open(TOP_FILE, encoding="utf-8", errors="ignore"):
                ln = ln.split("#", 1)[0].strip().lower()
                if ln:
                    keys.add(ln)
        except OSError:
            pass
        _top_cache.update(mt=mt, keys=frozenset(keys))
    keys = _top_cache["keys"]
    if not keys:
        return set()
    out = set()
    for t in tokens:
        if (get_name(t) or "").strip().lower() in keys or str(token_steamid(t) or "") in keys:
            out.add(t)
    return out

TEAM_RULES_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "team_rules.txt")
_rules_cache = {"mt": None, "rules": ()}
def team_rules_text():
    try:
        return open(TEAM_RULES_FILE, encoding="utf-8", errors="ignore").read()
    except OSError:
        return ""
def team_rules_set(text):
    """Admin (POST /teams): replace team_rules.txt. Lines: "apart A B [C ...]" (never on one team; with more of them than teams: as
    spread as possible), "together A B [...]" (same team). Names = shown names (any case) or Steam ids; # comments."""
    text = str(text)[:4000].replace("\r", "")
    tmp = TEAM_RULES_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, TEAM_RULES_FILE)
    log("TEAMS rules set by admin: %s" % " | ".join(l.strip() for l in text.splitlines() if l.strip() and not l.strip().startswith("#")))
    return team_rules()
def team_rules():
    """[(kind, text)] from team_rules.txt, re-read when it changes. `text` = the rest of the line (names may be "quoted", or unquoted
    with spaces: rule_groups matches it against the names of the players at hand, longest name first)."""
    try:
        mt = os.path.getmtime(TEAM_RULES_FILE)
    except OSError:
        _rules_cache.update(mt=None, rules=())
        return ()
    if mt != _rules_cache["mt"]:
        out = []
        for ln in team_rules_text().splitlines():
            ln = ln.split("#", 1)[0].strip()
            w = ln.split(None, 1)
            if len(w) == 2 and w[0].lower() in ("apart", "together"):
                out.append((w[0].lower(), w[1]))
        _rules_cache.update(mt=mt, rules=tuple(out))
    return _rules_cache["rules"]
def rule_match(text, tokens, key=None):
    """The tokens named in a rule line's text. Quotes are ignored; a name matches as whole words (case-insensitive), the longest
    names first and each stretch of text only once, so "john smith" does not also match a player called "a player". Steam ids match too."""
    import re
    norm = " " + " ".join(re.sub(r"[\"'`,;]", " ", text).lower().split()) + " "
    if key is None:
        key = {t: ((get_name(t) or "").strip().lower(), str(token_steamid(t) or "")) for t in tokens}
    out = []
    for t in sorted(tokens, key=lambda t: -len(key[t][0])):
        for nm in key[t]:
            nm = " ".join(nm.split())
            if nm and (" " + nm + " ") in norm:
                norm = norm.replace(" " + nm + " ", "  ", 1) if nm == key[t][0] else norm
                out.append(t)
                break
    return [t for t in tokens if t in out]
def rule_groups(tokens):
    """The rules (+ top_players.txt as one "apart" group) as token groups of the players at hand: [(kind, [tokens])], groups of 2+ only."""
    key = {t: ((get_name(t) or "").strip().lower(), str(token_steamid(t) or "")) for t in tokens}
    out = []
    for kind, text in team_rules():
        g = rule_match(text, tokens, key)
        if len(g) >= 2:
            out.append((kind, g))
    tp = top_players(tokens)
    top = [t for t in tokens if t in tp]
    if len(top) >= 2:
        out.append(("apart", top))
    return out
def rule_team_for(token, teams_of, rg):
    """A joiner and the admin rules: teams_of = {other token: team 0/1} for the players in the match. Returns (team or None, note):
    together > apart; an impossible combination gives (None, why) so the caller falls back to the balance."""
    tog, avoid = set(), set()
    for kind, g in rg:
        if token not in g:
            continue
        for x in g:
            if x == token or x not in teams_of:
                continue
            (tog if kind == "together" else avoid).add(teams_of[x])
    if len(tog) > 1:
        return None, "together rules need both teams"
    if tog:
        t = next(iter(tog))
        if t in avoid and len(avoid) == 1:
            return t, "together beats apart (both rules name team %d)" % t
        return t, "together"
    if len(avoid) == 1:
        return 1 - next(iter(avoid)), "apart"
    if len(avoid) == 2:
        return None, "apart rules exclude both teams"
    return None, ""

def split_teams(players, party_of=None, skill=None, top=None, groups_rules=None):
    """Two teams for `players` (ordered by queue time), sizes differing by at most 1, party
    members together (a party that cannot fit a team is split). Returns (team0, team1) with
    len(team0) >= len(team1). Queue order breaks ties, so it is deterministic."""
    party_of = party_of or {}
    n = len(players)
    cap = (n + 1) // 2
    groups, seen = [], {}
    for t in players:
        pid = party_of.get(t)
        if pid is None:
            groups.append([t])
        elif pid in seen:
            seen[pid].append(t)
        else:
            seen[pid] = [t]
            groups.append(seen[pid])
    groups.sort(key=len, reverse=True)           # stable: big parties first, then queue order
    teams = ([], [])
    if skill and n <= 12:
        # ("we need that manual switching players from teams ASAP"): every split of the lobby is scored (n <= 12: at most 924) -
        # admin rules first (team_rules.txt / top_players.txt: "apart" = fewest pairs of the group on one team, "together" = not split),
        # then parties kept whole, then the closest skill sums (top players count +0.5). Ties: the first split in queue order.
        import itertools
        top = top or set()
        rg = groups_rules or []
        w = {t: skill.get(t, SKILL_NEW) + (0.5 if t in top else 0.0) for t in players}
        tot = sum(w.values())
        parties = [g for g in groups if 1 < len(g) <= cap]
        best, best_c = None, None
        for c in itertools.combinations(range(n), cap):
            a = set(players[i] for i in c)
            if n % 2 == 0 and players[0] not in a:
                continue                                   # (mirror splits)
            pen = 0
            for kind, g in rg:
                k = sum(1 for x in g if x in a)
                if kind == "apart":           # each pair on one team: 100, + their strength (an unavoidable pair = the weakest two)
                    for side in ([x for x in g if x in a], [x for x in g if x not in a]):
                        for i in range(len(side)):
                            for j in range(i + 1, len(side)):
                                pen += 100 + 10 * (w[side[i]] + w[side[j]])
                elif 0 < k < len(g):
                    pen += 10000                  # together is a HARD rule: beats balance and parties
            for g in parties:
                k = sum(1 for x in g if x in a)
                if 0 < k < len(g):
                    pen += 500                         # a party is never split (social parties): only a together rule is harder
            cost = pen + abs(2 * sum(w[x] for x in a) - tot)
            if best_c is None or cost < best_c - 1e-9:
                best_c, best = cost, a
        t0 = [x for x in players if x in best]
        t1 = [x for x in players if x not in best]
        return (t0, t1) if len(t0) >= len(t1) else (t1, t0)
    if skill:
        # balance: parties first (biggest), then singles strongest first; each group goes to the team with the lower skill
        # sum that still has room (sizes stay within 1) - a greedy split that keeps the two sums close
        # top players (top_players.txt) are spread first: each goes to the team with fewer of them (counts differ by at most 1),
        # and they count as the strongest in the sums, so the rest of the lobby is balanced against them
        top = top or set()
        nt = lambda g: sum(1 for x in g if x in top)
        groups.sort(key=lambda g: (-len(g), -nt(g), -sum(skill.get(x, SKILL_NEW) + (0.5 if x in top else 0.0) for x in g)))
        tot, tops = [0.0, 0.0], [0, 0]
        for g in groups:
            gs = sum(skill.get(x, SKILL_NEW) + (0.5 if x in top else 0.0) for x in g)
            order = sorted((0, 1), key=lambda i: (tops[i] if nt(g) else 0, tot[i], len(teams[i])))
            placed = False
            for a in order:
                if len(teams[a]) + len(g) <= cap:
                    teams[a].extend(g); tot[a] += gs; tops[a] += nt(g); placed = True
                    break
            if not placed:
                for x in g:
                    a = 0 if len(teams[0]) < len(teams[1]) or (len(teams[0]) == len(teams[1]) and tot[0] <= tot[1]) else 1
                    teams[a].append(x); tot[a] += skill.get(x, SKILL_NEW) + (0.5 if x in top else 0.0); tops[a] += x in top
        return teams if len(teams[0]) >= len(teams[1]) else (teams[1], teams[0])
    for g in groups:
        a = 0 if len(teams[0]) <= len(teams[1]) else 1
        if len(teams[a]) + len(g) <= cap:
            teams[a].extend(g)                   # the whole party fits: keep it together
            continue
        for t in g:                              # does not fit: spill over
            a = 0 if len(teams[0]) <= len(teams[1]) else 1
            teams[a].append(t)
    return teams if len(teams[0]) >= len(teams[1]) else (teams[1], teams[0])

def order_slots(team0, team1, host=None):
    """Slot order for the 0x04 records: team_of(slot) = slot % 2, so team0 takes even slots and
    team1 odd ones; the host (slot 0) is moved to the front of its team (that team becomes
    team 0). Returns the player list."""
    # (live:, 3 v 2): the slot parity makes team 0 the one with the extra player in an odd lobby (5 players = slots 0, 2, 4 v
    # 1, 3). A host from the SMALLER team was swapped into team 0, so the interleave put the bigger team's last player on slot 4 = the
    # host's team: a player, B, C v D, E became a player, B v D, E, C. With uneven teams the
    # host must come from the bigger one (team_order picks it so); a host from the smaller one is not moved to the front (logged).
    if host in team1 and len(team1) >= len(team0):
        team0, team1 = team1, team0
    elif host in team1:
        log("TEAMS: host %s is on the smaller team - teams kept, host not slot 0" % tag(host))
        host = None
    t0, t1 = list(team0), list(team1)
    if host in t0:
        t0.remove(host); t0.insert(0, host)
    out = []
    for i in range(max(len(t0), len(t1))):
        if i < len(t0): out.append(t0[i])
        if i < len(t1): out.append(t1[i])
    return out

def team_order(players):
    """Final slot order for a formed match: parties together, balanced, ping-picked host."""
    party_of = {t: PARTIES.pid(t) for t in players if PARTIES.pid(t)}
    sk = player_skill(players) if os.environ.get("T2R_TEAM_BALANCE", "1") != "0" else None
    top = top_players(players) if sk else set()
    rg = rule_groups(players) if sk else []
    t0, t1 = split_teams(players, party_of, sk, top, rg)
    if rg:
        log("TEAMS rules: %s" % "; ".join("%s %s" % (k, ",".join(tag(x) for x in g)) for k, g in rg))
    for k, g in rg:
        if k == "together" and len({x in t0 for x in g}) > 1:
            log("TEAMS: together rule %s could not be kept" % ",".join(tag(x) for x in g))
    if sk:
        log("TEAMS balanced: %.2f v %.2f | %s v %s" % (sum(sk[x] for x in t0), sum(sk[x] for x in t1),
            ",".join("%s %.2f%s" % (tag(x), sk[x], " TOP" if x in top else "") for x in t0),
            ",".join("%s %.2f%s" % (tag(x), sk[x], " TOP" if x in top else "") for x in t1)))
    bo = best_host_order(players) if len(players) >= 3 else []
    host = bo[0] if bo else None
    if len(players) >= 3 and len(t0) != len(t1):     # uneven: the host comes from the bigger team (order_slots: slot parity)
        big = t0 if len(t0) > len(t1) else t1
        small = t1 if big is t0 else t0
        if not (bo and bo[0] in big):
            sw = None
            if HOST_SWAP and MM_PING_GATE and PING_KICK_MS > 0 and bo:
                try:
                    sw = host_swap_pick(bo[0], big, small, players, sk, top, rg, party_of)
                except Exception as e:                   # noqa: BLE001 - never breaks the formation: the old host rule follows
                    log("HOST-PICK swap ERR", repr(e)); sw = None
            if sw:
                big2, small2 = sw
                t0, t1 = big2, small2
                host = bo[0]
            else:
                host = host_from_team(big, players)
    if len(players) >= 3 and (host not in t0 and host not in t1):
        host = None
    if len(players) < 3:
        host = best_host_order(players)[0] if players else None      # (no_host.txt: the other one hosts)
    return order_slots(t0, t1, host)

HOST_SWAP = os.environ.get("T2R_HOST_SWAP", "1") != "0"
HOST_SWAP_SKILL = float(os.environ.get("T2R_HOST_SWAP_SKILL", "0.35"))    # the most the two swapped players' skills may differ

def _worst_ping(h, players):
    ps = [pair_ping(h, o) for o in players if o != h]
    return None if (not ps or None in ps) else max(ps)

def host_swap_pick(host, big, small, players, sk, top, rg, party_of):
    """Uneven teams, the ping-picked `host` sits on the SMALLER team (the engine needs the host on the bigger one, order_slots) and no player of
    the bigger team is within the match ping limit of everybody: host_from_team would take the best of them anyway and he would get somebody
    removed (match 5346a92f: host 189 ms worst on the smaller team, the bigger team's pick 228 ms, a player removed). Swap the host with the bigger
    team's player closest in skill instead: parties stay whole (no swap of a party member), no 'together' group is split that was whole, no 'apart'
    group gets a pair more on one team, the two skills differ by at most HOST_SWAP_SKILL. Returns (new bigger team, new smaller team) with the
    queue order kept, or None (no swap needed / possible: the caller falls back to host_from_team)."""
    lim = ping_match_ms()
    hw = _worst_ping(host, players)
    if host not in small or hw is None or hw > lim:
        return None
    for c in host_allowed(list(big)):
        w = _worst_ping(c, players)
        if w is not None and w <= lim:
            return None                                      # a bigger-team host within the limit exists: host_from_team takes him
    w = {x: ((sk or {}).get(x, 0.5) + (0.5 if x in (top or ()) else 0.0)) for x in players}
    def pid_mates(x, team):
        pid = party_of.get(x)
        return pid is not None and any(party_of.get(y) == pid for y in team if y != x)
    def cost(b, s):
        tog = apart = 0
        for kind, g in rg or []:
            k = sum(1 for x in g if x in b)
            if kind == "together" and 0 < k < len(g):
                tog += 1
            elif kind == "apart":
                apart += k * (k - 1) // 2 + (len(g) - k) * (len(g) - k - 1) // 2
        return tog, apart
    if pid_mates(host, small):
        log("HOST-PICK", tag(host), "is on the smaller team with party mates - no swap")
        return None
    base = cost(big, small)
    best = None
    for c in big:
        if c == host or pid_mates(c, big):
            continue
        gap = abs(w[c] - w[host])
        if sk and gap > HOST_SWAP_SKILL:
            continue
        nb = [host if x == c else x for x in big]
        ns = [c if x == host else x for x in small]
        k = cost(nb, ns)
        if k[0] > base[0] or k[1] > base[1]:
            continue
        if best is None or gap < best[0] - 1e-9:
            best = (gap, c, nb, ns)
    if not best:
        log("HOST-PICK", tag(host), "(worst %d ms) is on the smaller team and nobody on the bigger one is within %d ms - no valid swap (parties / rules / skill), old rule" % (hw, lim))
        return None
    gap, c, nb, ns = best
    log("HOST-PICK", tag(host), "(worst %d ms, within %d) is on the smaller team and nobody on the bigger one is within the limit (best %s ms) - swapped with %s (skill %.2f v %.2f); teams now %.2f v %.2f"
        % (hw, lim, ",".join(str(_worst_ping(x, players)) for x in big), tag(c), w[host], w[c], sum(w[x] for x in nb), sum(w[x] for x in ns)))
    return nb, ns

def host_from_team(big, players):
    """The host of an uneven match when the overall pick is on the smaller team: the bigger team's player whose pings to EVERY player of the
    match are best (same key and capacity rule as best_host_order). Match 8bb74ec7 (2 v 1: a Middle East player and a European party v a
    North American): the fallback ranked only the bigger team's 2 players among themselves - one link, so no ping decided and the queue's
    first player hosted (worst ping 225 ms where the European would have had 205). Unknown pings: the old fallback (queue order)."""
    allowed = host_allowed(list(big))
    keyed = []
    for c in allowed:
        pings = [pair_ping(c, o) for o in players if o != c]
        if None in pings:
            log("HOST-PICK", "bigger team: ping unknown for", tag(c), "- queue order")
            return (best_host_order(list(big)) or [None])[0]            # (no_host.txt still applies)
        keyed.append((c, hostmig.host_key(pings, perf_penalty(c))))
    if not keyed:
        return None
    if MM_PING_GATE and PING_KICK_MS > 0:          # (MM_PING_GATE) same rule as best_host_order: a host that gets nobody removed for ping wins
        fit = [kc for kc in keyed if max(pair_ping(kc[0], o) for o in players if o != kc[0]) <= ping_match_ms()]
        if fit and len(fit) < len(keyed):
            log("HOST-PICK", "bigger team: ping limit %d ms leaves" % ping_match_ms(), [tag(kc[0]) for kc in fit])
            keyed = fit
    best = min(keyed, key=lambda c: c[1])[0]
    if hostcap.MODE >= 2:
        try:
            alt, lines = hostcap.pick(keyed, len(players), HOSTCAP.verdict, label=tag)
            log("HOST-PICK", "bigger team cap:", "; ".join(lines), "->", tag(alt))
            best = alt or best
        except Exception as e:
            log("HOST-PICK cap ERR", repr(e))
    log("HOST-PICK", "overall pick is on the smaller team - bigger team",
        ", ".join("%s worst %d total %d" % (tag(c), k[0], k[1]) for c, k in keyed), "->", tag(best))
    return best

def _pid_of(t):
    import hashlib
    return hashlib.sha256(t.encode()).hexdigest()[:12]

def teams_live():
    """Admin: the running matches' players by team: [{mid, map, teams: [[{pid, name, slot, moving}], [...]]}]."""
    out = []
    now = time.time()
    with MM.lock:
        for mid, pl in MM.matches.items():
            if mid not in MM.started or mid in MM.ended:
                continue
            dep = MM.departed.get(mid, ())
            teams = [[], []]
            for i, t in enumerate(pl):
                if t in dep or t.startswith("~") or not get_name(t):
                    continue
                teams[team_in(mid, i, t)].append({"pid": _pid_of(t), "name": get_name(t), "slot": i, "swapped": t in TEAM_SWAP.get(mid, {}),
                                                   "inmatch": now - TS_CAP.get(t, 0) < TS_CAP_S})    # its ext applies an in-match swap (else kick + rejoin)
            mv = [{"pid": _pid_of(t), "name": get_name(t) or "PLAYER", "team": v[1]} for t, v in MM.move_team.items() if v[0] == mid]
            out.append({"mid": mid, "teams": teams, "moving": mv})
    return out

def team_counts_in(mid, pl, dep):
    """Players in the match per team (gone players and host-migration placeholders do not count)."""
    n = [0, 0]
    for i, t in enumerate(pl):
        if t not in dep and not (isinstance(t, str) and t.startswith("f0" * 30)):
            n[team_in(mid, i, t)] += 1
    return n

def team_swap_now(mid, slot, tok, want, why):
    """Put `tok` (slot `slot` of the running match `mid`) on team `want` IN THE MATCH (its ext must apply the 0xE5 swap block: TS_CAP).
    Returns the team counts after the swap, None when the match is gone."""
    with MM.lock:
        if mid not in MM.matches or mid in MM.ended:
            return None
        cur = team_in(mid, slot, tok)
        TEAM_SWAP.setdefault(mid, {})[tok] = want
        while len(TEAM_SWAP) > 200:
            TEAM_SWAP.pop(next(iter(TEAM_SWAP)))
        pl = list(MM.matches.get(mid) or ())
        dep = set(MM.departed.get(mid, ()))
    ct = CLAN_TEAM.get(mid)
    if ct is not None and tok in ct:
        ct[tok] = want
    if mid in INT.matches:
        INT.roster(mid, pl, lambda i, _p=pl: team_in(mid, i, _p[i]))
    n = team_counts_in(mid, pl, dep)
    log("TEAMS swap (%s): %s (match %s slot %d) team %d -> %d IN THE MATCH (its ext applies it; teams now %d v %d)" % (why, tag(tok), mid, slot, cur, want, n[0], n[1]))
    evt("admin-move-team" if why == "admin" else "auto-balance", tag(tok), mid, "team %d -> %d in match (slot %d%s)" % (cur, want, slot, ", host" if slot == 0 else ""))
    return n

# Auto balance ("a rule that runs every 1.5 minutes in live matches, if theres a unbalance like 3v1 it should always balance it out.
# 2v1s, 3v2s, and 4v3s stay untouched"): every AUTO_BALANCE_S per running match, a difference of 2 or more players is evened out by in-match
# team swaps (team_swap_now: same slot, same life, new side) until it is at most 1. A difference of 1 is never touched. Who moves: only a player
# whose ext applies the swap (TS_CAP; an older ext is never kicked for balance), never the host, never a player whose party mate is on the
# bigger team, not one already moved this match if anyone else can go; of those the newest arrival (highest slot = joined last; TUNING: no
# per-player score on the server to pick by). Skipped while a joiner is loading into that match (it fills the small side itself), during a
# host migration, in the last AUTO_BALANCE_MIN_LEFT s (TUNING) and once the match is over. T2R_AUTO_BALANCE=0 = off.
AUTO_BALANCE = os.environ.get("T2R_AUTO_BALANCE", "1") != "0"
AUTO_BALANCE_S = float(os.environ.get("T2R_AUTO_BALANCE_S", "90"))
AUTO_BALANCE_MIN_LEFT = 30.0       # TUNING: no move this close to the end of the clock
AB_NEXT = {}                        # mid -> time of its next balance check

def auto_balance_tick():
    if not AUTO_BALANCE:
        return
    now = time.time()
    with MM.lock:
        live = [(mid, list(pl), set(MM.departed.get(mid, ())), MM.clock.get(mid), any(m == mid for m, _ in MM.joining.values()))
                for mid, pl in MM.matches.items() if mid in MM.started and mid not in MM.ended]
    for k in [k for k in AB_NEXT if k not in {m for m, *_ in live}]:
        AB_NEXT.pop(k, None)
    for mid, pl, dep, ck, joining in live:
        nxt = AB_NEXT.get(mid)
        if nxt is None:
            AB_NEXT[mid] = now + AUTO_BALANCE_S              # (first check 1.5 min after the match started / the daemon saw it)
            continue
        if now < nxt:
            continue
        AB_NEXT[mid] = now + AUTO_BALANCE_S
        n = team_counts_in(mid, pl, dep)
        if abs(n[0] - n[1]) < 2:
            continue
        why = None
        if joining:
            why = "a joiner is loading into it"
        elif ck and (ck[2] or (ck[1] > 0 and ck[0] - (now - ck[3]) < AUTO_BALANCE_MIN_LEFT)):
            why = "the match is ending"
        else:
            try:
                hi = HM.host_info(mid) if HM.enabled else None
                if hi and (hi["pending"] or hi["given_up"] or hi.get("alone")):
                    why = "host migration in progress"
            except Exception:
                pass
        if why:
            log("TEAMS auto-balance: match %s is %d v %d - not now (%s)" % (mid, n[0], n[1], why))
            continue
        try:
            hi = HM.host_info(mid) if HM.enabled else None
            hslot = hi["host"] if hi else 0
        except Exception:
            hslot = 0
        moved_any = False
        while abs(n[0] - n[1]) >= 2:
            big = 0 if n[0] > n[1] else 1
            sw = TEAM_SWAP.get(mid, {})
            cands = []
            for i, t in enumerate(pl):
                if t in dep or (isinstance(t, str) and t.startswith("f0" * 30)) or team_in(mid, i, t) != big:
                    continue
                if i == hslot or now - TS_CAP.get(t, 0) >= TS_CAP_S:
                    continue
                pid = PARTIES.pid(t)
                if pid and any(PARTIES.pid(u) == pid for j, u in enumerate(pl) if u != t and u not in dep and team_in(mid, j, u) == big):
                    continue
                cands.append((t in sw, -i, i, t))            # not moved before first, then the newest arrival (highest slot)
            if not cands:
                log("TEAMS auto-balance: match %s is %d v %d - nobody can move (host / older game version / party mates)" % (mid, n[0], n[1]))
                break
            _, _, i, t = min(cands)
            r = team_swap_now(mid, i, t, 1 - big, "auto-balance")
            if r is None:
                break
            n = r; moved_any = True
        if moved_any:
            log("TEAMS auto-balance: match %s now %d v %d" % (mid, n[0], n[1]))

def admin_move_team(pid):
    """Admin live team switch. IN THE MATCH when the
    player's t2r-ext applies it (see below: same slot, same life, new team on every screen; the host too). Fallback for an older ext: the player leaves its match
    (back to the menu, like a kick) and its next FIND MATCH puts it back into the same match on the OTHER team (join in progress, a free
 slot of that team's parity; the old slot stays as a gone placeholder). Valid for 15 minutes (live: 3 minutes expired before the player searched again and it went back to its old team)."""
    pid = str(pid or "").lower()
    if not (4 <= len(pid) <= 64) or any(ch not in "0123456789abcdef" for ch in pid):
        return {"ok": False, "error": "bad pid"}
    with MM.lock:
        hit = [(mid, i, t) for mid, pl in MM.matches.items() if mid in MM.started and mid not in MM.ended
               for i, t in enumerate(pl) if t not in MM.departed.get(mid, ()) and _pid_of(t).startswith(pid)]
    if len(hit) != 1:
        return {"ok": False, "error": "player is not in a running match" if not hit else "ambiguous pid"}
    mid, slot, tok = hit[0]
    now = time.time()
    cur = team_in(mid, slot, tok)
    want = 1 - cur
    # (3 of 4 players were moved, all 3 went to the menu, the host was left alone and quit): IN-MATCH swap when the mover's ext applies it
    # (its 0xE3 polls carry flag 0x10, every 2-3 s in a match): the player stays in the match - same slot, same P2P peer, same life - and only
    # its team changes on every screen (0xE5 roster + team-swap block, jip_roster_op). No leave, so no clan day is cut and the host is never
    # left alone. The host can be moved this way too. An older ext (no flag 0x10 within TS_CAP_S) falls back to the kick + rejoin below.
    cap_t = TS_CAP.get(tok, 0)
    if now - cap_t < TS_CAP_S:
        n = team_swap_now(mid, slot, tok, want, "admin")
        if n is None:
            return {"ok": False, "error": "the match just ended"}
        return {"ok": True, "name": get_name(tok) or "PLAYER", "to_team": want, "match": mid, "mode": "in-match",
                "note": "stays in the match on the other team (applied within ~3 s)"}
    if slot == 0:
        return {"ok": False, "error": "%s hosts this match and its game is too old to switch teams in the match (no t2r-ext team swap seen in the last %d s) - a host cannot be kicked to rejoin" % (get_name(tok) or "PLAYER", TS_CAP_S)}
    # the ticket and the clan hold go in BEFORE the kick - the kick's leave path (abort award, clan excuse) must already see a team move
    with MM.lock:
        MM.move_team[tok] = (mid, want, time.time() + 900)
    CLAN_MOVED[(tok, mid)] = time.time()
    r = admin_kick(pid, move=True)      # (the client reads the move text and searches again at once: join in progress on the other team)
    if not r.get("ok", True) and "error" in r:
        with MM.lock:
            MM.move_team.pop(tok, None)
        CLAN_MOVED.pop((tok, mid), None)
        return r
    log("TEAMS move: admin moves %s (match %s slot %d team %d) to team %d - its client searches again by itself (join in progress)" % (tag(tok), mid, slot, slot % 2, want))
    evt("admin-move-team", tag(tok), mid, "team %d -> %d" % (slot % 2, want))
    return {"ok": True, "name": get_name(tok) or "PLAYER", "to_team": want, "match": mid, "mode": "rejoin",
            "note": "older game version: sent to the menu, it re-joins this match on the other team by itself"}

def teams_preview():
    """Admin: the split the queue would get right now (names + rating), with the rule groups that apply."""
    q = MM.queued()
    if len(q) < 2:
        return {"queue": [get_name(t) or "PLAYER" for t in q], "t0": [], "t1": []}
    sk = player_skill(q)
    t0, t1 = split_teams(q, {t: PARTIES.pid(t) for t in q if PARTIES.pid(t)}, sk, top_players(q), rule_groups(q))
    nm = lambda t: "%s (%.2f)" % (get_name(t) or "PLAYER", sk.get(t, SKILL_NEW))
    return {"queue": [get_name(t) or "PLAYER" for t in q], "t0": [nm(t) for t in t0], "t1": [nm(t) for t in t1]}

PARTIES = social.Parties(max_size=max(1, MATCH_SIZE // 2), lookup=lambda name: token_by_name(name))    # a party must fit one team (tools/social.py)

_tid_sid_cache = {}                       # install id -> (time, Steam id or 0)
TID_SID_TTL = 30.0

def install_steamid(token):
    """Steam id of a token the daemon has not linked yet, from the other tokens of its install. The launcher writes a new token when the
    player changes name (T2N + the new name + the same install id), and its FRIENDS poll can arrive before the game links that token: the
 friends page then showed the empty "t:" install account, and requests sent from it put the player on friends' lists twice.
    Only when every linked token of that install id has the same Steam id: install ids are not unique (a shipped token was shared by
 5 players), and an id seen with 2+ Steam accounts never resolves."""
    tid = names.token_id(token)
    if len(tid) != 24:
        return 0
    now = time.time()
    hit = _tid_sid_cache.get(tid)
    if hit and now - hit[0] < TID_SID_TTL:
        return hit[1]
    rows = db_read(lambda c: c.execute("SELECT DISTINCT steamid FROM players WHERE substr(lower(token),41,24)=? AND steamid>0 LIMIT 2",
                                       (tid,)).fetchall())
    sid = int(rows[0][0]) if len(rows) == 1 else 0
    if len(_tid_sid_cache) > 5000:
        _tid_sid_cache.clear()
    _tid_sid_cache[tid] = (now, sid)
    return sid

class _SocialHost:
    """What tools/social.py needs from the daemon."""
    now = staticmethod(time.time)
    log = staticmethod(log)
    db = staticmethod(dbc)               # (no _dblock: steamid / name below take it, and it is not re-entrant; social commits every write at once)
    @staticmethod
    def steamid(token):
        return token_steamid(token) or BRIDGES.steamid(token) or install_steamid(token) or 0
    @staticmethod
    def name(token):
        return get_name(token)
    @staticmethod
    def game_tokens():
        return PRES_TOKENS()
    @staticmethod
    def alive_tokens():
        alive = set(PRES_TOKENS())
        try:
            alive.update(MM.queue)
            for pl in list(MM.matches.values()):
                alive.update(pl)
        except RuntimeError:
            pass
        return alive
    @staticmethod
    def mm_state(token):
        # (no MM.lock: try_form holds it while it takes the database lock, this runs inside a database transaction - plain dict reads)
        try:
            for mid, pl in list(MM.matches.items()):
                if token in pl and token not in MM.departed.get(mid, ()) and mid not in MM.ended:
                    return "match"
            return "search" if token in MM.queue and time.time() - MM.seen.get(token, 0) < MM.QUEUE_TTL else None
        except RuntimeError:
            return None

SOCIAL = social.Social(_SocialHost, PARTIES)
def _lobby_chat_key(tok):
    """The LOBBY chat channel of a token: every searcher shares one room, the players the search page lists (searching_frame). It was the
    requeue group (the last match's players): a searcher from the queue, the backfill of a lobby, never saw those lines nor reached them."""
    if not tok:
        return None
    if book_on():
        try:
            with MM.lock:
                lid = MM.book.lobby_of(tok)
                if lid:
                    return lid
                m = MM.rq.mid_of(tok) if (REQUEUE and LOBBY_KEEP) else None
                if m:
                    return MM.book.key_for_mid(m)                  # on the post-match screens: the room of the lobby that played that match
                cm = MM._mid_locked(tok)
                return MM.book.mid_lid.get(cm) if cm else None   # countdown / in the match: the room of the lobby that formed it
        except Exception:                          # noqa: BLE001 - the chat never breaks the queue
            return None
    try:
        with MM.lock:
            if tok in MM.queue:
                return "queue"
    except Exception:                              # noqa: BLE001 - the chat never breaks the queue
        return None
    return MM.rq.mid_of(tok) if (REQUEUE and LOBBY_KEEP) else None
SOCIAL.lobby_of = _lobby_chat_key

def party_op(pl):
    """Op 0xDA -> reply frame (0xDB)."""
    if len(pl) < 65:
        return None
    token = pl[:64].decode("latin1", "ignore")
    if not all(ch in "0123456789abcdefABCDEF" for ch in token):
        return None
    sub, arg = pl[64], pl[65:]
    ok = True
    if sub == 1: ok = PARTIES.invite(token, arg)
    elif sub == 2: ok = PARTIES.accept(token, arg)
    elif sub == 3: PARTIES.leave(token)
    SOCIAL._drain()
    return msg(0xDB, ("ok=%d %s" % (1 if ok else 0, PARTIES.text(token, get_name))).encode())

def social_op(pl, addr=None):
    """Op 0xD8 [64B token][u8 ver=1][u8 lang 0 en / 1 pl][u8 sub][UTF-8 argument] -> 0xD9 JSON (tools/social.py). A write from a token the database never
    saw counts against its address like the other row-creating ops (new_identity_ok)."""
    if len(pl) < 67:
        return None
    token = pl[:64].decode("latin1", "ignore")
    if not is_token(token) or pl[64] != 1:
        return None
    lang, sub = pl[65], pl[66]
    arg = pl[67:67 + 420].decode("utf-8", "ignore")      # 420: 200 characters of in-game chat (Polish letters are 2 bytes) + a short prefix
    if sub in social.WRITE_SUBS and not new_identity_ok(token, addr):
        log("NEWID", addr, "op 0xD8 refused: too many new identities from this address")
        return msg(social.OP_REPLY, social.reply_json({"ok": 0, "err": "rate"}))
    return msg(social.OP_REPLY, social.reply_json(SOCIAL.handle(token, 1 if lang == 1 else 0, sub, arg)))

def PRES_TOKENS():
    now = time.time()
    with PRES.lock:
        return [t for t, p in PRES.players.items() if now - p.get("last", 0) < KEEPALIVE_TTL]

# ---------------------------------------------------------------- join in progress (T2R_JIP=1)
# re/join-in-progress.md. MM.jip_join picks the match and the slot; this is everything around it: the Steam helpers' peer table, the
# joiner's own 0x04 record + shortened countdown (map vote skipped, the map is fixed), the host-migration record, and the roster refresh
# op 0xE3 the t2r-ext of the players already in the match polls (it adds the joiner to their lobby-object roster, which winmm reads for
# the team of a slot: SlotTeam(slot) on the HOST decides the new player's team and the host's roster message tells every client).
_JIP_LAST_WHY = {}

def jip_push_peers(mid, players):
    """Steam P2P: the full slot table (0xF2) to EVERY active helper of the match with its own slot, the joiner's included. The helper
    routes gameplay by steamid -> slot, so the host's helper must know the joiner before the joiner's Hello (T2RO type 1) arrives.
    Same sid as the running players hold (match_sid -> SID_KEEP). The helper memsets its match state on 0xF2 and rebuilds it from
    the table (only its 'peer seen' flags restart, which re-sends a punch packet)."""
    if not MATCH_P2P.get(frozenset(players)):
        return
    with MM.lock:
        dep = set(MM.departed.get(mid, ()))
    live = [(i, t) for i, t in enumerate(players) if t not in dep]
    sid = match_sid(players)
    table = bytes([len(live)]) + b"".join(bytes([i]) + BRIDGES.steamid(t).to_bytes(8, "little") for i, t in live)
    for i, tok in live:
        try:
            BRIDGES.conn(tok).sendall(msg(0xF2, sid.to_bytes(4, "little") + bytes([i]) + table))
        except Exception as e:
            log("jip: PUSH-PEER ERR", tag(tok), e)

JIP_BACK = {}      # token -> (match id, time): its join in progress put it back into the match it had left (wave_byte bit 2)
JIP_LATE = {}      # token -> (match id, time): joined a running match (join in progress); answered to the joiner's own ext (0xE3 flag 0x04) so it can pick the safest intro point

def clan_rejoin_after_booking(token, mid):
    """The player joins (join in progress) the running match `mid`; if its day for `mid` is already booked this opens a second stint."""
    if CLAN_DONE.get(token) != mid:
        try:                                                         # (after a daemon restart the in-memory done mark is gone: the persisted snapshot still knows)
            with _dblock, dbc() as c:
                clan.snap_update(c, clan_key(c, token), mid, rejoined=True)
        except Exception:                                            # noqa: BLE001
            pass
        return
    # ("No word from the camp"): the player left this match (day booked 20 s later), joined it again and played on. Its second report was
    # answered "re-sent for an already booked match" and clan.apply_match refused the same mid: the post-match page waited 45 s for a day that never came.
    # The way back in opens a second stint: the done mark, the credit and the leave verdicts of the first one are void.
    CLAN_DONE.pop(token, None)
    CLAN_CREDITED.discard((token, mid))
    for _s in (CLAN_EXCUSED, CLAN_QUITTERS):
        _s.discard((token, mid))
    for _m in (CLAN_EXCUSE_WHY, CLAN_LEFT_AT, CLAN_REJOINED, CLAN_MOVED):
        _m.pop((token, mid), None)
    CLAN_REBOOK.add((token, mid))
    try:
        with _dblock, dbc() as c:
            clan.snap_update(c, clan_key(c, token), mid, rejoined=True)
    except Exception as ex:                                          # noqa: BLE001
        log("CLAN rejoin mark failed", repr(ex))
    log("CLAN rejoin", tag(token), "mid", mid, "after its day was booked - its next report replaces that booking (one day per match)")

def jip_start(token, mid, slot, players):
    """The searching `token` was put into the running match `mid` at `slot` (MM.jip_join): tell everything and start its client."""
    # ("join in progress is problematic when you back out at the moment a match is found"): jip_pass picks the match under the matchmaker
    # lock but starts it here, outside it. A back-out (op 0x02 -> leave_running_match -> MM.depart) that lands in between has already removed the
    # joiner from MM.joining: START-MATCH then went to a client that had left (it loaded a level of a match the server had dropped it from), and
    # HM.add_player put a ghost in the host-migration table that nothing ever marked gone. Check first, and again after the sends.
    if token not in MM.jip_joiners(mid):
        log("jip:", tag(token), "left before its START-MATCH for", mid, "went out - not started")
        evt("jip-aborted", mid, tag(token), "backed out at the moment the match was found")
        return False
    log("jip:", tag(token), "-> running match", mid, "slot", slot, "team", team_of(slot), "players", [tag(p) for p in players])
    JIP_LATE[token] = (mid, time.time())
    clan_rejoin_after_booking(token, mid)
    evt("jip-join", mid, tag(token), "slot", slot, "team", team_of(slot), "players", [tag(p) for p in players])
    if HM.enabled:
        HM.add_player(mid, token, slot)
    jip_push_peers(mid, players)
    LOADED.discard(token); CD_GOT.discard(token)
    conn = SESSIONS.conn(token)
    if conn:
        try:
            conn.sendall(match_start_frame(token, players))
            log("jip: START-MATCH", tag(token), "sent on session conn")
        except Exception as e:
            log("jip: START-MATCH ERR", tag(token), e)
    if token not in MM.jip_joiners(mid):                           # backed out while the frames above went out: the leave ran before HM.add_player
        if HM.enabled:
            HM.depart(token, "left (backed out at match found)")
        JIP_LATE.pop(token, None)
        log("jip:", tag(token), "backed out while START-MATCH went out - slot freed again")
        evt("jip-aborted", mid, tag(token), "backed out while its START-MATCH was sent")
        return False
    threading.Thread(target=jip_countdown, args=(token, mid), daemon=True).start()
    return True

def jip_countdown(token, mid):
    """The joiner's own countdown on its conn: the 2013 stages after the lobby (loadout, booster, horn) but no map vote - the map is the
    running match's. Level start (2, 0) -> the client loads -> op 0x09 -> maybe_game_start sends it 0x0a alone."""
    try:
        time.sleep(2.0)                                           # let the 0x04 land first (as _countdown_run)
        pre = int(os.environ.get("T2R_JIP_PRE_COUNTDOWN", "2"))
        secs = int(os.environ.get("T2R_COUNTDOWN", "10"))
        for phase, n in countdown_steps(pre, secs, False):
            if token not in MM.jip_joiners(mid):
                log("jip: countdown aborted (left)", tag(token))
                return
            c = SESSIONS.conn(token)
            if c:
                try:
                    push(c, msg(0x0c, bytes([phase, n])), drop=True)
                    if phase == 2 and n == 0: CD_GOT.add(token)
                except OSError: pass
            time.sleep(1.0)
        log("jip: countdown done", tag(token))
    except Exception as e:
        log("jip: COUNTDOWN-ERR", repr(e), tag(token))

def jip_pass():
    """Offer the running matches to the players searching right now (called after every queue change and by the matchmaker loop; a
    match the queue could form is only formed when no running match with room and time left (JIP_PREFER_LEFT) can take the players).
    A party joins together: its searching members one after the other into the same match and team (MM.jip_join need / only_mid); a member
    whose friends already play in a running match goes to the friends' team."""
    if not JIP:
        return
    ready = MM.jip_ready()
    done = set()
    for token in ready:
        if token in done:
            continue
        mates = [m for m in PARTIES.members(token) if m != token and m in ready and m not in done] if PARTIES.pid(token) else []
        why = []
        res = None
        nm = MM.rq.next_of(token) if (LOBBY_KEEP and REQUEUE) else None
        if nm and nm in MM.matches and nm not in MM.ended:
            # (persistent lobby) back from the post-match screens: into the match its lobby formed meanwhile (its seat was kept)
            res = MM.jip_join(token, why, need=1 + len(mates), only_mid=nm)
            if res:
                log("LOBBY:", tag(token), "back from the post-match screens - joins its lobby's match", nm)
            elif nm not in MM.started:
                continue                                       # its countdown: join once GAME-START went out
        if not res:
            res = MM.jip_join(token, why, need=1 + len(mates))
        if not res:
            w = "; ".join(why)
            wk = "".join(ch for ch in w if not ch.isdigit())          # (the seconds left change every second: log a reason once, not 1 line/s - PT55 had 160)
            if w and _JIP_LAST_WHY.get(token) != wk:
                _JIP_LAST_WHY[token] = wk
                log("jip:", tag(token), "no running match to join:", w)
            continue
        _JIP_LAST_WHY.pop(token, None)
        mid, slot, players, _ = res
        jip_start(token, mid, slot, players)
        done.add(token)
        for m in mates:
            why2 = []
            r2 = MM.jip_join(m, why2, only_mid=mid, team=team_of(slot))
            if r2:
                _JIP_LAST_WHY.pop(m, None)
                jip_start(m, r2[0], r2[1], r2[2])
                done.add(m)
            else:
                log("jip: party mate", tag(m), "could not follow", tag(token), "into", mid, "yet:", "; ".join(why2))

JIP_STARTED = {}        # token -> (mid, time of its GAME-START) of a joiner, kept a few minutes
JIP_REPUSH = {}         # token -> (last re-push time, count)
JIP_REPUSH_S = float(os.environ.get("T2R_JIP_REPUSH_S", "15"))
JIP_REPUSH_MAX = int(os.environ.get("T2R_JIP_REPUSH_MAX", "8"))

def jip_unreachable(token, now=None):
    """A join-in-progress player's ext reports the host silent (op 0xE6 flag) shortly after its GAME-START: its Steam punch to the host did
    not land (PT55 01:29:59-01:34:21: the rejoined player stared at 'host silent' for 4.5 min while the others played fine, and only a manual
    leave + search fixed it). Re-send the full slot table (0xF2) to every active helper of the match: the helpers restart their 'peer seen'
    flags = a new punch packet to every peer. Every JIP_REPUSH_S s, at most JIP_REPUSH_MAX times per join. Returns True when it re-pushed."""
    now = time.time() if now is None else now
    st = JIP_STARTED.get(token)
    if not st or now - st[1] > 300:
        return False
    last, n = JIP_REPUSH.get(token, (0.0, 0))
    if n >= JIP_REPUSH_MAX or now - last < JIP_REPUSH_S:
        return False
    players = MM.players_of(token)
    if not players or MM.mid_of(token) != st[0]:
        return False
    JIP_REPUSH[token] = (now, n + 1)
    log("jip: joiner", tag(token), "reports the host silent %.0f s after its start: re-pushing the peer table (%d)" % (now - st[1], n + 1))
    evt("jip-repush", st[0], tag(token), n + 1)
    jip_push_peers(st[0], players)
    return True

def wave_byte(late=False, back=False, mid=None):
    """Trailing byte of the 0xE5 reply: 0x80 = 'present', bit0 = wave respawn on, bit1 = the asker joined a running match (only answered when the
    request carried flag 0x04, see jip_roster_op) (one byte only: an older ext's reply buffer holds a full 8-player roster + exactly one more byte)."""
    # test server: 0x08 = every player may use the free camera (T2R_TEST_FREECAM=1), 0x10 = Supply Raid clock x2 (T2R_TEST_CLOCK2=1), 0x20 = team lives x2 (T2R_TEST_LIVES2=1).
    # A live server leaves both unset; an older ext ignores the bits.
    test = (8 if os.environ.get("T2R_TEST_FREECAM", "") == "1" else 0) | (0x10 if os.environ.get("T2R_TEST_CLOCK2", "") == "1" else 0) | (0x20 if os.environ.get("T2R_TEST_LIVES2", "") == "1" else 0)
    # 0x40 = no AFK removal on this server (T2R_AFK_KICK_S=0, the test server): the ext drops its inactivity warning too
    test |= 0x40 if AFK_KICK_S <= 0 else 0
    wave = WAVE_RESPAWN
    pr = PRIV.rules_of_mid(mid) if mid is not None else None
    if pr is not None:                                     # private match: RESPAWN TIMER 0 = waves, else a fixed wait (the ext reads it from 0xF9 RULES); AFK REMOVAL off
        wave = pr.get("resp", 0) == 0
        test |= 0x40 if not pr.get("afk", 0) else 0
    return 0x80 | (1 if wave else 0) | (2 if late else 0) | (4 if late and back else 0) | test

def jip_is_late(token):
    """True when `token` was put into its CURRENT match by join in progress (JIP_LATE is stamped by jip_start; a later match has another id)."""
    j = JIP_LATE.get(token)
    if not j:
        return False
    if time.time() - j[1] > 7200 or MM.mid_of(token) != j[0]:
        JIP_LATE.pop(token, None)
        return False
    return True

def jip_roster_op(pl):
    """Op 0xE3 [64B token][u8 flags][f32 seconds left][f32 duration] from the t2r-ext of a player in a running match (flags bit0 = the
    clock fields are valid, bit1 = the match is over) -> 0xE5 [u8 enabled][u8 n][n x 0x32 roster entries: [0] slot, [1..0x30] name,
    [0x31] team] (the same layout as the 0x04 record's roster) [u8 wave_byte: 0x80 | wave respawn on]. Every poll is also proof of life (PRES) and the clock report the matchmaker
    needs to offer the match. enabled 0 = join in progress is off on this server: the ext stops asking for the rest of the match."""
    if len(pl) < 64:
        return None
    t = pl[0:64].decode("latin1", "ignore")
    readopt_claim(t)
    if len(pl) > 64 and pl[64] & 0x20:
        RQ_CAP[t] = time.time()                          # this ext searches again by itself after the match (requeue.c)
    if len(pl) >= 80 and pl[76] == 0xAF:                 # [0xAF][u16 idle s][flags] = the ext's anti-AFK count (afk.c)
        try: afk_report(t, pl[77] | (pl[78] << 8), pl[79])
        except Exception as e: log("afk: ERR", repr(e))
    if not JIP:
        return msg(0xE5, bytes([0, 0, wave_byte()]))
    flags = pl[64] if len(pl) > 64 else 0
    rem, dur = struct.unpack_from("<ff", pl, 65) if len(pl) >= 73 else (0.0, 0.0)
    if flags & 0x10:
        TS_CAP[t] = time.time()                          # this ext applies the in-match team-swap block below
    PRES.touch_known(t)
    if flags & 1 or flags & 2:
        MM.jip_clock(t, rem if flags & 1 else 0.0, dur if flags & 1 else 0.0, bool(flags & 2))
    if len(pl) >= 76:                                    # [my team][ally lives][enemy lives] (newer ext): the late-join bonus
        MM.jip_lives(t, pl[73], pl[74], pl[75])
    players = MM.players_of(t) or []
    mid = MM.mid_of(t)
    gone = set(MM.departed.get(mid, ())) if mid is not None else set()
    body = bytearray()
    n = 0
    for i, tok in enumerate(players[:8]):
        if (tok in gone and tok != t) or (isinstance(tok, str) and tok.startswith("f0" * 30)):
            continue        # (a host-migration placeholder is nobody, 'PLAYER 6') PT54: a timed-out / left player stayed in the roster - team figures 2v3 while the scoreboard said 2v2
        n += 1
        e = bytearray(0x32)
        e[0] = i
        nm = (get_name(tok) or f"PLAYER {i + 1}").encode("latin1", "ignore")[:0x2e]
        e[1:1 + len(nm)] = nm
        e[0x31] = roster_team_in(mid, i, tok)
        body += e
    late = bool(flags & 4) and jip_is_late(t)       # flag 0x04: the ext asks 'did I join a running match?' during its own match load (late joiner intro point)
    jb = JIP_BACK.get(t)
    back = bool(late and jb and jb[0] == mid and time.time() - jb[1] < 600)   # bit 2: back into the match it left - the ext gives back the kit it had
    tail = b""
    if flags & 0x10 and mid is not None:
        # in-match team swap: [0xA7][k][k x (slot, team)] after the wave byte, ONLY for an ext that asked with flag 0x10 (an older ext's
        # reply buffer is exactly 8 roster entries + 1 byte: a longer reply would be dropped whole). Lists every slot whose token the admin
        # swapped this match (its current team, also when moved back). The roster entries above already carry the new team for every client
        # (older exts apply other slots' teams from them too); the block is what makes the MOVER apply its own team (an ext never takes its own
        # slot from the roster) and lets every screen show who switched.
        sw = TEAM_SWAP.get(mid) or {}
        items = [(i, roster_team_in(mid, i, tok)) for i, tok in enumerate(players[:8]) if tok in sw and tok not in gone]
        if items:
            tail = bytes([0xA7, len(items)]) + b"".join(bytes([i, tm]) for i, tm in items)
    return msg(0xE5, bytes([1, n]) + bytes(body) + bytes([wave_byte(late, back, mid)]) + tail)

def matchmaker_loop():
    """Forms matches that start on the fill timer (no new find-match press needed)."""
    while True:
        time.sleep(1.0)
        mid = players = None
        try:    # an exception used to end this thread for good: no timer-formed match until the server restarted
            shutdown_tick()
            try: SOCIAL.tick()
            except Exception as e: log("SOCIAL tick ERR", repr(e))
            try: PRIV.gc(lambda t: MM.in_match(t) or t in MM.queue)
            except Exception as e: log("PRIVATE gc ERR", repr(e))
            mid, players = MM.try_form()
            if mid:
                push_peers(players)
                start_match(players)
            else:
                jip_pass()                                  # nothing to form: the searchers may join a running match
            try: auto_balance_tick()                        # 3 v 1 -> 2 v 2 in the match, every AUTO_BALANCE_S
            except Exception as e: log("TEAMS auto-balance ERR", repr(e))
        except Exception as e:
            log("MATCHMAKER-ERR", repr(e))
            if mid:                                  # formed but never started: release the players, they queue again
                for p in players:
                    MM.leave(p)

MM = MatchMaker()

# Host migration (ON by default since, T2R_HOST_MIGRATION=0 = off): see tools/hostmig.py and re/host-migration.md
HM = hostmig.HostMigration(enabled=os.environ.get("T2R_HOST_MIGRATION", "1") == "1", log=log, now=time.time,
                           ping=lambda a, b: pair_ping(a, b), perf=lambda t: perf_penalty(t), pool=lambda ts: host_pool(ts),
                           link_alive=lambda t: SESSIONS.conn(t) is not None)     # a host still connected is not replaced on a short hitch

# Planned host re-pick (T2R_HOST_REPICK: 0 off, 1 check + log only (default), 2 live handover;): see hostmig.HostRepick
REPICK = hostmig.HostRepick(log=log, now=time.time)

def hostcap_repick_log(mid, players, alive, hslot, scores, act, what):
    """Log-only: what the capacity history says about the re-pick (the current host, a migration target, a better-proven candidate)."""
    n = len(alive)
    cur = HOSTCAP.verdict(players[hslot], n)
    cand = {s: HOSTCAP.verdict(players[s], n) for s in scores if s != hslot and scores.get(s) is not None}
    cur_k = scores.get(hslot)
    near = [s for s in cand if cur_k is None or scores[s][0] - cur_k[0] <= hostcap.WINDOW_MS]
    better = [s for s in near if cand[s][0] < cur[0]]
    if act == "migrate" and what in cand and cand[what][0] >= 2:
        verd = "would BLOCK the migration: target %s %s" % (tag(players[what]), cand[what][1])
    elif act == "migrate" and what in cand and cand[what][0] < cur[0]:
        verd = "agrees: target %s %s" % (tag(players[what]), cand[what][1])
    elif cur[0] >= 2 and better:
        verd = "would favour %s (%s) over the lagging host" % (tag(players[min(better, key=lambda s: (cand[s][0], scores[s]))]), cand[min(better, key=lambda s: (cand[s][0], scores[s]))][1])
    else:
        verd = "no change"
    log("HOST-REPICK", mid, "cap: host %s %s; %s" % (tag(players[hslot]), cur[1], verd))

SAT_TARGET_RANK = int(os.environ.get("T2R_HOST_SAT_TARGET_RANK", "1"))   # highest hostcap rank a saturated host may hand over to (0 = proven only, the old rule)

def repick_cap(mid, players, alive, hslot):
    """Capacity override for REPICK.check (hostmig REPICK_CAP_CONFIRM) or None: the host LAGGED below the upload this lobby needs in THIS
    match (hostcap verdict rank 2 + a lag charged in this match), or its link is saturated (every ping to it >= 3x its match baseline) and it
    never proved enough upload. Targets: rank < 2 (proven first) after a lag; on saturation (proven host or not) rank <= SAT_TARGET_RANK, proven first."""
    if hostcap.MODE < 1:
        return None
    n = len(alive)
    h = players[hslot]
    sat, why = REPICK.saturated(mid, h, [players[o] for o in alive if o != hslot], pair_ping)
    if n < 3 and not sat:
        return None                                    # (2 players: every status pass - no db reads unless the link looks saturated)
    vs = {s: HOSTCAP.verdict(players[s], max(n, 2)) for s in alive}
    bad, max_rank = None, 0
    if n >= 3 and vs[hslot][0] >= 2 and HOSTCAP.lagged_in(mid, h):
        bad, max_rank = "%s lagged in this match (%s)" % (tag(h), vs[hslot][1]), 1
    elif sat:
        # A host that PROVED its upload can still saturate (every ping to it several times its baseline for minutes); skipping saturation for a
        # proven host left the lobby lagging until the host left. A saturated link now moves: a proven target first, else one that never lagged
        # at this lobby size (rank 1, SAT_TARGET_RANK), never a lagged one (rank 2).
        bad, max_rank = "%s link saturated (%s; %s)" % (tag(h), why, vs[hslot][1]), SAT_TARGET_RANK
    if not bad:
        if hostcap.MODE >= 2 and n >= hostmig.REPICK_PROVEN_N > 0:
            # no capacity problem, but a ping-only handover in a big lobby may only go to a proven target (hostmig REPICK_PROVEN_N)
            prov = {s: HOSTCAP.proven_for(players[s], n) for s in alive}
            return {"host_bad": None, "rank": {s: 0 if prov[s] else 1 for s in alive}, "max_rank": 0,
                    "text": {s: ("proven for %d players" % n) if prov[s] else "not proven for %d players (needs ~%d KB/s: %s)" % (n, hostcap.need_kb_relay(n), vs[s][1]) for s in alive}}
        return None
    if hostcap.MODE < 2:
        log("HOST-REPICK", mid, "cap:", bad, "- override off (T2R_HOST_CAP=1, log only)")
        return None
    return {"host_bad": bad, "rank": {s: v[0] for s, v in vs.items()}, "text": {s: v[1] for s, v in vs.items()}, "max_rank": max_rank,
            "saturated": bool(sat)}

def repick_tick():
    """Every REPICK_INTERVAL s per running match: score every player as host on the live measured pings and, when the best is clearly
    better than the current host, log 'would migrate' (mode 1) or start a planned handover (mode 2). A host whose upload is not enough
    (repick_cap) is replaced even when the ping rule alone would hold; with 2 players only then."""
    if not HM.enabled or REPICK.mode <= 0:
        return
    now = time.time()
    for mid in HM.mids():
        snap = HM.snapshot(mid)
        if not snap:
            REPICK.forget(mid)
            continue
        t0, players, alive, hslot = snap
        try:                                           # every pass (10 s): each link's usual ping in this match (saturation baseline)
            REPICK.observe(mid, {(players[a], players[b]): pair_ping(players[a], players[b]) for a in alive for b in alive if a < b})
        except Exception as e:
            log("HOST-REPICK observe ERR", repr(e))
        if len(alive) < 2 or hslot not in alive or not REPICK.due(mid, t0):
            continue
        cap = None
        try:
            cap = repick_cap(mid, players, alive, hslot)
        except Exception as e:
            log("HOST-REPICK cap ERR", repr(e))
        if len(alive) < 3 and not cap:
            continue                                   # 2 players: both directions are the same link - only a saturated host moves
        stale, loading = HM.stale_slots(mid)
        if stale:
            log("HOST-REPICK", mid, "not polling host migration:", ", ".join("slot %d %s" % (s, tag(players[s])) for s in sorted(stale)),
                "- not a candidate, scored as a peer with its known pings")
        toks = [players[s] for s in alive]
        pool = host_pool(toks)[0]
        scores = {}
        for s in alive:
            t = players[s]
            if (t not in pool or s in stale) and s != hslot:
                continue
            pings = [pair_ping(t, players[o]) for o in alive if o != s]
            pings = [v for o, v in zip([o for o in alive if o != s], pings) if not (v is None and o in stale)]   # a stale peer's unknown ping is skipped
            scores[s] = None if (not pings or None in pings) else hostmig.host_key(pings, perf_penalty(t))
        with MM.lock:
            ck = MM.clock.get(mid)
        left = None
        if ck and not ck[2] and now - ck[3] < 20:
            left = max(0, ck[0] - (now - ck[3]))
        busy = None
        if HM.handover_pending(mid):
            busy = "a handover is already pending"
        elif REPICK.mode >= 2:
            ok, why = HM.handover_ok(mid)
            busy = None if ok else why
        else:
            hi = HM.host_info(mid)
            if hi and (hi["pending"] or hi["given_up"]):
                busy = "migration pending"
        labels = {s: tag(players[s]) for s in alive}
        info = {"rtt": {(a, b): pair_ping(players[a], players[b]) for a in alive for b in alive if a < b},
                "perf": {s: perf_penalty(players[s]) for s in alive}, "age": max([now - PING_AT.get((players[a], players[b]), now) for a in alive for b in alive if a != b] or [0])}
        act, what = REPICK.check(mid, t0, frozenset(alive), hslot, scores, left, busy, labels, info, cap)
        if hostcap.MODE >= 1:
            try:
                hostcap_repick_log(mid, players, alive, hslot, scores, act, what)
            except Exception as e:
                log("HOST-REPICK cap ERR", repr(e))
        if act == "migrate" and REPICK.mode >= 2:
            if HM.handover(mid, what):
                evt("host-repick", mid, "handover to", tag(players[what]))
                REPICK.migrated(mid, frozenset(alive))
    live = set(HM.mids())                                 # a match that left HM (ended / reaped) is never visited above again: its records went on growing
    for mid in set([m for m in list(REPICK.recs) if m not in live] + [m for m in list(REPICK.last_text) if m not in live] + [m for m in list(REPICK.base) if m not in live]):
        REPICK.forget(mid)

# Interrogation mode (T2R_MODE): see tools/interrogation.py and re/interrogation.md
INT = interrogation.Interrogation(log=log, now=time.time)

# Per-team gun limits: the daemon arbitrates which teammate holds a limited gun - see tools/gunlimit.py. T2R_GUN_LIMITS=0 = off
# (every claim granted, nothing ever 'full').
GL = gunlimit.GunLimits(log=log, now=time.time)
GL_ON = os.environ.get("T2R_GUN_LIMITS", "1") != "0"

def gunlimit_op(pl):
    """Op 0xDE -> reply frame (0xDF). The token's live match + team decide the books; no live match = on=0."""
    req = GL.parse_request(pl)
    if not req:
        return None
    token, mask = req
    readopt_claim(token)
    if not GL_ON:
        return msg(gunlimit.OP_REPLY, b"on=0")
    mid, active = MM.active_of(token)
    players = MM.players_of_mid(mid) if mid else None
    if not mid or not players or token not in players:
        GL.release(token)
        return msg(gunlimit.OP_REPLY, b"on=0")
    if PRIV.rule(mid, "glim") == 0:
        GL.release(token)
        return msg(gunlimit.OP_REPLY, b"on=0")          # private match with GUN LIMITS off
    GL.gc({m[0] for m in MM.snapshot()})
    team = roster_team_in(mid, players.index(token), token)
    pre = mid not in MM.started             # match formed, game start not sent yet: ranked + settled pre-match decisions (tools/gunlimit.py)
    grant, deny, full, held, pend = GL.claim(mid, team, token, mask, set(active), pre=pre, slot=players.index(token), with_pend=True)
    return msg(gunlimit.OP_REPLY, GL.text(1, team, held, grant, deny, full, pend).encode())

# ---------------------------------------------------------------- server notices (tools/notices.py)
NOTICES = notices.Notices(now=time.time)
FINAL_WAIT_S = float(os.environ.get("T2R_FINAL_WAIT_S", "6"))     # after the final 'shutting down' notice: clients poll every ~6 s
EXIT_FN = os._exit                                                # replaced by tests
_target = {"t": 0.0, "v": ""}
def update_target():
    """The build a player should be on: the stable channel of the update server's data dir (next to the database), else this server's build."""
    now = time.time()
    if now - _target["t"] > 30:
        v = ""
        try:
            ud = os.path.join(os.path.dirname(os.path.abspath(DB_PATH)), "update-data")
            with open(os.path.join(ud, "channels.json"), encoding="utf-8") as f:
                ch = str(json.load(f).get("stable") or "")
            # the channel's manifest NAME ("76") is not the build stamp the game reports (version.txt, "0f44cc94"): every player got
            # "update available" in every match. The target is the version.txt the published manifest ships.
            if ch:
                with open(os.path.join(ud, "manifests", "stable", ch + ".json"), encoding="utf-8") as f:
                    vt = [e for e in json.load(f).get("files", []) if e.get("path") == "version.txt"]
                if vt and re.fullmatch(r"[0-9a-f]{64}", vt[0].get("sha256", "")):
                    with open(os.path.join(ud, "blobs", vt[0]["sha256"]), encoding="utf-8") as f:
                        v = f.read().strip()
        except Exception:
            pass
        _target.update(t=now, v=v or SERVER_VERSION)
    return _target["v"]

def notice_items(token, lang=None):
    lang = lang or lang_of(token)
    cv = CLIENT_VERSIONS.get(token)
    tv = update_target()
    refused = bool(cv and SERVER_VERSION and cv != SERVER_VERSION and version_refusal(token))   # not in a match: its searches fail -> the 'v' notice
    return NOTICES.active(bool(cv and tv and cv != tv) or (bool(cv and SERVER_VERSION and cv != SERVER_VERSION) and not refused), refused, lang)

def notice_op(pl):
    """Op 0xBA [64B token][u8 ver][u8 lang][u8 flags: 1 = the game has the in-game FRIENDS panel, optional] -> 0xBB (notices.Notices.reply_text), or None for a bad payload."""
    if len(pl) < 64:
        return None
    token = pl[:64].decode("latin1", "ignore")
    if not is_token(token):
        return None
    readopt_claim(token)
    PRES.touch_known(token)
    if len(pl) > 65:
        note_lang(token, pl[65])                 # the game's TEXT language / the launcher's UI language, whoever polled last
    items = notice_items(token)
    try:
        items = SOCIAL.notes_for(token, pl[65] if len(pl) > 65 else 0, len(pl) > 66 and pl[66] == 1)[:2] + list(items)[:notices.MAX_LINES - 2]     # party / friend lines first (kind p)
    except Exception as e:
        log("SOCIAL notes failed", repr(e))
    return msg(notices.OP_REPLY, notices.Notices.reply_text(items))

def notice_text(kind, param, text, lang="en"):
    """One notice in one language (the op 0x11 note form of the 0xBB kinds; the in-game toast renders the same sentences itself, tools/ext/notice.c).
    text = the admin's restart reason / free text, already chosen for this language."""
    if kind == "u": return srv_text("update_available", lang)
    if kind == "v": return srv_text("update_refusal", lang)
    if kind == "r":
        m = max(1, (param + 59) // 60)
        return srv_text("restart_in_why", lang, m=m, r=text) if text else srv_text("restart_in", lang, m=m)
    if kind == "s": return srv_text("shutdown", lang)
    return text

def notice_english(kind, param, text):
    return notice_text(kind, param, text, "en")

def push_menu_note(text):
    """The text as an op 0x11 note on every held session socket of a player that is not in a match (menus / searching).
    text = a str (sent as is) or a function lang -> str: rendered for every recipient in its own last-known language."""
    n = 0
    with SESSIONS.lock:
        conns = list(SESSIONS.by_token.items())
    for tok, c in conns:
        if MM.in_match(tok):
            continue
        try:
            t = text(lang_of(tok)) if callable(text) else text
            c.sendall(msg(0x11, t.encode("utf-8", "replace")[:0xf0])); n += 1
        except OSError:
            pass
    return n

def notice_send(text, minutes=30.0, seconds=None, text_pl=None):
    """Admin free text. text_pl (optional) is the Polish form: players whose language is Polish get it, everybody else (and Polish players when
    it is empty) gets `text` as typed."""
    nid = NOTICES.add_text(text, minutes, seconds, text_pl)
    if nid:
        en = clean_str(text, 140) or clean_str(text_pl, 140)
        pl = clean_str(text_pl, 140) or en
        evt("notice", nid, "free text", en, ("| pl: " + pl) if pl != en else "")
        push_menu_note(lambda lang: pl if lang == "pl" else en)
    return nid

def shutdown_schedule(minutes, reason="", restart=True, reason_pl=None):
    r = NOTICES.schedule(minutes, reason, restart, reason_pl)
    evt("shutdown-scheduled", "in %.1f min" % float(minutes), "restart" if restart else "stop", clean_str(reason, 100))
    en, pl = clean_str(reason, 100), clean_str(reason_pl, 100)
    sec = int(float(minutes) * 60)
    push_menu_note(lambda lang: notice_text("r", sec, (pl if lang == "pl" else "") or en, lang))
    return r

def shutdown_cancel():
    had = NOTICES.cancel()
    if had:
        evt("shutdown-cancelled")
        push_menu_note(lambda lang: srv_text("restart_cancel", lang))
    return had

def reset_running_sessions():
    """ (PT92 live restart): a graceful FIN on the session socket of a player in a RUNNING match makes winmm leave the match ("Left the session"),
    only a Winsock error (WSAECONNRESET) starts the ext's session re-attach. Close those sockets with an RST (SO_LINGER 1/0) so the clients re-attach
    to the restarted daemon (READOPT + session_reattach) and keep playing. Returns the number of sockets reset."""
    n = 0
    try:
        with SESSIONS.lock:
            conns = list(SESSIONS.by_token.items())
        for tok, c in conns:
            try:
                if not (MM.in_match(tok) and MM.mid_of(tok) in MM.started):
                    continue
                c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                c.close()
                n += 1
            except Exception:                                  # noqa: BLE001
                pass
    except Exception as e:                                     # noqa: BLE001
        log("reset_running_sessions failed", repr(e))
    if n:
        log("daemon exit: reset", n, "session socket(s) of running matches (clients re-attach after the restart)")
    return n

def boot_all_sessions():
    """ the admin update boot. A graceful FIN on every session socket makes winmm leave the match
    ("Left the session", back to the menu), unlike the restart path (reset_running_sessions: RST = re-attach, the match goes on)."""
    n = 0
    with SESSIONS.lock:
        conns = list(SESSIONS.by_token.items())
    for tok, c in conns:
        try:
            c.shutdown(socket.SHUT_RDWR)
            c.close()
            n += 1
        except Exception:                                  # noqa: BLE001
            pass
    log("admin boot-all: closed", n, "session socket(s) gracefully (clients leave their match)")
    return n

def final_exit(why, restart=False):
    """Best effort, short: tell everybody, give the clients' poll one cycle, then leave (restart = exit code 4: T2R-Host.ps1 starts the server again)."""
    NOTICES.go_final()
    evt("shutdown", why)
    live_save()
    push_menu_note(lambda lang: notice_text("s", 0, "", lang))
    time.sleep(FINAL_WAIT_S)
    log("daemon exit:", why)
    reset_running_sessions()
    time.sleep(0.8)                       # the log flusher writes every 0.5 s
    EXIT_FN(4 if restart else 0)

def shutdown_tick():
    d = NOTICES.due()
    if d:
        final_exit("scheduled " + ("restart" if d["restart"] else "stop"), d["restart"])

def install_exit_hooks():
    """SIGINT / SIGTERM / SIGBREAK and a closing console window: one final notice, then exit."""
    import signal
    def on_sig(signum, frame):
        threading.Thread(target=final_exit, args=("signal %s" % signum,), daemon=True).start()
    for name in ("SIGINT", "SIGTERM", "SIGBREAK"):
        if hasattr(signal, name):
            try: signal.signal(getattr(signal, name), on_sig)
            except (ValueError, OSError): pass
    if os.name == "nt":
        try:
            import ctypes
            HandlerT = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_uint)
            def ctrl(ev):
                if ev in (2, 5, 6):                   # close / logoff / shutdown: Windows allows only a few seconds
                    NOTICES.go_final(); push_menu_note(lambda lang: notice_text("s", 0, "", lang)); time.sleep(min(FINAL_WAIT_S, 3.0))
                    return 0
                return 0
            global _ctrl_ref
            _ctrl_ref = HandlerT(ctrl)
            ctypes.windll.kernel32.SetConsoleCtrlHandler(_ctrl_ref, 1)
        except Exception as e:
            log("console close hook not installed:", repr(e))

MODEQ = modeq.ModeQ(now=time.time)

# ---------------------------------------------------------------- private matches (tools/private_lobby.py, re/private-lobby-1117.md)
PRIV = private_lobby.Lobbies(nmaps=len(MAPS), now=time.time, log=log)
PRIVATE_ON = os.environ.get("T2R_PRIVATE", "1") != "0"


def priv_allowed(players):
    """Map indices a private match of `players` may be played on: every map this server has (T2R_SP_MAPS, T2R_MAP_EXCLUDE, BROKEN_MAPS) that
    every player's DLL has (packs / SP sections). The max-team filter of public matches does NOT apply: a private lobby may play 4v4 on School."""
    ph = "f0" * 30
    players = [p for p in players if not (isinstance(p, str) and p.startswith(ph))]
    off = ({m[2] for m in SP_MAPS} - sp_maps_on()) | {x.strip() for x in os.environ.get("T2R_MAP_EXCLUDE", "").split(",") if x.strip()} | BROKEN_MAPS
    return [i for i, m in enumerate(MAPS) if m[2] not in off and PACKS.allowed(m[2], players) and SPSUP.allowed(m[2], players)]


def priv_pool(rules, players):
    """The private match's playable pool: its own pool mask on the allowed maps; Interrogation keeps the maps with lockbox spots. Never empty."""
    allowed = priv_allowed(players)
    pool = [i for i in allowed if int(rules.get("pool", -1)) >> i & 1] or allowed
    if rules.get("mode", 0) == 1 and INT.lockboxes:
        pool = [i for i in pool if i in INT.lockboxes] or [i for i in allowed if i in INT.lockboxes] or pool
    return pool or [0]


def priv_pick_map(mid, players):
    """start_match of a private match: the map. Returns (map index, vote options or None)."""
    r = PRIV.rules_of_mid(mid) or {}
    pool = priv_pool(r, players)
    m = int(r.get("map", private_lobby.MAP_VOTE))
    if m >= 0 and m in pool:
        return m, None
    if m == private_lobby.MAP_RANDOM or len(pool) == 1 or not map_vote_on():
        return pool[secrets.randbelow(len(pool))], None
    return pool[0], list(pool)[:20]            # a vote over the whole private pool (the ext's all-maps grid, MF_GRID = 20 cells)


def priv_op(pl):
    """Op 0xF8 [token][ver][sub][arg] -> 0xF9 reply (tools/private_lobby.py header)."""
    req = private_lobby.Lobbies.parse(pl)
    if not req:
        return None
    token, sub, arg = req
    if not is_token(token):
        return None
    if not PRIVATE_ON:
        return msg(private_lobby.OP_REPLY, b"ok=0 err=off in=0")
    PRIV.touch(token)
    if sub == private_lobby.S_RULES:
        mid = MM.mid_of(token)
        return msg(private_lobby.OP_REPLY, PRIV.rules_text(mid if mid and PRIV.is_private_mid(mid) else None).encode())
    err = ""
    if sub == private_lobby.S_CREATE:
        _, err = PRIV.create(token)
    elif sub == private_lobby.S_JOIN:
        _, err = PRIV.join(token, arg)
    elif sub == private_lobby.S_LEAVE:
        PRIV.leave(token)
        with MM.lock:
            if token in MM.queue:                  # a launching member that left stops searching for it
                MM.queue.pop(token, None)
    elif sub == private_lobby.S_SET:
        _, err = PRIV.set(token, arg)
    elif sub == private_lobby.S_TEAM:
        _, err = PRIV.set_team(token, arg)
    elif sub == private_lobby.S_KICK:
        _, err = PRIV.kick(token, arg)
    elif sub == private_lobby.S_START:
        lb = PRIV.lobby_of(token)
        busy = [t for t in (lb.members if lb else ()) if MM.in_match(t)]
        if busy:
            err = "inmatch"                        # a member is still in another match
        else:
            _, err = PRIV.start(token)
    elif sub == private_lobby.S_CANCEL:
        _, err = PRIV.cancel(token)
    if sub != private_lobby.S_STATE:
        log("PRIVATE", private_lobby.SUB_NAMES.get(sub, sub), tag(token), arg[:40], err or "ok")
    lb = PRIV.lobby_of(token)
    av = ""
    if lb:
        av = " av=%x" % sum(1 << i for i in priv_allowed(list(lb.members)))   # maps every member can play (the CHOOSE MAP / MAP POOL grid greys the others)
    return msg(private_lobby.OP_REPLY, (PRIV.state_text(token, get_name, err) + av).encode())

def requeue_keep_mode(token, mid):
    """A ticket holder searches again (MatchMaker.enqueue): the same mode as before - its own pick (refreshed, so a long session never falls back to the
    default mode after modeq.PREF_TTL) or, with no pick on record, the mode the finished match was played in."""
    try:
        with MODEQ.lock:
            e = MODEQ.pref.get(token)
        if e and MODEQ.now() - e[1] <= modeq.PREF_TTL:
            MODEQ.set(token, e[0])
            return
        mm = MODEQ.match_mode(mid)
        if mm in modeq.MODES:
            MODEQ.set(token, modeq.MODES.index(mm))
    except Exception as ex:                                       # noqa: BLE001 - a mode refresh never breaks the queue
        log("requeue: mode keep failed", repr(ex))

def requeue_status_text(token):
    """' rq=1 rqw=<n>' for the 0xBD reply while the token holds a requeue ticket (n = players its search waits for, 0 = not held), else ''."""
    if not REQUEUE:
        return ""
    with MM.lock:
        qd = list(MM.queue)
    now = time.time()
    lb = MM.rq.lobby(token, qd, lambda t: silence(t, now), MM.in_match, now)
    if lb is None:
        return ""
    # (persistent lobby) rqs = s until the next match forms anyway (0 = not held), rqb = members back, rqp = members still on the screens
    return " rq=1 rqw=%d rqs=%d rqb=%d rqp=%d" % (lb["wait"], lb["left"], lb["back"], lb["post"])

def modeq_op(pl):
    """Op 0xBC [token][sub][mode] -> reply frame (0xBD): sub 1 stores the pick for the player's next search, both answer the searching counts."""
    req = modeq.ModeQ.parse(pl)
    if not req:
        return None
    token, sub, mode = req
    if not is_token(token):
        return None
    if sub == 3:                                   # "my game window is closing" (Alt+F4 / the X during a match): its session drop is its own leave
        mid_ = MM.mid_of(token)
        CLAN_CLOSING[token] = (time.time(), mid_)
        if len(CLAN_CLOSING) > 4000:
            for _k in list(CLAN_CLOSING)[:1000]:
                CLAN_CLOSING.pop(_k, None)
        log("CLAN closing", tag(token), "mid", mid_, "- its game window is closing (a session drop now counts as its own leave)")
    if sub == 1:
        if MODEQ.set(token, mode):
            log("MODEQ", tag(token), "picks", "ANY (quick match)" if mode == modeq.ANY else modeq.MODES[mode])
        else:
            log("MODEQ", tag(token), "bad mode", mode)
    with MM.lock:
        fresh = [t for t in MM.queue if time.time() - MM.seen.get(t, 0) <= QUEUE_FRESH]
    q = MODEQ.counts(fresh, PARTIES.leader)
    # ("find match mode choosing doesnt account numbers for people in matches"): each mode's number also counts the players in a
    # running match of that mode (started, not ended, not departed; a match without a recorded mode is Supply Raid). T2R_MODEQ_PLAYING=0 = searchers only.
    playing = 0
    if os.environ.get("T2R_MODEQ_PLAYING", "1") != "0":
        with MM.lock:
            live = [(mid, [t for t in pl if t not in set(MM.departed.get(mid, ())) and t not in fresh]) for mid, pl in MM.matches.items()
                    if mid in MM.started and mid not in MM.ended]
        for mid, pl in live:
            mm = MODEQ.match_mode(mid) or modeq.MODES[0]
            k = modeq.MODES.index(mm) if mm in modeq.MODES else 0
            q[k] += len(pl); playing += len(pl)
    # the match's own mode as soon as the match is assigned
    # (mm=<row index>), so the loading / vote screens of a QUICK MATCH never guess. Only a live match of this token (never a finished one).
    mm = ""
    try:
        mid = MM.mid_of(token)
        with MM.lock:
            live = mid is not None and mid not in MM.ended
        if live:
            m_ = MODEQ.match_mode(mid)
            if not m_ and modeq.forced():
                m_ = os.environ.get("T2R_MODE", "").strip().lower()
            k_ = modeq.MODES.index(m_) if m_ in modeq.MODES else 0
            mm = " mm=%d" % k_
    except Exception as ex:
        log("MODEQ mm ERR", repr(ex))
    rqs = ""
    try:
        rqs = requeue_status_text(token)                         # the client's auto search + WAITING FOR PLAYERS... (ext requeue.c)
    except Exception as ex:                                       # noqa: BLE001
        log("requeue: status ERR", repr(ex))
    return msg(modeq.OP_REPLY, (modeq.ModeQ.reply(q, MODEQ.get(token, PARTIES.leader(token)), modeq.forced(), len(fresh) + playing) + mm + rqs).encode())

def interrogation_op(pl):
    """Op 0xDC -> reply frame (0xDD), or None for a bad payload."""
    req = INT.parse_request(pl)
    if not req:
        return None
    token, sub, seq, args = req
    readopt_claim(token)            # a poll right after a daemon restart must find the restored record (it answered on=0 and the HUD fell back to Supply Raid)
    int_rebind(token)
    return msg(interrogation.OP_REPLY, INT.event(token, sub, seq, args).encode())

def int_rebind(token):
    """ INT.by_token kept pointing at the player's PREVIOUS match when the new one has no Interrogation record (a Supply Raid
    match, or a join in progress into one): a player who crashed out of a running Interrogation match on Supermarket and joined a Resort match
    got the old match's state (map 23, its lockbox spots ~700 m off the map, its intel). The token's current match decides: its Interrogation
    record when it has one, else none (on=0). A token in no match keeps its record (an ended match shows its win to late clients)."""
    try:
        cur = MM.mid_of(token)
        if cur is None:
            return
        with INT.lock:
            had = INT.by_token.get(token)
            if had is None or had == cur:
                if had is None and cur in INT.matches:
                    INT.by_token[token] = cur
                return
            if cur in INT.matches:
                INT.by_token[token] = cur
            else:
                INT.by_token.pop(token, None)
        log("INTERROGATION rebind", tag(token), "from match", had, "to", cur if cur in INT.matches else "none (its match %s has no record)" % cur)
    except Exception as ex:                                       # noqa: BLE001 - the HUD state never breaks the request
        log("INTERROGATION rebind ERR", repr(ex))

ABORT_AWARDED = set()   # mids whose abort award ran

def abort_award(mid, others, host, why):
    """SAFETY NET: the host left a running match and no migration takes over (migration off / not armed on the
    clients). The remaining players still get everything a normal match end grants, as if it ended now: the clan day
    resolves as a normal day (CLAN_ENDED), the match is marked ended (scoreboard / requeue like after a 0x12) and the mode
    ends. Win / loss: NO team is credited with a win (winner 0xFF = draw: the daemon has no score; wins stay as they are,
    matches already counted at GAME-START). The host that left keeps a 'left early' day (CLAN_QUITTERS)."""
    if not mid or mid in ABORT_AWARDED:
        return False
    with MM.lock:
        joining = {t for t, (m, _) in MM.joining.items() if m == mid}
    others = [p for p in (others or []) if p not in joining]      # a join-in-progress player that has not started yet is not 'left over' (PT73 12:00)
    closed = [p for p in others if (p, mid) in CLAN_CLOSE_DROPS]   # a player who closed its window is not 'left over' - its session is just
    if closed:                                                    # still inside the re-attach window. Its own leave stays 'left early'.
        others = [p for p in others if p not in closed]
        for p in closed:
            if clan_leave_excuse(p, mid, "session lost") is None:
                CLAN_QUITTERS.add((p, mid))
        log("ABORT-AWARD", mid, "not for", [tag(p) for p in closed], "(closed the game window)")
    if not others:
        return False
    with MM.lock:
        if mid in MM.ended or mid not in MM.started:
            return False                    # a normal end already ran / the match never went live
    ABORT_AWARDED.add(mid)
    if host and clan_leave_excuse(host, mid, why) is None:      # (a closed window / an over-cap lost link counts too) a deliberate host quit stays 'left early' (a crash / timeout / start- or end-grace leave does not)
        CLAN_QUITTERS.add((host, mid))
    for p in others:                                              # the players it was ended for are never penalised for it (CLAN_ABORTED)
        CLAN_ABORTED[(p, mid)] = why
    if len(CLAN_ABORTED) > 2000:
        for _k in list(CLAN_ABORTED)[:500]:
            CLAN_ABORTED.pop(_k, None)
    clan_match_ended(others, mid)
    MM.end(mid)
    INT.end(mid, 0xFF)
    GL.end(mid)
    evt("abort-award", mid, "host", tag(host) if host else "?", why, "| awarded:", [tag(p) for p in others])
    log("ABORT-AWARD", mid, "host", tag(host) if host else "?", why, "players", [tag(p) for p in others])
    return True

def clan_why_deliberate(why):
    return any(d in (why or "") for d in CLAN_DELIBERATE_WHY)

_CLAN_LOST_COUNTED = set()   # (token, mid) already counted against CLAN_LOST_EXCUSE_MAX

def clan_leave_excuse(token, mid, why, at=None):
    """None = a deliberate leave of a running match that counts as 'left early'; else the reason it is excused (crash / timeout / ...).
    at = when the player really left (its session socket dropped); the verdict of a lost session comes SESSION_GRACE_S later, and the start /
    end grace windows are measured from the real departure, not from that timeout."""
    if CLAN_NO_PENALTY:
        return "no clan penalties on this server (T2R_CLAN_NO_PENALTY)"
    if not clan_why_deliberate(why):
        w = (why or "").lower()
        lost = "session lost" in w or "timed out" in w
        cl = CLAN_CLOSING.get(token)
        if lost and ((token, mid) in CLAN_CLOSE_DROPS or (cl and time.time() - cl[0] <= CLAN_CLOSING_S and cl[1] in (None, mid))):
            why = "closed the game window"              # the player's own leave: the grace windows / the update excuse below still apply
        else:
            kind = ("crash / lost connection (session socket lost, never came back)" if "session lost" in w else
                    "timeout (no packets from the player)" if "timed out" in w else
                    "never loaded the level" if "never loaded" in w or "never started" in w else
                    "removed by an admin" if "admin" in w else "not its own choice (%s)" % why)
            if lost and CLAN_LOST_EXCUSE_MAX > 0:
                now = time.time()
                key = (token, mid)
                ex = [t for t in CLAN_LOST_EXCUSES.get(token, []) if now - t < CLAN_LOST_EXCUSE_WINDOW]
                if key not in _CLAN_LOST_COUNTED:                    # (one match counts once: abort_award asks again for the same leave)
                    _CLAN_LOST_COUNTED.add(key)
                    if len(_CLAN_LOST_COUNTED) > 4000:
                        _CLAN_LOST_COUNTED.clear()
                    ex.append(now)
                CLAN_LOST_EXCUSES[token] = ex
                if len(ex) > CLAN_LOST_EXCUSE_MAX:
                    log("CLAN excuse", tag(token), "mid", mid, "%s refused - %d such exits in the last %.0f h (cap %d): counts as left early"
                        % (kind, len(ex), CLAN_LOST_EXCUSE_WINDOW / 3600, CLAN_LOST_EXCUSE_MAX))
                    return None
            return kind
    # The client ext force-closes the game 60 s after the server's 'update available' notice (kind 'u') reaches it in a match:
    # a player still on an older build than the published one leaves on the update's word, not by choice
    try:
        cv, tv = CLIENT_VERSIONS.get(token), update_target()
    except Exception:                                   # noqa: BLE001 - an excuse check never breaks a leave
        cv = tv = None
    if cv and tv and cv != tv:
        return "playing an older build after an update was published (%s, current %s)" % (cv, tv)
    now = at or time.time()
    with MM.lock:
        st_t = MM.started_t.get(mid)
        ck = MM.clock.get(mid)
    if st_t and mid not in RESTORED_T and now - st_t < CLAN_START_GRACE_S:      # (a restored match has no true start time)
        return "left %.0f s after the match start (< %.0f s: could not reach the host / bad start)" % (now - st_t, CLAN_START_GRACE_S)
    # A join in progress has the same bad-start window from ITS start: a joiner whose game never reached the host ("Could not reach host")
    # and who closed it seconds after its own GAME-START got a left-early day.
    js = JIP_STARTED.get(token)
    if js and js[0] == mid and 0 <= now - js[1] < CLAN_START_GRACE_S:
        return "left %.0f s after its join-in-progress start (< %.0f s: could not reach the host / bad start)" % (now - js[1], CLAN_START_GRACE_S)
    if ck:
        left = ck[0] - (now - ck[3])
        if ck[2] or (now - ck[3] < 60 and left < CLAN_END_GRACE_S):
            return "left in the last %.0f s of the round (decided)" % max(0.0, left)
    return None

def leave_running_match(token, why):
    """A player is out of its running match: they are free to queue again and the others play on (the match
    record stays for them - slots are list indexes, the host's result still finds its players; with host
    migration the host leaving also hands the match to someone else). The last one out removes the record.
    It used to delete the whole record without host migration, which orphaned the remaining players: their
    match-end result found no players and a later 'already in a match' check blocked nobody but also
    recorded nothing. Returns True when the match was kept for others."""
    GL.release(token)                       # a gun limit it held (tools/gunlimit.py) is free for its team at once
    _lost = SESS_LOST.pop(token, None)
    all_players = MM.players_of(token)
    migrating = bool(HM.enabled and HM.has(token))
    was_host = (HM.host_token(token) == token) if migrating else bool(all_players and all_players[0] == token)
    pre_mid = MM.mid_of(token)
    if pre_mid is not None:
        lastd = CLAN_LAST_DEPART.get(pre_mid)
        host_lost = False
        if HM.enabled:
            with HM.lock:
                hm = HM.matches.get(pre_mid)
                host_lost = bool(hm and (hm.get("lost") or hm.get("pending")) and not hm.get("given_up"))
        # a lost session is removed SESSION_GRACE_S (60 s) after its socket dropped; every window below is measured from the drop
        _at = _lost[0] if _lost and _lost[1] == pre_mid and "session lost" in (why or "") else None
        _t = _at or time.time()
        CLAN_LEFT_AT[(token, pre_mid)] = _t
        _why_ex = clan_leave_excuse(token, pre_mid, why, _at)
        if _why_ex is None and ((lastd and lastd[1] != token and _t - lastd[0] <= CLAN_EXCUSE_S) or host_lost):
            _why_ex = ("host lost (migration pending)" if host_lost else "%.0f s after %s left" % (_t - lastd[0], tag(lastd[1])))
        if _why_ex is None and migrating:
            # 07a228ac: the host hitched, the others moved on without it, its ext heard 'replaced as host - leaving the match' and sent the
            # 0x02 itself: that leave was the server's decision, not the player's
            _rep = HM.replaced_age(token)
            if _rep is not None and _rep <= hostmig.REPLACED_EXCUSE_S:
                _why_ex = "replaced as host by a migration %.0f s before (its game left on the server's word)" % _rep
        if _why_ex is not None:
            if (token, pre_mid) in CLAN_CLOSE_DROPS:
                CLAN_QUITTERS.discard((token, pre_mid))     # (a later excuse wins over the closed-window day: clan_closed_book)
            CLAN_EXCUSED.add((token, pre_mid))
            CLAN_EXCUSE_WHY[(token, pre_mid)] = _why_ex
            log("CLAN excuse", tag(token), "mid", pre_mid, "leaves (%s): %s" % (why, _why_ex), "- no left-early penalty")
        else:
            CLAN_EXCUSED.discard((token, pre_mid))
            log("CLAN leave", tag(token), "mid", pre_mid, "deliberate (%s) - counts as left early unless a later excuse applies" % why)
        CLAN_LAST_DEPART[pre_mid] = (_t, token)
    if migrating:
        HM.depart(token, why)
    mid = MM.depart(token, voluntary=why.startswith("left"))
    others = MM.players_of_mid(mid) if mid else None
    if mid and others and was_host and not (migrating and not HM.given_up(mid)):
        with MM.lock:
            dep = set(MM.departed.get(mid, ()))
        abort_award(mid, [p for p in others if p != token and p not in dep], token, "host left (" + why + "), no migration")
    elif mid and others:
        # (playtest: everyone else dropped out - server / host crash, disconnects - and the one player left got "YOU LEFT EARLY - NO
        # SUPPLIES" when they finally quit the empty match.) One player left in a running match = the match is over for them: the normal-day
        # award, as if it ended now. The ones who dropped keep their own 'left early' day.
        with MM.lock:
            dep = set(MM.departed.get(mid, ()))
        rest = [p for p in others if p != token and p not in dep]
        # (3 players were rebalanced, the host was left alone, the match ended and the host got a short day): players the admin is moving to
        # the other team are coming back into this match - they do not make it "everyone else left"
        moving = [t for t in dep | {token} if (MM.move_team.get(t) or (None,))[0] == mid]
        if len(rest) == 1 and not moving:
            abort_award(mid, rest, None, "everyone else left (" + why + ")")
        elif len(rest) == 1:
            log("ABORT-HOLD", mid, "only", tag(rest[0]), "left, but", [tag(t) for t in moving], "are being moved by the admin - no abort award")
    if mid:
        evt("left", mid, tag(token), why, "| others stay: %s" % ([tag(p) for p in others] if others else "none (match removed)"))
        if not others:
            HM.end(mid)
            forget_match_state(all_players)
    return bool(others)

def forget_match_state(mid_players):
    """Drop the per-match dictionaries of a finished / removed match (VOTES, MATCH_MAP, TIMING, MATCH_P2P grew forever)."""
    if not mid_players:
        return
    key = frozenset(mid_players)
    with MM.lock:
        if any(frozenset(p) == key for p in MM.matches.values()):
            return                          # the same people already formed a NEW match: its state is live
    with _vote_lock:
        VOTES.pop(key, None)
    MATCH_MAP.pop(key, None); TIMING.pop(key, None); MATCH_P2P.pop(key, None)
    SID_KEEP.pop(key, None)

# ---------------------------------------------------------------- replies
def build_server_lists():
    """0x08 payload: [map_count][map recs][udp_count][udp recs]. Both counts > 0.
    map rec=[l1][name][l2][image][l3][level][flag] (see MAPS); udp rec=[iplen][ip][u16 port]."""
    # PUBLIC endpoint the client's game-server check (UDP) must reach.
    # env-overridable: T2R_PUB_IP / T2R_PUB_UDP_PORT.
    gip = os.environ.get("T2R_PUB_IP", "209.25.141.16").encode()   # playit TCP+UDP tunnel IP
    uport = int(os.environ.get("T2R_PUB_UDP_PORT", "15389"))       # playit UDP port
    browser = bytes([len(MAPS)]) + b"".join(
        lp(name.encode()) + lp(os.environ.get("T2R_MAPIMG", "streaming/menus/to1/map-pvp-{img}.png").format(img=img).encode()) + lp(lvl.encode()) + bytes([RESPAWNS.get(lvl, 0)])
        for name, img, lvl in MAPS)
    udp = bytes([1]) + lp(gip) + uport.to_bytes(2, "little")
    return browser + udp

def auth_reply(name="PLAYER", extra=b""):
    # PROVEN sequence (reaches MP menu): 0x10 name + 0x08 both-lists + 0x0d done.
    # Do NOT drop the 0x0d — its absence regressed auth previously. `extra` frames go
    # before the 0x0d so a session socket still parses them.
    name_field = (safe_name(name) or "PLAYER").encode()[:31].ljust(32, b"\x00")
    return (msg(0x10, name_field) + msg(0x08, build_server_lists()) + extra
            + msg(0x0d, b"\x01"))

def auth_reply_open(name="PLAYER", extra=b""):
    """auth_reply() without the trailing 0x0d: for the held MP-menu socket (see handle_batch)."""
    name_field = (safe_name(name) or "PLAYER").encode()[:31].ljust(32, b"\x00")
    return msg(0x10, name_field) + msg(0x08, build_server_lists()) + extra

def lobby_snapshot():
    """Push current online roster to a session client: 03 clear + (04 player)* ."""
    out = msg(0x03)
    for p in PRES.online():
        nm = (p.get("name", "PLAYER")).encode()[:31].ljust(32, b"\x00")
        out += msg(0x04, nm)          # TODO: full 0x1b0 player-rec fields
    return out

# ---------------------------------------------------------------- TCP
def parse_login(payload):
    """0x0f payload: [1c][64B token][01 02 07][loadout blob].
    71B login has no loadout; the big PROFILE frame carries the opaque loadout,
    which we persist verbatim (byte-exact replay; slice boundaries are fragile).
    Name is NOT here — it arrives in the separate 0x0e frame. Returns
    (token_hex, raw_profile_or_None, None)."""
    token = loadout = None
    if len(payload) >= 65 and payload[0] == 0x1c:
        t = payload[1:65].decode("latin1", "ignore")
        if all(ch in "0123456789abcdefABCDEF" for ch in t):   # client tokens are 64 hex
            token = t
    if len(payload) > 100:            # PROFILE frame carries the loadout blob
        loadout = payload             # store verbatim for exact replay
    return token, loadout, None

def name_from_session(payload):
    """0x01/0x0e payload = [1c][8B nonce][32B name]. Return name or None."""
    if len(payload) >= 41:
        return payload[9:41].split(b"\x00")[0].decode("latin1", "ignore") or None
    return None

NOISY_OPS = {0xBC, 0xCC, 0xBA, 0xB4, 0xB6, 0x05, 0x15, 0xE3, 0xE6, 0xDC, 0xEA, 0xE8, 0xE0, 0xD0, 0xF3, 0xF4, 0xF6, 0xDA, 0xC6, 0xC8, 0xCA, 0x13,
             0xDE, 0xBF}   # gun-limit (0xDE) + rank-badge (0xBF) polls were 64 % of all daemon.log lines (35k in 15 min with ~100 players)
NOISY_COUNT = [0]
MAX_TCP_BUF = 256 * 1024    # a client frame is < 1 KB (the loadout profile ~350 B); more unparsed bytes = garbage
def split_frames(buf):
    """Cut [u16 len][op][payload] frames off the front of buf. Returns (frames, rest); rest is None when
    the stream is hopeless (more than MAX_TCP_BUF bytes pending). A zero length is skipped (2 bytes),
    it used to stall the parser for good and let the buffer grow without bound."""
    out = []
    i = 0
    while len(buf) - i >= 2:
        ln = int.from_bytes(buf[i:i + 2], "little")
        if ln == 0:
            i += 2
            continue
        if len(buf) - i < ln + 2:
            break
        out.append((buf[i + 2], buf[i + 3:i + 2 + ln]))
        i += 2 + ln
    rest = buf[i:]
    return out, (rest if len(rest) <= MAX_TCP_BUF else None)

MAX_TCP_CONNS = int(os.environ.get("T2R_MAX_CONNS", "512"))
MAX_PER_IP = int(os.environ.get("T2R_MAX_PER_IP", "64"))            # live TCP connections per source address (the playit tunnel gives each remote IP its own 127.x address)
PER_IP_LOOPBACK = os.environ.get("T2R_PER_IP_LOOPBACK", "0") == "1"  # 127.0.0.1 itself (the host's own game, probes) is exempt unless a test turns this on
PARTIAL_FRAME_S = float(os.environ.get("T2R_PARTIAL_FRAME_S", "30"))  # an incomplete frame must be completed within this many seconds
FLOOD_WINDOW_S = 10.0
FLOOD_MAX_RECVS = int(os.environ.get("T2R_FLOOD_MAX_RECVS", "3000"))  # reads per FLOOD_WINDOW_S on one connection (real clients: < 30)
FIRST_FRAME_S = float(os.environ.get("T2R_FIRST_FRAME_S", "15"))    # a new connection that has not sent one complete frame by then is closed (slowloris)
# overload: short read polls (main menu feed, rank table, clan / clan13 page, names read, meta) whose client gave up are dropped at
# the next _dblock wait instead of being served to a closed socket. The ext's t2r_rpc waits at most 5 s for a reply (outage.h callers),
# so POLL_DEADLINE_S after the frame arrived nobody reads the answer. Session and Steam helper connections never get a deadline. 0 = off.
POLL_DEADLINE_S = float(os.environ.get("T2R_POLL_DEADLINE_S", "6"))
POLL_DEADLINE_OPS = frozenset((0xB4, 0xBF, 0xE0, 0xB6, 0xD0, 0xE8))
_tcp_live = [0]
_tcp_live_lock = threading.Lock()
_tcp_per_ip = collections.Counter()

class AddrLimiter:
    """Sliding-window counter per source address: allow(key) is True for the first `n` calls per `window` s."""
    def __init__(self, n, window):
        self.n, self.window, self.hits, self.lock = n, window, {}, threading.Lock()
    def allow(self, key, now=None):
        now = time.time() if now is None else now
        with self.lock:
            if len(self.hits) > 4096:
                self.hits = {k: v for k, v in self.hits.items() if v and now - v[-1] < self.window}
            q = self.hits.setdefault(key, collections.deque())
            while q and now - q[0] > self.window:
                q.popleft()
            if len(q) >= self.n:
                return False
            q.append(now)
            return True
    def prune(self, now=None):
        """Drop the addresses whose window is empty (one key per source address ever seen otherwise). Returns how many went."""
        now = time.time() if now is None else now
        with self.lock:
            dead = [k for k, v in self.hits.items() if not v or now - v[-1] > self.window]
            for k in dead:
                del self.hits[k]
        return len(dead)
# new identities (a never-seen token) one address may introduce per window: DB rows / version entries / Linux marks are only created below this
NEW_TOKEN_N = int(os.environ.get("T2R_NEW_TOKENS_PER_10MIN", "12"))
NEW_TOKENS = AddrLimiter(NEW_TOKEN_N, 600.0)
HEX64 = frozenset("0123456789abcdefABCDEF")
def is_token(t):
    return isinstance(t, str) and len(t) == 64 and all(ch in HEX64 for ch in t)
def token_known(t):
    try:
        return db_read(lambda c: c.execute("SELECT 1 FROM players WHERE token=?", (t,)).fetchone()) is not None
    except Exception:
        return False
def new_identity_ok(token, addr):
    """Ops that CREATE database rows for a token (profile / loadout save, custom name, clan side, booster pick, clan report) are open to anybody
    on the internet and the token is whatever 64 hex characters the sender writes: a loop of random tokens grew the DB (players / names / clans /
    boosters rows) for ever. A token the DB knows is always fine; a never-seen one counts against its source address (NEW_TOKENS: 12 per
    10 min). The host's own 127.0.0.1 (game + tools, tests) is exempt like it is from the connection cap."""
    if addr is None or (addr[0] == "127.0.0.1" and not PER_IP_LOOPBACK):
        return True
    if not is_token(token):
        return False
    return token_known(token) or NEW_TOKENS.allow(addr[0])

def _creates_rows(op, pl, addr):
    """True when this op would create rows for a token the sender may not introduce (see new_identity_ok): the caller skips it."""
    if op in (0xD2, 0xE4, 0xEE, 0xEF, 0xE2) and len(pl) >= 64:
        if not new_identity_ok(pl[:64].decode("latin1", "ignore"), addr):
            log("NEWID", addr, "op 0x%02x refused: too many new identities from this address" % op)
            return True
    return False

STEAMID_MIN, STEAMID_MAX = 76561197960265728, 76561202255233024     # individual accounts (universe 1, type 1)

def tcp_conn(conn, addr):
    """One recv can carry MULTIPLE frames. We parse the whole batch, then decide the
    response by what the batch CONTAINS — because the same profile (0x0f) leads both
    the BOOT handshake (needs auth reply incl 0x0d = auth-done -> menu) and the
    MATCHMAKING action (op 0x01 host/join) where 0x0d = DISCONNECT and would bounce
    the client. So: batch with 0x01/0xfc = matchmaking (send 0x03, NO 0x0d);
    batch with only 0x0f/0x0e = boot (send full auth reply)."""
    conn = wrap_conn(conn)                          # writes go through the connection's own bounded queue (OutConn)
    conn.settimeout(FIRST_FRAME_S)                  # unauthenticated until a frame was parsed (then 120 s below)
    t_conn = time.monotonic(); got_frame = False
    st = {"token": None, "name": "PLAYER", "authed": False, "session": False, "is_bridge": False}
    buf = b""
    close_why = "handler ended"                      # why this connection ended (logged for session connections: a client whose session socket dies while searching is thrown to the title)
    partial_since = 0.0                              # monotonic time the bytes now in `buf` (an incomplete frame) started waiting; 0 = nothing pending
    recv_n, recv_t0 = 0, time.monotonic()            # recv() calls in the current FLOOD_WINDOW_S window (a polite client sends < 2 / s)
    try:
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)    # a vanished peer (no FIN) is noticed, not held for 120 s of silence
        if TCP_SNDBUF:
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, TCP_SNDBUF)    # tests only: a small send buffer makes a non-reading peer block quickly
    except OSError:
        pass
    try:
        while True:
            data = conn.recv(8192)
            if not data:
                close_why = "peer closed (FIN)"
                break
            mono = time.monotonic()
            if mono - recv_t0 >= FLOOD_WINDOW_S:
                recv_n, recv_t0 = 0, mono
            recv_n += 1
            if recv_n > FLOOD_MAX_RECVS:
                log("TCP", addr, "flood: %d reads in %.0f s, closing" % (recv_n, FLOOD_WINDOW_S))
                break
            buf += data
            batch, buf = split_frames(buf)
            # slowloris after the first frame: an incomplete frame must complete within PARTIAL_FRAME_S (the 120 s idle timeout restarts with every byte,
            # so one byte per minute held a thread and a connection slot for ever)
            if buf:
                if not partial_since or batch:
                    partial_since = mono
                elif mono - partial_since > PARTIAL_FRAME_S:
                    log("TCP", addr, "incomplete frame for %.0f s, closing" % PARTIAL_FRAME_S)
                    break
            else:
                partial_since = 0.0
            if batch and not got_frame:
                got_frame = True; conn.settimeout(120)
            elif not got_frame and time.monotonic() - t_conn > FIRST_FRAME_S:
                log("TCP", addr, "no complete frame in %.0f s, closing" % FIRST_FRAME_S)
                break
            if buf is None:
                log("TCP", addr, "garbage / oversized stream, closing")
                break
            ops = {op for op, _ in batch}
            if ops and ops <= NOISY_OPS:          # keepalives / polls (every 2-3 s per client): counted, not logged line by line
                NOISY_COUNT[0] += 1
            else:
                log("TCP", addr, f"recv {len(data)}B", hexcut(data))
            if 0x0f in ops and not ops & {0x01, 0xfc, 0xF0}:
                # A check-in alone gets the boot reply ending in 0x0d (session closed). A
                # find-match pressed at the same moment can arrive in the NEXT segment
                # (seen live: 382 B check-in, then 70 B 0x01): it would then go to a socket
                # the client already dropped and the start never arrived. Wait briefly.
                conn.settimeout(0.3)
                try:
                    more = conn.recv(8192)
                except (socket.timeout, TimeoutError):
                    more = None
                finally:
                    conn.settimeout(120)
                if more:
                    log("TCP", addr, f"recv {len(more)}B (joined)", hexcut(more))
                    buf += more
                    more_batch, buf = split_frames(buf)
                    if buf is None:
                        break
                    batch += more_batch
                elif more == b"":
                    handle_batch(conn, addr, st, batch)
                    break
            if batch:
                if POLL_DEADLINE_S > 0 and ops and ops <= POLL_DEADLINE_OPS and not st["session"] and not st["is_bridge"]:
                    _req.deadline = mono + POLL_DEADLINE_S
                try:
                    handle_batch(conn, addr, st, batch)
                except RequestAbandoned:
                    close_why = "request abandoned (client timed out waiting for the DB)"
                    break
                except (ConnectionError, socket.timeout, TimeoutError):
                    raise
                except Exception as e:
                    # a handler bug must not close the socket: for a searching client that is
                    # session state 5 = winmm quits to the title (re/mm-boot.md)
                    log("TCP", addr, "HANDLER ERR (conn kept)", repr(e))
                finally:
                    _req.deadline = 0.0           # (per batch: a later write on the same connection never inherits it)
    except Exception as e:
        close_why = "error %r" % (e,)
        log("TCP", addr, "ERR", e)
    finally:
        _req.deadline = 0.0
        if st["token"] and st["session"]:
            q = conn.queued() if isinstance(conn, OutConn) else 0
            log("SESSION conn closed", tag(st["token"]), "after %.0f s:" % (time.monotonic() - t_conn), close_why, "| searching" if st["token"] in MM.queue else "", "| in match" if MM.in_match(st["token"]) else "", "| %d B unsent" % q)
        if st["token"]:
            # presence is TTL-based (KEEPALIVE_TTL): clients reconnect every few
            # seconds, so dropping on close made the online count flap to 0.
            if st["is_bridge"]:
                if BRIDGES.unregister(st["token"], conn):
                    evt("bridge-gone", tag(st["token"]), "Steam helper disconnected")
                    if HM.enabled and HM.has(st["token"]):
                        HM.bridge_gone(st["token"])
            if st["session"]:
                note_session_lost(st["token"], conn)
                SESSIONS.unregister(st["token"], conn)
        conn.close()

def handle_leave(addr, st):
    """Op 0x02 (client leave/cancel). Runs BEFORE the other ops of its batch: it used to be the last
    branch of an elif chain, so a 0x02 sharing a segment with a 0x05 keepalive / 0x15 poll / 0x01
 re-queue was swallowed and the other player still went into the match alone."""
    if st["token"]:
        others = MM.cancel(st["token"])
        if others is not None:
            # backed out during the countdown: everyone else returns to SEARCHING
            # (op 0x03 clears the client's matched state, the menu page resets)
            for o in others:
                LOADED.discard(o)
                c = SESSIONS.conn(o)
                if c:
                    # 0x03 = back to searching (winmm session state 3 + the page logic resets to FINDING MATCH; t2r-ext pops its
                    # vote / loadout / booster pages when the phase is gone) + the lobby list; the client must then keep polling as a
                    # searcher: only that fresh poll makes it matchable again (MM.cancel marks it not fresh)
                    try: push(c, searching_frame(o), drop=True)
                    except OSError: pass
            log("LEAVE during countdown: match cancelled, back to searching", addr, st["name"],
                [tag(o) for o in others])
            evt("cancel", tag(st["token"]), "backed out before the start; back in the queue:", [tag(o) for o in others])
        elif (MM.in_match(st["token"]) and st["token"] not in LOADED and not MM.all_loaded(st["token"])
              and MM.match_age(st["token"]) <= countdown_seconds() + LEAVE_KEEP_S and st["token"] not in MM.joining):
            # the level start went out but this player never loaded (the start likely
            # hit a dying conn): keep the match, their re-queue re-sends the start
            log("LEAVE kept in match (launched, not loaded; re-queue re-sends the start)", addr, st["name"])
        else:
            if MM.dequeue(st["token"]):
                evt("dequeue", tag(st["token"]), "left the searching page (0x02)")
            mid_l = MM.mid_of(st["token"])
            with MM.lock:
                loading_l = bool(mid_l and mid_l not in MM.started and mid_l in MM.launched)
            jl = JIP_LATE.get(st["token"])
            if jl and jl[0] == mid_l and st["token"] not in LOADED and time.time() - jl[1] < 120:
                # (B on the gear up page right after a join-in-progress START-MATCH is the client's own leave - winmm sends 0x02 itself, the
                # server cannot tell it from a deliberate back-out; it is the same as backing out of a normal countdown: out of this match, free to
                # queue again. The line makes the case findable.)
                log("jip: joiner backed out before loading (0x02 %.0f s after the start)" % (time.time() - jl[1]), tag(st["token"]), "match", mid_l)
            kept = leave_running_match(st["token"], "left (0x02)")
            LOADED.discard(st["token"])
            log("LEAVE", addr, st["name"], "(the others play on)" if kept else "")
            if kept and loading_l:
                # S2: a player backed out while the level loads: the others may all be loaded already - start now, do not wait for the reaper
                with MM.lock:
                    dep_l = set(MM.departed.get(mid_l, ()))
                pl_l = MM.players_of_mid(mid_l) or []
                rest = [p for p in pl_l if p not in dep_l]
                if rest and (all(p in LOADED for p in rest) or (pl_l and pl_l[0] == st["token"])):     # (the host left: survivors back to the queue now)
                    maybe_game_start(rest[0])

def handle_match_end(addr, st, batch):
    """Op 0x12 [winning team] from the host. Must run BEFORE a 0x02 of the same batch (the host
    sends 0x12 then leaves; handle_leave first dropped the match and 0x12 found no players)."""
    for op, pl in batch:
        if op == 0x12 and st["token"]:
            readopt_claim(st["token"])
            if HM.enabled and HM.has(st["token"]) and HM.host_token(st["token"]) != st["token"]:
                # a host we migrated away from (the others could not hear it) ended its own
                # lonely copy of the match: the real host reports the result
                log("MATCH-END ignored (not the current host after a migration)", addr, tag(st["token"]))
                continue
            players = MM.players_of(st["token"])
            host_t = (HM.host_token(st["token"]) if (HM.enabled and HM.has(st["token"])) else (players[0] if players else None))
            if players and host_t and host_t != st["token"]:
                log("MATCH-END ignored (not from the host)", addr, tag(st["token"]), "host is", tag(host_t))
                continue
            winner = pl[0] if pl else 0xFF
            if winner not in (0, 1, 2):
                log("MATCH-END ignored (bad winner byte)", addr, tag(st["token"]), winner)
                continue
            log("MATCH-END", addr, "winner team", winner, "players", [tag(p) for p in players or []])
            evt("match-end", MM.mid_of(st["token"]), "winner team", winner, "reported by", tag(st["token"]),
                "after %.0f s" % MM.match_age(st["token"]), "players", [tag(p) for p in players or []])
            if players:
                with MM.lock:
                    left_early = set(MM.departed.get(MM._mid_locked(st["token"]), ()))
                mid_r = MM.mid_of(st["token"])
                if mid_r and mid_r in RESULT_BOOKED:
                    log("MATCH-END repeated for", mid_r, "- already booked, ignored")
                    continue
                if mid_r: RESULT_BOOKED.add(mid_r)
                record_result(players, winner, skip=left_early, mid=mid_r)
                if mid_r and winner in (0, 1, 2):
                    for i, t in enumerate(players):             # the clan chronicle: did this player's team win (read by clan_match_facts)
                        if t not in left_early:
                            CLAN_WIN[(t, mid_r)] = None if winner == 2 else (team_in(mid_r, i, t) == winner)
                    if len(CLAN_WIN) > 600:
                        CLAN_WIN.pop(next(iter(CLAN_WIN)))
                    try:                                        # who played on which team and who won: the RAIDS page's MATCH STATS list (clan_ingame section 5)
                        import clan_ingame
                        ros = [(t, get_name(t) or token_name(t) or "?", team_in(mid_r, i, t), t in left_early) for i, t in enumerate(players)]   # (names outside _dblock: deadlock)
                        with _dblock, dbc() as c:
                            clan_ingame.stats_roster(c, mid_r, CLAN_MAP.get(mid_r, ""), ros, winner)
                    except Exception as ex:                     # noqa: BLE001
                        log("CLAN match roster failed", repr(ex))
                if mid_r and mid_r in END_BY_REPORTS:       # the daemon closed the match from the clients' reports first: the late result still books the win,
                    log("MATCH-END after the client-report close of", mid_r, "- winner booked, nothing closed twice")   # the closers already ran
                    continue
                clan_match_ended(players, MM.mid_of(st["token"]))
                loaded_clear_match(MM.mid_of(st["token"]), players)
                # free the match for requeue only after the scoreboard: dropping it at once
                # made the clients quit to the menu right away (v1.11 notes; spec's build
                # held the scoreboard ~8 s, winmm's own hold is 10 s)
                mid_end = MM.mid_of(st["token"])
                if mid_end: MM.end(mid_end)
                if mid_end: INT.end(mid_end, winner)
                if mid_end: GL.end(mid_end)
                try: live_save()                            # fuzz seed 243: a restart in the 9 s scoreboard hold re-adopted the ended match as running
                except Exception as ex: log("live save after match end failed", ex)   # noqa: BLE001
                def _free(p0=players[0], m=mid_end, pl=list(players)):
                    if m: MM.drop(m)
                    else: MM.leave(p0)
                    if m: HM.end(m)
                    forget_match_state(pl)
                    evt("freed", m, [tag(p) for p in pl])
                threading.Timer(9.0, _free).start()

def loaded_clear_match(mid, players):
    """Forget the LOADED mark of a finished / dropped match's players - but NOT of a player who already plays in ANOTHER live match.
 once: a player had left match 83dc3bf8 (departed, but still listed in its player list) and was in 9435e28a; the host's result of
    83dc3bf8 cleared LOADED of every listed player, a player included -> 9435e28a looked 'not everybody loaded' for good and the join-in-progress
    GAME-START (0x0a) of a player and a player never went out: both sat on the loading screen in sub-state 5 for ever."""
    for p in players or []:
        cur = MM.mid_of(p)
        if cur is None or cur == mid:
            LOADED.discard(p)
        else:
            log("LOADED mark of", tag(p), "kept: it already plays in match", cur, "(match", mid, "ended / dropped)")

_JIP_WAIT_LOG = {}

def jip_requeue_if_empty(mid):
    """J3: every established player of a running match left while a joiner was still loading: a joiner must not be started alone into an
    empty world (PT79 d07f3e21: it sat alone for 30 s). Returns the joiners sent back to searching (they re-queue and may join another match)."""
    with MM.lock:
        pl = list(MM.matches.get(mid) or ())
        dep = set(MM.departed.get(mid, ()))
        joining = [t for t, (m, _) in MM.joining.items() if m == mid]
    if not joining:
        return []
    est = [p for p in pl if p not in dep and p not in joining]
    if est:
        return []
    for j in joining:
        LOADED.discard(j)
        c = SESSIONS.conn(j)
        leave_running_match(j, "match empty before its start")
        evt("jip-empty", mid, tag(j), "everybody else left the match: joiner sent back to searching")
        log("jip: match", mid, "has no established player left - joiner", tag(j), "goes back to searching")
        if c:
            try: c.sendall(msg(0x11, srv_note("jip_ended", j)) + searching_frame(j))
            except OSError: pass
    return joining

PRESTART_HOSTLOST_REQUEUE = os.environ.get("T2R_PRESTART_HOSTLOST_REQUEUE", "1") != "0"

def prestart_host_lost(mid, players, survivors):
    """ 32804413: the host (slot 0) crashed during the level load (session socket reset, removed 'session lost, never came back') and the
    survivors got their GAME-START anyway. They had never been welcomed by the host, so winmm could not reach it and the ext's host migration
    (armed only in the running match, and only for a welcomed client) never helped: "Could not reach host (timed out)" for everyone 30 s later.
    A match whose host is gone before it started has nobody to start it: it is called off and the survivors go back to SEARCHING at their old
    queue place (MM.abort, the same 0x11 notice + 0x03 searching frame that sends a loading join-in-progress player back to the queue,
    jip_requeue_if_empty), so the next match forms with a fresh host pick. No clan day: the match never went live."""
    if not PRESTART_HOSTLOST_REQUEUE:
        return False
    back = MM.abort(mid, only=set(survivors)) or []
    HM.end(mid)
    GL.end(mid)
    loaded_clear_match(mid, players)
    for p in back:
        CD_GOT.discard(p); _SILENT.pop(p, None); SESS_LOST.pop(p, None)
        c = SESSIONS.conn(p)
        if c:
            try: c.sendall(msg(0x11, srv_note("host_lost", p)) + searching_frame(p))
            except OSError: pass
    forget_match_state(players)
    log("MATCH-ABORT host lost before the start", mid, "host", tag(players[0]), "| back to searching:", [tag(p) for p in back])
    evt("reaped", mid, "host %s gone before the GAME-START: called off, survivors back in the queue at their old place" % tag(players[0]),
        [tag(p) for p in back])
    try: live_save()
    except Exception as ex: log("live save after the pre-start host loss failed", ex)   # noqa: BLE001
    return True

def maybe_game_start(token):
    """When ALL players still in the token's match have loaded -> send 0x0a (sets ctx+0x1b1) -> sub-state 5->6
    -> GAME STARTS. (Do NOT send 0x0b; ctx+0x1b2 must stay 0.) Players who left the match (departed) no longer
    count - before, a leaver kept everybody else on the loading screen for good. The bookkeeping (stats, host
    migration, mode) runs once per match. Returns True when the start went out."""
    mid, active = MM.active_of(token)
    players = MM.players_of(token)
    if not mid or not active:
        return False
    with MM.lock:
        running = mid in MM.started
    joiners = MM.jip_joiners(mid) if (JIP and running) else []
    if joiners and jip_requeue_if_empty(mid):
        return False
    if not joiners and not running and active:
        with MM.lock:
            pl_m = list(MM.matches.get(mid) or ())
            launched_m = mid in MM.launched
        if launched_m and pl_m and pl_m[0] not in active and prestart_host_lost(mid, pl_m, active):
            return False                                 # the host (slot 0) left during the load: nobody can start this match
    if not joiners and not all(p in LOADED for p in active):
        return False
    if joiners:
        # join in progress: the match is already going, so 0x0a (sub-state 5 -> 6) goes to the joiner(s) ONLY - the players in
        # the match must not get it again. It waits until the others' rosters list the joiner (jip_acked, max JIP_ACK_MAX s).
        # (only the JOINERS' own LOADED matters: the players already in the running match loaded long ago - requiring theirs too made one stale
        # LOADED mark of an established player block every joiner, see loaded_clear_match)
        go = [j for j in joiners if j in LOADED and MM.jip_acked(j)]
        for j in joiners:
            if j in LOADED and j not in go and time.time() - _JIP_WAIT_LOG.get(j, 0) > 5:
                _JIP_WAIT_LOG[j] = time.time()
                log("jip: GAME-START of", tag(j), "waits for the other players' roster refresh (%.0f s of max %.0f s)" % (MM.jip_age(j), JIP_ACK_MAX))
        for j in go:
            c = SESSIONS.conn(j)
            if c:
                try: c.sendall(msg(0x0a, b"\x01"))
                except OSError: pass
            done = MM.jip_done(j)
            JIP_STARTED[j] = (mid, time.time()); JIP_REPUSH.pop(j, None)
            if not PRIV.is_private_mid(mid):
                add_matches([j])
            clan_match_started([j], mid)
            log("jip: GAME-START 0x0a ->", tag(j), "(joined the running match", mid, "after %.0f s)" % (time.time() - (done[1] if done else time.time())))
            evt("jip-start", mid, tag(j))
        if go:
            live_save()          # (fuzz: a daemon restart within 10 s of a joiner's GAME-START forgot the joiner: the saved record listed the match without it)
        return bool(go)
    for p in active:
        c = SESSIONS.conn(p)
        if c:
            try: c.sendall(msg(0x0a, b"\x01"))
            except OSError: pass
    log("GAME-START 0x0a ->", [tag(p) for p in active])
    with MM.lock:
        first = mid not in MM.started
        MM.started.add(mid)
        MM.started_t[mid] = time.time()
    if first:
        evt("game-start", mid, "map", MAPS[map_index(players)][2], "players", [tag(p) for p in active])
        timing_summary(players)
        if not PRIV.is_private_mid(mid):
            add_matches(active)                          # (a private match is not a played match on the leaderboard)
        clan_match_started(players, mid)
        HM.start(mid, players)
        with MM.lock:
            gone_already = mid not in MM.matches
        if gone_already:                                 # (fuzz seed 86: the last player left between the GAME-START and this line; its leave had already run HM.end)
            HM.end(mid)
        with MM.lock:
            left_before = [p for p in players if p in MM.departed.get(mid, ())]
        for p in left_before:                           # S4: left during the load: its slot is gone from the first frame (a lost host migrates at once)
            if MM.mid_of(p) is None:
                HM.depart(p, "left before the start")
        mode = INT.start(mid, players, map_index(players), team_of, mode=MODEQ.match_mode(mid))
        log("MODE", mode, [tag(p) for p in players])
        live_save()
    return True

# Ext short-connection ops whose payload starts with the 64-hex-char token: each one is proof of life of a player that is already present (PRES).
# (link hold): a player whose winmm link is down sends no 0x05 / 0x15; the ext's own requests keep the reaper's silence test honest.
# Before this, only 0xE6 (every 2 s host / 10 s client) and 0xE3 (JIP on) / 0xBA touched PRES; any of these counts now. touch_known never creates a player.
EXT_PRESENCE_OPS = frozenset({0xBC, 0xCC, 0xD6, 0xE0, 0xE2, 0xE3, 0xE4, 0xE6, 0xE8, 0xEE, 0xEF, 0xBA, 0xDE, 0xCA, 0xB4, 0xB6, 0xD0})

def handle_batch(conn, addr, st, batch):
    ops = [op for op, _ in batch]
    done_end = False
    for op, pl in batch:
        if op in EXT_PRESENCE_OPS and len(pl) >= 64:
            tk = pl[:64].decode("latin1", "ignore")
            if is_token(tk):
                PRES.touch_known(tk)
    if 0x12 in ops:
        handle_match_end(addr, st, batch); done_end = True
    if 0x02 in ops:
        handle_leave(addr, st)

    # --- custom name / clan tag from t2r-ext (its own short connection) ---
    if any(op in (0xD0, 0xD2) for op in ops):
        for op, pl in batch:
            if op in (0xD0, 0xD2):
                if _creates_rows(op, pl, addr):
                    continue
                try:
                    rep = names_op(op, pl)
                except Exception as e:
                    log("NAMES ERR", addr, hex(op), e)
                    rep = None
                if rep:
                    conn.sendall(rep)
        return

    # --- anti-cheat: flag events / demo upload (t2r-ext) and player reports / recent players (launcher): own short connections ---
    if any(op in AC_OPS for op in ops):
        for op, pl in batch:
            if op in AC_OPS:
                try:
                    rep = {OP_AC_EVENTS: ac_events_op, OP_AC_DEMO: ac_demo_op, OP_AC_REPORT: ac_report_op, OP_AC_RECENT: ac_recent_op}[op](pl)
                except Exception as e:
                    log("AC ERR", addr, hex(op), repr(e))
                    rep = None
                if rep:
                    conn.sendall(rep)
        return

    # --- clan requests from t2r-ext (its own short connection) ---
    if any(op in (0xE0, 0xE2, 0xE4) for op in ops):
        for op, pl in batch:
            if op in (0xE0, 0xE2, 0xE4):
                if _creates_rows(op, pl, addr):
                    continue
                try:
                    rep = clan_op(op, pl)
                except Exception as e:
                    log("CLAN ERR", addr, hex(op), e)
                    rep = None
                if rep:
                    conn.sendall(rep)
        return

    # --- clan feed lines for the main menu notification (t2r-ext's own short connection): op 0xB4, see feed_op ---
    if 0xB4 in ops:
        for op, pl in batch:
            if op == 0xB4:
                try:
                    rep = feed_op(pl)
                except Exception as e:
                    log("FEED ERR", addr, repr(e))
                    rep = None
                if rep:
                    conn.sendall(rep)
        return

    # --- the rank table for the badge in front of every name (tools/rankbadge.py): op 0xBF [64B token] -> reply 0xBF ---
    if 0xBF in ops:
        for op, pl in batch:
            if op == 0xBF:
                try:
                    rep = rank_table_op(pl)
                except Exception as e:
                    log("RANKS ERR", addr, repr(e))
                    rep = None
                if rep:
                    conn.sendall(rep)
        return

    # --- achievements: op 0xA9 match counters (books) / 0xAA read / 0xAB live counters, see ach_report_op / ach_read_op / ach_live_op ---
    if 0xA9 in ops or 0xAA in ops or 0xAB in ops:
        for op, pl in batch:
            if op in (0xA9, 0xAA, 0xAB) and achievements:
                if _creates_rows(op, pl, addr):
                    continue
                try:
                    rep = ach_report_op(pl) if op == 0xA9 else ach_live_op(pl) if op == 0xAB else ach_read_op(pl)
                except Exception as e:
                    log("ACH ERR", addr, repr(e))
                    rep = None
                if rep:
                    conn.sendall(rep)
        return

    # --- the in-game CLAN page (to1/clan13): op 0xB6, see clan13_op ---
    if 0xB6 in ops:
        for op, pl in batch:
            if op == 0xB6:
                try:
                    rep = clan13_op(pl)
                except Exception as e:
                    log("CLAN13 ERR", addr, repr(e))
                    rep = None
                if rep:
                    conn.sendall(rep)
        return

    # --- boosters (t2r-ext's own short connection) ---
    if any(op in (0xE8, 0xEE, 0xEF) for op in ops):
        for op, pl in batch:
            if op in (0xE8, 0xEE, 0xEF):
                if _creates_rows(op, pl, addr):
                    continue
                try:
                    rep = meta_op(op, pl)
                except Exception as e:
                    log("META ERR", addr, hex(op), e)
                    rep = None
                if rep:
                    conn.sendall(rep)
        return

    # --- join in progress: clock report + roster refresh from t2r-ext (its own short connection) ---
    if 0xE3 in ops:
        for op, pl in batch:
            if op == 0xE3:
                try:
                    rep = jip_roster_op(pl)
                    if rep:
                        conn.sendall(rep)
                except (ConnectionError, OSError):
                    raise
                except Exception as e:
                    log("jip: ERR", addr, repr(e))
        if set(ops) <= {0xE3}:
            return
    # --- host migration poll from t2r-ext (its own short connection) ---
    if 0xE6 in ops:
        for op, pl in batch:
            if op == 0xE6 and len(pl) >= 64:
                t = pl[0:64].decode("latin1", "ignore")
                flags = pl[64] if len(pl) > 64 else 0
                epoch = pl[65] if len(pl) > 65 else 0
                # the ext polls this every 0.5-2 s for the whole match: proof the player is there. The reaper's silence test reads PRES,
                # which only the session keepalive (0x05 / 0x15) touched - a match whose keepalives stop (in-match sockets, a busy
                # client) would be reaped as 'every player offline' / 'timed out' while it is being played
                readopt_claim(t)
                PRES.touch_known(t)
                if flags & 1 and JIP:
                    try: jip_unreachable(t)
                    except Exception as e: log("jip: REPUSH ERR", repr(e))
                try:
                    conn.sendall(msg(0xE7, HM.poll(t, flags, epoch)))
                except Exception as e:
                    log("HOSTMIG ERR", addr, e)
        if set(ops) <= {0xE6}:
            return                                # (it used to fall through to the 'SESSION ops=[0xe6]' log line every 2 s)
    # --- HWID report from t2r-ext (its own short connection): op 0xB2, see hwid_op ---
    if OP_HWID in ops:
        for op, pl in batch:
            if op == OP_HWID:
                try:
                    conn.sendall(hwid_op(pl, addr[0] if addr else None))
                except (ConnectionError, OSError):
                    raise
                except Exception as e:
                    log("HWID ERR", addr, e)
        return
    # --- version check from the launcher (Play.ps1) / any client: op 0xB0, see version_op ---
    if OP_VERSION in ops:
        for op, pl in batch:
            if op == OP_VERSION:
                try:
                    conn.sendall(version_op(pl, addr))
                except (ConnectionError, OSError):
                    raise
                except Exception as e:
                    log("VERSION ERR", addr, e)
        return
    # --- server notices (t2r-ext short connection, every ~6 s in a match and in the menus): op 0xBA, see notice_op ---
    if notices.OP_REQ in ops:
        for op, pl in batch:
            if op == notices.OP_REQ:
                try:
                    rep = notice_op(pl)
                except Exception as e:
                    log("NOTICE ERR", addr, repr(e))
                    rep = None
                if rep:
                    conn.sendall(rep)
        return
    # --- private matches (t2r-ext short connection): op 0xF8, see priv_op ---
    if private_lobby.OP_REQ in ops:
        for op, pl in batch:
            if op == private_lobby.OP_REQ:
                try:
                    rep = priv_op(pl)
                except Exception as e:
                    log("PRIVATE ERR", addr, repr(e))
                    rep = None
                if rep:
                    conn.sendall(rep)
        return
    # --- party ops (t2r-ext short connection) ---
    if any(op == 0xDA for op in ops):
        for op, pl in batch:
            if op == 0xDA:
                try:
                    rep = party_op(pl)
                except Exception as e:
                    log("PARTY ERR", addr, e)
                    rep = None
                if rep:
                    conn.sendall(rep)
        return
    # --- friends + parties (the launcher's own short connection): op 0xD8, see social_op ---
    if social.OP_REQ in ops:
        for op, pl in batch:
            if op == social.OP_REQ:
                try:
                    rep = social_op(pl, addr)
                except Exception as e:
                    log("SOCIAL ERR", addr, repr(e))
                    rep = None
                if rep:
                    conn.sendall(rep)
        return
    # --- per-team gun limits (t2r-ext short connection): op 0xDE, see gunlimit_op ---
    if any(op == gunlimit.OP_REQ for op in ops):
        for op, pl in batch:
            if op == gunlimit.OP_REQ:
                try:
                    rep = gunlimit_op(pl)
                except Exception as e:
                    log("GUNLIMIT ERR", addr, e)
                    rep = None
                if rep:
                    conn.sendall(rep)
        return
    # --- per-mode queues (t2r-ext short connection): op 0xBC, see modeq_op ---
    if any(op == modeq.OP_REQ for op in ops):
        for op, pl in batch:
            if op == modeq.OP_REQ:
                try:
                    rep = modeq_op(pl)
                except Exception as e:
                    log("MODEQ ERR", addr, repr(e))
                    rep = None
                if rep:
                    conn.sendall(rep)
        return
    # --- Interrogation mode (t2r-ext short connection) ---
    if any(op == interrogation.OP_REQ for op in ops):
        for op, pl in batch:
            if op == interrogation.OP_REQ:
                try:
                    rep = interrogation_op(pl)
                except Exception as e:
                    log("INTERROGATION ERR", addr, e)
                    rep = None
                if rep:
                    conn.sendall(rep)
        return
    # --- performance report from t2r-ext (its own short connection) ---
    if hostmig.OP_PERF in ops:
        for op, pl in batch:
            if op == hostmig.OP_PERF:
                try:
                    rep = perf_op(pl)
                except Exception as e:
                    log("PERF ERR", addr, e)
                    rep = None
                if rep:
                    conn.sendall(rep)
        return
    # --- SP-section map support from t2r-ext (its own short connection) ---
    if map_packs.SP_OP_REPORT in ops:
        for op, pl in batch:
            if op == map_packs.SP_OP_REPORT:
                try:
                    rep = sp_support_op(pl)
                except Exception as e:
                    log("SPMAPS ERR", addr, e)
                    rep = None
                if rep:
                    conn.sendall(rep)
        return
    # --- map packs from t2r-ext (its own short connection) ---
    if map_packs.OP_REPORT in ops:
        for op, pl in batch:
            if op == map_packs.OP_REPORT:
                try:
                    rep = map_packs_op(pl)
                except Exception as e:
                    log("PACKS ERR", addr, e)
                    rep = None
                if rep:
                    conn.sendall(rep)
        return
    # --- map vote from t2r-ext (its own short connection) ---
    if any(op in (0xEA, 0xEC) for op in ops):
        for op, pl in batch:
            if op in (0xEA, 0xEC):
                try:
                    rep = vote_op(op, pl)
                except Exception as e:
                    log("VOTE ERR", addr, hex(op), e)
                    rep = None
                if rep:
                    conn.sendall(rep)
        return

    # --- bridge register (its own connection) ---
    for op, pl in batch:
        if op == 0xF0 and len(pl) >= 73:
            t = pl[0:64].decode("latin1", "ignore")
            steamid = int.from_bytes(pl[64:72], "little")
            nl = pl[72]; name = clean_str(pl[73:73 + nl], 31) or "PLAYER"
            port = int.from_bytes(pl[73 + nl:75 + nl], "little") if len(pl) >= 75 + nl else 0
            if not is_token(t) or not (STEAMID_MIN <= steamid < STEAMID_MAX) or not (0 < port < 65536):
                log("BRIDGE", addr, "refused: malformed registration")
                return
            if not token_known(t) and not NEW_TOKENS.allow(addr[0]):
                log("BRIDGE", addr, "refused: too many new identities from this address")
                return
            st["token"] = t; st["name"] = name; st["is_bridge"] = True
            set_steam(t, steamid, name)
            if HM.enabled and HM.has(t):
                HM.bridge_back(t)
            BRIDGES.register(t, conn, steamid, port, addr[0], name)
            PING_LOC.pop(t, None)                          # resend its location to the others
            game_tok = NAMELESS_BY_IP.get(addr[0])
            if game_tok and game_tok != t:
                adopt_bridge_name(game_tok, addr[0])
            log("BRIDGE", addr, name, "steamid", steamid, "port", port, "token", tag(t))
            ident_note(t, steamid=steamid, ip=addr[0] if addr else None)
            try: conn.sendall(msg(0xF1))                   # registration ack: the helper reports "server up" to the menu only after a real reply
            except OSError: pass
            return
    if st["is_bridge"]:
        for op, pl in batch:
            if op == 0xF4 and pl:
                loc = pl.decode("latin1", "ignore")
                if len(loc) > LOC_MAX or not LOC_OK.match(loc):
                    log("PING-LOC", tag(st["token"]), "refused: malformed location string (%d B)" % len(pl))
                    continue
                ping_location(st["token"], loc)
            elif op == 0xF6 and len(pl) >= 12:
                ping_report(st["token"], int.from_bytes(pl[0:8], "little"), int.from_bytes(pl[8:12], "little", signed=True))
        return                                             # helper keepalive (0xF3)

    # --- persist any profile/login; learn token ---
    for op, pl in batch:
        if op == 0x0f:
            t, loadout, _ = parse_login(pl)
            if t:
                readopt_claim(t)
                st["token"] = t
                if AC is not None and addr:
                    _iss = AC.note_ip(t, addr[0])              # one token from many addresses = a shared / stolen token (scored low, never acts alone)
                    if _iss:
                        ac_server_event(t, CLAN_LAST_MATCH.get(t) or "00000000", _iss[0], _iss[1], _iss[2])
                if loadout is not None and new_identity_ok(t, addr):
                    save_loadout(t, loadout)
                adopt_bridge_name(t, addr[0])
                PRES.touch(t, get_name(t) or st["name"])
        elif op in (0x01, 0x0e):
            nm = name_from_session(pl)
            if nm:
                nm = clean_str(nm, 31)
                st["name"] = nm
                # (a frame without a login used to create a player row keyed by the NAME: 47 junk rows from the open internet in the live DB)
                if nm and nm != "PLAYER" and st["token"] and is_token(st["token"]) and not get_name(st["token"]):   # skip echoed default
                    set_name(st["token"], nm)

    tok = st["token"] or st["name"]

    # --- match result: host sends op 0x12 [winning team] when the match ends
    #     (FUN_180062b00); clients show the scoreboard, then return to the menu. ---
    if not done_end:
        handle_match_end(addr, st, batch)

    # --- menu data requests (answered whatever else the batch holds) ---
    if 0x15 in ops:      # online-count poll, every 3s on the MP menu (FUN_1800590d0)
        conn.sendall(msg(0x16, struct.pack("<I", max(1, PRES.count()))))
    if 0x13 in ops:      # LEADERBOARDS page opened (FUN_180058f70)
        conn.sendall(msg(0x14, leaderboard_table()))

    # --- decide response by batch content ---
    if 0x01 in ops or 0xfc in ops:                        # MATCHMAKING (host/join / select)
        # ONLY match on a real 64-hex token. 0xfc/bare conns have no 0x0f -> would
        # enqueue as the fallback name "PLAYER" and corrupt identity/delivery.
        realtok = st["token"] if (st["token"] and len(st["token"]) == 64) else None
        if st.get("closed_sent"):
            # FIND MATCH pressed while our boot reply (ending in 0x0d) was on its way: the
            # client sees that 0x0d after it entered searching -> session state 5 -> winmm's
            # page quits to the TITLE (FUN_180065480). Nothing we send now reaches it, so do
            # not queue a ghost; its next press comes on a fresh conn (re/mm-boot.md).
            log("MATCHMAKE", addr, "after 0x0d on this conn (client drops to the title); not queued",
                tag(realtok) if realtok else st["name"])
            return
        st["session"] = True
        conn.sendall(searching_frame(realtok))             # ack -> state 3 (searching) + queue names, NO 0x0d
        if not realtok:
            log("MATCHMAKE", addr, f"ops={[hex(o) for o in ops]} (no token yet, ack only)")
            return
        if os.environ.get("T2R_STEAM_ONLY", "1") != "0" and realtok not in LINUX_CLIENTS and not (
                BRIDGES.steamid(realtok) and BRIDGES.port(realtok) and BRIDGES.conn(realtok)):
            # Steam-only matches (no server relay): a player whose Steam helper isn't
            # registered is not queued; the client (ext) shows why
            key, helper = no_helper_note(realtok, addr[0])
            log("MATCHMAKE", addr, "refused (no Steam helper)", tag(realtok),
                f"- helper {helper!r} on this IP: game files changed (substitute token)" if helper else "")
            MM.rq.drop(realtok)                            # (a refused search ends a requeue ticket - its group does not wait for it)
            conn.sendall(msg(0x11, srv_note(key, realtok)))
            return
        if is_banned(realtok, st.get("name"), addr[0] if addr else None):
            log("MATCHMAKE", addr, "refused (banned)", tag(realtok), ident_pid(realtok) or "")
            MM.rq.drop(realtok)
            conn.sendall(msg(0x11, ban_note(realtok)) + msg(0x0d, b"\x02"))   # note, then 0x0d [2]: winmm quits to the title (boat menu)
            return
        ir = install_refusal(realtok, addr[0] if addr else None)
        if ir:
            log("MATCHMAKE", addr, "refused (install)", tag(realtok), ir[0])
            MM.rq.drop(realtok)
            conn.sendall(msg(0x11, ir[1]) + msg(0x0d, b"\x02"))
            return
        vp = version_refusal(realtok)
        if vp:
            log("MATCHMAKE", addr, "refused (version)", tag(realtok), CLIENT_VERSIONS.get(realtok), "!=", SERVER_VERSION)
            MM.rq.drop(realtok)
            conn.sendall(msg(0x11, vp.encode("utf-8", "replace")[:0xf0]))
            return
        lh = None if MM.in_match(realtok) else loss_hold_note(realtok)
        if lh:
            log("MATCHMAKE", addr, "refused (packet loss hold)", tag(realtok))
            MM.rq.drop(realtok)
            conn.sendall(msg(0x11, lh))
            return
        SESSIONS.register(realtok, conn)                   # hold conn for match-start push
        PRES.touch(realtok, get_name(realtok) or st["name"])
        if MM.in_match(realtok) and (realtok in LOADED or MM.match_age(realtok) > 120):
            # (or: launched > 2 min ago and never loaded = crashed / killed while loading)
            # searching again although its match already started (game crashed / killed mid-match,
            # or left it): that match is over for this player. Holding it made the client show
            # MATCH FOUND forever (the old start re-sent, no countdown).
            log("MATCHMAKE", addr, "stale match (already started) dropped for", tag(realtok))
            LOADED.discard(realtok)
            leave_running_match(realtok, "searching again")
        if MM.in_match(realtok):
            # re-queued (client auto-cancel) while its match is loading: send the start right
            # here - some clients never send a keepalive on this conn, so they never got it
            if st.get("started") != MM.mid_of(realtok) and realtok not in LOADED:
                conn.sendall(match_start_frame(realtok, MM.players_of(realtok)))
                st["started"] = MM.mid_of(realtok)
                log("MATCHMAKE", addr, "already in match; start re-sent", tag(realtok))
                if MM.countdown_over(realtok) or not SESSIONS.conn(realtok):
                    late_countdown(realtok, conn)
            else:
                log("MATCHMAKE", addr, "already in match; holding")
        elif PARTIES.pid(realtok) and SOCIAL.search_gate(realtok, party_lead_searching):
            # party rule: a member does not search - his FIND MATCH press is READY UP / UNREADY (the client sends the ready op itself)
            # and the leader's search takes the ready members along; the leader waits until every mate is ready. Not queued; the client pops the search page.
            log("MATCHMAKE", addr, "party FIND MATCH not queued (member = ready toggle while the leader is not searching / leader = a mate is not ready)", tag(realtok))
        else:
            mid, players = MM.enqueue(realtok)
            log("MATCHMAKE", addr, f"ops={[hex(o) for o in ops]}", "tok", tag(realtok), "-> mid", mid)
            mesh_activate(realtok)                         # ping mesh: swap Steam locations with the searchers / in-match players now
            if not mid and PARTIES.pid(realtok):
                try: SOCIAL.on_queue(realtok, lambda m: m in MM.queue or MM.in_match(m))      # tells the party mates to press FIND MATCH too
                except Exception as e: log("SOCIAL queue note failed", repr(e))
            if mid:
                push_peers(players)
                start_match(players)
            elif JIP:
                try: jip_pass()                            # no new match: a running one with room? (jip:)
                except Exception as e: log("jip: ERR", repr(e))
    elif 0x0f in ops:                                     # BOOT handshake
        if not st["authed"] and st["token"] and len(st["token"]) == 64:
            ident_note(st["token"], ip=addr[0] if addr else None)          # identity id (Steam id + hardware + IP), ident.py (optional)
        if not st["authed"] and st["token"] and len(st["token"]) == 64 and is_banned(st["token"], st["name"], addr[0] if addr else None):
            # FACTIONS II pressed on the boat menu: the client opens the session and logs in. Refuse right here: the note (op 0x11,
            # shown by the MP-menu page tick) + 0x0d with payload 2 = spec's MM_REJECT variant that sets winmm's flag 0x180167979, which
            # the same page tick turns into FUN_180065480 = quit to the title (boat menu). Nothing is queued / announced / counted online.
            log("BOOT", addr, "refused (banned)", tag(st["token"]), "steam", BRIDGES.steamid(st["token"]) or token_steamid(st["token"]),
                ident_pid(st["token"]) or "", ("ip ban " + str(ip_banned(addr[0]))) if addr and ip_banned(addr[0]) else "")
            conn.sendall(msg(0x11, ban_note(st["token"])) + msg(0x0d, b"\x02"))
            st["closed_sent"] = True
            return
        if not st["authed"] and st["token"] and len(st["token"]) == 64:
            # an edited install: refused the same way, before anything is announced / counted online
            ir = install_refusal(st["token"], addr[0] if addr else None)
            if ir:
                log("BOOT", addr, "refused (install)", tag(st["token"]), ir[0])
                conn.sendall(msg(0x11, ir[1]) + msg(0x0d, b"\x02"))
                st["closed_sent"] = True
                return
        if not st["authed"] and 0x0e in ops and session_reattach(conn, addr, st):
            return
        if not st["authed"]:
            extra = b""
            shown = get_name(tok) or st["name"]
            if 0x0e in ops:
                PRES.touch(tok, shown)
                extra = (msg(0x16, struct.pack("<I", max(1, PRES.count())))
                         + msg(0x14, leaderboard_table()))
            if 0x0e in ops and not ANNOUNCE_CLOSE:
                # MP-menu announce (spec's MM_HELLO): keep this socket OPEN, no 0x0d. winmm's
                # FIND MATCH reuses a connected session socket (FUN_180057110, state 2: it just
                # appends op 0x01). The old trailing 0x0d closed it ~0.3 s after every announce
                # (~every 3 s); a press inside that window got the 0x0d AFTER entering the
                # searching state -> state 5 -> the page quit to the TITLE screen and the
                # queue dropped it 20 s later ("FIND MATCH sometimes boots back to main menu",
                # playtest33/37 logs: 12 of 13 queue drops). Held, the client keeps state 2,
                # heartbeats 0x05 + polls 0x15/0x13 on it, and the press arrives here.
                conn.sendall(auth_reply_open(shown, extra))   # 0x10+0x08+extra, NO 0x0d
                st["menu"] = True
                st["sent_name"] = shown
            else:
                conn.sendall(auth_reply(shown, extra))        # 0x10+0x08[+extra]+0x0d
                st["closed_sent"] = True
            st["authed"] = True
        if 0x0e in ops:                                   # presence announce (NOT queue)
            PRES.touch(tok, get_name(tok) or st["name"])
            log("ANNOUNCE", addr, st["name"])
    elif 0x09 in ops:                                     # client "level loaded / ready"
        t = st["token"]
        if t:
            LOADED.add(t); LOADED_T[t] = time.time()
            players = MM.players_of(t)
            tm = TIMING.get(frozenset(players)) if players else None
            if tm and tag(t) not in tm["loaded"]: tm["loaded"][tag(t)] = time.time()
            log("LOADED", addr, tag(t), "match", [tag(p) for p in players] if players else None)
            evt("loaded", MM.mid_of(t), tag(t))
            maybe_game_start(t)
    elif 0x05 in ops or 0x15 in ops:                      # session keepalive / poll
        if st["token"]: PRES.touch(st["token"], st["name"])
        tok2 = st["token"]
        if st.get("menu") and tok2 and (not st["session"] or (not MM.in_match(tok2) and tok2 in MM.queued())):
            # held menu socket: a name changed in-game (0xD2) reaches the client's own-name
            # (row 0 of the lobby list, nameplates) like the old 3 s re-announce did. Also while the player sits in the
            # matchmaking queue (the queued player's own row 0 kept the old name until the next game start); the
            # sent_name dedupe means one 0x10 per rename, never in a running match
            shown = safe_name(get_name(tok2) or st["name"])
            if shown != st.get("sent_name"):
                conn.sendall(msg(0x10, shown.encode()[:31].ljust(32, b"\x00")))
                st["sent_name"] = shown
                log("NAME-REFRESH", addr, tag(tok2), "->", shown)
        if st.get("menu") and tok2 and not MM.in_match(tok2) and tok2 not in MM.queued():
            # the lobby list shows the party (header "PARTY n/4" + the mates) as soon as it exists, before FIND MATCH, and follows
            # joins / leaves / kicks / disbands (an emptied list is sent once so the rows clear); T2R_NO_PARTY_LOBBY=1 = off
            try:
                if os.environ.get("T2R_NO_PARTY_LOBBY") != "1":
                    pn = party_lobby_names(tok2)
                    if pn != st.get("sent_lobby", []):
                        conn.sendall(lobby_list_frame(pn))
                        st["sent_lobby"] = pn
                        log("LOBBY-PARTY", addr, tag(tok2), [x.decode("utf-8", "ignore") for x in pn])
            except OSError:
                raise
            except Exception as ex:                                # noqa: BLE001 - the lobby list never breaks the keepalive
                log("LOBBY-PARTY failed", repr(ex))
        if st["session"] and tok2 and MM.in_match(tok2):
            # matched: push the match-start on THIS (live keepalive) conn, once.
            # The transient matchmaking conn that formed the match may be stale, so
            # the persistent keepalive conn is the reliable place to deliver it.
            if st.get("started") != MM.mid_of(tok2):
                conn.sendall(match_start_frame(tok2, MM.players_of(tok2)))
                st["started"] = MM.mid_of(tok2)
                log("START-ON-KEEPALIVE", addr, tag(tok2))
                if MM.countdown_over(tok2) or not SESSIONS.conn(tok2):
                    late_countdown(tok2, conn)
        elif st["session"]:
            if tok2: MM.poll(tok2)                         # queue heartbeat
            conn.sendall(searching_frame(tok2))            # still searching -> state 3 + queue names
        return
    elif 0x15 in ops or 0x07 in ops or 0x02 in ops:       # polls — noop (0x02 was handled first)
        pass
    else:
        log("SESSION", addr, f"ops={[hex(o) for o in ops]}",
            " | ".join(pl.hex(' ')[:40] for _, pl in batch))

BIND_RETRY_S = float(os.environ.get("T2R_BIND_RETRY_S", "30"))   # a taken port is retried this long before the FATAL exit 3
def bind_exclusive(s, kind, host=None, port=None, fatal=True):
    """Exclusive bind with bounded retry. On Windows SO_REUSEADDR lets a second daemon bind the same port (port stealing: the two split
    the relay), so SO_EXCLUSIVEADDRUSE is used and a taken port fails loudly. A restart right after a kill can see the port busy for a
    few seconds (dying process, handles not yet released): retry every 0.25 s up to BIND_RETRY_S (log at 1 s and then every 5 s), then
    FATAL + exit 3 (T2R-Host.ps1 shows a hint and does not restart). fatal=False raises the last OSError instead of exiting."""
    host = HOST if host is None else host; port = PORT if port is None else port
    if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    elif os.name != "nt":
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)    # POSIX: only lets a listener rebind over TIME_WAIT, never steals a live port
    t0 = time.monotonic(); deadline = t0 + BIND_RETRY_S; nxt = t0 + 1.0
    while True:
        try:
            s.bind((host, port))
            if time.monotonic() - t0 > 0.3:
                print(f"BIND {kind} {host}:{port} ok after {time.monotonic() - t0:.1f}s of retries", flush=True)
            return
        except OSError as e:
            now = time.monotonic()
            if now >= deadline:
                if not fatal: raise
                print(f"FATAL {kind} {host}:{port} already in use after {BIND_RETRY_S:.0f}s (is another server running?): {e}", flush=True)
                os._exit(3)
            if now >= nxt:
                print(f"BIND {kind} {host}:{port} busy ({e}); retrying for up to {deadline - now:.0f}s more", flush=True)
                nxt = now + 5.0
            time.sleep(0.25)

_tcp_refused = [0]
def tcp_server():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    bind_exclusive(s, "TCP"); s.listen(256)
    log("TCP LISTEN", f"{HOST}:{PORT}")
    while True:
        try:
            c, a = s.accept()
        except OSError as e:                      # e.g. too many open files / aborted accept: never end the accept loop
            log("TCP-ACCEPT-ERR", repr(e)); time.sleep(0.05); continue
        capped = PER_IP_LOOPBACK or a[0] != "127.0.0.1"
        with _tcp_live_lock:
            full = _tcp_live[0] >= MAX_TCP_CONNS
            ipfull = capped and _tcp_per_ip[a[0]] >= MAX_PER_IP
            if not full and not ipfull:
                _tcp_live[0] += 1
                if capped: _tcp_per_ip[a[0]] += 1
        if full or ipfull:
            if _tcp_refused[0] % 50 == 0:
                log("TCP", a, "refused: " + ("too many connections (%d)" % MAX_TCP_CONNS if full else "%d live connections from this address" % MAX_PER_IP))
            _tcp_refused[0] += 1
            c.close(); continue
        def run(c=c, a=a, capped=capped):
            try: tcp_conn(c, a)
            finally:
                with _tcp_live_lock:
                    _tcp_live[0] -= 1
                    if capped:
                        _tcp_per_ip[a[0]] -= 1
                        if _tcp_per_ip[a[0]] <= 0: del _tcp_per_ip[a[0]]
        try:
            threading.Thread(target=run, daemon=True).start()
        except Exception as e:                    # e.g. "can't start new thread": the slot and the socket are released, the accept loop goes on
            log("TCP-THREAD-ERR", repr(e))
            with _tcp_live_lock:
                _tcp_live[0] -= 1
                if capped:
                    _tcp_per_ip[a[0]] -= 1
                    if _tcp_per_ip[a[0]] <= 0: del _tcp_per_ip[a[0]]
            try: c.close()
            except OSError: pass
            time.sleep(0.05)

# ---------------------------------------------------------------- UDP
RELAY_IDLE_S = float(os.environ.get("T2R_RELAY_IDLE_S", "600"))   # a relay slot without a packet for this long is dead (was 1800: junk HELLOs lived half an hour)
UDP_MAX = int(os.environ.get("T2R_UDP_MAX", "2048"))          # gameplay packets are < 1.5 KB (MTU); bigger = junk, dropped
UDP_RATE_PPS = float(os.environ.get("T2R_UDP_PPS", "3000"))   # per source address: sustained packets / s (the host sends ~450 / s)
UDP_RATE_BPS = float(os.environ.get("T2R_UDP_BPS", str(6 * 1024 * 1024)))   # ... and bytes / s
UDP_BURST_S = 1.5                                              # bucket depth in seconds of the rate
RELAY_REBIND_S = 5.0                                           # a slot bound to another address this recently is not stolen by DATA
RELAY_STRICT = os.environ.get("T2R_RELAY_STRICT", "0") == "1"  # 1 = relay only sids of matches the server formed


class Bucket:
    """Token bucket for one source: packets and bytes."""
    __slots__ = ("pk", "by", "t", "dropped", "last_log")
    def __init__(self, now):
        self.pk = UDP_RATE_PPS * UDP_BURST_S
        self.by = UDP_RATE_BPS * UDP_BURST_S
        self.t = now; self.dropped = 0; self.last_log = 0.0
    def allow(self, size, now):
        dt = max(0.0, now - self.t); self.t = now
        self.pk = min(UDP_RATE_PPS * UDP_BURST_S, self.pk + dt * UDP_RATE_PPS)
        self.by = min(UDP_RATE_BPS * UDP_BURST_S, self.by + dt * UDP_RATE_BPS)
        if self.pk < 1 or self.by < size:
            self.dropped += 1
            return False
        self.pk -= 1; self.by -= size
        return True


class UdpRelay:
    """The UDP side of the daemon, separated from the socket so tests can feed it packets and a fake send().
    Opcodes: 03 keepalive (presence + relay to other keepalive sockets), 00 gameplay HELLO [sid][slot], 02 gameplay
    DATA [sid][src][dst][payload] relayed to the slot dst (or all but src for 0xFF), anything else relayed to the
    other 'gameplay' sockets. Hardening: packets over UDP_MAX are dropped, every source address has a token
    bucket (a flood or a stuck client cannot starve the loop or flood the others), a slot bound to another address
    is not taken over by a DATA packet within RELAY_REBIND_S of its last packet (a HELLO always rebinds), malformed
    packets are counted and never raise, a failed send to one peer never delays the others. The relay does not
    reorder, dedupe or retransmit: the game's own sequence numbers (T2RO) do that, and a lossy link of one peer only
    loses that peer's packets (each target is sent independently)."""
    def __init__(self, send, sim_send=None):
        self.send = send                # send(data, addr)
        self.sim_send = sim_send        # test-only lag / loss simulation: sim_send(data, addr) or None
        self.seen = {}
        self.KA = {}                    # addr -> last-seen ts of 03 keepalive sockets
        self.GP = {}                    # addr -> last-seen ts of non-03 sockets
        self.STALE = 15.0
        self.buckets = {}
        self.counters = collections.Counter()    # oversized / rate-limited / malformed / hijack-refused / no-target
        self.last_prune = 0.0
        self.strict_sids = None         # (time, set) cache for RELAY_STRICT
        # health numbers (UDP-HEALTH line every status tick): packet size histogram, per-address packets in / out,
        # the slowest handle() (a long one = this thread was starved, e.g. by the GIL while another thread wrote the db or the log)
        self.sizes = collections.Counter()
        self.pk_in = collections.Counter()
        self.pk_out = collections.Counter()
        self.max_handle_ms = 0.0
        self.sock_stats = ""

    def _emit(self, data, target):
        self.pk_out[target] += 1
        if self.sim_send:
            self.sim_send(data, target)
            return
        try: self.send(data, target)
        except OSError: self.counters["send-failed"] += 1

    def health_line(self):
        """One line with the numbers since the last call, then reset."""
        # (the UDP thread keeps adding keys: work on copies - a "changed size during iteration" error here cost the whole status tick)
        sizes, pk_in, pk_out = collections.Counter(dict(self.sizes)), collections.Counter(dict(self.pk_in)), collections.Counter(dict(self.pk_out))
        n = len(sizes) and sum(sizes.values())
        hist = " ".join("%s:%d" % (k, sizes[k]) for k in ("<=128", "<=256", "<=512", "<=1024", "<=1200", ">1200") if sizes[k])
        top_in = ", ".join("%s:%d" % (f"{a[0]}:{a[1]}", c) for a, c in pk_in.most_common(6))
        top_out = ", ".join("%s:%d" % (f"{a[0]}:{a[1]}", c) for a, c in pk_out.most_common(6))
        line = "in %d pkts | sizes %s | slowest handle %.2f ms | in/peer %s | out/peer %s | %s" % (
            n, hist or "-", self.max_handle_ms, top_in or "-", top_out or "-", self.sock_stats)
        self.sizes.clear(); self.pk_in.clear(); self.pk_out.clear(); self.max_handle_ms = 0.0
        return line

    TABLE_MAX = 512
    def _relay_table(self, table, data, src, now):
        if src not in table and len(table) >= self.TABLE_MAX:      # one packet is sent to EVERY other entry: a source-port spray made that O(table)
            for a in [a for a, ts in table.items() if now - ts > self.STALE]:
                table.pop(a, None)
            while len(table) >= self.TABLE_MAX:
                table.pop(min(table, key=table.get), None)
        table[src] = now
        for a, ts in list(table.items()):
            if now - ts > self.STALE:
                table.pop(a, None)
            elif a != src:
                self._emit(data, a)

    def _known_sid(self, sid, now):
        if not RELAY_STRICT:
            return True
        if self.strict_sids is None or now - self.strict_sids[0] > 2.0:
            self.strict_sids = (now, {match_sid(pl) for _, pl, *_ in MM.snapshot()})
        return sid in self.strict_sids[1]

    RELAY_MAX = 4096
    def _relay_room(self, now):
        """A HELLO / DATA with any sid creates a relay slot (T2R_RELAY_STRICT is off): junk packets filled RELAY for the 30 min its entries live. At
        RELAY_MAX slots the idle ones go first, then the oldest quarter."""
        if len(RELAY) < self.RELAY_MAX:
            return
        for k in [k for k, t in list(RELAY_TS.items()) if now - t > 120]:
            RELAY.pop(k, None); RELAY_TS.pop(k, None)
        if len(RELAY) >= self.RELAY_MAX:
            for k in sorted(RELAY_TS, key=RELAY_TS.get)[:self.RELAY_MAX // 4]:
                RELAY.pop(k, None); RELAY_TS.pop(k, None)
            self.counters["relay-evicted"] += 1
    def prune(self, now):
        if now - self.last_prune < 60:
            return
        self.last_prune = now
        for k in [k for k, t in list(RELAY_TS.items()) if now - t > RELAY_IDLE_S]:       # slots of finished matches (every packet refreshes its slot)
            RELAY.pop(k, None); RELAY_TS.pop(k, None)
        for a in [a for a, b in self.buckets.items() if now - b.t > 120]:
            del self.buckets[a]
        if len(self.seen) > 2000:               # (log-throttle memory: only the first few packets of a source are logged)
            self.seen.clear()

    def handle(self, data, addr, now=None):
        t0 = time.perf_counter()
        try:
            self._handle(data, addr, time.time() if now is None else now)
        finally:
            ms = (time.perf_counter() - t0) * 1000.0
            if ms > self.max_handle_ms:
                self.max_handle_ms = ms

    def _handle(self, data, addr, now):
        self.prune(now)
        if len(self.seen) > 4000:               # (the once-a-minute prune is too slow for a source-port spray: 70k packets / s)
            self.seen.clear()
        n = len(data)
        self.sizes["<=128" if n <= 128 else "<=256" if n <= 256 else "<=512" if n <= 512 else "<=1024" if n <= 1024 else "<=1200" if n <= 1200 else ">1200"] += 1
        self.pk_in[addr] += 1
        if len(data) > UDP_MAX:
            self.counters["oversized"] += 1
            if self.counters["oversized"] in (1, 10, 100, 1000):
                log("UDP-DROP oversized", addr, f"{len(data)}B (#{self.counters['oversized']})")
            return
        b = self.buckets.get(addr)
        if b is None:
            if len(self.buckets) > 4096:          # address spray: forget the quiet ones first; only when all are busy stop tracking and drop the new ones
                for a in [a for a, bb in self.buckets.items() if now - bb.t > 10]:
                    del self.buckets[a]
            if len(self.buckets) > 4096:
                self.counters["rate-limited"] += 1
                return
            b = self.buckets[addr] = Bucket(now)
        if not b.allow(len(data), now):
            self.counters["rate-limited"] += 1
            if now - b.last_log > 10:
                b.last_log = now
                log("UDP-DROP rate limit", addr, f"{b.dropped} dropped so far (> {UDP_RATE_PPS:.0f} pkt/s or {UDP_RATE_BPS / 1e6:.0f} MB/s)")
            return
        try:       # one bad packet / a failed sendto (Windows ICMP resets) must never end the relay thread
            ip = addr[0]
            peer_seen(ip, addr)
            op = data[0] if data else -1
            UDP_STATS[(addr, op)] += 1                 # (key = the address tuple: the "ip:port" string is built once per status tick, not per packet)
            if len(data) >= 5 and op == 0x03:     # keepalive: pong locally (no longer relayed, see below)
                self.send(b"\x04" + data[1:5], addr)   # pong: 04 + the client's seq (the game-server check needs the 0x04 opcode)
                PRES.ping(addr)
                k = (addr, 0x03)
                if self.seen.get(k, 0) < 2:
                    self.seen[k] = self.seen.get(k, 0) + 1
                    log("UDP", addr, f"03-keepalive {len(data)}B", data[:16].hex(" "))
                # Keepalives used to be copied to EVERY other keepalive socket on the server, whatever match they were in: n players made
                # n x (n - 1) packets, growing with the square of the player count. The game's keepalive record only accepts the 0x04 pong above
                # (validation: byte 0 == 4, matching seq); a copied 0x03 carries no sender and is dropped.
            elif len(data) >= 6 and op == 0x00:   # GAMEPLAY relay HELLO [00][u32 sid][u8 slot]
                sid = struct.unpack_from("<I", data, 1)[0]; slot = data[5]
                if not self._known_sid(sid, now):
                    self.counters["unknown-sid"] += 1
                    return
                first = RELAY.get((sid, slot)) != addr
                if first and (sid, slot) not in RELAY:
                    self._relay_room(now)
                RELAY[(sid, slot)] = addr
                RELAY_TS[(sid, slot)] = now
                self.send(b"\x01", addr)          # handshake ack -> client flag 0x180189961=1
                if first:
                    log("RELAY-HELLO", addr, f"sid={sid:08x} slot={slot}")
                    evt("relay-hello", f"{sid:08x}", "slot", slot, addr)
            elif len(data) >= 7 and op == 0x02:   # GAMEPLAY relay DATA [02][sid][src][dst][payload]
                sid = struct.unpack_from("<I", data, 1)[0]; src, dst = data[5], data[6]
                if not self._known_sid(sid, now):
                    self.counters["unknown-sid"] += 1
                    return
                cur = RELAY.get((sid, src))
                if cur is not None and cur != addr and now - RELAY_TS.get((sid, src), 0) < RELAY_REBIND_S and cur[0] != addr[0]:
                    # another host claims a live slot: not rebound by DATA (a restarted client sends HELLO first)
                    self.counters["hijack-refused"] += 1
                    if self.counters["hijack-refused"] in (1, 10, 100, 1000):
                        log("RELAY slot claim refused", addr, f"sid={sid:08x} slot={src} bound to {cur} (#{self.counters['hijack-refused']})")
                    return
                if (sid, src) not in RELAY:
                    self._relay_room(now)
                RELAY[(sid, src)] = addr
                RELAY_TS[(sid, src)] = now
                RELAY_STATS[(sid, src, dst)] += 1
                # T2RO msg type: [02][sid][src][dst] + "T2RO" + u16 + type
                if len(data) > 13 and data[7:11] == b"T2RO":
                    RELAY_TYPES[(src, dst, data[13], len(data))] += 1
                if dst == 0xFF:                   # broadcast (not seen yet; be permissive)
                    targets = [a for (sd, sl), a in list(RELAY.items()) if sd == sid and sl != src]
                else:
                    t = RELAY.get((sid, dst))
                    targets = [t] if t else []
                if not targets:
                    self.counters["no-target"] += 1
                for t in targets:
                    self._emit(data, t)
                k = ("relay", sid, src, dst)
                if self.seen.get(k, 0) < 3:
                    self.seen[k] = self.seen.get(k, 0) + 1
                    log("RELAY-DATA", addr, f"sid={sid:08x} {src}->{dst} {len(data)}B",
                        "no-target" if not targets else "", data[:24].hex(" "))
            else:                                 # unknown: relay to other GP sockets
                k = (addr, op)
                if self.seen.get(k, 0) < 5:
                    self.seen[k] = self.seen.get(k, 0) + 1
                    log("GAMEPLAY", addr, f"op=0x{(data[0] if data else 0):02x} {len(data)}B", data[:24].hex(" "))
                if data:
                    self._relay_table(self.GP, data, addr, now)
                else:
                    self.counters["malformed"] += 1
        except Exception as e:
            UDP_ERRS[0] += 1
            self.counters["malformed"] += 1
            if UDP_ERRS[0] <= 20: log("UDP-ERR", "handling", repr(e), addr)


UDP = None

def udp_server():
    global UDP
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    bind_exclusive(s, "UDP")
    log("UDP LISTEN", f"{HOST}:{PORT}")
    # Test only: simulate a bad connection on the gameplay relay (T2R_SIM_LAG ms one way,
    # T2R_SIM_JITTER ms random extra, T2R_SIM_LOSS percent dropped). Packets may reorder.
    SIM_LAG = int(os.environ.get("T2R_SIM_LAG", "0"))
    SIM_JITTER = int(os.environ.get("T2R_SIM_JITTER", "0"))
    SIM_LOSS = float(os.environ.get("T2R_SIM_LOSS", "0"))
    sim = None
    if SIM_LAG or SIM_JITTER or SIM_LOSS:
        log("SIM", f"lag {SIM_LAG} ms + jitter {SIM_JITTER} ms, loss {SIM_LOSS}%")
        import random
        # hot switch: tools/local/sim-off.txt present = no lag / jitter / loss, re-checked every second
        sim_off_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "local", "sim-off.txt")
        sim_off = [False, 0.0]
        def sim(data, t):
            now = time.time()
            if now - sim_off[1] > 1.0:
                was = sim_off[0]; sim_off[0] = os.path.exists(sim_off_path); sim_off[1] = now
                if was != sim_off[0]: log("SIM", "OFF (tools/local/sim-off.txt)" if sim_off[0] else f"ON again: lag {SIM_LAG} ms + jitter {SIM_JITTER} ms, loss {SIM_LOSS}%")
            if sim_off[0]:
                try: s.sendto(data, t)
                except OSError: pass
                return
            if random.random() * 100 < SIM_LOSS:
                return
            def go():
                try: s.sendto(data, t)
                except OSError: pass
            threading.Timer((SIM_LAG + random.random() * SIM_JITTER) / 1000.0, go).start()
    # bursts (the host's ~450 packets/s per peer, several peers at once) must queue in the kernel, not drop: the default
    # receive buffer is 64 KB (~100 packets), a GIL stall of a few tens of ms by another thread overflows it
    for opt, want in ((socket.SO_RCVBUF, 8 * 1024 * 1024), (socket.SO_SNDBUF, 4 * 1024 * 1024)):
        try: s.setsockopt(socket.SOL_SOCKET, opt, want)
        except OSError as e: log("UDP", "setsockopt failed", opt, e)
    rcv = s.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF); snd = s.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF)
    log("UDP", f"socket buffers: rcv {rcv} snd {snd} bytes")
    UDP = UdpRelay(lambda d, a: s.sendto(d, a), sim)
    UDP.sock_stats = f"sockbuf rcv {rcv // 1024} KB snd {snd // 1024} KB"
    while True:
        try:
            data, addr = s.recvfrom(65535)
        except ConnectionResetError:              # Windows: ICMP port-unreachable from a closed peer
            continue
        except OSError as e:                      # e.g. WinError 10040 (oversized datagram): never kill the UDP thread
            log("UDP-ERR", repr(e))
            continue
        UDP.handle(data, addr)



# ---------------------------------------------------------------- reaper (stuck states)
# Everything that can be left behind by a player or host that vanished without a clean leave (0x02) / match end (0x12):
#  - a match whose every player went offline (crash, power cut, router reset): the record blocked a re-queue
#    ("already in a match") and kept its vote / timing / host-migration state for ever
#  - a finished match whose 9 s free timer was lost
#  - a match whose countdown thread died: players saw MATCH FOUND for ever
#  - a player silent for PLAYER_DEPART seconds inside a running match: departed (free to queue again, the others
#    play on); the load screen no longer waits for them
#  - a launched match that never gets all players loaded: after LOAD_TIMEOUT the silent ones are departed and the
#    loaded players start (maybe_game_start)
OFFLINE_GRACE = float(os.environ.get("T2R_OFFLINE_GRACE", "60"))      # silent this long = offline (presence is touched every ~3 s)
PLAYER_DEPART = float(os.environ.get("T2R_PLAYER_DEPART", "120"))     # silent this long inside a launched match = gone
LOAD_SESSION_GRACE_S = float(os.environ.get("T2R_LOAD_SESSION_GRACE", "15"))   # a not-yet-loaded player without a session socket this long = gone (the others start without it)
LOAD_GRACE = float(os.environ.get("T2R_LOAD_GRACE", "45"))           # S1: a non-loader is dropped this long after the last other player loaded
PRELAUNCH_SILENT = float(os.environ.get("T2R_PRELAUNCH_SILENT", "25"))  # S3: a countdown player silent this long = crashed: the match is called off
MATCH_MAX_AGE = float(os.environ.get("T2R_MATCH_MAX_AGE", str(4 * 3600)))
ENDED_HOLD = float(os.environ.get("T2R_ENDED_HOLD", "30"))
LOAD_TIMEOUT = float(os.environ.get("T2R_LOAD_TIMEOUT", "90"))        # after the level start
NOLOAD_MARGIN = float(os.environ.get("T2R_NOLOAD_MARGIN", "30"))      # nobody loaded: called off this long after LOAD_TIMEOUT + LOAD_GRACE
LEAVE_KEEP_S = float(os.environ.get("T2R_LEAVE_KEEP", "15"))          # S2: a 0x02 this soon after the level start = dying conn, the re-queue re-sends the start
_SILENT = {}      # token -> time it was first seen silent inside a match (for the EVT log)

def silence(token, now):
    with PRES.lock:
        last = PRES.players.get(token, {}).get("last", 0)
    return now - last

def reap_matches(now=None):
    """One reaper pass; returns the list of (mid, action) taken (tests read it)."""
    now = time.time() if now is None else now
    done = []
    for mid, players, formed, launched, ended, ended_at, dep in MM.snapshot():
        age = now - formed
        active = [p for p in players if p not in dep]
        why = None
        if ended and ended_at and now - ended_at > ENDED_HOLD:
            why = "result in %.0f s ago, free timer lost" % (now - ended_at)
        elif age > MATCH_MAX_AGE:
            why = "older than %.0f h" % (MATCH_MAX_AGE / 3600)
        elif not active:
            why = "nobody left"
        elif (not launched and not ended and age > 10
              and any(silence(p, now) > PRELAUNCH_SILENT for p in active)):
            gone = [p for p in active if silence(p, now) > PRELAUNCH_SILENT]
            aborted = MM.abort(mid)
            if aborted is not None:
                for p in aborted:
                    LOADED.discard(p)
                    if p in gone:
                        MM.dequeue(p)                       # crashed: not a searcher any more
                        continue
                    c = SESSIONS.conn(p)
                    if c:
                        try: c.sendall(msg(0x11, srv_note("dropped", p)) + msg(0x03))
                        except OSError: pass
                forget_match_state(aborted)
                evt("reaped", mid, "countdown: %s silent > %.0f s, match called off, the others back in the queue" % ([tag(p) for p in gone], PRELAUNCH_SILENT), [tag(p) for p in aborted])
                done.append((mid, "abort-silent"))
            continue
        elif not launched and not ended and age > countdown_seconds() + 60:
            aborted = MM.abort(mid)
            if aborted is not None:
                for p in aborted:
                    LOADED.discard(p)
                    c = SESSIONS.conn(p)
                    if c:
                        try: c.sendall(msg(0x03))
                        except OSError: pass
                forget_match_state(aborted)
                evt("reaped", mid, "countdown stalled for %.0f s: players back in the queue" % age, [tag(p) for p in aborted])
                done.append((mid, "abort"))
            continue
        elif not ended and age > OFFLINE_GRACE and all(silence(p, now) > OFFLINE_GRACE for p in active):
            why = "every player offline (silent %s s)" % ", ".join("%s %.0f" % (tag(p), silence(p, now)) for p in active)
        if why:
            MM.drop(mid)
            HM.end(mid)
            loaded_clear_match(mid, players)
            for p in players:
                CD_GOT.discard(p); _SILENT.pop(p, None)
            forget_match_state(players)
            evt("reaped", mid, why, [tag(p) for p in players])
            done.append((mid, "drop"))
            continue
        if ended:
            continue
        if launched and CLAN_REPORTED_END.get(mid) and match_end_via_reports(mid, now):     # no host result, every player saw the end
            done.append((mid, "end-reports"))
            continue
        if launched and HM.enabled and active:
            _st = HM.pending_stale(mid, HOSTMIG_NOCAND_S)
            if _st:
                log("HOSTMIG", mid, "host lost %.0f s ago and nobody asked to take over - ghost match, ending it" % _st)
                abort_award(mid, list(active), None, "host lost, no successor for %.0f s" % _st)
                done.append((mid, "hostmig-ghost"))
                continue
        for p in active:                                # per-player silence inside the match
            lost = SESS_LOST.get(p)
            if lost and (lost[1] != mid or ended):
                SESS_LOST.pop(p, None)
            elif (lost and launched and SESSIONS.conn(p) is None
                  and now - lost[0] > (LOAD_SESSION_GRACE_S if (p not in LOADED and mid not in MM.started) else SESSION_GRACE_S)):
                # (PT92: a player whose session socket is gone while the level is still loading has crashed / quit: the others waited 82 s
                # for Gmontague835. Not loaded and not started yet = LOAD_SESSION_GRACE_S instead of SESSION_GRACE_S, then the others start.)
                # the session socket was lost in the running match and the player never re-attached: they left it (winmm's own reaction to a
                # dead socket), the others play on like after a timeout
                SESS_LOST.pop(p, None)
                evt("timeout", mid, tag(p), "session socket lost %.0f s ago and never came back: removed from the match, the others play on" % (now - lost[0]))
                LOADED.discard(p); _SILENT.pop(p, None)
                leave_running_match(p, "session lost")
                if mid not in MM.started:                    # it was the one the others waited for: start them now (maybe_game_start checks everybody left is loaded)
                    rest = [q for q in active if q != p and (q in LOADED or p == players[0])]     # (the host was lost: survivors back to the queue now)
                    if rest: maybe_game_start(rest[0])
                done.append((mid, "session-lost:" + p))
                continue
            sil = silence(p, now)
            if sil > OFFLINE_GRACE:
                if p not in _SILENT:
                    _SILENT[p] = now
                    evt("silent", mid, tag(p), "no presence for %.0f s (rtt/loss trouble or gone)" % sil)
                if launched and sil > PLAYER_DEPART:
                    evt("timeout", mid, tag(p), "silent %.0f s: removed from the match, the others play on" % sil)
                    LOADED.discard(p); _SILENT.pop(p, None)
                    leave_running_match(p, "timed out")
                    done.append((mid, "timeout:" + p))
            elif p in _SILENT:
                evt("back", mid, tag(p), "present again after %.0f s" % (now - _SILENT.pop(p)))
        if JIP:                                         # join in progress: a loaded joiner waits for the others' roster refresh (jip_acked), a stuck one is removed
            if jip_requeue_if_empty(mid):
                done.append((mid, "jip-empty"))
                continue
            for j in MM.jip_joiners(mid):
                if j in LOADED:
                    if not maybe_game_start(j) and MM.jip_age(j) > countdown_seconds() + LOAD_TIMEOUT + JIP_ACK_MAX + 30:
                        # loaded, but its GAME-START still did not go out long after the roster ack limit: never leave it on the loading
                        # screen - say why and send it back to the menu (its next search starts clean)
                        log("jip: joiner", tag(j), "loaded but never started after %.0f s - sent back to the menu" % MM.jip_age(j))
                        evt("jip-timeout", mid, tag(j), "loaded but not started after %.0f s" % MM.jip_age(j))
                        LOADED.discard(j)
                        c = SESSIONS.conn(j)
                        leave_running_match(j, "jip never started")
                        done.append((mid, "jip-timeout:" + j))
                        if c:
                            try: c.sendall(msg(0x0d, b""))
                            except OSError: pass
                elif MM.jip_age(j) > countdown_seconds() + LOAD_TIMEOUT:
                    evt("jip-timeout", mid, tag(j), "never loaded after %.0f s" % MM.jip_age(j))
                    LOADED.discard(j)
                    leave_running_match(j, "jip never loaded")
                    done.append((mid, "jip-timeout:" + j))
        mid2, still = MM.active_of(active[0]) if active else (None, [])
        late_start = launched and mid2 and mid not in MM.started
        missing = [p for p in still if p not in LOADED] if late_start else []
        loaded_now = [p for p in still if p in LOADED] if late_start else []
        # S1: the others have been loaded for LOAD_GRACE s and the missing ones are a minority / a single player: do not wait for the hard timeout
        # (never when it would leave one player: a slow loader in a 1 v 1 is often the host, it gets the full LOAD_TIMEOUT)
        soft = bool(late_start and missing and len(loaded_now) >= 2 and (len(missing) == 1 or len(loaded_now) > len(missing))
                    and now - max(LOADED_T.get(p, 0) for p in loaded_now) > LOAD_GRACE)
        if (late_start and still and not loaded_now and age > countdown_seconds() + LOAD_TIMEOUT + LOAD_GRACE + NOLOAD_MARGIN
                and not any(p in LOADED for p in players)):
            # nobody loaded at all (all stuck on the loading screen but still polling 0x05): the load-timeout path below needs one loaded player,
            # so the match used to live until MATCH_MAX_AGE. Call it off: everyone back to the menu with a reason, free to search again.
            MM.drop(mid)
            HM.end(mid)
            loaded_clear_match(mid, players)
            for p in players:
                CD_GOT.discard(p); _SILENT.pop(p, None)
                c = SESSIONS.conn(p)
                if c:
                    try: c.sendall(msg(0x11, srv_note("load_failed", p)) + msg(0x0d, b""))
                    except OSError: pass
            forget_match_state(players)
            log("MATCH-ABORT nobody loaded", mid, "age %.0f s" % age, [tag(p) for p in players])
            evt("reaped", mid, "MATCH-ABORT nobody loaded after %.0f s, everyone sent back to the menu" % age, [tag(p) for p in players])
            done.append((mid, "abort-noload"))
            continue
        if late_start and (soft or age > countdown_seconds() + LOAD_TIMEOUT):
            if missing and len(still) - len(missing) == 1 and mid not in MM.timed_out:
                #: the host never loaded, the one loaded player got a GAME-START alone, "Could not reach host", sat in an
                # empty level and left. One player is no match: call it off, everyone back to the menu with a reason, free to search again.
                MM.drop(mid)
                HM.end(mid)
                loaded_clear_match(mid, players)
                names = ", ".join(get_name(p) or "A player" for p in missing)
                for p in players:
                    CD_GOT.discard(p); _SILENT.pop(p, None)
                    c = SESSIONS.conn(p)
                    if c:
                        why = (srv_note("load_long_off", p) if p in missing else
                               srv_text("load_named", lang_of(p), n=names)[:120].encode("utf-8", "replace"))
                        try: c.sendall(msg(0x11, why) + msg(0x0d, b""))
                        except OSError: pass
                forget_match_state(players)
                log("MATCH-ABORT one player left after the load timeout", mid, "age %.0f s" % age, "missing", [tag(p) for p in missing],
                    "loaded", [tag(p) for p in loaded_now])
                evt("reaped", mid, "MATCH-ABORT %s never loaded, only one player left: called off, everyone back to the menu" % [tag(p) for p in missing],
                    [tag(p) for p in players])
                done.append((mid, "abort-solo"))
                continue
            if missing and len(still) - len(missing) >= 1:
                with MM.lock:
                    first = mid not in MM.timed_out
                    MM.timed_out.add(mid)
                if first:
                    evt("load-timeout", mid, "not loaded after %.0f s:" % age, [tag(p) for p in missing])
                    for p in missing:
                        c = SESSIONS.conn(p)
                        leave_running_match(p, "never loaded")
                        if c:                               # back to the menu with a short reason; it can rejoin through JIP
                            try: c.sendall(msg(0x11, srv_note("load_long", p)) + msg(0x0d, b""))
                            except OSError: pass
                    loaded = [p for p in still if p in LOADED]
                    if loaded:
                        maybe_game_start(loaded[0])
                    done.append((mid, "load-timeout"))
            elif mid2 and not missing:
                maybe_game_start(still[0])
    # a host-migration record whose match is gone (a leave raced the GAME-START bookkeeping, fuzz seed 86): nothing else would ever end it - it
    # polled "nobody left to host" every tick for ever
    if HM.enabled:
        with MM.lock:
            live_m = set(MM.matches)
        for hmid in [m for m in HM.mids() if m not in live_m]:
            HM.end(hmid)
            evt("reaped", hmid, "host-migration record without a match")
    online_now = set(PRES_TOKENS())
    for p in [p for p in list(_SILENT) if p not in online_now and not MM.in_match(p)]:
        _SILENT.pop(p, None)
    return done

# ---------------------------------------------------------------- live matches survive a daemon restart (premortem-matchmaking H3)
# Matches are peer-hosted: the game keeps running between the players when the daemon restarts (deploy, crash). Without a record the
# host's result (0x12) found no players (no win / clan day booked), host migration had no match, the match could not be joined, and its
# players who queued again formed a NEW one. The minimal state of every STARTED, not yet ended match is written next to the database
# (live-matches.json, atomic replace, every status-loop pass = 10 s + at game start + at shutdown). At start records younger than
# LIVE_MAX_AGE are loaded into READOPT; the first sign of life of a player of such a match (host-migration poll 0xE6, clock report
# 0xE3, result 0x12, clan report 0xE2, notices 0xBA, gun limit 0xDE, login 0x0f) puts the match back into MM / HM / GL / INT. A record
# nobody claims within LIVE_CLAIM_S is dropped. No protocol change: the clients already send their token in all of these.
LIVE_ON = os.environ.get("T2R_LIVE_STATE", "1") != "0"
LIVE_MAX_AGE = float(os.environ.get("T2R_LIVE_MAX_AGE", "1200"))     # a saved record older than this is never re-adopted
LIVE_CLAIM_S = float(os.environ.get("T2R_LIVE_CLAIM_S", "90"))       # an unclaimed record is dropped this long after the start
LIVE_VERSION = 1
READOPT = {}                  # mid -> {"rec": record, "until": time} (loaded from the file, waiting for a client of the match)
_readopt_lock = threading.Lock()

def live_path():
    return os.path.join(os.path.dirname(os.path.abspath(DB_PATH)), "live-matches.json")

def live_export(now=None):
    """The JSON-able live-state dict: every started, not ended match + the clan bookkeeping that belongs to it."""
    now = time.time() if now is None else now
    recs = []
    with MM.lock:
        for mid, pl in MM.matches.items():
            if mid in MM.ended or mid not in MM.started:
                continue
            # (551de4ee, host113): joiners still in MM.joining used to be written as DEPARTED - a JIP player who joined < 30 s before a restart
            # was dropped from the re-adopted match ("jip: roster slot 4 'a player' not in the server's roster - removed"). They are saved as
            # joiners (seconds since the join + whether they had loaded); _readopt_apply puts them back into MM.joining and finishes the join.
            dep = set(MM.departed.get(mid, ()))
            jn = {t: max(0.0, now - t0) for t, (m, t0) in MM.joining.items() if m == mid and t not in dep}
            ck = MM.clock.get(mid); lv = MM.lives.get(mid)
            recs.append({"mid": mid, "players": list(pl), "formed": MM.formed.get(mid, now), "started_t": MM.started_t.get(mid, now),
                         "launched": mid in MM.launched, "departed": sorted(dep),
                         "joining": jn, "joining_loaded": sorted(t for t in jn if t in LOADED),
                         "dep_vol": [t for t in pl if t in MM.departed_vol], "dep_t": {t: MM.departed_t[t] for t in pl if t in MM.departed_t},
                         "clock": list(ck) if ck else None, "lives": list(lv) if lv else None,
                         "late_bonus": {t: list(MM.late_bonus[t]) for t in pl if t in MM.late_bonus and MM.late_bonus[t][0] == mid},
                         "late_mult": {t: list(MM.late_mult[t]) for t in pl if t in MM.late_mult and MM.late_mult[t][0] == mid},
                         # the in-match team swaps (auto balance / admin). Without them a restart put every swapped player
                         # back on its slot-parity team in every roster reply, while the mover's own ext kept its new team: one-sided
                         # friendly fire, wrong nametags.
                         "team_swap": dict(TEAM_SWAP.get(mid) or {}),
                         "saved": now})
    for r in recs:
        key = frozenset(r["players"])
        r["map"] = MATCH_MAP.get(key); r["p2p"] = MATCH_P2P.get(key); r["sid"] = SID_KEEP.get(key)
        try: r["hm"] = HM.export_match(r["mid"]) if HM.enabled else None
        except Exception as e: r["hm"] = None; log("LIVE hm export failed", repr(e))
        r["gl"] = GL.export_held(r["mid"])
        with INT.lock:
            ir = INT.matches.get(r["mid"])
            r["int"] = json.loads(json.dumps(ir, default=str)) if ir else None
        r["clan_match"] = {t: r["mid"] for t in r["players"] if CLAN_MATCH.get(t) == r["mid"]}
    with _readopt_lock:
        for mid, e in READOPT.items():                    # not claimed yet: keep it (a second restart inside the window does not lose it)
            recs.append(e["rec"])
    mids = {r["mid"] for r in recs}
    with _clanlock:
        pend = {t: [p[0], p[1], p[2], {str(k): v for k, v in (p[3] or {}).items()}] for t, p in CLAN_PENDING.items()}
        ended = {m: t for m, t in CLAN_ENDED.items() if now - t < 3600}
    return {"v": LIVE_VERSION, "saved_at": now, "matches": recs, "clan_ended": ended, "clan_pending": pend,
            "clan_quitters": [[t, m] for t, m in list(CLAN_QUITTERS) if m in mids],
            "clan_credited": [[t, m] for t, m in list(CLAN_CREDITED) if m in mids],
            "clan_excused": [[t, m] for t, m in list(CLAN_EXCUSED) if m in mids],
            "parties": PARTIES.export()}             # a restart kept no party (social.Parties.restore)

_live_err = [False]
_live_save_lock = threading.Lock()
def live_save(now=None):
    """Atomic write of live_export(); never raises. A file with nothing in it is only written when an old file exists."""
    if not LIVE_ON:
        return False
    try:
        with _live_save_lock:        # (fuzz seed 225: two threads - status loop + a GAME-START - shared one .tmp file: os.replace raised PermissionError on Windows)
            data = live_export(now)
            path = live_path()
            if not data["matches"] and not data["clan_pending"] and not data["parties"]["of"] and not os.path.exists(path):
                return False
            tmp = path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(data, f, default=str)
            os.replace(tmp, path)
        return True
    except Exception as e:
        if not _live_err[0]:
            _live_err[0] = True; log("LIVE-STATE not saved:", repr(e))
        return False

def live_load(now=None):
    """At start: records younger than LIVE_MAX_AGE wait in READOPT for a client of their match (LIVE_CLAIM_S). The clan bookkeeping is
    restored at once (a day report that arrives before the match is claimed still finds its match id and end). Returns the mids loaded."""
    if not LIVE_ON:
        return []
    now = time.time() if now is None else now
    try:
        with open(live_path(), "r", encoding="utf-8") as f:
            data = json.load(f)
        if data.get("v") != LIVE_VERSION or now - float(data.get("saved_at", 0)) > LIVE_MAX_AGE:
            return []
    except (OSError, ValueError):
        return []
    mids = []
    for r in data.get("matches", []):
        try:
            if now - float(r.get("saved", 0)) > LIVE_MAX_AGE or not r.get("players") or not r.get("mid"):
                continue
            with _readopt_lock:
                READOPT[r["mid"]] = {"rec": r, "until": now + LIVE_CLAIM_S}
            mids.append(r["mid"])
            for t, m in (r.get("clan_match") or {}).items():
                CLAN_MATCH[t] = m
        except Exception as e:
            log("LIVE-STATE record skipped:", repr(e))
    for m, t in (data.get("clan_ended") or {}).items():
        CLAN_ENDED.setdefault(m, float(t))
    for t, p in (data.get("clan_pending") or {}).items():
        try:
            with _clanlock:
                CLAN_PENDING.setdefault(t, (p[0], int(p[1]), int(p[2]), {(int(k) if str(k).lstrip("-").isdigit() else k): v for k, v in (p[3] or {}).items()}))
            CLAN_DUE.setdefault(t, now + CLAN_GRACE)
        except Exception as e:
            log("LIVE-STATE clan report skipped:", repr(e))
    for t, m in data.get("clan_quitters") or []:
        CLAN_QUITTERS.add((t, m))
    for t, m in data.get("clan_credited") or []:
        CLAN_CREDITED.add((t, m))
    for t, m in data.get("clan_excused") or []:
        CLAN_EXCUSED.add((t, m))
    try:
        n = PARTIES.restore(data.get("parties"), now)
        if n:
            log("LIVE-STATE: parties restored -", n, "members in", len({PARTIES.pid(t) for t in PARTIES.grace}), "party(ies), kept %.0f s while they reconnect" % PARTIES.RESTORE_GRACE)
    except Exception as e:                                           # noqa: BLE001
        log("LIVE-STATE parties skipped:", repr(e))
    if mids:
        evt("readopt-load", "matches", mids, "waiting %.0f s for a player of each to report" % LIVE_CLAIM_S)
        log("LIVE-STATE: %d running match(es) from before the restart wait to be re-adopted:" % len(mids), mids)
    return mids

def readopt_claim(token, now=None):
    """A player's sign of life: if `token` belongs to a saved match waiting in READOPT, bring the match back. Returns the mid or None.
    Cheap when nothing waits (one dict truth test)."""
    if not READOPT or not token:
        return None
    with _readopt_lock:
        mid = next((m for m, e in READOPT.items()
                    if token in e["rec"]["players"] and token not in (e["rec"].get("departed") or ())), None)
        if mid is None:
            return None
        e = READOPT.pop(mid)
    try:
        _readopt_apply(mid, e["rec"], token, time.time() if now is None else now)
    except Exception as ex:
        log("LIVE-STATE re-adopt of", mid, "failed:", repr(ex))
        return None
    return mid

def _readopt_apply(mid, r, claimant, now):
    pl = list(r["players"])
    with MM.lock:
        if mid in MM.matches:
            return
        busy = {p for p in pl if p in MM.queue or any(p in o and p not in MM.departed.get(m, ()) and m not in MM.ended
                                                      for m, o in MM.matches.items())}
    dep = set(r.get("departed") or ()) | busy      # already queued / playing elsewhere again: it left this match
    joining = {t: float(a) for t, a in (r.get("joining") or {}).items() if t in pl and t not in dep}
    joining_loaded = set(r.get("joining_loaded") or ()) & set(joining)
    with MM.lock:
        MM.matches[mid] = pl
        MM.qtime[mid] = {}
        MM.formed[mid] = float(r.get("formed", now))
        if r.get("launched", True): MM.launched.add(mid)
        MM.started.add(mid)
        MM.started_t[mid] = float(r.get("started_t", now))
        if dep: MM.departed[mid] = set(dep)
        for t in r.get("dep_vol") or ():
            if t in dep: MM.departed_vol.add(t)
        for t, v in (r.get("dep_t") or {}).items():
            MM.departed_t[t] = float(v)
        if r.get("clock"): MM.clock[mid] = tuple(r["clock"])
        if r.get("lives"): MM.lives[mid] = tuple(r["lives"])
        for t, a in joining.items(): MM.joining[t] = (mid, now - a)      # a join in progress that the restart interrupted: finished below / by the reaper
        for t, v in (r.get("late_bonus") or {}).items(): MM.late_bonus[t] = (v[0], v[1])
        for t, v in (r.get("late_mult") or {}).items(): MM.late_mult[t] = (v[0], v[1])
        sw = {t: int(v) for t, v in (r.get("team_swap") or {}).items() if t in pl and int(v) in (0, 1)}
        if sw:
            TEAM_SWAP[mid] = sw                       # the in-match team swaps survive the restart (roster replies + the 0xA7 block)
    key = frozenset(pl)
    if r.get("map") is not None: MATCH_MAP[key] = r["map"]
    if r.get("p2p") is not None: MATCH_P2P[key] = r["p2p"]
    if r.get("sid"): SID_KEEP[key] = r["sid"]
    if HM.enabled and r.get("hm"):
        HM.import_match(mid, r["hm"])
        for p in busy:
            HM.depart(p, "left (back in the queue after the restart)")
    if r.get("gl"):
        GL.import_held(mid, r["gl"])
    if r.get("int"):
        with INT.lock:
            INT.matches[mid] = r["int"]
            for t in r["int"].get("players", []):
                INT.by_token[t] = mid
    for t in pl:
        if t not in dep:
            PRES.touch(t, get_name(t) or "PLAYER")      # every player gets the reaper's silence grace from now: the others' first polls follow
            if t not in joining or t in joining_loaded:   # (a joiner still on its loading screen reports its load itself)
                LOADED.add(t); LOADED_T[t] = now
            if SESSIONS.conn(t) is None:
                # (PT92 live, daemon restart at 21:37:45): the clients' session sockets were closed by the restart. Their ext re-attaches (login +
                # hello, session_reattach) only for a socket the daemon knows was lost: mark every restored player as one, so the re-attach is recognised
                # and not handled as a menu announce, and the reaper's session grace applies to a player that never returns.
                SESS_LOST[t] = (now, mid)
    RESTORED_T[mid] = now
    evt("readopt", mid, "claimed by", tag(claimant), "after the daemon restart | players", [tag(p) for p in pl if p not in dep],
        "| host slot", (r.get("hm") or {}).get("host", 0), "epoch", (r.get("hm") or {}).get("epoch", 0))
    log("LIVE-STATE: match", mid, "re-adopted (claimed by", tag(claimant) + ")", "players", [tag(p) for p in pl])
    if joining:
        log("LIVE-STATE: match", mid, "join in progress resumed for", [tag(t) for t in joining], "(loaded: %s)" % [tag(t) for t in joining_loaded])
        for t in joining_loaded:
            try: maybe_game_start(t)                  # GAME-START once the others' rosters list it (jip_acked) - else the reaper retries
            except Exception as ex: log("LIVE-STATE joiner start failed", tag(t), repr(ex))   # noqa: BLE001

RESTORED_T = {}       # mid -> time the match was brought back after a daemon restart (clan_finalize: no left-early penalty for what a restart caused)
RESTORE_EXCUSE_S = float(os.environ.get("T2R_RESTORE_EXCUSE_S", "600"))

def readopt_expire(now=None):
    """Drop the records nobody claimed in time. Returns the mids dropped."""
    if not READOPT:
        return []
    now = time.time() if now is None else now
    out = []
    with _readopt_lock:
        for mid in [m for m, e in READOPT.items() if now >= e["until"]]:
            READOPT.pop(mid, None); out.append(mid)
    for mid in out:
        evt("readopt-drop", mid, "no player of the match reported within %.0f s" % LIVE_CLAIM_S)
        log("LIVE-STATE: match", mid, "dropped - nobody came back")
    return out

def supervised(fn, name):
    """Run fn() for ever: an unexpected exception is logged and the loop restarts (a dead UDP / TCP / status thread
    used to stay dead silently while the process kept running)."""
    def run():
        n = 0
        while True:
            try:
                fn()
                return
            except Exception as e:
                n += 1
                log("THREAD-DIED", name, repr(e), "restart #%d" % n)
                time.sleep(min(5.0, 0.2 * n))
    return run

_last_net = [0.0]
def net_report(now=None, every=60.0):
    """Every `every` s per running match: one EVT line with the Steam ping estimates between its players and how long
    each has been silent (what a remote debugging session needs to see who had a bad link)."""
    now = time.time() if now is None else now
    if now - _last_net[0] < every:
        return
    _last_net[0] = now
    for mid, players, formed, launched, ended, _, dep in MM.snapshot():
        if not launched or ended:
            continue
        act = [p for p in players if p not in dep]
        pings = {"%s<>%s" % (tag(a), tag(b)): pair_ping(a, b) for i, a in enumerate(act) for b in act[i + 1:]
                 if pair_ping(a, b) is not None}
        evt("net", mid, "age %.0f s" % (now - formed), "players", [tag(p) for p in act], "ping-ms", pings or "unknown",
            "silent-s", {tag(p): int(silence(p, now)) for p in act if silence(p, now) > 10} or "none")

# ---------------------------------------------------------------- status
# Read-only live snapshot for tools/update_server and tools/admin_site (written every 10 s by status_loop, atomic replace). Default since: next to
# the database (t2r-status.json); T2R_STATUS_FILE=<path> moves it, T2R_STATUS_FILE=0 turns it off. No tokens in it (players are keyed by a hash).
_sf = os.environ.get("T2R_STATUS_FILE")
STATUS_FILE = "" if _sf in ("0", "off") else (_sf or os.path.join(os.path.dirname(os.path.abspath(DB_PATH)), "t2r-status.json"))
STARTED_AT = time.time()
_status_err = [False]

def _party_size(t):
    try: return len(PARTIES.members(t)) if PARTIES.pid(t) else 0
    except Exception: return 0

ROSTER_MAX = 200
def roster_public(snap):
    """Launcher 'who is online' panel (test feature): name + state only, no tokens / ids / addresses. state: menu | searching | match | party
    (party = in a party of 2+ and in the menus); `map` = map name of the match when known; `party` = in a party of 2+."""
    out = []
    try:
        maps = {m.get("id"): ((m.get("map") or {}).get("name")) for m in (snap or {}).get("matches", [])}
        for p in (snap or {}).get("players", []):
            party = (p.get("party") or 0) > 1
            st = p.get("state")
            state = "match" if st == "match" else "searching" if st == "queue" else "party" if party else "menu"
            out.append({"name": str(p.get("name") or "PLAYER")[:24], "state": state,
                        "map": (maps.get(p.get("match")) or None) if state == "match" else None, "party": party})
    except Exception as e:
        log("ROSTER-ERR", repr(e))
    return out[:ROSTER_MAX]

def _snap_match_mode(mid):
    """Mode key of a match for the snapshot (game_mode): the mode its lobby was formed in; the forced T2R_MODE; else the default mode (Supply Raid),
    the same fallback modeq_op uses for its per-mode counts. Read by the Discord bot's live player-count channels."""
    try:
        m = MODEQ.match_mode(mid)
    except Exception:                              # noqa: BLE001 - the snapshot never breaks on the mode lookup
        m = None
    return m or os.environ.get("T2R_MODE", "").strip().lower() or modeq.MODES[0]

def _snap_search_mode(tok):
    """Mode key a searcher waits in (search_mode), "any" for QUICK MATCH."""
    m = _search_mode(tok)
    return "any" if m == modeq.ANY else modeq.MODES[m] if 0 <= m < len(modeq.MODES) else modeq.MODES[0]

def live_snapshot():
    """Who is online / queued / in which match, for the admin site. Cheap (no db writes), never raises (returns {} on any problem)."""
    try:
        import hashlib
        now = time.time()
        def pid(t): return hashlib.sha256((t or "").encode()).hexdigest()[:12]
        with PRES.lock:
            pres = {t: dict(p) for t, p in PRES.players.items() if now - p.get("last", 0) < KEEPALIVE_TTL}
        with MM.lock:
            queue = dict(MM.queue)
            matches = {mid: list(pl) for mid, pl in MM.matches.items()}
            formed = dict(MM.formed); launched = set(MM.launched); ended = set(MM.ended); started = set(MM.started)
            departed = {m: set(d) for m, d in MM.departed.items()}
            clock = dict(MM.clock); lives = dict(MM.lives)
        where = {}
        for mid, pl in matches.items():
            for t in pl:
                if t not in departed.get(mid, ()) and mid not in ended:
                    where[t] = mid
        def nm(t):
            try: return get_name(t) or token_name(t) or "PLAYER"
            except Exception: return token_name(t) or "PLAYER"
        out_m = []
        for mid, pl in matches.items():
            key = frozenset(pl)
            mi = MATCH_MAP.get(key)
            lvl = MAPS[mi] if mi is not None and 0 <= mi < len(MAPS) else None
            hostt = pl[0] if pl else None
            try:
                hi = HM.host_info(mid) if HM.enabled else None
                if hi and 0 <= hi["host"] < len(pl): hostt = pl[hi["host"]]
            except Exception: pass
            ck = clock.get(mid); lv = lives.get(mid)
            plist = []
            for i, t in enumerate(pl):
                try: ping = pair_ping(hostt, t) if t != hostt else None
                except Exception: ping = None
                plist.append({"name": nm(t), "pid": pid(t), "team": team_in(mid, i, t), "left": t in departed.get(mid, ()), "host": t == hostt,
                              "ping_ms": ping})
            out_m.append({"id": mid, "state": "ended" if mid in ended else "running" if mid in started else "loading",
                          "age_s": round(now - formed.get(mid, now)), "map": {"level": lvl[2], "name": lvl[0]} if lvl else None,
                          "mode": os.environ.get("T2R_MODE", ""), "game_mode": _snap_match_mode(mid), "net": "steam-p2p" if MATCH_P2P.get(key) else "server-relay",
                          "players": plist, "clock": {"left": ck[0], "duration": ck[1], "ended": ck[2], "age_s": round(now - ck[3])} if ck else None,
                          "lives": {"team0": lv[0], "team1": lv[1], "age_s": round(now - lv[2])} if lv else None})
        out_p = []
        for t, p in pres.items():
            mid = where.get(t)
            out_p.append({"name": nm(t), "pid": pid(t), "steamid": BRIDGES.steamid(t) or 0, "state": "match" if mid else "queue" if t in queue else "menu",
                          "match": mid, "idle_s": round(now - p.get("last", now), 1), "queued_s": round(now - queue[t]) if t in queue else None, "queued": t in queue,
                          "version": CLIENT_VERSIONS.get(t, ""), "platform": "linux" if t in LINUX_CLIENTS else "windows",
                          "steam_helper": BRIDGES.conn(t) is not None, "party": _party_size(t),
                          "search_mode": _snap_search_mode(t) if t in queue else None})
        return {"started": STARTED_AT, "mode": os.environ.get("T2R_MODE", ""), "match_size": MATCH_SIZE, "min_players": MIN_PLAYERS,
                "players": out_p, "queue": [{"name": nm(t), "waited_s": round(now - q)} for t, q in queue.items()], "matches": out_m}
    except Exception as e:
        log("LIVE-SNAPSHOT-ERR", repr(e))
        return {}

STATE_TTL_S = float(os.environ.get("T2R_STATE_TTL_S", "3600"))   # a token not heard from for this long (and in no queue / match) loses its per-token records
STATE_GC_S = float(os.environ.get("T2R_STATE_GC_S", "300"))       # gc_state runs this often (status loop)
_gc_last = [0.0]
def _prune_keys(d, dead):
    """Delete from dict/set d every key for which dead(key) is true; returns the number deleted (a snapshot is walked: other threads keep writing)."""
    keys = [k for k in list(d) if dead(k)]
    if isinstance(d, (set, frozenset)):
        for k in keys: d.discard(k)
    else:
        for k in keys: d.pop(k, None)
    return len(keys)

def gc_state(now=None, ttl=None):
    """Bound every registry keyed by player / address / match that nothing else removes (soak audit): presence, per-token ping / region / feed /
    booster / clan-day / JIP records, finished-match flags, invitations, rate-limiter addresses. A token counts as ACTIVE (keeps everything) while it
    is queued, in a match, a registered helper / session, in a party, or was heard from within `ttl` seconds. Returns {name: entries dropped}."""
    now = time.time() if now is None else now
    ttl = STATE_TTL_S if ttl is None else ttl
    out = {}
    with MM.lock:
        live_mids = set(MM.matches)
        busy = set(MM.queue)
        for pl in MM.matches.values():
            busy.update(pl)
    busy |= set(BRIDGES.by_token) | set(SESSIONS.by_token)
    out["PRES"] = PRES.prune(ttl, keep=busy)
    with PRES.lock:
        keep = busy | {t for t, p in PRES.players.items() if now - p.get("last", 0) <= ttl}
    dead_tok = lambda t: t not in keep
    for nm in ("FEED_SEEN", "BOOST_PICK", "BOOST_ACTIVE", "CLAN_MATCH", "PING_LOC", "LOC_FWD", "LOC_HIST", "LOC_SPIKES", "REGION", "REGION_CAND", "JIP_LATE", "MESH_ON",
               "JIP_STARTED", "JIP_REPUSH", "_JIP_WAIT_LOG", "LOADED_T", "_SILENT", "LOADED", "CD_GOT"):
        n = _prune_keys(globals()[nm], dead_tok)
        if n: out[nm] = n
    n = _prune_keys(NAMELESS_BY_IP, lambda ip: NAMELESS_BY_IP.get(ip) not in keep)
    if n: out["NAMELESS_BY_IP"] = n
    n = _prune_keys(PING, lambda k: k[0] not in keep or k[1] not in keep)
    if n: out["PING"] = n
    _prune_keys(PING_AT, lambda k: k not in PING)
    n = _prune_keys(PING_LAST, lambda k: now - PING_LAST.get(k, (0, now))[1] > PING_GATE_AGE_S)
    if n: out["PING_LAST"] = n
    n = 0
    for d in (VOTES, MATCH_MAP, TIMING, SID_KEEP, MATCH_P2P):          # keyed by frozenset(match tokens): gone when none of its players is active any more
        n += _prune_keys(d, lambda k: not (k & busy))
    if n: out["match-sets"] = n
    # finished-match records: (token, mid) / mid keys of matches that are neither live nor waiting for their clan day any more
    gone_mid = lambda m: m not in live_mids and m not in CLAN_ENDED
    n = _prune_keys(CLAN_ENDED, lambda m: now - CLAN_ENDED.get(m, now) > max(ttl, 120))
    n += _prune_keys(CLAN_REPORTED_END, gone_mid) + _prune_keys(END_BY_REPORTS, gone_mid) + _prune_keys(RESULT_BOOKED, gone_mid)
    n += _prune_keys(CLAN_QUITTERS, lambda k: gone_mid(k[1]) and k[0] not in CLAN_PENDING)
    n += _prune_keys(CLAN_CREDITED, lambda k: gone_mid(k[1]) and k[0] not in CLAN_PENDING)
    n += _prune_keys(CLAN_EXCUSED, lambda k: gone_mid(k[1]) and k[0] not in CLAN_PENDING)
    n += _prune_keys(CLAN_EXCUSE_WHY, lambda k: gone_mid(k[1]) and k[0] not in CLAN_PENDING)
    n += _prune_keys(CLAN_LEFT_AT, lambda k: gone_mid(k[1]) and k[0] not in CLAN_PENDING)
    n += _prune_keys(CLAN_REJOINED, lambda k: gone_mid(k[1]) and k[0] not in CLAN_PENDING)
    n += _prune_keys(CLAN_LAST_DEPART, lambda m: gone_mid(m) and now - CLAN_LAST_DEPART[m][0] > max(ttl, 120))
    n += _prune_keys(ABORT_AWARDED, gone_mid)
    n += _prune_keys(CLAN_DONE, lambda t: t not in keep and t not in CLAN_PENDING)   # re-send dedupe only matters while the reporter is around
    n += _prune_keys(_JIP_LAST_WHY, lambda k: k not in keep and k not in live_mids)
    if n: out["clan-match"] = n
    if len(_POOL_LOGGED) > 500:
        _POOL_LOGGED.clear()
    for nm, fn in (("MM", lambda: MM.gc(keep, now)), ("PARTIES", lambda: PARTIES.gc(keep | SOCIAL.launcher_tokens(), now)), ("PERF", PERF.prune), ("NEW_TOKENS", lambda: NEW_TOKENS.prune(now))):
        n = fn()
        if n: out[nm] = n
    mono = time.monotonic()
    n = 0
    for cache in (_name_cache, _steam_cache):
        n += _prune_keys(cache, lambda t, cache=cache: cache[t][0] < mono if t in cache else True)
    if n: out["name-cache"] = n
    return {k: v for k, v in out.items() if v}

def gc_tick(now=None):
    """status_loop hook: gc_state at most every STATE_GC_S seconds; logs one line when something was dropped."""
    now = time.time() if now is None else now
    if now - _gc_last[0] < STATE_GC_S:
        return None
    _gc_last[0] = now
    try:
        r = gc_state(now)
    except Exception as e:                 # (an odd key in one registry must not abort the status tick: host migration tick + status file follow it)
        log("GC-ERR", repr(e))
        return None
    if r:
        log("GC", "dropped", " ".join("%s=%d" % kv for kv in sorted(r.items())))
    return r

SIZE_CAPS = {"Presence.udp_seen": Presence.UDP_SEEN_MAX, "PEERS": PEERS_MAX, "UdpRelay.KA": UdpRelay.TABLE_MAX, "UdpRelay.GP": UdpRelay.TABLE_MAX,
             "UdpRelay.buckets": 4096, "UdpRelay.seen": 2000, "RELAY": UdpRelay.RELAY_MAX, "RELAY_TS": UdpRelay.RELAY_MAX, "AddrLimiter.hits": 4096,
             "_POOL_LOGGED": 500, "log_queue": 50000, "PING": PING_MAX}      # tables with a hard cap (soak: growth up to the cap is not a leak)

def state_sizes():
    """Entry counts of every long-lived registry + thread / connection counts (admin GET /sizes; tools/tests/soak_daemon.py watches them for growth)."""
    out = {}
    def put(name, obj):
        try: out[name] = len(obj)
        except Exception: pass
    for nm in ("CLAN_MATCH", "CLAN_ENDED", "CLAN_PENDING", "CLAN_QUITTERS", "CLAN_CREDITED", "CLAN_LAST_DEPART", "CLAN_EXCUSED", "CLAN_EXCUSE_WHY", "CLAN_REJOINED", "CLAN_REBOOK", "CLAN_LEFT_AT", "CLAN_DUE", "BOOST_PICK", "BOOST_ACTIVE",
               "FEED_SEEN", "PEERS", "LOADED", "LOADED_T", "RELAY", "RELAY_TS", "NAMELESS_BY_IP", "CLIENT_VERSIONS", "LINUX_CLIENTS", "BROKEN_MAPS",
               "MATCH_MAP", "VOTES", "_POOL_LOGGED", "SID_KEEP", "TIMING", "PING_LOC", "PING", "LOC_HIST", "LOC_SPIKES", "REGION", "REGION_CAND",
               "JIP_LATE", "JIP_STARTED", "JIP_REPUSH", "_JIP_LAST_WHY", "_JIP_WAIT_LOG", "ABORT_AWARDED", "CD_GOT", "_SILENT", "MATCH_P2P", "READOPT",
               "UDP_STATS", "RELAY_STATS", "RELAY_TYPES", "_tcp_per_ip", "_JIP_LAST_WHY"):
        if nm in globals(): put(nm, globals()[nm])
    for owner, attrs in ((PRES, ("players", "udp_seen")), (BRIDGES, ("by_token",)), (SESSIONS, ("by_token",)), (PARTIES, ("of", "inv")),
                         (MM, ("queue", "seen", "matches", "formed", "left", "settle", "qtime", "launched", "departed", "ended", "ended_at", "timed_out",
                               "started", "started_t", "departed_vol", "departed_t", "clock", "polled", "joining", "lives", "late_bonus", "late_mult")),
                         (HM, ("matches", "by_token")), (PERF, ("by_token", "by_steam")), (REPICK, ("recs", "last_text", "base")),
                         (INT, ("matches", "by_token")), (GL, ("held", "want")), (NOTICES, ("texts",)), (NEW_TOKENS, ("hits",))):
        for a in attrs:
            put("%s.%s" % (type(owner).__name__, a), getattr(owner, a, ()))
    if UDP is not None:
        for a in ("seen", "KA", "GP", "buckets", "sizes", "pk_in", "pk_out"):
            put("UdpRelay." + a, getattr(UDP, a, ()))
    out["threads"] = threading.active_count()
    out["tcp_live"] = _tcp_live[0]
    out["_caps"] = dict(SIZE_CAPS)
    out["log_queue"] = len(_q)
    return out

STATS_LINES = 24
def _take(counter):
    """Snapshot (sorted, biggest count first) and clear a counter that another thread keeps adding to."""
    items = list(counter.items())
    counter.clear()
    items.sort(key=lambda kv: -kv[1])
    return items

def status_loop():
    last_bak = time.time()       # db_guard checked the DB at start; the first backup waits DB_BACKUP_S (it stalled ~80 s right after a restart)
    while True:
        time.sleep(10)
        if time.time() - last_bak > DB_BACKUP_S:         # rolling db backup (every 30 min)
            last_bak = time.time()
            if os.path.exists(DB_PATH) and db_backup():
                log("DB backup ok")
        try:    # an exception here used to end the thread for good (and with it host migration's tick and the db backup)
            log("STATUS", f"online={PRES.count()} queue={len(MM.queue)} matches={len(MM.matches)} polls={NOISY_COUNT[0]} tcp={_tcp_live[0]} roreads={RO_READS[0]}/{RO_READS[1]} abandoned={ABANDONED[0]}")
            NOISY_COUNT[0] = 0
            reap_matches()
            ac_tick()
            gc_tick()
            _save_tables_if_due()
            readopt_expire()
            live_save()
            net_report()
            try: ping_check()                                    # ping limit (never breaks the status tick)
            except Exception as e: log("PING-CHECK ERR", repr(e))
            try: loss_check()                                    # packet loss limit (never breaks the status tick)
            except Exception as e: log("LOSS-CHECK ERR", repr(e))
            if UDP is not None and UDP.pk_in:
                log("UDP-HEALTH", UDP.health_line())
            if UDP is not None and UDP.counters:
                log("UDP-DROPS", dict(UDP.counters)); UDP.counters.clear()
            db_maintenance_tick()
            if HM.enabled:
                HM.tick()
                repick_tick()
            # (the UDP thread adds keys while this runs: iterate a snapshot - "dictionary changed size during iteration" ended the whole tick here -
            # and print at most STATS_LINES entries: a source-port spray made this one line megabytes long)
            if UDP_STATS:
                snap = _take(UDP_STATS)
                log("UDP-STATS", " | ".join(f"{ad[0]}:{ad[1]} op0x{op:02x}:{c}" for (ad, op), c in snap[:STATS_LINES]) + (" | ... +%d more" % (len(snap) - STATS_LINES) if len(snap) > STATS_LINES else ""))
            if RELAY_STATS:
                snap = _take(RELAY_STATS)
                log("RELAY-STATS", " | ".join(f"{sid:08x} {a}->{b}:{c}" for (sid, a, b), c in snap[:STATS_LINES]) + (" | ... +%d more" % (len(snap) - STATS_LINES) if len(snap) > STATS_LINES else ""))
            if RELAY_TYPES:
                snap = _take(RELAY_TYPES)
                log("RELAY-TYPES", " | ".join(f"{a}->{b} t{t:02x} {n}B:{c}" for (a, b, t, n), c in snap[:STATS_LINES]) + (" | ... +%d more" % (len(snap) - STATS_LINES) if len(snap) > STATS_LINES else ""))
            if STATUS_FILE:     # read-only snapshot for tools/update_server + tools/admin_site (T2R_STATUS_FILE); its own try: written last: a slow db read here never delays the reaper
                try:
                    _tmp = STATUS_FILE + ".tmp"
                    with open(_tmp, "w", encoding="utf-8") as _f:
                        _live = live_snapshot()
                        json.dump({"ts": wall_time(), "server_version": SERVER_VERSION, "players_online": PRES.count(),
                                   "queue": len(MM.queue), "matches": len(MM.matches), "live": _live, "roster": roster_public(_live),
                                   "notices": notices_public(), "target_version": update_target()}, _f, default=str)
                    os.replace(_tmp, STATUS_FILE)
                except Exception as _e:
                    if not _status_err[0]:
                        _status_err[0] = True; log("STATUS-FILE-ERR", repr(_e))
        except Exception as e:
            log("STATUS-ERR", repr(e))

# ---------------------------------------------------------------- admin control (tools/admin_site; loopback only)
# A tiny HTTP listener on 127.0.0.1:T2R_ADMIN_PORT (default 7011, 0 = off). It is NOT on the game port (the playit tunnel only forwards that one),
# and every request except /status needs the shared secret in header X-Admin-Secret (file admin-secret.txt next to the database, created on first
# start, never sent to a client). GET /status = counts only (online / queue / matches, same numbers as the menu's online count);
# GET /live = live_snapshot(); POST /kick {"pid": <12 hex from /live>} = admin_kick. Every handler is wrapped: it can never take the daemon down.
# default follows the game port (live 7001 -> 7011, the A/B test daemon on 7101 -> 7111) so a second daemon never takes the live one's listener
ADMIN_PORT = int(os.environ.get("T2R_ADMIN_PORT", str(7011 + PORT - 7001) if 7001 <= PORT <= 7900 else "7011"))
ADMIN_SECRET_PATH = os.path.join(os.path.dirname(os.path.abspath(DB_PATH)), "admin-secret.txt")

def admin_secret():
    try:
        with open(ADMIN_SECRET_PATH, encoding="utf-8") as f:
            v = f.read().strip()
        if len(v) >= 16:
            return v
    except OSError:
        pass
    v = secrets.token_hex(16)
    with open(ADMIN_SECRET_PATH, "w", encoding="utf-8") as f:
        f.write(v + "\n")
    return v

def admin_reset_session(pid):
    """Test aid (A/B recipe for the session-socket re-attach): RST-close a player's held session socket the way a NAT that drops the flow does
    (SO_LINGER 0 -> the client's recv gets WSAECONNRESET 10054). Nothing else changes: the match record stays, the client has SESSION_GRACE_S to re-attach."""
    import hashlib
    with SESSIONS.lock:
        cand = [t for t in SESSIONS.by_token if hashlib.sha256(t.encode()).hexdigest().startswith(str(pid).lower())] if isinstance(pid, str) and len(pid) >= 4 else []
    if len(cand) != 1:
        return {"ok": False, "error": "no such session" if not cand else "ambiguous pid"}
    c = SESSIONS.conn(cand[0])
    if c is None:
        return {"ok": False, "error": "no session"}
    try:
        raw = getattr(c, "_sock", c)
        raw.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        raw.close()
    except OSError as e:
        return {"ok": False, "error": repr(e)}
    evt("admin-reset-session", tag(cand[0]), "session socket reset (test aid)")
    return {"ok": True, "player": tag(cand[0])}

def kick_reason_clean(reason):
    """Admin-typed kick reason: str only, control chars dropped, whitespace collapsed, trimmed, max 120 chars."""
    if not isinstance(reason, str):
        return ""
    return re.sub(r"\s+", " ", "".join(ch for ch in reason if ch >= " " and ch != "\x7f")).strip()[:120].strip()

def kick_text(reason, lang="en"):
    reason = kick_reason_clean(reason).replace('"', "'")
    return srv_text("kicked_why", lang, r=reason) if reason else srv_text("kicked", lang)

def kick_token(tok, why, ktxt, kind="admin-kick", reason="", keep_out=False, no_penalty=False):
    """The kick itself (admin kick / team move / AFK removal): out of the queue and the requeue, out of its match (a not yet launched match is called off
    and the others go back to searching, a running one is left like a normal leave with `why` as the reason: the others play on), LOADED / countdown /
    silence marks, presence, the text `ktxt` + a 0x0d (back to the menu) on its held matchmaking socket. keep_out (AFK): join in progress never puts it
    back into the match it was removed from (an admin kick clears that mark: the player may come back). Returns the list of what was done."""
    did = []
    if no_penalty:                                                # its clan day for this match never books a deficit (CLAN_KICKED)
        _km = MM.mid_of(tok)
        if _km is not None:
            CLAN_KICKED[(tok, _km)] = why
            if len(CLAN_KICKED) > 2000:
                for _k in list(CLAN_KICKED)[:500]:
                    CLAN_KICKED.pop(_k, None)
    if MM.dequeue(tok):
        did.append("queue")
    if MM.rq.drop(tok):
        did.append("requeue ticket")
    for _ in range(4):                                            # a token can sit in more than one record
        mid = MM.mid_of(tok)
        if mid is None:
            break
        others = MM.cancel(tok)                                   # before the level start: called off, the others search again
        if others is not None:
            for o in others:
                LOADED.discard(o)
                c = SESSIONS.conn(o)
                if c:
                    try: push(c, searching_frame(o), drop=True)
                    except OSError: pass
            did.append("match %s called off" % mid)
        elif mid in MM.ended:                                     # finished match still held: just step out (no award logic on a result already in)
            MM.depart(tok, voluntary=True)
            did.append("left finished match %s" % mid)
        else:
            leave_running_match(tok, why)
            did.append("left match %s" % mid)
    if MM.in_match(tok):                                          # still listed (ended-but-held records): drop its membership
        MM.leave(tok)
        did.append("membership dropped")
    with MM.lock:
        if not keep_out:
            MM.departed_vol.discard(tok)
        MM.joining.pop(tok, None); MM.late_bonus.pop(tok, None); MM.late_mult.pop(tok, None)
        MM.seen.pop(tok, None); MM.settle.pop(tok, None)
    LOADED.discard(tok); CD_GOT.discard(tok); _SILENT.pop(tok, None)
    try: SOCIAL.note(tok, "kicked", ktxt)                         # notice poll (op 0xBA, kind p): shown in a match too, for any client
    except Exception as e: log("KICK note failed", repr(e))
    c = SESSIONS.conn(tok)
    if c:
        try:
            c.sendall(msg(0x11, ktxt.encode("utf-8")[:0xf0]) + msg(0x0d, b"\x01")); did.append("sent back to the menu")
        except OSError:
            pass
        SESSIONS.unregister(tok, c)
    PRES.drop(tok)
    evt(kind, tag(tok), "-", ", ".join(did) or "nothing to clear", clean_str(reason, 120))
    return did

def afk_report(token, idle_s, flags=0):
    """Op 0xE3 trailer (ext afk.c): `idle_s` seconds without any input while alive in the running match (0xFFFF = unknown / the ext's AFK is off).
 At AFK_KICK_S the player is removed with the kick path: back to the menu with the AFK text, out of the match like a quit (no win; no clan
    deficit, CLAN_KICKED - kicks carry no penalty), no requeue ticket. Once per (token, match). Returns True when it removed the player."""
    if AFK_KICK_S <= 0 or idle_s == 0xFFFF or idle_s < AFK_KICK_S:
        return False
    with MM.lock:
        mid = MM._mid_locked(token)
        live = bool(mid and mid in MM.started and mid not in MM.ended and token not in MM.departed.get(mid, ()) and token not in MM.joining)
    if not live or (token, mid) in AFK_KICKED:
        return False
    if PRIV.rule(mid, "afk") == 0:
        return False                                     # private match with AFK REMOVAL off
    AFK_KICKED.add((token, mid))
    if len(AFK_KICKED) > 2000:
        AFK_KICKED.clear()
    log("afk:", tag(token), "no input for %d s in match %s (ext flags %d) - removed from the match (counts as leaving early)" % (idle_s, mid, flags))
    did = kick_token(token, "left: afk (no input for %d s)" % idle_s, srv_text("afk_kicked", lang_of(token), s=int(idle_s)), "afk-kick",
                     "no input for %d s" % idle_s, keep_out=True, no_penalty=True)
    log("afk:", tag(token), "removal done:", ", ".join(did) or "nothing")
    return True

def _ping_fresh(a, b, now):
    """pair_ping(a, b) when one of the two directions was measured in the last PING_FRESH_S, else None (a stale median says nothing about now)."""
    at = max(PING_AT.get((a, b), 0.0), PING_AT.get((b, a), 0.0))
    return pair_ping(a, b, raw=True) if now - at <= PING_FRESH_S else None

#: the ping gates (match formation, join in progress) read the fresh ping only. Searchers are not connected to each other, so
# their pair pings go stale within 90 s and an unknown ping passes: a player at 287 ms to the host was matched (EVT ping-ms had the 287), and a
# joiner refused at 262 ms one second earlier joined when the same ping turned stale; both were removed 60 s in. The gates fall back to the
# last measured ping up to PING_GATE_AGE_S old (two homes' route changes slowly). ping_check (the in-match removal timer) stays on fresh pings.
PING_GATE_AGE_S = float(os.environ.get("T2R_PING_GATE_AGE_S", "3600"))

def ping_wait_judge(players, host, pair, now, units_fn=None, helper=None, wait_s=None, host_ok=None):
    """Formation with unknown pair pings (match f160b685: 'HOST-PICK ping unknown, keeping queue order', the gate passed, two players at 281 / 210 ms
    to the host were removed once the pings arrived). Only players with a live Steam helper can have a ping; a pair of them with no ping is unknown.
    Returns (hold, leave_out): hold = some player in an unknown pair has waited less than PING_WAIT_S (since its first pass with one, kept in
    PING_WAIT_SINCE) - the caller waits; after that leave_out = the fewest players (whole parties) whose ping to the host is still unknown: the
    host is whichever player (`host` = the gate's pick wins a tie, host_ok = may host) has the fewest unknown pings; everybody with an unknown pair
    when no player can host. PING_WAIT_S 0 = off."""
    wait_s = PING_WAIT_S if wait_s is None else wait_s
    helper = helper or has_helper
    players = list(players)
    inv = set()
    if wait_s > 0 and len(players) >= 2:
        hp = [t for t in players if helper(t)]
        for i, a in enumerate(hp):
            for b in hp[i + 1:]:
                if pair(a, b) is None:
                    inv.add(a); inv.add(b)
    for t in players:
        if t not in inv:
            PING_WAIT_SINCE.pop(t, None)
    if not inv:
        return False, set()
    for t in inv:
        PING_WAIT_SINCE.setdefault(t, now)
    if len(PING_WAIT_SINCE) > 500:
        for k in [k for k, v in PING_WAIT_SINCE.items() if now - v > 3600]:
            PING_WAIT_SINCE.pop(k, None)
    # (20-29 searchers once got one 2-player match in 8 min) holding while ANY unknown player was new starved the queue: with that many
    # searchers someone new arrives every few seconds. The hold now ends once the longest-waiting unknown player has waited wait_s; the newer ones
    # whose ping to the host is still unknown are left out of this lobby (the next pass / a join in progress takes them).
    if max(now - PING_WAIT_SINCE[t] for t in inv) < wait_s:
        return True, set()
    def left_out(h):
        o = {t for t in inv if t != h and (h is None or pair(h, t) is None)}
        for t in list(o):
            o.update(m for m in (units_fn(t) if units_fn else ()) if m in players and m != h)
        return o
    best = None
    for i, h in enumerate([host] + players if host is not None else players):    # the gate's own host first: it wins a tie
        if h is None or (host_ok is not None and not host_ok(h)):
            continue
        o = left_out(h)
        if best is None or len(o) < len(best):
            best = o
    return False, (best if best is not None else left_out(host))

PING_OUT = {}            # token -> [first, last] pass the ping gate left it out for being over the limit to the lobby's host

def ping_left_out_note(dropped, now, note=None):
    """A searcher the ping gate keeps leaving out (every pass for PING_NOTE_AFTER_S s) hears 'no players within N ms right now - still searching',
    at most once per PING_NOTE_EVERY_S s (SOCIAL note kind 'pingfar', the notice poll shows it). Returns the tokens noted."""
    note = note or (lambda t, n: SOCIAL.note(t, "pingfar", str(n)))
    cur = set(dropped)
    for t in [t for t in PING_OUT if t not in cur]:
        PING_OUT.pop(t, None)
    sent = []
    for t in cur:
        rec = PING_OUT.get(t)
        if rec is None or now - rec[1] > 30.0:
            rec = PING_OUT[t] = [now, now]
        rec[1] = now
        if now - rec[0] >= PING_NOTE_AFTER_S and now - PING_NOTE_AT.get(t, -1e9) >= PING_NOTE_EVERY_S:
            PING_NOTE_AT[t] = now
            try:
                note(t, PING_KICK_MS)
                sent.append(t)
                log("PING-GATE", tag(t), "left out for %d s: search note sent (no players within %d ms)" % (now - rec[0], PING_KICK_MS))
            except Exception as e:                       # noqa: BLE001 - a note never breaks the queue
                log("PING-GATE note ERR", repr(e))
    if len(PING_NOTE_AT) > 500:
        for k in [k for k, v in PING_NOTE_AT.items() if now - v > 3600]:
            PING_NOTE_AT.pop(k, None)
    return sent

def _ping_gate(a, b, now):
    """The ping the gates judge: the fresh pair ping, else the last measured one when it is at most PING_GATE_AGE_S old, else None."""
    ms = _ping_fresh(a, b, now)
    if ms is None:
        at = max(PING_AT.get((a, b), 0.0), PING_AT.get((b, a), 0.0))
        ms = pair_ping(a, b, raw=True) if at and now - at <= PING_GATE_AGE_S else None
    if ms is None:                                        # not in PING (evicted / never kept): the last reported pair ping
        pl = PING_LAST.get(frozenset((a, b)))
        if pl and pl[0] is not None and now - pl[1] <= PING_GATE_AGE_S:
            ms = pl[0]
    if not MEASURED_PAIR and not PING_CORR:
        return ms
    if ms is not None:                                    # the per-player correction on the estimate
        ms += max(_corr_of(a, now), _corr_of(b, now))
    if PING_MEMORY_S > 0:                                 # what the last removal of this pair measured
        m = _measured_of(a, b, now)
        if m is not None and (ms is None or m > ms):
            ms = m
    return ms

def jip_ping_block(token, mid, host, active, joining, now):
    """Join in progress: the joiner's fresh ping to the host when ping_check would remove it for that ping, else None (unknown ping, under the
    limit, limit off for the match, or most players high to the host - ping_check removes nobody then). Match 8bb74ec7: a match hosted in the
    Middle East took North American joiners at 233-299 ms; each was removed after 60 s, and a removed player came back once and stayed.
    Called with MM.lock held (reads only)."""
    if PING_KICK_MS <= 0 or PRIV.rule(mid, "ping") == 0 or token == host:
        return None
    ms = _ping_gate(token, host, now)
    if ms is None or ms <= (ping_match_ms() if MM_PING_GATE else PING_KICK_MS):
        return None
    if MM_PING_GATE:
        # match 72784a06: one player already in at 254 ms made 'most players high' true with the joiner's own ping counted, so three joiners
        # at 238-280 ms got in and were removed once she left. A joiner over the limit never joins (the exception below is for players already in).
        return ms
    known = [ms] + [v for v in (_ping_fresh(p, host, now) for p in active if p not in (host, token) and p not in joining) if v is not None]
    high = [v for v in known if v > PING_KICK_MS]
    if len(known) >= 2 and len(high) * 2 > len(known):
        return None
    return ms

def ping_check(now=None):
    """Every status tick (10 s): the ping limit (PING_KICK_MS / PING_KICK_S, see the table above). Returns the tokens removed."""
    now = time.time() if now is None else now
    out = []
    if PING_KICK_MS <= 0:
        return out
    live = set()
    for mid, players, formed, launched, ended, _, dep in MM.snapshot():
        if not launched or ended or now - formed < PING_GRACE_S:
            continue
        act = [p for p in players if p not in dep]
        if len(act) < 2 or PRIV.rule(mid, "ping") == 0:     # (a private match with PING LIMIT off)
            continue
        host = HM.host_token(act[0]) if (HM.enabled and HM.has(act[0])) else players[0]
        if host not in act:
            continue
        # a player still on the loading screen has not received its GAME-START (the match itself not started, or a joiner waiting for its own):
        # its ping is not judged and no timer runs for it until the start went out
        with MM.lock:
            m_started = mid in MM.started
            joining = set(MM.joining)
        waiting = set(act) if not m_started else {p for p in act if p in joining}
        for p in waiting:
            PING_HIGH.pop((p, mid), None)
        pings = {p: _ping_fresh(p, host, now) for p in act if p != host and p not in waiting}
        high = [p for p, ms in pings.items() if ms is not None and ms > PING_KICK_MS]
        known = [p for p, ms in pings.items() if ms is not None]
        if len(known) >= 2 and len(high) * 2 > len(known):         # most of them are high to the host: its link is the problem, nobody is removed
            for p in act:
                PING_HIGH.pop((p, mid), None)
            continue
        for p in act:
            if p == host or p in waiting:
                continue
            key = (p, mid)
            live.add(key)
            if pings.get(p) is None and MM_PING_GATE:
                # match 06930992 (a 255 ms player never removed): the pair ping went stale (> PING_FRESH_S) between Steam estimates about once
                # a minute and the unknown value counted as 'back under', so the 60 s timer restarted every ~70 s. Unknown keeps the timer as it is;
                # the removal still needs a fresh reading over the limit.
                continue
            if p not in high:
                if PING_HIGH.pop(key, None) is not None:
                    log("ping:", tag(p), "back under %d ms to the host in match %s (%s ms)" % (PING_KICK_MS, mid, pings.get(p)))
                continue
            t0 = PING_HIGH.setdefault(key, now)
            if t0 == now:
                log("ping:", tag(p), "%d ms to the host %s in match %s - over the %d ms limit, removed after %d s of it" % (pings[p], tag(host), mid, PING_KICK_MS, PING_KICK_S))
            if now - t0 < PING_KICK_S or key in PING_KICKED:
                continue
            PING_KICKED.add(key)
            if len(PING_KICKED) > 2000:
                PING_KICKED.clear()
            ms, secs = int(pings[p]), int(now - t0)
            try:
                ping_kick_learn(mid, p, host, ms, now)
            except Exception as e:                           # noqa: BLE001 - the memory never blocks the removal
                log("ping-learn ERR", repr(e))
            log("ping:", tag(p), "%d ms to the host for %d s in match %s - removed from the match (not counted as leaving early)" % (ms, secs, mid))
            did = kick_token(p, "removed: high ping (%d ms to the host for %d s)" % (ms, secs),
                             srv_text("ping_kicked", lang_of(p), ms=ms, s=secs, lim=PING_KICK_MS), "ping-kick",
                             "%d ms to the host for %d s (limit %d ms)" % (ms, secs, PING_KICK_MS), keep_out=True, no_penalty=True)
            log("ping:", tag(p), "removal done:", ", ".join(did) or "nothing")
            out.append(p)
    for k in [k for k in PING_HIGH if k not in live]:            # finished matches / departed players
        PING_HIGH.pop(k, None)
    return out

def parse_loss(pl):
    """Trailer 3 of an op 0xCA payload (flags bit2; after trailer 1 and, with flags bit1, trailer 2):
    [u8 ver=1][u8 flags: bit0 the reporter's own msg-7 sends are paced (t2r-m7-lod.txt), bit1 its host upload budget is on (t2r-host-bw.txt)][u8 n]
    n x ([u8 slot][u32 LE FNV-1a of the peer's roster name, up to 24 B][u16 LE msg-7 received][u16 LE msg-7 lost]) = what the reporter
    received from each other player since its last report (9 B per peer: clan_send caps the payload). -> {"paced", "hbw", "peers": [(slot,
    name hash, received, lost)]} or None."""
    if len(pl) < 72 + 7 or not (pl[71] & 4) or pl[72] != 1:
        return None
    off = 79 + (7 if pl[71] & 2 else 0)
    if len(pl) < off + 3 or pl[off] != 1:
        return None
    fl, n = pl[off + 1], pl[off + 2]
    if n > 32 or len(pl) < off + 3 + n * 9:
        return None
    out = []
    for i in range(n):
        e = pl[off + 3 + i * 9: off + 3 + (i + 1) * 9]
        out.append((e[0], int.from_bytes(e[1:5], "little"), int.from_bytes(e[5:7], "little"), int.from_bytes(e[7:9], "little")))
    return {"paced": bool(fl & 1), "hbw": bool(fl & 2), "peers": out}

def _roster_key(nm):
    """FNV-1a 32 of a name as the ext reads it back from its peer record: the roster's latin1 bytes (build_player_record), the first 24, up to a NUL."""
    if isinstance(nm, str):
        nm = nm.encode("latin1", "ignore")
    h = 2166136261
    for c in bytes(nm[:24]).split(b"\0", 1)[0]:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h

def loss_note(token, rep, now=None):
    """Book one reporter's per-peer loss in its live match. A name that is not exactly one other player of that match is skipped."""
    if not rep or LOSS_KICK_PCT <= 0:
        return
    now = time.time() if now is None else now
    mid, act = MM.active_of(token)
    if not mid:
        return
    LOSS_FLAGS[(mid, token)] = (rep["paced"], rep["hbw"])
    by = {}
    for p in act:
        if p != token:
            by.setdefault(_roster_key(get_name(p) or ""), []).append(p)
    for slot, h, rx, lost in rep["peers"]:
        subj = by.get(h)
        if not subj or len(subj) != 1 or rx + lost <= 0:
            continue
        LOSS.setdefault((mid, token, subj[0]), collections.deque(maxlen=4)).append((now, rx, lost))
    if len(LOSS) > 4000:                             # bounded: oldest keys out (finished matches are swept by loss_check)
        for k in list(LOSS)[:1000]:
            LOSS.pop(k, None)

def loss_of(mid, subj, now):
    """{observer: loss %} of one player in one match over the last LOSS_WINDOW_S, observers with LOSS_MIN_PKTS or more only."""
    out = {}
    for (m, obs, s), d in list(LOSS.items()):
        if m != mid or s != subj:
            continue
        rx = sum(r for t, r, l in d if now - t <= LOSS_WINDOW_S)
        lo = sum(l for t, r, l in d if now - t <= LOSS_WINDOW_S)
        if rx + lo >= LOSS_MIN_PKTS:
            out[obs] = 100.0 * lo / (rx + lo)
    return out

def loss_check(now=None):
    """Every status tick (10 s): the packet loss limit (LOSS_KICK_PCT / LOSS_KICK_S, see the table above). Returns the tokens removed."""
    now = time.time() if now is None else now
    out = []
    if LOSS_KICK_PCT <= 0:
        return out
    live, mids = set(), set()
    for mid, players, formed, launched, ended, _, dep in MM.snapshot():
        if not launched or ended:
            continue
        mids.add(mid)
        if now - formed < PING_GRACE_S:
            continue
        act = [p for p in players if p not in dep]
        if len(act) < LOSS_MIN_OBS + 1 or PRIV.rule(mid, "ping") == 0:     # a private match with PING LIMIT off has no loss limit either
            continue
        with MM.lock:
            m_started = mid in MM.started
            joining = set(MM.joining)
        if not m_started:
            continue
        if any(LOSS_FLAGS.get((mid, p), (0, 0))[1] for p in act):   # a host upload budget drops relayed msg 7 on purpose: seq gaps are not loss
            continue
        med = {}
        for p in act:
            if p in joining or LOSS_FLAGS.get((mid, p), (0, 0))[0]:   # its own sends to far peers are paced: gaps by design
                continue
            obs = {o: v for o, v in loss_of(mid, p, now).items() if o in act and o != p}
            if len(obs) >= LOSS_MIN_OBS:
                v = sorted(obs.values())
                med[p] = (v[len(v) // 2] if len(v) % 2 else (v[len(v) // 2 - 1] + v[len(v) // 2]) / 2.0, len(v))
        high = [p for p, (v, n) in med.items() if v >= LOSS_KICK_PCT]
        near = [p for p, (v, n) in med.items() if v >= LOSS_KICK_PCT / 2.0]
        if len(med) >= 2 and len(near) * 2 >= len(med):          # half or more are lossy: the host / relay link is the problem, nobody is removed
            for p in act:
                LOSS_HIGH.pop((p, mid), None)
            continue
        for p in act:
            key = (p, mid)
            live.add(key)
            if p not in med:                                      # unknown keeps the timer as it is (as the ping limit)
                continue
            pct, nobs = med[p]
            rest = sorted(v for q, (v, n) in med.items() if q != p)
            base = (rest[len(rest) // 2] if len(rest) % 2 else (rest[len(rest) // 2 - 1] + rest[len(rest) // 2]) / 2.0) if rest else 0.0
            if p in high and base >= LOSS_BASE_PCT:               # the others lose packets too: the match's route, not this player alone
                if LOSS_HIGH.pop(key, None) is not None:
                    log("loss:", tag(p), "%.1f%% but the rest of match %s loses %.1f%% too (limit %.0f%%): not judged" % (pct, mid, base, LOSS_BASE_PCT))
                continue
            if p not in high:
                if LOSS_HIGH.pop(key, None) is not None:
                    log("loss:", tag(p), "back under %.0f%% in match %s (%.1f%%, %d observers)" % (LOSS_KICK_PCT, mid, pct, nobs))
                continue
            t0 = LOSS_HIGH.setdefault(key, now)
            if t0 == now:
                log("loss:", tag(p), "median packet loss %.1f%% seen by %d players in match %s - at or over the %.0f%% limit, removed after %d s of it%s"
                    % (pct, nobs, mid, LOSS_KICK_PCT, LOSS_KICK_S, "" if LOSS_GATE >= 2 else " (LOG ONLY)"))
            if now - t0 < LOSS_KICK_S or key in LOSS_KICKED:
                continue
            LOSS_KICKED.add(key)
            if len(LOSS_KICKED) > 2000:
                LOSS_KICKED.clear()
            secs, ipct = int(now - t0), int(round(pct))
            if LOSS_GATE < 2:
                log("loss:", tag(p), "%.1f%% loss for %d s in match %s - would be removed (T2R_LOSS_GATE=%d: log only)" % (pct, secs, mid, LOSS_GATE))
                continue
            sk = BRIDGES.steamid(p) or p
            st = [t for t in LOSS_STRIKES.get(sk, []) if now - t < LOSS_REPEAT_S] + [now]
            LOSS_STRIKES[sk] = st
            if len(LOSS_STRIKES) > 5000:
                LOSS_STRIKES.clear()
            hold = LOSS_HOLD_S if len(st) >= 2 and LOSS_HOLD_S > 0 else 0
            if hold:
                LOSS_HOLD[sk] = (now, ipct)
                if len(LOSS_HOLD) > 5000:
                    LOSS_HOLD.clear()
            log("loss:", tag(p), "%.1f%% packet loss (%d observers, rest of the match %.1f%%) for %d s in match %s - removed from the match (not counted as leaving early), %s"
                % (pct, nobs, base, secs, mid, "searches held %d s (removal %d in %.0f h)" % (hold, len(st), LOSS_REPEAT_S / 3600) if hold else "may search again at once (first removal)"))
            ktxt = (srv_text("loss_kicked", lang_of(p), pct=ipct, s=secs, lim=int(LOSS_KICK_PCT), m=max(1, int(hold // 60))) if hold else
                    srv_text("loss_kicked_free", lang_of(p), pct=ipct, s=secs, lim=int(LOSS_KICK_PCT)))
            did = kick_token(p, "removed: packet loss (%d%% for %d s)" % (ipct, secs), ktxt, "loss-kick",
                             "%d%% packet loss for %d s (limit %d%%)" % (ipct, secs, int(LOSS_KICK_PCT)), keep_out=True, no_penalty=True)
            log("loss:", tag(p), "removal done:", ", ".join(did) or "nothing")
            out.append(p)
    for k in [k for k in LOSS_HIGH if k not in live]:
        LOSS_HIGH.pop(k, None)
    for k in [k for k in LOSS if k[0] not in mids]:               # finished matches
        LOSS.pop(k, None)
    for k in [k for k in LOSS_FLAGS if k[0] not in mids]:
        LOSS_FLAGS.pop(k, None)
    return out

def loss_hold_note(token, now=None):
    """The note a new search is refused with while the player's loss hold runs (LOSS_HOLD_S after a loss removal), else None."""
    if LOSS_GATE < 2 or LOSS_HOLD_S <= 0 or not LOSS_HOLD:
        return None
    now = time.time() if now is None else now
    k = BRIDGES.steamid(token) or token
    h = LOSS_HOLD.get(k)
    if not h:
        return None
    left = LOSS_HOLD_S - (now - h[0])
    if left <= 0:
        LOSS_HOLD.pop(k, None)
        return None
    return srv_note("loss_hold", token, pct=h[1], m=max(1, int((left + 59) // 60)))

def admin_kick(pid, reason="", move=False):
    """Clear a player's stuck server-side state so it can matchmake again: queue, match membership (a not yet launched match is called off and the
    others go back to searching, a running one is left like a normal leave: the others play on), LOADED / countdown / silence marks, presence,
    and a 0x0d (back to the menu) on its held matchmaking socket. `pid` = the 12-hex id live_snapshot() gives (sha256 of the token). Returns a dict."""
    import hashlib
    try:
        if not isinstance(pid, str) or not (4 <= len(pid) <= 64) or any(ch not in "0123456789abcdef" for ch in pid.lower()):
            return {"ok": False, "error": "bad pid"}
        pid = pid.lower()
        with PRES.lock:
            cand = set(PRES.players)
        with MM.lock:
            cand |= set(MM.queue)
            for pl in MM.matches.values():
                cand |= set(pl)
        with SESSIONS.lock:
            cand |= set(SESSIONS.by_token)
        cand |= set(LOADED)
        toks = [t for t in cand if t and hashlib.sha256(t.encode()).hexdigest().startswith(pid)]
        if len(toks) != 1:
            return {"ok": False, "error": "no such player" if not toks else "ambiguous pid"}
        tok = toks[0]
        name = tag(tok)
        ktxt = srv_text("moved", lang_of(tok)) if move else kick_text(reason, lang_of(tok))
        did = kick_token(tok, "team move" if move else "admin kick", ktxt, "admin-kick", kick_reason_clean(reason), no_penalty=not move)
        return {"ok": True, "name": name, "did": did}
    except Exception as e:
        log("ADMIN-KICK-ERR", repr(e))
        return {"ok": False, "error": "kick failed: %r" % (e,)}

CLAN_RESET_DIR = os.path.join(os.path.dirname(os.path.abspath(DB_PATH)), "clan-resets")

def admin_clan_reset(body):
    """POST /clan/reset {"name", "steamid", "tokenIds": [24-hex install ids of the person], "names": false} (admin site player page): backs up and wipes the
    person's clan (progress, boosters, roster, diary, A-Life) so the next poll reads 'no clan' -> CHOOSE YOUR SIDE like a new player. The game client keeps no clan
    state of its own (it re-reads every poll), so nothing else has to be told; in-memory leftovers (pending day report) are dropped here."""
    try:
        if clan_reset is None:
            return {"ok": False, "error": "clan_reset.py missing in this build"}
        name = str(body.get("name") or "player")[:64]
        sid = int(body.get("steamid") or 0)
        tids = [str(t) for t in (body.get("tokenIds") or [])][:64]
        with _dblock, dbc() as c:
            extra = [clan.token_id(t) for (t,) in c.execute("SELECT token FROM players WHERE steamid=?", (sid,))] if sid > 0 else []
            keys_all = tids + extra
            r = clan_reset.reset(c, CLAN_RESET_DIR, name, sid, keys_all, bool(body.get("names")))
        if not r.get("ok"):
            return r
        ks = set(r["keys"])
        with _clanlock:
            for t in [t for t in CLAN_PENDING if clan.token_id(t) in ks]:
                CLAN_PENDING.pop(t, None)
        evt("admin-clan-reset", name, sid, r["backup"])
        log("CLAN reset by admin:", name, "steam", sid, "rows", r["rows"], "backup", r["backup"])
        return r
    except Exception as e:
        log("ADMIN-CLAN-RESET-ERR", repr(e))
        return {"ok": False, "error": "reset failed: %r" % (e,)}

def admin_clan_restore(body):
    try:
        if clan_reset is None:
            return {"ok": False, "error": "clan_reset.py missing in this build"}
        with _dblock, dbc() as c:
            r = clan_reset.restore(c, CLAN_RESET_DIR, str(body.get("file") or ""))
        if r.get("ok"):
            log("CLAN restore by admin:", r)
        return r
    except Exception as e:
        log("ADMIN-CLAN-RESTORE-ERR", repr(e))
        return {"ok": False, "error": "restore failed: %r" % (e,)}

def admin_server():
    import hmac
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    secret = admin_secret()
    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"
        timeout = float(os.environ.get("T2R_ADMIN_TIMEOUT_S", "10"))     # a connection that never finishes its request line / body is closed (it held a thread for ever)
        def log_message(self, *a): pass
        def _send(self, code, obj):
            b = json.dumps(obj, default=str).encode()
            self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b)))
            self.send_header("Cache-Control", "no-store"); self.end_headers(); self.wfile.write(b)
        def _authed(self):
            return hmac.compare_digest(self.headers.get("X-Admin-Secret", ""), secret)
        def do_GET(self):
            try:
                if self.path == "/status":
                    return self._send(200, {"ok": True, "online": PRES.count(), "queue": len(MM.queue), "matches": len(MM.matches),
                                            "server_version": SERVER_VERSION, "uptime_s": round(time.time() - STARTED_AT)})
                if not self._authed():
                    return self._send(403, {"ok": False, "error": "forbidden"})
                if self.path == "/notices":
                    return self._send(200, {"ok": True, "notices": notices_public(), "target_version": update_target(), "now": wall_time()})
                if self.path == "/sizes":
                    return self._send(200, {"ok": True, "sizes": state_sizes(), "uptime_s": round(time.time() - STARTED_AT)})
                if self.path == "/teams":
                    return self._send(200, {"ok": True, "rules": team_rules_text(), "preview": teams_preview(), "live": teams_live()})
                if self.path.startswith("/clan/backups"):
                    sid = int(self.path.partition("steamid=")[2] or 0) if "steamid=" in self.path else 0
                    return self._send(200, {"ok": True, "backups": clan_reset.list_backups(CLAN_RESET_DIR, sid or None) if clan_reset else []})
                if self.path == "/live":
                    return self._send(200, {"ok": True, "ts": wall_time(), "server_version": SERVER_VERSION, "live": live_snapshot()})
                self._send(404, {"ok": False})
            except Exception as e:
                log("ADMIN-ERR", repr(e))
        def do_POST(self):
            try:
                if not self._authed():
                    return self._send(403, {"ok": False, "error": "forbidden"})
                n = min(int(self.headers.get("Content-Length") or 0), 8192)
                body = json.loads(self.rfile.read(n) or b"{}")
                if self.path == "/notice":
                    sec = body.get("seconds")
                    nid = notice_send(str(body.get("text", "")), float(body.get("minutes", 30) or 30), float(sec) if sec is not None else None, str(body.get("text_pl", "") or ""))
                    return self._send(200, {"ok": bool(nid), "id": nid} if nid else {"ok": False, "error": "empty text"})
                if self.path == "/notice/clear":
                    return self._send(200, {"ok": NOTICES.clear(int(body.get("id", -1)))})
                if self.path == "/shutdown":
                    return self._send(200, {"ok": True, "shutdown": shutdown_schedule(float(body.get("minutes", 5)), str(body.get("reason", "")), bool(body.get("restart", True)), str(body.get("reason_pl", "") or ""))})
                if self.path == "/boot-all":
                    return self._send(200, {"ok": True, "closed": boot_all_sessions()})
                if self.path == "/shutdown/cancel":
                    return self._send(200, {"ok": shutdown_cancel()})
                if self.path == "/teams/move":
                    return self._send(200, admin_move_team(body.get("pid")))
                if self.path == "/teams":
                    team_rules_set(body.get("rules", ""))
                    return self._send(200, {"ok": True, "rules": team_rules_text(), "preview": teams_preview()})
                if self.path == "/kick":
                    return self._send(200, admin_kick(body.get("pid"), body.get("reason") or ""))
                if self.path == "/handover":
                    # A/B test aid (hm-peer0 fix): start a planned host handover of match `mid` (default: the only live one) to `slot` now,
                    # without the re-pick's ping / age rules. Needs T2R_HOST_REPICK=2 and the admin secret like every admin call.
                    mids = list(HM.mids())
                    mid = body.get("mid") or (mids[0] if len(mids) == 1 else None)
                    if REPICK.mode < 2 or mid is None:
                        return self._send(200, {"ok": False, "error": "needs T2R_HOST_REPICK=2 and one live match (or mid)", "mids": mids})
                    ok = HM.handover(mid, int(body.get("slot", 1)))
                    log("ADMIN handover", mid, "to slot", body.get("slot", 1), "->", ok, "|", HM.handover_ok(mid)[1])
                    return self._send(200, {"ok": bool(ok), "mid": mid, "why": HM.handover_ok(mid)[1]})
                if self.path == "/reset-session":
                    return self._send(200, admin_reset_session(body.get("pid")))
                if self.path == "/clan/reset":
                    return self._send(200, admin_clan_reset(body))
                if self.path == "/clan/restore":
                    return self._send(200, admin_clan_restore(body))
                self._send(404, {"ok": False})
            except Exception as e:
                log("ADMIN-ERR", repr(e))
                try: self._send(400, {"ok": False, "error": "bad request"})
                except Exception: pass
    # ("Notices need the daemon admin listener: Forbidden"): an older daemon (test, port 7101) still held 127.0.0.1:7011 with its own
    # secret; HTTPServer's SO_REUSEADDR let the live daemon bind the same port on Windows and the admin site's requests reached the old one.
    # Exclusive bind: a taken port fails here and is logged ("ADMIN server not started").
    class XServer(ThreadingHTTPServer):
        allow_reuse_address = False
        def server_bind(self):
            bind_exclusive(self.socket, "ADMIN", self.server_address[0], self.server_address[1], fatal=False)   # exclusive + bounded retry (restart); raises when still taken
            self.server_address = self.socket.getsockname()
            self.server_name = self.server_address[0]; self.server_port = self.server_address[1]
    srv = XServer(("127.0.0.1", ADMIN_PORT), H)
    srv.daemon_threads = True
    log("ADMIN listening on 127.0.0.1:%d (secret file %s)" % (ADMIN_PORT, ADMIN_SECRET_PATH))
    srv.serve_forever()

def admin_start():
    if ADMIN_PORT <= 0:
        return
    def run():
        try: admin_server()
        except Exception as e: log("ADMIN server not started:", repr(e))
    threading.Thread(target=run, daemon=True).start()

def merge_dbs(paths):
    """python daemon.py --merge-db a.db b.db ...: build DB_PATH (must not exist yet) from several playtest databases (a new
    PT folder started a fresh t2r.db and the clans of the last PT were gone). Newest file is the base; from the others: a clan is kept
    when it is further along (weeks_total, day) or as far and newer; players keep the row with more matches; names / boosters union."""
    import shutil
    paths = [p for p in paths if os.path.exists(p)]
    if not paths or os.path.exists(DB_PATH):
        print("merge-db: nothing to do (no sources, or %s exists)" % DB_PATH); return
    paths.sort(key=os.path.getmtime, reverse=True)
    os.makedirs(os.path.dirname(DB_PATH) or ".", exist_ok=True)
    src = sqlite3.connect("file:%s?mode=ro" % paths[0], uri=True); dst = sqlite3.connect(DB_PATH)
    src.backup(dst); src.close()
    tabs = {r[0] for r in dst.execute("select name from sqlite_master where type='table'")}
    for p in paths[1:]:
        o = sqlite3.connect("file:%s?mode=ro" % p, uri=True)
        otabs = {r[0] for r in o.execute("select name from sqlite_master where type='table'")}
        def cols(c, t): return [r[1] for r in c.execute("pragma table_info(%s)" % t)]
        if "clans" in tabs & otabs:
            cc = cols(dst, "clans"); oc = cols(o, "clans"); common = [c for c in cc if c in oc]
            for row in o.execute("select %s from clans" % ",".join(common)):
                r = dict(zip(common, row))
                cur = dst.execute("select weeks_total, day, updated from clans where token_id=?", (r["token_id"],)).fetchone()
                better = cur is None or ((r.get("weeks_total") or 0, r.get("day") or 0, r.get("updated") or 0) > (cur[0] or 0, cur[1] or 0, cur[2] or 0))
                if better:
                    dst.execute("insert or replace into clans(%s) values(%s)" % (",".join(common), ",".join("?" * len(common))), row)
        if "players" in tabs & otabs:
            pc = [c for c in cols(dst, "players") if c in cols(o, "players")]
            for row in o.execute("select %s from players" % ",".join(pc)):
                r = dict(zip(pc, row))
                cur = dst.execute("select matches from players where token=?", (r["token"],)).fetchone()
                if cur is None or (r.get("matches") or 0) > (cur[0] or 0):
                    dst.execute("insert or replace into players(%s) values(%s)" % (",".join(pc), ",".join("?" * len(pc))), row)
        for t, key in (("names", "token_id"), ("boosters", None)):
            if t in tabs & otabs:
                tc = [c for c in cols(dst, t) if c in cols(o, t)]
                dst.executemany("insert or ignore into %s(%s) values(%s)" % (t, ",".join(tc), ",".join("?" * len(tc))),
                                o.execute("select %s from %s" % (",".join(tc), t)).fetchall())
        o.close()
    dst.commit(); dst.close()
    print("merge-db: %s built from %d database(s)" % (DB_PATH, len(paths)))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--merge-db":
        merge_dbs(sys.argv[2:]); sys.exit(0)
    threading.Thread(target=_flusher, daemon=True).start()
    log("DB check:", db_guard())
    live_load()
    log("daemon version", SERVER_VERSION or "dev (no version check)", "| require client version:", REQUIRE_VERSION)
    threading.Thread(target=supervised(udp_server, "udp"), daemon=True).start()
    threading.Thread(target=supervised(status_loop, "status"), daemon=True).start()
    threading.Thread(target=supervised(matchmaker_loop, "matchmaker"), daemon=True).start()
    _clan_sched[0] = True; threading.Thread(target=supervised(clan_scheduler, "clan-scheduler"), daemon=True).start()
    admin_start()
    install_exit_hooks()
    threading.Thread(target=ident_backfill, daemon=True).start()     # identity ids for accounts recorded before ident.py (optional)
    log("daemon start")
    tcp_server()
