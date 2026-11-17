// deathreact.c - directional death reactions (tools/re/death_react_tables.py). Included near the end of t2r-ext.c (after paired.c: it
// uses the death hooks' statics: vdf_*, g_vdf_*, g_dd_limp_frame).
//
// The rule, exactly: TLOU2 SP's direction- and weapon-aware death animation plays in TWO cases only
//   (a) a DOWNED player is killed by being SHOT (gun / bow / crossbow shot - not an execution, not the bleed-out, not a melee hit),
//   (b) a STANDING player is killed instantly by the Military Sniper (rifle-milsniper one-shot kill).
// Every other death keeps today's behaviour (the downed deaths ragdoll from the lying pose with no death anim, the standing ones play the
// engine's own death).
//
// Where the clips come from (decoded offline, tools/re/death_react_tables.py): dc1/player-hit-reactions.bin *player-death-reaction-list* (212 rows:
// clip, source-type mask = hit-reaction-source-type-names, direction / state masks) - its bullet-sniper rows are the four
// ellie-death-idle-explosion-{fw,bw,90l,90r} (a player hit by a sniper bullet is thrown by the blast clip of the hit side) and its prone rows
// ellie-death-prone-{left,right,l-shldr} - and dc1/hit-reactions.bin's NPC maps (*thug-death-reactions*: default-med / running / GROUND /
// run-ground / ...), whose clips live in anim-npc-m.pak: npc-m-death-explosion-{fw,bw,lt,rt}-a (the same thrown-by-a-blast family, NPC rig) and
// npc-m-death-ground-{lt,rt} (a thug lying on the ground shot dead). Both families are TLOU2's own clips of stock paks; no UC4 clips.
// Which family plays: the NPC one first (it plays on our Ellie-based body through the engine's NPC -> player retarget exactly like the stun-bomb
// clips, and natively on the puppets), the Ellie one when the NPC pak is not loaded - every clip is probed with get-animation-duration first.
// Direction: where the shooter is around the victim (dir_around, victim's facing): 0 front, 1 behind, 2 left, 3 right. The clip names follow the
// SP convention "<side of the SOURCE>": offline FK of the clips (decoded with the game's own handlers) confirmed the body falls away from that side.
//
// Sync: the victim derives (kind, side, weapon class) from the hit record it just took and announces it (MSG_DEATHREACT, sent 3x before the
// lethal hit); every other screen plays the same clip on the puppet when it kills it. The attacker's screen also knows its own last hit on the
// puppet (dr_note_shot, gore_note_hit) and uses it when the announcement did not arrive. The kill itself is unchanged.
//
// Own body: the clip starts right before the lethal hit; the Death state's anim side is withheld like an execution victim's kept half (paired.c
// hooks 2 - 4 under dr_hold_active(); the death SCREAM, the credit, the death screen (death_ui.c) and the respawn timer are untouched), and
// dr_release_own() ragdolls the body 0.10 s before the clip's end (from the clip's last pose, no pop). Puppets: npc-die with no death anim of
// its own + the clip, the ragdoll hand-over (bleed_rd_tick) is held until the clip ends.
// (c) a player killed by the FLAMETHROWER (flame2013.c: standing or downed - the flame never downs) BURNS TO DEATH on every screen: kind DR_FLAME.
// Clips (decoded offline, hit-reactions.bin *thug-death-reactions* rows npc-m-death-molotov-{fw-a,bw,lt-a,rt-a} = SP's humans set on fire, source type
// fire; player-hit-reactions.bin *player-death-reaction-list* rows ellie-death-fire-{fw,bw,90l,90r}[-prone]): standing = the NPC molotov death of the
// shooter's side (anim-npc-m.pak, already a MP level add), Ellie's fire death when that pak is missing; downed = Ellie's PRONE fire death (anim-ellie-player-
// movement.pak, resident in MP: the prone clips come from it), else the plain NPC ground death. Burning fx on the body: puppets get the engine's own
// npc-set-on-fire (the NPC burn state script ss-fx-npc-burn, fire-* body groups of part-npc-fire), with a manual particle fallback when the engine does not
// light them; our own body gets the same groups spawned by hand (no npc-* call on the player object). Puppets also scream (vox-eff-gen-death-molotov-fast).
// Opt-out: t2r-no-burn-death.txt (flame kills die with no clip / fx / scream of ours). xlog prefix "burndeath:".
// Opt-out: t2r-no-death-react.txt next to the exe (re-read every 2 s). Dev (non-release): t2r-dev-deathreact.txt "<own|slot N> <downed|sniper> [dir 0-3]"
// plays the clip on that body for a visual check, no kill.

#define DR_DOWNED 1
#define DR_SNIPER 2
#define DR_FLAME 3              // burning death (flamethrower kill), standing or downed (prone clips)
#define DR_BLEND 0.12f          // s the clip blends in (from the lying pose / locomotion)
#define DR_RELEASE_BEFORE 0.10f // s before the clip's end: the ragdoll starts (the clip's last pose is the ragdoll's start)
// weapon classes (record + log; the engine's own rows differ by them only for the sniper bullets)
#define DR_WC_NONE 0
#define DR_WC_BULLET 1
#define DR_WC_SHOTGUN 2
#define DR_WC_ARROW 3
#define DR_WC_SNIPER 4
#define DR_WC_FLAME 5
#define DR_WC_MOLO 6            // a MOLOTOV kill (burning death of a standing player; default ON, opt-out t2r-no-burn-death.txt)
static int g_bdMolo;           // set while a molotov burning death is being decided: bd_off / dr_off use the molotov rules

// [family 0 NPC / 1 Ellie][kind - 1][dir 0 fw 1 bw 2 lt 3 rt]
static const char *const DR_CLIPS[2][3][4] = {
    {                           // npc-m (anim-npc-m.pak, base-female rig)
     // downed: npc-m-death-ground-lt / -rt (hit from the left / right; the front shot falls the body back = -lt, the shot from behind forward = -rt)
     { "npc-m-death-ground-lt", "npc-m-death-ground-rt",
      "npc-m-death-ground-lt", "npc-m-death-ground-rt"},
     // sniper: the thrown-back blast family of the hit side
     { "npc-m-death-explosion-fw-a", "npc-m-death-explosion-bw-a",
      "npc-m-death-explosion-lt-a", "npc-m-death-explosion-rt-a"},
     // flame (standing): SP's humans burning to death, the hit side's clip (thug map rows, source type fire)
     // the CARRIER names of tools/features/molo_death_pak.py (anim-npc-m-cover-share.pak with SP's npc-m-death-molotov-{fw-a,bw,lt-a,rt-a} ClipData baked in;
     // clip names in the game are the carrier names). The full anim-npc-m.pak stays out of MP (it crashed); no pak = anim_len 0 -> the Ellie fire death plays.
     { "npc-bow-cover-low-l-hunker-idle",
      "npc-bow-cover-stand-r-hunker-idle",
      "npc-bow-cover-low-r-hunker-idle",
      "npc-bow-cover-stand-l-hunker-idle"} },
    {                           // ellie (anim-ellie-player-movement[-combat].pak, ellie-skel)
     { "ellie-death-prone-left", "ellie-death-prone-right",
      "ellie-death-prone-left", "ellie-death-prone-right"},
     { "ellie-death-idle-explosion-fw", "ellie-death-idle-explosion-bw",
      "ellie-death-idle-explosion-90l", "ellie-death-idle-explosion-90r"},
     { "ellie-death-fire-fw", "ellie-death-fire-bw",
      "ellie-death-fire-90l", "ellie-death-fire-90r"} }
};

// flame, downed: Ellie's prone fire deaths (player-death-reaction-list has fw / bw / 90r prone rows; the 90l side is the mirrored 90r row, which a clip
// call cannot mirror: the left side uses the fw clip), then the plain NPC ground deaths (+ the burning fx)
static const char *const DR_FLAME_PRONE[4] =
    { "ellie-death-fire-fw-prone", "ellie-death-fire-bw-prone",
"ellie-death-fire-fw-prone", "ellie-death-fire-90r-prone" };

static int g_drOff = -1;
static uint64_t g_drOffMs;
static int DrOff(void)
{
    uint64_t now = GetTickCount64();
    if (g_drOff < 0 || now - g_drOffMs > 2000) {
        // SP death animations cut entirely (downed bodies just ragdoll) - opt-IN now: t2r-death-react.txt
        FILE *f = ExeSideOpen("t2r-death-react.txt", "r");
        int off = f == NULL;
        if (f)
            fclose(f);
        if (off != g_drOff)
            Xlog("deathreact: %s",
                 off ?
                 "OFF (default since 10-15; t2r-death-react.txt = on) - every death plays as before"
                 : "ON (t2r-death-react.txt)");
        g_drOff = off;
        g_drOffMs = now;
    }
    return g_drOff;
}

static int DrWclass(uint64_t w)
{
    if (!w)
        return DR_WC_NONE;
    if (FlameIsGun(w))
        return DR_WC_FLAME;
    if (w == 0x2a3164cb4a1c95d9ULL)
        return DR_WC_MOLO;
    uint64_t b = BaseWeapon(w);
    if (b == FNV64("rifle-milsniper"))
        return DR_WC_SNIPER;
    if (b == FNV64("bow-ellie") || b == FNV64("crossbow"))
        return DR_WC_ARROW;
    if (b == FNV64("shotgun-remington-pump")
        || b == FNV64("shotgun-baserri-over-under")
        || b == FNV64("shotgun-tactical") || b == FNV64("shotgun-shorty"))
        return DR_WC_SHOTGUN;
    return BaseWeaponIsGun(w) ? DR_WC_BULLET : DR_WC_NONE;
}

static const char *DrWcName(int wc)
{
    return wc == DR_WC_MOLO ? "molotov" : wc ==
        DR_WC_FLAME ? "flamethrower" : wc ==
        DR_WC_SNIPER ? "sniper" : wc ==
        DR_WC_ARROW ? "bow/crossbow" : wc ==
        DR_WC_SHOTGUN ? "shotgun" : wc == DR_WC_BULLET ? "bullet" : "none";
}
static const char *const DR_DIRN[4] =
    { "front", "behind", "left", "right" };
static const char *DrKindName(int kind)
{
    return kind == DR_FLAME ? "flame" : kind ==
        DR_DOWNED ? "downed" : "sniper";
}

// the character locator of a process (exe 0x133db30: world transform of proc + 0x100): [0..2] pos, [4..7] quat xyzw
static const float *DrLocOf(uint64_t h, float *buf)
{
    typedef void *(*fn_proc)(uint64_t);
    typedef const float *(*fn_loc)(void *, float *);
    if (!h)
        return NULL;
    uint8_t *pr = ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h);
    if (!pr || IsBadReadPtr(pr + 0x100, 0x50))
        return NULL;
    return ((fn_loc) ((uint8_t *) GetModuleHandleW(NULL) + 0x133db30)) (pr
                                                                        +
                                                                        0x100,
                                                                        buf);
}

static const float *DrPeerLoc(uint32_t slot, float *buf)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    if (slot >= T2R_MAX_PEERS)
        return NULL;
    uint8_t *peer = ((fn_peer) T2rPeer) (slot);
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    return DrLocOf(h, buf);
}

// dir of the point `pt` around the body at `loc`: 0 front, 1 behind, 2 left, 3 right (same as dir_around)
static int DrDirOf(const float *loc, const float *pt)
{
    return DirAround(loc, pt);
}

