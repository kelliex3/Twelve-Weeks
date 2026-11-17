// interrogation.c - TLOU 2013 Factions INTERROGATION mode for t2r-ext.
// Included from t2r-ext.c just before t2r_tick (after skills.c), so it uses t2r-ext's statics.
//
// The server decides whether a match is an Interrogation match (daemon env T2R_MODE, tools/
// interrogation.py) and keeps the counters; this file only acts and reports:
//  - asks the server once a match goes live (op 0xDC sub 0); reply on=0 -> nothing else runs this
//    match (Supply Raid stays exactly as before);
//  - INTERROGATE (the 2013 move itself): in an Interrogation match the E special execution of a downed enemy IS an interrogation - t2r-ext's
//    execution machinery plays the 2013 interrogate-enter pair (MX_INTERROGATE: the holder shakes the victim 4.6 s, then the kill; 7.2 s clips) on both
//    bodies (paired.c / psync) whatever weapon is carried; a standing enemy shivved (stunned or from behind) plays the shiv interrogation (MX_INT_SHIV,
//    kill ~2.5 s). This file watches the running pair: at its kill frame the interrogation is booked (server sub 1); being downed / killed before it
//    denies it (+50 for the denier, MSG_INTERROGATE state 3). The voices are intvox.c. Switch: t2r-no-interrogation.txt.
//  - our deaths are reported (sub 3: kills count as intel too, KILLS_PER_INFO on the server);
//  - LOCKBOX: candidates per map/team in lockboxes.h (tools/interrogation_lockboxes.py), the
//    server picks the index. Our own lockbox is always marked; the enemy's once our team has
//    enough intel (rev<team>). Hold E at the enemy lockbox for `hold` s per stage, 3 stages
//    (sub 2), stage progress kept by the server;
//  - WIN: a team finishes stage 3 -> the server's reply has win=<team> -> every client calls
//    winmm's match end FUN_180062b00(team) (the host's copy sends op 0x12 as for a timer end);
//  - RESPAWN ON: Supply Raid's team lives (lobby object +0x2b84 / +0x2b88) are topped up by the
//    same amount for both teams, so nobody runs out and the difference (= kills) is kept for
//    winmm's own timer-end decision;
//  - HUD (from the marker draw hook): intel counter, lockbox world markers, hold progress.
// Switches: t2r-no-interrogation.txt = off whatever the server says.
#include "lockboxes.h"

#define MSG_INTERROGATE 0x60    // peer msg (ours): [u8 victim slot][u8 1 start / 2 done / 3 stopped by a denier][u8 killer slot]
static float g_intTotalS = 4.6f;      // seconds from the start of the running interrogation to its kill frame (MELEE2013 kill_s)
#define INT_SECS      g_intTotalS     // (interrogation_hud.c divides the running interrogation's ticks by it)
#define INT_DENY_PARTS 50       // killing / downing an interrogator mid-hold
#define LB_RANGE      1.7f      // m (horizontal) to the lockbox
#define LB_PARTS      250       // per lockbox stage: 2013 "Opening lockbox" (net-10 stat 0x1dbd030a), 250 x 3 stages
#define INT_UNLOCK_PARTS 30     // 2013 "Unlock Interrupted" (stat 0x2860ee8f): shooting an unlocker
// every Interrogation notification is the 2013 one: banners through notify.c (ntf_banner), feed lines through msg_hook (the toast)
static int NtfBanner(const char *text, float secs);    // notify.c
static float NtfTextSize(float px);   // notify.c: nametag-font size for a text line height in 1280 x 720 px
static float NtfW(const char *s, float size);  // notify.c
static uint64_t IntSafeEvt(const char *ev, int t);    // (below, after the lockbox props)
static uint32_t IntRnd(void);
#define LIVES_FLOOR   5         // top up team lives below this ...
#define LIVES_ADD     10        // ... by this, both teams

#include "interrogation.h"
static IntT g_int;             // last reply (game thread copy)
static int g_noInt;            // t2r-no-interrogation.txt
static volatile LONG g_int_busy, g_int_have;    // request in flight / reply waiting (1 ok, 2 failed)
static uint8_t g_intReq[64 + 1 + 2 + 1];
static int g_intReqLen;
static char g_intRep[512];
static struct {
    uint8_t sub, arg;
    uint16_t seq;
} g_int_q[16];                  // events waiting for the server's ack
static int g_intQn;
static uint16_t g_intSeq;
static uint64_t g_intVictimAt;        // frame we were interrogated (our death is not a kill report)
static uint64_t g_intLiveFrame;       // first in-game frame of this match
static int g_int_tries, g_int_ended_sent, g_int_rev_seen[2];
static uint64_t g_int_hold_frames, g_lb_hold_frames;    // hold progress in 1/60 s ticks of REAL time (hold_add), not rendered frames
static uint64_t g_int_hold_last, g_lb_hold_last;
// +1 on the first call of a hold, then the g_t60 ticks since the previous call (a hitch / alt-tab adds at most 12 = 0.2 s): the hold
// takes INT_SECS / the lockbox time at 30, 60 or 144 fps alike (it was one count per frame = 2.4x too fast at 144 fps)
static uint64_t HoldAdd(uint64_t *cnt, uint64_t *last)
{
    uint64_t d = *cnt ? (*last && g_t60 > *last ? g_t60 - *last : 0) : 1;
    if (d > 12)
        d = 12;
    *last = g_t60;
    return *cnt += d;
}

static int g_intTarget = -1;   // slot of the victim of the interrogation that runs now (interrogation.h)
static int g_intFail;          // requests in a row without a reply (reset by one): the poll / event retry spread out

static DWORD WINAPI IntWorker(LPVOID arg)
{
    (void) arg;
    HMODULE ws = LoadLibraryA("ws2_32.dll");
    T2rSin a;
    T2rNet net;
    LONG res = 2;
    if (ws && HmServer(&a) && T2rNetLoad(&net, ws)) {
        uint8_t frame[3 + sizeof g_intReq];
        int n = g_intReqLen + 1;
        frame[0] = (uint8_t) n;
        frame[1] = 0;
        frame[2] = 0xDC;
        memcpy(frame + 3, g_intReq, g_intReqLen);
        char rep[sizeof g_intRep];
        int got = T2rRpc(&net, &a, frame, n + 2, rep, (int) sizeof rep - 1, 2000, 2000, 2000, NULL);   // (the old plain connect() blocked ~21 s on a dead tunnel)
        int ln = got >= 3 ? (rep[0] & 0xff) + ((rep[1] & 0xff) << 8) : 0;
        if (ln >= 1 && got >= ln + 2 && (uint8_t) rep[2] == 0xDD
            && ln < (int) sizeof g_intRep) {
            memcpy(g_intRep, rep + 3, ln - 1);
            g_intRep[ln - 1] = 0;
            res = 1;
            SrvMarkAlive();
        }
    }
    InterlockedExchange(&g_int_have, res);
    InterlockedExchange(&g_int_busy, 0);
    return 0;
}

static int IntAsk(uint8_t sub, uint16_t seq, uint8_t arg)
{                               // 1 = request started
    static char tok[65];
    if (!tok[0] && !ClanToken(tok)) {
        tok[0] = 0;
        return 0;
    }
    if (g_int_have || InterlockedCompareExchange(&g_int_busy, 1, 0))
        return 0;
    memcpy(g_intReq, tok, 64);
    g_intReq[64] = sub;
    g_intReq[65] = (uint8_t) seq;
    g_intReq[66] = (uint8_t) (seq >> 8);
    g_intReq[67] = arg;
    g_intReqLen = 68;         // a poll carries our live unlock percent (arg) too (server h0 / h1; an old server ignores it)
    HANDLE h = CreateThread(NULL, 0, IntWorker, NULL, 0, NULL);
    if (!h) {
        InterlockedExchange(&g_int_busy, 0);
        return 0;
    }
    CloseHandle(h);
    return 1;
}

static void IntParse(const char *t, IntT *m)
{
    char k[16], v[32];
    int n;
    memset(m, 0, sizeof *m);
    m->win = -1;
    while (sscanf(t, " %15[^= ]=%31s%n", k, v, &n) == 2) {
        int x = atoi(v);
        if (!strcmp(k, "on"))
            m->on = x;
        else if (!strcmp(k, "map"))
            m->map = x;
        else if (!strcmp(k, "team"))
            m->team = x & 1;
        else if (!strcmp(k, "need"))
            m->need = x;
        else if (!strcmp(k, "kpi"))
            m->kpi = x;
        else if (!strcmp(k, "p0"))
            m->p[0] = x;
        else if (!strcmp(k, "p1"))
            m->p[1] = x;
        else if (!strcmp(k, "rev0"))
            m->rev[0] = x;
        else if (!strcmp(k, "rev1"))
            m->rev[1] = x;
        else if (!strcmp(k, "box0"))
            m->box[0] = x;
        else if (!strcmp(k, "box1"))
            m->box[1] = x;
        else if (!strcmp(k, "st0"))
            m->st[0] = x;
        else if (!strcmp(k, "st1"))
            m->st[1] = x;
        else if (!strcmp(k, "h0"))
            m->h[0] = x;
        else if (!strcmp(k, "h1"))
            m->h[1] = x;
        else if (!strcmp(k, "win"))
            m->win = x;
        else if (!strcmp(k, "ended"))
            m->ended = x;
        else if (!strcmp(k, "hold"))
            m->hold = (float) atof(v);
        else if (!strcmp(k, "sp"))
            m->sp = x;
        t += n;
    }
    m->ok = 1;
}

static void IntEvent(uint8_t sub, uint8_t arg)
{
    if (g_intQn == (int) (sizeof g_int_q / sizeof *g_int_q))
        return;
    if (!++g_intSeq)
        g_intSeq = 1;
    g_int_q[g_intQn].sub = sub;
    g_int_q[g_intQn].arg = arg;
    g_int_q[g_intQn].seq = g_intSeq;
    g_intQn++;
}

