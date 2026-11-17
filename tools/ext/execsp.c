// ---- execution SP audio layer: unarmed executions need proper SP audio instead of the mockups they had (keeping the 2013 turnover audio), and many special
// executions (not all) felt odd because there is no audio in between the audio called in the execution: for the unarmed
// executions, between the turnover and first punch, it was silent. So: some grunts and other SP melee audio, and begs that do not sound like
// the victim is about to get shot (not the long NOOOOOO scream), more like Wait wai- Ugh, Argh, Bleeeh (on the finishing punch). Only audio gathered
// from SP, no mockups.
// Included by t2r-ext.c right after explus.c (before ex_tick; uses g_ex, the exec_beg.c helpers and the vox helpers).
//
// What:
//  1. Contacts. The three punches of the 2-hand fist executions, the gut kick / boot stomp of the stomp pair and the neck snap used TLOU2 punch WAVES ripped into our PCM mixer
//     (t2m_blob "mockups"). The 2013 timeline's contact moments now fire the SP events of the Abby fist capture natively at the victim: punch
//     sfx-unarmed-hit-face-skin at heada, the killing punch sfx-unarmed-finisher-face-skin, gut kick sfx-unarmed-hit-gut at pelvis, stomp sfx-unarmed-hit-kick-face-skin,
//     neck snap sfx-unarmed-finisher-neckbreak-combat (banks wpn-melee-unarmed + gbl-fight are in the MP level adds). The 2013 TURNOVER audio (grab / push / body fall labels and the
//     whole-move foley stream) is untouched. Old behaviour: t2r-no-exec-sp-audio.txt next to the exe.
//  2. Fillers. The 2013 timeline leaves long silent stretches (the unarmed 3-punch: 0.7 s of turnover tail and 0.55 s between punches carry only the whole-move foley). ES_ROWS puts
//     the SP layers of the capture into them: attacker give-med / give-lrg-short / give-hvy-short at each wind-up (SP +72..209 ms after the move start), the swing foley
//     sfx-gear-swish-cotton at the attacker's wrist / elbow (+170..340) and sfx-unarmed-finisher-whoosh at the wrist for the kill (+437), sfx-pants-swish-cotton at the knee for the
//     kick, the victim's SP reactions take-face / take-med-blnt after the blows, take-med-blnt-gut after the gut kick, meatshield-grabbed-combat at the grab, and
//     death-melee-blunt-large ("bleeh") on the finishing blow. Gun / blade / bat executions get the same kind of layer in their longest silent gaps (victim breath / groan,
//     attacker wind-up effort). Every key was seen resolving in the SP trace or is aliased for the MP voices (dc_data_patches SP_VOX_SUBS).
//  3. Pleas (exec_beg.c ebm_*). The unarmed victim begs between the turnover and the first blow and again before the finishing blow: short interrupted takes of its own
//     voice set ("Wait, wait--!" ... / "Don't!" "No!" "No, no!"), cut by the blow (stop-vox) and followed by take-face "Ugh", ending in the death take. Never the long "No!!" shouts.
//     t2r-no-melee-beg.txt = no pleas.
// Every row runs on the move clock (psync_ms - g_ex.t0), so the attacker, the victim and the observers all play the same layer at the same move time, each on its own screen.
// A row more than 250 ms late is skipped (a late start does not replay old sounds). Log: "exec sp:" lines.
static int MhfxClass(uint64_t w, int *two_h, const char **name);       // melee_hitfx.c: 1 blunt, 2 blade, 0 fists / other
static int EsOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-no-exec-sp-audio.txt", &at, &off, 120);
}

 
enum { ESS_SWISH_RW, ESS_SWISH_LE, ESS_SWISH_RE, ESS_FIN_WHOOSH,
        ESS_KNEE_SWISH, ESS_N };

static const struct {
    const char *ev, *joint;
} ES_SFX[ESS_N] = { 
{ "sfx-gear-swish-cotton", "r_wrist" }, { "sfx-gear-swish-cotton", "l_elbow" }, { "sfx-gear-swish-cotton", "r_elbow" },    // SP: every fist swing (+170..340 ms)
{ "sfx-unarmed-finisher-whoosh", "r_wrist" },   // SP: kill moves (+437 ms)
{ "sfx-pants-swish-cotton", "r_knee" }
};                              // SP: the kick (+437 ms)
enum { ESV_GIVE_MED, ESV_GIVE_LRG, ESV_GIVE_HVY, ESV_TAKE_FACE,
        ESV_TAKE_BLNT, ESV_TAKE_GUT, ESV_GRABBED, ESV_DEATH_BLNT,
        ESV_BREATH, ESV_TAKE_SHRP, ESV_GURGLE, ESV_N };

