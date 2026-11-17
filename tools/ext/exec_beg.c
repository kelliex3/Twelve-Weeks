// Execution begging: begging for life voicelines (with adrenaline, not quiet normal begging, more like NO PLEASE! or NOO!) as
// characters are about to get shot in special executions, interrupted by the shot. In animations where the victim is shot multiple
// times, the begging can start before the first shot and be interrupted, with the sounds of how a voice sounds when somebody is
// shot multiple times while talking, and then stay quiet on the death shot.
// The first (PCM mixer) version sounded 2D and unlike the real vox, the mouths did not move and it did not use the language vox, so this
// version goes through the ENGINE's speech path like every other character line (team callouts, burn screams, reload callouts):
//   start : `say(character, vox-info record, 2, 0)` on the victim's body (SID_PLAYER on our own screen, the puppet elsewhere). The records
//           t2r-vox-beg-<voice>-<kind>-NN live in the mod's vox-igc-greenlight module (vox_lines.py genbeg -> vox_beg.h, dc_data_patches.patch_vox_beg) and
//           point at the NPC voice set's own speech streams, so the line is 3D at the head on the game's voice bus, lip-synced, and in the ACTIVE voice
//           language (the engine resolves the stream in speech1/<language>.psarc; takes without a stream there would fall back to English, so they are
//           filtered with VXG_AVAIL_<language> using the language the engine latched, g_vx_lang_nm, as the team lines do).
//   cut   : the native `stop-vox(character)` (the one shivkit.c uses to cut a grunt for a choke) at each shot, then - for a non-final shot - the next
//           `say` (a bullet-hit grunt) starts; the death shot only cuts and nothing follows.
// Timeline (every gun pair that fires, every client, from ex_begin on the psync clock): the plea starts EB_LEAD_MS before the first shot (WLF
// last-words-plead "No, please--!" / Seraphite "No!!" "Oh no!!"), the shot cuts it, every non-final shot is followed by a hit-by-pistol / -rifle / -shtgn
// grunt of the same voice that the next shot cuts, silence on the death shot. The victim's other `say` voices (gx_play pain / death, explus ep_vox) stay
// off from the plea until the move ends. t2r-no-exec-beg.txt next to the exe: off. Logs 'exec beg: ...'.
// Included from t2r-ext.c after deathreact.c (uses the voice-set helpers of the burn screams).
#include "vox_beg.h"
#define EB_LEAD_MS 800          // the plea starts this long before the first shot (the takes are 0.4-1.1 s; the shot cuts a longer one)
static struct {
    int last_head, on, pair, nshot, shot_i, pkind, beg_done, fac, g, v,
        last_pain, last_beg, limb, scr_on, scr_step, scr_last, scr_n,
        chk_kind, chk_rec, chk_ev;
    uint32_t seed;              // same on every screen for one execution (victim slot, pair, count of that victim's executions): the take picks
    uint64_t chk_at;
    uint16_t shot[6];
    double t0;
    uint32_t beg_at;
    double scr_next;            // dismember scream: ms (move clock) the next take of the chain starts
    uint64_t ent;
    double mw_from, mw_to;      // melee pleas (execsp.c): the boost window, ms of the move clock
    int speaking;               // a line of ours may still be playing on the victim (stop-vox at the next shot)
} g_eb;
static int EbOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-no-exec-beg.txt", &at, &off, 120);
}

// takes with a stream in the active voice language (NULL table = English / unknown language: no filtering)
static const uint8_t *EbAvail(void)
{
    static const uint8_t *tab;
    static int done;
    if (!done) {
        VxLangTab(0);         // (latches the engine's language into g_vx_lang_nm once it is up)
        if (!g_vxLangNm)
            return NULL;        // not known yet: retried at the next execution
        done = 1;
        for (size_t i = 0; i < sizeof VXG_LANGS / sizeof *VXG_LANGS; i++)
            if (!_stricmp(VXG_LANGS[i].name, g_vxLangNm))
                tab = VXG_LANGS[i].tab;
        Xlog("exec beg: voice language '%s' - takes without a stream in it are skipped%s", g_vxLangNm, tab ? "" : " (language not in the table: unfiltered)");
    }
    return tab;
}

// the plea / grunts hold the victim's voice from shortly before the plea until the move ends
static int EsHoldsVictim(void);       // execsp.c: an unarmed / blunt execution's SP victim voice timeline owns the victim's voice
static int EbCoversGun(void)
{
    if (!g_eb.on)
        return 0;
    double ems = PsyncMs() - g_eb.t0;
    // (a melee plea set-up has no shots and beg_at 0, so this covered - silenced explus / gx / downed groans - for the whole
    // execution) melee pleas cover only while their own take plays
    if (g_eb.nshot <= 0)
        return g_eb.mw_to > 0.0 && ems >= g_eb.mw_from - 150.0
            && ems <= g_eb.mw_to;
    return ems >= (double) g_eb.beg_at - 150.0;
}

static int EbCovers(void)
{
    return EbCoversGun() || EsHoldsVictim();
}