static int IntLockbox(int team, float out[3])
{                               // a team's lockbox position (this match)
    if (g_int.map < 0 || g_int.map >= LB_MAPS)
        return 0;
    int n = LOCKBOXES[g_int.map][team & 1].n;
    if (n <= 0)
        return 0;
    memcpy(out,
           LOCKBOXES[g_int.map][team & 1].pos[g_int.box[team & 1] % n],
           12);
    return 1;
}

static int IntLockboxYaw(int team, float *yaw)
{                               // the crate's lid side (wall normal) when the table has one (navmesh-planned maps), else 0
    if (g_int.map < 0 || g_int.map >= LB_MAPS)
        return 0;
    int n = LOCKBOXES[g_int.map][team & 1].n;
    if (n <= 0)
        return 0;
    float y = LOCKBOXES[g_int.map][team & 1].yaw[g_int.box[team & 1] % n];
    if (y > 100.0f)
        return 0;
    *yaw = y;
    return 1;
}

#define LB_SPIN_N 32256         // samples of the 2013 safe-lock-spin (mono s16, 48 kHz, 0.67 s)
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl lb_spin_blob\nlb_spin_blob:\n.incbin \"safe_lock_spin.pcm\"\n.globl lb_spin_blob_end\nlb_spin_blob_end:\n.text\n");
extern const uint8_t lb_spin_blob[];
static uint64_t g_lbObj[2];    // spawned lockbox props (teams 0 / 1), 0 = none
static uint64_t g_lbBlk[2][2]; // the invisible collision blocks under the safe
// safes were still walk-through: spawn-object type SimpleAttachable makes no Havok body; only the INVISIBLE blocks use this (visual props stay SimpleAttachable for their animation). SP itself places invisible-block-* / invis-collision as AutoSpawn SimpleObject
// (pak spawner type field), so the type arg is tried as SimpleObject first (logged: type + return), SimpleAttachable as the fallback.
static uint64_t LbSpawnSolid(const char *art, const char *what)
{
    static const char *const TY[2] =
        { "SimpleObject", "SimpleAttachable" };
    static int lg;
    uint64_t o = 0;
    for (int i = 0; i < 2; i++) {
        uint64_t a[9] = { FNV64(art), FNV64(TY[i]), 0, 0, 1, 0, 0, 0, 0 };
        o = DcCall(FNV64("spawn-object"), 9, a);
        if (lg < 8) {
            lg++;
            Xlog("lockbox prop: spawn-object %s (%s) as %s -> %llx", art,
                 what, TY[i], (unsigned long long) o);
        }
        if (o && o != ~0ULL)
            return o;
    }
    return 0;
}

static float g_lbPosePos[2][3], g_lb_pose_yaw[2];     // where a player stands to work the safe (SP clip apReference offset) and which way it faces
static int g_lbSafe[2];        // 1 = the lockbox prop is the systemic-safe actor (open clip door-anim), 0 = the NR military crate (open)
static int g_lbOpened[2];      // the crate's open animation was played

// ---------------------------------------------------------------- safe-dial pose: hold E at the enemy safe = SP's "Ellie enters a safe combination"
// The player is smoothed (0.3 s, position + yaw, no snap) to the spot in front of the safe (g_lb_pose_*), then plays SP's ellie-systemic-safe-idle (the
// hand-on-dial crouch; actor97/systemic-safe.pak, already in the MP level through the systemic-safe actor) for as long as E is held. The clips' align is
// identity at their start, so animate_now from the placed spot is exact (no animate_ap). Replicated to the other screens by winmm's AnimCmdList capture of our
// own animate_ calls (like every other player clip); the safe's door/dial are animated by int_lockbox_open / dial-anim. Ellie's clips: a body whose rig does
// not resolve them (anim_len 0) keeps the old behaviour (plain hold). Ends on release, out of range, damage, down/death, an execution, or safe removal.
#define LB_POSE_CLIP "ellie-systemic-safe-idle"
#define LB_POSE_OPEN "ellie-systemic-safe-open-success"
#define LB_POSE_SMOOTH_MS 300
// phase 0 smoothing onto the spot, 1 the dial idle (E held), 2 the open-success clip (stage 3): it plays out whatever E does and blends into the engine crouch
// After opening the safe the character instantly went to a standing pose; it should morph into the crouched pose and stay
// like that: the end of the pose stopped "ellie-systemic-safe-idle" by name while the OPEN clip was the one playing (released E = the pose ended 2 frames after stage 3, the
// stop missed the playing clip, which ran out and cut to standing locomotion). Now the playing clip is the one stopped, with an SP-length blend (LB_OPEN_BLEND), and the
// stance under it is the engine crouch (player-force-crouch every tick from the open on and LB_CROUCH_HOLD_MS after the blend, the pull-out rise's recipe, melee_pullout.c):
// the blend lands in the crouch idle and the player stays crouched (no force-stand; the player stands up with their own crouch / sprint input).
#define LB_OPEN_BLEND 0.45f
#define LB_CROUCH_HOLD_MS 700
static struct {
    int on, team, phase;
    uint64_t t0, last_seen, idle_at, block_until, open_end;
    float from[3], from_yaw, hp0;
    const char *clip;
} g_lbp;
static uint64_t g_lbCrouchUntil;
static void LbClipOut(const char *anim, float bl)
{
    uint64_t a[5] = { SID_PLAYER, ANIM_LAYER, 0, FNV64(anim), 0 };
    memcpy(&a[2], &bl, 4);
    uint64_t r = DcCall(FNV64("stop-animating"), 5, a);
    Xlog("lockbox pose: %s blends out over %.2f s -> %llx", anim,
         (double) bl, (unsigned long long) r);
}

static void LbPoseEnd(const char *why)
{
    if (!g_lbp.on)
        return;
    int opening = g_lbp.phase == 2;
    if (g_lbp.phase >= 1 || g_lbp.clip)
        LbClipOut(g_lbp.clip ? g_lbp.clip : LB_POSE_CLIP,
                    opening ? LB_OPEN_BLEND : STUN_BLEND_OUT);
    if (opening && !g_downed && LocalHp() > 0.0f) {
        uint64_t none = 0;
        DcCall(FNV64("player-force-crouch"), 0, &none);
        g_lbCrouchUntil =
            GetTickCount64() + (uint64_t) (LB_OPEN_BLEND * 1000.0f) +
            LB_CROUCH_HOLD_MS;
    }
    Xlog("lockbox pose: ended (%s) after %.2f s%s", why,
         (double) (GetTickCount64() - g_lbp.t0) / 1000.0,
         opening ? " - into the crouch, the player stays crouched" : "");
    g_lbp.on = 0;
    g_lbp.phase = 0;
    g_lbp.clip = NULL;
}

static float LbAngdiff(float a, float b)
{
    float d = b - a;
    while (d > 3.14159265f)
        d -= 6.2831853f;
    while (d < -3.14159265f)
        d += 6.2831853f;
    return d;
}

// The move stick still drove the body during the hold, it drifted off the spot
// (> 0.4 m = 'walked off') or out of range and the hold ended. SP locks movement while the dial turns; letting go of E is the way out.
static void LbLockInput(void)
{
    uint64_t none = 0;
    DcCall(FNV64("player-disable-move-stick"), 0, &none);
    DcCall(FNV64("player-disable-jump"), 0, &none);
    DcCall(FNV64("player-disable-roll"), 0, &none);
    DcCall(FNV64("player-disable-unholstering"), 0, &none);
}

