// ---- execution sound design plus (plenty of audio during special executions, so they feel grounded and
// physical: footstep audio, whooshes, clothing, character reactions, etc.; the punches of the unarmed execution were fine, everything else was missing).
// Included by t2r-ext.c right before ex_tick (it uses g_ex, the PCM mixer, the vox helpers).
//
// What: for every gun / melee execution pair a table of 50-110 layered, physically timed sounds (execplus_tl.h, tools: re/factions2013/t1anim/gen_execplus.py read
// from the 2013 clips: foot plants, limb swing speed peaks, hand / knee contacts on the victim, ground contacts of pelvis / head / knees / hands, slides, weapon
// handling) played from TLOU2's own SP fight / foley banks (execplus.pcm, tools/rip_t2_execplus.py; SP sounds are silent in MP unless their bank is loaded, so the
// waves are ripped) and TLOU2's vox events (breaths, strain, pain, choke, whimper: the `say` call, a female speaker gets her own take). Footsteps are
// footsteps.pcm's MLB tiers (the ext does not know the surface under a body: one neutral stone / dirt step, see footsteps.c) plus a sneaker heel / toe sweetener.
//
// The rows run in ex_tick on the same move clock the 2013 timeline uses (psync_ms), so the attacker, the victim and every observer play the same layers at the same
// move time, each on its own screen, 3D at the joint (get-joint-locator -> the mixer's world-position voice; the body's live position when the joint is not known).
// Level: row gain x layer base (execplus_layers.py) x the lead master (t2r-melee-sfx-gain.txt, 45 %) x t2r-exec-foley-plus-gain.txt (percent of the default, re-read every 2 s; the default is -3 dB).
// The generator ducked every row that sits within 90 ms of a punch / kick / blow, 120 ms of a gunshot or 90 ms of a body impact of the 2013 timeline, so the lead
// sounds stay on top. Mixer budget: MIX_VOICES = 32; a priority 2 layer (gear, cloth, blood, thumps) is dropped when 16 voices play, priority 1 at 22, priority 0 at 27.
// A move that is cut (gx_abort / psync abort) stops its layers at once (ep_cut); a row that is more than 250 ms late is never played (a late start does not replay old
// sounds); the first row of a lagging start plays from the middle of its wave. Opt-out: t2r-no-exec-foley-plus.txt next to the exe (re-read every 2 s).
// Log: one "exec+:" line per move (rows scheduled / played, what was skipped and why).
static int ShivkitActive(void);        // shivkit.c
static int IntfoleyGate(int kind, int idx);    // intfoley.c: the Interrogation pairs' own switch (t2r-no-int-foley.txt)
static int IntfoleyVicLine(double ems);       // intfoley.c: a victim line of intvox.c is playing (its foley is ducked)
static int IntfoleyVoxClash(double ems);      // intfoley.c: exec+ voice rows yield to intvox.c's lines
#include "execplus.h"
#include "execplus_tl.h"
#include "footsteps.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl ep_blob\nep_blob:\n.incbin \"execplus.pcm\"\n.globl ep_blob_end\nep_blob_end:\n.text\n");
extern const int16_t ep_blob[], ep_blob_end[];
extern const int16_t fs_blob[]; // footsteps.c: footsteps.pcm
#define EP_JOINT_COUNT EPJ_N

static int EpOff(void)
{
    static int off = -1;
    static uint64_t last;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - last > 2000) {
        FILE *f = ExeSideOpen("t2r-no-exec-foley-plus.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
        last = now;
    }
    return off;
}

static float EpMaster(void)
{                               // lead master (t2r-melee-sfx-gain.txt, default 45 %) x the plus master (t2r-exec-foley-plus-gain.txt, default 100 %)
    static float v = 0.45f;
    static uint64_t last;
    uint64_t now = GetTickCount64();
    if (now - last > 2000) {
        last = now;
        float lead = 0.45f, plus = 0.70f;       // plus 0.70 = -3 dB: the summed layers peaked at the level of a lead punch in the offline mix (preview of 3 moves, peak 0.33-0.36 vs 0.40)
        FILE *f = ExeSideOpen("t2r-melee-sfx-gain.txt", "r");
        if (f) {
            int pc;
            if (fscanf(f, "%d", &pc) == 1 && pc >= 0 && pc <= 400)
                lead = pc / 100.0f;
            fclose(f);
        }
        f = ExeSideOpen("t2r-exec-foley-plus-gain.txt", "r");
        if (f) {
            int pc;
            if (fscanf(f, "%d", &pc) == 1 && pc >= 0 && pc <= 400)
                plus = 0.70f * pc / 100.0f;
            fclose(f);
        }                       // percent of the default
        v = lead * plus;
    }
    return v;
}

static int EpVoicesBusy(void)
{
    int n = 0;
    for (int v = 0; v < MIX_VOICES; v++)
        if (g_mixv[v].on)
            n++;
    return n;
}

