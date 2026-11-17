// backpack.c - the TAB backpack (crafting / shop) as other players see it. Included after paired.c in t2r-ext.c
// (uses its helpers: dc_call, xlog, animate_from, anim_len, psync_oneway, psync_stop, psync_dead, g_send_tramp).
//
// Own side (feature_backpack_prop in t2r-ext.c): the worn bag <-> prop hand-off runs inside the engine's own
// EnterBackpack / LeaveBackpack (the clip's enter-backpack / leave-backpack events), weapons stay slung.
// This file: the owner broadcasts its backpack phase (MSG_BACKPACK), every receiver plays the craft clips on that
// player's puppet at the owner's clip time + the one-way latency (the psync pattern).
//
// The bag-off state IS on the wire (bagoff = g_bp_hid, the
// engine's own enter / leave swap on the owner), so a receiver now takes the bag off its puppet too: the worn bag parts are hidden and a
// held bag prop (the satchel bag actor, SimpleAttachable - a puppet cannot drive a ProcessBackpack) follows the puppet's right hand through
// the intro / outro and rests on the floor in front of it during the crouch loop. Never zero bags: the worn bag goes off only once the prop
// object exists (no prop = the old on-the-back craft). The owner also sends the moment bagoff flips (it used to ride the 500 ms period only).
// Cleaned up on bag-on, death / down / paired move / respawn / match end / timeout. t2r-no-puppet-bag-off.txt = the old behaviour.
// Second pass: first choice is the SAME animated bag the
// owner has - the look's TAB prop look (BP_PROP_LOOKS, skeleton backpack-abby) spawned as a SimpleAttachable, set on the puppet's locator every
// frame (the prop bind space = the character space) and given the prop clips backpack-intro / -loop / -outro at the same moment and phase as
// the puppet's craft clip (bp_pup_play), so straps, lid, the drop to the floor and the pick-up move frame for frame. Probed at the first take-off
// (spawn ok + get-animation-duration(backpack-intro) > 0, logged "bp: ... animated bag"); if the engine will not animate it: the look's own bag
// as a rigid actor (held_bag_pak.py, HB_ACTOR) following the hand / resting on the floor; if that actor is missing: the satchel actor.
// (older text) Puppets keep their worn bag on the back for the whole anim: the TAB prop is a ProcessBackpack that the engine only
// drives from a Player - its set-parent handler (0x141158d00, event set-parent) stores the parent at +0x600 and the
// prop follows the parent Player's backpack anim state; a puppet (NPC-type character, no player anim state, no
// PlayerBackpackAnimCtrl) cannot drive one. Hiding the puppet's bag without a prop in its hands would show NO bag,
// so the puppet crouches and crafts with the bag on (the 2013 Factions craft) - never zero, never two bags.
//
// MSG_BACKPACK [u8 ver 1][u8 phase][u8 bag off][u8 seq][u32 ms since the phase began][u16 enter ms][u16 leave ms]
//   phase 0 idle, 1 open (intro -> loop), 2 closing (outro), 3 abort (death / down / match end: blend out now).
//   Sent on every change (+100 / +300 ms copies), every 500 ms while open / closing and every 3 s while idle, so a lost
//   packet or a late joiner converges to the right state within one period. enter / leave ms = where the bag swap
//   happened in the owner's clip time (log / diagnosis; 0xffff = not seen yet).
// Logs: "bp: ..." with real ms (QPC).

#define MSG_BACKPACK 0x5B
#define BP_VER 1
#pragma pack(push, 1)
typedef struct {
    uint8_t ver, phase, bagoff, seq;
    uint32_t el_ms;
    uint16_t enter_ms, leave_ms;
} BpMsgT;
#pragma pack(pop)

enum { BPP_IDLE = 0, BPP_OPEN = 1, BPP_CLOSING = 2, BPP_ABORT = 3 };
static const char *const BPP_NAME[4] =
    { "idle", "open", "closing", "abort" };

// ---- own phase
static struct {
    int phase;
    double t_phase;             // bp_ms() the phase began
    uint8_t seq;
    double next_send;
    int copies;                 // copies: change copies left (+100 / +300 ms)
    double t_change;
    uint16_t enter_ms, leave_ms;
    uint32_t seen_enter, seen_leave;
} g_bpo = { 0, 0, 0, 0, 0, 0, 0xffff, 0xffff, 0, 0 };

static int BpOwnPhaseNow(void)
{
    return g_bpo.phase;
}

static void BpSend(double now)
{
    if (!g_sendTramp)
        return;
    BpMsgT m;
    m.ver = BP_VER;
    m.phase = (uint8_t) g_bpo.phase;
    m.bagoff = (uint8_t) (g_bpHid != 0);
    m.seq = g_bpo.seq;
    double el = now - g_bpo.t_phase;
    m.el_ms = el > 0 ? (uint32_t) el : 0;
    m.enter_ms = g_bpo.enter_ms;
    m.leave_ms = g_bpo.leave_ms;
    g_sendTramp(MSG_BACKPACK, (uint8_t *) & m, sizeof m, 0xff);
}

static void BptOpenDone(void);
static void BpOwnSet(int phase, double now, const char *why)
{
    if (phase == g_bpo.phase)
        return;
    Xlog("bp: own phase %s -> %s at t %.0f ms (%s)",
         BPP_NAME[g_bpo.phase & 3], BPP_NAME[phase & 3], now, why);
    if (phase == BPP_IDLE
        && (g_bpo.phase == BPP_CLOSING || g_bpo.phase == BPP_ABORT))
        BptOpenDone();
    g_bpo.phase = phase;
    g_bpo.t_phase = now;
    g_bpo.seq++;
    g_bpo.t_change = now;
    g_bpo.copies = 2;
    BpSend(now);
    g_bpo.next_send = now + 100.0;
}

static int BpOwnDead(void)
{
    uint64_t h = SID_PLAYER;
    uint64_t d =
        DcCall(0xe93f704f02a9ecd5ULL /* character-is-dead? */ , 1, &h);
    return d != ~0ULL && (d & 0xff);
}

static void BptOwnSample(double now);
static void BptSendTick(double now);
static void BpOwnTick(double now)
{
    static int menu_was;
    int live = MatchSubstate() == 6;
    int menu = live && (PlayerMenuActive() == 1 || g_tabOpen);
    int gone = !live || g_downed || BpOwnDead();
    if (g_bpo.seen_enter != g_bp_n_enter) {     // where the swap sat in our clip time (from the open)
        g_bpo.seen_enter = g_bp_n_enter;
        if (g_bpo.phase == BPP_OPEN) {
            double d = g_bp_t_enter - g_bpo.t_phase;
            g_bpo.enter_ms = d >= 0 && d < 60000 ? (uint16_t) d : 0xffff;
        }
    }
    if (g_bpo.seen_leave != g_bp_n_leave) {
        g_bpo.seen_leave = g_bp_n_leave;
        if (g_bpo.phase == BPP_CLOSING) {
            double d = g_bp_t_leave - g_bpo.t_phase;
            g_bpo.leave_ms = d >= 0 && d < 60000 ? (uint16_t) d : 0xffff;
        }
    }
    if (gone) {
        if (g_bpo.phase == BPP_OPEN || g_bpo.phase == BPP_CLOSING)
            BpOwnSet(BPP_ABORT, now,
                       !live ? "match end / not in game" : g_downed ?
                       "downed" : "dead");
        else if (g_bpo.phase == BPP_ABORT && now - g_bpo.t_phase > 1000.0)
            BpOwnSet(BPP_IDLE, now, "abort sent");
    } else if (menu && !menu_was)
        BpOwnSet(BPP_OPEN, now, "menu opened = intro clip starts");
    else if (!menu && menu_was && g_bpo.phase == BPP_OPEN)
        BpOwnSet(BPP_CLOSING, now, "menu closed = outro clip starts");
    else if (g_bpo.phase == BPP_CLOSING
             && ((!g_bpHid && now - g_bpo.t_phase > 1500.0)
                 || now - g_bpo.t_phase > 4000.0))
        BpOwnSet(BPP_IDLE, now, "outro over");
    else if (g_bpo.phase == BPP_ABORT && now - g_bpo.t_phase > 1000.0)
        BpOwnSet(BPP_IDLE, now, "abort sent");
    menu_was = menu;
    {
        static int off_was;
        if ((g_bpHid != 0) != off_was) {       // the bag swap moment: tell the puppets now (+100 / +300 ms copies), not at the next 500 ms period
            off_was = g_bpHid != 0;
            BpSend(now);
            g_bpo.t_change = now;
            g_bpo.copies = 2;
            g_bpo.next_send = now + 100.0;
        }
    }
    BptOwnSample(now);
    BptSendTick(now);
    if (now >= g_bpo.next_send) {       // change copies at +100 / +300 ms, then the period (500 ms active, 3 s idle)
        BpSend(now);
        double period = g_bpo.phase == BPP_OPEN
            || g_bpo.phase == BPP_CLOSING ? 500.0 : 3000.0;
        if (g_bpo.copies == 2) {
            g_bpo.copies = 1;
            g_bpo.next_send = g_bpo.t_change + 300.0;
        } else {
            g_bpo.copies = 0;
            g_bpo.next_send = now + period;
        }
    }
}

#include "bp_track.h"
// ---- puppets
enum { BPC_NONE = 0, BPC_INTRO = 1, BPC_LOOP = 2, BPC_OUTRO = 3 };
static const char *const BPC_NAME[4] =
    { "none", "intro", "loop", "outro" };