static void EbStopVoice(void)
{
    if (!g_eb.speaking || !g_eb.ent)
        return;
    uint64_t a[1] = { g_eb.ent };
    uint64_t r = VoxStop(a[0]);
    g_eb.speaking = 0;
    static int nl;
    if (LOGCAP(nl, 100))
        Xlog("exec beg: stop-vox on %s -> %llx",
             g_eb.ent == SID_PLAYER ? "our body" : "their puppet",
             (unsigned long long) r);
}

static void EbCut(const char *why)
{
    if (!g_eb.on)
        return;
    EbStopVoice();
    static int nl;
    if (LOGCAP(nl, 60))
        Xlog("exec beg: stopped (%s)", why);
    memset(&g_eb, 0, sizeof g_eb);
}

// the victim's NPC voice set on THIS screen (bd_voice_sid's rule): 0 = known, fills fac / g / v
static uint32_t eb_vslot;
static int EbVoice(uint64_t ent, int *fac, int *g, int *v, char *desc,
                    size_t dn)
{
    int look, fem;
    if (FactionLooksOff()) {
        snprintf(desc, dn, "faction looks off");
        return 0;
    }
    // the faction a screen WEARS is local (client-side clan faction: two WLF players on opposite teams both see themselves as WLF), so the voice set
    // follows the faction of the body shown on THIS screen - a Seraphite-looking body never speaks WLF lines. The take is the same on every screen (eb_pick's seed).
    if (ent == SID_PLAYER) {
        *fac = VxOwnFac();
        look = g_fl.cur;
        fem = OwnFemale();
        eb_vslot = *(uint32_t *) T2R_SYM(kMySlot);
    } else {
        uint32_t slot =
            g_exThird ? g_thirdVicSlot : (uint32_t) g_gx.slot;
        if (slot >= T2R_MAX_PEERS) {
            snprintf(desc, dn, "victim slot %u not known", slot);
            return 0;
        }
        int team;
        const char *why;
        look = FlCoreShown(&g_fl, slot, &team);
        *fac = VxFacOfSlot(slot, &why);
        fem = SlotFemale(slot);
        eb_vslot = slot;
    }
    if ((*fac != 0 && *fac != 1) || look < 0) {
        snprintf(desc, dn, "faction %d / look %d not known", *fac, look);
        return 0;
    }
    *g = fem ? 1 : 0;
    *v = VxVoiceOfLook(*fac, look);
    snprintf(desc, dn, "%s %s voice %c", *fac ? "WLF" : "Seraphite",
             fem ? "female" : "male", 'a' + *v);
    return 1;
}

// a record of kind `kind` (0 plea, 1 / 2 / 3 hit grunt) of the voice set with a stream in the voice language (never `avoid`); grunts fall back to the
// other hit kinds of the same voice; -1 = none
static int KIsVerbal(int k)
{
    return k == 0;
}

// the long NOOOOOO scream did not fit even gunshot executions, it should be more like "Wait, wai-": the plea pool no longer holds the long panicked
// shouts (VXG_SCREAM: "No!!" / "No, no!!" / "Oh no!!", Seraphite voices only - every WLF plea is an interrupted "Wait, wait--!" / "No, please--!"). t2r-exec-keep-long-scream.txt
// next to the exe = the shouts back in the gun executions' pool. g_eb_pick_max: a take longer than this many ms is not picked (the melee pleas: short, cut by the next blow).
static int EbKeepScream(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-exec-keep-long-scream.txt", &at, &off, 120);
}

static int g_ebPickMax;
static int EbPick(int kind, int avoid, uint32_t salt)
{
    // fallback kinds per kind (-1 ends): grunts fall back to the other hit kinds, the scream chain to its other takes, then to a hit grunt
    static const int8_t FB[8][5] =
        { { 0, -1 }, { 1, 2, 3, -1 }, { 2, 3, 1, -1 }, { 3, 1, 2, -1 },
        { 4, 5, 6, 3, -1 }, { 5, 4, 6, 3, -1 }, { 6, 5, 4, 3, -1 }, { 7, 6,
                                                                     5, 3,
                                                                     -1 }
    };
    const uint8_t *av = EbAvail();
    for (int t = 0; t < 5 && FB[kind][t] >= 0; t++) {
        int k = FB[kind][t];
        int n = VXG_RANGE[g_eb.fac][g_eb.g][g_eb.v][k][1], first =
            VXG_RANGE[g_eb.fac][g_eb.g][g_eb.v][k][0], cand[16], nc = 0;
        // only the verbal plea (kind 0) needs a stream in the voice language; grunts / screams / cries are non-verbal efforts, any take serves (the
        // polish screen skipped every scream take and the dismember scream stayed silent). A language-less kind still prefers takes that have the stream.
        int drop_scream = KIsVerbal(k) && !EbKeepScream();
        for (int i = 0; i < n && nc < 16; i++) {
            if (drop_scream && VXG_SCREAM[first + i])
                continue;
            if (KIsVerbal(k) && g_ebPickMax
                && VXG_DUR[first + i] > g_ebPickMax)
                continue;
            if (!av || !KIsVerbal(k) || av[first + i])
                cand[nc++] = first + i;
        }
        if (!nc)
            continue;
        uint32_t hs =
            (g_eb.seed ^ (kind * 0x9e3779b1u) ^ (salt * 0x85ebca6bu)) *
            2654435761u;
        hs ^= hs >> 15;
        hs *= 2246822519u;
        hs ^= hs >> 13;
        int ci = (int) (hs % (uint32_t) nc);    // deterministic: the same take on every screen
        if (nc > 1 && cand[ci] == avoid)
            ci = (ci + 1) % nc;
        return cand[ci];
    }
    return -1;
}

