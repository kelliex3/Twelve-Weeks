// TLOU2 blade melee sound design as layered PCM cues (Ellie's knife / machete SFX; NOT the 2013 shiv kills, which keep their audio).
// Included by t2r-ext.c right after t2melee.h (before ex_play_label); ctxdown.c calls blade_cd_act / blade_pending_tick.
//   t2blade.pcm / t2blade.h = tools/rip_t2_blade.py: each cue = the layers of one TLOU2 blade event mixed with their offsets (stab = shing + flesh
//   + thud, neck slice = skin slash + neck tear + blood squirt + gush ...), 4 variants per cue. Played through the PCM mixer, 3D on the body.
//   - executions: ex_play_label maps EXS_MACHETE_WHOOSH / EXS_HIT_MACHETE_* (machete + tomahawk + 1h blade executions, SP blade finishers) to the
//     M_* cues (blade_label_cue); EXS_SHIV_* are not touched.
//   - contextual takedowns: ctxdown's knife (style 3) and 1h-blade (style 1) pairs use BLADE_CD (cue per pair: swish / hit / heavy / extract).
// t2r-no-blade-pcm.txt next to the exe = off (the previous t2melee waves play again).  t2r-melee-sfx-gain.txt (percent) is the master, as for t2melee.
#include "t2blade.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl t2b_blob\nt2b_blob:\n.incbin \"t2blade.pcm\"\n.text\n");
extern const int16_t t2b_blob[];

static int BladePcmOn(void)
{
    static int off = -1;
    static uint64_t last;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - last > 2000) {
        FILE *f = ExeSideOpen("t2r-no-blade-pcm.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
        last = now;
    }
    return !off;
}

static float BladeMaster(void)
{
    static float v = 0.45f;
    static uint64_t last;
    uint64_t now = GetTickCount64();
    if (now - last > 2000) {
        last = now;
        v = 0.45f;
        FILE *f = ExeSideOpen("t2r-melee-sfx-gain.txt", "r");
        if (f) {
            int pc;
            if (fscanf(f, "%d", &pc) == 1 && pc >= 0 && pc <= 400)
                v = pc / 100.0f;
            fclose(f);
        }
    }
    return v;
}

// one cue (a random variant) at a body (ent) or a world position
static int BladeCuePlay(int cue, float gain, uint64_t ent,
                          const float *wpos, const char *tag)
{
    static unsigned rot;
    static int nl;
    if (cue < 0 || cue >= BC_N || !T2B_CUE[cue].count)
        return 0;
    unsigned v =
        T2B_CUE[cue].first + (rot++ * 5u +
                              (unsigned) (GetTickCount64() >> 4)) %
        T2B_CUE[cue].count;
    int ok =
        PcmPlayAt2(t2b_blob + T2B_WAVE[v].off, (int) T2B_WAVE[v].n,
                     gain * BladeMaster() * T2B_CUE[cue].gain / 100.0f,
                     T2B_RATE, ent, wpos);
    if (nl++ < 160)
        Xlog("blade sfx: %s: cue %s variant %u -> %d", tag ? tag : "?",
             T2B_NAME[cue], v - T2B_CUE[cue].first, ok);
    return ok;
}

// execution labels -> cue (-1 = not a blade label: the caller's old path; the shiv labels are never mapped)
static int BladeLabelCue(int label)
{
    switch (label) {
    case EXS_MACHETE_WHOOSH:
        return BC_M_WHOOSH;
    case EXS_HIT_MACHETE_TORSO_FINISHER:
        return BC_M_TORSO;
    case EXS_HIT_MACHETE_TORSO_EXTRACT:
        return BC_M_EXTRACT;
    case EXS_HIT_MACHETE_HEAD_FINISHER:
        return BC_M_NECK;
    }
    return -1;
}