// ---- recorded own-body prop track (our OWN bag open animation is right, the bystander's is not -> replay ours on the puppet).
// Every frame while OUR bag prop is out, the prop's spinec joint is read in the body locator's yaw frame and stored per clip (1 intro, 2 loop, 3 outro) and 25 ms step
// (the clip time = our phase clock, the same clock the puppets derive their clip start from: msg el_ms). The puppets then place the prop so that ITS spinec is at
// body locator + R(yaw) * track(clip, clip time): the bag's travel in front of the body comes from the real own animation, not from a guessed channel.
// t2r-bp-record.txt next to the exe = also log every sample ("bptrack: clip C idx I rel x y z look L"), tools/bp_track_make.py turns the log into bp_track.h.
// The look does not matter: every look's prop is the backpack-abby skeleton playing the same Abby clips (male + female alike, IsAbby forced while the bag is off).
#define BPT_DT 25.0
#define BPT_N 320
// The logger is meant to catch how the backpack moves relative to the body and reproduce that onto other players: the track was
// only learned from the VIEWER's own opens and the recorder flag was never set (bp_track.h empty), so A, who never opened its own bag, had no track and B's bag on A
// fell back to the spinec follow (log: no "bptrack: slot" line all session). Now, with no flag:
//   - every own open is recorded (as before) and saved next to the exe (t2r-bp-track.bin), loaded at start: the local track survives restarts;
//   - the owner streams its samples to every peer while it records (MSG_BACKPACK ver 0x54 chunks, one per 50 ms) and resends the whole track after each close
//     (lost packets, late joiners); the receiver keeps it per slot = the bag on another player follows that player's own recorded body-relative path;
//   - per clip the puppet latches its source at the clip start: the owner's track when complete, else our own, else the spinec follow (logged).
static float g_bpt[4][BPT_N][3];
static uint8_t g_bptOk[4][BPT_N];
static int g_bpt_init, g_bpt_live;
static float g_bptp[T2R_MAX_PEERS][4][BPT_N][3];
static uint8_t g_bptpOk[T2R_MAX_PEERS][4][BPT_N];      // per owner slot, from its chunks
static uint8_t g_bptDirty[4][BPT_N];   // own samples not yet sent
static int g_bptNrx[T2R_MAX_PEERS];
static int g_bptOpenN[4];     // own samples recorded in the current open, per clip
#define BPT_FILE "t2r-bp-track.bin"
#define BPT_MAGIC 0x31545042u   // "BPT1"
static void BptrLoad(void);
static void BptLoadHeader(void)
{
    if (g_bpt_init)
        return;
    g_bpt_init = 1;
    for (int k = 0; k < BPT_H_N; k++)
        if (BPT_H[k].c >= 1 && BPT_H[k].c <= 3 && BPT_H[k].i < BPT_N) {
            memcpy(g_bpt[BPT_H[k].c][BPT_H[k].i], BPT_H[k].v, 12);
            g_bptOk[BPT_H[k].c][BPT_H[k].i] = 1;
        }
    int nf = 0;
    FILE *f = ExeSideOpen(BPT_FILE, "rb");
    if (f) {
        uint32_t mg = 0;
        static float tb[4][BPT_N][3];
        static uint8_t okb[4][BPT_N];
        if (fread(&mg, 4, 1, f) == 1 && mg == BPT_MAGIC
            && fread(okb, sizeof okb, 1, f) == 1
            && fread(tb, sizeof tb, 1, f) == 1)
            for (int c = 1; c <= 3; c++)
                for (int i = 0; i < BPT_N; i++)
                    if (okb[c][i] && tb[c][i][0] == tb[c][i][0]
                        && fabsf(tb[c][i][0]) < 3.0f
                        && fabsf(tb[c][i][1]) < 3.0f
                        && fabsf(tb[c][i][2]) < 3.0f) {
                        memcpy(g_bpt[c][i], tb[c][i], 12);
                        g_bptOk[c][i] = 1;
                        g_bptDirty[c][i] = 1;
                        nf++;
                    }
        fclose(f);
    }
    Xlog("bptrack: %d recorded samples from bp_track.h, %d from %s (own opens of earlier sessions, sent to the peers)", BPT_H_N, nf, BPT_FILE);
    BptrLoad();
}

// gap-tolerant read: between the nearest recorded samples around the clip time, held at the first / last one outside them
static int BptGetT(const float (*tb)[3], const uint8_t *ok, double ms,
                     float *out)
{
    if(ms < 0)
        ms = 0;
    double f = ms / BPT_DT;
    int i = (int) f;
    if (i > BPT_N - 1)
        i = BPT_N - 1;
    int a = i, b = i + 1;
    while (a >= 0 && !ok[a])
        a--;
    while (b < BPT_N && !ok[b])
        b++;
    if (a < 0 && b >= BPT_N)
        return 0;
    if (a < 0) {
        memcpy(out, tb[b], 12);
        return 1;
    }
    if (b >= BPT_N) {
        memcpy(out, tb[a], 12);
        return 1;
    }
    float t = (float) ((f - a) / (double) (b - a));
    if (t < 0)
        t = 0;
    if (t > 1)
        t = 1;
    for (int k = 0; k < 3; k++)
        out[k] = tb[a][k] + (tb[b][k] - tb[a][k]) * t;
    return 1;
}

// usable = at least 16 samples (400 ms) and no hole over 200 ms between the first and the last
static int BptComplete(const uint8_t *ok, int *n_out)
{
    int n = 0, last = -1, gap = 0;
    for (int i = 0; i < BPT_N; i++)
        if (ok[i]) {
            if (last >= 0 && i - last > gap)
                gap = i - last;
            last = i;
            n++;
        }
    if (n_out)
        *n_out = n;
    return n >= 16 && gap <= 8;
}

static int BptGet(int clip, double ms, float *out)
{
    BptLoadHeader();
    if (clip < 1 || clip > 3)
        return 0;
    return BptGetT((const float (*)[3]) g_bpt[clip], g_bptOk[clip], ms,
                     out);
}

static void BptSave(void)
{
    FILE *f = ExeSideOpen(BPT_FILE, "wb");
    if (!f) {
        Xlog("bptrack: cannot write %s", BPT_FILE);
        return;
    }
    uint32_t mg = BPT_MAGIC;
    fwrite(&mg, 4, 1, f);
    fwrite(g_bptOk, sizeof g_bptOk, 1, f);
    fwrite(g_bpt, sizeof g_bpt, 1, f);
    fclose(f);
}

// MSG_BACKPACK track chunk: [u8 0x54][u8 clip][u16 first idx][u8 n][u8 0] then n x {u8 has, i16 x, i16 y, i16 z} (mm, body yaw frame).
// Old receivers drop it (ver != BP_VER).
#define BPT_VER 0x54
#define BPT_CHUNK 24
static int BptrSendOne(void);
static void BptSendTick(double now)
{
    static double next;
    static int alt;
    if (!g_sendTramp || now < next || MatchSubstate() != 6)
        return;
    if ((alt++ & 1) && BptrSendOne()) {
        next = now + 50.0;
        return;
    }                           // the root track (the one the puppets replay) and the spinec track take turns
    for (int c = 1; c <= 3; c++) {
        int i0 = 0;
        while (i0 < BPT_N && !g_bptDirty[c][i0])
            i0++;
        if (i0 >= BPT_N)
            continue;
        uint8_t b[6 + BPT_CHUNK * 7];
        int n = 0;
        b[0] = BPT_VER;
        b[1] = (uint8_t) c;
        b[2] = (uint8_t) (i0 & 0xff);
        b[3] = (uint8_t) (i0 >> 8);
        b[5] = 0;
        for (int i = i0; i < BPT_N && n < BPT_CHUNK; i++, n++) {
            uint8_t *e = b + 6 + n * 7;
            int16_t q[3] = { 0, 0, 0 };
            e[0] = g_bptOk[c][i];
            if (e[0])
                for (int k = 0; k < 3; k++)
                    q[k] = (int16_t) lrintf(g_bpt[c][i][k] * 1000.0f);
            memcpy(e + 1, q, 6);
            g_bptDirty[c][i] = 0;
        }
        b[4] = (uint8_t) n;
        g_sendTramp(MSG_BACKPACK, b, (uint64_t) (6 + n * 7), 0xff);
        next = now + 50.0;
        return;
    }
    if (BptrSendOne())
        next = now + 50.0;
}

static void BptNetRecv(int s, const uint8_t *b, uint64_t len)
{
    if (len < 6 || b[1] < 1 || b[1] > 3)
        return;
    int c = b[1], i0 = b[2] | (b[3] << 8), n = b[4];
    if (len < (uint64_t) (6 + n * 7) || i0 + n > BPT_N)
        return;
    for (int j = 0; j < n; j++) {
        const uint8_t *e = b + 6 + j * 7;
        int16_t q[3];
        memcpy(q, e + 1, 6);
        if (!e[0])
            continue;           // a hole in the owner's track: keep what we had there
        g_bptpOk[s][c][i0 + j] = 1;
        for (int k = 0; k < 3; k++)
            g_bptp[s][c][i0 + j][k] = q[k] / 1000.0f;
    }
    if (!g_bptNrx[s]++ || g_bptNrx[s] % 50 == 0) {
        int nn;
        BptComplete(g_bptpOk[s][c], &nn);
        Xlog("bptrack: slot %d track chunk %d (clip %d idx %d+%d, that clip now %d samples)", s, g_bptNrx[s], c, i0, n, nn);
    }
}

// ---- ROOT track. The spinec track put the prop where the owner's prop SPINEC was by feedback
// (move the root by target - the spinec read back): the joint read is a frame behind the root we set, so in the fast part of the intro / outro the prop trailed its
// target (A log: "off target 30.8 cm" at 594 ms) - lag - and only the spinec POSITION was replayed, not the prop's orientation - offset. The prop plays the same
// "<clip>--backpack" clip on both screens, so what has to be copied is only where the engine puts the prop's ROOT on the owner (the prop is parented to the player
// there): its full locator (position + rotation) relative to the body locator, per clip and 25 ms, sampled from the owner's ProcessBackpack locator. The puppet sets its
// prop root to body locator * that, open loop, every frame at its own clip time (no feedback, no read-back) - the clip moves the bag the same way on both screens.
// Wire: MSG_BACKPACK ver 0x55 chunks [u8 0x55][u8 clip][u16 idx][u8 n][u8 0] + n x {u8 has, i16 pos x y z mm, i16 quat x y z w * 32767}; t2r-bp-root.bin next to the exe.
#define BPTR_FILE "t2r-bp-root.bin"
#define BPTR_MAGIC 0x31525042u  // "BPR1"
#define BPTR_VER 0x55
#define BPTR_CHUNK 16
static float g_bptr[4][BPT_N][7];
static uint8_t g_bptrOk[4][BPT_N], g_bptr_dirty[4][BPT_N];
static float g_bptrp[T2R_MAX_PEERS][4][BPT_N][7];
static uint8_t g_bptrpOk[T2R_MAX_PEERS][4][BPT_N];
static int g_bptrOpenN[4], g_bptr_nrx[T2R_MAX_PEERS], g_bptr_live;
static int BptrValid(const float *v)
{
    float n = v[3] * v[3] + v[4] * v[4] + v[5] * v[5] + v[6] * v[6];
    return __builtin_isfinite(v[0] + v[1] + v[2] + n) && fabsf(v[0]) < 3.0f
        && fabsf(v[1]) < 3.0f && fabsf(v[2]) < 3.0f && n > 0.8f
        && n < 1.2f;
}

static void BptrLoad(void)
{
    int nf = 0;
    FILE *f = ExeSideOpen(BPTR_FILE, "rb");
    if (f) {
        uint32_t mg = 0;
        static float tb[4][BPT_N][7];
        static uint8_t okb[4][BPT_N];
        if (fread(&mg, 4, 1, f) == 1 && mg == BPTR_MAGIC
            && fread(okb, sizeof okb, 1, f) == 1
            && fread(tb, sizeof tb, 1, f) == 1)
            for (int c = 1; c <= 3; c++)
                for (int i = 0; i < BPT_N; i++)
                    if (okb[c][i] && BptrValid(tb[c][i])) {
                        memcpy(g_bptr[c][i], tb[c][i], 28);
                        g_bptrOk[c][i] = 1;
                        g_bptr_dirty[c][i] = 1;
                        nf++;
                    }
        fclose(f);
    }
    Xlog("bptrack: %d root samples from %s (own opens of earlier sessions, sent to the peers)", nf, BPTR_FILE);
}

