// ---------------------------------------------------------------- 2013 explosives: molotov + nail bomb, ONE model
// (re/factions2013/rules/molotov-nailbomb-2013.md; included from t2r-ext.c after ext_lethal_hit)
//
// WHY THIS REPLACES THE OLD CODE: the old model guessed (140-base damage 84 / 35, one ray, horizontal distance for the molotov,
// time-only burn ticks within 3.5 m of a burst for 14 s, one-shots on any hp drop near our own aim target, a relay that
// forwarded the puppet's hp drop) and had several paths that hurt or killed us without the 2013 rules asking for it. The 2013
// engine does this (process-explosion.cpp), and so do we now:
//   blast: at the explode point, up to 6 body probes (head, chest, pelvis, 2 thighs, foot); a ray from the centre to each probe
//   (static world); each reached probe is judged by its own 3-D distance against the ring table; ratio = reached / 6 (kill:
//   reached / 3, clamped); ratio < 0.25 = nothing; kill band = lethal, R1 band = mult x CURRENT hp x ratio, R2 band likewise;
//   the vest absorbs first; the damage is floor()ed.
//   fire: only while the engine's own fire is really hurting us inside a burst's reach (the oracle = an hp drop with no hit
//   message), replaced by 20 hp/s; never on time alone (except a downed body the engine ignores).
// Events come from the engine's own ProjectileThrowable explode (expl_hook): the exact burst point, no aim-target guesses.
// Logs: "explosive: ..." (blast) and "fire: ..." (burn). t2r-old-molotov.txt = the old behaviour (g_old_expl).

#include "throwtest_log.h"      // dev-only structured "throwtest:" lines (tools/win/throwtest_report.py)
#define EX13_NAIL 1
#define EX13_MOLO 2
#define EX13_PBOMB 4            // the PLACED proximity nail bomb (placedbomb2013.c): *bomb-explosion-online* player ring
#define EX13_LAUNCH 5           // the EXPLOSIVE BOW's arrow (expbow2013.c): *gren-launcher-explosion-factions* player ring = the 2013 Launcher's blast
#define EX13_STUN 3             // own stun / smoke bombs only occupy a slot of the throw queue (their stun is the old flow)
#define EX13_PROBES 6
#define EX13_ORIGINS 5          // colliders eat blasts: a probe is reached if the ray from ANY of these origins is clear (burst +0.35 up / +0.8 up / +0.6 up and 0.25 m toward the victim)
#define EX13_MIN_RATIO 0.25f
#define EX13_KILL_ARMOUR_DMG 200.0f
#define EX13_FIRE_DPS 20.0f     // *molotov-flame-damage-online* (explosions.bin)
#define EX13_FIRE_REACH 4.5f    // where an engine fire hurting us is attributed to a burst (engine fire reaches beyond the 3 m ring)
#define EX13_FIRE_CORE 3.0f     // a DOWNED body (the engine fire ignores it) burns on time inside this
#define EX13_FIRE_HOT 2.0f      // a standing body this close to a burst (clear line) burns on time even when the engine fire stays silent (half the bursts within 2 m never hurt through the oracle in 9 reports)
#define EX13_FIRE_DY 1.5f       // vertical window burst <-> our feet for the fire (a patch lies on the floor we stand on)
#define EX13_FIRE_DY_UP 3.0f    // the burst may sit above the floor the flames land and spread on (ledge, stairs, prop): the engine-fire oracle confirms we really burn, so the window upward is wide
#define EX13_HELMET_ABSORBS 1   // 2013 (PS4 FUN_0003b280): vest AND helmet both within the 5 m armour radius of every blast, nearest first

static struct {
    int kind;
    uint64_t ms;
} g_ex_own[6];                  // our throws still in flight, oldest first
static struct {
    volatile int n;
    struct {
        int kind, slot;
        float pos[3];
    } e[8];
} g_exPend;                    // explode events from the engine hook
static struct {
    int kind, slot;
    float pos[3], feet[3];
    uint64_t frame, job[EX13_PROBES][EX13_ORIGINS],
        jobr[EX13_PROBES][EX13_ORIGINS], jaway[EX13_ORIGINS];
    float tf[EX13_PROBES][EX13_ORIGINS], org[EX13_ORIGINS][3];
    int phase;
    uint8_t shell[EX13_ORIGINS];
    float rlen[EX13_PROBES][EX13_ORIGINS];
    float pt[EX13_PROBES][3];
    int used;
} g_ex_q[4];

static int g_noFireHot;       // t2r-no-fire-hot.txt: burn only through the engine-fire oracle
static int Ex13Flag(const char *fn, int *cache, uint64_t *at)
{                               // t2r-*.txt next to the exe, re-read every 2 s
    uint64_t now = GetTickCount64();
    if (!*at || now - *at > 2000) {
        FILE *f = ExeSideOpen(fn, "r");
        *cache = f != NULL;
        if (f)
            fclose(f);
        *at = now;
    }
    return *cache;
}

static int Ex13NailDowns(void)
{
    static int v;
    static uint64_t at;
    return !Ex13Flag("t2r-nail-kills.txt", &v, &at);
}                               // a pipe bomb DOWNS (never kills a standing player); t2r-nail-kills.txt = the 2013 kill band

// our own throw leaves the hand (send_hook, message type 2): remember what it is so the engine's explode can be matched to it
static void Ex13NoteThrow(uint64_t sid)
{
    int kind =
        sid == 0xcf576d288d014eacULL ? EX13_NAIL : sid ==
        0x2a3164cb4a1c95d9ULL ? EX13_MOLO : (sid == 0x8502a9c43b674354ULL
                                             || sid ==
                                             0xa2aa922866d0479dULL) ?
        EX13_STUN : 0;
    if (!kind || g_oldExpl)
        return;
    uint64_t now = GetTickCount64();
    int k = 0;
    for (int i = 0; i < 6; i++) {
        if (g_ex_own[i].ms && now - g_ex_own[i].ms > 20000)
            g_ex_own[i].ms = 0;
        if (g_ex_own[i].ms < g_ex_own[k].ms)
            k = i;
    }
    g_ex_own[k].kind = kind;
    g_ex_own[k].ms = now;
}

static int Ex13OwnPop(int *kind)
{                               // the oldest unmatched own throw of the last 20 s
    uint64_t now = GetTickCount64();
    int k = -1;
    for (int i = 0; i < 6; i++) {
        if (!g_ex_own[i].ms)
            continue;
        if (now - g_ex_own[i].ms > 20000) {
            g_ex_own[i].ms = 0;
            continue;
        }
        if (k < 0 || g_ex_own[i].ms < g_ex_own[k].ms)
            k = i;
    }
    if (k < 0)
        return 0;
    *kind = g_ex_own[k].kind;
    g_ex_own[k].ms = 0;
    return 1;
}

static int Ex13OwnTake(int kind)
{                               // the oldest own throw of THIS kind (the explode's handle says what it is: a molotov bursts before an earlier nail bomb)
    uint64_t now = GetTickCount64();
    int k = -1;
    for (int i = 0; i < 6; i++) {
        if (!g_ex_own[i].ms || g_ex_own[i].kind != kind)
            continue;
        if (now - g_ex_own[i].ms > 20000) {
            g_ex_own[i].ms = 0;
            continue;
        }
        if (k < 0 || g_ex_own[i].ms < g_ex_own[k].ms)
            k = i;
    }
    if (k < 0)
        return 0;
    g_ex_own[k].ms = 0;
    return 1;
}