// first key that answers wins (an unknown key of the speaker's bank returns ~0); vxe = the female pool kind (fem_effort_sid)
    static const struct {
    const char *k[3];
    int vxe;
} ES_VOX[ESV_N] = { 
{ { "vox-eff-gen-fight-give-med", NULL, NULL}, VXE_GIVE }, // SP: light punches, left-1 (+72)
{ { "vox-eff-gen-fight-give-lrg-short", NULL, NULL}, VXE_GIVE },        // SP: attack-3, sprint punch, the kick (+140..175)
{ { "vox-eff-gen-fight-give-hvy-short", NULL, NULL}, VXE_GIVE },        // SP: kill moves (+136..209)
{ { "vox-eff-gen-fight-take-face", NULL, NULL}, VXE_PAIN },     // SP: every face punch, 36 ms after the contact
{ { "vox-eff-gen-fight-take-med-blnt", NULL, NULL}, VXE_PAIN }, // SP: every punch, 3 ms after the contact
{ { "vox-eff-gen-fight-take-med-blnt-gut", NULL, NULL}, VXE_PAIN },     // SP: the gut punch
{ { "vox-eff-gen-meatshield-grabbed-combat", NULL, NULL}, VXE_PAIN },   // SP: the NPC grabbed (re/sp-sandbox: mtshld-grbbd-cmbt takes)
{ { "vox-eff-gen-death-melee-blunt-large", NULL, NULL}, VXE_DEATH },    // SP: killing blunt blow (= take-hvy-blnt takes): the "bleeh"
{ { "vox-eff-gen-breath-fight-injured", "vox-eff-gen-brth-fight-injrd",
   "vox-eff-gen-brth-fight-med"}, VXE_PAIN },

{ { "vox-eff-gen-fight-take-sml-shrp", "vox-eff-gen-fight-take-med-shrp", NULL}, VXE_PAIN },

{ { "vox-eff-gen-death-gurgle-blood-sml", "vox-eff-gen-death-gurgle-rattle", NULL}, VXE_PAIN }
};

enum { EST_SFX, EST_VOX, EST_BEG };

// pair, ms from the clip start, type, who (0 attacker / 1 victim), arg (ES_SFX / ESV id; for a plea: the earliest start, ms; ms = the blow that cuts it)
    typedef struct {
    const char *pair;
    uint16_t ms;
    uint8_t type, who;
    uint16_t arg;
} EsRow;

#define ESF(P) \
{
P, 260, EST_VOX, 1, ESV_GRABBED}, /* the grab (2013 label 266 ms): SP's NPC "grabbed" yelp; the 2013 turnover audio stays as it is */ \

{
P, 1300, EST_BEG, 1, 720}, /* plea 1: between the turnover and the first punch, cut by punch 1 */ \

{
P, 1100, EST_VOX, 0, ESV_GIVE_MED}, /* wind-up of punch 1 (SP give-med) */ \

{
P, 1130, EST_SFX, 0, ESS_SWISH_RW}, /* its swing foley */ \

{
P, 1560, EST_SFX, 0, ESS_SWISH_LE}, /* punch 2 (left hand) */ \

{
P, 1650, EST_VOX, 0, ESV_GIVE_HVY}, \
 {
P, 1870, EST_VOX, 1, ESV_TAKE_BLNT}, /* "Argh" */ \

{
P, 2933, EST_BEG, 1, 2150}, /* plea 2: while the victim is hauled up, cut by the finishing punch */ \

{
P, 2640, EST_SFX, 0, ESS_FIN_WHOOSH}, /* the finisher whoosh (SP kill moves +437) */ \

{
P, 2700, EST_VOX, 0, ESV_GIVE_HVY}, \
 {
P, 2965, EST_VOX, 1, ESV_DEATH_BLNT}, /* "Bleeeh" on the finishing punch (the 2013 timeline's contact 2933 ms) */ 

#define ESS2(P) \
{
P, 420, EST_SFX, 0, ESS_KNEE_SWISH}, /* the gut kick (2013 633 ms): SP kick wind-up */ \

{
P, 470, EST_VOX, 0, ESV_GIVE_LRG}, \
 {
P, 660, EST_VOX, 1, ESV_TAKE_GUT}, /* the victim's gut take after the kick */ \