// one tick while E is held at team `enemy`'s safe; mp = our locator (pos, 1, quat). Returns nothing; starts / advances the pose.
static void LbPoseTick(int enemy, const float *mp)
{
    if (!g_lbObj[enemy] || !g_lbSafe[enemy] || g_gx.until > g_t60)
        return;
    uint64_t now = GetTickCount64();
    if (!g_lbp.on) {
        if (now < g_lbp.block_until)
            return;
        static int ok = -1;
        static uint64_t chk;
        if (ok != 1 && now - chk > 3000) {
            chk = now;
            ok = AnimLen(SID_PLAYER, LB_POSE_CLIP) > 0.5f;
            static int nl;
            if (LOGCAP(nl, 4))
                Xlog("lockbox pose: %s %s on this body", LB_POSE_CLIP,
                     ok ? "resolves" : "NOT loaded - plain hold");
        }
        if (ok != 1)
            return;
        g_lbp.on = 1;
        g_lbp.team = enemy;
        g_lbp.phase = 0;
        g_lbp.t0 = now;
        g_lbp.idle_at = 0;
        memcpy(g_lbp.from, mp, 12);
        g_lbp.from_yaw = 2.0f * __builtin_atan2f(mp[5], mp[7]);
        g_lbp.hp0 = LocalHp();
        // The glide would run with the gun in the hand (arm out, aim stance). SP's safe
        // empties the hands first: the weapon goes away instantly as the takedowns do (ctxdown.c), no unholster until the pose ends (lb_lock_input).
        {
            uint64_t z[2] = { 0, 0 }, n0 = 0, held =
                DcCall(FNV64("get-player-weapon-id"), 0, &n0);
            if (held && held != ~0ULL) {
                uint64_t r = (g_holsterInstFrame =
                              g_frame ? g_frame : 1,
                              DcCall(FNV64
                                      ("holster-player-weapon-instantly"),
                                      2, z));
                Xlog("lockbox pose: hands emptied for the safe (held %llx -> holster-player-weapon-instantly %llx)", (unsigned long long) held, (unsigned long long) r);
            }
        }
        // The dial idle clip started only after the 0.3 s glide (the body slid
        // in its locomotion pose). It starts with the glide now (the clip's root is static; the glide places the body every frame).
        {
            float ln = AnimLen(SID_PLAYER, LB_POSE_CLIP);
            uint64_t r = AnimateNow(SID_PLAYER, LB_POSE_CLIP);
            g_lbp.clip = LB_POSE_CLIP;
            g_lbp.idle_at =
                now + (uint64_t) ((ln > 0.5f ? ln : 2.9f) * 1000.0f) - 50;
            Xlog("lockbox pose: idle clip with the glide (%.2f s) -> %llx",
                 (double) ln, (unsigned long long) r);
        }
        Xlog("lockbox pose: start at team %d safe, from %.2f %.2f %.2f to %.2f %.2f %.2f yaw %.0f", enemy, (double) mp[0], (double) mp[1], (double) mp[2], (double) g_lbPosePos[enemy][0], (double) g_lbPosePos[enemy][1], (double) g_lbPosePos[enemy][2], (double) (g_lb_pose_yaw[enemy] * 57.29578f));
    }
    g_lbp.last_seen = g_frame;
    LbLockInput();
    if (g_lbp.phase == 2)
        return;                 // the open-success clip owns the body (lb_pose_watch ends it)
    // The character glided into position in an A pose and was then free to move for 2-3 seconds, even while holding E, before the unlocking anim snapped in.
    // The idle clip started in the same frame as holster-player-weapon-instantly, the holster reset the body's animation (bind pose in the
    // glide, then plain locomotion: free to walk) and the clip only came back at its 2.9 s re-issue. Every 100 ms the clip is checked (get-animation-playing?) and started
    // again at once when it is not running.
    {
        static uint64_t chk;
        if (now - chk >= 100) {
            chk = now;
            int pl = PbAnimPlaying(SID_PLAYER, LB_POSE_CLIP);
            if (pl == 0) {
                float ln = AnimLen(SID_PLAYER, LB_POSE_CLIP);
                uint64_t r = AnimateNow(SID_PLAYER, LB_POSE_CLIP);
                g_lbp.clip = LB_POSE_CLIP;
                g_lbp.idle_at =
                    now + (uint64_t) ((ln > 0.5f ? ln : 2.9f) * 1000.0f) -
                    50;
                static int nr;
                if (LOGCAP(nr, 12))
                    Xlog("lockbox pose: idle clip not playing %llu ms after the start (phase %d) - started again -> %llx", (unsigned long long) (now - g_lbp.t0), g_lbp.phase, (unsigned long long) r);
            }
        }
    }
    const float *to = g_lbPosePos[enemy];
    float ty = g_lb_pose_yaw[enemy];
    if (g_lbp.phase == 0) {     // smoothing: smoothstep over 0.3 s, position (xz; our own height) + shortest-arc yaw
        float k = (float) (now - g_lbp.t0) / (float) LB_POSE_SMOOTH_MS;
        if (k > 1.0f)
            k = 1.0f;
        float e = k * k * (3.0f - 2.0f * k);
        float pos[3] =
            { g_lbp.from[0] + (to[0] - g_lbp.from[0]) * e, mp[1],
     g_lbp.from[2] + (to[2] - g_lbp.from[2]) * e };
        float yw = g_lbp.from_yaw + LbAngdiff(g_lbp.from_yaw, ty) * e;
        float rot[4] =
            { 0, __builtin_sinf(yw * 0.5f), 0, __builtin_cosf(yw * 0.5f) };
        TeleportPlayerTo(pos, rot);
        if (k >= 1.0f)
            g_lbp.phase = 1;    // (the idle clip already runs: idle_at kept)
        return;
    }
    float dx = mp[0] - to[0], dz = mp[2] - to[2];
    if (dx * dx + dz * dz > 0.4f * 0.4f) {
        LbPoseEnd("walked off the spot");
        g_lbp.block_until = now + 1500;
        return;
    }
    float len = AnimLen(SID_PLAYER, LB_POSE_CLIP);
    if (now >= g_lbp.idle_at) {
        uint64_t r = AnimateNow(SID_PLAYER, LB_POSE_CLIP);
        g_lbp.clip = LB_POSE_CLIP;
        g_lbp.idle_at =
            now + (uint64_t) ((len > 0.5f ? len : 2.9f) * 1000.0f) - 50;
        static int nl;
        if (LOGCAP(nl, 6))
            Xlog("lockbox pose: idle clip (%.2f s) -> %llx", (double) len,
                 (unsigned long long) r);
    }
}

// called every tick of the lockbox logic: ends the pose when the hold went away
static void LbPoseWatch(int dead_or_down)
{
    uint64_t none = 0, now = GetTickCount64();
    if (!g_lbp.on) {
        if (g_lbCrouchUntil) {        // the open's blend-out lands in the crouch: held a moment so the stance latches, then left to the player
            if (now < g_lbCrouchUntil && !dead_or_down
                && LocalHp() > 0.0f)
                DcCall(FNV64("player-force-crouch"), 0, &none);
            else {
                g_lbCrouchUntil = 0;
                Xlog("lockbox pose: crouch hold over - the player stays crouched (no force-stand)");
            }
        }
        return;
    }
    if (dead_or_down) {
        LbPoseEnd("down / dead");
        g_lbp.block_until = now + 1500;
        g_lbCrouchUntil = 0;
        return;
    }
    if (g_lbp.phase == 2) {     // the open-success clip plays out (E released or not), the stance under it is the crouch
        LbLockInput();
        DcCall(FNV64("player-force-crouch"), 0, &none);
        if (LocalHp() < g_lbp.hp0 - 0.5f) {
            LbPoseEnd("damage while opening");
            g_lbp.block_until = now + 2000;
        } else if (now + (uint64_t) (LB_OPEN_BLEND * 1000.0f) >=
                   g_lbp.open_end)
            LbPoseEnd("safe opened");
        return;
    } else if (g_frame - g_lbp.last_seen > 2)
        LbPoseEnd("hold released / out of range");
    else if (LocalHp() < g_lbp.hp0 - 0.5f) {
        if (g_lastHitSlot < T2R_MAX_PEERS && g_sendTramp) {
            uint8_t b[3] =
                { (uint8_t) g_lbp.team, 5, (uint8_t) g_lastHitSlot };
            g_sendTramp(MSG_INTERROGATE, b, 3, 0xff);
        }                       // the shooter's "Unlock Interrupted"
        LbPoseEnd("damage");
        g_lbp.block_until = GetTickCount64() + 2000;
    }
}