static void BptrSave(void)
{
    FILE *f = ExeSideOpen(BPTR_FILE, "wb");
    if (!f) {
        Xlog("bptrack: cannot write %s", BPTR_FILE);
        return;
    }
    uint32_t mg = BPTR_MAGIC;
    fwrite(&mg, 4, 1, f);
    fwrite(g_bptrOk, sizeof g_bptrOk, 1, f);
    fwrite(g_bptr, sizeof g_bptr, 1, f);
    fclose(f);
}

// gap tolerant as bpt_get_t; the rotation is blended in the same hemisphere and renormalised
static int BptrGetT(const float (*tb)[7], const uint8_t *ok, double ms,
                      float *out)
{
    if(ms < 0)
        ms = 0;
    double f = ms / BPT_DT;
    int i = (int) f;
    if (i > BPT_N - 1)
        i = BPT_N - 1;
    int a = i, b = i + 1;
    while (a >= 0 && !ok[a])
        a--;
    while (b < BPT_N && !ok[b])
        b++;
    if (a < 0 && b >= BPT_N)
        return 0;
    if (a < 0) {
        memcpy(out, tb[b], 28);
        return 1;
    }
    if (b >= BPT_N) {
        memcpy(out, tb[a], 28);
        return 1;
    }
    float t = (float) ((f - a) / (double) (b - a));
    if (t < 0)
        t = 0;
    if (t > 1)
        t = 1;
    float sg =
        tb[a][3] * tb[b][3] + tb[a][4] * tb[b][4] + tb[a][5] * tb[b][5] +
        tb[a][6] * tb[b][6] < 0 ? -1.0f : 1.0f;
    for (int k = 0; k < 3; k++)
        out[k] = tb[a][k] + (tb[b][k] - tb[a][k]) * t;
    float n = 0;
    for (int k = 3; k < 7; k++) {
        out[k] = tb[a][k] + (sg * tb[b][k] - tb[a][k]) * t;
        n += out[k] * out[k];
    }
    n = sqrtf(n);
    if (n < 1e-3f)
        return 0;
    for (int k = 3; k < 7; k++)
        out[k] /= n;
    return 1;
}

// world locator (pos xyzw, quat xyzw) of a process (its bound frame +0x100, 0x14133db30)
static int BptrProcLoc(uint8_t *pr, float *l8)
{
    typedef const float *(*fn_loc)(void *, float *);
    if (!pr || IsBadReadPtr(pr + 0x100, 0x50))
        return 0;
    float a[16];
    const float *p =
        ((fn_loc) ((uint8_t *) GetModuleHandleW(NULL) + 0x133db30)) (pr +
                                                                     0x100,
                                                                     a);
    if (!p || IsBadReadPtr(p, 32))
        return 0;
    memcpy(l8, p, 32);
    float n =
        l8[4] * l8[4] + l8[5] * l8[5] + l8[6] * l8[6] + l8[7] * l8[7];
    return __builtin_isfinite(l8[0] + l8[1] + l8[2] + n) && n > 0.8f
        && n < 1.2f;
}

// owner: the prop root relative to the body locator: p = conj(qb) (p_prop - p_body), q = conj(qb) q_prop
static void BptrOwnSample(uint8_t *pr, const float *loc, int clip, int i)
{
    float l8[8];
    if (!BptrProcLoc(pr, l8)) {
        static int nb;
        if (nb++ < 3)
            Xlog("bptrack: own prop root locator unreadable (proc %p)",
                 (void *) pr);
        return;
    }
    float cq[4] = { -loc[4], -loc[5], -loc[6], loc[7] }, d[3] =
        { l8[0] - loc[0], l8[1] - loc[1], l8[2] - loc[2] }, v[7];
    Qrotv(cq, d, v);
    BpQmul(cq, l8 + 4, v + 3);
    if (!BptrValid(v))
        return;
    memcpy(g_bptr[clip][i], v, 28);
    g_bptrOk[clip][i] = 1;
    g_bptr_dirty[clip][i] = 1;
    g_bptrOpenN[clip]++;
    g_bptr_live++;
    static int nl;
    if (nl < 8 && (i % 10) == 0) {
        nl++;
        Xlog("bptrack: own prop root clip %d idx %d rel pos %.3f %.3f %.3f quat %.3f %.3f %.3f %.3f", clip, i, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
    }
}

static int BptrSendOne(void)
{
    for (int c = 1; c <= 3; c++) {
        int i0 = 0;
        while (i0 < BPT_N && !g_bptr_dirty[c][i0])
            i0++;
        if (i0 >= BPT_N)
            continue;
        uint8_t b[6 + BPTR_CHUNK * 15];
        int n = 0;
        b[0] = BPTR_VER;
        b[1] = (uint8_t) c;
        b[2] = (uint8_t) (i0 & 0xff);
        b[3] = (uint8_t) (i0 >> 8);
        b[5] = 0;
        for (int i = i0; i < BPT_N && n < BPTR_CHUNK; i++, n++) {
            uint8_t *e = b + 6 + n * 15;
            int16_t q[7] = { 0 };
            e[0] = g_bptrOk[c][i];
            if (e[0]) {
                for (int k = 0; k < 3; k++)
                    q[k] = (int16_t) lrintf(g_bptr[c][i][k] * 1000.0f);
                for (int k = 3; k < 7; k++)
                    q[k] = (int16_t) lrintf(g_bptr[c][i][k] * 32767.0f);
            }
            memcpy(e + 1, q, 14);
            g_bptr_dirty[c][i] = 0;
        }
        b[4] = (uint8_t) n;
        g_sendTramp(MSG_BACKPACK, b, (uint64_t) (6 + n * 15), 0xff);
        return 1;
    }
    return 0;
}

static void BptrNetRecv(int s, const uint8_t *b, uint64_t len)
{
    if (len < 6 || b[1] < 1 || b[1] > 3)
        return;
    int c = b[1], i0 = b[2] | (b[3] << 8), n = b[4];
    if (len < (uint64_t) (6 + n * 15) || i0 + n > BPT_N)
        return;
    for (int j = 0; j < n; j++) {
        const uint8_t *e = b + 6 + j * 15;
        int16_t q[7];
        memcpy(q, e + 1, 14);
        if (!e[0])
            continue;
        float v[7];
        for (int k = 0; k < 3; k++)
            v[k] = q[k] / 1000.0f;
        for (int k = 3; k < 7; k++)
            v[k] = q[k] / 32767.0f;
        if (!BptrValid(v))
            continue;
        memcpy(g_bptrp[s][c][i0 + j], v, 28);
        g_bptrpOk[s][c][i0 + j] = 1;
    }
    if (!g_bptr_nrx[s]++ || g_bptr_nrx[s] % 50 == 0) {
        int nn;
        BptComplete(g_bptrpOk[s][c], &nn);
        Xlog("bptrack: slot %d root chunk %d (clip %d idx %d+%d, that clip now %d samples)", s, g_bptr_nrx[s], c, i0, n, nn);
    }
}

static void BptrOpenDone(void)
{
    int n[4] = { 0 };
    for (int c = 1; c <= 3; c++) {
        BptComplete(g_bptrOk[c], &n[c]);
        for (int i = 0; i < BPT_N; i++)
            if (g_bptrOk[c][i])
                g_bptr_dirty[c][i] = 1;
    }
    Xlog("bptrack: own open root samples intro %d / loop %d / outro %d (root track now %d / %d / %d), saved to %s", g_bptrOpenN[1], g_bptrOpenN[2], g_bptrOpenN[3], n[1], n[2], n[3], BPTR_FILE);
    memset(g_bptrOpenN, 0, sizeof g_bptrOpenN);
    if (g_bptr_live)
        BptrSave();
}

// own close: log what this open recorded, save it, resend the whole track (lost chunks, late joiners)
static void BptOpenDone(void)
{
    BptLoadHeader();
    int n[4] = { 0 };
    for (int c = 1; c <= 3; c++) {
        BptComplete(g_bptOk[c], &n[c]);
        for (int i = 0; i < BPT_N; i++)
            if (g_bptOk[c][i])
                g_bptDirty[c][i] = 1;
    }
    Xlog("bptrack: own open recorded intro %d / loop %d / outro %d samples (track now %d / %d / %d), saved to %s, resending to the peers", g_bptOpenN[1], g_bptOpenN[2], g_bptOpenN[3], n[1], n[2], n[3], BPT_FILE);
    memset(g_bptOpenN, 0, sizeof g_bptOpenN);
    if (g_bpt_live)
        BptSave();
    BptrOpenDone();
}

static int BptRecOn(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 3000) {
        at = n;
        FILE *f = ExeSideOpen("t2r-bp-record.txt", "r");
        v = f != NULL;
        if (f)
            fclose(f);
    }
    return v;
}

static int BptOff(void)
{
    static int v = -1;
    if (v < 0) {
        FILE *f = ExeSideOpen("t2r-no-bp-track.txt", "r");
        v = f != NULL;
        if (f)
            fclose(f);
    }
    return v;
}

// our side: sample (called every frame from bp_own_tick)
static void BptOwnSample(double now)
{
    if ((g_bpo.phase != BPP_OPEN && g_bpo.phase != BPP_CLOSING)
        || !g_bpHid || BptOff())
        return;
    BpStateT bs = BpState();
    if (!bs.ctrl || bs.vis != 1 || IsBadReadPtr(bs.ctrl + 0x198, 8))
        return;
    uint8_t *hh = *(uint8_t **) (bs.ctrl + 0x198), *pr = (hh
                                                          &&
                                                          !IsBadReadPtr(hh,
                                                                        16))
        ? *(uint8_t **) (hh + 8) : NULL;
    uint64_t ph = (pr
                   && !IsBadReadPtr(pr,
                                    0xd0)) ? *(uint64_t *) (pr + 0xc0) : 0;
    if (!ph)
        return;
    double el = now - g_bpo.t_phase;
    int clip;
    double ms;
    if (g_bpo.phase == BPP_CLOSING) {
        clip = BPC_OUTRO;
        ms = el;
    } else if (el < 1567.0) {
        clip = BPC_INTRO;
        ms = el;
    } else {
        clip = BPC_LOOP;
        ms = fmod(el - 1567.0, 7400.0);
    }
    float lb[16];
    const float *loc = PlayerLocator(lb);
    float pj[3], pq[4];
    if (!loc || !XbxLoc(ph, "spinec", pj, pq))
        return;
    float yw = 2.0f * atan2f(loc[5], loc[7]), cs = cosf(yw), sn = sinf(yw);
    float dx = pj[0] - loc[0], dz = pj[2] - loc[2];
    float r[3] = { dx * cs - dz * sn, pj[1] - loc[1], dx * sn + dz * cs };
    if (fabsf(r[0]) > 3.0f || fabsf(r[1]) > 3.0f || fabsf(r[2]) > 3.0f)
        return;
    int i = (int) (ms / BPT_DT + 0.5);
    if (i < 0 || i >= BPT_N)
        return;
    BptLoadHeader();
    memcpy(g_bpt[clip][i], r, 12);
    g_bptOk[clip][i] = 1;
    g_bpt_live++;
    g_bptDirty[clip][i] = 1;
    g_bptOpenN[clip]++;
    BptrOwnSample(pr, loc, clip, i);
    if (BptRecOn())
        Xlog("bptrack: clip %d idx %d rel %.4f %.4f %.4f look %s", clip, i,
             r[0], r[1], r[2], LookName(BpPlayerLook(BpMe())));
}