// `say` of the record answered 0 on both screens and nothing was heard, while the same call (winmm FUN_18000b640 = say(speaker, line, 2, 0), checked
// in the disassembly: identical arguments) speaks the team callouts. 0 is the answer of every started AND every silently refused line, so it proves nothing:
// the engine's dialogue speech refuses lines for a body in some states (a paired execution, a downed / dying body), while the EFFORT events (vox-eff-gen-*,
// resolved through the character's own vox table) do play on execution bodies (exec+ voices, the audit). So each line is verified: EB_CHECK_MS after the
// `say`, the native is-speaking?(body) is asked; an explicit "no" switches the session to the effort-event fallback below (the same moments, the same cuts) and the
// refused line is re-spoken that way at once. Raw answers are logged ('exec beg: is-speaking? ...') so the next test shows which path the engine takes.
#define EB_CHECK_MS 140
static const char *const EB_EV[8][3] = {
    { "vox-eff-gen-fight-take-hvy", "vox-eff-gen-begs-for-life-stun", NULL },   // 0 plea: a heavy panicked cry (the closest effort to "NO!")
    { "vox-eff-gen-hit-by-bullet", "vox-eff-gen-fight-take-med", NULL },        // 1 pistol grunt
    { "vox-eff-gen-hit-by-rifle", "vox-eff-gen-hit-by-bullet", NULL },  // 2 rifle grunt
    { "vox-eff-gen-hit-by-shotgun", "vox-eff-gen-hit-by-bullet", NULL },        // 3 shotgun grunt
    { "vox-eff-gen-hit-by-explosion", "vox-eff-gen-death-dismember", "vox-eff-gen-fight-take-hvy" },    // 4 long agony scream
    { "vox-eff-gen-fight-take-hvy", "vox-eff-gen-fight-take-lrg", NULL },       // 5 rapid short screams
    { "vox-eff-gen-death-gurgle-whine-lrg", "vox-eff-gen-death-gurgle-whine-sml", NULL },       // 6 crying
    { "vox-eff-gen-brth-fight-injrd", "vox-eff-gen-brth-fight-med", NULL }
};                              // 7 injured panting

static int g_ebEvents;         // learned: the dialogue record path was refused on an execution body -> effort events from now on (this session)
static uint64_t EbSayEvent(int kind)
{
    uint64_t r = ~0ULL;
    for (int i = 0; i < 3 && EB_EV[kind][i]; i++) {
        uint64_t a[4] = { g_eb.ent, Fnv64Rt(EB_EV[kind][i]), 2, 0 };
        uint64_t fs = FemEffortSid(g_eb.ent, kind >= 4 ? VXE_PAIN : VXE_PAIN);        // a female speaker: her own take
        if (fs)
            a[1] = fs;
#ifndef T2R_RELEASE
        PvoxProbeInit();
        g_pvoxArm = g_pvoxTramp != NULL;
#endif
        r = DcCall(FNV64("say"), 4, a);
#ifndef T2R_RELEASE
        g_pvoxArm = 0;
#endif
        static int nl;
        if (LOGCAP(nl, 60))
            Xlog("exec beg: effort event %s on %s -> %llx", EB_EV[kind][i],
                 g_eb.ent == SID_PLAYER ? "our body" : "their puppet",
                 (unsigned long long) r);
        if (r != ~0ULL) {
            g_eb.speaking = 1;
            break;
        }
    }
    return r;
}

static uint64_t EbSay(int rec, int kind)
{
    uint64_t r;
    if (g_ebEvents)
        r = EbSayEvent(kind);
    else {
        uint64_t a[4] = { g_eb.ent, VXG_LINES[rec], 2, 0 };
#ifndef T2R_RELEASE
        PvoxProbeInit();
        g_pvoxArm = g_pvoxTramp != NULL;
#endif
        r = DcCall(FNV64("say"), 4, a);
#ifndef T2R_RELEASE
        g_pvoxArm = 0;
#endif
        if (r != ~0ULL)
            g_eb.speaking = 1;
    }
    g_eb.chk_at = GetTickCount64() + EB_CHECK_MS;
    g_eb.chk_kind = kind;
    g_eb.chk_rec = rec;
    g_eb.chk_ev = g_ebEvents;
    return r;
}

