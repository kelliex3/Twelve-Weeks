// Downed players groan and struggle (long pained groans under the breath, struggling while crawling). Every client plays the lines locally for EVERY downed body - our own and each puppet - from
// state it already has (g_downed / MSG_DOWNED's g_peer_downed, the UC4 downed loop's crawl flag), so there is no new message.
//   lying still  : a bleed-out groan every 6-11 s
//   crawling     : an injured breath every 2.6-4.2 s
// Never over another line of that body: is-speaking? is asked first (the downing scream, the plea, a team callout), the next try is 500 ms later.
// Silent from the down until DV_FIRST_MS (the downing scream owns the first moments), during a paired move (execution / revive / tackle), while
// dying, and the moment any of that starts a groan still playing is cut with stop-vox. t2r-no-downed-vox.txt next to the exe: off. Logs 'downed vox: ...'.
#define DV_FIRST_MS 2600
#define DV_N (T2R_MAX_PEERS + 1)        // 0..3 puppets by slot, T2R_MAX_PEERS = our own body
// The engine's generic effort events (vox-eff-gen-*) were silent or the wrong voice on MP bodies, and the SP waves tried next through the
// PCM mixer sounded old and flat. The lines are now vox-info records
// of the mod's greenlight module (tools/features/vox_lines.py gendowned -> vox_downed.h, dc_data_patches.patch_vox_downed) pointing at each MP voice
// set's own takes - <voice>-eff-gen-death-bleed-out-NNN (groans while lying) and <voice>-eff-gen-brth-fight-injrd-NNN (breaths while crawling) -
// played through `say` on the body: 3D at it, the voice bus, the voice of the look the body wears on this screen (like the burning-death screams).
// Cadence (the first version was too fast and too frequent): a groan every 6-11 s while lying, a breath every 2.6-4.2 s while crawling.
#include "vox_downed.h"
static struct {
    uint64_t next, since, spoke_until, ent;
    int act, crawl, last[2];
} g_dv[DV_N];
static int DvOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-no-downed-vox.txt", &at, &off, 120);
}

static void DvCut(int i, const char *why)
{                               // the body stops being a groaning downed body: a line of ours still playing is cut
    uint64_t now = GetTickCount64();
    if (g_dv[i].spoke_until > now && g_dv[i].ent) {
        uint64_t a[1] = { g_dv[i].ent };
        uint64_t r = VoxStop(a[0]);
        static int nl;
        if (LOGCAP(nl, 40))
            Xlog("downed vox: %s - %s, stop-vox -> %llx",
                 i == T2R_MAX_PEERS ? "our body" : "puppet", why,
                 (unsigned long long) r);
    }
    memset(&g_dv[i], 0, sizeof g_dv[i]);
    g_dv[i].last[0] = g_dv[i].last[1] = -1;
}

// the record of a take of `kind` in this body's own voice (0 = not known: looks off / faction not known / that voice has no take of either kind)
static uint64_t DvSid(int i, uint32_t slot, int kind, char *desc,
                       size_t dn)
{
    int own = i == T2R_MAX_PEERS, fac, look, fem;
    if (FactionLooksOff()) {
        snprintf(desc, dn, "faction looks off");
        return 0;
    }
    if (own) {
        fac = VxOwnFac();
        look = g_fl.cur;
        fem = OwnFemale();
    } else {
        int team;
        const char *why;
        look = FlCoreShown(&g_fl, slot, &team);
        fac = VxFacOfSlot(slot, &why);
        fem = SlotFemale(slot);
    }
    if ((fac != 0 && fac != 1) || look < 0) {
        snprintf(desc, dn, "faction %d / look %d not known", fac, look);
        return 0;
    }
    int v = VxVoiceOfLook(fac, look), k = kind;
    if (VXD_RANGE[fac][fem != 0][v][k][1] <= 0)
        k ^= 1;                 // a voice without injured-breath takes (scrfa, scrfd, mltfd) groans instead
    int n = VXD_RANGE[fac][fem != 0][v][k][1];
    if (n <= 0) {
        snprintf(desc, dn, "no takes");
        return 0;
    }
    int t = (int) (T2rRand() % (uint32_t) n);
    if (n > 1 && t == g_dv[i].last[k])
        t = (t + 1) % n;        // never the same take twice in a row
    g_dv[i].last[k] = t;
    int ri = VXD_RANGE[fac][fem != 0][v][k][0] + t;
    snprintf(desc, dn, "%s %s voice %c, %s take %d",
             fac ? "WLF" : "Seraphite", fem ? "female" : "male", 'a' + v,
             k ? "breath" : "groan", t + 1);
    return VXD_LINES[ri];
}