// called from the engine's explode hook (cheap: no script calls in there, ex13_tick does the work next frame)
static void Ex13Pend(int kind, int slot, const float *pos)
{
    if (g_oldExpl || !pos || !__builtin_isfinite(pos[0] + pos[1] + pos[2])
        || (kind != EX13_NAIL && kind != EX13_MOLO && kind != EX13_PBOMB
            && kind != EX13_LAUNCH))
        return;
    PbBlastNear(kind, slot, pos);     // an enemy blast / fire next to a placed bomb sets it off (placedbomb2013.c)
    int n = g_exPend.n;
    if (n >= 8)
        return;
    g_exPend.e[n].kind = kind;
    g_exPend.e[n].slot = slot;
    memcpy(g_exPend.e[n].pos, pos, 12);
    g_exPend.n = n + 1;
}

static float Ex13Scale(int slot, int molotov)
{                               // Explosion Expert of the THROWER scales every ring radius
    static const float B[3] = { 1.0f, 1.2f, 1.4f }, M[3] =
        { 1.0f, 1.1f, 1.2f };
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    // (seen as "killed by YOURSELF - MOLOTOV" with Explosion Expert II): the skill makes the thrower's blasts and fires bigger for the
    // ENEMY, not for the thrower himself - our own explosive is always judged against our body with the base rings
    if ((uint32_t) slot == self)
        return 1.0f;
    int lv =
        (uint32_t) slot == self ? Skl(SK_EXPLO) : SkPeer((uint32_t) slot,
                                                          SK_EXPLO);
    if (lv < 0)
        lv = 0;
    if (lv > 2)
        lv = 2;
    return molotov ? M[lv] : B[lv];
}

// the player ring of explosions.bin: kill R0, R1 (mult m1), R2 (mult m2), R3 flinch.
// R3 = PS4 Remastered v1.11 values (bin/dc1/explosions.bin, the final Factions): the reaction-only band beyond R2 was cut from the PS3 8 / 6 / 9 m to R2 (5 / 4.5 / 6 m), i.e. no flinch-only band any more (re/factions2013/ps4/logic/utilities.md)
static const struct {
    float kill, r1, r2, r3, m1, m2;
} EX13_RING[6] =
    { { 0 }, { 2.0f, 3.5f, 5.0f, 5.0f, 0.6f, 0.25f }, { 1.6f, 3.0f, 4.5f,
                                                       4.5f, 0.5f, 0.25f },
    { 0 },
{ 3.0f, 4.5f, 6.0f, 6.0f, 0.6f, 0.2f }, // [4] = *bomb-explosion-online* PLAYER ring (explosions.bin 0x182c): kill 3 / 4.5 x0.6 / 6 x0.2 / flinch 9
{ 0.0f, 2.5f, 6.0f, 8.0f, 1.2f, 0.55f }
};                              // group-getter - inside 2.5 m one hit DOWNS (x1.2 so partly hidden still downs; never kills: no kill band), 6 m x0.55, flinch 8 m. Was the 2013 ring below:  // [5] = the 2013 Launcher: TLOU2's own port `*gren-launcher-explosion-factions*` (explosions.bin 0x1120) PLAYER ring (+0x40, T1 slot 2):

                                               // kill 0 (no kill band) / R1 3.25 x0.55 / R2 5.0 x0.30 / flinch 5.0 - the only ring of the struct that differs from the nail bomb's
static const char *Ex13Name(int kind)
{
    return kind == EX13_MOLO ? "molotov" : kind ==
        EX13_PBOMB ? "placed nail bomb" : kind ==
        EX13_LAUNCH ? "explosive bow arrow" : "nail bomb";
}

// start the 6 probe rays for one explode event; a fire field is registered for every molotov burst
static void ImmBlastNote(int kind, const float *pos); // immersion.c: tinnitus for a close blast
// Our own position history (most throwables that should have hit land where the victim repositions themselves - a player ran past a
// pipe bomb as it burst: 2.34 m at the burst message, 60 dmg, no kill). The thrower saw our puppet one-way latency + the puppet smoothing
// delay in the past; the blast is judged at whichever is CLOSER to the burst: where we are now, or where we were then (rewound <= 250 ms).
static struct {
    uint64_t ms;
    float p[8];
} g_ex_hist[32];
static int g_exHistI;
static void Ex13HistTick(void)
{
    float lb[16];
    const float *l = PlayerLocator(lb);
    if (!l)
        return;
    int i = g_exHistI++ & 31;
    g_ex_hist[i].ms = GetTickCount64();
    memcpy(g_ex_hist[i].p, l, sizeof g_ex_hist[i].p);
}

static const float *Ex13Rewound(uint64_t back_ms)
{
    uint64_t want = GetTickCount64() - back_ms, best = ~0ULL;
    int bi = -1;
    for (int i = 0; i < 32; i++) {
        if (!g_ex_hist[i].ms)
            continue;
        uint64_t d =
            g_ex_hist[i].ms >
            want ? g_ex_hist[i].ms - want : want - g_ex_hist[i].ms;
        if (d < best) {
            best = d;
            bi = i;
        }
    }
    return bi >= 0 && best < 60 ? g_ex_hist[bi].p : NULL;
}

static float PsyncOneway(int slot);
// with a 390 ms rtt, two molotovs read 4 / 26 damage on the victim's screen:
// the thrower saw us at ONE moment (one-way + puppet delay ago), but that moment is a guess - the closest of all our positions over the whole
// window [now - (one-way + 100 ms, max 350 ms), now] is used for the distance (favour the thrower, as hitscan lag compensation does).
static const float *Ex13Closest(const float *pos, uint64_t back_ms,
                                 float *dist2)
{
    uint64_t now = GetTickCount64();
    const float *best = NULL;
    float bd = 1e30f;
    for (int i = 0; i < 32; i++) {
        if (!g_ex_hist[i].ms || g_ex_hist[i].ms + back_ms + 20 < now)
            continue;
        float dx = pos[0] - g_ex_hist[i].p[0], dz =
            pos[2] - g_ex_hist[i].p[2], d2 = dx * dx + dz * dz;
        if (d2 < bd) {
            bd = d2;
            best = g_ex_hist[i].p;
        }
    }
    if (dist2)
        *dist2 = bd;
    return best;
}