// the first loaded clip of the kind / direction: NPC family, then Ellie. Returns the name (NULL = none loaded), dur = its length
// prone (flame only): the downed body - Ellie's prone fire death first, then the NPC ground death
static const char *DrPick(uint64_t ent, int kind, int dir, int prone,
                           float *dur, int *fam_out)
{
    if (kind < 1 || kind > 3 || dir < 0 || dir > 3)
        return NULL;
    if (kind == DR_FLAME && prone) {
        const char *c[2] =
            { DR_FLAME_PRONE[dir], DR_CLIPS[0][DR_DOWNED - 1][dir] };
        for (int i = 0; i < 2; i++) {
            float len = AnimLen(ent, c[i]);
            if (len > 0.2f) {
                if (dur)
                    *dur = len;
                if (fam_out)
                    *fam_out = i == 0 ? 1 : 0;
                return c[i];
            }
        }
        return NULL;
    }
    // a molotov burning death plays ELLIE's own fire death first (anim-ellie-player-movement.pak: ellie-death-fire-{fw,bw,90l,90r}, player rig, in the pak MP already loads) -
    // the NPC molotov clips live in anim-npc-m.pak, which is NOT an MP level add by default (T2R_NPC_M_PAK=0: crash risk), so they are only the fallback
    // SP's own NPC molotov death (the carrier pak above) first, Ellie's fire death when that pak is not in the build
    for (int fi = 0; fi < 2; fi++) {
        int fam = fi;
        const char *n = DR_CLIPS[fam][kind - 1][dir];
        float len = AnimLen(ent, n);
        if (len > 0.2f) {
            if (dur)
                *dur = len;
            if (fam_out)
                *fam_out = fam;
            return n;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------- state
static struct {
    int phase;                  // 0 idle, 1 clip plays (hold on), 2 released: ragdolled, clip layer being cleared
    int hold;                   // the paired.c hooks defer the Death setup (dr_hold_active)
    int kind, dir, wc, fam, prone, scream;      // scream: the burning scream replaced the engine's death scream (molotov, own body)
    const char *clip;
    uint64_t sid;
    uint64_t t0, until, fail;   // g_t60: start / ragdoll release / hard stop
    float dur;
    uint64_t rag_frame;
    int keep_n;
    uint64_t keep_ms;
    int bit;
} g_dr;
static int DrHoldActive(void)
{
    return g_dr.hold;
}

static int DrScreamReplaced(void)
{
    return g_dr.phase == 1 && g_dr.scream;
}

static int g_drOwnSlow;
// Death enter on our body during a molotov burning death: the engine's death-vox request with the burning-death event (see the burn start above)
static void DrOwnBurnEvent(uint8_t *player)
{
    uint8_t *o = player
        && !IsBadReadPtr(player + 0x2eb0,
                         8) ? *(uint8_t **) (player + 0x2eb0) : NULL;
    o = o ? o + 0x130 : NULL;
    void **vt = o && !IsBadReadPtr(o, 8) ? *(void ***) o : NULL;
    if (!vt || IsBadReadPtr(vt + 0x1f, 8)
        || IsBadCodePtr((FARPROC) vt[0x1f])) {
        Xlog("burndeath: OWN BODY death-vox request object not found - the burning scream is silent");
        return;
    }
    uint64_t ev[16] = { Fnv64Rt(g_drOwnSlow ? "vox-eff-gen-death-molotov-slow" : "vox-eff-gen-death-molotov-fast") };        // (the engine's own request: sid at +0, zeroed payload after it)
    // Engine's own request (0xb579c8..0xb57a39, also 0xb5d0b9 / 0xd81220): +0x50 dword 0xffffffff, +0x54 word 0x0100, +0x56 byte 0, the rest zero.
    ev[10] = 0x0000010000000000ULL | 0xffffffffULL;
    ((void (*)(void *, void *)) vt[0x1f]) (o, ev);
    Xlog("burndeath: OWN BODY burning scream requested through the engine's death-vox path (vfunc 0xf8, event %llx). Why `say` is silent on the own body: not found in the logs - the dialogue path answers 0 / is-speaking 1 yet nothing is heard (player speaker / death mix suspected)", (unsigned long long) ev[0]);
}

static uint8_t g_drEp;
static void DrReset(const char *why)
{
    if (g_dr.phase)
        Xlog("deathreact: own reaction ends (%s)", why);
    memset(&g_dr, 0, sizeof g_dr);
}

// announcements of the other victims / our own last gun hit per puppet
static struct {
    uint8_t kind, dir, wc, ep, prone;
    uint64_t ms;
} g_dr_peer[T2R_MAX_PEERS];
static struct {
    uint64_t w, ms;
    float dmg;
} g_dr_shot[T2R_MAX_PEERS];
static struct {
    uint64_t h, sid, t0, end;
    const char *clip;
    int on, logged, keep_n;
    uint64_t keep_ms;
    int pre, dir;
    uint64_t pre_frame;
    float dur;
} g_dr_pup[T2R_MAX_PEERS];
// a puppet's burning-death clip must be on its layer BEFORE it dies (a clip queued on a dead / dying body is dropped; observed phase 0.00 at every
// check, 8 re-applies) - dr_recv starts it on the victim's announcement (dr_pup_pre), the kill (npc-die after >= 15 net-dead frames / the engine's own) finds it running.
#define DR_PRE_HOLD 12          // frames: the puppet's lethal hp is held at 1 this long after the pre-start (dr_pup_hold)

static void DrPupPre(uint32_t slot);
static void DrRecv(uint32_t from, const uint8_t *buf, uint64_t len)
{
    if (from >= T2R_MAX_PEERS || !buf || len < 4)
        return;
    if (buf[0] < 1 || buf[0] > 3 || buf[1] > 3)
        return;
    uint64_t now = GetTickCount64();
    if (g_dr_peer[from].ms && now - g_dr_peer[from].ms < 6000
        && g_dr_peer[from].ep == buf[3] && g_dr_peer[from].kind == buf[0])
        return;                 // the 3x resend
    g_dr_peer[from].kind = buf[0];
    g_dr_peer[from].dir = buf[1];
    g_dr_peer[from].wc = buf[2];
    g_dr_peer[from].ep = buf[3];
    g_dr_peer[from].ms = now;
    g_dr_peer[from].prone = len >= 5 ? buf[4] : 0;
    if (buf[0] == DR_FLAME && buf[2] == DR_WC_MOLO
        && !g_dr_peer[from].prone)
        VxAllyBurnReact(from);       // the closest teammate who sees it cries out (t2r-ext.c)
    Xlog("deathreact: slot %u announces %s death - shooter %s, weapon class %s%s", from, buf[0] == DR_FLAME ? (buf[2] == DR_WC_MOLO ? "a burning (molotov)" : "a burning (flamethrower)") : buf[0] == DR_DOWNED ? "a downed-shot" : "a sniper-kill", DR_DIRN[buf[1]], DrWcName(buf[2]), g_dr_peer[from].prone ? ", downed (prone)" : "");
    if (buf[0] == DR_FLAME && buf[2] == DR_WC_MOLO
        && !g_dr_peer[from].prone)
        DrPupPre(from);       // the clip takes its layer while the puppet still lives
}

// ---------------------------------------------------------------- burning fx on a flame-kill body
// Puppets: the engine's own NPC ignition (npc-set-on-fire(npc, 0, person-on-fire): SP's melee-pickup-npc-fire / molotov crossfire scripts do exactly this; the
// ss-fx-npc-burn state script then spawns the fire-* body groups of part-npc-fire per the default burn pattern). When npc-is-on-fire? still says no 0.3 s later,
// the same groups are spawned by hand. Our own body: by hand from the start (no npc-* call on the player object).
// The groups / joints are the rows of *default-burn-info-map* (ss-fx/fx-npc-burn-settings.bin): fire-leg-upper at l_upper_leg / r_upper_leg, fire-leg-lower at
// l_knee / r_knee, fire-head at headb, fire-torso-lower at spinea, fire-torso-upper at spined, fire-arm-upper at l_/r_shoulder, fire-arm-lower at l_/r_elbow,
// fire-torso-upper-flame at spined (part-character-fire). The spawn needs the module resident: "part-module/part-npc-fire" is held with the blood modules (feel.c).
#define BD_N (T2R_MAX_PEERS + 1)
#define BD_MAXH 16
// the burning scream (without the forced reverb of SP deaths) is the victim's OWN NPC voice - the
// <voice>-eff-gen-death-fire-fast / -slow-NNN efforts every MP voice set has (militia / scar x male / female x a..d, vox_lines.py genburn -> vox_burn.h,
// vox-info records of the mod's greenlight module like the other efforts), played through `say` on the burning body (3D at it, no SP reverb return): the
// faction / gender / voice letter of the look the victim wears on this screen, fast = a standing burn, slow = downed. (the event name
// vox-eff-gen-death-molotov-fast made no sound in MP; then the Ellie / Abby SP waves through the PCM mixer - those stay ONLY as the fallback when the say fails.)
#include "vox_burn.h"
#include "burnscream.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl bs_blob\nbs_blob:\n.incbin \"burnscream.pcm\"\n.text\n");
extern const int16_t bs_blob[];
static uint64_t g_bsMs[BD_N];
// the victim's own fire-death take: 0 = not known (looks off / no faction yet / no takes) -> the caller falls back
static uint64_t BdVoiceSid(int idx, int slow, char *desc, size_t dn)
{
    static int16_t last[2][2][4][2];
    int own = idx == T2R_US, fac, look, fem;
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
        look = FlCoreShown(&g_fl, (uint32_t) idx, &team);
        fac = VxFacOfSlot((uint32_t) idx, &why);
        fem = SlotFemale((uint32_t) idx);
    }
    if ((fac != 0 && fac != 1) || look < 0) {
        snprintf(desc, dn, "faction %d / look %d not known", fac, look);
        return 0;
    }
    int v = VxVoiceOfLook(fac, look), n =
        VXB_RANGE[fac][fem][v][slow][1];
    if (n <= 0) {
        snprintf(desc, dn, "no takes");
        return 0;
    }
    // A Polish molotov death fell back to a 24 kHz Ellie wave: the fire-death takes are non-verbal efforts, language-neutral. polish.psarc carries none of
    // the 540+ efforts (only english.psarc does) and the engine resolves them from the English archive in every voice language - the same way SP plays its deaths and
    // exec_beg.c's screams. So no per-language filter here; the PCM wave stays the last resort (say refused, bs_verify_tick).
    int first = VXB_RANGE[fac][fem][v][slow][0], cand[32], nc = 0;
    for (int q = 0; q < n && nc < 32; q++)
        cand[nc++] = q;
    (void) first;
    int i = cand[T2rRand() % (uint32_t) nc];
    if (nc > 1 && i == last[fac][fem][v][slow]) {
        int k0 = 0;
        while (cand[k0] != i)
            k0++;
        i = cand[(k0 + 1) % nc];
    }                           // never the same take twice in a row
    last[fac][fem][v][slow] = (int16_t) i;
    snprintf(desc, dn, "%s %s voice %c, %s take %d of %d",
             fac ? "WLF" : "Seraphite", fem ? "female" : "male", 'a' + v,
             slow ? "slow" : "fast", i + 1, n);
    return VXB_LINES[VXB_RANGE[fac][fem][v][slow][0] + i];
}

static uint64_t VoxStop(uint64_t ent);
// Compiled in release builds too (so interrogation-victim refusals always have a reason in the log); it hooks the engine only when t2r-vox-probe.txt exists next to the exe, as before
// Probe (t2r-vox-probe.txt next to the exe, checked at the first burn scream): wraps the engine's VoxController::PlayVoxInternalNoLock (exe+0x331620, the
// gate every line passes; the `say` native always answers 0) and logs ITS result for the burn scream's say: 'vox gate ...'.
// Results (exe 1.6.10721, table in tools/sp-sandbox/t2r-sp.c VOX_RES): 0 played, 2 character_busy_speaking, 19 character_is_dead, 15 invalid_vox_priority ...
typedef uint32_t(*fn_pvox) (void *, const uint8_t *, void *, void *,
                            uint64_t);
static fn_pvox g_pvoxTramp;
static volatile int g_pvoxArm;
static volatile uint32_t g_pvoxRes = 0xffffu, g_pvox_prio;     // the last armed gate result / request priority (intvox.c reads them after its say: the engine's own answer, not the 140 ms is-speaking? guess)
static int g_pvoxForce;        // intvox.c: install the probe without t2r-vox-probe.txt (dev builds)
// every gate answer of the last moments by line key (vp+0x30), so a caller can find ITS answer even when the request is run later than the `say` call (async)
static struct {
    uint64_t key, t;
    uint32_t res, prio;
} g_pvox_ring[32];
static volatile LONG g_pvoxRi;
static int PvoxResultOf(uint64_t key, uint64_t since, uint32_t *res,
                          uint32_t *prio)
{
    int hit = 0;
    for (int i = 0; i < 32; i++)
        if (g_pvox_ring[i].key == key && g_pvox_ring[i].t >= since
            && (!hit || g_pvox_ring[i].t >= g_pvox_ring[hit - 1].t)) {
            *res = g_pvox_ring[i].res;
            *prio = g_pvox_ring[i].prio;
            hit = i + 1;
        }
    return hit != 0;
}

static uint32_t PvoxProbe(void *self, const uint8_t *vp, void *facts,
                           void *conv, uint64_t x5)
{
    uint32_t r = g_pvoxTramp(self, vp, facts, conv, x5);
    if (vp && !IsBadReadPtr(vp, 0x58)) {
        int i = (int) (InterlockedIncrement(&g_pvoxRi) & 31);
        g_pvox_ring[i].key = *(const uint64_t *) (vp + 0x30);
        g_pvox_ring[i].t = GetTickCount64();
        g_pvox_ring[i].res = r;
        g_pvox_ring[i].prio = vp[0x50];
    }
    if (g_pvoxArm && vp && !IsBadReadPtr(vp, 0x58)) {
        g_pvoxRes = r;
        g_pvox_prio = vp[0x50];
    }
    if (g_pvoxArm && vp && !IsBadReadPtr(vp, 0x58))
        Xlog("vox gate PlayVoxInternalNoLock -> %u (0 played, 2 busy speaking, 19 character dead, 21 cannot interrupt), line key %llx, priority %u", r, (unsigned long long) *(const uint64_t *) (vp + 0x30), (unsigned) vp[0x50]);
    return r;
}

// Static decode (exe 1.6.10721, capstone on tlou-ii.exe): `say` -> script request-vox -> PlayRequestVoxInternal 0x32f820 -> by the request's line field [args+8]: 0x330ec0 (a vox-info RECORD, our
// cloned lines) or 0x32fad0 (an EVENT, remapped per character). Both return their VoxPlayResult-like codes through a local (not the return value) and several of them leave BEFORE the gate at 0x331620 the probe
// above sees: 0x330ec0 -> 4 not initialised ([0x3d.. flag]), 5 disabled (controller+0x7c0 & 8), 6 invalid_character_id (controller+0x660 == 0), 7 vox_not_found (record lookup / type != vox-info),
// 13 no_lines_for_this_character (0x32b720(record, controller+0x660) == null), 12 no_lines (matched entry has 0 lines, or the speaker's owner type [+0x48] is not NetCharacter and the request names no line);
// 0x32fad0 -> 9 remap_produced_nil_vox-id (0x327ae0 remap by controller+0x660), 4, 5, 6. So the probes below log, per call, the controller's +0x660 / +0x7c0 and the request's two key fields.
typedef uint64_t(*fn_prv) (void *, void *, void *, void *);     // (rax passed through whole: only the low byte is logged)
static fn_prv g_prvTramp[2];
// The C return value of 0x32fad0 / 0x330ec0 is only a BOOL (r14b: every refusal returns 0 and the refusal REASON stays in a local, [rbp+0x50] of the
// frame, VoxPlayResult numbering = IVX_RES in intvox.c), so 'ret 0' never said why. The frame's result struct ([rbp+0x20]) is destroyed by 0x328f30 right before the return (rcx = it, the code at +0x30), so
// a hook on that destructor sees the real code. Decoded exits of 0x32fad0 (exe 1.6.10721): 0x1f ignored_default_broadcast_not_host, 9 remap nil, 4 / 5 / 6, 7 vox_not_found, 0x16 speech_is_suppressed (0x33a1b0: the vox id
// against the lists of the speaker TEAM, byte [process+0x200]), 8 player_dead (local player down: only 0x16 / 0x19 / 0x1a / 0x1b types), 0xa / 0xb shut_up (now - ctl+0xa8 / +0xb0 under the window, types 1-21), 0x14 queue_reject_same,
// 0x17 / 0x18 / 0x19 idle / murmuration suppressed, 0x13 character_is_dead (owner handle stale or 0x32d6d0 says dead; types 0x1a / 0x1b pass), 0xd no_lines_for_this_character (0x32b720 vs ctl+0x660), 0xe failed_die_roll,
// 0xc no_lines, 0x20 missing_radio_target, 0x1e MISSING_XVAG_SOUND_KEY (the line's stream key unknown to the sound system), 1 = accepted (the gate 0x331620 runs later).
static struct {
    uint64_t t, c660, k0, k1;
    uint32_t r, f7c0, code, own_team, cur_line_lo;
    uint8_t which, c690, code_ok, own_ok;
} g_prv_ring[32];
static volatile DWORD g_prvExitTid;
static volatile uint32_t g_prvExitCode = 0xffffu;     // the last result-struct destructor call (0x328f30): thread id + the VoxPlayResult code at +0x30
static uint64_t(*g_prv_dtor_tramp) (void *);
static uint64_t PrvDtorHook(void *s)
{
    if (s && !IsBadReadPtr((char *) s + 0x30, 4)) {
        g_prvExitCode = *(uint32_t *) ((char *) s + 0x30);
        g_prvExitTid = GetCurrentThreadId();
    }
    return g_prv_dtor_tramp(s);
}

static volatile LONG g_prvRi;
static void PrvNote(int which, void *ctl, void *req, uint64_t r)
{
    int i = (int) (InterlockedIncrement(&g_prvRi) & 31);
    g_prv_ring[i].code_ok = g_prvExitTid == GetCurrentThreadId();
    g_prv_ring[i].code = g_prv_ring[i].code_ok ? g_prvExitCode : 0xffffu;
    g_prvExitTid = 0;
    g_prv_ring[i].c690 = ctl
        && !IsBadReadPtr((char *) ctl + 0x690,
                         1) ? *((uint8_t *) ctl + 0x690) : 0xff;
    g_prv_ring[i].cur_line_lo = ctl
        && !IsBadReadPtr((char *) ctl + 0x280,
                         8) ? (uint32_t) * (uint64_t *) ((char *) ctl +
                                                         0x280) :
        0xffffffffu;
    g_prv_ring[i].own_ok = 0;
    g_prv_ring[i].own_team = 0xffu;     // the speaker's process (controller +0x50 handle -> [+8]) and its team byte [+0x200] (0x33a1b0 / the relation table read it)
    if (ctl && !IsBadReadPtr((char *) ctl + 0x50, 8)) {
        void **h = *(void ***) ((char *) ctl + 0x50);
        if (h && !IsBadReadPtr(h, 16)) {
            char *o = (char *) h[1];
            if (o && !IsBadReadPtr(o + 0x200, 1)) {
                g_prv_ring[i].own_ok = 1;
                g_prv_ring[i].own_team = (uint8_t) o[0x200];
            }
        }
    }
    g_prv_ring[i].t = GetTickCount64();
    g_prv_ring[i].which = (uint8_t) which;
    g_prv_ring[i].r = (uint32_t) (r & 0xff);
    g_prv_ring[i].c660 = ctl
        && !IsBadReadPtr((char *) ctl + 0x660,
                         8) ? *(uint64_t *) ((char *) ctl + 0x660) : ~0ULL;
    g_prv_ring[i].f7c0 = ctl
        && !IsBadReadPtr((char *) ctl + 0x7c0,
                         4) ? *(uint32_t *) ((char *) ctl +
                                             0x7c0) : 0xffffffffu;
    g_prv_ring[i].k0 = req
        && !IsBadReadPtr(req, 16) ? *(uint64_t *) req : ~0ULL;
    g_prv_ring[i].k1 = req
        && !IsBadReadPtr(req, 16) ? *((uint64_t *) req + 1) : ~0ULL;
    // Decode of the event gate (0x32fad0, exe 1.6.10721): rdi = the controller's OWNER handle (ctl+0x50 slot -> [+8] process, ctl+0x58 its generation = [process+0x94]); a mismatch exits 330e19,
    // else the gate calls 0x32d6d0(process): dead = process null OR [process+0xc] & 1 (destroyed - the player kill watch logs +c 07 on a kill) OR the Character IsDead vfunc 0xce0 (0x3f24d0: health comp
    // [process+0x610] vfunc 0x28 = GetHealth() <= 0); only request types 0x1a / 0x1b pass a dead owner. EXIT 19 therefore means: THIS controller's owner process is a destroyed / 0 hp object - not the
    // live body the ext sees. 'vox owner:' logs the controller's owner against our live Player process on every EXIT 19 (the next run names which of the three it is).
    if (which == 1 && g_prv_ring[i].code_ok && g_prv_ring[i].code == 19) {
        static int nv;
        char *o = NULL;
        char *slot = NULL;
        uint32_t gen58 = 0xffffffffu;
        if (ctl && !IsBadReadPtr((char *) ctl + 0x50, 16)) {
            slot = *(char **) ((char *) ctl + 0x50);
            gen58 = *(uint32_t *) ((char *) ctl + 0x58);
            if (slot && !IsBadReadPtr(slot, 16))
                o = *(char **) (slot + 8);
        }
        char *me = (char *) VdfPlayer();
        char od[240], md[240];
        od[0] = md[0] = 0;
        for (int w = 0; w < 2; w++) {
            char *p = w ? me : o;
            char *b = w ? md : od;
            if (!p || IsBadReadPtr(p, 0x850)) {
                snprintf(b, 240, "%s", p ? "unreadable" : "none");
                continue;
            }
            char *comp = *(char **) (p + 0x610);
            uint32_t c40 = 0xffffffffu, c48 = 0xffffffffu;
            float f40 = -1.0f;
            if (comp && !IsBadReadPtr(comp, 0x50)) {
                c40 = *(uint32_t *) (comp + 0x40);
                c48 = *(uint32_t *) (comp + 0x48);
                memcpy(&f40, &c40, 4);
            }
            snprintf(b, 240,
                     "%p vt-player %d id %x flags +a %02x +c %02x +32 %02x state %llx comp %p +40 %08x (%.1f) +48 %08x",
                     (void *) p,
                     *(uint64_t *) p ==
                     (uint64_t) GetModuleHandleW(NULL) + 0x2cfc988ULL,
                     *(uint32_t *) (p + 0x94), (uint8_t) p[0xa],
                     (uint8_t) p[0xc], (uint8_t) p[0x32],
                     (unsigned long long) VdfState((uint8_t *) p),
                     (void *) comp, c40, (double) f40, c48);
        }
        if (nv < 40) {
            nv++;
            Xlog("vox owner: EXIT 19 - ctl %p owner handle slot %p gen +58 %x | OWNER process %s | OUR LIVE Player %s | same process %d, ctl+660 %llx", ctl, (void *) slot, gen58, od, md, o && o == me, (unsigned long long) g_prv_ring[i].c660);
        }
    }
}

static uint64_t PrvProbe0(void *a, void *b, void *c, void *d)
{
    g_prvExitTid = 0;
    uint64_t r = g_prvTramp[0] (a, b, c, d);
    PrvNote(0, a, b, r);
    return r;
}                               // 0x330ec0: the record path

static uint64_t PrvProbe1(void *a, void *b, void *c, void *d)
{
    g_prvExitTid = 0;
    uint64_t r = g_prvTramp[1] (a, b, c, d);
    PrvNote(1, a, b, r);
    return r;
}                               // 0x32fad0: the event path

static const char *PrvExitTxt(uint32_t c)
{
    static char b[8][48];
    static volatile LONG bi;
    static const char *const N[35] =
        { "0 played", "1 queued", "2 character_busy_speaking",
"3 waiting_for_priority_delay", "4 vox_system_not_initialized", "5 disabled",
"6 invalid_character_id", "7 vox_not_found", "8 player_dead",
        "9 remap_nil", "10 character_shut_up", "11 shut_up_murmuration",
            "12 no_lines", "13 no_lines_for_this_character",
            "14 failed_die_roll", "15 invalid_vox_priority",
            "16 missing_metadata", "17 sound_process_failed_to_spawn",
        "18 sound_failed_to_play", "19 character_is_dead",
            "20 queue_reject_same", "21 cannot_interrupt",
            "22 speech_is_suppressed", "23 idle_speech_suppressed",
            "24 murmuration_suppressed", "25 murmuration_disallowed",
        "26 team-allies_not_audible", "27 headset_speech", "28 underwater",
            "29 temp_VO_not_played", "30 MISSING_XVAG_SOUND_KEY",
            "31 ignored_broadcast_default_not_host",
            "32 missing_radio_target", "33 interrupted", "34 stopped"
    };
    if (c < 35)
        return N[c];
    char *o = b[InterlockedIncrement(&bi) & 7];
    snprintf(o, 48, "%u ?", c);
    return o;
}

// what the vox system saw since `since` (GetTickCount64): the PlayRequestVoxInternal calls (and their bool) and every gate call (line key, result, priority) - intvox.c logs it when a native line does not start
static void PvoxDump(uint64_t since, char *out, size_t on)
{
    size_t k = 0;
    out[0] = 0;
    int nr = 0, ng = 0;
    for (int i = 0; i < 32; i++)
        if (g_prv_ring[i].t >= since && g_prv_ring[i].t)
            nr++;
    for (int i = 0; i < 32; i++)
        if (g_pvox_ring[i].t >= since && g_pvox_ring[i].t)
            ng++;
    k += (size_t) snprintf(out + k, on - k,
                           "%s; vox-info/event calls %d, gate calls %d:",
                           g_prvTramp[0] ? "request probes on" :
                           "request probes OFF", nr, ng);
    for (int i = 0; i < 32 && k + 150 < on; i++)
        if (g_prv_ring[i].t >= since && g_prv_ring[i].t)
            k += (size_t) snprintf(out + k, on - k,
                                   " [%s +%llu ms ret %u EXIT %s ctl+660 %llx ctl+7c0 %x ctl+690 %x ctl+280 %x speaker team %d req %llx %llx]",
                                   g_prv_ring[i].
                                   which ? "event 32fad0" :
                                   "record 330ec0",
                                   (unsigned long long) (g_prv_ring[i].t -
                                                         since),
                                   g_prv_ring[i].r,
                                   g_prv_ring[i].
                                   code_ok ? PrvExitTxt(g_prv_ring[i].
                                                          code) :
                                   "unseen (destructor hook off)",
                                   (unsigned long long) g_prv_ring[i].c660,
                                   g_prv_ring[i].f7c0, g_prv_ring[i].c690,
                                   g_prv_ring[i].cur_line_lo,
                                   g_prv_ring[i].
                                   own_ok ? (int) g_prv_ring[i].
                                   own_team : -1,
                                   (unsigned long long) g_prv_ring[i].k0,
                                   (unsigned long long) g_prv_ring[i].k1);
    for (int i = 0; i < 32 && k + 80 < on; i++)
        if (g_pvox_ring[i].t >= since && g_pvox_ring[i].t)
            k += (size_t) snprintf(out + k, on - k,
                                   " [gate %llx +%llu ms -> %u prio %u]",
                                   (unsigned long long) g_pvox_ring[i].key,
                                   (unsigned long long) (g_pvox_ring[i].t -
                                                         since),
                                   g_pvox_ring[i].res,
                                   g_pvox_ring[i].prio);
}

static void PvoxProbeInit(void)
{
    static int failed, file_tried;
    if (g_pvoxTramp || failed)
        return;
    if (!g_pvoxForce) {        // (only the file decides for the other callers; intvox.c forces it on in dev builds)
        if (file_tried)
            return;
        file_tried = 1;
        FILE *f = ExeSideOpen("t2r-vox-probe.txt", "r");
        if (!f)
            return;
        fclose(f);
    }
    failed = 1;                 // (cleared below once installed: a prologue mismatch is not retried)
    // the function starts with a REX-prefixed push (40 55 = push rbp), so the old 20-byte pattern (55 53 ...) never matched and the probe was never
    // installed ('HOOK FAIL: burndeath: vox probe - PlayVoxInternalNoLock prologue differs' in every log): every 'engine gate' answer was missing.
    // 40 55 53 56 57 41 54 41 55 41 56 41 57 (push rbp,rbx,rsi,rdi,r12..r15, 13 bytes) + lea rbp,[rsp-0x398] (8): position independent, 21 bytes copied
    static const uint8_t pro[21] =
        { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x68, 0xFC, 0xFF, 0xFF };
    uint8_t *fn = (uint8_t *) GetModuleHandleW(NULL) + 0x331620;
    if (IsBadReadPtr(fn, 21) || memcmp(fn, pro, 21)) {
        HOOK_MISS(fn, 21,
                  "burndeath: vox probe - PlayVoxInternalNoLock prologue differs, not hooked");
        return;
    }
    uint8_t *t =
        VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE);
    if (!t)
        return;
    memcpy(t, fn, 21);
    t[21] = 0xFF;
    t[22] = 0x25;
    *(uint32_t *) (t + 23) = 0;
    *(uint64_t *) (t + 27) = (uint64_t) (fn + 21);
    g_pvoxTramp = (fn_pvox) t;
    DWORD old;
    VirtualProtect(fn, 21, PAGE_EXECUTE_READWRITE, &old);
    fn[0] = 0xFF;
    fn[1] = 0x25;
    *(uint32_t *) (fn + 2) = 0;
    *(uint64_t *) (fn + 6) = (uint64_t) PvoxProbe;
    for (int i = 14; i < 21; i++)
        fn[i] = 0x90;
    VirtualProtect(fn, 21, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 21);
    failed = 0;
    {                           // (the victim's `say` never reached PlayVoxInternalNoLock): wrap the two callers above the gate (see prv_probe0 / 1). 21 / 19-byte position-independent prologues.
        static const uint8_t pa[21] =
            { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41,
0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x28, 0xFF, 0xFF, 0xFF };
        static const uint8_t pb[19] =
            { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
0x41, 0x57, 0x48, 0x81, 0xEC, 0x20, 0x02, 0x00, 0x00 };
        static const struct {
            uint32_t rva;
            const uint8_t *pro;
            int n;
            void *hook;
        } H[2] =
            { { 0x330ec0, pa, 21, (void *) PrvProbe0 }, { 0x32fad0, pb,
                                                          19,
                                                          (void *)
                                                          PrvProbe1 } };
        for (int h = 0; h < 2; h++) {
            uint8_t *f2 = (uint8_t *) GetModuleHandleW(NULL) + H[h].rva;
            int n = H[h].n;
            if (IsBadReadPtr(f2, (SIZE_T) n)
                || memcmp(f2, H[h].pro, (size_t) n)) {
                HOOK_MISS(f2, (size_t) n,
                          "burndeath: request-vox probe - prologue differs, not hooked");
                continue;
            }
            uint8_t *t2 =
                VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
            if (!t2)
                continue;
            memcpy(t2, f2, (size_t) n);
            t2[n] = 0xFF;
            t2[n + 1] = 0x25;
            *(uint32_t *) (t2 + n + 2) = 0;
            *(uint64_t *) (t2 + n + 6) = (uint64_t) (f2 + n);
            g_prvTramp[h] = (fn_prv) t2;
            DWORD o2;
            VirtualProtect(f2, (SIZE_T) n, PAGE_EXECUTE_READWRITE, &o2);
            f2[0] = 0xFF;
            f2[1] = 0x25;
            *(uint32_t *) (f2 + 2) = 0;
            *(uint64_t *) (f2 + 6) = (uint64_t) H[h].hook;
            for (int q = 14; q < n; q++)
                f2[q] = 0x90;
            VirtualProtect(f2, (SIZE_T) n, o2, &o2);
            FlushInstructionCache(GetCurrentProcess(), f2, (SIZE_T) n);
            Xlog("burndeath: request-vox probe %d on (rva %x)", h,
                 (unsigned) H[h].rva);
        }
    }
    {                           // the result-struct destructor 0x328f30 (20-byte position-independent prologue: 3 rsp-relative stores, push rdi, sub rsp,0x60): the refusal code at +0x30 of its rcx
        static const uint8_t pd[20] =
            { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x60 };
        uint8_t *f3 = (uint8_t *) GetModuleHandleW(NULL) + 0x328f30;
        if (IsBadReadPtr(f3, 20) || memcmp(f3, pd, 20))
            HOOK_MISS(f3, 20,
                      "burndeath: request-vox exit-code probe - 0x328f30 prologue differs, not hooked");
        else {
            uint8_t *t3 =
                VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
            if (t3) {
                memcpy(t3, f3, 20);
                t3[20] = 0xFF;
                t3[21] = 0x25;
                *(uint32_t *) (t3 + 22) = 0;
                *(uint64_t *) (t3 + 26) = (uint64_t) (f3 + 20);
                g_prv_dtor_tramp = (uint64_t(*)(void *)) t3;
                DWORD o3;
                VirtualProtect(f3, 20, PAGE_EXECUTE_READWRITE, &o3);
                f3[0] = 0xFF;
                f3[1] = 0x25;
                *(uint32_t *) (f3 + 2) = 0;
                *(uint64_t *) (f3 + 6) = (uint64_t) PrvDtorHook;
                for (int q = 14; q < 20; q++)
                    f3[q] = 0x90;
                VirtualProtect(f3, 20, o3, &o3);
                FlushInstructionCache(GetCurrentProcess(), f3, 20);
                Xlog("burndeath: request-vox exit-code probe on (rva 328f30)");
            }
        }
    }
    Xlog("burndeath: vox probe on - the engine's result of every burn scream's say is logged");
}

static int g_bsSeries;         // A later scream of the burn series (bd_tick): not held back by the one-scream rule, no PCM fallback
static int BdPcm(int idx, uint64_t ent, int female, const char *who);
static struct {
    uint64_t ent, due;
    int stage, slow, female, series;
} g_bsv[BD_N];
static int BdScream(int idx, uint64_t ent, int female, int slow,
                     const char *who)
{
    uint64_t now = GetTickCount64();
    if (idx < 0 || idx >= BD_N || !ent)
        return 0;
    if (!g_bsSeries && g_bsMs[idx] && now - g_bsMs[idx] < 5000) {
        Xlog("burndeath: %s scream already played %llu ms ago - one scream per death", who, (unsigned long long) (now - g_bsMs[idx]));
        return 0;
    }
    char desc[96];
    uint64_t sid = BdVoiceSid(idx, slow, desc, sizeof desc);
    if (sid) {
        // root cause (disassembly, exe 0x331620 -> 0x338210): the gate compares the request's priority with the line the speaker is saying NOW and answers
        // 2 character_busy_speaking when that one is not lower - while `say` itself reports 0 either way. The burning body is mid-line (the engine's death scream /
        // hit grunt, SP's event is priority 26; our cloned take records carry the team-call template's low one), so the take was refused silently. The headshot
        // death take (downed_vox.c hs_play), heard in the same test, cuts the speaker with stop-vox first - the same here.
        uint64_t sr = VoxStop(ent);
#ifndef T2R_RELEASE
        PvoxProbeInit();
        g_pvoxArm = g_pvoxTramp != NULL;
#endif
        uint64_t a[4] = { ent, sid, 2, 0 };
        uint64_t r = DcCall(FNV64("say"), 4, a);
#ifndef T2R_RELEASE
        g_pvoxArm = 0;
#endif
        (void) sr;
        // `say` answers 0 when it accepts the event (every other say caller of the mod logs '-> 0' and is heard), ~0 = no such event. the earlier test
        // `r && r != ~0` sent every started scream to the PCM fallback (Ellie / Abby, 19.2 kHz, 2D) and a44d2099 (0 = ok) was silent in testing because the picked
        // takes (scrfa fast-004, mltfb fast-004) have no stream in english.psarc: vox_lines.py genburn now lists only takes with audio (126 records).
        if (r != ~0ULL) {
            g_bsMs[idx] = now;
            g_bsv[idx].ent = ent;
            g_bsv[idx].stage = 1;
            g_bsv[idx].due = now + 160;
            g_bsv[idx].slow = slow;
            g_bsv[idx].female = female;
            g_bsv[idx].series = g_bsSeries;
            Xlog("burndeath: %s scream = the victim's own NPC fire-death effort (%s) %llx through `say` at the body -> %llx", who, desc, (unsigned long long) sid, (unsigned long long) r);
            return 1;
        }
        Xlog("burndeath: %s own-voice fire-death effort (%s) %llx: say answered %llx - FALLBACK to the TLOU2 SP wave through the PCM mixer", who, desc, (unsigned long long) sid, (unsigned long long) r);
    } else
        Xlog("burndeath: %s no own-voice fire-death effort (%s) - FALLBACK to the TLOU2 SP wave through the PCM mixer", who, desc);
    return BdPcm(idx, ent, female, who);
}

// the SP fire-death wave (Ellie / Abby) through the PCM mixer: the fallback when the voice system plays no own take
// Ellie / Abby screams (Abby pitched down) were still heard on molotov deaths: another character's voice is worse than no wave, so the wave is
// opt-IN (t2r-burn-pcm.txt). 18 fallbacks in player logs were all 'no own-voice fire-death effort' (the Polish filter, removed in 6f51df32).
static int BdPcm(int idx, uint64_t ent, int female, const char *who)
{
    uint64_t now = GetTickCount64();
    static uint64_t pcm_at;
    static int pcm_on;
    if (!FeelFlag("t2r-burn-pcm.txt", &pcm_at, &pcm_on, 120)) {
        g_bsMs[idx] = now;
        Xlog("burndeath: %s no own-voice scream - Ellie / Abby wave fallback is off (t2r-burn-pcm.txt turns it on)", who);
        return 0;
    }
    int grp = female ? 0 : 1, cand[BS_N], nc = 0;
    for (int i = 0; i < BS_N; i++)
        if (BS_WAVE[i].grp == grp)
            cand[nc++] = i;
    if (!nc)
        return 0;
    static unsigned pick;
    int w = cand[(pick++ + T2rRand()) % (unsigned) nc];
    int rate = grp ? (int) (BS_RATE * BS_MALE_RATE) : BS_RATE;
    int r =
        PcmPlayAt(bs_blob + BS_WAVE[w].off, (int) BS_WAVE[w].n, 1.0f,
                    rate, ent);
    g_bsMs[idx] = now;
    Xlog("burndeath: %s scream (fallback) = TLOU2 fire-death wave #%d (%s voice, %d Hz, %.1f s) through the PCM mixer at the body -> %d", who, w, female ? "female (Ellie)" : "male (Abby, pitched down)", rate, (double) BS_WAVE[w].n / rate, r);
    return r != 0;
}

// verification (the `say` native answers 0 whether the line started or the voice gate refused it): 160 ms after a take is said, is-speaking? on the body
// is asked; "no" -> the SP event itself (vox-eff-gen-death-molotov-fast / -slow, what SP requests: prio 26) -> still "no" 160 ms later -> the PCM wave.
static void BsVerifyTick(void)
{
    uint64_t now = GetTickCount64();
    for (int i = 0; i < BD_N; i++) {
        if (!g_bsv[i].stage || now < g_bsv[i].due)
            continue;
        uint64_t ent = g_bsv[i].ent, a1 = ent, r =
            DcCall(FNV64("is-speaking?"), 1, &a1);
        int spk = r != ~0ULL && (r & 0xff);
        Xlog("burndeath: slot %d is-speaking? %d ms after the %s -> %llx",
             i, 160, g_bsv[i].stage == 1 ? "own take" : "SP event",
             (unsigned long long) r);
        if (spk || r == ~0ULL) {
            g_bsv[i].stage = 0;
            continue;
        }
        if (g_bsv[i].stage == 1) {
            uint64_t a[4] =
                { ent,
        FNV64(g_bsv[i].
              slow ? "vox-eff-gen-death-molotov-slow" :
              "vox-eff-gen-death-molotov-fast"), 2, 0 };
#ifndef T2R_RELEASE
            g_pvoxArm = g_pvoxTramp != NULL;
#endif
            uint64_t sr = DcCall(FNV64("say"), 4, a);
#ifndef T2R_RELEASE
            g_pvoxArm = 0;
#endif
            Xlog("burndeath: own take not speaking - SP event vox-eff-gen-death-molotov-%s said -> %llx", g_bsv[i].slow ? "slow" : "fast", (unsigned long long) sr);
            g_bsv[i].stage = 2;
            g_bsv[i].due = now + 160;
        } else {
            // screams 2-4 of the series were refused by the gate while the first one (SP event) was still sounding, and each went to the PCM wave =
            // the second, pitched-down "Abby" scream on top of the voice's own. A later scream of the series is never replaced by a wave: one voice, one gender.
            if (g_bsv[i].series)
                Xlog("burndeath: slot %d series scream refused (previous line still sounding) - no PCM wave on top of the voice's own scream", i);
            else
                BdPcm(i, ent, g_bsv[i].female,
                       "slot (voice refused both)");
            g_bsv[i].stage = 0;
        }
    }
}

static const struct {
    const char *p, *j;
} BD_FX[] = {
    { "fire-torso-upper", "spined" }, { "fire-torso-lower", "spinea" },
        { "fire-head", "headb" }, { "fire-arm-upper", "l_shoulder" },
        { "fire-arm-upper", "r_shoulder" },
    { "fire-arm-lower", "l_elbow" }, { "fire-arm-lower", "r_elbow" },
        { "fire-leg-upper", "l_upper_leg" }, { "fire-leg-upper",
                                              "r_upper_leg" },
    { "fire-leg-lower", "l_knee" }, { "fire-leg-lower", "r_knee" },
        { "fire-torso-upper-flame", "spined" }
};

static struct {
    uint64_t ent, t0, until;
    int on, chk, manual, nh, nsc, molo;
    uint64_t h[BD_MAXH];
} g_bd[BD_N];
// a series of screams, as burn victims have in SP, instead of a one-off scream: SP's burning NPC cries out again and again (its on-fire
// state script, not the clip: the molotov clips' event tables only hold foot / gear foley) - fast cries first, slow choking ones as it goes down. Seconds after the start
// of the burn, slow flag. Puppets only (our own screen is black / spectating at that time).
static const struct {
    float t;
    int slow;
} BD_SERIES[] = { { 1.4f, 0 }, { 2.7f, 0 }, { 3.9f, 1 }, { 5.0f, 1 } };

static int g_bdOff = -1;
static uint64_t g_bdOffMs;
static int g_bdKeep;           // dev trigger on our own (living) body: the fire is not put out by "we are alive again"
static int BdmOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-no-burn-death.txt", &at, &off, 120);
}                               // molotov burning death: opt-OUT