// where a row's joint is (TLOU2 skeleton names; WEAPON = the executioner's gun hand)
static const char *const EP_JOINT_NAME[EPJ_N] =
    { "root", "headb", "spined", "pelvis", "l_palm", "r_palm", "l_ankle",
"r_ankle", "l_knee", "r_knee", "r_palm" };
// voice event pools (all names are in sidbase.bin; an event the speaker's bank does not have returns ~0 and is skipped for good)
// (the pools start with the events the contextual takedown clips themselves use - ctx_vox_events.h, A/B-proven to resolve in MP - then the other fight-set events)
static const char *const EPV_ATT_BREATH[] =
    { "vox-eff-gen-meatshield-grab-struggle-combat",
"vox-eff-gen-brth-fight-med", "vox-eff-gen-breath-fight-med", "vox-eff-gen-fight-give-grab" };
static const char *const EPV_ATT_EFFORT[] =
    { "vox-eff-gen-fight-give-grab", "vox-eff-gen-fight-give-hvy-short",
"vox-eff-gen-effort-sml", "vox-eff-gen-fight-give-sml" };
static const char *const EPV_ATT_RELIEF[] =
    { "vox-eff-gen-breath-relief-sigh",
"vox-eff-gen-breath-exhausted-recover", "vox-eff-gen-fight-give-exhstd", "vox-eff-gen-breath-fight-med" };
static const char *const EPV_VIC_PAIN[] =
    { "vox-eff-gen-fight-take-face", "vox-eff-gen-fight-take-med-blnt",
"vox-eff-gen-fight-take-med-blnt-gut", "vox-eff-gen-fight-take-sml-shrp" };
static const char *const EPV_VIC_STRUGGLE[] =
    { "vox-eff-gen-meatshield-grabbed-combat",
"vox-eff-gen-meatshield-grabbed-struggle-combat", "vox-eff-gen-fight-struggle-med",
"vox-eff-gen-choke-take-struggle" };
static const char *const EPV_VIC_CHOKE[] =
    { "vox-eff-gen-death-gurgle-blood-sml",
"vox-eff-gen-death-gurgle-rattle", "vox-eff-gen-death-gurgle-blood-lrg", "vox-eff-gen-cough-hvy" };
static const char *const EPV_VIC_FALL[] =
    { "vox-eff-gen-fight-take-hit-ground",
"vox-eff-gen-fight-take-med-blnt-gut", "vox-eff-gen-fight-take-knockdown",
"vox-eff-gen-fight-take-stunned" };
static const char *const EPV_VIC_WHIMPER[] =
    { "vox-eff-gen-whimper-light", "vox-eff-gen-begs-for-life-stun",
"vox-eff-gen-whimper-heavy", "vox-eff-gen-meatshield-grabbed-combat" };
static const struct {
    int layer;
    const char *const *ev;
    int n;
    int vxe;
} EP_VOX[] = {
    { EPL_VOX_ATT_BREATH, EPV_ATT_BREATH, 4, 0 }, { EPL_VOX_ATT_EFFORT,
                                                   EPV_ATT_EFFORT, 4, 0 },
        { EPL_VOX_ATT_RELIEF, EPV_ATT_RELIEF, 4, 0 },
    { EPL_VOX_VIC_PAIN, EPV_VIC_PAIN, 4, 1 }, { EPL_VOX_VIC_STRUGGLE,
                                               EPV_VIC_STRUGGLE, 4, 1 },
        { EPL_VOX_VIC_CHOKE, EPV_VIC_CHOKE, 4, 1 },
    { EPL_VOX_VIC_FALL, EPV_VIC_FALL, 4, 1 }, { EPL_VOX_VIC_WHIMPER,
                                               EPV_VIC_WHIMPER, 4, 1 }
};

static uint8_t g_epVoxBad[EPL_N][4];  // event never answered for this speaker class: not tried again this session
static uint64_t g_epVicVoxMs;
static int g_epVicDying;      // the shiv kit's victim reached its choke / gurgle: no more pains, struggles or whimpers

