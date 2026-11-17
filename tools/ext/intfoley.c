// intfoley.c - the body foley of an Interrogation (there were no struggle sounds (body, clothing etc), so the anims played just 2 sounds in total, one voice each). Included from t2r-ext.c after intvox.c (uses g_ivx, g_ex, explus.c's ep_* helpers and the PCM mixer).
//
// The interrogate-enter pairs (MX_INTERROGATE fists 4.6 s, MX_INT_SHIV shiv 2.25 / 2.55 s; interrogation.c) are the 2013 clips; their authored sound events are
// TLOU1 foot / gear / hand / voice-effect events the TLOU2 engine does not play, and there is no whole-move stream for them (rip_t1_execution_foley.py: "the
// 2013 clips' own sound events"). So they were silent but for intvox.c's two voices. What now plays, on every screen (interrogator, victim, bystanders - 3D at the
// body's joint, each screen the same layers at the same move time), all from TLOU2's own SP waves of the execution sound design plus (explus.c):
//   1. the timeline of the exec+ system, now generated for the four interrogate pairs (execplus_tl.h pairs 21-24, re/factions2013/t1anim/gen_execplus.py reads
//      the 2013 clips: foot plants, kick / knee, limb swing peaks = whooshes, cloth by body motion, pack / gear shifts, hand grabs (GR_*) and the held victim
//      (STRUG_*), body / head / knee / hand contacts on the floor, the victim's fall after the kill)
//   2. the 2013 clips' own lead impacts (def-net-fist-interrogate-enter-*: sfx-gut-kick f9, sfx-joel-impact-foot f18, impact-body f23 / f62, impact-head f68;
//      front-dup shiv: sfx-hit-knee-face f6, grab f23) as EXSFX_MELEE rows 21-24 of exsfx2013.h (the kill itself: shivkit.c's SP kit / the engine's death)
//   3. THIS file: the shaking. The hold is a long steady grip the motion analysis only samples where the hands move fast, so a sparse extra layer runs through
//      the hold window (fists: 2.4 s .. kill - 0.3 s; shiv: 0.7 s .. kill - 0.3 s) every 560-800 ms: the victim fighting the grip (STRUG_TORSO / STRUG_HEAD,
//      cloth, a boot scuff or a dragged back), the interrogator re-gripping (cloth, pack, grip), rising in strength to the kill; never within 300 ms of a
//      generated STRUG row (no doubling) and never dense or rhythmic (rapid rubbing sounded wrong).
//   4. the two voice systems do not overlap: exec+'s breath / strain / struggle voice rows of an interrogate pair are skipped while an intvox.c line speaks or is
//      due within 400 ms (one voice per speaker).
// t2r-no-int-foley.txt next to the exe = off (all of the above for the interrogate pairs; the exec+ switch t2r-no-exec-foley-plus.txt still turns off everything).
// Log: 'int foley:' (one line at the start of each interrogation: rows, window; one per supplement cue up to 60).
static struct {
    double t0;
    int on, k;
    double next;
    uint32_t seed;
    int fist;
    double a, b;
    uint64_t att, vic;
} g_ifo;
static int IfoOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-no-int-foley.txt", &at, &off, 120);
}

// 1 = fist interrogation, 2 = shiv interrogation, 0 = any other melee pair
static int IfoKind(int pair)
{
    if (pair < 0 || (size_t) pair >= MX_N)
        return 0;
    const char *nm = MELEE2013[pair].name;
    return !strncmp(nm, "fist-interrogate", 16) ? 1 : !strncmp(nm,
                                                               "shiv-interrogate",
                                                               16) ? 2 : 0;
}

// explus.c ep_begin: an interrogate pair with the switch off plays no exec+ rows
static int IntfoleyGate(int kind, int idx)
{
    if (kind != 1 || !IfoKind(idx))
        return 0;
    if (IfoOff()) {
        static int nl;
        if (LOGCAP(nl, 6))
            Xlog("int foley: off (t2r-no-int-foley.txt) - %s plays no body foley", MELEE2013[idx].name);
        return 1;
    }
    return 0;
}