static int BdOff(void)
{
    if (g_bdMolo)
        return BdmOff();
    uint64_t now = GetTickCount64();
    if (g_bdOff < 0 || now - g_bdOffMs > 2000) {
        FILE *f = ExeSideOpen("t2r-burn-death.txt", "r");     // SP death animations cut - opt-IN (t2r-burn-death.txt)
        int off = f == NULL;
        if (f)
            fclose(f);
        if (off != g_bdOff)
            Xlog("burndeath: %s",
                 off ?
                 "OFF (default since 10-15; t2r-burn-death.txt = on) - flamethrower kills die with no burning clip / fx / scream"
                 : "ON (t2r-burn-death.txt)");
        g_bdOff = off;
        g_bdOffMs = now;
    }
    return g_bdOff;
}

static void BdManual(int idx, const char *why)
{
    if (g_bd[idx].manual)
        return;
    g_bd[idx].manual = 1;
    int ok = 0;
    for (size_t i = 0;
         i < sizeof BD_FX / sizeof *BD_FX && g_bd[idx].nh < BD_MAXH; i++) {
        uint64_t r =
            ArmorFxSpawn(FNV64(BD_FX[i].p), g_bd[idx].ent, BD_FX[i].j);
        if (r && r != ~0ULL) {
            g_bd[idx].h[g_bd[idx].nh++] = r;
            ok++;
        }
    }
    Xlog("burndeath: %s %d: %d of %zu burn particle groups spawned by hand (%s)", idx == T2R_US ? "own body" : "slot", idx == T2R_US ? 0 : idx, ok, sizeof BD_FX / sizeof *BD_FX, why);
}