// the player's states (dc1/anim-player/player-inventory2.bin): inventory-intro = stand^craft-intro, inventory-loop =
// crouch-craft-idle, inventory-outro = stand^craft-outro (character column; the prop column is backpack-*). The puppet
// resolves the overlay name itself; first clip its anim tables know (get-animation-duration > 0) wins, logged.
static const char *const BP_CLIPS[4][4] = {
    { NULL },
    { "abby-stand^craft-intro", "stand^craft-intro",
     "abby-lg-stand^craft-intro", "lg-stand^craft-intro" },
    { "abby-crouch-craft-idle", "crouch-craft-idle", NULL, NULL },
    { "abby-stand^craft-outro", "stand^craft-outro",
     "abby-lg-stand^craft-outro", "lg-stand^craft-outro" }
};

#define BPP_BLEND 0.25f
static struct {
    int phase;                  // the owner's last phase
    uint8_t seq;
    double rx_ms;               // last message (bp_ms)
    double ct_rx;               // the owner's clip time (ms in phase) at rx, latency-corrected
    int clip;
    double t0;
    float dur;                  // playing clip, its time 0 (bp_ms), length (s)
    const char *name;
    uint64_t h;                 // puppet handle the clip runs on
    int warned_none;
    int want_off;               // the owner's last bagoff
    int pmode;                  // 1 = animated TAB prop look, 2 = rigid per-look bag, 3 = satchel (hand follow)
    int off;                    // 1 = we took this puppet's worn bag off and hold the prop
    uint64_t prop;              // held bag prop object (0 = none)
    int path;
    double probe_t;
    float lid0[3];              // joint probe of the waiting prop
    uint8_t *proc;              // its ProcessBackpack (only while the prop waits for its clips)
    uint8_t was_vis[BP_NPARTS]; // worn parts shown at the take-off
    float floor_pos[3];
    int floor_ok;               // where the bag rests during the crouch loop
    int tsrc, tsrc_clip;
    double tsrc_t0;             // track replay: source latched at the clip start (0 none = spinec follow, 1 the owner's track, 2 our own)
    int rsrc, rsrc_clip;
    double rsrc_t0;             // root track replay: the same, for the root track (preferred over the spinec track)
    int ready;
    double t_off, enter_pred;   // prop spawned + animating early, waiting for the swap moment; when the puppet's bag went off; the owner's swap clip time
    float last_set[3];
    uint64_t last_prop;         // track replay: where we put the prop last frame (its spinec read then gives the clip's own offset)
    float bpoff[3];
    int bpoff_st;               // open-offset fix: constant world correction (body spinec - prop spinec at the first clip frame), 0 = unmeasured, 1 = applied, 2 = off/unreadable
    double t_hide, t_proof;     // last re-assert of the hidden parts / last proof log
} g_bpp[T2R_MAX_PEERS];
static uint64_t BpPuppet(int slot)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint8_t *peer = slot >= 0
        && slot <
        T2R_MAX_PEERS ? ((fn_peer) T2rPeer) ((uint32_t) slot) : NULL;
    return peer
        && !IsBadReadPtr(peer + 0x60, 8) ? *(uint64_t *) (peer + 0x60) : 0;
}

// ---- the puppet's bag off / held prop
static int BpPupOffDisabled(void)
{
    static int v = -1;
    if (v < 0) {
        FILE *f = ExeSideOpen("t2r-no-puppet-bag-off.txt", "r");
        v = f != NULL;
        if (f)
            fclose(f);
    }
    return v;
}

static void BpPropPose(uint64_t o, const float *pos, const float *loc8)
{
    static __attribute__((aligned(16))) PsyncBf ring[8];
    static unsigned n;
    PsyncBf *bf = &ring[n++ & 7];
    float yaw = 2.0f * atan2f(loc8[5], loc8[7]);
    float a[7] =
        { pos[0], pos[1], pos[2], 0.0f, sinf(yaw * 0.5f), 0.0f,
   cosf(yaw * 0.5f) };
    PsyncBfMake(bf, a);
    SetObjLoc(o, bf);
}

static void BpPupPropKill(int s, int live)
{
    if (g_bpp[s].prop && live) {
        uint64_t o = g_bpp[s].prop;
        DcCall(FNV64("kill-entity"), 1, &o);
    }
    g_bpp[s].prop = 0;
    g_bpp[s].floor_ok = 0;
    g_bpp[s].pmode = 0;
    g_bpp[s].bpoff_st = 0;
    g_bpp[s].ready = 0;
}

// bag back on the puppet: the parts that were shown at the take-off, the prop gone
static void BpPupBagOn(int s, uint64_t h, int live, const char *why)
{
    if (!g_bpp[s].off && !g_bpp[s].prop)
        return;
    uint8_t *dc = live && h ? GvisDc(h) : NULL;
    g_bpPupOff[s] = 0;        // the armour rig re-asserts its copy / original pair from here
    if (dc)
        for (int i = 0; i < BP_NPARTS; i++)
            if (g_bpp[s].was_vis[i])
                BpDcShow(dc, g_bpPartSid[i]);
    BpPupPropKill(s, live);
    if (g_bpp[s].off)
        Xlog("bp: slot %d puppet bag back on (%s) at t %.0f ms", s, why,
             BpMs());
    g_bpp[s].off = 0;
}

// the puppet's look -> its pool row (FACTION_LOOKS / FACTION_LOOKS_WINTER, winter crew keep the base bag)
static const char *const BP_HB_ACTOR[2][8] = {
    { "mp-t2r-hb-sm000000", "mp-t2r-hb-sm000000", "mp-t2r-hb-sm000000",
     "mp-t2r-hb-sm000000", "mp-t2r-hb-sm000000", "mp-t2r-hb-sm000000",
     "mp-t2r-hb-sf00000000", "mp-t2r-hb-sf00000000" },
    { "mp-t2r-hb-m1100000000000", "mp-t2r-hb-m0800000000000",
     "mp-t2r-hb-m1200000000000", "mp-t2r-hb-m0500000000000",
     "mp-t2r-hb-m1300000000000", "mp-t2r-hb-m1400000000000",
     "mp-t2r-hb-f130000000000000", "mp-t2r-hb-f120000000000000" }
};

static int BpPupRow(int s, int *pool, int *idx)
{
    const char *ls = g_flSpawnStr[s];
    if (!ls[0])
        return 0;
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < FL_N && i < 8; i++) {
            const char *a = FACTION_LOOKS[t][i], *b =
                FACTION_LOOKS_WINTER[t][i];
            if ((a && !strncmp(ls, a, strlen(a)))
                || (b && !strncmp(ls, b, strlen(b)))) {
                *pool = t;
                *idx = i;
                return 1;
            }
        }
    return 0;
}

static const char *BpPropLookOf(int pool, int idx)
{
    BpTables();
    for (int i = 0; i < BP_NPROP; i++)
        if (!strcmp(BP_PROP_LOOKS[i][0], FACTION_LOOKS[pool][idx]))
            return (g_bpAlways[i]
                    || g_bp_fl_on) ? BP_PROP_LOOKS[i][1] : NULL;
    return NULL;
}

static uint64_t BpSpawnProp(const char *art)
{
    uint64_t a[9] =
        { FNV64(art), FNV64("SimpleAttachable"), 0, 0, 1, 0, 0, 0, 0 };
    uint64_t o = DcCall(FNV64("spawn-object"), 9, a);
    return o && o != ~0ULL ? o : 0;
}

// THE REAL THING (a SimpleAttachable spawned from a look name has no anim set - "plays no backpack-intro"): the engine's own prop spawn
// for the local player, exe 0x140afd7f0 (PlayerBackpackAnimCtrl, this = controller: +0x118 player, +0x190 pending, +0x198 prop handle ptr, +0x1a0 gen;
// rdx = the player's world locator, 32 bytes). It builds a ProcessBackpack spawn (art group at [rsp+0x198]: our detour bp_pick answers g_bp_spawn_art),
// spawns it (0x1414cd790), sends it set-parent(player) + the hide message and stores the handle at ctrl+0x198. Run here on a COPY of our own controller (so
// the player behind +0x118 is a real Player: the vfuncs +0xb18 / +0x3a8 / +0x390 and the parent link are valid; our real controller's state is untouched)
// with the puppet's locator. The prop is then not the puppet's - it is parented to our body - but the puppet never drives it either way: we set its
// locator on the puppet every frame and play backpack-intro / -loop / -outro on it ourselves (it has the backpack-abby skeleton + anim set, which the
// SimpleAttachable did not). ShowObject (0xc2f7fb66d732410b, what EnterBackpack sends) makes it visible.
typedef void (*fn_bp_real_spawn)(void *ctrl, const float *loc8);
typedef void *(*fn_bp_send)(void *, void *, uint64_t, void *, void *,
                            void *, void *, void *);
static uint64_t BpSpawnReal(const float *loc8, uint64_t art,
                              uint8_t **proc_out)
{
    static __attribute__((aligned(32))) uint8_t cc[0x300];
    uint8_t *me = BpMe(), *c = me ? BpCtrlOf(me) : NULL;
    if (!c || IsBadReadPtr(c, 0x1f0))
        return 0;
    memcpy(cc, c, 0x1f0);
    *(uint64_t *) (cc + 0x198) = 0;
    *(uint32_t *) (cc + 0x1a0) = 0;
    cc[0x190] = 1;
    __attribute__((aligned(32))) float lc[8];
    memcpy(lc, loc8, 32);
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    g_bpSpawnArt = art ? art : FNV64("backpack-abby");
    ((fn_bp_real_spawn) (exe + 0xafd7f0)) (cc, lc);
    g_bpSpawnArt = 0;
    uint8_t *slot = *(uint8_t **) (cc + 0x198);
    if (!slot || IsBadReadPtr(slot, 16))
        return 0;
    uint8_t *proc = *(uint8_t **) (slot + 8);
    if (!proc || IsBadReadPtr(proc, 0xd0)
        || *(uint32_t *) (proc + 0x94) != *(uint32_t *) (cc + 0x1a0))
        return 0;
    *proc_out = proc;
    if (*(uint8_t **) (proc + 0x80) != slot)
        Xlog("bp: spawned prop process %p: proc+0x80 %p is not its handle slot %p (events would be dropped)", (void *) proc, *(void **) (proc + 0x80), (void *) slot);
    return *(uint64_t *) (proc + 0xc0);
}