static void DvOne(int i, uint32_t slot, uint64_t ent, int crawl,
                   uint64_t now)
{
    g_dv[i].ent = ent;
    if (!g_dv[i].act) {         // the down just began (or the first poll): the downing scream owns the first moments
        g_dv[i].act = 1;
        g_dv[i].since = now;
        g_dv[i].crawl = crawl;
        g_dv[i].last[0] = g_dv[i].last[1] = -1;
        g_dv[i].next = now + DV_FIRST_MS + (T2rRand() % 2000);
        return;
    }
    if (crawl != g_dv[i].crawl) {       // lying <-> crawling: the other cadence takes over (never sooner than 1.5 s)
        g_dv[i].crawl = crawl;
        uint64_t t = now + 1500 + (T2rRand() % 1500);
        if (t < g_dv[i].next)
            g_dv[i].next = t;
    }
    if (now < g_dv[i].next || now - g_dv[i].since < DV_FIRST_MS)
        return;
    uint64_t sa[1] = { ent };
    uint64_t sp = DcCall(FNV64("is-speaking?"), 1, sa);
    if (sp != ~0ULL && (sp & 0xff)) {
        g_dv[i].next = now + 700;
        return;
    }                           // never over another line of this body (incl. our own last one)
    char desc[96];
    uint64_t sid = DvSid(i, slot, crawl ? 1 : 0, desc, sizeof desc);
    if (!sid) {
        static int nl0;
        if (LOGCAP(nl0, 20))
            Xlog("downed vox: %s - no line (%s)",
                 i == T2R_MAX_PEERS ? "our body" : "puppet", desc);
        g_dv[i].next = now + 3000;
        return;
    }
    uint64_t a[4] = { ent, sid, 2, 0 };
    uint64_t r = DcCall(FNV64("say"), 4, a);
    static int nl;
    if (LOGCAP(nl, 80))
        Xlog("downed vox: %s %s (%llx) -> %llx",
             i == T2R_MAX_PEERS ? "our body" : "puppet", desc,
             (unsigned long long) sid, (unsigned long long) r);
    g_dv[i].spoke_until = now + 3000;
    g_dv[i].next =
        now + (crawl ? 2600 + (T2rRand() % 1600) : 6000 +
               (T2rRand() % 5000));
}

static void DvTick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    static uint64_t last_poll;
    uint64_t now = GetTickCount64();
    if (now - last_poll < 50)
        return;
    last_poll = now;
    int playing = MatchSubstate() == 6;
    if (!playing || DvOff()) {
        for (int i = 0; i < DV_N; i++)
            if (g_dv[i].act || g_dv[i].spoke_until)
                DvCut(i,
                       !playing ? "match left the playing state" :
                       "switched off");
        return;
    }
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        uint8_t *pe = s != me ? ((fn_peer) T2rPeer) (s) : NULL;
        uint64_t h = pe && pe[4] && !pe[5]
            && !IsBadReadPtr(pe, 0x70) ? *(uint64_t *) (pe + 0x60) : 0;
        int ok = h && h != ~0ULL && g_peerDowned[s] == 1
            && !g_puppetDeadHere[s] && !PsyncBusySlot(s)
            && !GxSlotRunning(s) && !g_uc4_pup[s].dying;
        if (ok)
            DvOne((int) s, s, h, g_uc4_pup[s].crawl != 0, now);
        else if (g_dv[s].act || g_dv[s].spoke_until)
            DvCut((int) s,
                   !h ? "no puppet" : g_peerDowned[s] !=
                   1 ? "up / dying / dead" :
                   "execution / revive / death starts");
    }
    {                           // our own body
        int ok = g_downed && !g_downDying && !PsyncActive()
            && !g_uc4_me.dying && !EbCovers() && !(g_gx.until
                                                    && g_t60 < g_gx.until);
        if (ok)
            DvOne(T2R_MAX_PEERS, me, SID_PLAYER, g_uc4_me.crawl != 0,
                   now);
        else if (g_dv[T2R_MAX_PEERS].act
                 || g_dv[T2R_MAX_PEERS].spoke_until)
            DvCut(T2R_MAX_PEERS,
                   g_downed ? "execution / revive / death starts" :
                   "up / dead");
    }
}