// Safes should be actual safe models, so the one from No Return is used: No Return's assault lockbox is the military cache crate
// (ss-assault-lockbox -> int-mp-crate-military-1, actor rl-cache-crate-military, 1.31 x 0.49 x 0.49 m) - the
// same actor nrbox.c dresses the supply boxes with, in the MP level through dc_data_patches MP_LEVEL_ADDS. It stands at the lockbox spot facing the other
// team's lockbox, closed, and opens (NR clip `open`, 0.5 s) when its team's lockbox has been unlocked (stage 3). A failed spawn (actor not in the build)
// is logged once and the HUD markers alone mark the spot.
static void IntLockboxProps(int want)
{                               // spawn / remove the visible lockboxes
    if (!want) {
        LbPoseEnd("safe removed");
        for (int t = 0; t < 2; t++)
            if (g_lbObj[t]) {
                DcCall(FNV64("kill-entity"), 1, &g_lbObj[t]);
                g_lbObj[t] = 0;
            }
        for (int t = 0; t < 2; t++)
            for (int k = 0; k < 2; k++)
                if (g_lbBlk[t][k]) {
                    DcCall(FNV64("kill-entity"), 1, &g_lbBlk[t][k]);
                    g_lbBlk[t][k] = 0;
                }
        g_lbOpened[0] = g_lbOpened[1] = 0;
        return;
    }
    for (int t = 0; t < 2; t++) {
        float p[3], q[3];
        // Team t's lockbox spawns the moment the ENEMY (the team that must open it) completes its intel (5 interrogations), not at match start; the defenders see
        // it (and its DEFEND marker) at once, the attackers only get the marker/unlock once it is located (rev, the timer). Per team, from the server reply on every screen (joiners too).
        if (g_lbObj[t]
            || g_int.p[t ^ 1] < (g_int.need > 0 ? g_int.need : 5))
            continue;
        if (!IntLockbox(t, p))
            continue;
        // The safe spawned as a supply box: the real safe is SP's `systemic-safe` actor (sp-common; combination safe 0.85 x 1.0 x 0.94 m, skeleton joints
        // root / door / handle / dial / button, clips door-anim / dial-anim / button-anim / idle). Origin = the front-left-bottom corner, the front face at local +z, the body
        // behind it (z -0.9 .. +0.04). Spawned first (actor in the MP level through dc_data_patches MP_LEVEL_ADDS, T2R_INT_SAFE=0 leaves it out); a failed spawn falls back to
        // the NR military crate (the earlier prop). t2r-old-lockbox-crate.txt next to the exe = the crate only. g_lb_obj[t] stays the spawned object handle; its root joint is 'root'
        // for both props. g_lb_safe[t] = 1 when it is the safe (the open clip is then door-anim).
        int safe = 0;
        uint64_t o = 0;
        {
            static int oc = -1;
            if (oc < 0) {
                FILE *fo = ExeSideOpen("t2r-old-lockbox-crate.txt", "r");
                oc = fo != NULL;
                if (fo)
                    fclose(fo);
            }
            if (!oc) {
                uint64_t sa[9] =
                    { FNV64("systemic-safe"), FNV64("SimpleAttachable"), 0,
           0, 1, 0, 0, 0, 0 };
                o = DcCall(FNV64("spawn-object"), 9, sa);
                if (o && o != ~0ULL)
                    safe = 1;
                else {
                    o = 0;
                    static int nf0;
                    if (LOGCAP(nf0, 2))
                        Xlog("lockbox prop: spawn-object systemic-safe failed for team %d (actor not in the MP level?) - NR crate instead", t);
                }
            }
        }
        if (!o) {
            uint64_t a[9] =
                { FNV64("rl-cache-crate-military"),
        FNV64("SimpleAttachable"), 0, 0, 1, 0, 0, 0, 0 };
            o = DcCall(FNV64("spawn-object"), 9, a);
            if (!o || o == ~0ULL) {
                static int nf;
                if (LOGCAP(nf, 2))
                    Xlog("lockbox prop: spawn-object rl-cache-crate-military failed for team %d (actor not in the MP level?)", t);
                continue;
            }
        }
        g_lbSafe[t] = safe;
        float yaw = 0.0f;
        if (IntLockboxYaw(t, &yaw)) {
        }                       // planned flush against a wall: the lid side = the wall normal
        else if (IntLockbox(t ^ 1, q))
            yaw = __builtin_atan2f(q[0] - p[0], q[2] - p[2]);   // else the lid side faces the other team's lockbox
        static __attribute__((aligned(16))) float tp[2][4], tq[2][4];
        memcpy(tp[t], p, 12);
        tp[t][3] = 1.0f;
        if (safe) {             // the safe's origin is its front-left-bottom corner: put its back face where the crate's back was (crate centre + (-0.425, 0, +0.646) in the yaw frame)
            float sn = __builtin_sinf(yaw), cs = __builtin_cosf(yaw), lx =
                -0.425f, lz = 0.646f;
            tp[t][0] += lx * cs + lz * sn;
            tp[t][2] += -lx * sn + lz * cs;
        }
        {                       // the player is smoothed to the safe and plays SP's combination animation: the SP clips ellie-systemic-safe-* carry apReference = the safe's origin in the player's frame
            // (idle / dial / open-success: pos (0.46, 0, 0.688), rot 180 deg about Y; measured offline from actor97/systemic-safe.pak) -> the player stands at (0.46, 0.688) in the safe's
            // frame (front = +z), facing the safe (yaw + pi). The same rotation as the safe-origin shift above.
            float sn = __builtin_sinf(yaw), cs = __builtin_cosf(yaw), lx =
                0.46f, lz = 0.688f;
            g_lbPosePos[t][0] = tp[t][0] + lx * cs + lz * sn;
            g_lbPosePos[t][1] = tp[t][1];
            g_lbPosePos[t][2] = tp[t][2] - lx * sn + lz * cs;
            g_lb_pose_yaw[t] = yaw + 3.14159265f;
        }
        tq[t][0] = 0;
        tq[t][1] = __builtin_sinf(yaw * 0.5f);
        tq[t][2] = 0;
        tq[t][3] = __builtin_cosf(yaw * 0.5f);
        uint64_t bfa[5] = { (uint64_t) tp[t], (uint64_t) tq[t], 0, 0, 0 };
        uint64_t bf =
            DcCall(0x871ec52cc86b7427ULL /* alloc-bound-frame */ , 5,
                    bfa);
        uint64_t la[2] = { o, bf };
        uint64_t r = bf
            && bf != ~0ULL ? DcCall(FNV64("set-object-locator"), 2,
                                     la) : 0;
        g_lbObj[t] = o;
        // Safes had no collision: a spawn-object'd SimpleAttachable carries no Havok body (the crate was walked through too).
        // Two of the game's invisible collision blocks (invisible-block-050x150x100: 0.5 x 1.5 tall x 1 m, origin = bottom corner, sp-common; actor added in dc_data_patches
        // T2R_INT_SAFE) are spawned the same way and laid under the safe body (0.85 wide x 0.94 deep, centre 0.216 m in front of the crate centre), long axis along the safe's width.
        // EXPERIMENT: whether a spawned block is solid is not known (logs 'lockbox prop: collision'); if not, the blocks must be baked into the level pak like the map's invisible walls.
        if (safe) {
            float sn = __builtin_sinf(yaw), cs = __builtin_cosf(yaw);
            float ex[2] = { cs, -sn }, ez[2] = { sn, cs };      // the safe's width / front axes on the floor (x, z)
            for (int k = 0; k < 2; k++) {
                float zc = 0.216f + (k ? 0.25f : -0.25f);
                static
                    __attribute__((aligned(16))) float bp[2][2][4],
                    bq[2][2][4];
                bp[t][k][0] =
                    p[0] + ez[0] * zc + 0.25f * ez[0] - 0.5f * ex[0];
                bp[t][k][1] = p[1] - 0.5f;      // 1.5 m tall block sunk 0.5: top flush with the safe top (1.0 m)
                bp[t][k][2] =
                    p[2] + ez[1] * zc + 0.25f * ez[1] - 0.5f * ex[1];
                bp[t][k][3] = 1.0f;
                float hy = (yaw + 1.5707963f) * 0.5f;
                bq[t][k][0] = 0;
                bq[t][k][1] = __builtin_sinf(hy);
                bq[t][k][2] = 0;
                bq[t][k][3] = __builtin_cosf(hy);
                uint64_t bo =
                    LbSpawnSolid("invisible-block-050x150x100", "block");
                if (!bo || bo == ~0ULL) {
                    static int nb;
                    if (LOGCAP(nb, 2))
                        Xlog("lockbox prop: collision block spawn failed (actor not in the MP level?)");
                    continue;
                }
                uint64_t bfb[5] =
                    { (uint64_t) bp[t][k], (uint64_t) bq[t][k], 0, 0, 0 };
                uint64_t bbf = DcCall(0x871ec52cc86b7427ULL, 5, bfb);
                uint64_t lb2[2] = { bo, bbf };
                uint64_t rb = bbf
                    && bbf != ~0ULL ? DcCall(FNV64("set-object-locator"),
                                              2, lb2) : 0;
                g_lbBlk[t][k] = bo;
                {
                    static int bl;
                    if (bl++ < 4)
                        Xlog("lockbox prop: block bounds team %d #%d: corner %.2f %.2f %.2f, 0.5 x 1.5 x 1.0 (block frame), top y %.2f (safe top %.2f)", t, k, (double) bp[t][k][0], (double) bp[t][k][1], (double) bp[t][k][2], (double) (bp[t][k][1] + 1.5f), (double) (p[1] + 1.0f));
                }
                Xlog("lockbox prop: team %d collision block %d obj %llx (set-object-locator -> %llx)", t, k, (unsigned long long) bo, (unsigned long long) rb);
            }
        }
        Xlog("lockbox prop: team %d %s obj %llx at %.1f %.1f %.1f yaw %.0f (set-object-locator -> %llx)", t, safe ? "safe" : "NR crate", (unsigned long long) o, p[0], p[1], p[2], (double) (yaw * 57.29578f), (unsigned long long) r);
    }
}

static void IntLockboxOpen(int t)
{                               // team t's lockbox was unlocked: the crate opens (once)
    if (t < 0 || t > 1 || g_lbOpened[t] || !g_lbObj[t])
        return;
    g_lbOpened[t] = 1;
    // NR capture/siege safe (ss-rogue-tumbler-safe, spawner safe-res-rogue-1) opens with animate_(self, succ-anim-safe) = ellie-systemic-safe-open-success--safe (the -long variant
    // in the long version), blend 0.5; door-anim is the bare door channel, kept as the fallback when that clip is not resident.
    uint64_t r =
        AnimateNow(g_lbObj[t],
                    g_lbSafe[t] ? "ellie-systemic-safe-open-success--safe"
                    : "open");
    if (g_lbSafe[t] && (r == 0 || r == ~0ULL))
        r = AnimateNow(g_lbObj[t], "door-anim");
    Xlog("lockbox prop: team %d %s opens (animate_ -> %llx)", t,
         g_lbSafe[t] ? "safe (open-success--safe / door-anim)" : "crate",
         (unsigned long long) r);
    if (g_lbSafe[t])
        IntSafeEvt((IntRnd() & 1) ? "sfx-gbl-safe-open-success--safe-1" : "sfx-gbl-safe-open-success--safe-2", t);   // the safe's own open sound, at the final stage and at match victory
}

// Lockbox phase for a player of `team`: 1 = attacker (his team has all its intel, the enemy lockbox is unopened), 2 = defender (the enemy has the intel), 0 = no phase (neither / both
// complete, match decided, no boxes). target = the lockbox being opened, home = the attackers' own lockbox.
static int IntPhaseInfo(int team, float target[3], float home[3])
{
    if (g_noInt || !g_int.ok || !g_int.on || g_int.ended
        || g_int.win >= 0)
        return 0;
    int need = g_int.need > 0 ? g_int.need : 5, t = team & 1;
    int mine = g_int.p[t] >= need, theirs = g_int.p[t ^ 1] >= need;
    if (mine == theirs)
        return 0;
    int att = mine ? t : t ^ 1; // the attacking team
    if (g_int.st[att] >= 3)
        return 0;
    if (!IntLockbox(att ^ 1, target) || !IntLockbox(att, home))
        return 0;
    return mine ? 1 : 2;
}

static int IntNearLockbox(const float *p, float r, int team)
{                               // spawn picker: within r m (horizontal) of either lockbox; team = the team being spawned
    if (g_noInt || !g_int.ok || !g_int.on)
        return 0;
    for (int t = 0; t < 2; t++) {
        float lb[3];
        if (!IntLockbox(t, lb))
            continue;
        float dx = lb[0] - p[0], dz = lb[2] - p[2];
        // The enemy safe our team located keeps us 30 m away (else r)
        // (the spawned slot's team, not our own - the host picks for every slot)
        float rr = team >= 0 && team < 2 && t == (team ^ 1)
            && g_int.rev[team] && r < 30.0f ? 30.0f : r;
        if (dx * dx + dz * dz < rr * rr)
            return 1;
    }
    return 0;
}

// Once a team lockbox is discovered, that team only respawns in the respawns around the lockbox (not on the other side of the
// map), and enemies do not spawn in those spawns, basically separating the map. Once either safe has been located, the map splits in two halves
// by the two safes (a spawner belongs to the team whose safe is nearer): every team respawns only in its own half - the defenders around their found
// safe, the attackers never among them. The spawn picker skips the other half; when that leaves no spawner at all it picks again without the split
// (g_int_zone_relax, logged). t2r-int-zones-off.txt next to the exe = old rules.
static int g_intZoneRelax;
static int IntSpawnZoneActive(void)
{
    static int off = -1;
    if (off < 0)
        off =
            GetFileAttributesA("t2r-int-zones-off.txt") !=
            INVALID_FILE_ATTRIBUTES;
    return !off && !g_intZoneRelax && !g_noInt && g_int.ok && g_int.on
        && (g_int.rev[0] || g_int.rev[1]);
}

static int IntSpawnZoneBad(const float *p, int team)
{                               // 1 = the spawner lies in the other team's half
    if (team < 0 || team > 1 || !IntSpawnZoneActive())
        return 0;
    float a[3], b[3];
    if (!IntLockbox(team, a) || !IntLockbox(team ^ 1, b))
        return 0;
    float da =
        (a[0] - p[0]) * (a[0] - p[0]) + (a[2] - p[2]) * (a[2] - p[2]), db =
        (b[0] - p[0]) * (b[0] - p[0]) + (b[2] - p[2]) * (b[2] - p[2]);
    return db < da;
}