static void BpSendMsg2(uint8_t *proc, uint64_t msg, void *arg)
{
    static
        __attribute__((aligned(16))) uint8_t z[6][16], tgt[16], out[0x40];
    // (crash seen twice at exe+1573557 = the event sender's slot read `mov rdi,[rdi+8]`, right after "bp: ... path A ... shown"):
    // the cached process pointer can be stale (the prop process freed / its memory reused), so proc+0x80 held garbage. Send only when
    // the process still owns its handle slot (slot readable and slot+8 points back at this process), else drop the message.
    if (!proc || IsBadReadPtr(proc, 0xd0))
        return;
    uint8_t *slot = *(uint8_t **) (proc + 0x80);
    if (!slot || IsBadReadPtr(slot, 16) || *(uint8_t **) (slot + 8) != proc
        || !*(uint32_t *) (proc + 0x94)) {
        static int nl;
        if (nl < 20) {
            nl++;
            Xlog("bp: event %llx to a stale prop process %p dropped (slot %p)", (unsigned long long) msg, (void *) proc, (void *) slot);
        }
        return;
    }
    memset(z, 0, sizeof z);
    memset(out, 0, sizeof out);
    *(uint64_t *) tgt = *(uint64_t *) (proc + 0x80);
    *(uint32_t *) (tgt + 8) = *(uint32_t *) (proc + 0x94);
    *(uint32_t *) (tgt + 12) = 0;
    ((fn_bp_send) ((uint8_t *) GetModuleHandleW(NULL) + 0x15734d0)) (out,
                                                                     z[0],
                                                                     msg,
                                                                     tgt,
                                                                     arg ?
                                                                     arg :
                                                                     z[1],
                                                                     z[2],
                                                                     z[3],
                                                                     z[4]);
}

static void BpSendMsg(uint8_t *proc, uint64_t msg)
{
    BpSendMsg2(proc, msg, NULL);
}

// the spawn parents the prop to our player (set-parent 0xe93d6ecbdf018772, argument built by 0x1414cfc60 from the player's handle): so that the prop's per-frame
// update never reads OUR backpack state, the same message again with a null handle
static void BpPropUnparent(uint8_t *proc)
{
    typedef void *(*fn_harg)(void *, void *);
    static __attribute__((aligned(16))) uint8_t nul[16], arg[0x20];
    memset(nul, 0, sizeof nul);
    memset(arg, 0, sizeof arg);
    ((fn_harg) ((uint8_t *) GetModuleHandleW(NULL) + 0x14cfc60)) (arg,
                                                                  nul);
    BpSendMsg2(proc, 0xe93d6ecbdf018772ULL, arg);
}

// path B (night26 RE: the player state graph's prop column - inventory-intro/loop/outro -> backpack-intro/loop/outro - drives the ProcessBackpack through its PARENT: the
// prop handler 0x141158d00 stores set-parent's target at +0x600 and the prop reads that parent's state through its vfunc +0x870; bp_prop_unparent removed that link): the same
// set-parent message with the PUPPET as the target (handle = {proc+0x80, proc+0x94}, what the engine builds for the player at 0x140afda6b)
static int BpPropParentPuppet(uint8_t *proc, uint64_t puppet_h)
{
    typedef void *(*fn_harg)(void *, void *);
    typedef uint8_t *(*fn_proc)(uint64_t);
    static __attribute__((aligned(16))) uint8_t tgt[16], arg[0x20];
    uint8_t *pp =
        puppet_h ? ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (puppet_h) :
        NULL;
    if (!pp || IsBadReadPtr(pp, 0xa0) || !proc || IsBadReadPtr(proc, 0xd0))
        return 0;
    memset(tgt, 0, sizeof tgt);
    memset(arg, 0, sizeof arg);
    *(uint64_t *) tgt = *(uint64_t *) (pp + 0x80);
    *(uint32_t *) (tgt + 8) = *(uint32_t *) (pp + 0x94);
    ((fn_harg) ((uint8_t *) GetModuleHandleW(NULL) + 0x14cfc60)) (arg,
                                                                  tgt);
    BpSendMsg2(proc, 0xe93d6ecbdf018772ULL, arg);
    return 1;
}

static int BpParentOptout(void)
{
    static int v = -1;
    if (v < 0) {
        FILE *f = ExeSideOpen("t2r-no-puppet-bag-parent.txt", "r");
        v = f != NULL;
        if (f)
            fclose(f);
    }
    return v;
}

static int BpRigidOptin(void)
{
    static int v = -1;
    if (v < 0) {
        FILE *f = ExeSideOpen("t2r-puppet-bag-rigid.txt", "r");
        v = f != NULL;
        if (f)
            fclose(f);
    }
    return v;
}

// the animated prop sits on the puppet's locator (its bind space is the character space)
static void BpPropOnBody(uint64_t o, const float *loc8)
{
    static __attribute__((aligned(16))) PsyncBf ring[8];
    static unsigned n;
    PsyncBf *bf = &ring[n++ & 7];
    PsyncBfFromLoc(bf, loc8);
    SetObjLoc(o, bf);
}

// THE PROP'S CLIP NAMES (night26 RE): the state graph's prop column says "backpack-intro/loop/outro" (player-inventory2.bin +0x28) but those sids are resolved
// through the player's anim overlay; no such clip is in any table (get-animation-duration read 0.00 s for 2 s on the prop, so animate_ on them cannot work).
// The real clips are in sp-common's anim-abby-player-backpack.pak (already loaded: the own body plays its abby-* craft clips from it) as "<character clip>--backpack"
// (abby-stand^craft-intro--backpack, abby-crouch-craft-idle--backpack, abby-stand^craft-outro--backpack, ...), authored for the backpack-abby skeleton.
// The first candidate the prop's get-animation-duration knows wins (logged once per slot and clip); none known = the first one is tried anyway.
static const char *BpPropClipName(int s, int clip, uint64_t prop)
{
    static char cache[T2R_MAX_PEERS][4][96];
    static char cand[96];
    clip &= 3;
    if (!clip)
        return NULL;
    if (cache[s][clip][0])
        return cache[s][clip];
    static const char *const FB[4][4] =
        { { NULL }, { "abby-stand^craft-intro--backpack",
                     "abby-crouch-craft-intro--backpack", "backpack-intro",
                     NULL },
    { "abby-crouch-craft-idle--backpack",
     "abby-crouch-craft-generic^idle--backpack", "backpack-loop", NULL },
{ "abby-stand^craft-outro--backpack", "abby-crouch-craft-outro--backpack", "backpack-outro", NULL }
    };
    const char *list[6];
    int n = 0;
    static char own[96];
    if (clip == (g_bpp[s].clip & 3) && g_bpp[s].name
        && strlen(g_bpp[s].name) < 80) {
        snprintf(own, sizeof own, "%s--backpack", g_bpp[s].name);
        list[n++] = own;
    }
    for (int i = 0; FB[clip][i]; i++)
        list[n++] = FB[clip][i];
    const char *pick = NULL;
    for (int i = 0; i < n; i++) {
        float L = AnimLen(prop, list[i]);
        Xlog("bp: slot %d prop clip candidate %s: duration %.2f s%s", s,
             list[i], L, L > 0 ? " - USED" : "");
        if (L > 0) {
            pick = list[i];
            break;
        }
    }
    if (!pick) {
        pick = list[0];
        Xlog("bp: slot %d no prop clip candidate has a duration - trying %s anyway", s, pick);
    }
    snprintf(cand, sizeof cand, "%s", pick);
    strcpy(cache[s][clip], cand);
    return cache[s][clip];
}

// prop clip in step with the puppet's craft clip `clip` at phase ph
static void BpPropClip(int s, int clip, float ph, float blend)
{
    if (g_bpp[s].pmode != 1 || !g_bpp[s].prop || !(clip & 3))
        return;
    const char *nm = BpPropClipName(s, clip, g_bpp[s].prop);
    uint64_t r = AnimateFrom(g_bpp[s].prop, nm, blend, ph);
    static int nl;
    if (nl < 40) {
        nl++;
        Xlog("bp: slot %d bag prop %s from phase %.2f blend %.2f -> %llx",
             s, nm, ph, blend, (unsigned long long) r);
    }
}