static int EpVox(int layer, uint64_t ent, int who)
{
    for (size_t i = 0; i < sizeof EP_VOX / sizeof EP_VOX[0]; i++) {
        if (EP_VOX[i].layer != layer)
            continue;
        uint64_t now = GetTickCount64();
        if (who == 0) {
            if (g_attVoxMs && now - g_attVoxMs < 600)
                return -1;
        } else if (g_epVicVoxMs && now - g_epVicVoxMs < 600)
            return -1;
        if (who == 1 && g_epVicDying && layer != EPL_VOX_VIC_CHOKE)
            return -1;
        if (who == 1 && EbCovers())
            return -1;          // exec_beg.c: the plea / grunts / silence hold the victim's voice
        static unsigned rr;
        for (int t = 0; t < EP_VOX[i].n; t++) {
            int k = (int) ((rr + (unsigned) t) % (unsigned) EP_VOX[i].n);
            if (g_epVoxBad[layer][k])
                continue;
            uint64_t a[4] = { ent, Fnv64Rt(EP_VOX[i].ev[k]), 2, 0 };
            uint64_t fs = FemEffortSid(ent, EP_VOX[i].vxe ? VXE_PAIN : VXE_GIVE);     // a female speaker: her own effort / pain take
            if (fs)
                a[1] = fs;
            uint64_t r = DcCall(FNV64("say"), 4, a);
            if (r == ~0ULL) {
                g_epVoxBad[layer][k] = 1;
                continue;
            }
            rr += 1 + (unsigned) (now & 3);
            if (who == 0)
                g_attVoxMs = now;
            else
                g_epVicVoxMs = now;
            static int nl;
            if (LOGCAP(nl, 60))
                Xlog("exec+: voice %s on the %s -> %llx", EP_VOX[i].ev[k],
                     who ? "victim" : "attacker", (unsigned long long) r);
            return 1;
        }
        return 0;
    }
    return 0;
}

enum { EPR_PLAYED = 1, EPR_VOICE = -1, EPR_PRIO = -2, EPR_LATE =
        -3, EPR_NOSRC = -4, EPR_MIXER = -5, EPR_VOX = -6, EPR_BODY = -7 };

// one sound of a layer on a body: the core of a timeline row and of the helpers of the other melee systems (ctxdown takedowns, standing finishers, downed
// melee strikes). ent = the body (0 = unknown: wpos, if any, is the place), g01 = 0..1 row gain, lateness = ms the moment has already passed (the wave starts that far in),
// mat = cloth material of that body, level = the move's distance gain (1 for the two bodies). Returns EPR_*.
static int EpFire(int layer, int who, int joint, float g01,
                   double lateness, uint64_t ent, const float *wpos,
                   int mat, float level)
{
    if (lateness > 250.0)
        return EPR_LATE;
    if (lateness < 0.0)
        lateness = 0.0;
    if (EP_LAYER[layer].src < 0) {
        if (!ent || ent == ~0ULL)
            return EPR_BODY;
        if (lateness > 120.0)
            return EPR_LATE;
        int r = EpVox(layer, ent, who);
        return r > 0 ? EPR_PLAYED : EPR_VOX;
    }
    int busy = EpVoicesBusy(), prio = EP_LAYER[layer].prio;
    if (busy >= 27)
        return EPR_VOICE;
    if ((prio >= 1 && busy >= 22) || (prio >= 2 && busy >= 16))
        return EPR_PRIO;
    if ((!ent || ent == ~0ULL) && !wpos)
        return EPR_BODY;
    if (ent == ~0ULL)
        ent = 0;
    static unsigned rr[EPL_N];
    const int src = EP_LAYER[layer].src;
    const int16_t *pcm;
    int n, rate;
    if (src > 0) {              // footsteps.pcm tier (src - 1)
        int tier = src - 1;
        unsigned w =
            FS_TIER[tier].first + (rr[layer]++ * 5u +
                                   (unsigned) (GetTickCount64() >> 3)) %
            FS_TIER[tier].count;
        pcm = fs_blob + FS_WAVE[w].off;
        n = (int) FS_WAVE[w].n;
        rate = FS_RATE;
    } else {
        unsigned nv = EP_LAYER[layer].nv, mats = EP_LAYER[layer].mats;
        unsigned m = mats > 1 ? (unsigned) mat % mats : 0;
        unsigned v =
            (rr[layer]++ * 3u + (unsigned) (GetTickCount64() >> 3)) % nv;
        unsigned w = EP_LAYER[layer].first + m * nv + v;
        pcm = ep_blob + EP_WAVE[w].off;
        n = (int) EP_WAVE[w].n;
        rate = EP_WAVE[w].rate;
    }
    int skip = (int) (lateness * (double) rate / 1000.0);
    if (skip > n - 256)
        return EPR_LATE;
    pcm += skip;
    n -= skip;
    float gain =
        g01 * (float) EP_LAYER[layer].base / 100.0f * EpMaster() * level;
    float pos[3];
    int ok;
    static uint8_t jfail[EPJ_N];
    static uint8_t jok[EPJ_N];  // a joint name the puppet skeleton never answers is not asked again (6 misses, no hit)
    if (ent && ent != SID_PLAYER && joint >= 0 && joint < EPJ_N
        && (jok[joint] || jfail[joint] < 6)) {
        if (GxJointPos(ent, EP_JOINT_NAME[joint], pos)) {
            jok[joint] = 1;
            ok = PcmPlayAt2(pcm, n, gain, rate, 0, pos);
        }                       // a puppet's joint: a world-position voice there
        else {
            if (!jok[joint])
                jfail[joint]++;
            ok = PcmPlayAt2(pcm, n, gain, rate, ent, wpos);
        }
    } else
        ok = PcmPlayAt2(pcm, n, gain, rate, ent, wpos);       // our own body / unknown joint: the body (live position)
    return ok ? EPR_PLAYED : EPR_MIXER;
}

