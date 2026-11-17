// Faction look sync - the protocol core (re/faction-looks.md). Pure C, no game / Windows calls, so
// tools/tests/test_faction_looks.py compiles this same file into a test DLL and plays whole matches
// with it (several clients, latency, loss, respawns, late join, host migration, match changes).
// t2r-ext.c includes it (FL_API = static) and does the game side: hooks, strings, sending, healing.
//
// Model: every player owns its look. Own spawn count `seq`, `cur` = the look of this life, `next` =
// the look of the coming life, chosen one life ahead and announced, so a puppet spawned before the
// spawn's message arrives still gets the right look. Message (MSG_FACTION_LOOK, at each own spawn
// and every second): [team][cur idx / 0xff][next idx][seq lo][seq hi][FL_VER][msg counter].
// The counter drops stale / reordered copies (the relay does not keep the order of unreliable sends).
// Receiver: per sender slot, the last message (`peer`) and which of the sender's lives its puppet
// shows (`shown`: seq, idx, died = the sender died / the match changed since that spawn).
#ifndef FACTION_LOOK_CORE_H
#define FACTION_LOOK_CORE_H
#include <stdint.h>
#include <string.h>
#ifndef T2R_MAX_PEERS
#define T2R_MAX_PEERS 8         // player slots (the ext defines it before including this)
#endif
#ifndef FL_API
#define FL_API static
#endif

#define FL_N 8                  // look slots per faction (masks are 8 bits: max 8): 0..FL_MALE_N-1 male, then the female looks
                            // (keep in sync with dc_data_patches.FACTION_LOOKS + FEMALE_LOOKS)
#define FL_MALE_N 6             // male looks per faction (the pool every pick uses unless a female mode widens it, fl_core_set_pool)
#define FL_VER 3                // message version byte (1 = the old 4-look pool, 2 = pools of 3; ignored)
#define FL_UNKNOWN 0xff         // shown idx: spawned before any message, winmm's own string
enum { FL_SPAWN_START = 0, FL_SPAWN_RESPAWN = 1 };
// puppet spawn kinds: CREATE = winmm's per-peer tick / re-teleport (first sight, re-creation),
// RESPAWN = FUN_1800507a0 (the respawn grant 0xc, or winmm's dead-puppet recovery)
enum { FL_PUP_CREATE = 0, FL_PUP_RESPAWN = 1 };
#define FL_FRESH_MS 2000        // a life's message newer than this at a respawn spawn = it announced this life
                            // (lives are >= ~3 s apart: respawn timer); else this spawn is its next life
// fl_core_puppet_spawn results
#define FL_PUP_DEAD (-2)        // the original returns at once (peer +5): no spawn, nothing changed
#define FL_PUP_NOMSG (-1)       // no message from that player yet: winmm's string stays

typedef struct {
    uint8_t valid, team, cur, next, ctr;
    uint16_t seq;
    uint64_t seq_ms, last_ms;
} FlPeerT;                    // seq_ms: its life began (msg)
typedef struct {
    uint8_t valid, team, idx, died, bound, heals;
    uint16_t seq;
    uint64_t ms;
} FlShownT;
typedef struct {
    uint32_t me;                // own slot
    uint16_t seq;               // own spawns (lives) so far
    uint8_t cur, next, team, team_known;
    uint8_t load_spawned;       // this match load already had its first own spawn
    uint8_t prev_state;         // winmm match state (lobby object +0x2b80) last seen
    uint8_t ctr;                // own message counter
    uint8_t warm[2];            // per team: looks already spawned on this client (art streamed in)
    uint8_t pick_lo, pick_hi;   // OUR picks are drawn from [pick_lo, pick_hi) (default 0..FL_MALE_N; female modes: fl_core_set_pool)
    uint64_t spawn_ms;          // last own life start
    uint64_t rng;
    FlPeerT peer[T2R_MAX_PEERS];
    FlShownT shown[T2R_MAX_PEERS];
} FlCoreT;