{
P, 1080, EST_VOX, 1, ESV_TAKE_BLNT}, \
 {
P, 2233, EST_BEG, 1, 1450}, /* plea before the boot, cut by it */ \

{
P, 2020, EST_SFX, 0, ESS_KNEE_SWISH}, /* the stomp's wind-up (2013 boot head 2233 ms) */ \

{
P, 2060, EST_VOX, 0, ESV_GIVE_HVY}, \
 {
P, 2265, EST_VOX, 1, ESV_DEATH_BLNT}, 
static const EsRow ES_ROWS[] = { 

        // ---- gun executions: the victim breathes / groans, the attacker winds up, inside the longest silent gaps of the 2013 timeline (not under a plea or a shot)
    { "pistol-execute-left", 1000, EST_VOX, 1, ESV_BREATH }, 
        { "9mm-execution-dlc5-right", 1000, EST_VOX, 1, ESV_BREATH }, 
        { "9mm-execution-dlc5-left", 850, EST_VOX, 1, ESV_BREATH }, 
        { "execution-revolver-left", 1000, EST_VOX, 1, ESV_BREATH }, 
        { "execution-revolver-left", 2050, EST_VOX, 1, ESV_BREATH }, 
        { "hunting-rifle-execution-dlc5-right", 1000, EST_VOX, 1,
         ESV_BREATH }, 
{ "execution-bow-right", 1850, EST_VOX, 1,
                         ESV_TAKE_SHRP }, 
{ "long-gun-execute", 850,
                                            EST_VOX, 1, ESV_TAKE_BLNT }, 
        // ---- unarmed
    ESF("fist-execute-enter-front") 
        ESF("fist-execute-enter-back") 
        ESS2("fist-stomp-execute-left") 
        ESS2("fist-stomp-execute-right") 
        { "fist-execute-neckbreak-fast-left", 1000, EST_VOX, 1,
             ESV_GRABBED }, { "fist-execute-neckbreak-fast-left", 1330,
                             EST_VOX, 0, ESV_GIVE_HVY },
        
{ "fist-execute-neckbreak-fast-right", 1000, EST_VOX, 1,
          ESV_GRABBED }, { "fist-execute-neckbreak-fast-right", 1330,
                          EST_VOX, 0, ESV_GIVE_HVY },
        
{ "fist-execute-neckbreak-fastest-left", 420, EST_VOX, 1,
          ESV_GRABBED }, { "fist-execute-neckbreak-fastest-left", 660,
                          EST_VOX, 0, ESV_GIVE_HVY },
        
{ "fist-execute-neckbreak-fastest-right", 420, EST_VOX, 1,
          ESV_GRABBED }, { "fist-execute-neckbreak-fastest-right", 660,
                          EST_VOX, 0, ESV_GIVE_HVY }, 
        // ---- blunt (pipe / bat): wind-up efforts ahead of the whooshes, the victim's blunt takes after the blows, "bleeh" on the killing blow
    { "pipe-execute", 1300, EST_VOX, 0, ESV_GIVE_LRG }, { "pipe-execute",
                                                         1545, EST_VOX, 1,
                                                         ESV_TAKE_BLNT },
        { "pipe-execute", 1950, EST_VOX, 1, ESV_TAKE_GUT },
        
{ "pipe-execute", 2430, EST_VOX, 0, ESV_GIVE_HVY },
        { "pipe-execute", 2665, EST_VOX, 1, ESV_DEATH_BLNT },
        
{ "pipe-execute-left", 330, EST_VOX, 0, ESV_GIVE_LRG },
        { "pipe-execute-left", 660, EST_VOX, 1, ESV_TAKE_GUT },
        { "pipe-execute-left", 1750, EST_VOX, 1, ESV_TAKE_BLNT },
        
{ "pipe-execute-left", 2260, EST_VOX, 0, ESV_GIVE_HVY },
        { "pipe-execute-left", 2535, EST_VOX, 1, ESV_DEATH_BLNT }, 
        // ---- blade: wind-up efforts, the victim's sharp take at the stab, a gurgle in the long gap after it
    { "machete-decapitate", 1100, EST_VOX, 1, ESV_BREATH },
        { "machete-decapitate", 1660, EST_VOX, 0, ESV_GIVE_LRG },
        
{ "machete-execute-right", 1330, EST_VOX, 0, ESV_GIVE_LRG },
        { "machete-execute-right", 1610, EST_VOX, 1, ESV_TAKE_SHRP },
        { "machete-execute-right", 1950, EST_VOX, 1, ESV_GURGLE },
        
{ "machete-execute-right", 2640, EST_VOX, 0, ESV_GIVE_MED }, 
};