// (hits went through solid cover - stuns and pipe bombs: the rewound position was used for the RAYS too - a player who ducked behind cover
// in the last 250 ms was judged out in the open where the thrower last saw him. The rewind now only counts for DISTANCE (reach + ring: the
// closer of now / then); line of sight is always judged to where we ARE - cover we are behind protects.)
static float g_exShift[4][3];  // per queue entry: rewound position - current position (0 = no rewind)
static void Ex13Start(int kind, int slot, const float *pos)
{
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    const char *nm = Ex13Name(kind);
    if (kind == EX13_MOLO)
        MolotovBurstNote((uint32_t) slot == self ? -1 : slot, pos);   // the fire field (ex13_burn), even when the blast is out of range
    if (MatchSubstate() != 6 || R13Off(R13_EXPLO))
        return;
    ImmBlastNote(kind, pos);
    if ((uint32_t) slot != self && IsTeammate((uint32_t) slot)) {
        Xlog("explosive: %s of teammate slot %d - no friendly fire", nm,
             slot);
        TT_LOG_FRIENDLY(kind, slot);
        return;
    }
    float lb[16], shift[3] = { 0, 0, 0 };
    const float *pl = PlayerLocator(lb);
    if (!pl || LocalHp() <= 0.0f)
        return;
    if ((uint32_t) slot != self && slot >= 0 && slot < T2R_MAX_PEERS) { // lag fairness: where the thrower saw us, if that is closer (distance only)
        float ow = PsyncOneway(slot);
        if (!(ow >= 0.0f && ow < 400.0f))
            ow = 50.0f;
        uint64_t back = (uint64_t) (ow + 100.0f);
        if (back > 350)
            back = 350;
        float dr = 0;
        const float *rp = Ex13Closest(pos, back, &dr);
        if (rp) {
            float cx = pos[0] - pl[0], cz = pos[2] - pl[2];
            float dc = cx * cx + cz * cz;
            if (dr + 0.01f < dc && __builtin_fabsf(rp[1] - pl[1]) < 1.0f) {
                static int nl;
                if (LOGCAP(nl, 30))
                    Xlog("explosive: judged where slot %d saw us %llu ms ago (%.2f m from the burst, now %.2f m)", slot, (unsigned long long) back, __builtin_sqrtf(dr), __builtin_sqrtf(dc));
                shift[0] = rp[0] - pl[0];
                shift[2] = rp[2] - pl[2];
            }
        }
    }
    float s = Ex13Scale(slot, kind == EX13_MOLO);
    float dx = pos[0] - pl[0], dy = pos[1] - (pl[1] + 0.9f), dz =
        pos[2] - pl[2], d = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
    {
        float ex = dx - shift[0], ez = dz - shift[2], d2 =
            __builtin_sqrtf(ex * ex + dy * dy + ez * ez);
        if (d2 < d)
            d = d2;
    }
    float reach = EX13_RING[kind].r3 * s + 1.0f;
    Xlog("explosive: %s of slot %d%s burst at (%.2f %.2f %.2f), %.2f m from our body (ring scale %.1f, reach %.1f)", nm, slot, (uint32_t) slot == self ? " (OURS)" : "", pos[0], pos[1], pos[2], d, s, reach);
    TT_LOG_BURST(kind, slot, pos, pl, d, reach, s);
    if (d > reach)
        return;
    if (kind == EX13_PBOMB && (pos[1] - pl[1] <= -1.5f || pos[1] - pl[1] >= 1.8f)) {    // a placed mine on another floor (placedbomb2013.c PB_FLOOR_*)
        Xlog("explosive: %s of slot %d is on another floor (%.2f m vs our feet) - no damage", nm, slot, pos[1] - pl[1]);
        return;
    }
    int q = -1;
    for (int i = 0; i < 4; i++)
        if (!g_ex_q[i].used) {
            q = i;
            break;
        }
    if (q < 0) {
        Xlog("explosive: queue full - event dropped");
        return;
    }
    // body probes: head, chest, pelvis, two thighs (sideways), foot; a lying body is flat
    float h[EX13_PROBES] =
        { 1.60f, 1.25f, 0.95f, 0.60f, 0.60f, 0.15f }, side[EX13_PROBES] =
        { 0, 0, 0, 0.2f, -0.2f, 0 };
    float f = StunStanceScale();      // a crouched / prone body's probes sit at its real height (was standing height unless downed: blasts over low cover)
    float hx = -dz, hz = dx, hl = __builtin_sqrtf(hx * hx + hz * hz);
    if (hl < 0.001f) {
        hx = 1;
        hz = 0;
        hl = 1;
    }
    hx /= hl;
    hz /= hl;
    memset(&g_ex_q[q], 0, sizeof g_ex_q[q]);
    g_ex_q[q].used = 1;
    g_ex_q[q].kind = kind;
    g_ex_q[q].slot = slot;
    g_ex_q[q].frame = g_frame;
    memcpy(g_exShift[q], shift, sizeof shift);
    memcpy(g_ex_q[q].pos, pos, 12);
    memcpy(g_ex_q[q].feet, pl, 12);
    // a ground burst sits in the floor / against a curb, car body or prop: three origins (lifted 0.35, lifted 0.8, lifted 0.6 and 0.25 m toward the victim)
    float tl = __builtin_sqrtf(dx * dx + dz * dz);
    if (tl < 0.001f)
        tl = 1.0f;
    float tx = -dx / tl * 0.25f, tz = -dz / tl * 0.25f; // dx/dz = burst - body: the victim lies the other way
    // (pipe bombs did no damage: every nail bomb near a player read 0 of 6 with ALL rays blocked 0.1-0.4 m from their origins - the
    // thrower's burst point sits in / under the floor, a curb or the prop it rolled against, so origins only 0.35-0.8 m up still started inside
    // static geometry. Origins now go higher (0.5 / 1.0 / 1.4 m) and every ray starts 0.45 m along its way: geometry hugging the burst point no
    // longer counts as cover between the blast and the body.)
    // (throw test: with a 1.4 m origin a pipe bomb 2.3 m behind waist-high cover still reached 6 / 6 - blasts went over low
    // cover. Top origin back to 1.0 m. The origins at 0.5 m+ clear the floor / curb the burst lies on.)
    float O[EX13_ORIGINS][3] =
        { { pos[0], pos[1] + 0.5f, pos[2] }, { pos[0], pos[1] + 0.8f,
                                              pos[2] }, { pos[0] + tx,
                                                         pos[1] + 1.0f,
                                                         pos[2] + tz },
        { pos[0], pos[1] + 0.5f, pos[2] }, { pos[0], pos[1] + 0.5f,
                                            pos[2] } };
    // Molotovs did not work on enemies standing at the wall they were thrown against (victim-side logs of molotovs bursting on cover: origin 0 rays all hit at 0.2 m,
    // 'p1[0.2m/-/-] p2[0.2m/-/-]'): a molotov bursts ON the surface it hit, so origins right at the burst can sit inside that wall's collision, and a victim
    // standing along the wall is reached only by a ray running in the wall's plane. Two more origins of a MOLOTOV burst, 0.35 m to each side of the burst-victim
    // line (one of them is on the room side of a wall, whichever way it faces), 0.8 m up. Other kinds keep copies of origin 0 (same answer, no new behaviour).
    if (kind == EX13_MOLO) {
        float px = -tz, pz = tx, pl2 = __builtin_sqrtf(px * px + pz * pz);
        if (pl2 > 0.0001f) {
            px = px / pl2 * 0.35f;
            pz = pz / pl2 * 0.35f;
        }
        float o3[3] = { pos[0] + px, pos[1] + 0.8f, pos[2] + pz }, o4[3] =
            { pos[0] - px, pos[1] + 0.8f, pos[2] - pz };
        memcpy(O[3], o3, 12);
        memcpy(O[4], o4, 12);
    }
    for (int i = 0; i < EX13_PROBES; i++) {
        g_ex_q[q].pt[i][0] = pl[0] + hx * side[i];
        g_ex_q[q].pt[i][1] = pl[1] + h[i] * f;
        g_ex_q[q].pt[i][2] = pl[2] + hz * side[i];
        // (a trap 4.2 m away in the open: "0 of 6 probes reached"; every blast within ~5 m read 0 / 6, far ones 6 / 6): the ray
        // ended ON the probe inside our own body, so its last few cm met our own collision (t ~0.93 at 4 m < the 0.95 block threshold) - the
        // closer the blast, the surer the "block". The ray now stops 0.45 m short of the probe: any hit on it is real cover.
        for (int k = 0; k < (kind == EX13_MOLO ? EX13_ORIGINS : 3); k++) {      // pm81e: only a molotov has origins 3 / 4 (the others kept copies of origin 0: 12 more rays per victim for the same answer; the unused slots stay 0 = never read)
            // The ray starts AT the origin (blast test). The old 0.45 m start skip let a bomb lying 0.1-0.4 m behind a wall hit
            // through it (6 / 6 at every height). The "collision shell" seen in a real throw and in earlier logs (every ray hit at 0.4 / 0.4 /
            // 0.1 m) was reproduced at its exact burst point with synthetic blasts: a real wall 0.4 m from the burst toward the target,
            // not a shell - correct cover. Real open throws read 6 / 6 from the origins (test).
            const float *o = O[k];
            float rx = g_ex_q[q].pt[i][0] - o[0], ry =
                g_ex_q[q].pt[i][1] - o[1], rz =
                g_ex_q[q].pt[i][2] - o[2], rl =
                __builtin_sqrtf(rx * rx + ry * ry + rz * rz);
            float cut = rl > 0.9f ? 0.45f / rl : 0.5f;
            float e[3] =
                { g_ex_q[q].pt[i][0] - rx * cut,
           g_ex_q[q].pt[i][1] - ry * cut, g_ex_q[q].pt[i][2] - rz * cut };
            g_ex_q[q].rlen[i][k] = rl * (1.0f - cut);
            // (log 100243: "placed nail bomb of slot 3: 0 of 6 probes reached ... p0[0.7m/0.4m/0.0m]X p1[0.6m/0.6m/0.0m]X ..." - every ray of a mine lying in
            // Hillside tall grass hit 0.0-0.7 m from its origin, the blast did no damage): world_ray_kick's mask 0x207 stops on tall grass ~0.45 m above the ground,
            // and a mine blast starts at the grass. A placed mine's rays use 0x1b07 (passes grass / foliage, walls still block; characters other than us now block
            // too - ours is still ignored). Pipe bombs / molotovs / grenades keep 0x207.
            g_ex_q[q].job[i][k] =
                kind == EX13_PBOMB ? WorldRayKickM(o, e, 0,
                                                      0x1b07) :
                WorldRayKick(o, e);
            g_ex_q[q].jobr[i][k] = 0;
        }
    }
    memcpy(g_ex_q[q].org, O, sizeof O);
    g_ex_q[q].phase = 0;
}