// explus.c ep_row: voice rows of an interrogate pair yield to intvox.c's lines
static int IntfoleyVoxClash(double ems)
{
    if (g_ex.kind != 2 || !IfoKind(g_ex.pair) || g_ivx.t0 != g_ex.t0
        || !g_ivx.on)
        return 0;
    ems += g_ivx.shift;         // (ems = move clock of the exec+ rows (clip time); intvox.c's lines are in real ms, intwarp.c)
    if ((g_ivx.busy[0] > 0.0 && ems <= g_ivx.busy[0] + 100.0)
        || (g_ivx.busy[1] > 0.0 && ems <= g_ivx.busy[1] + 100.0))
        return 1;
    for (int i = g_ivx.step; i < g_ivx.nstep; i++) {
        double d = ems - g_ivx.st[i].at;
        if (d > -400.0 && d < 400.0)
            return 1;
    }
    return 0;
}

// a victim line (intvox.c, PCM) is playing at this move time (clip ms, as intfoley_vox_clash): the grip foley under it is ducked (explus.c ep_row) and the shaking cues wait
static int IntfoleyVicLine(double ems)
{
    if (g_ex.kind != 2 || !IfoKind(g_ex.pair) || g_ivx.t0 != g_ex.t0
        || !g_ivx.on || g_ivx.vline[1] <= 0.0)
        return 0;
    ems += g_ivx.shift;
    return ems >= g_ivx.vline[0] - 50.0 && ems <= g_ivx.vline[1] + 50.0;
}

// is a generated STRUG row of this pair within 300 ms of `ms`?
static int IfoStruggleNear(int pair, double ms)
{
    for (size_t i = 0; i < EXPLUS_N; i++) {
        if (EXPLUS_TL[i].kind != 1 || EXPLUS_TL[i].pair != pair)
            continue;
        if (EXPLUS_TL[i].layer != EPL_STRUG_TORSO
            && EXPLUS_TL[i].layer != EPL_STRUG_HEAD)
            continue;
        double d = (double) EXPLUS_TL[i].ms - ms;
        if (d > -300.0 && d < 300.0)
            return 1;
    }
    return 0;
}

// the shaking cues (ep_rec: delay ms, layer, who 0 interrogator / 1 victim, joint, gain percent). Fists: the victim lies on the ground and is dragged / shaken;
// shiv: the victim stands, held from behind (or in front) - boots scuff instead of a back dragging.
#define IR(d, l, w, j, g) {(d), EPL_##l, (w), EPJ_##j, (g)}
static const EpRec IFO_F0[] =
    { IR(0, STRUG_TORSO, 1, SPINE, 62), IR(50, CL_M, 0, SPINE, 46), IR(120,
                                                                       CL_S,
                                                                       1,
                                                                       SPINE,
                                                                       42)
    };
static const EpRec IFO_F1[] =
    { IR(0, STRUG_HEAD, 1, HEAD, 56), IR(40, CL_M, 1, SPINE, 44), IR(90,
                                                                     GEAR_PACK,
                                                                     0,
                                                                     SPINE,
                                                                     38) };
static const EpRec IFO_F2[] =
    { IR(0, CL_L, 1, SPINE, 54), IR(20, GR_ARM, 0, R_HAND, 40), IR(80,
                                                                   SLIDE,
                                                                   1,
                                                                   PELVIS,
                                                                   34) };
static const EpRec IFO_S0[] =
    { IR(0, STRUG_TORSO, 1, SPINE, 60), IR(50, CL_M, 0, SPINE, 46), IR(110,
                                                                       SCUFF,
                                                                       1,
                                                                       L_FOOT,
                                                                       42)
    };
static const EpRec IFO_S1[] =
    { IR(0, STRUG_HEAD, 1, HEAD, 54), IR(40, CL_M, 1, SPINE, 44), IR(100,
                                                                     GEAR_PACK,
                                                                     0,
                                                                     SPINE,
                                                                     36) };