// the 2013 safe-lock-spin as a world sound at team t's safe (every screen: the unlocker sends state 4 with each spin, int_recv plays it for everyone), 6 m full / 35 m gone
static void LbSpinAt(int t)
{
    float lb[3];
    if (t < 0 || t > 1 || !IntLockbox(t, lb))
        return;
    lb[1] += 0.8f;
    PcmPlayAtRange((const int16_t *) lb_spin_blob, LB_SPIN_N, 0.6f,
                      48000, lb, 6.0f, 35.0f);
}

// the dial click used to be one repeated sample: the dial is the safe's own TLOU2 events (bank gbl-safe is in the MP level adds, dc_data_patches), picked at random per
// tick: mostly tick-normal (10 variants in the bank), now and then tick-clue, between them a cw / ccw rotate. 3D at the safe's dial joint on every screen (the unlocker sends state 4
// with each tick). An event that does not start falls back to the 2013 safe-lock-spin wave.
static uint64_t IntSafeEvt(const char *ev, int t)
{
    if (t < 0 || t > 1 || !g_lbObj[t])
        return 0;
    uint64_t a[5] =
        { FNV64(ev), g_lbObj[t], FNV64(g_lbSafe[t] ? "dial" : "root"), 0,
0 };
    uint64_t r = DcCall(FNV64("play-sound-at-joint"), 5, a);
    static int nl;
    if (LOGCAP(nl, 30))
        Xlog("lockbox sfx: %s at team %d safe -> %llx", ev, t,
             (unsigned long long) r);
    return r == ~0ULL ? 0 : r;
}

static void IntUiEvt(const char *ev)
{                               // a 2D UI event (ui-confirm-number, the 2013 lockbox stingers); not proof of audibility (mp-sound-events-silent)
    uint64_t a[2] = { FNV64(ev), 0 };
    uint64_t r = DcCall(FNV64("play-sound"), 2, a);
    static int nl;
    if (LOGCAP(nl, 30))
        Xlog("lockbox sfx: ui %s -> %llx", ev, (unsigned long long) r);
}

static uint32_t IntRnd(void)
{
    static uint32_t sd;
    if (!sd)
        sd = (uint32_t) GetTickCount64() * 2654435761u | 1u;
    sd = sd * 1664525u + 1013904223u;
    return sd >> 8;
}

static void LbDialSfx(int t)
{
    uint32_t r = IntRnd();
    unsigned k = r % 8u;
    const char *ev =
        k == 0 ? "sfx-gbl-safe-dial-rotate-tick-clue" : k <=
        4 ? "sfx-gbl-safe-dial-rotate-tick-normal" : (r >> 4) & 1 ?
        "sfx-gbl-safe-dial-rotate-cw" : "sfx-gbl-safe-dial-rotate-ccw";
    if (!IntSafeEvt(ev, t))
        LbSpinAt(t);
}

static int IntLive(void)
{
    return !g_noInt && g_int.ok && g_int.on && MatchSubstate() == 6;
}

// the other side of the interrogation: the pair itself kills (the execution machinery, both screens). This message is only the news: 1 = you are being
// interrogated, 2 = somebody finished an interrogation near you, 3 = [victim][3][killer] the interrogator was downed / killed before the kill frame
static uint64_t g_lbOtherMs[2];
static uint32_t g_lbOtherSlot[2];     // The last dial tick of another player at team t's safe (one unlocker at a time)
static void IntRecv(uint32_t from, const uint8_t *buf, uint64_t len)
{
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    if (!buf || len < 2 || from > T2R_MAX_PEERS - 1)
        return;
    if (buf[1] == 4) {
        if (buf[0] < 2 && IntLive() && from != me) {
            LbDialSfx(buf[0]);
            g_lbOtherMs[buf[0]] = GetTickCount64();
            g_lbOtherSlot[buf[0]] = from;
        }
        return;
    }                           // state 4 [lockbox team]: someone works that safe's dial
    if (buf[1] == 5) {          // [lockbox team][5][shooter slot]: an unlocker was hit - the shooter earns the 2013 "Unlock Interrupted" (+30)
        if (len >= 3 && buf[2] == me && !IsTeammate(from) && IntLive()) {
            Award(INT_UNLOCK_PARTS, me, "Unlock Interrupted");
            Xlog("interrogation: interrupted slot %u's unlock (+%d)", from,
                 INT_UNLOCK_PARTS);
        }
        return;
    }
    if (buf[1] == 3) {          // an interrogator was downed / killed; buf[2] = who did it
        if (len >= 3 && buf[2] == me && !IsTeammate(from) && IntLive()) {
            Award(INT_DENY_PARTS, me, "Interrogation Denied");  // 2013 label (net-10 stat 0x92d1de09); the award line is the whole notification
            Xlog("interrogation: stopped slot %u's interrogation (+%d)",
                 from, INT_DENY_PARTS);
        } else if (buf[0] == me && IntLive())
            MsgHook("The interrogator was stopped", 2.5f, 0, 0);
        return;
    }
    // (no "You are being interrogated!" notification for the victim; the "teammate / enemy interrogated" lines are gone, the 2013 "Enemy team has N interrogations" feed line replaces them)
}

static void IntSend(uint8_t victim, uint8_t state)
{
    uint8_t b[2] = { victim, state };
    if (g_sendTramp)
        g_sendTramp(MSG_INTERROGATE, b, 2, 0xff);
}

// the running interrogation pair of OUR half (attacker): 1 fists, 2 shiv, 0 none
static int IntPair(void)
{
    if (!g_gx.melee || g_gx.role != 1 || !g_gx.mname)
        return 0;
    if (!strncmp(g_gx.mname, "fist-interrogate", 16))
        return 1;
    if (!strncmp(g_gx.mname, "shiv-interrogate", 16))
        return 2;
    return 0;
}

static struct {
    uint64_t start;
    int slot, booked, denied;
} g_intRun;                    // the interrogation pair we run (g_gx.start identifies it)
// Press-to-interrogate: one press plays the whole pair. t2r-int-hold.txt next to the exe =
// the hold-to-continue test (the key / pad button must stay down; released for 0.25 s after the first 0.4 s = cancel).
static int IntReleased(void)
{
    static int tap = -1;
    static uint64_t rel_ms, run;
    if (tap < 0) {
        FILE *f = ExeSideOpen("t2r-int-hold.txt", "r");
        tap = f == NULL;
        if (f)
            fclose(f);
    }
    if (tap)
        return 0;
    if (run != g_gx.start) {
        run = g_gx.start;
        rel_ms = 0;
    }
    if (PsyncMs() - g_gx.t0 < 400.0 || !g_gx.t0)
        return 0;
    if (InteractDown()
        || (GameFocused() && (GetAsyncKeyState(0x45) & 0x8000))) {
        rel_ms = 0;
        return 0;
    }
    uint64_t now = GetTickCount64();
    if (!rel_ms)
        rel_ms = now;
    return now - rel_ms >= 250;
}

// ---------------------------------------------------------------- the 2013 notifications
// Everything below is derived from the server reply (g_int) on every client and fires once per change: banners (centre, gold) for the big moments, feed lines (the toast) for the
// rest. A change is only ever an increase (the optimistic local stage count can be one reply ahead of the server). Seeded from the first reply of the match (no replay).
static struct {
    int init, full[2], rev[2], st[2], p[2], act[2], sp, win;
    uint64_t open_at, warn_at;
} g_inn;
static int g_lbHolding;        // 1 = the unlock hold ran this frame (E held in range at the revealed enemy lockbox)
static uint64_t g_lbRelT60;
// letting go (or leaving range / being interrupted): the stage progress drains back at 6x the unlock rate (2013 lockbox update: unlock-time += dt * 6 against dt * 3 while unlocking),
// never below the start of the current stage - completed stages are kept by the server, the running stage only decays
static void LbRelease(void)
{
    g_lbHolding = 0;
    g_lb_hold_last = 0;
    uint64_t d = g_lbRelT60
        && g_t60 > g_lbRelT60 ? g_t60 - g_lbRelT60 : 0;
    if (d > 12)
        d = 12;
    g_lbRelT60 = g_t60;
    if (g_lb_hold_frames)
        g_lb_hold_frames =
            g_lb_hold_frames > 6 * d ? g_lb_hold_frames - 6 * d : 0;
}

// Our team holds all the intel it needs (the server's last reply; 0 without a reply)
// Lockbox phase for the supply-bottle drop rule: some team has all its intel, the match is not decided (works without the lockbox positions, which the
// defenders' client does not know before it is located - int_phase_info needs them)
static int IntLockboxPhase(void)
{
    if (g_noInt || !g_int.ok || !g_int.on || g_int.ended
        || g_int.win >= 0)
        return 0;
    int need = g_int.need > 0 ? g_int.need : 5;
    return g_int.p[0] >= need || g_int.p[1] >= need;
}

// 2013 NET_STAT_INT_ATTACKER_DOWN / INT_DEFENDER_KILL +100 (interrogation-supplies.md, mode lambda 0x3d30): the downer / executer is within 15 m of ITS OWN team lockbox.
// "[enemy] flagged as being at it" is not decoded [guess]: read as the lockbox phase with the actor's team DEFENDING (the enemy has all its intel, our box is not open).
// victim 0xFF = an execution (no victim needed). The position of any slot: our own player locator, a puppet's body locator.
static const float *KillBodyLocator(uint32_t victim);
static int IntDefenseBonus(uint32_t actor, uint32_t victim)
{
    if (!R13IntMode() || actor >= T2R_MAX_PEERS || MatchSubstate() != 6)
        return 0;
    if (victim < T2R_MAX_PEERS && SameTeam(actor, victim))
        return 0;
    int t = TeamOfSlot(actor), need = g_int.need > 0 ? g_int.need : 5;
    if (t < 0 || t > 1 || !IntLockboxPhase() || g_int.p[t] >= need
        || g_int.p[t ^ 1] < need || g_int.st[t ^ 1] >= 3)
        return 0;               // our team defends
    float lb[3], pb[16];
    const float *p =
        actor ==
        *(uint32_t *) T2R_SYM(kMySlot) ? PlayerLocator(pb) :
        KillBodyLocator(actor);
    if (!p || !IntLockbox(t, lb))
        return 0;
    float dx = p[0] - lb[0], dy = p[1] - lb[1], dz = p[2] - lb[2];
    return dx * dx + dy * dy + dz * dz <= 15.0f * 15.0f;
}