// EB_CHECK_MS after a `say`: was the line really taken? (is-speaking? raw answer logged; ~0 = the native is not callable: unknown, nothing changes)
static void EbCheck(void)
{
    if (!g_eb.chk_at || GetTickCount64() < g_eb.chk_at)
        return;
    g_eb.chk_at = 0;
    uint64_t a[1] = { g_eb.ent };
    uint64_t r = DcCall(FNV64("is-speaking?"), 1, a);
    static int nl;
    if (LOGCAP(nl, 80))
        Xlog("exec beg: is-speaking? on %s %dms after the %s (kind %d) -> %llx", g_eb.ent == SID_PLAYER ? "our body" : "their puppet", EB_CHECK_MS, g_eb.chk_ev ? "effort event" : "record line", g_eb.chk_kind, (unsigned long long) r);
    if (r == ~0ULL || (r & 0xff) || g_eb.chk_ev || g_ebEvents)
        return;
    g_ebEvents = 1;
    Xlog("exec beg: the record line is NOT speaking on an execution body (refused by the dialogue system) - effort events from now on, re-speaking now");
    EbSayEvent(g_eb.chk_kind);
}

// ex_begin (kind 0 = a gun pair), every client
static void EbBegin(int pair, float skip_ms)
{
    memset(&g_eb, 0, sizeof g_eb);
    g_eb.last_beg = g_eb.last_pain = -1;
    if (EbOff() || pair < 0 || (size_t) pair >= GX_N)
        return;
    uint64_t weapon = 0;
    if (g_exThird) {
        typedef uint8_t *(*fn_peer)(uint32_t);
        uint8_t *pa =
            g_thirdAttSlot <
            T2R_MAX_PEERS ? ((fn_peer) T2rPeer) (g_thirdAttSlot) : NULL;
        weapon = pa
            && !IsBadReadPtr(pa + 0x91, 8) ? *(uint64_t *) (pa + 0x91) : 0;
    } else
        weapon = g_gx.weapon ? g_gx.weapon : g_gxVicWeapon;
    uint16_t sh[6];
    int ns = GxShotSched(pair, weapon, sh), n = 0;
    g_eb.limb = -1;
    for (int i = 0; i < ns && i < 6; i++)
        if (!(sh[i] & GXS_ARROW)) {
            if (GUNEXEC_AIM[pair][i] == GXA_KNEE)
                g_eb.limb = n;
            g_eb.shot[n++] = sh[i];
        }
    if (!n)
        return;                 // no shot (pistol whip, butt smash, arrow stab): nothing to beg before
    g_eb.nshot = n;
    g_eb.last_head = 0;
    for (int i = 0, k = 0; i < ns && i < 6; i++)
        if (!(sh[i] & GXS_ARROW)) {
            if (++k == n)
                g_eb.last_head = GUNEXEC_AIM[pair][i] == GXA_HEAD;
        }
    g_eb.ent = g_ex.vic_ent;
    if (!g_eb.ent || g_eb.ent == ~0ULL) {
        Xlog("exec beg: %s skipped - victim body not known",
             GUNEXEC_PAIRS[pair].name);
        g_eb.nshot = 0;
        return;
    }
    char desc[96];
    if (!EbVoice
        (g_eb.ent, &g_eb.fac, &g_eb.g, &g_eb.v, desc, sizeof desc)) {
        Xlog("exec beg: %s skipped - victim voice set unknown (%s)",
             GUNEXEC_PAIRS[pair].name, desc);
        g_eb.nshot = 0;
        return;
    }
    int cls = GUNEXEC_PAIRS[pair].cls;
    g_eb.pkind = cls == GX_SHOTGUN ? 3 : (cls == GX_LONG
                                          || cls == GX_RIFLE) ? 2 : 1;
    static int last_beg = -1;
    {
        static uint16_t cnt[T2R_MAX_PEERS];
        uint32_t vs = eb_vslot < T2R_MAX_PEERS ? eb_vslot : 0;
        g_eb.seed =
            (vs + 1) * 0x27d4eb2fu ^ (uint32_t) (pair +
                                                 1) *
            0x165667b1u ^ (uint32_t) cnt[vs]++ * 0x9e3779b9u;
    }
    int bc = EbPick(0, -1, 0);
    if (bc < 0) {
        Xlog("exec beg: %s skipped - no plea take with a stream in the voice language for %s", GUNEXEC_PAIRS[pair].name, desc);
        g_eb.nshot = 0;
        return;
    }
    last_beg = bc;
    // the plea starts so that the shot cuts it ~80 % in (a short take - "Don't!" 0.26 s - used to end 0.5 s before the shot and leave a silent gap)
    int lead = (int) (VXG_DUR[bc] * 0.8f);
    if (lead > EB_LEAD_MS)
        lead = EB_LEAD_MS;
    if (lead < 180)
        lead = 180;
    int at = (int) g_eb.shot[0] - lead;
    if (at < 80)
        at = 80;
    g_eb.pair = pair;
    g_eb.beg_at = (uint32_t) at;
    g_eb.last_beg = bc;
    g_eb.on = 1;
    g_eb.t0 = PsyncMs() - (double) skip_ms;
    char sl[64] = { 0 };
    for (int i = 0; i < n; i++)
        snprintf(sl + strlen(sl), sizeof sl - strlen(sl), "%s%d",
                 i ? " " : "", g_eb.shot[i]);
    Xlog("exec beg: %s victim %s (%s): plea record %llx starts at %d ms (`say` on the character), cut by stop-vox at the shot at %d ms; %d shot(s) [%s ms]: %s, silence on the death shot at %d ms%s", GUNEXEC_PAIRS[pair].name, g_eb.ent == SID_PLAYER ? "us" : "their puppet", desc, (unsigned long long) VXG_LINES[bc], at, g_eb.shot[0], n, sl, n > 1 ? "bullet-hit grunts after the others" : "no grunts (one shot)", g_eb.shot[n - 1], skip_ms > 0 ? " (late start)" : "");
    if (g_eb.limb >= 0)
        Xlog("dismember scream: %s - shot %d of %d (%d ms) blows the limb off: %s screams in a chain of takes from there until the death shot (%d ms)", GUNEXEC_PAIRS[pair].name, g_eb.limb + 1, n, g_eb.shot[g_eb.limb], desc, g_eb.shot[n - 1]);
}