// ---------------------------------------------------------------- death by a headshot
// A beg cut off by a headshot played a short screaming / yelping death sound; deaths by headshot should play SP's headshot vox death, not the
// body shot one. The scream came from the engine (the Death-enter vox event on our own body, the NPC death on a puppet) or from the generic death `say`
// events of the execution code. A body that dies by a headshot now says SP's headshot death take - <voice>-eff-gen-death-hdsht-NNN, a short choke, VXD
// kind 2, in the voice of the look the body wears (as the downed groans): what was playing on it (the plea, a grunt, the engine's scream) is cut with
// stop-vox first. No take for that voice (not in the voice language): silence - never the yelp.
// Sources of the headshot: the gun execution's kill shot when it points at the head (exec_beg.c, on every screen), our own head hit on a puppet
// (hit path, di[0x9b4] == 9) that then dies, the downed-kill gore sync of the shooter (head flag; own body and bystanders). Our own body's engine
// scream is withheld at Death enter (paired.c vdf_hook). t2r-no-headshot-vox.txt next to the exe: off. Logs 'headshot death: ...'.
static struct {
    uint64_t ent, at;
} g_hs_mark[8];
static struct {
    uint64_t ent, due;
    int force;
} g_hs_q[6];
static int HsOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-no-headshot-vox.txt", &at, &off, 120);
}

static void HsMark(uint64_t ent)
{
    if (!ent || ent == ~0ULL || HsOff())
        return;
    int f = -1;
    for (int i = 0; i < 8; i++) {
        if (g_hs_mark[i].ent == ent) {
            f = i;
            break;
        }
        if (f < 0 && !g_hs_mark[i].ent)
            f = i;
    }
    if (f < 0)
        f = (int) (GetTickCount64() & 7);
    g_hs_mark[f].ent = ent;
    g_hs_mark[f].at = GetTickCount64();
}

// a hit that was NOT on the head lands on the body: the earlier head mark no longer says how it dies
static void HsUnmark(uint64_t ent)
{
    for (int i = 0; i < 8; i++)
        if (g_hs_mark[i].ent == ent)
            g_hs_mark[i].ent = 0;
}

static int HsRecent(uint64_t ent)
{
    if (HsOff())
        return 0;
    for (int i = 0; i < 8; i++)
        if (g_hs_mark[i].ent == ent)
            return GetTickCount64() - g_hs_mark[i].at < 3000;
    return 0;
}

static void HsArm(uint64_t ent, int force, int delay_ms)
{
    if (!ent || ent == ~0ULL || HsOff())
        return;
    for (int i = 0; i < 6; i++)
        if (g_hs_q[i].ent == ent) {
            if (force)
                g_hs_q[i].force = 1;
            return;
        }
    for (int i = 0; i < 6; i++)
        if (!g_hs_q[i].ent) {
            g_hs_q[i].ent = ent;
            g_hs_q[i].due = GetTickCount64() + (uint64_t) delay_ms;
            g_hs_q[i].force = force;
            return;
        }
}

// A headshot kill has no audible voice, a gargle at most. The hdsht dialogue takes (<voice>-eff-gen-death-hdsht-NNN) are
// voiced (heard as a short yelp), so a headshot death is now: every vocal cut (stop-vox), then at most SP's wet gargle EFFORT event (the neck-wound
// gurgles of the shiv kill) - no scream, no grunt, no dialogue take.
static const char *const HS_GARGLE[] =
    { "vox-eff-gen-death-gurgle-blood-sml",
"vox-eff-gen-death-gurgle-rattle" };
static int HsPlay(uint64_t ent)
{
    if (HsOff() || !ent || ent == ~0ULL)
        return 0;
    static int nl;
    uint64_t sr = VoxStop(ent), r = ~0ULL;
    const char *used = "-";
    for (int i = 0; i < 2 && r == ~0ULL; i++) {
        uint64_t a[4] = { ent, Fnv64Rt(HS_GARGLE[i]), 2, 0 };
#ifndef T2R_RELEASE
        PvoxProbeInit();
        g_pvoxArm = g_pvoxTramp != NULL;
#endif
        r = DcCall(FNV64("say"), 4, a);
#ifndef T2R_RELEASE
        g_pvoxArm = 0;
#endif
        used = HS_GARGLE[i];
    }
    if (LOGCAP(nl, 60))
        Xlog("death vocal: headshot 1 -> voice cut (stop-vox -> %llx), gargle only: %s on %s -> %llx", (unsigned long long) sr, used, ent == SID_PLAYER ? "our body" : "puppet", (unsigned long long) r);
    return 1;
}

static void HsTick(void)
{
    uint64_t now = GetTickCount64();
    for (int i = 0; i < 6; i++) {
        if (!g_hs_q[i].ent || now < g_hs_q[i].due)
            continue;
        uint64_t ent = g_hs_q[i].ent;
        int force = g_hs_q[i].force;
        g_hs_q[i].ent = 0;
        if (MatchSubstate() != 6)
            continue;
        if (force || HsRecent(ent))
            HsPlay(ent);
    }
}