// 2013 end-of-match team bonus of the winning team (mode lambda 0x3d80): BLOWOUT_WIN 1000 (a team opened its box), CLEAR_VICTORY 600 (intel counters differ by >= 3),
// MINOR_WIN 300 (by exactly 2). 0 = none (a smaller difference pays nothing).
static int IntMatchBonus(int win, const char **label)
{
    if (!g_int.ok || !g_int.on || win < 0 || win > 1)
        return 0;
    int d = g_int.p[win] - g_int.p[win ^ 1];
    if (g_int.st[win] >= 3 || g_int.win == win) {
        *label = "Blowout Win";
        return 1000;
    }
    if (d >= 3) {
        *label = "Clear Victory";
        return 600;
    }
    if (d == 2) {
        *label = "Minor Win";
        return 300;
    }
    return 0;
}

static int IntOwnFull(void)
{
    int need = g_int.need > 0 ? g_int.need : 5, t = g_int.team;
    return (t == 0 || t == 1) && g_int.p[t] >= need;
}

static void IntNewsTick(int team, int enemy)
{
    IntT *g = &g_int;
    int need = g->need > 0 ? g->need : 5;
    int full[2] = { g->p[0] >= need, g->p[1] >= need };
    int act[2] = { g_lbHolding || g->h[team] > 0, g->h[enemy] > 0 };
    if (!g_inn.init) {
        memset(&g_inn, 0, sizeof g_inn);
        g_inn.init = 1;
        g_inn.full[0] = full[0];
        g_inn.full[1] = full[1];
        g_inn.rev[0] = g->rev[0];
        g_inn.rev[1] = g->rev[1];
        g_inn.st[0] = g->st[0];
        g_inn.st[1] = g->st[1];
        g_inn.p[0] = g->p[0];
        g_inn.p[1] = g->p[1];
        g_inn.sp = g->sp;
        g_inn.win = g->win >= 0;
        return;
    }
    uint64_t now = GetTickCount64();
    char t[96];
    if (full[team] && !g_inn.full[team])
        NtfBanner("All intel gathered! Locating lockbox...", 5.0f);
    if (full[enemy] && !g_inn.full[enemy])
        NtfBanner("Our lockbox is exposed!  Defend it!", 5.0f);
    if (g->p[enemy] > g_inn.p[enemy]) { // the other team got an interrogation: "Enemy team has N interrogation(s)" (2013 feed, 3 s)
        int n = g->p[enemy] < need ? g->p[enemy] : need;
        snprintf(t, sizeof t,
                 n ==
                 1 ? "Enemy team has %d interrogation" :
                 "Enemy team has %d interrogations", n);
        MsgHook(t, 3.0f, 0, 0);
    }
    if (g->rev[team] && !g_inn.rev[team]) {
        NtfBanner("Enemy team lockbox located!", 5.0f);
        IntUiEvt("sfx-ui-mp-interrogation-lockbox-pos");
    }
    if (act[0] && !g_inn.act[0] && g->rev[team] && g->st[team] < 3
        && now - g_inn.open_at > 20000) {
        g_inn.open_at = now;
        NtfBanner("We are opening the enemy lockbox!", 5.0f);
    }
    if (act[1] && !g_inn.act[1] && g->rev[enemy] && g->st[enemy] < 3
        && now - g_inn.warn_at > 20000) {
        g_inn.warn_at = now;
        NtfBanner("WARNING: Our lockbox is being opened by the enemy!",
                   3.0f);
        IntUiEvt("sfx-ui-mp-interrogation-lockbox-neg");
    }
    if (g->st[team] > g_inn.st[team]) {
        if (g->st[team] == 1)
            NtfBanner("We have the enemy lockbox partially open!", 3.0f);
        else if (g->st[team] == 2)
            NtfBanner("We have the enemy lockbox almost open!", 3.0f);
        if (g->st[team] < 3) {
            IntUiEvt("sfx-ui-mp-interrogation-lockbox-pos");
            IntUiEvt("sfx-gbl-safe-ui-confirm-number");
        }                       // stage complete: the safe's own confirm tick + the 2013 stinger
    }
    if (g->st[enemy] > g_inn.st[enemy] && g->st[enemy] == 2) {
        NtfBanner("WARNING: The enemy has almost opened our lockbox!",
                   3.0f);
        IntUiEvt("sfx-ui-mp-interrogation-lockbox-neg");
    }
    if (g->sp > g_inn.sp && g->rev[team] && g->st[team] < 3 && g->win < 0)
        MsgHook("Unlock speed increased!", 5.0f, 0, 0);
    if (g->win >= 0 && !g_inn.win && g->st[g->win] >= 3)
        NtfBanner(g->win ==
                   team ? "You Win - Enemy lockbox opened" :
                   "You Lose - Your team's lockbox was opened", 3.0f);
    if (full[0] > g_inn.full[0])
        g_inn.full[0] = full[0];
    if (full[1] > g_inn.full[1])
        g_inn.full[1] = full[1];
    for (int i = 0; i < 2; i++) {
        if (g->rev[i] > g_inn.rev[i])
            g_inn.rev[i] = g->rev[i];
        if (g->st[i] > g_inn.st[i])
            g_inn.st[i] = g->st[i];
        if (g->p[i] > g_inn.p[i])
            g_inn.p[i] = g->p[i];
        g_inn.act[i] = act[i];
    }
    if (g->sp > g_inn.sp)
        g_inn.sp = g->sp;
    if (g->win >= 0)
        g_inn.win = 1;
}