// 2013 piece absorption for a blast (PS4 v1.11 FUN_0003b280): every armour piece with a joint within 5.0 m of the burst centre (explosion-settings +0x12c; all
// Factions explosions) takes part, NEAREST FIRST, each soaking min(its points, the damage still to go). A burst on the ground is nearest to the vest (chest), one
// above chest height to the helmet. Both pools are used (a blast right under you with armour on uses up BOTH pieces and you survive hurt).
// (Whether a piece's def flag +0x2c, which decides if its loss also reduces the damage, is set for the helmet could not be read from the data: assumed set.)
static float Ex13Armour(float dmg, float burst_y, float feet_y)
{
    if (!(dmg > 0.0f) || (g_vest <= 0.01f && g_helm <= 0.01f))
        return dmg;
    int helm_first = EX13_HELMET_ABSORBS && burst_y > feet_y + 1.45f;
    float hv[2] = { g_helm, g_vest }, was[2] = { g_helm, g_vest }, rem =
        dmg;
    int order[2] = { helm_first ? 0 : 1, helm_first ? 1 : 0 };
    for (int k = 0; k < 2; k++) {
        int pc = order[k];
        if (pc == 0 && !EX13_HELMET_ABSORBS)
            continue;
        float t = hv[pc] < rem ? hv[pc] : rem;
        hv[pc] -= t;
        rem -= t;
    }
    for (int pc = 0; pc < 2; pc++)
        if (hv[pc] < 0.01f)
            hv[pc] = 0.0f;
    ArmorSet(hv[0], hv[1]);
    for (int pc = 0; pc < 2; pc++)
        if (was[pc] > 0.0f && hv[pc] < was[pc]) {
            ArmorHudHit(pc);
            if (hv[pc] <= 0.0f && pc == 1)
                ArmorBrokeBanner();
            ArmorFxOn(T2R_US, pc, hv[pc] <= 0.0f);
        }
    ArmorBroadcast();
    Xlog("explosive: armour absorbed %.0f of %.0f (helmet %.0f -> %.0f, vest %.0f -> %.0f), %.0f goes through", dmg - rem, dmg, was[0], hv[0], was[1], hv[1], rem);
    return rem;
}

// The thrower never learned what its blast did (only the victims' logs had the numbers): the victim
// sends the result back, the thrower logs it ("explosive: our <kind> on slot N: ...").
#define MSG_BLASTRES 0x93       // peer msg (ours): [u8 kind][u8 probes reached][u8 best ring][u8 damage][u8 vest absorbed]
static void Ex13ResultSend(int kind, int slot, int reached, int best,
                             float dmg, float absorbed)
{
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    if ((uint32_t) slot == self || slot < 0 || slot >= T2R_MAX_PEERS
        || !g_sendTramp)
        return;
    uint8_t m[5] =
        { (uint8_t) kind, (uint8_t) reached, (uint8_t) best,
 (uint8_t) (dmg > 255 ? 255 : dmg < 0 ? 0 : dmg),
 (uint8_t) (absorbed > 255 ? 255 : absorbed < 0 ? 0 : absorbed) };
    g_sendTramp(MSG_BLASTRES, m, sizeof m, (uint8_t) slot);
}

static void Ex13ResultRecv(uint32_t from, const uint8_t *b, uint64_t len)
{
    if (!b || len < 5)
        return;
    static const char *RING[5] =
        { "out of reach", "flinch", "outer ring", "inner ring",
"KILL BAND" };
    Xlog("explosive: our %s on slot %u: %u of 6 probes reached, %s, %u damage%s (vest absorbed %u)", Ex13Name(b[0]), from, b[1], RING[b[2] < 5 ? b[2] : 0], b[3], b[2] == 4 ? " - downed / killed" : "", b[4]);
}