static int EpRow(size_t ri, double ems)
{
    if (g_ex.kind == 2 && EP_LAYER[EXPLUS_TL[ri].layer].src < 0
        && IntfoleyVoxClash(ems))
        return EPR_VOX;         // an Interrogation line is speaking / due (intfoley.c)
    // the unarmed execution had clapping sounds at the start, on both screens, from the more
    // immersive waves: the FALL_HAND layer (a bracing hand / forearm slap) fires 3-4 times in the first second of the fist moves (567 / 633 /
    // 800 / 1167 ms of fist-execute-enter-front) and the slaps read as clapping - the layer is off (t2r-explus-hand-slaps.txt = back on)
    if (EXPLUS_TL[ri].layer == EPL_FALL_HAND) {
        static int on = -1;
        if (on < 0) {
            FILE *f = ExeSideOpen("t2r-explus-hand-slaps.txt", "r");
            on = f != NULL;
            if (f)
                fclose(f);
        }
        if (!on)
            return EPR_PLAYED;
    }
    // the unarmed 3 punch special exec started with a rhythmic rubbing sound: before the first punch (1300 ms) the two
    // fist moves fired ~25 arm / leg whooshes, cloth rubs and pack rattles every 50-200 ms - a fast rhythmic rubbing. t2r-explus-fist-rub.txt = all rows back on.
    if (g_ex.kind == 2 && (g_ex.pair == 0 || g_ex.pair == 1)
        && EXPLUS_TL[ri].ms < 1300) {
        int l = EXPLUS_TL[ri].layer;
        // (the slapping was still audible, like quick slaps: the body hits / knee and body falls / grab release
        // (HIT_BODY 233 + 833 ms, FALL_KNEE 933-1333 ms) landed next to the 2013 recording's own grab transients (180 / 280 / 790 ms) as
        // quick slaps - they are out of the window too; only the grab, the struggle and the vox remain before the first punch)
        if ((l >= EPL_SCUFF && l <= EPL_CL_L)
            || (l >= EPL_GR_RELEASE && l <= EPL_GEAR_STRAP
                && l != EPL_STRUG_TORSO && l != EPL_STRUG_HEAD)
            || l == EPL_GEAR_HOLSTER || l == EPL_GEAR_MELEE) {
            static int on = -1;
            if (on < 0) {
                FILE *f = ExeSideOpen("t2r-explus-fist-rub.txt", "r");
                on = f != NULL;
                if (f)
                    fclose(f);
            }
            if (!on)
                return EPR_PLAYED;
        }
    }
    // the unarmed head kick smash special execution had 2 clap-sounding sfx: at the head stomp of the two fist-stomp moves (2233 ms)
    // the extra-foley layer stacked FALL_HEAD (a nose-finisher bodyfall + hair-rustle wave, 13 ms rise) and BONE (a dry 4-5 kHz bone-single crack + an
    // elbow-skin-bone slap, "under heavy weapon / kill blows, not under punches") within 8 ms on top of the TLOU2 boot-head hit, the 2013 recording's own
    // stomp (onset 2224 ms) and the body hit: two sharp bright transients that read as claps. Both are out of the stomp moves; the boot-head hit,
    // the 2013 stomp recording, THUMP, the body fall and the vox stay. t2r-explus-stomp-claps.txt = back on.
    if (g_ex.kind == 2 && (g_ex.pair == 2 || g_ex.pair == 3)
        && (EXPLUS_TL[ri].layer == EPL_FALL_HEAD
            || EXPLUS_TL[ri].layer == EPL_BONE)) {
        static int on = -1;
        if (on < 0) {
            FILE *f = ExeSideOpen("t2r-explus-stomp-claps.txt", "r");
            on = f != NULL;
            if (f)
                fclose(f);
        }
        if (!on)
            return EPR_PLAYED;
    }
    int who = EXPLUS_TL[ri].who;
    float gn = (float) EXPLUS_TL[ri].gain / 255.0f;
    if (g_ex.kind == 2 && IntfoleyVicLine(ems))
        gn *= 0.5f;             // an Interrogation victim line is speaking - the cloth / grip / scuff rows sit under it (intfoley.c)
    // shiv executions sounded like slashes: the arm whooshes of a stabbing move are the swish the kit's thunks sit under - duck them while the SP kit plays
    if (g_ex.kind == 2 && EXPLUS_TL[ri].layer >= EPL_WH_ARM_S
        && EXPLUS_TL[ri].layer <= EPL_WH_ARM_L && ShivkitActive())
        gn *= 0.30f;
    return EpFire(EXPLUS_TL[ri].layer, who, EXPLUS_TL[ri].joint, gn,
                   ems - (double) EXPLUS_TL[ri].ms,
                   who ? g_ex.vic_ent : g_ex.att_ent, EXPOS,
                   g_ex.ep_mat[who ? 1 : 0], g_ex.gain);
}