// The dismemberment scream chain (TLOU2 NPCs scream in a horrifying way, in long continuous / rapid screams, when their limbs are blown
// off): takes of the victim's own voice, started back to back by their voiced length (VXG_DUR, 0.93 of it, so a take's tail rings into the next and
// there is no gap), the intensity falling off as SP's NPC does: the long agony scream (hit-by-explsn, 2-2.6 s) and the rapid short screams
// (fight-take-hvy) first, then the crying (death-whine-lrg) with injured panting (brth-fight-injrd) between.
// SP clip of a shotgun leg dismember: ~0.5-2 s after the hit four short pained cries, from ~2.7 s one long sustained wail. The leg shot of a gun execution
// sits ~1 s before the death shot, so the 2.4 s agony scream started first was cut a second in (mostly its quiet wind-up: heard as no scream): short cries first.
static const int8_t EB_SCR_SEQ[] = { 5, 5, 5, 5, 4, 6, 7, 5, 6, 7, 6, 7 };

static void EbScreamStep(double ems)
{
    int kind =
        EB_SCR_SEQ[g_eb.scr_step <
                   (int) (sizeof EB_SCR_SEQ) ? g_eb.
                   scr_step : (int) (sizeof EB_SCR_SEQ) - 1 -
                   (g_eb.scr_step & 1)];
    int rec = EbPick(kind, g_eb.scr_last, (uint32_t) g_eb.scr_step);
    if (rec < 0) {
        g_eb.scr_on = 0;
        Xlog("dismember scream: no take of kind %d for this voice set - chain ends", kind);
        return;
    }
    g_eb.scr_last = rec;
    uint64_t r = EbSay(rec, kind);
    int dur = VXG_DUR[rec] ? VXG_DUR[rec] : 700;
    g_eb.scr_next = ems + dur * 0.93;
    g_eb.scr_step++;
    g_eb.scr_n++;
    static int nl;
    if (LOGCAP(nl, 120))
        Xlog("dismember scream: take %d at %.0f ms on %s: kind %d record %llx (%d ms voiced) -> say %llx, next at %.0f ms", g_eb.scr_n, ems, g_eb.ent == SID_PLAYER ? "our body" : "their puppet", kind, (unsigned long long) VXG_LINES[rec], dur, (unsigned long long) r, g_eb.scr_next);
}

// every frame (ex_tick)
// the begs felt a little quiet during special executions: the plea / grunts are the engine's `say` (no gain of
// ours), and the victim's execution music (exec_music.c, PCM) runs under them - it is ducked to EB_MUSIC_DUCK from just before the plea to just after
// the last shot (the mixer ramps the gain per block), then back up for its hit on the death screen
#define EB_MUSIC_DUCK 0.35f
static void EbMusicDuck(void)
{
    static int ducked;
    int want = 0;
    if (g_eb.on && g_eb.nshot > 0) {
        double ems = PsyncMs() - g_eb.t0;
        want = ems >= (double) g_eb.beg_at - 100.0
            && ems <= (double) g_eb.shot[g_eb.nshot - 1] + 350.0;
    }
    if (want == ducked || g_exmus.a < 0 || g_exmus.fading
        || g_mixState != 1) {
        if (g_exmus.a < 0)
            ducked = 0;
        return;
    }
    float g =
        EXMUS_LEVEL * ExmusUserVolume() * (want ? EB_MUSIC_DUCK : 1.0f);
    EnterCriticalSection(&g_mixCs);
    int vv[2] = { g_exmus.a, g_exmus.b };
    for (int k = 0; k < 2; k++) {
        int v = vv[k];
        if (v < 0 || !g_mixv[v].on
            || g_mixv[v].src != (k ? g_exmus.l + EXMUS_N : g_exmus.l))
            continue;
        if (k)
            g_mixv[v].tgr = g;
        else
            g_mixv[v].tgl = g;
    }
    LeaveCriticalSection(&g_mixCs);
    ducked = want;
    static int nl;
    if (LOGCAP(nl, 40))
        Xlog("exec beg: execution music %s (gain %.2f)",
             want ? "ducked under the plea" : "back up", (double) g);
}