// idx: slot of the puppet, or T2R_US for our own body. secs: how long the body burns
static void BdStart(int idx, uint64_t ent, float secs)
{
    if (idx < 0 || idx >= BD_N || !ent || BdOff())
        return;
    if (idx != T2R_US && g_bd[idx].on && g_bd[idx].ent == ent
        && g_t60 < g_bd[idx].until)
        return;                 // already burning since the pre-start (dr_pup_pre)
    memset(&g_bd[idx], 0, sizeof g_bd[idx]);
    g_bd[idx].molo = g_bdMolo; // the scream series is a MOLOTOV death's only
    g_bd[idx].on = 1;
    g_bd[idx].ent = ent;
    g_bd[idx].t0 = g_t60 ? g_t60 : 1;
    g_bd[idx].until = g_bd[idx].t0 + S60(secs);
    if (idx == T2R_US) {
        BdManual(idx, "own body");
        return;
    }
    uint64_t a[3] = { ent, 0, FNV64("person-on-fire") };
    uint64_t r = DcCall(FNV64("npc-set-on-fire"), 3, a);
    uint64_t p1 = ent, on = DcCall(FNV64("npc-is-on-fire?"), 1, &p1);
    Xlog("burndeath: slot %d puppet %llx - npc-set-on-fire -> %llx, npc-is-on-fire? right after %d (a hand-spawned fallback follows if it is still no 0.3 s later)", idx, (unsigned long long) ent, (unsigned long long) r, (int) (on != ~0ULL && (on & 0xff)));
    // the puppet's scream (no engine death anim on it: the Ellie / NPC clip carries none we know of): the SP burning-NPC death vox
    BdScream(idx, ent, SlotFemale((uint32_t) idx), 0, "slot puppet");
}