// the owner took the bag off: the prop first, then the worn parts go (no prop = no change)
// go = 0: get the prop ready (spawned, its clip taking) but keep the worn bag on; go = 1: the swap
static void BpPupBagOff(int s, uint64_t h, double now, int go)
{
    static int bad, abad;
    uint8_t *dc = GvisDc(h);
    if (!dc || bad >= 3)
        return;
    float hp[3], hq[4], lb[16];
    const float *loc = PuppetLocator(h, lb);
    if (!loc)
        return;
    int pool = 0, idx = 0, row = BpPupRow(s, &pool, &idx);
    const char *plook = row ? BpPropLookOf(pool, idx) : NULL;
    uint64_t o = 0;
    int mode = 0;
    if (abad < 3) {
        if (!g_bpp[s].clip)
            return;             // the craft clip has not started here yet: the prop would stand in its bind pose
        if (g_bpp[s].prop) {    // spawned on an earlier tick, hidden, waiting for the clips to take
            o = g_bpp[s].prop;
            // (get-animation-duration is a character-clip query: it read 0.00 s on this prop for 2 s in the night26 logs, so it is no gate. The gate is the clip itself:
            // animate_ answers a live layer and the prop's joints are readable. Until then the prop stays HIDDEN - shown unanimated it stands in its bind pose at shoulder height beside the puppet = a second bag)
            float jp[3] =
                { g_bpp[s].lid0[0], g_bpp[s].lid0[1], g_bpp[s].lid0[2] },
                jq[4];
            uint64_t r = 0;
            int moved = g_bpp[s].ready;
            if (!moved && g_bpp[s].probe_t > 0 && now - g_bpp[s].probe_t >= 66.0 && XbxLoc(o, "lid_a", jp, jq)) {      // the pose of the clip put on 66 ms (4 frames) ago: did a joint move?
                float d =
                    fabsf(jp[0] - g_bpp[s].lid0[0]) + fabsf(jp[1] -
                                                            g_bpp[s].
                                                            lid0[1]) +
                    fabsf(jp[2] - g_bpp[s].lid0[2]);
                moved = d > 0.003f;
                r = 1;
                if (!moved)
                    g_bpp[s].probe_t = 0;
            }
            if (!moved && g_bpp[s].probe_t <= 0) {
                {
                    g_bpp[s].prop = o;
                    r = AnimateFrom(o, BpPropClipName(s, BPC_INTRO, o),
                                     0.0f, 0.5f);
                    g_bpp[s].prop = o;
                }
                if (r && r != ~0ULL && XbxLoc(o, "lid_a", jp, jq)) {
                    memcpy(g_bpp[s].lid0, jp, 12);
                    g_bpp[s].probe_t = now;
                }
            }
            if (!moved && g_bpp[s].path == 0 && now - g_bpp[s].t_proof >= 700.0 && !BpParentOptout()) {       // path A (animate_ on the unparented prop) did not move a joint: path B, the puppet as its parent
                g_bpp[s].path =
                    BpPropParentPuppet(g_bpp[s].proc, h) ? 2 : 1;
                g_bpp[s].probe_t = 0;
                Xlog("bp: slot %d path A (animate_ on the unparented prop) moved nothing in %.0f ms - %s", s, now - g_bpp[s].t_proof, g_bpp[s].path == 2 ? "path B: the prop now has the puppet as its parent (set-parent), probing again" : "path B not possible (no puppet process)");
            }
            if (moved && !go) { // ready before the swap moment: hidden on the body until then
                if (!g_bpp[s].ready)
                    Xlog("bp: slot %d prop ready (its clip took after %.0f ms), waiting for the swap at clip %.0f ms", s, now - g_bpp[s].t_proof, g_bpp[s].enter_pred);
                g_bpp[s].ready = 1;
                BpPropOnBody(o, loc);
                return;
            }
            if (moved) {
                g_bpp[s].ready = 0;
                Xlog("bp: slot %d real prop took its craft clip via path %s after %.0f ms (lid_a moved %.3f m) - shown", s, g_bpp[s].path == 2 ? "B (puppet parent)" : "A (animate_ direct)", now - g_bpp[s].t_proof, (double) (fabsf(jp[0] - g_bpp[s].lid0[0]) + fabsf(jp[1] - g_bpp[s].lid0[1]) + fabsf(jp[2] - g_bpp[s].lid0[2])));
                {               // (bystanders: 'the backpack just disappears'): the stale-process guard dropped EVERY ShowObject ('event c2f7fb66 to a stale prop process
                    // ... dropped (slot 0)') - the spawn-time process pointer is stale by now, so the opened bag was never shown. Resolve the live process from the prop's handle.
                    typedef uint8_t *(*fn_proc2)(uint64_t);
                    uint8_t *pp =
                        ((fn_proc2) T2R_SYM(kFnEntityFromHandle)) (o);
                    if (pp && !IsBadReadPtr(pp, 0xd0)
                        && *(uint64_t *) (pp + 0xc0) == o)
                        g_bpp[s].proc = pp;
                    else
                        Xlog("bp: slot %d prop handle %llx did not resolve to a live process (cached %p)", s, (unsigned long long) o, (void *) g_bpp[s].proc);
                    if (g_bpp[s].proc
                        && !IsBadReadPtr(g_bpp[s].proc, 0xd0))
                        BpSendMsg(g_bpp[s].proc, 0xc2f7fb66d732410bULL);      // ShowObject
                }
                {               // the art the shown prop really wears vs the puppet look's pick
                    uint64_t pa = ObjLook(o), want =
                        plook ? FNV64(plook) : 0;
                    Xlog("bp: slot %d shown prop art %llx (%s), puppet look %s wants %s%s", s, (unsigned long long) pa, LookName(pa), g_flSpawnStr[s], plook ? plook : "backpack-abby", want && pa == want ? " - match" : " - MISMATCH");
                }
                mode = 1;
                g_bpp[s].prop = 0;
                g_bpp[s].probe_t = 0;
            } else if (now - g_bpp[s].t_proof < 2000.0) {
                BpPropOnBody(o, loc);
                return;
            } else {
                Xlog("bp: slot %d the real prop never animated (lid_a did not move) in 2 s (animate_ %llx, try %d) - killed, the bag stays on the back", s, (unsigned long long) r, ++abad);
                DcCall(FNV64("kill-entity"), 1, &o);
                o = 0;
                g_bpp[s].prop = 0;
                g_bpp[s].probe_t = 0;
            }
        } else {
            uint8_t *proc = NULL;
            o = BpSpawnReal(loc, plook ? FNV64(plook) : 0, &proc);    // the puppet look's own TAB prop art (BP_PROP_LOOKS, the pick bp_pick makes for our look), else backpack-abby
            if (o) {
                BpPropUnparent(proc);
                Xlog("bp: slot %d real ProcessBackpack (art %s, parent cleared) spawned hidden: proc %p handle %llx", s, plook ? plook : "backpack-abby", (void *) proc, (unsigned long long) o);
                g_bpp[s].prop = o;
                g_bpp[s].proc = proc;
                g_bpp[s].t_proof = now;
                g_bpp[s].probe_t = 0;
                g_bpp[s].pmode = 0;
                g_bpp[s].path = 0;
                BpPropOnBody(o, loc);
                return;
            } else
                Xlog("bp: slot %d real ProcessBackpack spawn gave no object (try %d) - the bag stays on the back", s, ++abad);
        }
    }
    if (!mode && BpRigidOptin()) {    // (opt-in only, t2r-puppet-bag-rigid.txt: night21 showed the rigid bag floating at hip height)
        if (!XbxLoc(h, "r_hand_prop_attachment", hp, hq))
            return;
        const char *art = row ? BP_HB_ACTOR[pool][idx] : ISP_SAT_ACTOR;
        o = BpSpawnProp(art);
        if (o)
            mode = row ? 2 : 3;
        if (!o) {
            Xlog("bp: slot %d rigid bag prop %s failed (try %d)", s, art,
                 ++bad);
            return;
        }
    }
    if (!mode)
        return;                 // nothing animated: the worn bag stays on the back (old behaviour)
    g_bpp[s].prop = o;
    g_bpp[s].pmode = mode;
    g_bpp[s].floor_ok = 0;
    g_bpp[s].bpoff_st = 0;
    if (mode == 1) {
        BpPropOnBody(o, loc);
        double el = now - g_bpp[s].t0;
        float ph =
            g_bpp[s].dur >
            0 ? (float) (el / (g_bpp[s].dur * 1000.0)) : 0.0f;
        ph -= (float) (int) ph;
        if (ph > 0.98f)
            ph = 0.98f;
        if (ph < 0)
            ph = 0;
        BpPropClip(s, g_bpp[s].clip, ph, 0.0f);
    } else {
        hp[1] -= 0.45f;         // the bag hangs from the hand (origin = underside)
        BpPropPose(o, hp, loc);
    }
    for (int i = 0; i < BP_NPARTS; i++) {
        if (!g_bpPartSid[i])
            g_bpPartSid[i] = FNV64(BP_PARTS[i]);
        int ix = BpDcFind(dc, g_bpPartSid[i]);
        g_bpp[s].was_vis[i] = ix >= 0 && !BpDcHidden(dc, ix);
        if (ix >= 0)
            BpDcHide(dc, g_bpPartSid[i]);
    }
    g_bpPupOff[s] = 1;
    g_bpp[s].off = 1;
    g_bpp[s].t_hide = now;
    g_bpp[s].t_off = now;
    Xlog("bp: slot %d puppet bag OFF at t %.0f ms - worn parts hidden, %s %llx (look %s)", s, now, mode == 1 ? "animated bag (TAB prop look, clips in step)" : mode == 2 ? "rigid per-look bag at the hand" : "satchel at the hand", (unsigned long long) o, g_flSpawnStr[s]);
}

// (the animated bag was offset from where the worn bag was -> a pop): measure at the first clip frame the world delta between the body's spinec (the worn bag
// rides it) and the prop's spinec (root as fallback), log it ("backpack: open offset ..."), and keep it as a constant correction on the prop's locator for the whole clip.
// Clamped to 15 cm. t2r-no-bp-offset-fix.txt = off (measure + log only).
static int BpOfffixOff(void)
{
    static int v = -1;
    if (v < 0) {
        FILE *f = ExeSideOpen("t2r-no-bp-offset-fix.txt", "r");
        v = f != NULL;
        if (f)
            fclose(f);
    }
    return v;
}