// the victim's scream was quieter than the executioner's voiceline: the plea is a `say` of a vox-info stream record, which the
// base mix (mix-default) plays 6 dB under the effort events the attacker's grunts are (vox -6 / -6 vs vox-effort -6 / 0). The same `say` call as winmm's
// team lines (FUN_18000b640 = say(speaker, line, 2, 0)), so no other path would be louder; instead a mix snapshot (a spare SP-only record of audio.bin,
// dc_data_patches PLEA_SNAPSHOT: dialogue rows +9 dB, effort / HUD / radio / crowd rows at the base mix) is applied every frame of the plea window
// (set-per-frame-mixer-override, like the stun / blast muffle) on every screen where the plea plays. t2r-no-plea-boost.txt next to the exe: off.
#define EB_PLEA_SNAP 0x334b0a906e6eb50eULL
static void EbPleaBoost(void)
{
    static uint64_t at;
    static int off;
    if (!g_eb.on || FeelFlag("t2r-no-plea-boost.txt", &at, &off, 120))
        return;
    double ems = PsyncMs() - g_eb.t0;
    if (g_eb.nshot <= 0) {      // a melee execution's pleas (execsp.c): the boost covers each plea's own window
        if (GetFileAttributesA("t2r-melee-plea-boost.txt") ==
            INVALID_FILE_ATTRIBUTES)
            return;             // the snapshot sat on the punches (off by default)
        if (g_eb.mw_to <= g_eb.mw_from || ems < g_eb.mw_from - 100.0
            || ems > g_eb.mw_to)
            return;
        uint64_t am[1] = { EB_PLEA_SNAP };
        uint64_t rm =
            DcCall(FNV64("set-per-frame-mixer-override"), 1, am);
        static int nlm;
        if (LOGCAP(nlm, 6))
            Xlog("exec beg: melee plea boost snapshot applied at %.0f ms (set-per-frame-mixer-override -> %llx)", ems, (unsigned long long) rm);
        return;
    }
    // (with a limb shot the scream that follows is boosted too, until the death shot cuts it)
    double until =
        g_eb.limb >=
        0 ? (double) g_eb.shot[g_eb.nshot - 1] +
        60.0 : (double) g_eb.shot[0] + 60.0;
    if (ems < (double) g_eb.beg_at - 100.0 || ems > until)
        return;
    uint64_t a[1] = { EB_PLEA_SNAP };
    uint64_t r = DcCall(FNV64("set-per-frame-mixer-override"), 1, a);
    static int nl;
    if (LOGCAP(nl, 6))
        Xlog("exec beg: plea boost snapshot applied at %.0f ms (set-per-frame-mixer-override -> %llx)", ems, (unsigned long long) r);
}

static void EbTick(void)
{
    EbMusicDuck();
    EbPleaBoost();
    if (!g_eb.on)
        return;
    EbCheck();
    if (MatchSubstate() != 6) {
        EbCut("match left the playing state");
        return;
    }
    double ems = PsyncMs() - g_eb.t0;
    if (ems > 8000.0) {
        EbStopVoice();
        g_eb.on = 0;
        return;
    }                           // (covers the victim voice until the move is over)
    if (!g_eb.beg_done && ems >= (double) g_eb.beg_at) {
        g_eb.beg_done = 1;
        if (ems - (double) g_eb.beg_at < 350.0
            && ems < (double) g_eb.shot[0] - 120.0) {
            uint64_t r = EbSay(g_eb.last_beg, 0);
            static int nl;
            if (LOGCAP(nl, 60))
                Xlog("exec beg: plea on %s at %.0f ms (shot at %d ms): say -> %llx", g_eb.ent == SID_PLAYER ? "our body" : "their puppet", ems, g_eb.shot[0], (unsigned long long) r);
        }
    }
    for (;
         g_eb.shot_i < g_eb.nshot
         && (double) g_eb.shot[g_eb.shot_i] <= ems; g_eb.shot_i++) {
        int i = g_eb.shot_i, last = i == g_eb.nshot - 1;
        double late = ems - (double) g_eb.shot[i];
        static int nl;
        EbStopVoice();        // the shot interrupts whatever the victim was saying / grunting
        if (last) {
            if (g_eb.last_head)
                HsArm(g_eb.ent, 1, 0); // a death shot into the head: SP's headshot death take (downed_vox.c), not the body-shot scream
            if (LOGCAP(nl, 100))
                Xlog("exec beg: shot %d of %d at %.0f ms - the death shot: voice cut, %s", i + 1, g_eb.nshot, ems, g_eb.last_head ? "headshot death take" : "silence");
            if (g_eb.scr_on) {
                Xlog("dismember scream: death shot at %.0f ms - stop-vox, %d take(s) played, silence", ems, g_eb.scr_n);
                g_eb.scr_on = 0;
            }
            continue;
        }
        if (i == g_eb.limb && late < 400.0) {
            g_eb.scr_on = 1;
            g_eb.scr_step = 0;
            g_eb.scr_n = 0;
            g_eb.scr_last = -1;
            EbScreamStep(ems);
            continue;
        }                       // the limb goes: the chain starts
        if (late > 200.0)
            continue;           // a stalled frame: the moment has passed
        int pc =
            EbPick(g_eb.pkind, g_eb.last_pain,
                    100u + (uint32_t) g_eb.shot_i);
        if (pc < 0) {
            if (LOGCAP(nl, 100))
                Xlog("exec beg: shot %d of %d at %.0f ms - voice cut, no hit grunt with a stream in the voice language", i + 1, g_eb.nshot, ems);
            continue;
        }
        g_eb.last_pain = pc;
        uint64_t r = EbSay(pc, g_eb.pkind);
        if (LOGCAP(nl, 100))
            Xlog("exec beg: shot %d of %d at %.0f ms - voice cut, bullet-hit grunt (kind %d, record %llx): say -> %llx", i + 1, g_eb.nshot, ems, g_eb.pkind, (unsigned long long) VXG_LINES[pc], (unsigned long long) r);
    }
    if (g_eb.scr_on && ems >= g_eb.scr_next
        && ems < (double) g_eb.shot[g_eb.nshot - 1])
        EbScreamStep(ems);
}