// feature_puppet_no_burn (t2r-ext.c): the engine fire we lit on a puppet for its burning death is not put out while that death plays (log: npc-set-on-fire ->
// is-on-fire 1, then 'burn: slot 0 puppet put out (alive)' 3 frames later - the net-dead flag of the victim is still 0 - and '+0.3 s: npc-is-on-fire? 0')
static int BdPuppetBurning(uint32_t slot)
{
    return slot < T2R_MAX_PEERS && g_bd[slot].on && slot != T2R_US
        && g_t60 < g_bd[slot].until;
}

static void BdTick(void)
{
    BsVerifyTick();
    for (int i = 0; i < BD_N; i++) {
        if (!g_bd[i].on)
            continue;
        uint64_t el = g_t60 - g_bd[i].t0;
        int own = i == T2R_US;
        if (MatchSubstate() != 6
            || (own && !g_bdKeep && el > 60 && LocalHp() > 0))
            g_bd[i].until = g_t60;      // left the match / we respawned: the fire goes out now
        // the burn (~8 s) can outlast the dying puppet (wave respawn re-creates it): no npc-* call on a handle that is no
        // longer this slot's live puppet (crash dumps showed torn-down player objects) - the burn just ends
        if (!own) {
            typedef uint8_t *(*fn_peer)(uint32_t);
            typedef void *(*fn_proc)(uint64_t);
            uint8_t *pe = ((fn_peer) T2rPeer) ((uint32_t) i);
            uint64_t cur = pe
                && !IsBadReadPtr(pe, 0x68) ? *(uint64_t *) (pe + 0x60) : 0;
            if (cur != g_bd[i].ent
                || !((fn_proc) T2R_SYM(kFnEntityFromHandle)) (g_bd[i].
                                                              ent)) {
                if (g_t60 < g_bd[i].until)
                    Xlog("burndeath: slot %d puppet %llx gone / replaced (now %llx) - burn ends, no npc calls", i, (unsigned long long) g_bd[i].ent, (unsigned long long) cur);
                g_bd[i].chk = 1;
                g_bd[i].until = g_t60;
            }
        }
        // Molotov screams played like 5 times on top of each other for ages like a low quality sound; only the first
        // real one should play. The series stacked 4 more 3 s screams on the first (in Polish every one fell to the PCM wave), so it is off; the first scream
        // (bd_start) stays. t2r-burn-series.txt next to the exe brings the series back.
        static uint64_t bss_at;
        static int bss_on;
        if (!own && g_bd[i].molo
            && FeelFlag("t2r-burn-series.txt", &bss_at, &bss_on, 120)
            && g_bd[i].nsc < (int) (sizeof BD_SERIES / sizeof *BD_SERIES)
            && g_t60 < g_bd[i].until
            && el >= (uint64_t) S60(BD_SERIES[g_bd[i].nsc].t)) {
            int k = g_bd[i].nsc++;
            g_bsSeries = 1;
            int ok =
                BdScream(i, g_bd[i].ent, SlotFemale((uint32_t) i),
                          BD_SERIES[k].slow, "slot puppet (series)");
            g_bsSeries = 0;
            Xlog("burndeath: slot %d scream %d of %zu (%s) at %.1f s of the burn -> %s", i, k + 1, sizeof BD_SERIES / sizeof *BD_SERIES, BD_SERIES[k].slow ? "slow" : "fast", (double) el / 60.0, ok ? "played" : "not played");
        }
        if (!own && !g_bd[i].chk && el >= 18) {
            g_bd[i].chk = 1;
            uint64_t p1 = g_bd[i].ent, on =
                DcCall(FNV64("npc-is-on-fire?"), 1, &p1);
            int fire = on != ~0ULL && (on & 0xff);
            Xlog("burndeath: slot %d puppet %llx +0.3 s: npc-is-on-fire? %d", i, (unsigned long long) g_bd[i].ent, fire);
            BdManual(i,
                      fire ?
                      "the engine burn state is on - the hand groups ride with it (the A/B 10-20 video showed them, the engine groups unproven in MP)"
                      : "the engine did not light the puppet");
        }
        if (g_t60 >= g_bd[i].until) {
            for (int k = 0; k < g_bd[i].nh; k++) {
                uint64_t a[2] = { g_bd[i].h[k], 0 };
                DcCall(FNV64("kill-particles"), 2, a);
            }
            Xlog("burndeath: %s %d burn ends (%d hand-spawned groups killed)", own ? "own body" : "slot", own ? 0 : i, g_bd[i].nh);
            memset(&g_bd[i], 0, sizeof g_bd[i]);
            if (own)
                g_bdKeep = 0;
        }
    }
}

