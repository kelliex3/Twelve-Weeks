// peertrust.h - receive-side trust helpers for the ext's peer messages. Pure C (no engine calls): tests/peertrust_test.c
// compiles this file and throws random payloads at every helper. The sender SLOT is stamped by winmm (trusted); every payload byte is not.
#ifndef PEERTRUST_H
#define PEERTRUST_H
#include <stdint.h>
#include <string.h>
#define PT_SLOTS 8
// ---- numbers from the wire
static inline int PtFOk(float f, float lim)
{
    return f == f && f > -lim && f < lim;
}                               // false for NaN / inf / |f| >= lim

static inline int PtV3Ok(const float *v, float lim)
{
    return PtFOk(v[0], lim) && PtFOk(v[1], lim) && PtFOk(v[2], lim);
}

static inline int PtQuatOk(const float *q)
{                               // finite, roughly unit (a zero quat makes NaN transforms)
    if (!PtFOk(q[0], 4.0f) || !PtFOk(q[1], 4.0f)
        || !PtFOk(q[2], 4.0f) || !PtFOk(q[3], 4.0f))
        return 0;
    float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    return n >= 0.25f && n <= 4.0f;
}

static inline int PtSlotOk(uint32_t s)
{
    return s < PT_SLOTS;
}

// a slot byte that may also be "none" (0xff)
static inline int PtSlotOrNone(uint32_t s)
{
    return s < PT_SLOTS || s == 0xffu;
}

// MSG_UC4_REACH [u8 target][u8 on]: a START names a real slot that is not the sender; a STOP (on = 0) carries whatever target the sender last had - uc4_reach(99, 0) is sent
// when we go down mid-revive (no target any more), and a dropped STOP leaves the reviver's puppet reaching until the 5 s timeout
static inline int PtReachWireBad(uint32_t tgt, uint32_t on,
                                    uint32_t from)
{
    return (tgt >= PT_SLOTS && on) || tgt == from;
}

// a count claimed by the payload, clamped to what the table holds and what the message really carries (per = bytes per entry, hdr = bytes before the entries)
static inline uint32_t PtCount(uint32_t claimed, uint32_t table_max,
                                uint64_t len, uint32_t Hdr, uint32_t per)
{
    if (len < Hdr || !per)
        return 0;
    uint64_t fit = (len - Hdr) / per;
    uint32_t n = claimed < table_max ? claimed : table_max;
    return n < fit ? n : (uint32_t) fit;
}

// ---- host-only messages: role 0 none / 1 host / 2 client. A client takes them from the slot it believes hosts; the host takes none (nobody else is host).
static inline int PtHostOk(uint32_t slot, int cur_host, int role,
                             uint32_t me)
{
    if (role == 2)
        return slot < PT_SLOTS && (int) slot == cur_host;
    if (role == 1)
        return 0;
    return slot != me;          // no session role (solo test): anything but ourselves
}

// ---- distance judged on the sender's puppet (have = its position is known; unknown = the caller lets it pass)
static inline int PtWithin(int have, float dx, float dy, float dz,
                            float maxxz, float maxy)
{
    if (!have)
        return 1;
    if (!PtFOk(dx, 1e7f) || !PtFOk(dy, 1e7f) || !PtFOk(dz, 1e7f))
        return 0;
    return dx * dx + dz * dz <= maxxz * maxxz && dy >= -maxy && dy <= maxy;
}

// ---- rate limit: fixed 1 s window per (sender, type); pt_log_due = true at most once per 5 s per sender (the caller keeps log_ms per sender)
typedef struct {
    uint64_t win_ms;
    uint32_t n, dropped;
} PtRlT;
static inline int PtRate(PtRlT *r, uint64_t now, uint32_t limit_per_s)
{
    if (now < r->win_ms || now - r->win_ms >= 1000) {
        r->win_ms = now;
        r->n = 0;
    }
    if (r->n >= limit_per_s) {
        r->dropped++;
        return 0;
    }
    r->n++;
    return 1;
}

static inline int PtLogDue(uint64_t *log_ms, uint64_t now)
{
    if (*log_ms && now >= *log_ms && now - *log_ms < 5000)
        return 0;
    *log_ms = now ? now : 1;
    return 1;
}