static void Ex13Deliver(int kind, int slot, int lethal, float dmg,
                         int type)
{
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    uint64_t w =
        kind == EX13_MOLO ? 0x2a3164cb4a1c95d9ULL : kind ==
        EX13_LAUNCH ? ExpbowWeaponSid() : SID_PIPE_BOMB_EXPLOSION;
    const char *nm = Ex13Name(kind);
    typedef uint64_t(*fn_me) (void);
    uint64_t me = ((fn_me) T2R_SYM(kFnLocalPlayer)) ();
    if (!me || LocalHp() <= 0.0f)
        return;
    g_lastEnemyHit = g_frame;
    g_lastEnemyHitMs = GetTickCount64();     // the fire oracle must not eat this drop
    g_lastHitSlot = (uint32_t) slot;
    if (lethal) {
        int downs = (kind == EX13_NAIL || kind == EX13_PBOMB || kind == EX13_LAUNCH) && (kind == EX13_LAUNCH || Ex13NailDowns()) && !g_downed;        // the launcher has no kill band: a share-of-health hit that would take the last hp DOWNS (the engine's knocked-down rule), never kills a standing player   // pipe bombs (thrown or placed) down, never kill outright, own ones too (t2r-nail-kills.txt = kill)
        if (downs) {
            // the pipe bombs' 200 marker is turned into a down by the explosion-id rule of recv_hit_hook; the launcher's hit carries a GUN weapon id, whose
            // would-kill rule ("dmg < 200 and >= hp - 1": cut to hp - 1, downed_set) needs the damage under 200: our current health (>= hp - 1)
            float dh =
                kind == EX13_LAUNCH ? (LocalHp() <
                                       199.0f ? LocalHp() : 199.0f) :
                200.0f;
            uint8_t hit[0x16] = { 0 };
            *(uint64_t *) hit = w;
            *(float *) (hit + 0x10) = dh;
            g_lastHitSlot = (uint32_t) slot;
            if ((uint32_t) slot == self)
                ((fn_recv_hit) T2R_SYM(kFnReceiveHit)) (me, me, w, 0, dh, 0);   // own bomb: no peer object for our own slot to inject from - the direct self-hit path (as the non-lethal branch)
            else {
                g_r13Inject = 1;
                Recv3Exec((uint64_t) slot, hit, sizeof hit);
                g_r13Inject = 0;
            }
            Xlog("explosive: %s of slot %d DOWNS us (kill band, a pipe bomb never kills outright)", nm, slot);
            return;
        }
        if ((uint32_t) slot == self) {
            TradeCredit(self, w, "own explosive");     // death card: YOURSELF + the weapon
            g_selfBomb = 1;
            ExtLethalHit(-1, w,
                           "explosive: own blast kill band - suicide");
            g_selfBomb = 0;
        } else if (downs) {
            uint8_t hit[0x16] = { 0 };
            *(uint64_t *) hit = w;
            *(float *) (hit + 0x10) = 200.0f;
            g_lastHitSlot = (uint32_t) slot;
            g_r13Inject = 1;
            Recv3Exec((uint64_t) slot, hit, sizeof hit);
            g_r13Inject = 0;
        } else {
            g_selfBomb = 1;
            ExtLethalHit(slot, w, "explosive: blast kill band");
            g_selfBomb = 0;    // g_self_bomb: no 'explosion downs' rule
        }
        Xlog("explosive: %s of slot %d KILLS us (result type %d)", nm,
             slot, type);
        return;
    }
    float before = LocalHp();
    if (kind == EX13_MOLO)
        HitfxSend(slot, 1);
    if (kind == EX13_MOLO)
        g_fx_diag_until = GetTickCount64() + 4000;      // 2013: the thrower's hit marker for a molotov that hurt us
    if ((uint32_t) slot == self) {
        g_selfBomb = 1;
        ((fn_recv_hit) T2R_SYM(kFnReceiveHit)) (me, me, w, 0, dmg, 0);
        g_selfBomb = 0;
    } else {
        uint8_t hit[0x16] = { 0 };
        *(uint64_t *) hit = w;
        *(float *) (hit + 0x10) = dmg;
        g_r13Inject = 1;
        Recv3Exec((uint64_t) slot, hit, sizeof hit);
        g_r13Inject = 0;
    }
    Xlog("explosive: %s of slot %d: %.0f damage delivered (hp %.1f -> %.1f)", nm, slot, dmg, before, LocalHp());
}

static int g_ex_dry, g_ex_dry_reached = -1, g_ex_shell_n;       // dev blast test: probes only, no damage (always 0 in release)
static void Ex13FinishCore(int q)
{
    g_ex_shell_n = 0;
    int kind = g_ex_q[q].kind, slot = g_ex_q[q].slot;
    float s = Ex13Scale(slot, kind == EX13_MOLO);
    const float *c = g_ex_q[q].pos;
    int reached = 0, best = 0;
    int nO = kind == EX13_MOLO ? EX13_ORIGINS : 3;      // origins that were really kicked
    char blog[EX13_PROBES * 40 + 8];
    int bl = 0;
    blog[0] = 0;                // (5 origins x '0.2m/' per probe: the old 24 B each cut the log after 3 probes)
    for (int i = 0; i < EX13_PROBES; i++) {
        int clear = 0;
        char one[40];
        int ol = 0;
        for (int k = 0; k < nO; k++) {
            float t = g_ex_q[q].tf[i][k];       // read on the first frame (ex13_shell_pass)
            int c1 = !(g_ex_q[q].job[i][k] && t >= 0.0f && t < 0.999f); // no ray / no answer = counts as reached (never a hidden block); the ray ends short of the body
            if (c1)
                clear = 1;
            if (ol < (int) sizeof one - 8)
                ol +=
                    snprintf(one + ol, SNLEFT(one, ol), c1 ? "-" : "%.1fm",
                             t * g_ex_q[q].rlen[i][k]);
            if (ol < (int) sizeof one - 2 && k < nO - 1)
                one[ol++] = '/';
        }
        one[ol] = 0;
        if (bl < (int) sizeof blog - 40)
            bl +=
                snprintf(blog + bl, SNLEFT(blog, bl), " p%d[%s]%s", i, one,
                         clear ? "" : "X");
        if (!clear)
            continue;
        reached++;
        float dx = c[0] - g_ex_q[q].pt[i][0], dy =
            c[1] - g_ex_q[q].pt[i][1], dz = c[2] - g_ex_q[q].pt[i][2];
        float d = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
        {
            float ex = dx - g_exShift[q][0], ez =
                dz - g_exShift[q][2], d2 =
                __builtin_sqrtf(ex * ex + dy * dy + ez * ez);
            if (d2 < d)
                d = d2;
        }                       // ring: the closer of now / where the thrower saw us
        int ty =
            d < EX13_RING[kind].kill * s ? 4 : d <=
            EX13_RING[kind].r1 * s ? 3 : d <=
            EX13_RING[kind].r2 * s ? 2 : d <=
            EX13_RING[kind].r3 * s ? 1 : 0;
        if (ty > best)
            best = ty;
    }
    float ratio = (float) reached / EX13_PROBES;
    if (best == 4) {
        ratio *= 2.0f;
        if (ratio > 1.0f)
            ratio = 1.0f;
    }                           // kill: reached / (0.5 * probes)
    TT_NOTE_PROBES(reached, best, ratio);
    Xlog("explosive: %s of slot %d: %d of %d probes reached, best ring type %d, ratio %.2f | rays (%d origins, hit distance in m or - clear, X = blocked from all):%s ", Ex13Name(kind), slot, reached, EX13_PROBES, best, ratio, nO, blog);
    if (g_ex_dry) {
        g_ex_dry_reached = reached;
        return;
    }
    if (best == 0 || ratio < EX13_MIN_RATIO) {
        if (best)
            Xlog("explosive: too little of us is visible (< %.2f) - no effect", EX13_MIN_RATIO);
        return;
    }
    if (best == 1)
        return;                 // flinch ring: reaction only, no damage
    float hp = LocalHp();
    if (!(hp > 0.0f))
        return;
    float base = kind == EX13_LAUNCH ? ExpbowDmgBase(hp) : hp;        // every other blast: a share of the target's CURRENT health (rules/molotov-nailbomb-2013.md section 3)
    float dmg =
        best == 4 ? hp : (best ==
                          3 ? EX13_RING[kind].m1 : EX13_RING[kind].m2) *
        base * ratio;
    dmg = __builtin_floorf(dmg);
    TT_NOTE_DMG(dmg);
    if (dmg < 1.0f)
        return;
    // 2013 (a blast right under you with armour on uses up both pieces and you survive hurt): the kill band is NOT armour-proof. Without any armour it is
    // the lethal hit as before (a vest of 50 must not turn a lethal 100 into 50 - that was a vest-only absorb of the HP-sized hit). With armour the kill band
    // hits as EX13_KILL_ARMOUR_DMG (the 2013 kill result is "full base, no ratio"; the base the engine passes is not readable, 200 = the lethal marker the mod always used),
    // both pieces soak nearest first and what is left hurts; only a hit that still takes all our health kills / downs.
    float d0 = dmg, armour0 = g_vest + g_helm;
    int lethal = best == 4;
    float burst_y = g_ex_q[q].pos[1], feet_y = g_ex_q[q].feet[1];
    if (best == 4 && armour0 > 0.01f) {
        dmg = EX13_KILL_ARMOUR_DMG;
        lethal = 0;
    }
    if (!lethal)
        dmg = Ex13Armour(dmg, burst_y, feet_y);
    Ex13ResultSend(kind, slot, reached, best, dmg,
                     armour0 - (g_vest + g_helm));
    (void) d0;
    if (dmg < 0.5f) {
        Xlog("explosive: everything absorbed by the armour - hit marker to slot %d", slot);
        HitfxSend(slot, kind == EX13_MOLO ? 1 : 3);    // 2013: an armour-absorbed hit still registers for the thrower (hit sent regardless of what passed)
        return;
    }
    Ex13Deliver(kind, slot, lethal || dmg >= hp - 0.5f, dmg, best);
}