FL_API void FlCoreInit(FlCoreT *c, uint32_t me, uint64_t seed)
{
    memset(c, 0, sizeof *c);
    c->me = me;
    c->cur = c->next = c->team = 0xff;
    c->pick_lo = 0;
    c->pick_hi = FL_MALE_N;
    c->rng = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

FL_API uint64_t FlCoreRand(FlCoreT *c)
{
    uint64_t s = c->rng;
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return c->rng = s;
}

// A random pool index. Teammates must not share a look: avoid what they show now (puppet here /
// their message's cur) and their announced next; also avoid `avoid` (our own current look) so a
// respawn changes the model. Relaxed in that order when the pool is too small.
FL_API uint8_t FlCorePick(FlCoreT *c, int team, uint8_t avoid)
{
    int tc = 0, tn = 0;
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        if (s == c->me)
            continue;
        const FlShownT *sh = &c->shown[s];
        const FlPeerT *p = &c->peer[s];
        if (sh->valid && sh->team == team && sh->idx < FL_N && !sh->died)
            tc |= 1 << sh->idx;
        if (p->valid && p->team == team) {
            if (p->cur < FL_N)
                tc |= 1 << p->cur;
            if (p->next < FL_N)
                tn |= 1 << p->next;
        }
    }
    int own = avoid < FL_N ? 1 << avoid : 0;
    // (a "prefer looks already spawned here" rule was tried: it locked a 1v1 player to two of
    // the three looks, the third never got spawned; the art is in at the first spawn)
    const int masks[6] = { tc | tn | own, tc | tn, tc | own, tc, own, 0 };
    for (int m = 0; m < 6; m++) {
        uint8_t cand[FL_N];
        int n = 0;
        for (int i = c->pick_lo; i < c->pick_hi && i < FL_N; i++)
            if (!(masks[m] >> i & 1))
                cand[n++] = (uint8_t) i;
        if (n)
            return cand[FlCoreRand(c) % (uint64_t) n];
    }
    return 0;
}

// Which looks WE may pick (female characters, re/female-characters.md): [lo, hi) of the pool. 0..FL_MALE_N = the original male
// pool (female off, what every earlier build did), 0..FL_N = mixed (female about 1 in 4), FL_MALE_N..FL_N = always female.
// A coming life (`next`) outside the new range is re-picked; the life being lived keeps its look. Returns 1 when it changed.
FL_API int FlCoreSetPool(FlCoreT *c, int lo, int hi)
{
    if (lo < 0 || hi > FL_N || lo >= hi
        || (c->pick_lo == lo && c->pick_hi == hi))
        return 0;
    c->pick_lo = (uint8_t) lo;
    c->pick_hi = (uint8_t) hi;
    if (c->team_known && (c->next < lo || c->next >= hi))
        c->next = FlCorePick(c, c->team, c->cur);
    return 1;
}

// Our team (1 = WLF, 0 = Seraphites, anything else = not known). Nothing is picked, announced or
// spawned before the team is known (in game both clients started as "Seraphite (default)":
// the own slot is only set when the session starts, after the intro spawn). Unknown keeps the
// known team; a real change (lobby) picks `next` again and forgets `cur` (it belonged to the other
// side; the message then says "no current life" until the next spawn).
FL_API int FlCoreSetTeam(FlCoreT *c, int raw)
{
    if (raw != 0 && raw != 1)
        return 0;
    if (c->team_known && c->team == raw && c->next < FL_N)
        return 0;
    int changed = c->team_known && c->team != raw;
    c->team = (uint8_t) raw;
    c->team_known = 1;
    c->next = FlCorePick(c, c->team, 0xff);
    if (changed)
        c->cur = 0xff;
    return 1;
}

// A look was spawned on this client (own or puppet): its art is (being) streamed in.
FL_API void FlCoreWarm(FlCoreT *c, int team, int idx)
{
    if (team >= 0 && team <= 1 && idx >= 0 && idx < FL_N)
        c->warm[team] |= (uint8_t) (1 << idx);
}