static void EpSummary(const char *why)
{
    static int nl;
    if (!g_ex.ep_on)
        return;
    if (LOGCAP(nl, 300))
        Xlog("exec+: %s %s (%s): %d rows scheduled, %d played - skipped: %d mixer refused, %d low-priority budget, %d late, %d voice, %d no body, %d all voices busy - %s", g_ex.kind == 1 ? "gun" : "melee", g_ex.kind == 1 ? GUNEXEC_PAIRS[g_ex.pair].name : MELEE2013[g_ex.pair].name, g_ex.ep_att_mat ? "WLF cloth" : "Seraphite cloth", g_ex.ep_n, g_ex.ep_played, g_ex.ep_skip[0], g_ex.ep_skip[1], g_ex.ep_skip[2], g_ex.ep_skip[3], g_ex.ep_skip[4], g_ex.ep_skip[5], why);
    g_ex.ep_on = 0;
}

// cloth material of a body: the faction's (WLF: leather / military, Seraphites: cotton / parka); unknown -> cotton
static int EpMaterial(uint64_t ent)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    if (!ent || ent == ~0ULL)
        return 0;
    int fac = -1;
    if (ent == SID_PLAYER)
        fac = VxOwnFac();
    else {
        for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
            uint8_t *pe = ((fn_peer) T2rPeer) (s);
            if (pe && pe[4] && *(uint64_t *) (pe + 0x60) == ent) {
                const char *why;
                fac = VxFacOfSlot(s, &why);
                break;
            }
        }
    }
    return fac == 1 ? 1 : 0;
}

// one-time consistency check of the generated tables (a bad rip / table pairing plays nothing instead of garbage)
static int EpTablesOk(void)
{
    static int ok = -1;
    if (ok >= 0)
        return ok;
    size_t total = (size_t) (ep_blob_end - ep_blob);
    ok = 1;
    for (size_t w = 0; w < sizeof EP_WAVE / sizeof EP_WAVE[0]; w++)
        if ((size_t) EP_WAVE[w].off + EP_WAVE[w].n > total
            || EP_WAVE[w].rate < 8000)
            ok = 0;
    for (int l = 0; l < EPL_N; l++) {
        if (EP_LAYER[l].src == 0
            && (!EP_LAYER[l].nv
                || (size_t) EP_LAYER[l].first +
                (size_t) EP_LAYER[l].nv * EP_LAYER[l].mats >
                sizeof EP_WAVE / sizeof EP_WAVE[0]))
            ok = 0;
        if (EP_LAYER[l].src > 0 && EP_LAYER[l].src - 1 >= FS_TIERS)
            ok = 0;
    }
    int prev_k = -1, prev_p = -1, prev_ms = -100000;
    for (size_t i = 0; i < EXPLUS_N; i++) {
        if (EXPLUS_TL[i].layer >= EPL_N || EXPLUS_TL[i].joint >= EPJ_N)
            ok = 0;
        if (EXPLUS_TL[i].kind == prev_k && EXPLUS_TL[i].pair == prev_p
            && EXPLUS_TL[i].ms < prev_ms)
            ok = 0;             // rows of a pair must run in time order
        prev_k = EXPLUS_TL[i].kind;
        prev_p = EXPLUS_TL[i].pair;
        prev_ms = EXPLUS_TL[i].ms;
    }
    Xlog("exec+: tables %s - %u rows, %u waves, %u KB of layer audio (opt-out t2r-no-exec-foley-plus.txt)", ok ? "ok" : "INCONSISTENT - layers off", (unsigned) EXPLUS_N, (unsigned) (sizeof EP_WAVE / sizeof EP_WAVE[0]), (unsigned) (total * 2 / 1024));
    return ok;
}

static void EpBegin(int kind, int idx)
{                               // kind 0 gun / 1 melee (ex_begin's)
    g_ex.ep_on = 0;
    if (EpOff() || !EpTablesOk())
        return;
    const char *want =
        kind == 0 ? ((size_t) idx <
                     GX_N ? GUNEXEC_PAIRS[idx].
                     name : NULL) : ((size_t) idx <
                                     MX_N ? MELEE2013[idx].name : NULL);
    const char *have = NULL;
    if (kind == 0
        && (size_t) idx <
        sizeof EXPLUS_GUN_NAMES / sizeof EXPLUS_GUN_NAMES[0])
        have = EXPLUS_GUN_NAMES[idx];
    if (kind == 1
        && (size_t) idx <
        sizeof EXPLUS_MELEE_NAMES / sizeof EXPLUS_MELEE_NAMES[0])
        have = EXPLUS_MELEE_NAMES[idx];
    if (!want || !have || strcmp(want, have)) {
        Xlog("exec+: pair %d is %s but the table was made for %s - off (regenerate execplus_tl.h)", idx, want ? want : "?", have ? have : "?");
        return;
    }
    size_t lo = 0, hi = 0;
    for (size_t i = 0; i < EXPLUS_N; i++) {
        if (EXPLUS_TL[i].kind == kind && EXPLUS_TL[i].pair == idx) {
            if (!hi)
                lo = i;
            hi = i + 1;
        } else if (hi)
            break;
    }
    if (!hi)
        return;
    if (IntfoleyGate(kind, idx))
        return;
    g_ex.ep_on = 1;
    g_ex.ep_i = (int) lo;
    g_ex.ep_end = (int) hi;
    g_ex.ep_n = (int) (hi - lo);
    g_ex.ep_played = 0;
    memset(g_ex.ep_skip, 0, sizeof g_ex.ep_skip);
    g_ex.ep_mat[0] = (uint8_t) EpMaterial(g_ex.att_ent);
    g_ex.ep_mat[1] = (uint8_t) EpMaterial(g_ex.vic_ent);
    g_ex.ep_att_mat = g_ex.ep_mat[0];
}