static void Ex13Finish(int q)
{
#ifndef T2R_RELEASE
    float hp0 = LocalHp(), v0 = g_vest;
    int kind = g_ex_q[q].kind, slot = g_ex_q[q].slot;
    float pos[3], feet[3];
    memcpy(pos, g_ex_q[q].pos, 12);
    memcpy(feet, g_ex_q[q].feet, 12);
    TtResReset();
    Ex13FinishCore(q);
    TtLogResult(kind, slot, pos, feet, hp0, LocalHp(), v0, g_vest);
#else
    Ex13FinishCore(q);
#endif
}

// per frame: start events, resolve the rays of earlier frames
// first frame after the kick: read the origin rays (a ray answer is only valid on the next frame; finish_core uses the stored values)
static int Ex13ShellPass(int q)
{
    if (g_ex_q[q].phase)
        return 0;
    g_ex_q[q].phase = 1;
    for (int i = 0; i < EX13_PROBES; i++)
        for (int k = 0; k < EX13_ORIGINS; k++)
            g_ex_q[q].tf[i][k] =
                g_ex_q[q].job[i][k] ? RayT(g_ex_q[q].job[i][k]) : -2.0f;
    return 0;
}

static void Ex13Tick(void)
{
    Ex13HistTick();
    if (g_fxDumpN) {          // diag: the head of our own anim list while a molotov hurt us / burned us (armed in ex13_deliver / ex13_burn)
        static int nd;
        LONG m = g_fxDumpN;
        char b[48 * 9 + 8];
        int l = 0;
        for (LONG i = 0; i < m && i < 48; i++)
            l += snprintf(b + l, SNLEFT(b, l), "%08x ", g_fxDump[i]);
        if (LOGCAP(nd, 120))
            Xlog("firediag: own anim list (%ld dwords, %d ms left in the window): %s", (long) m, (int) ((int64_t) g_fx_diag_until - (int64_t) GetTickCount64()), b);
        g_fxDumpN = 0;
    }
    if (g_oldExpl) {
        g_exPend.n = 0;
        return;
    }
    if (MatchSubstate() != 6) {
        g_exPend.n = 0;
        memset(g_ex_q, 0, sizeof g_ex_q);
        memset(g_ex_own, 0, sizeof g_ex_own);
        return;
    }
    int n = g_exPend.n;
    g_exPend.n = 0;
    for (int i = 0; i < n && i < 8; i++)
        Ex13Start(g_exPend.e[i].kind, g_exPend.e[i].slot,
                   g_exPend.e[i].pos);
    for (int q = 0; q < 4; q++) {
        if (!g_ex_q[q].used || g_ex_q[q].frame == g_frame)
            continue;
        g_ex_q[q].used = 0;
        if (LocalHp() <= 0.0f) {
            Xlog("explosive: we died before the rays came back - hit not delivered");
            continue;
        }
        if (Ex13ShellPass(q)) {
            g_ex_q[q].used = 1;
            g_ex_q[q].frame = g_frame;
            continue;
        }
        Ex13Finish(q);
    }
}