// Match boundaries (both sides use the same rule): leaving the load / match states (3..6), or
// leaving 6. The next match load starts a new life for everyone: own -> the first own spawn of the
// load advances; peers -> their puppets count as dead (the next puppet spawn is a new life).
FL_API int FlCoreState(FlCoreT *c, uint8_t st)
{
    int was = c->prev_state >= 3 && c->prev_state <= 6, in = st >= 3
        && st <= 6;
    int edge = (was && !in) || (c->prev_state == 6 && st != 6);
    c->prev_state = st;
    if (edge) {
        c->load_spawned = 0;
        for (int s = 0; s < T2R_MAX_PEERS; s++)
            if (c->shown[s].valid)
                c->shown[s].died = 1;
    }
    return edge;
}

// An own spawn. START = winmm's match-start spawns (the intro teleport FUN_1800336f0 in state 3 and
// FUN_1800235f0 at the start, often many seconds apart): one life per match load. RESPAWN = the
// respawn grant (FUN_1800374f0): always a new life (1.5 s guard against a doubled apply).
// Returns 1 when a new life started (send the message).
// -1 = team not known: nothing done, winmm's own look string stays for this spawn.
FL_API int FlCoreOwnSpawn(FlCoreT *c, int kind, uint64_t now)
{
    if (!c->team_known || c->next >= FL_N)
        return -1;
    int same = kind == FL_SPAWN_START ? c->seq && c->load_spawned : c->seq
        && now - c->spawn_ms < 1500;
    c->load_spawned = 1;
    if (same)
        return 0;
    c->spawn_ms = now;
    c->seq++;
    c->cur = c->next;
    c->next = FlCorePick(c, c->team, c->cur);
    return 1;
}

#define FL_MSG_LEN 7
FL_API int FlCoreMsg(FlCoreT *c, uint8_t *b)
{
    if (!c->team_known || c->next >= FL_N)
        return 0;
    b[0] = c->team;
    b[1] = c->seq && c->cur < FL_N ? c->cur : 0xff;
    b[2] = c->next;
    b[3] = (uint8_t) c->seq;
    b[4] = (uint8_t) (c->seq >> 8);
    b[5] = FL_VER;
    b[6] = ++c->ctr;
    return FL_MSG_LEN;
}

// Teammates' looks must differ. Our coming look must not be a teammate's current look, nor the
// coming look a LOWER-slot teammate announced (the higher slot gives way, so both sides agree).
// Picks happen at the same moment on two machines (the intro), so collisions do happen; this
// re-picks `next`. Allowed in the lobby, and in a match only while we are alive and our life is
// >= 1 s old (receivers use our `next` only for our coming spawn; the new one reaches them long
// before our respawn - death + respawn timer; a late copy is healed). Returns 1 when changed.
FL_API int FlCoreNextFix(FlCoreT *c)
{
    if (!c->team_known || c->next >= FL_N)
        return 0;
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        const FlPeerT *p = &c->peer[s];
        if (s == c->me || !p->valid || p->team != c->team)
            continue;
        if (p->cur == c->next || (s < c->me && p->next == c->next)) {
            uint8_t old = c->next;
            c->next = FlCorePick(c, c->team, c->seq ? c->cur : 0xff);
            return c->next != old;
        }
    }
    return 0;
}