static void EpTick(double ems)
{
    if (!g_ex.ep_on)
        return;
    while (g_ex.ep_i < g_ex.ep_end
           && (double) EXPLUS_TL[g_ex.ep_i].ms <= ems) {
        int r = EpRow((size_t) g_ex.ep_i, ems);
        if (r == EPR_PLAYED)
            g_ex.ep_played++;
        else if (r == EPR_MIXER)
            g_ex.ep_skip[0]++;
        else if (r == EPR_PRIO)
            g_ex.ep_skip[1]++;
        else if (r == EPR_LATE)
            g_ex.ep_skip[2]++;
        else if (r == EPR_VOX)
            g_ex.ep_skip[3]++;
        else if (r == EPR_BODY)
            g_ex.ep_skip[4]++;
        else
            g_ex.ep_skip[5]++;
        g_ex.ep_i++;
    }
    if (g_ex.ep_i >= g_ex.ep_end)
        EpSummary("done");
}

// ---- the other melee systems get the same layers, from recipes (they have no 2013 clip to read: the TLOU2 clips' times are the systems' own events)
//   ctxdown takedowns  per act of cd_sfx_arm's event list (grab, windup, blows, head into the surface, the fall, the kick) on the move clock, both bodies known
//   standing finishers per label of fin_sfx_begin's fractions, the attacker side only (the victim is not a known body there)
//   downed melee       per cue of dm_snd_tick (effort, swing, contact, moan)
// delayed rows wait in a small queue (ep_q_tick from the top of ex_tick). Opt-out and gains: the same files as the timeline.
enum { EPC_GRAB, EPC_GRUNT, EPC_HIT, EPC_HEAVY, EPC_THUD, EPC_FALL, EPC_WHOOSH, EPC_PAIN, EPC_CRY, EPC_KICK };  // = ctxdown.c CE_* (static-asserted there)
typedef struct {
    int16_t dly;
    uint8_t layer, who, joint, g;
} EpRec;                       // g: gain 0..100 (percent of a full row), who 0 attacker / 1 victim
static struct {
    double due;
    uint8_t layer, who, joint, g, mat[2];
    uint64_t ent[2];
} g_epq[24];
static void EpQueue(const EpRec *r, uint64_t att, uint64_t vic, int matA,
                     int matV)
{
    uint64_t ent = r->who ? vic : att;
    if (!ent || ent == ~0ULL)
        return;
    if (r->dly <= 1) {
        EpFire(r->layer, r->who, r->joint, r->g / 100.0f, 0.0, ent, NULL,
                r->who ? matV : matA, 1.0f);
        return;
    }
    int free_i = -1;
    for (int i = 0; i < 24; i++)
        if (!g_epq[i].due) {
            free_i = i;
            break;
        }
    if (free_i < 0)
        return;
    g_epq[free_i].due = (double) GetTickCount64() + r->dly;
    g_epq[free_i].layer = r->layer;
    g_epq[free_i].who = r->who;
    g_epq[free_i].joint = r->joint;
    g_epq[free_i].g = r->g;
    g_epq[free_i].ent[0] = att;
    g_epq[free_i].ent[1] = vic;
    g_epq[free_i].mat[0] = (uint8_t) matA;
    g_epq[free_i].mat[1] = (uint8_t) matV;
}

static void EpQTick(void)
{
    double now = (double) GetTickCount64();
    for (int i = 0; i < 24; i++) {
        if (!g_epq[i].due || now < g_epq[i].due)
            continue;
        double late = now - g_epq[i].due;
        uint64_t ent = g_epq[i].ent[g_epq[i].who];
        uint8_t l = g_epq[i].layer, w = g_epq[i].who, j =
            g_epq[i].joint, g = g_epq[i].g, m = g_epq[i].mat[w];
        g_epq[i].due = 0;
        if (!EpOff())
            EpFire(l, w, j, g / 100.0f, late, ent, NULL, m, 1.0f);
    }
}