static const EpRec IFO_S2[] =
    { IR(0, CL_L, 1, SPINE, 52), IR(20, GR_ARM, 0, R_HAND, 40), IR(70,
                                                                   STEP_L,
                                                                   0,
                                                                   R_FOOT,
                                                                   40),
IR(130, SCUFF, 1, R_FOOT, 40) };
#undef IR
static void IntfoleyTick(void)
{
    int run = g_ex.kind == 2 ? IfoKind(g_ex.pair) : 0;
    if (!run) {
        g_ifo.on = 0;
        return;
    }
    if (IfoOff() || EpOff())
        return;
    double ems = PsyncMs() - g_ex.t0;
    if (g_ex.t0 != g_ifo.t0 || !g_ifo.on) {
        if (g_ex.t0 == g_ifo.t0)
            return;             // this run was cut / finished
        memset(&g_ifo, 0, sizeof g_ifo);
        g_ifo.t0 = g_ex.t0;
        g_ifo.on = 1;
        g_ifo.fist = run == 1;
        g_ifo.att = g_ex.att_ent;
        g_ifo.vic = g_ex.vic_ent;
        double kill = (double) MELEE2013[g_ex.pair].kill_s * 1000.0;
        g_ifo.a = g_ifo.fist ? 2400.0 : 700.0;
        g_ifo.b = kill - 300.0;
        g_ifo.next = g_ifo.a;
        g_ifo.seed =
            (uint32_t) (g_ex.t0 * 0.001) *
            2654435761u ^ (uint32_t) g_ex.pair * 0x9e3779b1u;
        int rows = 0;
        for (size_t i = 0; i < EXPLUS_N; i++)
            if (EXPLUS_TL[i].kind == 1 && EXPLUS_TL[i].pair == g_ex.pair)
                rows++;
        Xlog("int foley: %s - %d exec+ timeline rows (2013 clip motion: steps, whooshes, cloth, gear, grabs, held struggle, falls), shaking layer %.0f..%.0f ms every ~0.6 s, " "voices yield to int vox; exec+ %s", MELEE2013[g_ex.pair].name, rows, g_ifo.a, g_ifo.b, g_ex.ep_on ? "armed" : "NOT armed (exec+ off / table mismatch)");
    }
    while (g_ifo.next < g_ifo.b && g_ifo.next <= ems) {
        double at = g_ifo.next;
        g_ifo.seed = g_ifo.seed * 1664525u + 1013904223u;
        double gap = 560.0 + (double) ((g_ifo.seed >> 16) % 240u);      // 560-800 ms, never a steady beat
        g_ifo.next += gap * (1.0 - 0.25 * (at - g_ifo.a) / (g_ifo.b - g_ifo.a + 1.0));  // a little faster towards the kill
        if (ems - at > 300.0) {
            g_ifo.k++;
            continue;
        }                       // joined late: the moment is gone
        int k = g_ifo.k++;
        if (IfoStruggleNear(g_ex.pair, at) || IntfoleyVicLine(at))
            continue;           // (no shaking cue over the victim's line)
        const EpRec *rec;
        int n;
        switch (k % 3) {
        case 0:
            rec = g_ifo.fist ? IFO_F0 : IFO_S0;
            n = g_ifo.fist ? (int) (sizeof IFO_F0 /
                                    sizeof IFO_F0[0]) : (int) (sizeof
                                                               IFO_S0 /
                                                               sizeof
                                                               IFO_S0[0]);
            break;
        case 1:
            rec = g_ifo.fist ? IFO_F1 : IFO_S1;
            n = g_ifo.fist ? (int) (sizeof IFO_F1 /
                                    sizeof IFO_F1[0]) : (int) (sizeof
                                                               IFO_S1 /
                                                               sizeof
                                                               IFO_S1[0]);
            break;
        default:
            rec = g_ifo.fist ? IFO_F2 : IFO_S2;
            n = g_ifo.fist ? (int) (sizeof IFO_F2 /
                                    sizeof IFO_F2[0]) : (int) (sizeof
                                                               IFO_S2 /
                                                               sizeof
                                                               IFO_S2[0]);
            break;
        }
        EpPlayRecipe(rec, n, g_ifo.att, g_ifo.vic);
        static int nl;
        if (LOGCAP(nl, 60))
            Xlog("int foley: +%.0f ms shaking cue %d (%s) on the interrogator and the victim", at, k, k % 3 == 0 ? "victim fights the grip" : k % 3 == 1 ? "head / cloth / pack" : "re-grip, drag");
    }
}