// messages per second one sender may send of one ext type. Generous (an honest peer never gets near it); the point is the cap on per-frame work.
static inline uint32_t PtLimit(uint8_t type)
{
    switch (type) {
    case 0x41:
    case 0x44:
    case 0x45:
    case 0x49:
    case 0x4F:
    case 0x52:
    case 0x54:
    case 0x77:
    case 0x7B:
    case 0x94:
        return 20;              // revive, stun, burst, heal, markpoint, kf weapon ...
    case 0x53:
        return 6;               // crafter gift box
    case 0x43:
    case 0x47:
    case 0x48:
    case 0x4C:
    case 0x4E:
    case 0x50:
    case 0x51:
    case 0x58:
    case 0x60:
    case 0x64:
    case 0x66:
    case 0x67:
    case 0x97:
    case 0x98:
    case 0x9C:
    case 0x68:
    case 0x6A:
    case 0x6B:
    case 0x6D:
    case 0x6E:
    case 0x6F:
    case 0x72:
    case 0x74:
    case 0x7C:
    case 0x93:
    case 0x95:
        return 40;
    case 0xAF:
        return 30;              // shot lines (<= 15/s honest; MSG_SHOTRAY, immersion.c)
    case 0x62:
        return 80;              // mines (resync sends a burst)
    case 0x6C:
        return 400;             // reliable hit copies + acks
    case 0x7D:
        return 160;             // bomb track every 3 frames per bomb
    default:
        return 120;
    }
}

// ---- MSG_MINE
// the id is (placer slot << 8 | counter) in every build: a placement whose id names another slot is a spoof (it would shadow that player's mines)
static inline int PtMineIdOk(uint32_t from, uint16_t id,
                                const float *pos)
{                               // the wire part: id names the sender, finite position
    return from < PT_SLOTS && (uint32_t) (id >> 8) == from
        && PtV3Ok(pos, 1e5f);
}

static inline int PtMineCapOk(int owner_live, int cap)
{
    return owner_live < cap + 2;
}                               // the state part (needs the receiver's table: can lag)

static inline int PtMinePlaceOk(uint32_t from, uint16_t id,
                                   const float *pos, int owner_live,
                                   int cap)
{
    return PtMineIdOk(from, id, pos) && PtMineCapOk(owner_live, cap);
}

// ---- the roll / cycle numbers of item spots, health kits, container loot: a taken-claim may name the roll we hold (rolls - 1) or at most one ahead
static inline int PtRollOk(int n, int rolls)
{
    return n >= 0 && n <= rolls + 1 && n <= 250;
}

// ---- revive: the reviver is a living teammate of the revived, the revived is down, and they stand together (9 m: the hand-out reach + lag)
enum { PT_RV_OK =
        0, PT_RV_SLOT, PT_RV_ENEMY, PT_RV_SENDER_DOWN, PT_RV_TARGET_UP,
        PT_RV_FAR };
static inline int PtReviveOk(int slot_ok, int target_ok, int teammates,
                               int sender_downed, int target_downed,
                               int have_pos, float dx, float dy, float dz)
{
    if (!slot_ok || !target_ok)
        return PT_RV_SLOT;
    if (!teammates)
        return PT_RV_ENEMY;
    if (sender_downed)
        return PT_RV_SENDER_DOWN;
    if (!target_downed)
        return PT_RV_TARGET_UP;
    if (!PtWithin(have_pos, dx, dy, dz, 9.0f, 4.0f))
        return PT_RV_FAR;
    return PT_RV_OK;
}