static void EpPlayRecipe(const EpRec *rec, int n, uint64_t att,
                           uint64_t vic)
{
    if (EpOff())
        return;
    int ma = EpMaterial(att), mv = EpMaterial(vic);
    for (int i = 0; i < n; i++)
        EpQueue(&rec[i], att, vic, ma, mv);
}

#define ER(d, l, w, j, g) {(d), EPL_##l, (w), EPJ_##j, (g)}
static const EpRec EPR_C_GRAB[] =
    { ER(0, GR_TORSO, 0, R_HAND, 75), ER(20, CL_M, 1, SPINE, 60), ER(0,
                                                                     STEP_M,
                                                                     0,
                                                                     R_FOOT,
                                                                     55),
ER(60, GEAR_PACK, 0, SPINE, 45) };
static const EpRec EPR_C_GRUNT[] =
    { ER(0, STEP_M, 0, L_FOOT, 50), ER(100, CL_M, 0, SPINE, 45) };
static const EpRec EPR_C_HIT[] =
    { ER(0, CL_L, 0, SPINE, 55), ER(30, CL_M, 1, SPINE, 50), ER(0,
                                                                GEAR_PACK,
                                                                0, SPINE,
                                                                40) };
static const EpRec EPR_C_HEAVY[] =
    { ER(0, CL_L, 0, SPINE, 70), ER(40, GEAR_PACK, 0, SPINE, 50), ER(50,
                                                                     CL_L,
                                                                     1,
                                                                     SPINE,
                                                                     60) };
static const EpRec EPR_C_HEAVY_W[] = { ER(0, THUMP, 1, SPINE, 55), ER(5, BONE, 1, HEAD, 55) }; // weapon finishers only: never under a punch
static const EpRec EPR_C_THUD[] =
    { ER(0, FALL_HEAD, 1, HEAD, 70), ER(10, CL_L, 1, SPINE, 60), ER(30,
                                                                    THUMP,
                                                                    1,
                                                                    SPINE,
                                                                    50),
ER(40, FALL_HAND, 1, R_HAND, 50) };
static const EpRec EPR_C_FALL[] =
    { ER(0, FALL_BODY_H, 1, PELVIS, 80), ER(0, THUMP, 1, PELVIS, 65),
ER(20, FALL_HAND, 1, L_HAND, 50), ER(50, GEAR_PACK, 1, SPINE, 60),
    ER(90, FALL_HEAD, 1, HEAD, 45), ER(140, CL_L, 1, SPINE, 60), ER(220,
                                                                    SLIDE,
                                                                    1,
                                                                    PELVIS,
                                                                    45)
};
static const EpRec EPR_C_WHOOSH[] =
    { ER(0, WH_BODY, 0, PELVIS, 55), ER(40, CL_L, 0, SPINE, 55), ER(70,
                                                                    STEP_M,
                                                                    0,
                                                                    L_FOOT,
                                                                    60) };
static const EpRec EPR_C_KICK[] =
    { ER(0, HIT_BODY, 1, SPINE, 80), ER(0, STEP_H, 0, R_FOOT, 80), ER(20,
                                                                      CL_L,
                                                                      0,
                                                                      SPINE,
                                                                      60)
    };
// ctxdown: act = CE_*, style -1 fists (the punches stay the lead: no thump / bone under them), weapons otherwise
static void EpCtxAct(int act, int style, uint64_t att, uint64_t vic)
{
#define PLAY(a) EpPlayRecipe(a, (int)(sizeof a / sizeof a[0]), att, vic)
    switch (act) {
    case EPC_GRAB:
        PLAY(EPR_C_GRAB);
        break;
    case EPC_GRUNT:
        PLAY(EPR_C_GRUNT);
        break;
    case EPC_HIT:
        PLAY(EPR_C_HIT);
        break;
    case EPC_HEAVY:
        PLAY(EPR_C_HEAVY);
        if (style != -1)
            PLAY(EPR_C_HEAVY_W);
        break;
    case EPC_THUD:
        PLAY(EPR_C_THUD);
        break;
    case EPC_FALL:
        PLAY(EPR_C_FALL);
        break;
    case EPC_WHOOSH:
        PLAY(EPR_C_WHOOSH);
        break;
    case EPC_KICK:
        PLAY(EPR_C_KICK);
        break;
    default:
        break;
    }
#undef PLAY
}

// standing finishers (fin_sfx_begin): the attacker's own side, at the labelled moments
static const EpRec EPR_F_GRAB[] =
    { ER(0, GR_ARM, 0, R_HAND, 60), ER(10, CL_M, 0, SPINE, 50), ER(0,
                                                                   STEP_L,
                                                                   0,
                                                                   L_FOOT,
                                                                   45) };