// ---------------------------------------------------------------- fire (2013: 20 hp/s while a patch hurts us)
// The engine's own fire is the oracle that a patch really touches us (its hp drop with no hit message); the drop is replaced by
// 20 hp/s. No time-only burning, no static hit, no one-shots (those were the old suicide paths). A burst needs a clear line to
// our waist and our feet within EX13_FIRE_DY of its height. Downed body: the engine does nothing to it, burns on time inside the core.
static void Ex13Burn(void)
{
    static float last_hp = -1, total;
    static uint64_t last_tick_ms, burn_start_ms;
    static int ticks, src_slot = -2;
    if (g_noMolotov2013)
        return;
    uint64_t now = GetTickCount64();
    if (ticks && now - last_tick_ms > 1500) {
        Xlog("fire: burn ended - %d ticks, %.1f damage in %.2f s, hp %.1f",
             ticks, total, (last_tick_ms - burn_start_ms) / 1000.0f,
             LocalHp());
        TT_LOG_FIRE(ticks, total,
                    (last_tick_ms - burn_start_ms) / 1000.0f);
        ticks = 0;
        total = 0;
    }
    if (MatchSubstate() != 6) {
        last_hp = -1;
        return;
    }
    float hp = LocalHp();
    if (!(hp > 0)) {
        if (ticks)
            Xlog("fire: we died burning - %d ticks, %.1f damage", ticks,
                 total);
        ticks = 0;
        total = 0;
        last_tick_ms = 0;
        last_hp = -1;
        return;
    }
    float b[16];
    const float *pl = PlayerLocator(b);
    int src = -2, core = 0, hot = 0;
    static struct {
        uint64_t ms, job, job2, job3, job4, at;
        int clear;
    } L[MAX_BURSTS];
    for (int i = 0; pl && i < MAX_BURSTS; i++) {
        if (!g_bursts[i].ms
            || now - g_bursts[i].ms >
            (uint64_t) (MOLOTOV_FIRE_LIFE * 1000))
            continue;
        if (g_bursts[i].slot >= 0
            && IsTeammate((uint32_t) g_bursts[i].slot))
            continue;
        float sc =
            g_bursts[i].slot >= 0 ? Ex13Scale(g_bursts[i].slot,
                                               1) : Ex13Scale(*(uint32_t
                                                                 *)
                                                               T2R_SYM
                                                               (kMySlot),
                                                               1);
        float dx = g_bursts[i].at[0] - pl[0], dy =
            g_bursts[i].at[1] - pl[1], dz = g_bursts[i].at[2] - pl[2], h2 =
            dx * dx + dz * dz;
        if (h2 > EX13_FIRE_REACH * sc * EX13_FIRE_REACH * sc
            || dy < -EX13_FIRE_DY || dy > EX13_FIRE_DY_UP)
            continue;
        if (L[i].ms != g_bursts[i].ms) {
            memset(&L[i], 0, sizeof L[i]);
            L[i].ms = g_bursts[i].ms;
            L[i].clear = -1;
        }
        if (L[i].job && g_frame != L[i].at) {
            float t = RayT(L[i].job), t2 =
                L[i].job2 ? RayT(L[i].job2) : 0.0f, t3 =
                L[i].job3 ? RayT(L[i].job3) : 0.0f, t4 =
                L[i].job4 ? RayT(L[i].job4) : 0.0f;
            L[i].clear = !(t >= 0.0f && t < 0.95f) || (L[i].job2
                                                       && !(t2 >= 0.0f
                                                            && t2 < 0.95f))
                || (L[i].job3 && !(t3 >= 0.0f && t3 < 0.95f)) || (L[i].job4
                                                                  && !(t4
                                                                       >=
                                                                       0.0f
                                                                       &&
                                                                       t4 <
                                                                       0.95f));
            L[i].job = 0;
            L[i].job2 = 0;
            L[i].job3 = 0;
            L[i].job4 = 0;
        }                       // clear from EITHER origin (curbs / props at the burst must not shield a body the engine fire burns)
        if (!L[i].job && (L[i].clear < 0 || now - L[i].at > 300)) {
            float a[3] =
                { g_bursts[i].at[0], g_bursts[i].at[1] + 0.4f,
           g_bursts[i].at[2] }, w2[3] = { pl[0], pl[1] + 0.6f, pl[2] };
            L[i].job = WorldRayKick(a, w2); {
                float a2[3] = { a[0], a[1] + 0.6f, a[2] };
                L[i].job2 = WorldRayKick(a2, w2);
            }
            {                   // a burst on a wall sits in that wall's collision - two more origins 0.35 m to each side of the burst-body line (a clear one = the room side)
                float ux = w2[0] - a[0], uz = w2[2] - a[2], ul =
                    __builtin_sqrtf(ux * ux + uz * uz);
                if (ul > 0.05f) {
                    float px = -uz / ul * 0.35f, pz = ux / ul * 0.35f;
                    float a3[3] =
                        { a[0] + px, a[1] + 0.4f, a[2] + pz }, a4[3] =
                        { a[0] - px, a[1] + 0.4f, a[2] - pz };
                    L[i].job3 = WorldRayKick(a3, w2);
                    L[i].job4 = WorldRayKick(a4, w2);
                }
            }
            L[i].at = g_frame;
            if (!L[i].job && L[i].clear < 0)
                L[i].clear = 1;
        }
        if (L[i].clear != 1)
            continue;
        if (g_bursts[i].slot >= 0 || src == -2)
            src = g_bursts[i].slot;     // an enemy's fire wins (kill credit)
        if (h2 < EX13_FIRE_CORE * sc * EX13_FIRE_CORE * sc)
            core = 1;
        // a player walked around their own flames but still got hurt and then killed by themselves 7 s after it landed: the time-only hot core is for ENEMY fire
        // (their flames often never hurt through the oracle); our own fire burns us only when the engine's fire really touches us - flames you
        // can see, with the engine's own burn reaction and screams
        if (g_bursts[i].slot >= 0
            && h2 < EX13_FIRE_HOT * sc * EX13_FIRE_HOT * sc && dy > -1.0f
            && dy < 1.0f && now - g_bursts[i].ms < 7000
            && g_noFireHot == 0)
            hot = 1;            // 7 s: the flames die down before MOLOTOV_FIRE_LIFE
    }
    float prev = last_hp;
    last_hp = hp;
    if (src != -2)
        g_fire_diag_until = now + 6000; // diagnosis (spp_fire_diag): every sound request on our body is logged while a fire field is on us + 6 s
    if (src == -2 || prev < 0)
        return;
    float drop = prev - hp;
    int down_burn = g_downed && !g_downAt && !g_uc4DyingUntil
        && !FinisherGuardOn() && !(g_execUntil && g_t60 < g_execUntil);
    int guarded = (g_downed && !down_burn) || g_downAt
        || g_uc4DyingUntil || FinisherGuardOn();
    if (guarded)
        return;
    if (down_burn && core) {
        if (g_frame % 6)
            return;
        prev = hp;
        drop = 0;
    } else if (hot && !g_downed
               && (drop <= 0.01f || g_frame - g_lastEnemyHit <= 3)) {
        if (g_frame % 6)
            return;
        prev = hp;
        drop = 0;
    }                           // standing in the hot core: on time
    else if (drop <= 0.01f || g_frame - g_lastEnemyHit <= 3)
        return;
    float gap = last_tick_ms ? (now - last_tick_ms) / 1000.0f : 99.0f;
    if (gap > 1.0f) {
        burn_start_ms = now;
        ticks = 0;
        total = 0;
        gap = 0.1f;
        Xlog("fire: burning (%s), %.0f hp/s",
             src >= 0 ? "enemy fire" : "our own fire", EX13_FIRE_DPS);
        g_fx_diag_until = now + 4000;
    }
    if (gap > 0.5f)
        gap = 0.5f;
    last_tick_ms = now;
    src_slot = src;
    g_fire_tick_ms = now;
    if (src >= 0)
        g_fire_enemy_ms = now;
    float due = EX13_FIRE_DPS * gap;
    ticks++;
    total += due;
    if (src_slot >= 0) {
        TradeCredit((uint32_t) src_slot, 0x2a3164cb4a1c95d9ULL,
                     "molotov burn");
        HitfxSend(src_slot, 1);
    }
    float want = prev - due;
    if (want < 1.0f)
        want = 0.0f;
    Xlog("fire: tick %.2f (engine %.2f) total %.1f, hp %.1f -> %.1f (%s)",
         due, drop, total, prev, want > 0 ? want : 0.0f,
         src_slot >= 0 ? "enemy fire" : "own fire");
    if (want <= 0.0f) {
        ExtLethalHit(src_slot, 0x2a3164cb4a1c95d9ULL,
                       "fire: burned to death");
        last_hp = -1;
        return;
    }
    uint64_t a[2] = { SID_PLAYER, 0 };
    memcpy(&a[1], &want, sizeof want);
    DcCall(0xcac2cb5a30fccf12ULL /* set-object-health */ , 2, a);
    last_hp = want;
}