// ---- melee pleas (begs that do not sound like the victim is about to get shot, more like "Wait wai- ... Ugh, Argh,
// Bleeeh" on the finishing punch). The victim of an unarmed execution begs through the same engine path as the gun pleas (the victim's own NPC voice set, `say` of the
// plea record: 3D, lip-synced), but ONLY from short pleas (WLF "Wait, wait--!" / "No, please--!" ..., Seraphite "Don't!" / "No!" / "No, no!"; the long "No!!" shouts are
// never in the pool) and each one is cut by the next blow (stop-vox), so it ends as "Wait, wai-" + the blow's take-face "Ugh". The state is g_eb with nshot = 0: eb_tick then
// only runs the is-speaking? check, the 8 s cover and the match-state cut. t2r-no-exec-beg.txt (shared with the gun pleas) = off for both, t2r-no-melee-beg.txt = only these.
#define EBM_MAXDUR 1100
static int EbmOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-no-melee-beg.txt", &at, &off, 120);
}

// victim body / pair -> g_eb voice set; 0 = no plea (voice set not known, opt-out)
static int EbmSetup(uint64_t vic, int pair, double t0)
{
    memset(&g_eb, 0, sizeof g_eb);
    g_eb.last_beg = g_eb.last_pain = -1;
    if (EbOff() || EbmOff() || !vic || vic == ~0ULL)
        return 0;
    char desc[96];
    if (!EbVoice(vic, &g_eb.fac, &g_eb.g, &g_eb.v, desc, sizeof desc)) {
        Xlog("exec beg (melee): skipped - victim voice set unknown (%s)",
             desc);
        return 0;
    }
    g_eb.ent = vic;
    {
        static uint16_t cnt[T2R_MAX_PEERS];
        uint32_t vs = eb_vslot < T2R_MAX_PEERS ? eb_vslot : 0;
        g_eb.seed =
            (vs + 1) * 0x2545f491u ^ (uint32_t) (pair +
                                                 1) *
            0x9e3779b1u ^ (uint32_t) cnt[vs]++ * 0x85ebca6bu;
    }
    g_eb.pair = pair;
    g_eb.t0 = t0;
    g_eb.on = 1;
    Xlog("exec beg (melee): victim %s (%s)",
         vic == SID_PLAYER ? "us" : "their puppet", desc);
    return 1;
}

// one short plea take (never `avoid`); -1 = none
static int EbmPick(int avoid, uint32_t salt)
{
    g_ebPickMax = EBM_MAXDUR;
    int r = EbPick(0, avoid, salt);
    g_ebPickMax = 0;
    return r;
}

static void EbmSay(int rec, double ems, double cut)
{
    if (!g_eb.on)
        return;
    uint64_t r = EbSay(rec, 0);
    g_eb.mw_from = ems;
    g_eb.mw_to = cut + 60.0;
    static int nl;
    if (LOGCAP(nl, 60))
        Xlog("exec beg (melee): plea record %llx (%d ms) at %.0f ms on %s, cut by the blow at %.0f ms: say -> %llx", (unsigned long long) VXG_LINES[rec], VXG_DUR[rec], ems, g_eb.ent == SID_PLAYER ? "our body" : "their puppet", cut, (unsigned long long) r);
}

static void EbmCut(double ems)
{
    if (!g_eb.on)
        return;
    EbStopVoice();
    static int nl;
    if (LOGCAP(nl, 60))
        Xlog("exec beg (melee): the blow at %.0f ms cuts the plea", ems);
}