// ---- switches. Master: t2r-no-peertrust.txt next to the exe, or env T2R_PEERTRUST=0 -> every check below is bypassed (the old behaviour), re-read at most once
// per second. Per check: PT_DROP rejects, PT_LOG only logs "would drop" (rate limited, the message goes through exactly as before), PT_OFF skips it. The defaults:
// PT_DROP only where the rule is a pure property of the bytes (finite floats, a state byte no sender ever writes, a slot >= 8, an id that names another player's slot)
// and was verified against the sending code; every rule that needs the receiver's own picture of the world (teams, positions, who is down, who is host, rolls)
// is PT_LOG, because that picture lags / is stale around joins, host migrations and loss and a wrong drop is a desync. t2r-peertrust.txt (optional) overrides:
// whitespace / comma separated "name=drop|log|off" tokens, "all=log|off|drop".
enum { PT_OFF = 0, PT_LOG = 1, PT_DROP = 2 };
#define PT_KINDS \
    X(gate_slot, DROP)     /* sender slot >= 8 (255 = queued before the session slot was set) */ \
    X(gate_len, DROP)      /* payload over 4096 bytes (the send size is a u8) */ \
    X(rate_flood, DROP)    /* more than 8x pt_limit() of one type from one sender in a second */ \
    X(rate, LOG)           /* more than pt_limit() */ \
    X(wire, DROP)          /* finite / range sanity of wire numbers: NaN damage, bursts, finisher pos+quat, state byte, kill_s, victim slot, grant winner, agility */ \
    X(dmg_range, LOG)      /* a finite but negative / absurd hit damage */ \
    X(death_victim, LOG)   /* winmm death naming a victim that is not the sender (winmm itself already ignores it) */ \
    X(downed_award, LOG)   /* +100 Downed Enemy spacing 6 s instead of 3 s */ \
    X(revive, LOG)         /* reviver a living teammate of the target, both in reach */ \
    X(revive_award, LOG)   /* +100 Revive only for a down we know of, once per 15 s per pair */ \
    X(finpos_near, LOG)    /* finisher pose within 12 m of where we stand */ \
    X(uc4_team, LOG)       /* UC4 reach target is a teammate of the sender */ \
    X(exec_enemy, LOG)     /* EXEC_E from an enemy */ \
    X(psync_kind, LOG)     /* paired move kind exists */ \
    X(psync_team, LOG)     /* a move on us is an enemy's, a revive a teammate's */ \
    X(burst_near, LOG)     /* stun / nail bomb / explosive arrow burst within reach of its thrower's puppet */ \
    X(burst_rate, LOG)     /* one burst per 0.2 / 0.4 s per thrower */ \
    X(sk_valid, LOG)       /* declared skills exist and fit 14 LP */ \
    X(sk_gift, LOG)        /* crafter gift box from a living teammate with Crafter II, 1.5 s spacing */ \
    X(int_near, LOG)       /* interrogation 'done' from an up player within 9 m */ \
    X(mine_id, DROP)       /* mine placement: id high byte == sender slot, finite position */ \
    X(mine_cap, LOG)       /* live mines of one player below cap + 2 */ \
    X(mine_auth, LOG)      /* mine trigger / remove / shot from the owner or an enemy of the owner */ \
    X(host_only, LOG)      /* seeds / grants / snapshots only from the slot we believe hosts */ \
    X(claim_roll, LOG)     /* taken-claim roll / cycle at most one ahead of the one we hold */ \
    X(claim_near, LOG)     /* taken-claim from within 16 m of the spot */ \
    X(pstow_def, DROP)     /* stow list weapon id has a weapon definition here (else GiveWeapon on garbage) */ \
    X(xbolt_award, LOG)    /* crossbow awards rate / only after a known bolt */ \
    X(intro_budget, LOG)   /* intro guard: 4 announcements / 60 s per slot and match */ \
    X(gore_enemy, LOG)     /* bystander gore only for an enemy's kill */ \
    X(armor_cap, LOG)      /* armour soak / left capped to the piece's points */ \
    X(glass_piece, LOG)    /* glass piece index < 4096 */
enum {
#define X(n, d) PTK_##n,
    PT_KINDS
#undef X
    PTK_N
};
static const char *const PT_NAME[] = {
#define X(n, d) #n,
    PT_KINDS
#undef X
};

typedef struct {
    uint8_t mode[PTK_N];
} PtCfgT;
static inline void PtCfgDefault(PtCfgT *c)
{
    static const uint8_t def[] = {
#define X(n, d) PT_##d,
        PT_KINDS
#undef X
    };
    memcpy(c->mode, def, sizeof def);
}

static inline int PtModeOf(const char *s, size_t n)
{
    if (n == 4 && !strncmp(s, "drop", 4))
        return PT_DROP;
    if (n == 3 && !strncmp(s, "log", 3))
        return PT_LOG;
    if (n == 3 && !strncmp(s, "off", 3))
        return PT_OFF;
    return -1;
}

// text of t2r-peertrust.txt: "name=mode" tokens; unknown names / modes are skipped; returns how many tokens applied
static inline int PtCfgParse(PtCfgT *c, const char *t)
{
    int applied = 0;
    while (*t) {
        while (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n'
               || *t == ',' || *t == ';')
            t++;
        const char *s = t;
        while (*t && *t != ' ' && *t != '\t' && *t != '\r' && *t != '\n'
               && *t != ',' && *t != ';')
            t++;
        size_t n = (size_t) (t - s);
        const char *eq = n ? memchr(s, '=', n) : NULL;
        if (!eq)
            continue;
        int m = PtModeOf(eq + 1, n - (size_t) (eq + 1 - s));
        if (m < 0)
            continue;
        size_t kn = (size_t) (eq - s);
        if (kn == 3 && !strncmp(s, "all", 3)) {
            for (int k = 0; k < PTK_N; k++)
                c->mode[k] = (uint8_t) m;
            applied++;
            continue;
        }
        for (int k = 0; k < PTK_N; k++)
            if (strlen(PT_NAME[k]) == kn && !strncmp(PT_NAME[k], s, kn)) {
                c->mode[k] = (uint8_t) m;
                applied++;
                break;
            }
    }
    return applied;
}

// what a check that found a violation does: 1 = drop it, 0 = let it through (log-only / off / master off). *log = 1 when a "would drop" line is due.
static inline int PtDecide(int master_on, const PtCfgT *c, int k,
                            int *log)
{
    *log = 0;
    if (!master_on || k < 0 || k >= PTK_N)
        return 0;
    if (c->mode[k] == PT_DROP)
        return 1;
    if (c->mode[k] == PT_LOG)
        *log = 1;
    return 0;
}
#endif