#define ES_N (sizeof ES_ROWS / sizeof ES_ROWS[0])
    static struct {
    
int on, kind, pair, lo, hi, own_vic, nbeg;
    
 uint32_t done;            // bit per row of the pair already handled
    double t0;
    
 struct {
        int rec;
        double start, cut;
        int said, stopped;
    } beg[3];
    
 int played, skipped;

} g_es;

// the unarmed victim's voice is this timeline's (plea / takes / death): the explus voices and gx_play's pain / death lines stay off while it runs
// the unarmed execution then had no audio except some faint punch layers: taking the victim's voice away from the
// proven explus / gx PCM layers left it near silent - the timeline adds to them now (t2r-exec-sp-owns-voice.txt = it owns the voice again).
static int EsHoldsVictim(void)
{
    
static uint64_t at;
    static int on;
    
uint64_t now = GetTickCount64();
    
if (!at || now - at >= 2000) {
        at = now ? now : 1;
        FILE *f = ExeSideOpen("t2r-exec-sp-owns-voice.txt", "r");
        on = f != NULL;
        if (f)
            fclose(f);
    }
    
return on && g_es.on && g_es.own_vic && g_ex.kind
        && PsyncMs() - g_es.t0 < 4500.0;

}


static const char *EsPairName(int kind, int idx)
{
    
if (kind == 0)
        return (size_t) idx < GX_N ? GUNEXEC_PAIRS[idx].name : NULL;
    
return idx >= 0 && (size_t) idx < MX_N ? MELEE2013[idx].name : NULL;

} 
static int EsUnarmedName(const char *n)
{
    return n && (!strncmp(n, "fist-execute-enter", 18)
                 || !strncmp(n, "fist-stomp", 10)
                 || !strncmp(n, "fist-execute-neckbreak", 22));
}


static void EsBegin(int kind, int idx, float skip_ms)
{
    
memset(&g_es, 0, sizeof g_es);
    
if (EsOff())
        return;
    
const char *nm = EsPairName(kind, idx);
    
if (!nm)
        return;
    
int lo = -1, hi = -1;
    
for (int i = 0; i < (int) ES_N; i++)
        if (!strcmp(ES_ROWS[i].pair, nm)) {
            if (lo < 0)
                lo = i;
            hi = i + 1;
        }
    
if (lo < 0 || hi - lo > 31)
        return;
    
if (kind == 1 && !strncmp(nm, "pipe-execute", 12)) {       // the pipe-class pairs carry the axes / hatchets too: the blunt takes + "bleeh" are for a blunt weapon only
        uint64_t w = PsWeaponNow();
        
if (!w && g_ex.att_ent == SID_PLAYER)
            w = CurrentWeapon();
        
if (!w || MhfxClass(w, NULL, NULL) != 1) {
            Xlog("exec sp: %s: weapon %llx is not a known blunt weapon - no blunt layers", nm, (unsigned long long) w);
            return;
        }
    
}
    
g_es.on = 1;
    g_es.kind = kind;
    g_es.pair = idx;
    g_es.lo = lo;
    g_es.hi = hi;
    g_es.t0 = g_ex.t0;
    
int plea = 0;
    
for (int i = lo; i < hi; i++) {
        
if (ES_ROWS[i].type == EST_BEG)
            plea = 1;
        
if (ES_ROWS[i].type == EST_VOX && ES_ROWS[i].arg == ESV_DEATH_BLNT
             && ES_ROWS[i].who == 1 && kind == 1)
            g_es.own_vic = 1;
    
}
    
int pleas = 0;
    
if (plea && EbmSetup(g_ex.vic_ent, idx, g_ex.t0)) {
        
int prev = -1;
        
for (int i = lo; i < hi && g_es.nbeg < 3; i++) {
            
if (ES_ROWS[i].type != EST_BEG)
                continue;
            
int rec = EbmPick(prev, 7u + (uint32_t) g_es.nbeg * 13u);
            
if (rec < 0) {
                Xlog("exec sp: %s: no short plea take for this voice set (plea %d skipped)", nm, g_es.nbeg + 1);
                continue;
            }
            
prev = rec;
            
double dur =
                (double) VXG_DUR[rec] ? (double) VXG_DUR[rec] : 600.0,
                cut = (double) ES_ROWS[i].ms, start = cut - dur * 0.72;
            
if (start < (double) ES_ROWS[i].arg)
                start = (double) ES_ROWS[i].arg;
            
g_es.beg[g_es.nbeg].rec = rec;
            g_es.beg[g_es.nbeg].start = start;
            g_es.beg[g_es.nbeg].cut = cut;
            g_es.nbeg++;
            pleas++;
    
} 
}
    
Xlog
        ("exec sp: %s (%s pair %d, skip %.0f ms): %d SP rows, %d plea(s)%s",
         nm, kind ? "melee" : "gun", idx, (double) skip_ms, hi - lo, pleas,
         g_es.own_vic ? ", the victim's voice is this timeline's" : "");

} 
static void EsCut(const char *why)
{
    
if (!g_es.on)
        return;
    
static int nl;
    if (LOGCAP(nl, 40))
        Xlog("exec sp: stopped (%s) after %d layers", why, g_es.played);
    
g_es.on = 0;

}