static const EpRec EPR_F_PUNCH[] =
    { ER(0, CL_M, 0, SPINE, 50), ER(0, STEP_M, 0, R_FOOT, 50), ER(30,
                                                                  GEAR_PACK,
                                                                  0, SPINE,
                                                                  35) };
static const EpRec EPR_F_HEAVY[] =
    { ER(0, CL_L, 0, SPINE, 65), ER(0, STEP_H, 0, R_FOOT, 60), ER(30,
                                                                  GEAR_PACK,
                                                                  0, SPINE,
                                                                  45) };
static const EpRec EPR_F_KICK[] =
    { ER(0, STEP_H, 0, L_FOOT, 70), ER(0, CL_L, 0, SPINE, 60), ER(30,
                                                                  GEAR_PACK,
                                                                  0, SPINE,
                                                                  40) };
static const EpRec EPR_F_SWING[] =
    { ER(0, WH_BODY, 0, PELVIS, 50), ER(0, CL_L, 0, SPINE, 55), ER(60,
                                                                   STEP_M,
                                                                   0,
                                                                   L_FOOT,
                                                                   50),
ER(30, GEAR_MELEE, 0, SPINE, 45) };
static const EpRec EPR_F_BLOW[] =
    { ER(0, CL_L, 0, SPINE, 65), ER(0, STEP_H, 0, R_FOOT, 55), ER(20,
                                                                  GEAR_MELEE,
                                                                  0, SPINE,
                                                                  50) };
static void EpFinLabel(int label)
{
    uint64_t att = SID_PLAYER;
#define PLAY(a) EpPlayRecipe(a, (int)(sizeof a / sizeof a[0]), att, 0)
    switch (label) {
    case EXS_GRAB_TORSO:
        PLAY(EPR_F_GRAB);
        break;
    case EXS_HIT_FIST_FACE:
        PLAY(EPR_F_PUNCH);
        break;
    case EXS_HIT_FIST_FACE_FINISHER:
        PLAY(EPR_F_HEAVY);
        break;
    case EXS_GUT_KICK:
    case EXS_HIT_BOOT_FACE:
        PLAY(EPR_F_KICK);
        break;
    case EXS_PIPE_WHOOSH:
    case EXS_MACHETE_WHOOSH:
        PLAY(EPR_F_SWING);
        break;
    case EXS_HIT_PIPE_HEAD_FINISHER:
    case EXS_HIT_MACHETE_TORSO_FINISHER:
    case EXS_HIT_MACHETE_HEAD_FINISHER:
        PLAY(EPR_F_BLOW);
        break;
    default:
        break;
    }
#undef PLAY
}

// downed melee (dm_snd_tick): kind 0 effort, 1 weapon swing, 2 contact, 3 moan; cls 0 = kick, 1 = weapon
static const EpRec EPR_D_EFFORT[] =
    { ER(0, CL_M, 0, SPINE, 50), ER(0, STEP_M, 0, L_FOOT, 55), ER(40,
                                                                  GEAR_PACK,
                                                                  0, SPINE,
                                                                  40) };
static const EpRec EPR_D_SWING[] =
    { ER(0, WH_BODY, 0, PELVIS, 50), ER(30, CL_L, 0, SPINE, 55) };
static const EpRec EPR_D_KICK[] =
    { ER(0, HIT_BODY, 1, SPINE, 75), ER(0, STEP_H, 0, R_FOOT, 70), ER(10,
                                                                      CL_L,
                                                                      0,
                                                                      SPINE,
                                                                      55),
ER(20, CL_M, 1, SPINE, 50) };
static const EpRec EPR_D_BLOW[] =
    { ER(0, HIT_BODY, 1, SPINE, 70), ER(0, STEP_H, 0, R_FOOT, 65), ER(5,
                                                                      BONE,
                                                                      1,
                                                                      HEAD,
                                                                      40),
ER(10, CL_L, 0, SPINE, 55), ER(20, CL_M, 1, SPINE, 50) };
static const EpRec EPR_D_MOAN[] =
    { ER(0, FALL_HAND, 1, R_HAND, 45), ER(30, CL_M, 1, SPINE, 45) };
static void EpDm(int kind, int cls, uint64_t att, uint64_t vic)
{
#define PLAY(a) EpPlayRecipe(a, (int)(sizeof a / sizeof a[0]), att, vic)
    switch (kind) {
    case 0:
        PLAY(EPR_D_EFFORT);
        break;
    case 1:
        PLAY(EPR_D_SWING);
        break;
    case 2:
        if (cls)
            PLAY(EPR_D_BLOW);
        else
            PLAY(EPR_D_KICK);
        break;
    default:
        PLAY(EPR_D_MOAN);
        break;
    }
#undef PLAY
}

#undef ER

// the move was cut (gx_abort / psync abort): the timeline stops and nothing delayed of the recipes fires either
static void ExplusCut(const char *why)
{
    EpSummary(why);
    memset(g_epq, 0, sizeof g_epq);
    EbCut(why);
    EsCut(why);
}