static void BpPupOpenOffset(int s, uint64_t h, const float *loc,
                               double now)
{
    float c[16];
    memcpy(c, loc, sizeof c);
    {                           // root track (see bptr_*): prop root = body locator * owner's recorded root-relative locator at the puppet's own clip time, open loop
        int cl = g_bpp[s].clip;
        double ems = now - g_bpp[s].t0;
        if (cl == BPC_LOOP)
            ems = fmod(ems, 7400.0);
        if (cl >= 1 && cl <= 3
            && (g_bpp[s].rsrc_clip != cl
                || g_bpp[s].rsrc_t0 != g_bpp[s].t0)) {
            BptLoadHeader();
            int np = 0, nl = 0, pk =
                BptComplete(g_bptrpOk[s][cl], &np), lk =
                BptComplete(g_bptrOk[cl], &nl);
            g_bpp[s].rsrc = BptOff()? 0 : pk ? 1 : lk ? 2 : 0;
            g_bpp[s].rsrc_clip = cl;
            g_bpp[s].rsrc_t0 = g_bpp[s].t0;
            static int nlog;
            if (nlog < 60) {
                nlog++;
                Xlog("bptrack: slot %d clip %s root source %s (owner's root track %d samples, own %d)", s, BPC_NAME[cl & 3], g_bpp[s].rsrc == 1 ? "OWNER ROOT" : g_bpp[s].rsrc == 2 ? "own root" : "none - spinec track / follow", np, nl);
            }
        }
        float r7[7];
        int got = 0;
        if (cl >= 1 && cl <= 3 && g_bpp[s].rsrc == 1)
            got =
                BptrGetT((const float (*)[7]) g_bptrp[s][cl],
                           g_bptrpOk[s][cl], ems, r7);
        else if (cl >= 1 && cl <= 3 && g_bpp[s].rsrc == 2)
            got =
                BptrGetT((const float (*)[7]) g_bptr[cl], g_bptrOk[cl],
                           ems, r7);
        if (got) {
            float pw[3], q[4];
            Qrotv(loc + 4, r7, pw);
            BpQmul(loc + 4, r7 + 3, q);
            float qn =
                sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] +
                      q[3] * q[3]);
            if (qn > 0.5f) {
                c[0] = loc[0] + pw[0];
                c[1] = loc[1] + pw[1];
                c[2] = loc[2] + pw[2];
                c[3] = 1.0f;
                for (int k = 0; k < 4; k++)
                    c[4 + k] = q[k] / qn;
                {               // proof every 250 ms: the prop spinec (pose of the last frame) vs the owner's recorded spinec path at that clip time, when one is there
                    static double tl[T2R_MAX_PEERS];
                    if (now - tl[s] >= 250.0) {
                        tl[s] = now;
                        float tr[3], pj[3], pq[4], er = -1.0f;
                        int ht =
                            g_bpp[s].rsrc ==
                            1 ? BptGetT((const float (*)[3])
                                          g_bptp[s][cl], g_bptpOk[s][cl],
                                          ems - 16.7, tr) : BptGet(cl,
                                                                    ems -
                                                                    16.7,
                                                                    tr);
                        if (ht && XbxLoc(g_bpp[s].prop, "spinec", pj, pq)) {
                            float yw = 2.0f * atan2f(loc[5], loc[7]), cs =
                                cosf(yw), sn = sinf(yw);
                            float w[3] =
                                { loc[0] + tr[0] * cs + tr[2] * sn,
                           loc[1] + tr[1],
                           loc[2] - tr[0] * sn + tr[2] * cs };
                            er = sqrtf((pj[0] - w[0]) * (pj[0] - w[0]) +
                                       (pj[1] - w[1]) * (pj[1] - w[1]) +
                                       (pj[2] - w[2]) * (pj[2] - w[2]));
                        }
                        Xlog("bptrack: slot %d clip %d at %.0f ms (%s root): root rel %.3f %.3f %.3f, prop spinec off the owner's spinec path %.1f cm", s, cl, ems, g_bpp[s].rsrc == 1 ? "owner" : "own", r7[0], r7[1], r7[2], er * 100.0f);
                    }
                }
                BpPropOnBody(g_bpp[s].prop, c);
                return;
            }
        }
    }
    {                           // replay of our own recorded open/close animation (bpt_*): prop spinec = body locator + R(yaw) * track(clip, clip time)
        float tr[3];
        double ems = now - g_bpp[s].t0;
        if (g_bpp[s].clip == BPC_LOOP)
            ems = fmod(ems, 7400.0);
        int cl = g_bpp[s].clip;
        if (cl >= 1 && cl <= 3
            && (g_bpp[s].tsrc_clip != cl
                || g_bpp[s].tsrc_t0 != g_bpp[s].t0)) {
            BptLoadHeader();
            int np = 0, nl = 0, pk =
                BptComplete(g_bptpOk[s][cl], &np), lk =
                BptComplete(g_bptOk[cl], &nl);
            g_bpp[s].tsrc = BptOff()? 0 : pk ? 1 : lk ? 2 : 0;
            g_bpp[s].tsrc_clip = cl;
            g_bpp[s].tsrc_t0 = g_bpp[s].t0;
            static int nlog;
            if (nlog < 60) {
                nlog++;
                Xlog("bptrack: slot %d clip %s source %s (owner's track %d samples, own %d)", s, BPC_NAME[cl & 3], g_bpp[s].tsrc == 1 ? "OWNER TRACK" : g_bpp[s].tsrc == 2 ? "own track" : "none - spinec follow", np, nl);
            }
        }
        int got = 0;
        if (cl >= 1 && cl <= 3 && g_bpp[s].tsrc == 1)
            got =
                BptGetT((const float (*)[3]) g_bptp[s][cl],
                          g_bptpOk[s][cl], ems, tr);
        else if (cl >= 1 && cl <= 3 && g_bpp[s].tsrc == 2)
            got = BptGet(cl, ems, tr);
        if (got) {
            float yw = 2.0f * atan2f(loc[5], loc[7]), cs = cosf(yw), sn =
                sinf(yw), pj[3], pq[4];
            float w[3] =
                { loc[0] + tr[0] * cs + tr[2] * sn, loc[1] + tr[1],
           loc[2] - tr[0] * sn + tr[2] * cs };
            if (g_bpp[s].last_prop == g_bpp[s].prop
                && XbxLoc(g_bpp[s].prop, "spinec", pj, pq)) {
                for (int k = 0; k < 3; k++)
                    c[k] = g_bpp[s].last_set[k] + w[k] - pj[k];
                float dx = c[0] - loc[0], dy = c[1] - loc[1], dz =
                    c[2] - loc[2], dl = sqrtf(dx * dx + dy * dy + dz * dz);
                if (dl > 1.0f)
                    for (int k = 0; k < 3; k++)
                        c[k] = loc[k] + (c[k] - loc[k]) / dl;
            }
            memcpy(g_bpp[s].last_set, c, 12);
            g_bpp[s].last_prop = g_bpp[s].prop;
            {                   // proof every 250 ms: the target vs where the prop spinec is now (set last frame), in cm
                static double tl[T2R_MAX_PEERS];
                if (now - tl[s] >= 250.0) {
                    tl[s] = now;
                    float pj2[3], pq2[4], er = -1.0f;
                    if (XbxLoc(g_bpp[s].prop, "spinec", pj2, pq2))
                        er = sqrtf((pj2[0] - w[0]) * (pj2[0] - w[0]) +
                                   (pj2[1] - w[1]) * (pj2[1] - w[1]) +
                                   (pj2[2] - w[2]) * (pj2[2] - w[2]));
                    Xlog("bptrack: slot %d clip %d at %.0f ms (%s): prop spinec -> body + track %.3f %.3f %.3f, off target %.1f cm", s, cl, ems, g_bpp[s].tsrc == 1 ? "owner" : "own", tr[0], tr[1], tr[2], er * 100.0f);
                }
            }
            BpPropOnBody(g_bpp[s].prop, c);
            return;
        }
    }
    if (g_bpp[s].bpoff_st == 0 && g_bpp[s].clip) {
        BpPropOnBody(g_bpp[s].prop, loc);    // pose it uncorrected, then read the joints next tick
        g_bpp[s].bpoff_st = -1;
        return;
    }
    if (g_bpp[s].bpoff_st == -1) {
        float bp_[3], bq[4], pp[3], pq[4];
        const char *jn = "spinec";
        int ok = XbxLoc(h, jn, bp_, bq)
            && XbxLoc(g_bpp[s].prop, jn, pp, pq);
        if (!ok) {
            jn = "root";
            ok = XbxLoc(h, jn, bp_, bq)
                && XbxLoc(g_bpp[s].prop, jn, pp, pq);
        }
        if (ok) {
            float d[3] =
                { bp_[0] - pp[0], bp_[1] - pp[1], bp_[2] - pp[2] };
            float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (L > 0.15f) {
                float k = 0.15f / L;
                d[0] *= k;
                d[1] *= k;
                d[2] *= k;
            }
            {
                float yw = 2.0f * atan2f(loc[5], loc[7]), cs = cosf(yw), sn = sinf(yw); // the prop root that puts the prop spinec on the body spinec, stored in the body's yaw frame (= the prop parented to the body joint with a fixed offset)
                float wx = loc[0] + d[0] - bp_[0], wz =
                    loc[2] + d[2] - bp_[2];
                g_bpp[s].bpoff[0] = wx * cs - wz * sn;
                g_bpp[s].bpoff[2] = wx * sn + wz * cs;
                g_bpp[s].bpoff[1] = d[1];
            }
            Xlog("backpack: open offset slot %d joint %s: body %.3f %.3f %.3f prop %.3f %.3f %.3f delta %.3f %.3f %.3f (%.1f cm)%s", s, jn, bp_[0], bp_[1], bp_[2], pp[0], pp[1], pp[2], d[0], d[1], d[2], L * 100.0f, L > 0.15f ? " CLAMPED" : "");
            g_bpp[s].bpoff_st = BpOfffixOff()? 2 : 1;
        } else {
            Xlog("backpack: open offset slot %d unreadable (joint locators) - no correction", s);
            g_bpp[s].bpoff_st = 2;
        }
    }
    if (g_bpp[s].bpoff_st == 1) {
        // ROOT CAUSE night46 (bystander bag moved only up/down): the prop root was pinned to the network locator while the body's crouch lean (spine travel) is not in the bag clip.
        // The engine parents the prop to the body (set-parent -> follows the player); here the prop root is parented to the puppet's spinec joint: root xz = spinec xz + the fixed start offset (body yaw frame),
        // height = locator + the start delta; the clip plays on top. Bound to 0.6 m from the locator.
        float bj[3], bq[4];
        if (XbxLoc(h, "spinec", bj, bq)) {
            float yw = 2.0f * atan2f(loc[5], loc[7]), cs = cosf(yw), sn =
                sinf(yw);
            float x =
                bj[0] + g_bpp[s].bpoff[0] * cs + g_bpp[s].bpoff[2] * sn,
                z =
                bj[2] - g_bpp[s].bpoff[0] * sn + g_bpp[s].bpoff[2] * cs;
            float dx = x - loc[0], dz = z - loc[2], dl =
                sqrtf(dx * dx + dz * dz);
            if (dl > 0.6f) {
                x = loc[0] + dx * 0.6f / dl;
                z = loc[2] + dz * 0.6f / dl;
            }
            c[0] = x;
            c[2] = z;
        }
        c[1] += g_bpp[s].bpoff[1];
    }
    BpPropOnBody(g_bpp[s].prop, c);
}

// every frame while off: animated = on the puppet's locator; rigid = hand (intro / outro) or floor (loop); the hidden parts re-asserted every 200 ms
static void BpPupBagTick(int s, uint64_t h, double now)
{
    float hp[3], hq[4], lb[16];
    const float *loc = PuppetLocator(h, lb);
    if (!loc)
        return;
    if (g_bpp[s].pmode == 1) {
        BpPupOpenOffset(s, h, loc, now);
        if (now - g_bpp[s].t_proof >= 500.0) {  // proof the bag clip plays: expected clip phase + a prop joint that moves with it
            g_bpp[s].t_proof = now;
            float jp[3], jq[4], root[3] = { loc[0], loc[1], loc[2] };
            int okj = XbxLoc(g_bpp[s].prop, "lid_a", jp, jq);
            double el = now - g_bpp[s].t0;
            float dur = g_bpp[s].dur;
            Xlog("bp: slot %d bag prop %s at %.0f / %.0f ms of its clip; lid_a %s %.2f %.2f %.2f (rel. puppet %.2f %.2f %.2f)", s, (g_bpp[s].clip & 3) ? BpPropClipName(s, g_bpp[s].clip, g_bpp[s].prop) : "-", el, (double) dur * 1000.0, okj ? "at" : "unreadable", okj ? jp[0] : 0, okj ? jp[1] : 0, okj ? jp[2] : 0, okj ? jp[0] - root[0] : 0, okj ? jp[1] - root[1] : 0, okj ? jp[2] - root[2] : 0);
        }
    } else if (g_bpp[s].clip == BPC_LOOP) {
        if (!g_bpp[s].floor_ok) {
            if (!XbxLoc(h, "r_hand_prop_attachment", hp, hq))
                return;
            g_bpp[s].floor_pos[0] = hp[0];
            g_bpp[s].floor_pos[2] = hp[2];
            g_bpp[s].floor_pos[1] = loc[1] + 0.004f;
            g_bpp[s].floor_ok = 1;
        }
        BpPropPose(g_bpp[s].prop, g_bpp[s].floor_pos, loc);
    } else {
        g_bpp[s].floor_ok = 0;
        if (XbxLoc(h, "r_hand_prop_attachment", hp, hq)) {
            hp[1] -= 0.45f;
            BpPropPose(g_bpp[s].prop, hp, loc);
        }
    }
    if (now - g_bpp[s].t_hide > 200.0) {
        g_bpp[s].t_hide = now;
        uint8_t *dc = GvisDc(h);
        if (dc)
            for (int i = 0; i < BP_NPARTS; i++) {
                int idx = BpDcFind(dc, g_bpPartSid[i]);
                if (idx >= 0 && !BpDcHidden(dc, idx))
                    BpDcHide(dc, g_bpPartSid[i]);
            }
    }
}

static void BpPupStop(int s, float blend, const char *why)
{
    if (g_bpp[s].clip && g_bpp[s].h && g_bpp[s].name)
        PsyncStop(g_bpp[s].h, FNV64(g_bpp[s].name), blend);
    if (g_bpp[s].clip)
        Xlog("bp: slot %d puppet %s stopped (%s), blend %.2f s at t %.0f ms", s, BPC_NAME[g_bpp[s].clip & 3], why, blend, BpMs());
    g_bpp[s].clip = BPC_NONE;
    g_bpp[s].name = NULL;
}