static void FeatureInterrogation(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    typedef void *(*fn_proc)(uint64_t);
    typedef const float *(*fn_loc)(void *, float *);
    static uint64_t next_poll, prompt_frame;
    static int me_dead;
    // The player still stood up (log: [93015] stage 3 + open-success clip, [93036] "team 0 stole the lockbox - match end", [93389] pose "ended (match left)"):
    // opening the last stage WINS the match, and every tick after the win returned (g_int_ended_sent) before lb_pose_watch - no force-crouch, no blend: the open clip ran out and
    // the engine stood the player up. The pose watch (open clip end -> blend into the crouch -> crouch hold) runs first, every tick, whatever the match state.
    lb_pose_watch(me_dead || g_downed || (g_lbp.on && LocalHp() <= 0.0f));
    if (g_int_have) {           // a reply (or a failure) came back
        LONG h = g_int_have;
        if (h == 1) {
            IntT m;
            IntParse(g_intRep, &m);
            if (g_intReq[64] && g_intQn) {    // the event at the queue head was booked
                memmove(&g_int_q[0], &g_int_q[1],
                        sizeof g_int_q[0] * (size_t) --g_intQn);
            }
            if (!g_int.ok || m.on != g_int.on)
                Xlog("interrogation: server says %s", g_intRep);
            // a match never changes its mode: an 'on=0' (record not found - daemon restarted, record not re-adopted yet) must not turn a running Interrogation into Supply Raid
            if (g_int.ok && g_int.on && !m.on)
                Xlog("interrogation: ignored on=0 (match is Interrogation)");
            else
                g_int = m;
            g_int_tries = 0;
            g_intFail = 0;
        } else {
            g_intFail++;
            // 'no reply' = old server only while the daemon answers something else (outage.h): a blip at the match start must not turn THIS client's
            // match into Supply Raid for good while everybody else plays Interrogation. Not answered: g_int stays !ok and the mode code idles.
            if (++g_int_tries >= 3 && !g_int.ok && SrvAliveWithin(60000)) {
                g_int.ok = 1;
                g_int.on = 0;   // old server / no reply: Supply Raid
                Xlog("interrogation: no reply from the server - off this match");
            }
        }
        InterlockedExchange(&g_int_have, 0);
    }
    if (MatchSubstate() != 6) {
        if (g_intLiveFrame) {
            g_intLiveFrame = 0;
            memset(&g_int, 0, sizeof g_int);
            g_intQn = 0;
            g_int_tries = 0;
            g_int_ended_sent = 0;
            g_int_rev_seen[0] = g_int_rev_seen[1] = 0;
            g_intTarget = -1;
            g_int_hold_frames = g_lb_hold_frames = 0;
            me_dead = 0;
            memset(&g_intRun, 0, sizeof g_intRun);
            memset(&g_inn, 0, sizeof g_inn);
            g_lbHolding = 0;
            g_lbRelT60 = 0;
            LbPoseEnd("match left");
            IntLockboxProps(0);
        }
        return;
    }
    if (g_noInt)
        return;
    if (!g_intLiveFrame) {
        g_intLiveFrame = g_t60;
        next_poll = g_frame;
    }
    // Live: the host's first poll came before the daemon had the Interrogation record (match formed 07:34:27,
    // record 07:35:13) -> 'on=0 mode=supply-raid', and this return stopped every later poll: the HOST played Supply Raid (no lives top-up = the
    // whole lobby ran out of reinforcements; E = a gun Special Execution instead of the interrogation). A Supply Raid answer now keeps a slow
    // poll (5 s) all match long, and a later on=1 switches this client to Interrogation.
    if (g_int.ok && !g_int.on) {
        if (g_frame >= next_poll && IntAsk(0, 0, 0))
            next_poll = g_frame + 300;
        return;
    }
    // the score banner's mode label (winmm's widget says SUPPLY RAID)
    if (g_int.ok && g_int.on && !(g_frame % 30))
        UiText("anim-group.scoreBanner.fader.teamPanel.modeLabel",
                "INTERROGATION");
    // the physical lockboxes (pickup-chest art): up shortly after the match goes live, removed when it is decided
    if (g_int.ok && g_int.on && !(g_frame % 30) && !g_int.ended
        && g_int.win < 0 && g_t60 - g_intLiveFrame > 120)
        IntLockboxProps(1);
    // the safe stays at match victory and opens (below, with its open clip + sound); the props go when the match is left (match_substate != 6 above)
    // a lockbox that its attackers got through (3 stages) opens; the crates stay until the match ends (the winners' camera shows the open safe)
    if (g_int.ok && g_int.on && !(g_frame % 30))
        for (int t = 0; t < 2; t++)
            if (g_int.st[t ^ 1] >= 3)
                IntLockboxOpen(t);
    // events go out at once (one request in flight, a failed one is retried 1/3 s), else a poll 1/s
    static uint64_t last_ask;
    if (g_frame >= next_poll
        || (g_intQn && g_int.ok
            && g_frame - last_ask >= 20 + 30u * (g_intFail >
                                                 4 ? 4 : g_intFail))) {
        float lbh = g_int.hold > 0.5f ? g_int.hold : 8.0f;
        unsigned pct = g_lb_hold_frames ? (unsigned) (100 * g_lb_hold_frames / (uint64_t) (lbh * 60.0f)) : 0;   // our live unlock progress of this stage
        int started =
            g_intQn ? IntAsk(g_int_q[0].sub, g_int_q[0].seq,
                               g_int_q[0].arg) : IntAsk(0, 0,
                                                         (uint8_t) (pct >
                                                                    99 ? 99
                                                                    :
                                                                    pct));
        // (1/3 s while somebody unlocks, so the marker rings fill live on both teams' screens; else 1/s)
        if (started) {
            next_poll =
                g_frame +
                ((g_lb_hold_frames || g_int.h[0]
                  || g_int.h[1]) ? 20 : 60) + 60 * (g_intFail >
                                                    5 ? 5 : g_intFail);
            last_ask = g_frame;
        }                       // (failing: polls spread out to 6 s)
    }
    if (!g_int.ok)
        return;
    int team = g_int.team, enemy = team ^ 1;
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);

    IntNewsTick(team, enemy); // the 2013 banners / feed lines (above the win return, so the win banner shows)
    // the match is decided: winmm's own match end on every client (host's copy sends 0x12)
    if (g_int.win >= 0 && !g_int_ended_sent) {
        g_int_ended_sent = 1;
        Xlog("interrogation: team %d stole the lockbox - match end",
             g_int.win);
        ((void (*)(int)) T2R_SYM(kFnMatchEnd)) (g_int.win);
        return;
    }
    if (g_int_ended_sent || g_int.ended)
        return;

    // respawn on: keep both teams' lives above the floor, same amount each (difference = kills)
    uint8_t *lo = LobbyObj();
    if (lo && !(g_frame % 30)) {
        int32_t *lv = (int32_t *) (lo + 0x2b84);
        if (lv[0] >= 0 && lv[1] >= 0 && lv[0] < 10000 && lv[1] < 10000
            && (lv[0] < LIVES_FLOOR || lv[1] < LIVES_FLOOR)) {
            lv[0] += LIVES_ADD;
            lv[1] += LIVES_ADD;
            Xlog("interrogation: team lives topped up -> %d / %d", lv[0],
                 lv[1]);
        }
    }

    // The clock ending an Interrogation match is decided by the match's PROGRESS, not by winmm's lives (= kills): lockbox stages,
    // then interrogations (capped at the 5 needed); equal = a DRAW (2013 Interrogation has no kill score). winmm's own timer end compares the two team
    // lives (equal = draw, else the higher wins), so in the last seconds both lives are written to give exactly that verdict. t2r-no-int-tiebreak.txt = old.
    if (lo && !g_int.ended && g_int.win < 0 && lo[0x2b94] && !lo[0x2ca4]) {
        float *tm = (float *) (lo + 0x2b8c);
        if (tm[0] > 0.0f && tm[1] <= 3.0f) {
            static int tb_off = -1;
            static uint64_t tb_chk;
            if (tb_off < 0 || GetTickCount64() - tb_chk > 3000) {
                tb_chk = GetTickCount64();
                FILE *f = ExeSideOpen("t2r-no-int-tiebreak.txt", "r");
                tb_off = f != NULL;
                if (f)
                    fclose(f);
            }
            if (!tb_off) {
                int32_t *lv = (int32_t *) (lo + 0x2b84);
                int need = g_int.need > 0 ? g_int.need : 5;
                int s0 = g_int.st[0], s1 = g_int.st[1], p0 =
                    g_int.p[0] > need ? need : g_int.p[0], p1 =
                    g_int.p[1] > need ? need : g_int.p[1];
                int w =
                    s0 != s1 ? (s0 > s1 ? 0 : 1) : p0 != p1 ? (p0 >
                                                               p1 ? 0 : 1)
                    : -1;
                int want0 = w < 0 ? 50 : w == 0 ? 51 : 50, want1 =
                    w < 0 ? 50 : w == 1 ? 51 : 50;
                if (lv[0] != want0 || lv[1] != want1) {
                    static int tb_n;
                    if (LOGCAP(tb_n, 6))
                        Xlog("interrogation: clock end (%.1f s left) - stages %d/%d, interrogations %d/%d -> %s; team lives %d / %d -> %d / %d", (double) tm[1], s0, s1, p0, p1, w < 0 ? "DRAW" : w ? "team 1 ahead" : "team 0 ahead", lv[0], lv[1], want0, want1);
                    lv[0] = want0;
                    lv[1] = want1;
                }
            }
        }
    }

    // our deaths count as enemy kills (not when the death was an interrogation)
    {                           // we are the victim of an interrogation pair (the execution machinery plays it): that death is the interrogation, not a kill report
        const char *vn = g_gx.role == 2 && g_gx.melee ? g_gx.mname : NULL;
        if (vn
            && (!strncmp(vn, "fist-interrogate", 16)
                || !strncmp(vn, "shiv-interrogate", 16)))
            g_intVictimAt = g_t60;
    }
    if (!(g_frame % 10)) {
        uint64_t ps = SID_PLAYER;
        uint64_t d =
            DcCall(0xe93f704f02a9ecd5ULL /* character-is-dead? */ , 1,
                    &ps);
        int dead = d != ~0ULL && (d & 0xff);
        if (dead && !me_dead
            && !(g_intVictimAt && g_t60 - g_intVictimAt < 600))
            IntEvent(3,
                      (uint8_t) (g_lastHitSlot <
                                 T2R_MAX_PEERS ? g_lastHitSlot : 0xff));
        me_dead = dead;
    }
    // the interrogation pair we run: booked at its kill frame, denied when we go down / die before it (the execution machinery aborts the pair; the victim lives)
    {
        int ip = IntPair();
        if (ip && g_gx.start != g_intRun.start) {      // a new interrogation starts
            memset(&g_intRun, 0, sizeof g_intRun);
            g_intRun.start = g_gx.start;
            g_intRun.slot = g_gx.slot;
            g_intTotalS = g_gx.kill_s > 0.5f ? g_gx.kill_s : 4.6f;
            g_intTarget = g_gx.slot;
            g_int_hold_frames = 1;
            g_int_hold_last = 0;
            IntSend((uint8_t) g_gx.slot, 1);
            Xlog("interrogation: %s on slot %d - kill frame %.2f s",
                 g_gx.mname, g_gx.slot, (double) g_intTotalS);
        }
        if (g_intRun.start && !g_intRun.booked && !g_intRun.denied) {
            if (me_dead || g_downed) {
                g_intRun.denied = 1;
                g_intTarget = -1;
                g_int_hold_frames = 0;
                MsgHook("Interrogation incomplete", 5.0f, 0, 0);       // 2013 feed line for the interrogator who was downed
                if (g_lastHitSlot < T2R_MAX_PEERS) {  // 3 = stopped by being downed / killed: [victim][3][killer]
                    uint8_t b[3] =
                        { (uint8_t) g_intRun.slot, 3,
                 (uint8_t) g_lastHitSlot };
                    if (g_sendTramp)
                        g_sendTramp(MSG_INTERROGATE, b, 3, 0xff);
                } else
                    IntSend((uint8_t) g_intRun.slot, 0);
                Xlog("interrogation: slot %d's interrogation was stopped - we %s before the kill frame", g_intRun.slot, me_dead ? "died" : "went down");
            } else if (!ip) {   // the pair ended on our side before its kill frame (victim gone / path / abort)
                g_intRun.denied = 1;
                g_intTarget = -1;
                g_int_hold_frames = 0;
                Xlog("interrogation: the pair on slot %d ended without a kill - not booked", g_intRun.slot);
            } else if (IntReleased()) {        // the interrogation is HOLD-to-continue; letting go stops it
                int sl = g_intRun.slot;
                if (ip == 2)
                    Xlog("shiv: kept (interrogation released) - nothing spent");
                GxAbort("interrogation released (hold lost)", 1);      // both halves blended out (psync abort -> victim client), vox / sfx timeline + cameras cut, victim stays downed
                g_intRun.denied = 1;
                g_intTarget = -1;
                g_int_hold_frames = 0;
                Xlog("interrogation: slot %d's interrogation was let go before the kill frame - cancelled, no reveal, victim released downed", sl);
            } else {
                g_int_hold_frames =
                    g_t60 > g_gx.start ? g_t60 - g_gx.start : 1;
                {               // the engine's own interactable prompt (button glyph follows the input device + the hold ring, filled clockwise) over the victim
                    float pg =
                        (float) g_int_hold_frames /
                        ((g_intTotalS >
                          0.5f ? g_intTotalS : (float) INT_SECS) *
                         60.0f);
                    static int ring = -1;
                    if (ring < 0)
                        ring = GetFileAttributesA("t2r-int-no-hold-ring.txt") == INVALID_FILE_ATTRIBUTES;       // the floating SP hold prompt "Interrogating" is kept (only the LET GO band was to go); opt-out file
                    if (ring)
                        WorldPrompt(g_gx.h, "Interrogating", pg < 0.02f ? 0.02f : pg > 0.99f ? 0.99f : pg);    // the "E Interrogate" prompt stayed on screen the whole interrogation - hidden while paired (hold still works); t2r-int-hold-ring.txt = the ring back
                }
                if (g_t60 >= g_gx.kill_at + 6) {        // the kill frame passed with us alive and up: the victim is dead
                    g_intRun.booked = 1;
                    g_intTarget = -1;
                    g_int_hold_frames = 0;
                    IntSend((uint8_t) g_intRun.slot, 2);
                    IntEvent(1, (uint8_t) g_intRun.slot);
                    Xlog("interrogation: slot %d interrogated (%s)",
                         g_intRun.slot, g_gx.mname);
                }
            }
        }
    }
    // (lb_pose_watch runs at the top of this function now: night57 the stage-3 open won the match 0.35 s later and the win return below skipped it)
    if (me_dead) {
        LbRelease();
        return;
    }
    if (g_downed) {
        LbRelease();
        return;
    }

    uint8_t *mep = ((uint8_t * (*)(void)) T2R_SYM(kFnLocalPlayer)) ();
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    float mb[16];
    const float *mp =
        mep ? ((fn_loc) (exe + 0x133db30)) (mep + 0x100, mb) : NULL;
    if (!mp)
        return;
    int held = InteractDown() || (GameFocused() && (GetAsyncKeyState(0x45) & 0x8000));        // the engine interact (pad latch; only live with focus) or the raw E key (focus checked)
    if (g_gx.until && g_t60 < g_gx.until) {
        LbRelease();
        return;
    }                           // an execution / interrogation runs: no lockbox hold in the middle of it

    // steal: at the enemy lockbox, once revealed
    float lb[3];
    // A teammate finishing a stage resets OUR progress too. Before, every holder kept its own count across the
    // team's stage change, so two players holding from the start opened 2 stages in one stage's time, three players all 3 (2013: no speed-up from extra unlockers)
    {
        static int seen_st = -1;
        if (g_int.st[team] != seen_st) {
            g_lb_hold_frames = 0;
            g_lb_hold_last = 0;
            seen_st = g_int.st[team];
        }
    }
    if (!g_int.rev[team] || g_int.st[team] >= 3 || !IntLockbox(enemy, lb)) {
        LbRelease();
        if (g_int.st[team] >= 3)
            g_lb_hold_frames = 0;
        return;
    }
    float dx = lb[0] - mp[0], dy = lb[1] - mp[1], dz = lb[2] - mp[2];
    if (dx * dx + dz * dz > LB_RANGE * LB_RANGE || dy < -2.0f || dy > 2.0f) {
        if (g_lbp.on) {
            static int nr;
            if (LOGCAP(nr, 10))
                Xlog("lockbox: hold lost - out of range (%.2f m, dy %.2f)",
                     (double) __builtin_sqrtf(dx * dx + dz * dz),
                     (double) dy);
        }
        LbRelease();
        return;
    }
    (void) prompt_frame;
    // the interaction prompt beside a DISCOVERED enemy safe = the engine's own interactable prompt (the revive / crate hold prompt: the key glyph, pad = its own button, + the hold ring) with
    // "Unlock"; shown while in range and not unlocking (the hold itself replaces it), gone out of range / unrevealed (those paths return above)
    if (!held && g_lbObj[enemy])
        WorldPrompt(g_lbObj[enemy], "Unlock", 0.0f);
    // Only one player may unlock a safe at a time: another player's dial ticks at this safe in the last 900 ms (sent every 0.25-0.65 s while
    // they hold) = it is taken; both starting together = the lower slot keeps it. No Unlock prompt then. t2r-lockbox-multi.txt = several unlockers again.
    {
        uint32_t me_s = *(uint32_t *) T2R_SYM(kMySlot);
        uint64_t nw = GetTickCount64();
        if (g_lbOtherMs[enemy] && nw - g_lbOtherMs[enemy] < 900
            && (!g_lbHolding || g_lbOtherSlot[enemy] < me_s)
            && GetFileAttributesA("t2r-lockbox-multi.txt") ==
            INVALID_FILE_ATTRIBUTES) {
            static uint64_t tl;
            if (nw - tl > 3000) {
                tl = nw;
                Xlog("lockbox: slot %u is unlocking team %d's safe - no second unlocker", g_lbOtherSlot[enemy], enemy);
            }
            if (g_lbHolding)
                LbPoseEnd("another player unlocks this safe");
            LbRelease();
            return;
        }
    }
    if (!held && g_lbp.on) {
        static int nh;
        if (LOGCAP(nh, 10))
            Xlog("lockbox: hold lost - E / interact released (interact %d, key %d, focus %d)", InteractDown(), (GetAsyncKeyState(0x45) & 0x8000) != 0, GameFocused());
    }
    if (!held) {
        LbRelease();
        return;
    }                           // (no "Hold E to break into..." text - the safe's marker ring and the native prompt are the cue)
    float hold = g_int.hold > 0.5f ? g_int.hold : 18.0f;
    int fresh = !g_lbHolding;
    g_lbHolding = 1;
    g_lbRelT60 = 0;
    LbPoseTick(enemy, mp);    // smooth onto the spot + SP's safe-dial idle while E is held
    // as in 2013: the safe dial spun fast (hud-mp.bnk safe-lock-spin, tools/rip_safe_lock_spin.py), repeated while E is held, and the safe's own dial-anim
    // when a stage starts. No bar / text any more (the unlock needs the long hold, the server's `hold`: 20 s at the start -> 8 s after 12 min).
    {
        static uint64_t spin_at;
        if (g_t60 >= spin_at) {
            spin_at = g_t60 + 14 + IntRnd() % 26u;
            LbDialSfx(enemy);
            IntSend((uint8_t) enemy, 4);
        }                       // (randomised ticks / rotates, 0.25 .. 0.65 s apart)
        if (fresh && g_lbObj[enemy] && g_lbSafe[enemy])
            AnimateNow(g_lbObj[enemy], "dial-anim");
    }
    if (HoldAdd(&g_lb_hold_frames, &g_lb_hold_last) >=
        (uint64_t) (hold * 60)) {
        int stage = ++g_int.st[team];   // optimistic; the next reply has the server's count
        IntEvent(2, (uint8_t) stage);
        Award(LB_PARTS, me, "Opening lockbox"); // 2013: +250 per stage (stage 1, 2 and the final one)
        Xlog("interrogation: lockbox stage %d", stage);
        if (stage >= 3)
            MsgHook("Lockbox opened!", 3.0f, 0, 0);    // 2013 feed line of the unlocker (stages 1 / 2 are the banners of int_news_tick)
        g_lb_hold_frames = 0;
        g_lb_hold_last = 0;
        if (g_lbObj[enemy] && g_lbSafe[enemy])
            AnimateNow(g_lbObj[enemy], "dial-anim");  // the dial turns once more as the stage opens
        if (stage >= 3) {       // the last stage: our open-success clip on the body (SP's, with its sound events) and the safe door at once
            float olen =
                g_lbp.on ? AnimLen(SID_PLAYER, LB_POSE_OPEN) : 0.0f;
            if (olen > 0.5f) {
                uint64_t none = 0;
                DcCall(FNV64("player-force-crouch"), 0, &none);        // (the stance changes under the clip, unseen: its blend-out lands in the crouch)
                uint64_t r = AnimateNow(SID_PLAYER, LB_POSE_OPEN);
                g_lbp.phase = 2;
                g_lbp.clip = LB_POSE_OPEN;
                g_lbp.open_end =
                    GetTickCount64() + (uint64_t) (olen * 1000.0f);
                g_lbp.idle_at = g_lbp.open_end + 5000;
                Xlog("lockbox pose: open-success clip (%.2f s) -> %llx, blends into the crouch %.2f s before its end", (double) olen, (unsigned long long) r, (double) LB_OPEN_BLEND);
            }
            IntLockboxOpen(enemy);
        }
    }
}