static void DrNoteShot(uint32_t slot, uint64_t w, float dmg)
{
    if (slot >= T2R_MAX_PEERS)
        return;
    g_dr_shot[slot].w = w;
    g_dr_shot[slot].dmg = dmg;
    g_dr_shot[slot].ms = GetTickCount64();
}

// ---------------------------------------------------------------- our own body
static int DrOwnBeginI(int kind, uint64_t w, uint32_t att_slot,
                          float dmg, int prone);
static int DrOwnBegin(int kind, uint64_t w, uint32_t att_slot, float dmg,
                        int prone)
{
    g_bdMolo = kind == DR_FLAME && DrWclass(w) == DR_WC_MOLO;
    int r = DrOwnBeginI(kind, w, att_slot, dmg, prone);
    g_bdMolo = 0;
    return r;
}

static int DrOwnBeginI(int kind, uint64_t w, uint32_t att_slot,
                          float dmg, int prone)
{
    if ((!g_bdMolo && DrOff()) || g_dr.phase || MatchSubstate() != 6)
        return 0;
    int wc = DrWclass(w);
    if (kind == DR_FLAME) {
        if ((wc != DR_WC_FLAME && wc != DR_WC_MOLO) || BdOff())
            return 0;
    }                           // (c) the flamethrower: a burning death
    else if (kind ==
             DR_DOWNED ? (wc == DR_WC_NONE
                          || wc == DR_WC_FLAME) : wc != DR_WC_SNIPER)
        return 0;               // (a) any gun / bow / crossbow shot, (b) the Military Sniper only
    const char *why =
        kind == DR_FLAME ? (wc ==
                            DR_WC_MOLO ?
                            "standing player burned alive by a molotov" :
                            prone ?
                            "downed player burned to death by the flamethrower"
                            :
                            "standing player burned to death by the flamethrower")
        : kind ==
        DR_DOWNED ? "downed player shot dead" :
        "standing player killed by the Military Sniper";
    float lb[16];
    const float *vl = Uc4MyLoc(lb);
    float ab[16];
    const float *al =
        att_slot < T2R_MAX_PEERS ? DrPeerLoc(att_slot, ab) : NULL;
    if (!vl || (!al && kind != DR_FLAME)) {
        Xlog("deathreact: %s - no direction (shooter slot %u, locators %s / %s) - normal death", why, att_slot, vl ? "ok" : "none", al ? "ok" : "none");
        return 0;
    }
    int dir = al ? DrDirOf(vl, al) : 0;       // (a flame kill without a shooter locator burns as if hit from the front)
    float dur = 0;
    int fam = 0;
    const char *clip = DrPick(SID_PLAYER, kind, dir, prone, &dur, &fam);
    if (!clip) {
        Xlog("deathreact: %s - no death clip loaded for %s / %s (tried %s, %s) - normal death", why, DrKindName(kind), DR_DIRN[dir], kind == DR_FLAME && prone ? DR_FLAME_PRONE[dir] : DR_CLIPS[0][kind - 1][dir], kind == DR_FLAME && prone ? DR_CLIPS[0][DR_DOWNED - 1][dir] : DR_CLIPS[1][kind - 1][dir]);
        return 0;
    }
    if ((kind == DR_DOWNED || prone) && g_uc4Down)
        Uc4Stop(&g_uc4_me);    // the UC4 lying loop's own clip: the death clip takes the layer
    uint64_t r = AnimateBlend(SID_PLAYER, clip, DR_BLEND);
    if (!r || r == ~0ULL) {
        Xlog("deathreact: %s - animate_(%s) failed (%llx) - normal death",
             why, clip, (unsigned long long) r);
        return 0;
    }
    memset(&g_dr, 0, sizeof g_dr);
    g_dr.phase = 1;
    g_dr.hold = 1;
    g_dr.kind = kind;
    g_dr.dir = dir;
    g_dr.wc = wc;
    g_dr.fam = fam;
    g_dr.clip = clip;
    g_dr.sid = FNV64(clip);
    g_dr.dur = dur;
    g_dr.prone = prone;
    g_dr.t0 = g_t60 ? g_t60 : 1;
    float rel = dur - DR_RELEASE_BEFORE;
    g_dr.until = g_dr.t0 + S60(rel > 0.1f ? rel : 0.1f);
    g_dr.fail = g_dr.t0 + S60(dur + 1.5f);
    uint8_t b[5] =
        { (uint8_t) kind, (uint8_t) dir, (uint8_t) wc, (uint8_t)++ g_drEp,
 (uint8_t) (prone ? 1 : 0) };
    if (g_sendTramp)
        for (int i = 0; i < 3; i++)
            g_sendTramp(MSG_DEATHREACT, b, 5, 0xff);
    Xlog("deathreact: OWN %s - shooter slot %u %s (weapon %llx = %s, hit %.0f): %s clip %s on our body -> %llx, %.2f s, ragdoll %.2f s in; announced (epoch %u)", why, att_slot, DR_DIRN[dir], (unsigned long long) w, DrWcName(wc), dmg, fam ? "ellie" : "npc", clip, (unsigned long long) r, dur, rel, b[3]);
    if (kind == DR_FLAME && wc == DR_WC_MOLO) {
        // the burning scream (SP's burning NPC death vox; the event resolves to our own voice set, male / female) REPLACES the engine's death scream:
        // it plays now, the Death-enter hook (paired.c vdf_hook) then withholds the normal one. Only when our say went through, else the normal scream stays.
        // Observed: `say` at the player (the dialogue path) answered 0 and is-speaking? 1 yet was inaudible on the victim's own screen, while the engine's own
        // death scream is audible there: the engine's death event is posted by Death enter (paired.c vdf_hook -> 0x140b57990) through [player+0x2eb0]+0x130 vfunc 0xf8.
        // The burning scream is that SAME request with SP's burning-death event (vox-eff-gen-death-molotov-fast / -slow) in place of vox-event-all-death, made from
        // the hook (dr_own_burn_event): same speaker, same bus, the body's own voice set. Nothing is said here.
        g_dr.scream = 1;
        g_drOwnSlow = prone;
        g_bsMs[T2R_US] = GetTickCount64();
        Xlog("burndeath: OWN BODY burning scream = SP's %s event through the engine's own death-vox request at Death enter (the normal death scream is swapped, not played)", prone ? "molotov-slow" : "molotov-fast");
    }
    if (kind == DR_FLAME) {
        Xlog("burndeath: OWN BODY - burning death %s (%s), %s clip %s, %.2f s", prone ? "downed / prone" : "standing", DR_DIRN[dir], fam ? "ellie" : "npc", clip, dur);
        BdStart(T2R_US, SID_PLAYER, dur + 0.4f);
    }
    return 1;
}