#define FL_RECV_CHANGED 1
#define FL_RECV_MISMATCH 2
#define FL_RECV_RESET 4
#define FL_RECV_STALE (-2)
// A message from `slot`. Returns FL_RECV_* bits, -1 = ignored (bad / other version), FL_RECV_STALE =
// an older copy than one already taken (reordered / duplicated). A player that left is cleared by
// the caller (slot empty 2 s), so the next one starts fresh; a life count going back by more than 2
// with a newer counter is a new game in that slot all the same.
FL_API int FlCoreRecv(FlCoreT *c, uint32_t slot, const uint8_t *b,
                        uint64_t len, uint64_t now)
{
    if (slot >= T2R_MAX_PEERS || slot == c->me || !b || len < FL_MSG_LEN
        || b[5] != FL_VER || b[0] > 1 || b[2] >= FL_N || (b[1] >= FL_N
                                                          && b[1] != 0xff))
        return -1;
    FlPeerT *p = &c->peer[slot];
    FlShownT *sh = &c->shown[slot];
    uint16_t seq = (uint16_t) (b[3] | b[4] << 8);
    int r = 0;
    if (p->valid && (int8_t) (b[6] - p->ctr) <= 0)
        return FL_RECV_STALE;
    if (p->valid && (int16_t) (seq - p->seq) < -2) {
        memset(sh, 0, sizeof *sh);
        r |= FL_RECV_RESET;
    } else if (p->valid && (int16_t) (seq - p->seq) < 0)
        return FL_RECV_STALE;
    if (!p->valid || p->seq != seq || p->next != b[2] || p->team != b[0]
        || p->cur != b[1])
        r |= FL_RECV_CHANGED;
    if (!p->valid || p->seq != seq)
        p->seq_ms = now;
    p->valid = 1;
    p->team = b[0];
    p->cur = b[1];
    p->next = b[2];
    p->seq = seq;
    p->ctr = b[6];
    p->last_ms = now;
    // a puppet spawned before any message (winmm's string): it shows the sender's current life
    if (sh->valid && !sh->bound && !sh->died && seq && b[1] < FL_N) {
        sh->bound = 1;
        sh->seq = seq;
        sh->team = b[0];
    }
    if (sh->valid && sh->bound && !sh->died && sh->seq == seq
        && sh->team == b[0] && b[1] < FL_N && sh->idx != b[1])
        r |= FL_RECV_MISMATCH;
    return r;
}

FL_API void FlCorePuppetDead(FlCoreT *c, uint32_t slot)
{
    if (slot < T2R_MAX_PEERS && c->shown[slot].valid)
        c->shown[slot].died = 1;
}

FL_API void FlCorePeerReset(FlCoreT *c, uint32_t slot)
{
    if (slot < T2R_MAX_PEERS) {
        memset(&c->peer[slot], 0, sizeof c->peer[slot]);
        memset(&c->shown[slot], 0, sizeof c->shown[slot]);
    }
}

// winmm spawns slot's puppet (FUN_18004f2d0). Which of the sender's lives does it show?
// RESPAWN (a new life by definition): the life its message announced < FL_FRESH_MS ago (message
// first), else its coming life (next; the usual case, the grant beats the message). Does not
// depend on having seen the death (a late joiner still loading misses deaths).
// CREATE: - same life (no death / match change since our last spawn of it, look known): keep it
//  - the sender died since and we already showed its latest announced life: its coming life (next)
//  - otherwise its current life (cur), or the coming one when it has not spawned yet
// `how` (optional) gets a short reason for the log.
FL_API int FlCorePuppetSpawn(FlCoreT *c, uint32_t slot, int kind,
                                int peer_dead, uint64_t now,
                                const char **how)
{
    const char *h_ = "";
    if (slot >= T2R_MAX_PEERS || slot == c->me)
        return FL_PUP_NOMSG;
    if (peer_dead)
        return FL_PUP_DEAD;
    FlPeerT *m = &c->peer[slot];
    FlShownT *sh = &c->shown[slot];
    if (!m->valid) {
        if (!sh->valid || sh->died) {
            memset(sh, 0, sizeof *sh);
            sh->valid = 1;
            sh->idx = FL_UNKNOWN;
            sh->team = 0xff;
            sh->ms = now;
        }
        if (how)
            *how = "no message yet, winmm's string";
        return FL_PUP_NOMSG;
    }
    int ahead = (int16_t) (sh->seq - m->seq) >= 0;
    if (kind == FL_PUP_RESPAWN) {
        int shown_older = !sh->valid || !sh->bound || !ahead;
        if (m->seq && m->cur < FL_N && shown_older
            && now - m->seq_ms <= FL_FRESH_MS) {
            sh->idx = m->cur;
            sh->seq = m->seq;
            h_ = "respawn, their new life (message first)";
        } else {
            sh->idx = m->next;
            sh->seq = (uint16_t) (m->seq + 1);
            h_ = "respawn, their coming life";
        }
        sh->valid = 1;
        sh->bound = 1;
        sh->team = m->team;
        sh->died = 0;
        sh->heals = 0;
        sh->ms = now;
        if (how)
            *how = h_;
        return sh->idx;
    }
    if (sh->valid && !sh->died && sh->bound && sh->team == m->team
        && sh->idx < FL_N && ahead) {
        if (how)
            *how = "kept (same life)";
        return sh->idx;
    }
    if (sh->valid && sh->died && sh->bound && ahead) {
        sh->idx = m->next;
        sh->seq = (uint16_t) (m->seq + 1);
        h_ = "their coming life";
    } else if (m->seq && m->cur < FL_N) {
        sh->idx = m->cur;
        sh->seq = m->seq;
        h_ = "their current life";
    } else {
        sh->idx = m->next;
        sh->seq = (uint16_t) (m->seq + 1);
        h_ = "their coming life (not spawned yet)";
    }
    sh->valid = 1;
    sh->bound = 1;
    sh->team = m->team;
    sh->died = 0;
    sh->heals = 0;
    sh->ms = now;
    if (how)
        *how = h_;
    return sh->idx;
}