#ifndef T2R_RELEASE
// dev BLAST TEST (the throw harness could not land a bomb behind cover - bombs roll 4-5 m, its cover finder found no direction).
// t2r-blasttest.txt next to the VICTIM's exe (any content; "<slot>" = the attacker slot whose blast it is, default the first enemy): around our
// own feet, 16 directions x 2 / 3 / 4 m, a nail-bomb burst point on our foot height is run through the REAL probe code (ex13_start + finish, dry:
// no damage), plus 3 plain rays burst -> body at 0.3 / 1.0 / 1.6 m (low / waist / head: what stands between). One log line per point:
//   blasttest: dir D dist R: probes N/6 | between low X waist X head X  (X = hit distance m or - clear)
// Reading: nothing between -> 6/6 expected; waist-high cover (low+waist blocked, head clear) -> few probes; a full wall -> 0.
// Lines "x y z" = test spots: we are moved there (1.5 s settle) and the 48 points run around each in turn.
static struct {
    int on, i, slot, phase;
    uint64_t job[3];
    float pos[3];
    float base[3];
} g_bt;
static float g_btSpots[8][3];
static uint32_t g_btMask[8];
static int g_btBurst[8], g_bt_here[8];
static int g_bt_ns, g_bt_si;
static uint64_t g_btTpMs;
static uint32_t g_btKeepmask;
static void BlasttestTick(void)
{
    static uint64_t next;
    if (!g_bt.on) {
        uint64_t now = GetTickCount64();
        if (g_bt_si < g_bt_ns) {        // next queued spot: move there, settle, then start
            if (g_btBurst[g_bt_si]) {  // "b x y z [mask]": ONE burst at that point, we stay where we are
                static int ph;
                static uint32_t keep;
                if (ph == 0) {
                    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
                    int sl = -1;
                    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
                        uint8_t *pe = T2rPeer(s);
                        if (s != self && pe && pe[4] && !IsTeammate(s)) {
                            sl = (int) s;
                            break;
                        }
                    }
                    keep = g_worldRayMask;
                    if (g_btMask[g_bt_si])
                        g_worldRayMask = g_btMask[g_bt_si];
                    g_ex_dry = 1;
                    g_ex_dry_reached = -1;
                    Xlog("blasttest: burst %d at %.2f %.2f %.2f mask %x (slot %d)", g_bt_si + 1, g_btSpots[g_bt_si][0], g_btSpots[g_bt_si][1], g_btSpots[g_bt_si][2], g_worldRayMask, sl);
                    if (sl >= 0)
                        Ex13Start(EX13_NAIL, sl, g_btSpots[g_bt_si]);
                    ph = 1;
                    return;
                }
                if (++ph < 6)
                    return;
                Xlog("blasttest: burst %d -> probes %d/6", g_bt_si + 1,
                     g_ex_dry_reached);
                g_ex_dry = 0;
                g_worldRayMask = keep;
                ph = 0;
                g_bt_si++;
                return;
            }
            if (g_bt_here[g_bt_si]) {
                g_bt_si++;
                goto start;
            }
            if (!g_btTpMs) {
                float lb[16];
                const float *l = PlayerLocator(lb);
                if (!l)
                    return;
                float l8[8] =
                    { g_btSpots[g_bt_si][0], g_btSpots[g_bt_si][1],
              g_btSpots[g_bt_si][2], 1.0f, l[4], l[5], l[6], l[7] };
                TeleportPlayerTo(l8, l8 + 4); // (set-object-locator over 44 m launched the player)
                g_btTpMs = now;
                Xlog("blasttest: moved to spot %d (%.2f %.2f %.2f)",
                     g_bt_si + 1, l8[0], l8[1], l8[2]);
                return;
            }
            if (now - g_btTpMs < 1500)
                return;
            g_btTpMs = 0;
            g_bt_si++;
            goto start;
        }
        if (now < next)
            return;
        next = now + 2000;
        FILE *f = ExeSideOpen("t2r-blasttest.txt", "r");
        if (!f)
            return;
        g_bt_ns = g_bt_si = 0;
        g_btTpMs = 0;
        {
            char ln[128];
            while (g_bt_ns < 8 && fgets(ln, sizeof ln, f)) {
                float x, y, z;
                unsigned m = 0;
                int isb = ln[0] == 'b';
                if (!strncmp(ln, "here", 4)) {
                    g_bt_here[g_bt_ns] = 1;
                    g_btBurst[g_bt_ns] = 0;
                    g_btMask[g_bt_ns] =
                        sscanf(ln + 4, "%x", &m) == 1 ? m : 0;
                    g_bt_ns++;
                    continue;
                }               // "here [mask]": the 48 points around where we stand, no move
                int n = sscanf(ln + isb, "%f %f %f %x", &x, &y, &z, &m);
                if (n >= 3) {
                    g_bt_here[g_bt_ns] = 0;
                    g_btBurst[g_bt_ns] = isb;
                    g_btSpots[g_bt_ns][0] = x;
                    g_btSpots[g_bt_ns][1] = y;
                    g_btSpots[g_bt_ns][2] = z;
                    g_btMask[g_bt_ns] = n == 4 ? m : 0;
                    g_bt_ns++;
                }
            }
        }
        fclose(f);
        char pa[MAX_PATH];
        GetModuleFileNameA(NULL, pa, MAX_PATH);
        char *sa = strrchr(pa, 92);
        if (sa) {
            strcpy(sa + 1, "t2r-blasttest.txt");
            remove(pa);
        }
        if (g_bt_ns)
            return;
      start:;
        int sl = -1;
        uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
        if (sl < 0)
            for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
                uint8_t *pe = T2rPeer(s);
                if (s != self && pe && pe[4] && !IsTeammate(s)) {
                    sl = (int) s;
                    break;
                }
            }
        float lb[16];
        const float *pl = PlayerLocator(lb);
        if (sl < 0 || !pl || MatchSubstate() != 6) {
            Xlog("blasttest: not started (enemy slot %d, in match %d)", sl,
                 MatchSubstate() == 6);
            return;
        }
        memset(&g_bt, 0, sizeof g_bt);
        g_bt.on = 1;
        g_bt.slot = sl;
        memcpy(g_bt.base, pl, 12);
        g_btKeepmask = g_worldRayMask;       // "x y z mask" (hex): collision layer mask for this spot's rays (foliage layer hunt)
        if (g_bt_si > 0 && g_btMask[g_bt_si - 1]) {
            g_worldRayMask = g_btMask[g_bt_si - 1];
            Xlog("blasttest: ray mask %x for this spot", g_worldRayMask);
        }
        Xlog("blasttest: start - 48 burst points around our feet (%.2f %.2f %.2f), blast of slot %d", pl[0], pl[1], pl[2], sl);
        return;
    }
    if (g_bt.i >= 48) {
        g_bt.on = 0;
        g_ex_dry = 0;
        g_worldRayMask = g_btKeepmask;
        Xlog("blasttest: done");
        return;
    }
    int d = g_bt.i % 16, r = g_bt.i / 16;
    float dist = 2.0f + (float) r;
    float a = (float) d * 6.2831853f / 16.0f;
    if (g_bt.phase == 0) {
        g_bt.pos[0] = g_bt.base[0] + __builtin_sinf(a) * dist;
        g_bt.pos[1] = g_bt.base[1] + 0.05f;
        g_bt.pos[2] = g_bt.base[2] + __builtin_cosf(a) * dist;
        static const float hh[3] = { 0.3f, 1.0f, 1.6f };
        for (int k = 0; k < 3; k++) {
            float o[3] =
                { g_bt.pos[0], g_bt.base[1] + hh[k], g_bt.pos[2] }, e[3] =
                { g_bt.base[0], g_bt.base[1] + hh[k], g_bt.base[2] };
            g_bt.job[k] = WorldRayKick(o, e);
        }
        g_ex_dry = 1;
        g_ex_dry_reached = -1;
        Ex13Start(EX13_NAIL, g_bt.slot, g_bt.pos);
        g_bt.phase = 1;
        return;
    }
    static char bw[3][16];
    if (g_bt.phase == 1)        // the plain rays: answered on the NEXT frame only (read later they are stale: run 1 read 0.0 for all)
        for (int k = 0; k < 3; k++) {
            float tt = RayT(g_bt.job[k]);
            if (g_bt.job[k] && tt >= 0.0f && tt < 0.999f)
                snprintf(bw[k], sizeof bw[k], "%.1f", tt * dist);
            else
                strcpy(bw[k], "-");
        }
    if (g_bt.phase < 4) {
        g_bt.phase++;
        return;
    }                           // ex13_tick resolves the probes on a later frame
    Xlog("blasttest: dir %3d dist %.0f: probes %d/6 | between low %s waist %s head %s", d * 360 / 16, dist, g_ex_dry_reached, bw[0], bw[1], bw[2]);
    g_bt.i++;
    g_bt.phase = 0;
}
#endif