static void DrOwnTick(void)
{
    if (!g_dr.phase)
        return;
    if (MatchSubstate() != 6) {
        DrReset("match over");
        return;
    }
    uint64_t now = g_t60;
    if (g_dr.phase == 1) {
        // a clip layer the Death enter dropped (round 2 of re/factions2013/exec-victim-death.md: the layer bookkeeping lied) comes back at the right phase
        float ph = PsyncPhase(SID_PLAYER, g_dr.sid);
        double el = (double) (now - g_dr.t0) / 60.0;
        // get-animation-phase answers 0.000 (not -1) for a layer the Death enter dropped (paired.c psync_keep_tick, every 1007/1008 log) - the old
        // "< 0" test never fired, so the downed-shot clip sat at phase 0.00 for its whole length and the body kept the lying pose until the ragdoll
        if ((ph < 0.001f) && el > 0.15 && el < (double) g_dr.dur - 0.25
            && g_dr.keep_n < 8 && GetTickCount64() - g_dr.keep_ms >= 250) {
            g_dr.keep_ms = GetTickCount64();
            g_dr.keep_n++;
            uint64_t r =
                AnimateFrom(SID_PLAYER, g_dr.clip, 0.05f,
                             (float) (el / g_dr.dur));
            Xlog("deathreact: own clip %s dropped (layer phase %.2f) at %.2f s - re-applied at phase %.2f -> %llx (#%d)", g_dr.clip, ph, el, el / g_dr.dur, (unsigned long long) r, g_dr.keep_n);
        }
        static uint64_t last_log;
        if (now - last_log >= 30 && (now - g_dr.t0) % 30 < 2 && el < (double) g_dr.dur) {       // evidence: the clip plays through the Death state
            last_log = now;
            Xlog("deathreact: own clip %s at %.2f s: layer phase %.2f, state %llx, hp %.1f", g_dr.clip, el, ph, (unsigned long long) VdfState(VdfPlayer()), LocalHp());
        }
        if (now >= g_dr.until || now >= g_dr.fail) {
            int bit = -1;
            if (DrReleaseOwn
                (now >= g_dr.fail ? "failsafe" : "clip end", &bit)) {
                g_dr.phase = 2;
                g_dr.hold = 0;
                g_dr.rag_frame = g_frame ? g_frame : 1;
                g_dr.bit = bit;
                if (g_downDying) {
                    g_vdf_ragged = 1;
                    g_dd_limp_frame = g_frame ? g_frame : 1;
                }               // the downed hand-over (dd_limp_tick) as after its own ragdoll
                Xlog("deathreact: own clip %s over (phase %.2f, %.2f s) - Death setup run, death controller ragdoll (vfunc 0x38), ragdoll bit %d", g_dr.clip, ph, el, bit);
            } else if (now >= g_dr.fail) {
                Xlog("deathreact: own body never reached the Death state (state %llx, hp %.1f) - hold dropped", (unsigned long long) VdfState(VdfPlayer()), LocalHp());
                DrReset("no Death state");
            }
        }
        return;
    }
    // phase 2: the clip layer is cleared right after the ragdoll call (a scripted clip would hold the pose), the bit is logged
    uint64_t el = g_frame - g_dr.rag_frame;
    if (el == 1) {
        uint64_t a[5] = { SID_PLAYER, ANIM_LAYER, 0, 0, 0 };
        uint64_t r = DcCall(FNV64("stop-animating"), 5, a);
        AnimEndBlend(SID_PLAYER, g_dr.clip, 0.0f);
        uint8_t *me = VdfPlayer();
        Xlog("deathreact: own clip layer cleared (-> %llx), ragdoll bit %d", (unsigned long long) r, me ? (me[0x9fd] & 1) : -1);
    }
    if (el == 30 || el == 120) {
        uint8_t *me = VdfPlayer();
        Xlog("deathreact: own body +%llu frames after the ragdoll: bit %d, state %llx", (unsigned long long) el, me ? (me[0x9fd] & 1) : -1, (unsigned long long) VdfState(me));
    }
    if (el >= 120)
        DrReset("done");
}

// ---------------------------------------------------------------- puppets
static int DrPupBusy(uint32_t slot, uint64_t h)
{
    return slot < T2R_MAX_PEERS && g_dr_pup[slot].on
        && g_dr_pup[slot].h == h && g_t60 < g_dr_pup[slot].end + 90;
}

static int DrPuppetBeginI(uint32_t slot, uint64_t h, int from_down,
                             int exec_death, uint64_t * hold_t60);
static int DrPuppetBegin(uint32_t slot, uint64_t h, int from_down,
                           int exec_death, uint64_t *hold_t60)
{
    int r = DrPuppetBeginI(slot, h, from_down, exec_death, hold_t60);
    g_bdMolo = 0;
    return r;
}

static int DrPuppetBeginI(uint32_t slot, uint64_t h, int from_down,
                             int exec_death, uint64_t *hold_t60)
{
    if (hold_t60)
        *hold_t60 = 0;
    if (slot >= T2R_MAX_PEERS || !h || exec_death)
        return 0;
    if (g_finisherFrame && g_t60 - g_finisherFrame < 300)
        return 0;               // our finisher on them plays: the paired move kills them
    if (GxBusySlot(slot) || PsyncBusySlot(slot))
        return 0;               // an execution plays on them
    uint64_t nowm = GetTickCount64();
    int kind = 0, dir = -1, wc = 0, prone = from_down;
    const char *src = "";
    if (g_dr_peer[slot].ms && nowm - g_dr_peer[slot].ms < 6000
        && (g_dr_peer[slot].kind == (from_down ? DR_DOWNED : DR_SNIPER)
            || g_dr_peer[slot].kind == DR_FLAME)) {
        kind = g_dr_peer[slot].kind;
        dir = g_dr_peer[slot].dir;
        wc = g_dr_peer[slot].wc;
        src = "the victim's announcement";
        if (kind == DR_FLAME)
            prone = from_down || g_dr_peer[slot].prone;
    } else if (g_dr_shot[slot].ms && nowm - g_dr_shot[slot].ms < 1500) {
        wc = DrWclass(g_dr_shot[slot].w);
        kind =
            wc == DR_WC_FLAME ? DR_FLAME : from_down ? (wc !=
                                                        DR_WC_NONE ?
                                                        DR_DOWNED : 0)
            : (wc == DR_WC_SNIPER ? DR_SNIPER : 0);
        if (kind) {
            float pb[16], ob[16];
            const float *pl = DrLocOf(h, pb);
            const float *ol = Uc4MyLoc(ob);   // we shot it: the shooter is us
            if (pl && ol) {
                dir = DrDirOf(pl, ol);
                src = "our own last hit on them";
            } else if (kind == DR_FLAME) {
                dir = 0;
                src = "our own flame (no locator: front)";
            } else
                kind = 0;
        }
    }
    g_bdMolo = kind == DR_FLAME && wc == DR_WC_MOLO && !from_down;     // molotov burning death: standing victims only, default on
    if (kind == DR_FLAME && wc == DR_WC_MOLO && from_down)
        kind = 0;               // (a downed victim keeps today's death)
    if (!g_bdMolo && DrOff())
        kind = 0;
    if (kind == DR_FLAME && BdOff())
        kind = 0;               // t2r-no-burn-death.txt: a molotov kill / (opt-in t2r-burn-death.txt) flame kill dies with no clip of ours
    g_dr_peer[slot].ms = 0;
    if (!kind || dir < 0) {
        static int nl;
        if (LOGCAP(nl, 40))
            Xlog("deathreact: slot %u died (%s) - no hit record for a death reaction (announcement %s, our last hit %s) - normal death", slot, from_down ? "from the downed state" : "standing", g_dr_peer[slot].kind ? "of the other kind / stale" : "none", g_dr_shot[slot].ms ? "not a kill weapon / stale" : "none");
        return 0;
    }
    g_dr_shot[slot].ms = 0;
    float dur = 0;
    int fam = 0;
    const char *clip = DrPick(h, kind, dir, prone, &dur, &fam);
    if (!clip) {
        Xlog("deathreact: slot %u %s death - no clip loaded for %s / %s - normal death", slot, kind == DR_FLAME ? "burning" : kind == DR_DOWNED ? "downed-shot" : "sniper", DrKindName(kind), DR_DIRN[dir]);
        return 0;
    }
    // the victim's announcement already started this clip on the living puppet (dr_pup_pre) - it is on its layer, do not queue it again on the dying body
    int reuse = g_dr_pup[slot].pre && g_dr_pup[slot].h == h
        && g_dr_pup[slot].clip == clip && g_t60 - g_dr_pup[slot].t0 < 600;
    uint64_t r = reuse ? 1 : AnimateBlend(h, clip, DR_BLEND);
    if (!r || r == ~0ULL) {
        Xlog("deathreact: slot %u puppet %llx - animate_(%s) failed (%llx) - normal death", slot, (unsigned long long) h, clip, (unsigned long long) r);
        return 0;
    }
    uint64_t t0 = reuse ? g_dr_pup[slot].t0 : (g_t60 ? g_t60 : 1);
    if (reuse)
        Xlog("deathreact: slot %u puppet %llx - the clip %s has been playing since the announcement (%llu frames ago): layer phase %.3f at the kill", slot, (unsigned long long) h, clip, (unsigned long long) (g_t60 - t0), PsyncPhase(h, FNV64(clip)));
    else {
        uint64_t p2 = h, dd =
            DcCall(0xe93f704f02a9ecd5ULL /* character-is-dead? */ , 1,
                    &p2);
        Xlog("deathreact: slot %u puppet %llx - NO pre-start (announcement late / none): the clip is queued on a body that is %s", slot, (unsigned long long) h, (dd != ~0ULL && (dd & 0xff)) ? "ALREADY DEAD" : "alive");
    }
    g_dr_pup[slot].pre = 0;
    float rel = dur - DR_RELEASE_BEFORE;
    g_dr_pup[slot].h = h;
    g_dr_pup[slot].sid = FNV64(clip);
    g_dr_pup[slot].clip = clip;
    g_dr_pup[slot].t0 = t0;
    g_dr_pup[slot].end = t0 + S60(rel > 0.1f ? rel : 0.1f);
    g_dr_pup[slot].on = 1;
    g_dr_pup[slot].logged = 0;
    g_dr_pup[slot].keep_n = 0;
    g_dr_pup[slot].keep_ms = 0;
    if (hold_t60)
        *hold_t60 = g_dr_pup[slot].end;
    Xlog("deathreact: slot %u puppet %llx %s - shooter %s (%s, weapon class %s): %s clip %s -> %llx, %.2f s, ragdoll hand-over %.2f s in", slot, (unsigned long long) h, kind == DR_FLAME ? (prone ? "burning death (downed / prone)" : "burning death") : kind == DR_DOWNED ? "downed-shot death" : "sniper-kill death", DR_DIRN[dir], src, DrWcName(wc), fam ? "ellie" : "npc", clip, (unsigned long long) r, dur, rel);
    if (kind == DR_FLAME) {
        Xlog("burndeath: slot %u puppet %llx - burning death %s (%s), %s clip %s, %.2f s", slot, (unsigned long long) h, prone ? "downed / prone" : "standing", DR_DIRN[dir], fam ? "ellie" : "npc", clip, dur);
        BdStart((int) slot, h, dur + 0.4f);
    }
    return 1;
}

// the victim announced a molotov burning death (dr_recv, before the lethal hit reaches us): start the clip on the puppet NOW, while it lives, and scream. The
// puppet's lethal hp is held at 1 for DR_PRE_HOLD frames (dr_pup_hold, the set-health hook) so the engine's own kill does not land before the clip has its
// layer; the real death follows through feature_remote_deaths (npc-die, >= 15 net-dead frames) / the dead-here edge, which finds the clip running.
static void DrPupPre(uint32_t slot)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    if (slot >= T2R_MAX_PEERS || MatchSubstate() != 6 || !g_winmm)
        return;
    g_bdMolo = 1;
    int off = BdOff();
    if (off) {
        g_bdMolo = 0;
        return;
    }
    uint8_t *peer = ((fn_peer) T2rPeer) (slot);
    uint64_t h = peer
        && !IsBadReadPtr(peer, 0x68) ? *(uint64_t *) (peer + 0x60) : 0;
    if (!h || !peer[4] || peer[5] || g_peerDowned[slot]
        || GxBusySlot(slot) || PsyncBusySlot(slot) || (g_finisherFrame
                                                           && g_t60 -
                                                           g_finisherFrame
                                                           < 300)) {
        g_bdMolo = 0;
        return;
    }
    if (g_dr_pup[slot].pre && g_dr_pup[slot].h == h
        && g_t60 - g_dr_pup[slot].t0 < 600) {
        g_bdMolo = 0;
        return;
    }
    uint64_t p2 = h, dd =
        DcCall(0xe93f704f02a9ecd5ULL /* character-is-dead? */ , 1, &p2);
    if (dd != ~0ULL && (dd & 0xff)) {
        Xlog("deathreact: slot %u puppet %llx - the announcement came after the puppet died here (no pre-start possible)", slot, (unsigned long long) h);
        g_bdMolo = 0;
        return;
    }
    int dir = g_dr_peer[slot].dir;
    float dur = 0;
    int fam = 0;
    const char *clip = DrPick(h, DR_FLAME, dir, 0, &dur, &fam);
    g_bdMolo = 0;
    if (!clip)
        return;
    uint64_t r = AnimateBlend(h, clip, DR_BLEND);
    if (!r || r == ~0ULL) {
        Xlog("deathreact: slot %u puppet %llx - pre-start animate_(%s) failed (%llx)", slot, (unsigned long long) h, clip, (unsigned long long) r);
        return;
    }
    uint64_t t0 = g_t60 ? g_t60 : 1;
    float rel = dur - DR_RELEASE_BEFORE;
    g_dr_pup[slot].h = h;
    g_dr_pup[slot].sid = FNV64(clip);
    g_dr_pup[slot].clip = clip;
    g_dr_pup[slot].t0 = t0;
    g_dr_pup[slot].end = t0 + S60(rel > 0.1f ? rel : 0.1f);
    g_dr_pup[slot].on = 1;
    g_dr_pup[slot].logged = 0;
    g_dr_pup[slot].keep_n = 0;
    g_dr_pup[slot].keep_ms = 0;
    g_dr_pup[slot].pre = 1;
    g_dr_pup[slot].pre_frame = g_frame ? g_frame : 1;
    g_dr_pup[slot].dir = dir;
    g_dr_pup[slot].dur = dur;
    Xlog("deathreact: slot %u puppet %llx PRE-START on the announcement (alive): %s clip %s -> %llx, %.2f s; lethal hp held %d frames, scream now", slot, (unsigned long long) h, fam ? "ellie" : "npc", clip, (unsigned long long) r, dur, DR_PRE_HOLD);
    g_bdMolo = 1;              // (bd_off: the molotov rule, opt-out file)
    BdStart((int) slot, h, dur + 0.4f);        // the fire + the first scream now (the series follows in bd_tick); the kill is held until the clip ends
    g_bdMolo = 0;
}