static void IntBar(char *o, size_t n, float f)
{                               // "[||||||||||..........]"
    int k = (int) (f * 20.0f + 0.5f);
    size_t i = 0;
    if (k < 0)
        k = 0;
    if (k > 20)
        k = 20;
    if (n < 24) {
        if (n)
            o[0] = 0;
        return;
    }
    o[i++] = '[';
    for (int j = 0; j < 20; j++)
        o[i++] = j < k ? '|' : '.';
    o[i++] = ']';
    o[i] = 0;
}

// drawn from the marker draw hook (1280x720 virtual screen)
static void IntTextC(float x, float y, float sc, uint32_t col,
                       const char *s)
{                               // centred
    s = LocTrDraw(s);         // localisation
    uint64_t wh = ((fn_measure) T2R_SYM(kFnMeasureText)) (s, sc);
    float w;
    uint32_t lo = (uint32_t) wh;
    memcpy(&w, &lo, 4);
    if (!(w == w) || w < 0 || w > 2000)
        w = 0;
    ((fn_text_va) T2R_SYM(kFnDrawTextVa)) (Fbits(x - w * 0.5f), Fbits(y),
                                           Fbits(sc), (uint64_t) col, "%s",
                                           s);
}

// the draw hook's share of the 2013 notifications - the two PERSISTENT lines (everything else is a banner / feed line): "Locating enemy lockbox..." from "All intel gathered!" to the
// reveal, (the "Unlocking..." feed line was cut). Plain nametag-font text. The old text HUD (INTEL / ENEMY LOCKBOX counters, [ ] ATTACK / DEFEND,
// OPENING LOCKBOX bar) is gone; the widget HUD (interrogation_hud.c) is the one HUD.
static void IntPlain(float x, float y, float px, uint32_t col,
                      const char *s, int centred)
{
    s = LocTrDraw(s);
    float sz = NtfTextSize(px), w = NtfW(s, sz);
    ((fn_text_va)
     T2R_SYM(kFnDrawTextVa)) (Fbits(centred ? x - w * 0.5f : x),
                              Fbits(y - px * 0.5f), Fbits(sz),
                              (uint64_t) col, "%s", s);
}

static void FeatureInterrogationDraw(void)
{
    if (!IntLive() || g_int_ended_sent || HUD_HIDDEN() || g_int.win >= 0)
        return;
    int team = g_int.team;
    int need = g_int.need > 0 ? g_int.need : 5;
    // the plain white "Locating enemy lockbox..." line is cut - the notification banner already says it and stays.
    (void) team;
    (void) need;
    (void) IntPlain;
}

static int R13IntMode(void)
{
    return g_int.ok && g_int.on && !g_noInt;
}                               // 2013 rules: mode-specific values (bleed-out 7 s)