// contextual takedown pairs: {pair name, hit cue, heavy cue, cue after the heavy blow (BC_N = none)}; the swish is the style's
static const struct {
    const char *pair;
    int hit, heavy, post;
} BLADE_CD[] = {
    // ---- Ellie's knife (style 3)
    { "knife-wall-kneed-gut-slice", BC_K_SLASH, BC_K_GUT_SLICE, BC_N },
    { "knife-wall-neck-jab", BC_K_NECK_STAB, BC_K_NECK_STAB,
     BC_K_EXTRACT },
    { "knife-wall-neck-slice", BC_K_SLASH, BC_K_NECK_SLICE, BC_N },
    { "knife-wall-chest-stab", BC_K_STAB, BC_K_STAB_DEEP, BC_K_EXTRACT },
    { "knife-cover-chest-stab", BC_K_STAB, BC_K_STAB_DEEP, BC_K_EXTRACT },
    { "knife-cover-gut-to-eye", BC_K_STAB, BC_K_EYE, BC_K_EXTRACT },
    { "knife-cover-ice-pick", BC_K_STAB, BC_K_HEAD_STAB, BC_K_EXTRACT },
    { "knife-cover-neck-stab", BC_K_STAB, BC_K_NECK_STAB, BC_K_EXTRACT },
    // ---- 1h blade / machete (style 1)
    { "blade-wall-gut-chop", BC_M_SLASH, BC_M_GUT, BC_M_EXTRACT },
    { "blade-wall-neck-chop", BC_M_SLASH, BC_M_NECK, BC_N },
    { "blade-wall-shoulder-chop", BC_M_SLASH, BC_M_TORSO, BC_M_EXTRACT },
    { "blade-wall-skull-chop", BC_M_TORSO, BC_M_NECK, BC_N },
    { "blade-wall-gut-stab", BC_M_TORSO, BC_M_TORSO, BC_M_EXTRACT },
    { "blade-wall-neck-slice", BC_M_SLASH, BC_M_NECK, BC_N },
    { "blade-cover-hack", BC_M_SLASH, BC_M_GUT, BC_N },
    // ---- the upgraded 1h melee one-shots (ctxdown.c kind 0 rows, cd_upg_start): hit = the blow the clip starts with, heavy = the crowbar's yank out /
    //      the machete pulled out after the kick. The crowbar row is style 0 (1h blunt) but its taped blades stab the EYE: the knife's eye-stab layers
    { "upg-crowbar-eye-jab", BC_K_EYE, BC_K_EXTRACT, BC_N },
    { "upg-blade-gut-chop-kick", BC_M_GUT, BC_M_EXTRACT, BC_N },
};

#define BLADE_POST_MS 280       // the blade comes back out this long after the heavy blow
static struct {
    double due;
    int cue;
    uint64_t ent;
    char tag[40];
} g_blade_pend[4];
static void BladePendingTick(void)
{
    double now = (double) GetTickCount64();
    for (int i = 0; i < 4; i++)
        if (g_blade_pend[i].cue >= 0 && g_blade_pend[i].due
            && now >= g_blade_pend[i].due) {
            if (now - g_blade_pend[i].due < 600.0)
                BladeCuePlay(g_blade_pend[i].cue, 1.0f,
                               g_blade_pend[i].ent, NULL,
                               g_blade_pend[i].tag);
            g_blade_pend[i].due = 0;
        }
}

// act: 0 = the swish before a blow, 1 = the first blow, 2 = the heavy / final blow.  style 3 knife, 1 blade.  Returns 1 when played (the caller skips its own label).
static int BladeCdAct(const char *pair, int style, int act, uint64_t vic,
                        uint64_t att)
{
    int listed = 0;
    for (size_t i = 0; i < sizeof BLADE_CD / sizeof BLADE_CD[0]; i++)
        listed |= !strcmp(BLADE_CD[i].pair, pair);
    if ((style != 1 && style != 3 && style != 4 && !listed)
        || !BladePcmOn())
        return 0;               // (a listed pair of another style: the upgraded crowbar's taped blades)
    int hit = style == 3 ? BC_K_STAB : BC_M_SLASH, heavy =
        style == 3 ? BC_K_STAB_DEEP : BC_M_TORSO, post = BC_N;
    for (size_t i = 0; i < sizeof BLADE_CD / sizeof BLADE_CD[0]; i++)
        if (!strcmp(BLADE_CD[i].pair, pair)) {
            hit = BLADE_CD[i].hit;
            heavy = BLADE_CD[i].heavy;
            post = BLADE_CD[i].post;
        }
    if (act == 0)
        return BladeCuePlay(style == 3 ? BC_K_SWISH : BC_M_WHOOSH, 1.0f,
                              att, NULL, pair);
    if (act == 1)
        return BladeCuePlay(hit, 1.0f, vic, NULL, pair);
    int ok = BladeCuePlay(heavy, 1.0f, vic, NULL, pair);
    if (post < BC_N) {
        for (int i = 0; i < 4; i++)
            if (!g_blade_pend[i].due) {
                g_blade_pend[i].due =
                    (double) GetTickCount64() + BLADE_POST_MS;
                g_blade_pend[i].cue = post;
                g_blade_pend[i].ent = vic;
                snprintf(g_blade_pend[i].tag, sizeof g_blade_pend[i].tag,
                         "%s", pair);
                break;
            }
    }
    return ok;
}