static uint64_t EsEnt(int who)
{
    uint64_t e = who ? g_ex.vic_ent : g_ex.att_ent;
    return e == ~0ULL ? 0 : e;
}


static uint64_t EsSfxAt(const char *ev, uint64_t ent, const char *joint)
{
    
uint64_t a[5] = { Fnv64Rt(ev), ent, Fnv64Rt(joint), 0, 0 };
    
return DcCall(FNV64("play-sound-at-joint"), 5, a);

}


// one voice row: returns 1 when a key answered
static int EsSayRow(int vid, uint64_t ent, int who, double ems)
{
    
static int nl;
    
if (who == 0) {            // the attacker: not on top of its own line (ex_grunt / gx_play share g_att_vox_ms)
        uint64_t now = GetTickCount64();
        
if (g_attVoxMs && now - g_attVoxMs < 250)
            return 0;
        
g_attVoxMs = now;
    
}
    
for (int t = 0; t < 3 && ES_VOX[vid].k[t]; t++) {
        
uint64_t a[4] = { ent, Fnv64Rt(ES_VOX[vid].k[t]), 2, 0 };
        
uint64_t fs = FemEffortSid(ent, ES_VOX[vid].vxe);    // a female speaker: her own take of that kind
        if (fs)
            a[1] = fs;
        
uint64_t r = DcCall(FNV64("say"), 4, a);
        
if (r == ~0ULL)
            continue;
        
if (LOGCAP(nl, 160))
            Xlog("exec sp: %s +%.0f ms: %s on the %s -> %llx",
                 g_ex.kind ==
                 1 ? GUNEXEC_PAIRS[g_ex.pair].name : MELEE2013[g_ex.pair].
                 name, ems, ES_VOX[vid].k[t], who ? "victim" : "attacker",
                 (unsigned long long) r);
        
return 1;
    
}
    
return 0;

}


static void EsTick(double ems)
{
    
if (!g_es.on)
        return;
    
if (!g_ex.kind || g_ex.kind - 1 != g_es.kind
         || g_ex.pair != g_es.pair) {
        g_es.on = 0;
        return;
    }
    
for (int b = 0; b < g_es.nbeg; b++) {
        
if (!g_es.beg[b].said && ems >= g_es.beg[b].start) {
            
g_es.beg[b].said = 1;
            
if (ems - g_es.beg[b].start < 250.0
                 && ems < g_es.beg[b].cut - 60.0) {
                EbmSay(g_es.beg[b].rec, ems, g_es.beg[b].cut);
                g_es.played++;
            }
            
            else
                g_es.beg[b].stopped = 1;
        
}
        
if (g_es.beg[b].said && !g_es.beg[b].stopped
             && ems >= g_es.beg[b].cut) {
            g_es.beg[b].stopped = 1;
            EbmCut(ems);
        }
    
}
    
for (int i = g_es.lo; i < g_es.hi; i++) {
        
uint32_t bit = 1u << (i - g_es.lo);
        
const EsRow *r = &ES_ROWS[i];
        
if ((g_es.done & bit) || r->type == EST_BEG
             || ems < (double) r->ms)
            continue;
        
g_es.done |= bit;
        
if (ems - (double) r->ms > 250.0) {
            g_es.skipped++;
            continue;
        }
        
uint64_t ent = EsEnt(r->who);
        
if (!ent) {
            g_es.skipped++;
            continue;
        }
        
if (r->type == EST_SFX) {
            
uint64_t h =
                EsSfxAt(ES_SFX[r->arg].ev, ent, ES_SFX[r->arg].joint);
            
static int nl;
            if (LOGCAP(nl, 160))
                Xlog("exec sp: %s +%.0f ms: %s at %s on the attacker -> %llx", ES_ROWS[i].pair, ems, ES_SFX[r->arg].ev, ES_SFX[r->arg].joint, (unsigned long long) h);
            
g_es.played++;
        
} else {
            
                // a gun execution's plea / shot grunts own the victim's voice around them; the unarmed timeline owns it itself
                if (r->who == 1 && !g_es.own_vic && EbCoversGun()) {
                g_es.skipped++;
                continue;
            }
            
if (EsSayRow(r->arg, ent, r->who, ems))
                g_es.played++;
            else
                g_es.skipped++;
        
}
    
}

}