// The look the live puppet of `slot` must have (the sender's cur for the life it shows), or -1 when
// unknown / not the same life. The caller compares it with what the puppet was spawned with and
// re-spawns it (winmm FUN_1800507a0) when they differ: late join (spawned with winmm's string),
// a lost / reordered message, or a puppet re-spawn that was taken for a new life but was not one
// (winmm's dead-puppet recovery while its player lived on: 2 s after our spawn its messages still
// tell the old life).
FL_API int FlCoreHealWant(const FlCoreT *c, uint32_t slot)
{
    if (slot >= T2R_MAX_PEERS || slot == c->me)
        return -1;
    const FlPeerT *m = &c->peer[slot];
    const FlShownT *sh = &c->shown[slot];
    if (!m->valid || !sh->valid || sh->died || !sh->bound || !m->seq
        || m->cur >= FL_N)
        return -1;
    if (sh->seq == m->seq)
        return m->cur;
    if (sh->seq == (uint16_t) (m->seq + 1) && m->last_ms > sh->ms + 2000)
        return m->cur;          // predicted a life that never came
    return -1;
}

FL_API void FlCoreHealed(FlCoreT *c, uint32_t slot, uint64_t now)
{
    if (slot >= T2R_MAX_PEERS)
        return;
    FlShownT *sh = &c->shown[slot];
    sh->idx = c->peer[slot].cur;
    sh->team = c->peer[slot].team;
    sh->seq = c->peer[slot].seq;
    sh->heals++;
    sh->ms = now;
}

// heal_want's look is what the puppet already has: just take the sender's life as the one it shows
FL_API void FlCoreRebind(FlCoreT *c, uint32_t slot)
{
    if (slot >= T2R_MAX_PEERS)
        return;
    FlShownT *sh = &c->shown[slot];
    sh->idx = c->peer[slot].cur;
    sh->team = c->peer[slot].team;
    sh->seq = c->peer[slot].seq;
}

// What the puppet shows now (-1 = unknown / not known yet), for the per-frame string rewrite.
FL_API int FlCoreShown(const FlCoreT *c, uint32_t slot, int *team)
{
    if (slot >= T2R_MAX_PEERS)
        return -1;
    const FlShownT *sh = &c->shown[slot];
    if (!sh->valid || sh->idx >= FL_N || sh->team > 1)
        return -1;
    if (team)
        *team = sh->team;
    return sh->idx;
}
#endif