// the set-health hook (t2r-ext.c): hold a lethal hp write on this slot's puppet while its pre-started clip gets its layer
static int DrPupHold(uint32_t slot)
{
    // not only while the clip gets its layer - until the whole SP burning death has played (dr_burn_defer_for holds winmm's kill of the puppet, exkill_tick ends it)
    return slot < T2R_MAX_PEERS && g_dr_pup[slot].pre
        && (g_frame - g_dr_pup[slot].pre_frame < DR_PRE_HOLD
            || g_t60 < g_dr_pup[slot].end);
}

// (the right animation played for ~0.5 s, then the body ragdolled): winmm's kill of the victim's puppet (damage 1e6, recv_hit_hook) lands ~10 frames after the
// pre-start and the engine's Death state ragdolls the body whatever clip layer plays on it (log 86332: dead here 0 -> 1; pelvis 0.17 m at +1.25 s, while the clip's
// own pelvis is 0.93 m at 1 s / 0.76 m at 1.5 s and only reaches the floor at 3.5 s - offline decode of npc-m-death-molotov-fw-a). Same cure as the shiv kills (g_exk):
// the kill is HELD while the pre-started clip plays, the puppet dies at its end (npc-die no-anim, ragdoll hand-over from the final pose).
static int DrBurnDeferActive(int slot)
{
    return slot >= 0 && slot < T2R_MAX_PEERS && g_dr_pup[slot].on
        && g_dr_pup[slot].pre && g_t60 < g_dr_pup[slot].end;
}

static int DrBurnDeferFor(uint64_t me)
{                               // me = the victim's process of a damage-1e6 kill: the slot whose burning-death clip it must wait for, else -1
    typedef uint8_t *(*fn_peer)(uint32_t);
    for (int s = 0; s < T2R_MAX_PEERS; s++) {
        if (!DrBurnDeferActive(s))
            continue;
        uint8_t *pe = ((fn_peer) T2rPeer) ((uint32_t) s);
        uint64_t h = pe
            && !IsBadReadPtr(pe, 0x68) ? *(uint64_t *) (pe + 0x60) : 0;
        uint8_t *pr =
            h ? ((uint8_t * (*)(uint64_t))
                 T2R_SYM(kFnEntityFromHandle)) (h) : NULL;
        if (pr && h == g_dr_pup[s].h && (uint64_t) (uintptr_t) pr == me)
            return s;
    }
    return -1;
}

static void DrPupTick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    for (uint32_t sl = 0; sl < T2R_MAX_PEERS; sl++) {
        if (!g_dr_pup[sl].on)
            continue;
        uint8_t *peer = ((fn_peer) T2rPeer) (sl);
        uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
        if (!h || h != g_dr_pup[sl].h || g_t60 > g_dr_pup[sl].end + 180) {
            g_dr_pup[sl].on = 0;
            g_dr_pup[sl].pre = 0;
            continue;
        }
        if (g_dr_pup[sl].pre && g_frame - g_dr_pup[sl].pre_frame >= 150 && !(g_exk.on && g_exk.slot == (int) sl)) {     // the announced death never came (the victim lived / the message was stale): the clip comes off
            uint64_t p2 = h, dd =
                DcCall(0xe93f704f02a9ecd5ULL /* character-is-dead? */ , 1,
                        &p2);
            if (!(dd != ~0ULL && (dd & 0xff)) && !peer[5]) {
                Xlog("deathreact: slot %u puppet %llx - the announced burning death did not happen in 2.5 s: clip %s stopped -> %llx", sl, (unsigned long long) h, g_dr_pup[sl].clip, (unsigned long long) AnimEndBlend(h, g_dr_pup[sl].clip, 0.3f));
                g_dr_pup[sl].on = 0;
                g_dr_pup[sl].pre = 0;
                continue;
            }
        }
        uint64_t el = g_t60 - g_dr_pup[sl].t0;
        {                       // same dropped-layer rule as our own body (phase 0.000 = no layer): the clip goes back on at the elapsed phase
            double es = (double) el / 60.0;
            float dd = PsyncDur(h, g_dr_pup[sl].sid);
            if (dd > 0.2f && es > 0.15 && es < (double) dd - 0.25
                && g_t60 < g_dr_pup[sl].end && g_dr_pup[sl].keep_n < 8
                && GetTickCount64() - g_dr_pup[sl].keep_ms >= 250
                && PsyncPhase(h, g_dr_pup[sl].sid) < 0.001f) {
                g_dr_pup[sl].keep_ms = GetTickCount64();
                g_dr_pup[sl].keep_n++;
                uint64_t r =
                    AnimateFrom(h, g_dr_pup[sl].clip, 0.05f,
                                 (float) (es / dd));
                Xlog("deathreact: slot %u puppet clip %s dropped at %.2f s - re-applied at phase %.2f -> %llx (#%d)", sl, g_dr_pup[sl].clip, es, es / dd, (unsigned long long) r, g_dr_pup[sl].keep_n);
            }
        }
        if (el >= 30 && !g_dr_pup[sl].logged && g_t60 < g_dr_pup[sl].end) {     // evidence: the clip holds the dead body
            g_dr_pup[sl].logged = 1;
            uint64_t p2 = h, d2 =
                DcCall(0xe93f704f02a9ecd5ULL /* character-is-dead? */ , 1,
                        &p2);
            Xlog("deathreact: slot %u puppet clip %s at %.2f s: layer phase %.2f, dead %d", sl, g_dr_pup[sl].clip, el / 60.0, PsyncPhase(h, g_dr_pup[sl].sid), (int) (d2 != ~0ULL && (d2 & 0xff)));
        }
    }
}

// ---------------------------------------------------------------- dev trigger (t2r-dev-deathreact.txt)
#ifndef T2R_RELEASE
static struct {
    uint64_t ent;
    const char *clip;
    uint64_t until;
} g_drDev;
static void DrDev(const char *t)
{
    char who[8] = "", what[16] = "";
    int n = 0, dir = 0;
    if (sscanf(t, "%7s %15s %d", who, what, &dir) < 2) {
        Xlog("dev: deathreact \"%s\" ignored (own|slot N downed|sniper [dir 0-3]; slot form: slot <n> <downed|sniper> <dir>)", t);
        return;
    }
    uint64_t ent = SID_PLAYER;
    if (!strcmp(who, "slot")) {
        char w2[16] = "";
        sscanf(t, "%*s %d %15s %d", &n, w2, &dir);
        strcpy(what, w2);
        typedef uint8_t *(*fn_peer)(uint32_t);
        uint8_t *peer = n >= 0
            && n <
            T2R_MAX_PEERS ? ((fn_peer) T2rPeer) ((uint32_t) n) : NULL;
        ent = peer ? *(uint64_t *) (peer + 0x60) : 0;
        if (!ent) {
            Xlog("dev: deathreact slot %d has no puppet", n);
            return;
        }
    }
    int flame = !strcmp(what, "flame") || !strcmp(what, "flameprone");  // "flame" = standing burning death, "flameprone" = the downed (prone) one
    int prone = !strcmp(what, "flameprone");
    int kind =
        flame ? DR_FLAME : !strcmp(what, "sniper") ? DR_SNIPER : DR_DOWNED;
    if (dir < 0 || dir > 3)
        dir = 0;
    float dur = 0;
    int fam = 0;
    const char *clip = DrPick(ent, kind, dir, prone, &dur, &fam);
    if (!clip) {
        Xlog("dev: deathreact %s %s %s: no clip loaded (tried %s, %s)",
             who, what, DR_DIRN[dir],
             prone ? DR_FLAME_PRONE[dir] : DR_CLIPS[0][kind - 1][dir],
             prone ? DR_CLIPS[0][DR_DOWNED - 1][dir] : DR_CLIPS[1][kind -
                                                                   1]
             [dir]);
        return;
    }
    uint64_t r = AnimateBlend(ent, clip, DR_BLEND);
    if (flame) {
        g_bdKeep = ent == SID_PLAYER;
        BdStart(ent == SID_PLAYER ? T2R_US : n, ent, dur + 1.0f);
    }                           // dev: the burning fx too, put out with the clip
    g_drDev.ent = ent;
    g_drDev.clip = clip;
    g_drDev.until = g_t60 + S60(dur + 1.0f);
    Xlog("dev: deathreact %s %s %s -> %s clip %s on %llx -> %llx (%.2f s, stops %.1f s after the start; no kill)", who, what, DR_DIRN[dir], fam ? "ellie" : "npc", clip, (unsigned long long) ent, (unsigned long long) r, dur, dur + 1.0f);
}

static void DrDevTick(void)
{
    if (g_drDev.ent && g_t60 >= g_drDev.until) {
        Xlog("dev: deathreact clip %s stopped -> %llx", g_drDev.clip,
             (unsigned long long) AnimEndBlend(g_drDev.ent,
                                                 g_drDev.clip, 0.3f));
        g_drDev.ent = 0;
    }
}
#endif

// the molotov kill held by ext_lethal_hit (t2r-ext.c g_ext_pend): released once the clip reads a layer phase > 0 on our body (>= 2 frames), or DR_PEND_MAX frames
// at the latest, or at once if the reaction ended / something else killed us. Never leaves the kill pending.
#define DR_PEND_MAX 8
static void DrPendTick(void)
{
    if (!g_extPend.on)
        return;
    uint64_t fr = g_frame - g_extPend.frame;
    if (MatchSubstate() != 6) {
        g_extPend.on = 0;
        Xlog("burndeath: OWN BODY - the held lethal hit dropped (match over)");
        return;
    }
    if (LocalHp() <= 0.0f) {
        g_extPend.on = 0;
        Xlog("burndeath: OWN BODY - the held lethal hit dropped: we died meanwhile (hp %.1f)", LocalHp());
        return;
    }
    if (g_dr.phase != 1) {
        ExtLethalPendRun("the reaction ended first");
        return;
    }
    float ph = PsyncPhase(SID_PLAYER, g_dr.sid);
    if (fr >= 2 && ph > 0.02f) {
        char b[64];
        snprintf(b, sizeof b, "clip layer phase %.3f", ph);
        ExtLethalPendRun(b);
    } else if (fr >= DR_PEND_MAX) {
        char b[64];
        snprintf(b, sizeof b,
                 "wait over, clip layer phase %.3f - the clip may not hold",
                 ph);
        ExtLethalPendRun(b);
    }
}

static void DrTick(void)
{
    DrPendTick();
    DrOwnTick();
    DrPupTick();
    BdTick();
#ifndef T2R_RELEASE
    DrDevTick();
#endif
}