// The contact moments of the 2013 timeline (ex_play_label, exec = 1): the SP impact events of the Abby fist capture instead of the ripped punch waves. Returns 1 when
// the event was started. Only the unarmed pairs; the punches' labels are the 2013 hit events (1300 / 1833 / 2933 ms of the 3-punch).
static int EsContact(int label, const char *pair)
{
    
static int nl;
    
if (g_ex.kind != 2 || EsOff() || !EsUnarmedName(pair))
        return 0;
    
        // the native events sounded faint next to the ripped PCM of the same SP punch layers (sfx-unarmed-hit-face-skin), which play by default
        // again; t2r-exec-sp-contacts.txt = the native events
    {
        static uint64_t at;
        static int on;
        uint64_t now = GetTickCount64();
        
if (!at || now - at >= 2000) {
            at = now ? now : 1;
            FILE *f = ExeSideOpen("t2r-exec-sp-contacts.txt", "r");
            on = f != NULL;
            if (f)
                fclose(f);
        }
        
if (!on)
            return 0;
    }
    
const char *ev = NULL, *joint = "heada";
    
double ems = PsyncMs() - g_ex.t0;
    
switch (label) {
    
case EXS_HIT_FIST_FACE:
        ev = "sfx-unarmed-hit-face-skin";
        break;
    
case EXS_HIT_FIST_FACE_FINISHER:
        ev = ems >
            2500.0 ? "sfx-unarmed-finisher-face-skin" :
            "sfx-unarmed-hit-face-skin";
        break;                  // the 2nd blow is no kill: SP plays hit-face-skin for it
    case EXS_GUT_KICK:
        if (strncmp(pair, "fist-stomp", 10))
            return 0;
        ev = "sfx-unarmed-hit-gut";
        joint = "pelvis";
        break;
    
case EXS_HIT_BOOT_HEAD:
    case EXS_HIT_BOOT_FACE:
        ev = "sfx-unarmed-hit-kick-face-skin";
        break;                  // SP: the kick kill
    case EXS_BREAK_NECK:
        ev = "sfx-unarmed-finisher-neckbreak-combat";
        joint = "headb";
        break;
    
default:
        return 0;
    
}
    
uint64_t ent = EsEnt(1);
    
if (!ent)
        return 0;
    
uint64_t r = EsSfxAt(ev, ent, joint);
    
if (LOGCAP(nl, 120))
        Xlog("exec sp: %s +%.0f ms: contact %s -> SP %s at %s on the victim -> %llx", pair, ems, EXSFX_NAME[label], ev, joint, (unsigned long long) r);
    
return r != ~0ULL && r;

}


// intwarp.c: the slowed hold of the fist interrogation moves the exec timelines' clock forward by the time lost (one place, all copies together, so the "same run"
// tests (t0 equality) hold): g_ex.t0 and the copies of it in the Interrogation voice / foley timelines and the SP audio layer. g_ivx.shift = the total, intvox.c
// schedules its lines in real ms (clip ms + shift).
static void IwCopiesShift(double d)
{
    
if (g_ex.kind != 2 || IfoKind(g_ex.pair) != 1)
        return;
    
double o = g_ex.t0;
    
if (g_ivx.t0 == o) {
        g_ivx.t0 += d;
        g_ivx.shift += d;
    }
    
if (g_ifo.t0 == o)
        g_ifo.t0 += d;
    
if (g_es.t0 == o)
        g_es.t0 += d;
    
g_ex.t0 += d;

}