// ---- ps31 FINISHER GASP. SP melee-only run: `request-dialog ai-enemy-melee-pre-strike-shock` on the victim within 16 ms of every finisher intro start (59 of
// 59; it resolves to vox-event-ai-gen-enmy-pre-strike-shock-spj = the victim voice set's own short shocked line, e.g. scrmd-gen-enmy-shckd-spj-014, priority 15 - and
// is dropped as character_busy_speaking when the victim is mid-grunt). MP had no voice at a finisher's start: only the clips' own effort / pain / death events
// (ctx_vox_events.h) and, 1.1 s after the down, the attacker's enemy-down line. The gasp here is ONE short plea take of the victim's own voice set (faction, sex and
// voice letter of the body shown on THIS screen: eb_voice's rule, by slot) through the engine's `say` of the vox-info record (the gun / melee pleas' path: 3D at
// the head, lip-synced, subtitled, in the voice language) - "No!" / "Don't!" / "Wait, wait--!", never the long shouts (VXG_SCREAM), <= EBF_MAXDUR ms. The same take on
// every screen (seed). No PCM. A voice set whose record line was refused on paired bodies this session (g_eb_events) says the begs-for-life effort instead.
// Called at the finisher intro's first frame where an intro plays (attacker: mdu_fw_begin; victim: the SWING message's intro replica) and at the finisher pair's
// start on every screen (ctxdown.c p31v_arm) - once per finisher. t2r-fin-voices-ps30.txt = off (with the attacker's early kill line).
#define EBF_MAXDUR 800          // [TUNING ps31] ms: the gasp is short (the finisher's first blow follows 0.3-0.6 s later)
static int EbfOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-fin-voices-ps30.txt", &at, &off, 120);
}

static void EbFinGasp(uint64_t ent, uint32_t slot, uint32_t seed,
                        const char *why)
{
    static uint64_t last[T2R_MAX_PEERS + 1];
    static int nl;
    uint64_t now = GetTickCount64();
    if (EbOff() || EbfOff() || !ent || ent == ~0ULL
        || MatchSubstate() != 6)
        return;
    uint32_t li = ent == SID_PLAYER ? (uint32_t) T2R_MAX_PEERS : slot;
    if (li > (uint32_t) T2R_MAX_PEERS
        || (last[li] && now - last[li] < 3000))
        return;                 // once per finisher
    if (g_eb.on) {
        if (LOGCAP(nl, 80))
            Xlog("fin voices: no gasp (%s) - an execution's plea state owns the victim voice (ps31)", why);
        return;
    }
    int fac, look, fem, team = 0;
    if (ent == SID_PLAYER) {
        fac = VxOwnFac();
        look = g_fl.cur;
        fem = OwnFemale();
    } else {
        const char *fw;
        if (slot >= T2R_MAX_PEERS)
            return;
        look = FlCoreShown(&g_fl, slot, &team);
        fac = VxFacOfSlot(slot, &fw);
        fem = SlotFemale(slot);
    }
    (void) team;
    if (FactionLooksOff() || (fac != 0 && fac != 1) || look < 0) {
        if (LOGCAP(nl, 80))
            Xlog("fin voices: no gasp (%s) - the victim's voice set is not known here (faction %d, look %d; ps31)", why, fac, look);
        return;
    }
    int g = fem ? 1 : 0, v = VxVoiceOfLook(fac, look);
    if (v < 0 || v > 3)
        return;
    int n = VXG_RANGE[fac][g][v][0][1], first =
        VXG_RANGE[fac][g][v][0][0], cand[16], nc = 0;
    const uint8_t *av = EbAvail();
    for (int pass = 0; pass < 2 && !nc; pass++)
        for (int i = 0; i < n && nc < 16; i++) {
            if (VXG_SCREAM[first + i]
                || (!pass && VXG_DUR[first + i] > EBF_MAXDUR))
                continue;
            if (!av || av[first + i])
                cand[nc++] = first + i;
        }
    last[li] = now;
    uint32_t hs = (seed ^ 0x9e3779b1u) * 2654435761u;
    hs ^= hs >> 15;
    hs *= 2246822519u;
    hs ^= hs >> 13;
    int rec = nc ? cand[hs % (uint32_t) nc] : -1, ev = g_ebEvents
        || rec < 0;
    uint64_t a[4] =
        { ent,
ev ? FNV64("vox-eff-gen-begs-for-life-stun") : VXG_LINES[rec], 2, 0 };
    if (ev) {
        uint64_t fs = FemEffortSid(ent, VXE_PAIN);
        if (fs)
            a[1] = fs;
    }                           // (a female speaker: her own take)
    uint64_t r = DcCall(FNV64("say"), 4, a);
    if (LOGCAP(nl, 80))
        Xlog("fin voices: victim gasp on %s (%s %s voice %c; %s): %s %llx (%d ms) -> say %llx (ps31; SP: pre-strike shock at the intro start)", ent == SID_PLAYER ? "our body" : "their puppet", fac ? "WLF" : "Seraphite", fem ? "female" : "male", 'a' + v, why, ev ? (rec < 0 ? "no short plea take with a stream in the voice language - effort event" : "record lines are refused on paired bodies this session - effort event") : "plea record", (unsigned long long) a[1], !ev ? (int) VXG_DUR[rec] : 0, (unsigned long long) r);
}