// start `clip` on the puppet at clip time ct (ms); 0 = nothing playable
static int BpPupPlay(int s, uint64_t h, int clip, double ct, float blend,
                       double now)
{
    const char *nm = NULL;
    float dur = 0;
    SUBT0(al);
    for (int i = 0; i < 4 && BP_CLIPS[clip][i] && dur <= 0; i++)
        if ((dur = AnimLen(h, BP_CLIPS[clip][i])) > 0)
            nm = BP_CLIPS[clip][i];
    SUBT1(al, "bp_net: puppet anim_len lookups");
    if (!nm) {
        if (!g_bpp[s].warned_none++)
            Xlog("bp: slot %d puppet %llx knows no %s craft clip (%s / %s) - no backpack anim on it", s, (unsigned long long) h, BPC_NAME[clip], BP_CLIPS[clip][0], BP_CLIPS[clip][1]);
        return 0;
    }
    if (ct < 0)
        ct = 0;
    float ph = (float) (ct / (dur * 1000.0));
    if (clip == BPC_LOOP)
        ph -= (float) (int) ph;
    if (ph > 0.98f)
        ph = 0.98f;
    uint64_t r;
    SUBP("bp_net: puppet animate_from", r =
         AnimateFrom(h, nm, blend, ph));
    g_bpp[s].clip = clip;
    g_bpp[s].name = nm;
    g_bpp[s].dur = dur;
    g_bpp[s].h = h;
    g_bpp[s].t0 = now - (double) ph *dur * 1000.0;
    if (r && r != ~0ULL)
        BpPropClip(s, clip, ph, blend);
    Xlog("bp: slot %d puppet %s %s (%.2f s) from %.0f ms (phase %.2f), blend %.2f, at t %.0f ms -> %llx; worn bag stays on the puppet", s, BPC_NAME[clip], nm, dur, (double) ph * dur * 1000.0, ph, blend, now, (unsigned long long) r);
    return r && r != ~0ULL;
}

// bring the puppet to what the owner's phase / clip time says (late joiner, lost packets, phase change)
static void BpPupSync(int s, uint64_t h, double now)
{
    double ct = g_bpp[s].ct_rx + (now - g_bpp[s].rx_ms);
    int ph = g_bpp[s].phase;
    if (ph == BPP_OPEN) {
        if (g_bpp[s].clip == BPC_INTRO || g_bpp[s].clip == BPC_LOOP)
            return;
        if (g_bpp[s].clip == BPC_OUTRO) {
            BpPupPlay(s, h, BPC_INTRO, 0, BPP_BLEND, now);
            return;
        }                       // reopened mid-outro
        float di = AnimLen(h, BP_CLIPS[BPC_INTRO][0]);
        if (di <= 0)
            di = AnimLen(h, BP_CLIPS[BPC_INTRO][1]);
        if (di > 0 && ct >= di * 1000.0 - 150.0)
            BpPupPlay(s, h, BPC_LOOP, ct - di * 1000.0, BPP_BLEND, now);      // joined late: already crouched
        else
            BpPupPlay(s, h, BPC_INTRO, ct, BPP_BLEND, now);
    } else if (ph == BPP_CLOSING) {
        if (g_bpp[s].clip == BPC_OUTRO)
            return;
        float d = AnimLen(h, BP_CLIPS[BPC_OUTRO][0]);
        if (d <= 0)
            d = AnimLen(h, BP_CLIPS[BPC_OUTRO][1]);
        if (g_bpp[s].clip == BPC_NONE
            && (d <= 0 || ct >= d * 1000.0 - 250.0))
            return;             // the outro is already over on the owner
        BpPupPlay(s, h, BPC_OUTRO, ct, 0.2f, now);
    } else if (ph == BPP_ABORT) {
        BpPupStop(s, 0.2f, "owner aborted (death / down / match end)");
    } else if (ph == BPP_IDLE
               && (g_bpp[s].clip == BPC_INTRO
                   || g_bpp[s].clip == BPC_LOOP)) {
        BpPupPlay(s, h, BPC_OUTRO, 0, 0.2f, now);     // the closing was lost: stand up from the start of the outro
    }
}

static void BpNetRecv(uint32_t slot, const uint8_t *buf, uint64_t len)
{
    if (slot > T2R_MAX_PEERS - 1 || !buf)
        return;
    if (len >= 6 && buf[0] == BPT_VER) {
        if (slot != *(uint32_t *) T2R_SYM(kMySlot))
            BptNetRecv((int) slot, buf, len);
        return;
    }
    if (len >= 6 && buf[0] == BPTR_VER) {
        if (slot != *(uint32_t *) T2R_SYM(kMySlot))
            BptrNetRecv((int) slot, buf, len);
        return;
    }
    if (len < sizeof(BpMsgT))
        return;
    BpMsgT m;
    memcpy(&m, buf, sizeof m);
    if (m.ver != BP_VER || m.phase > BPP_ABORT)
        return;
    if (slot == *(uint32_t *) T2R_SYM(kMySlot))
        return;
    double now = BpMs();
    int s = (int) slot, changed = m.phase != g_bpp[s].phase
        || m.seq != g_bpp[s].seq;
    float ow = PsyncOneway(s);
    g_bpp[s].rx_ms = now;
    g_bpp[s].ct_rx = (double) m.el_ms + ow;
    if (changed)
        Xlog("bp: slot %d rx phase %s (seq %u, %u ms into it, one-way ~%.0f ms, owner bag %s, swap at enter %d / leave %d ms) at t %.0f ms; puppet %s", s, BPP_NAME[m.phase & 3], m.seq, m.el_ms, ow, m.bagoff ? "off" : "on", m.enter_ms == 0xffff ? -1 : m.enter_ms, m.leave_ms == 0xffff ? -1 : m.leave_ms, now, BPC_NAME[g_bpp[s].clip & 3]);
    g_bpp[s].phase = m.phase;
    g_bpp[s].seq = m.seq;
    g_bpp[s].want_off = m.bagoff != 0;
    if (m.enter_ms != 0xffff && m.enter_ms > 0 && m.enter_ms < 1500)
        g_bpp[s].enter_pred = m.enter_ms;
}

static void BpPupTick(double now)
{
    int live = MatchSubstate() == 6;
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    for (int s = 0; s < T2R_MAX_PEERS; s++) {
        if ((uint32_t) s == me)
            continue;
        if (!g_bpp[s].clip && g_bpp[s].phase == BPP_IDLE && !g_bpp[s].off
            && !g_bpp[s].prop)
            continue;
        uint64_t h = live ? BpPuppet(s) : 0;
        if ((g_bpp[s].clip || g_bpp[s].off) && g_bpp[s].h && g_bpp[s].h != h) { // new body (respawn) / gone: the clip died with the old one; a held prop / hidden worn bag too (no clip between two phases)
            Xlog("bp: slot %d puppet changed %llx -> %llx mid %s - state cleared", s, (unsigned long long) g_bpp[s].h, (unsigned long long) h, BPC_NAME[g_bpp[s].clip & 3]);
            g_bpp[s].clip = BPC_NONE;
            g_bpp[s].name = NULL;
            g_bpp[s].phase = BPP_IDLE;
            g_bpPupOff[s] = 0;
            BpPupPropKill(s, live);
            g_bpp[s].off = 0;
            g_bpp[s].want_off = 0;
            continue;
        }
        if (!h) {
            g_bpp[s].phase = BPP_IDLE;
            g_bpPupOff[s] = 0;
            BpPupPropKill(s, live);
            g_bpp[s].off = 0;
            g_bpp[s].want_off = 0;
            continue;
        }
        // B was in its backpack when A pulled it out; the menu closed at the
        // drag start and here the close played the STANDING craft outro on B's puppet over the drag's victim half (A [21343], layer lost at 257 ms)
        // - B stood through the drag on A's screen. A puppet in a paired move (psync / pull-out) plays no craft clip: the pair owns its body.
        const char *clr =
            !live ? "match end" : g_peerDowned[s] ? "downed" :
            PsyncDead(h) ? "dead" : (PsyncBusySlot((uint32_t) s)
                                      || PulPuppetBusy((uint32_t) s)) ?
            "in a paired move" : NULL;
        if (clr) {
            BpPupStop(s, 0.2f, clr);
            BpPupBagOn(s, h, live, clr);
            g_bpp[s].phase = BPP_IDLE;
            g_bpp[s].want_off = 0;
            continue;
        }
        if ((g_bpp[s].phase == BPP_OPEN || g_bpp[s].phase == BPP_CLOSING)
            && now - g_bpp[s].rx_ms > 2500.0) {
            Xlog("bp: slot %d no backpack message for %.0f ms while %s - taken as closed", s, now - g_bpp[s].rx_ms, BPP_NAME[g_bpp[s].phase]);
            g_bpp[s].phase = BPP_IDLE;
            g_bpp[s].want_off = 0;
        }
        BpPupSync(s, h, now);
        if (!BpPupOffDisabled()) {
            // A took B's bag off 329 ms after the open (B: 133 ms) - it waited for B's bag-off message, then spawned the prop and probed its
            // clip for 150 ms with the worn bag still on. Now the prop is readied as soon as the intro starts here and the swap happens at the owner's own swap clip time
            // (enter ms of its last open, 135 ms before one is known) on the puppet's clip clock, or at the owner's bag-off message if that comes first.
            if (g_bpp[s].enter_pred <= 0)
                g_bpp[s].enter_pred = 135.0;
            int early = g_bpp[s].phase == BPP_OPEN
                && g_bpp[s].clip == BPC_INTRO;
            int go = g_bpp[s].want_off || (early
                                           && now - g_bpp[s].t0 >=
                                           g_bpp[s].enter_pred);
            if ((g_bpp[s].want_off || early) && !g_bpp[s].off)
                BpPupBagOff(s, h, now, go);
            else if (!g_bpp[s].off && g_bpp[s].prop && !g_bpp[s].want_off) {
                Xlog("bp: slot %d readied prop not used (no swap in the intro) - removed", s);
                BpPupPropKill(s, live);
            } else if (!g_bpp[s].want_off && g_bpp[s].off
                       && !(g_bpp[s].phase == BPP_OPEN
                            && now - g_bpp[s].t_off < 800.0))
                BpPupBagOn(s, h, live, "owner's bag back on");
            if (g_bpp[s].off && g_bpp[s].prop)
                BpPupBagTick(s, h, now);
        }
        if (!g_bpp[s].clip)
            continue;
        double el = now - g_bpp[s].t0, len = g_bpp[s].dur * 1000.0;
        if (g_bpp[s].clip == BPC_INTRO && el >= len - 150.0) {
            if (g_bpp[s].phase == BPP_OPEN)
                BpPupPlay(s, h, BPC_LOOP, 0, 0.2f, now);
        } else if (g_bpp[s].clip == BPC_LOOP && el >= len - 150.0) {
            BpPupPlay(s, h, BPC_LOOP, 0, 0.15f, now);
        } else if (g_bpp[s].clip == BPC_OUTRO && el >= len - 250.0) {
            BpPupStop(s, 0.25f, "outro done");
            if (g_bpp[s].phase == BPP_CLOSING)
                g_bpp[s].phase = BPP_IDLE;
        }
    }
}

static void BpNetTick(void)
{
    double now = BpMs();
    SUBP("bp_net: own tick", BpOwnTick(now));
    SUBP("bp_net: puppets tick", BpPupTick(now));
}
