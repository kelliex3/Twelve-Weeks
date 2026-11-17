// ---------------------------------------------------------------- death screen, victim POV
// Included by t2r-ext.c (one translation unit). Log prefix 'death ui:'. Off: t2r-no-death-ui.txt next to the exe (polled every 2 s;
// winmm's own to1/death-banner then shows as before).
//
// A little after the kill frame (DU_DELAY_MS from when our hp reaches 0 / the player enters the engine's Death state - the same frame an execution victim's client
// kills itself, a bleed-out or a shot) the screen cuts to black in DU_CUT_MS (a near-cut, TLOU2 SP), audio untouched (the killing
// shot / blow plays on); as the black lands TLOU2 SP's death sting plays (gbl-hud.bnk t2-death-stinger-shorter, ripped by
// tools/rip_t2_death_sting.py: the game's sound events are silent in MP and *music-playlist-player-death* is an empty playlist =
// the music cut, which the stinger layer still does at the death edge). Then, over the black, fading in:
//   - a TLOU2 SP death tip, native look (hud-post-death-tip's icon0 / label0 copied into hud-root, the SP illustration
//     streaming/large/death-tips/<name>.png): SP tips that fit Factions PvP, and our Factions tips (MF_TIPS, the loading screen's)
//     on a fitting SP picture; one per death, rotating
//   - KILLED BY: the killer's name, the weapon's icon and name (guns from the GEAR UP tables = the loadout icons incl. Shorty /
//     El Diablo / suppressed rows, melee weapons, shiv, FISTS for an unarmed execution, molotov / nail bomb)
//   - RESPAWNING IN n: winmm's real respawn timer (lobby +0x2bf8: +0 duration, +4 remaining s, +8 running; winmm shows ceil)
//   - "SPECTATE TEAMMATE" (E / interact) when a teammate is alive: the tip and card fade out, winmm's own spectate camera is
//     switched on (lobby +0x2c44, the out-of-lives spectate: candidates through spectate_ok = teammates with a sane camera) and
//     the black lifts onto it; the countdown stays as a small overlay; the same button brings the death screen back.
// At remaining <= 1 s everything fades to black (DU_FINAL_MS); the respawn's own fade from black (trade_tick) follows, our black
// leaves under it. The match end takes priority: the whole death screen fades out (DU_END_MS) for the VICTORY / DEFEAT banner.
// winmm's death banner (to1/death-banner, DAT_180189b98) is kept hidden while ours is up; winmm's text draws (radar, tags) follow
// our black (g_dui_text_alpha in radar_text_stub).
#include "death_sting.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl ds_blob\nds_blob:\n.incbin \"death_sting.pcm\"\n.text\n");
extern const int16_t ds_blob[];

// timing: the black comes a little AFTER the kill (as in TLOU2 SP: you see your character die / the execution gore at
// the shot), then the fade itself is fast; the tip / card / countdown come up right as the black lands. 0.25 s = TLOU2 SP's
// kill -> fade start (the engine's own, "Override Death Time" in its debug menu, is not in the data);
// t2r-death-delay.txt next to the exe ("<ms>", 0..3000) overrides it for tuning, read at each match start.
#define DU_DELAY_MS    83       // kill frame -> the fade to black starts (5 frames at 60 fps) (an execution victim's kill frame is our death edge)
#define DU_CUT_MS      70       // the fade to black itself (near a cut)
#define DU_LAND        (g_duDelay + g_duCut)  // black is full: death screen comes up
// the SP death animations clashed with the instant cut. Default now: the stinger plays AT the death edge, the world stays
// visible while our body's death plays (DU_LEAD_MS, or the death-reaction clip's length, capped DU_LEAD_MAX_MS), then a slow fade to black
// (DU_SOFT_CUT_MS) and the death screen widgets fade in (DU_SOFT_UI_MS). t2r-death-screen-delay.txt = lead in seconds (e.g. 2.0);
// t2r-death-screen-instant.txt = the old behaviour (cut at DU_DELAY_MS, sting as the black lands). Execution deaths stay instant (their own camera).
#define DU_LEAD_MS     2000
#define DU_LEAD_MAX_MS 3000
#define DU_SOFT_CUT_MS 700
#define DU_SOFT_UI_MS  400
#define DU_UI_MS       180      // tip / card / countdown fade in as the black lands
#define DU_SPEC_OUT_MS 300      // spectate: tip + card out ...
#define DU_SPEC_LIFT   350      // ... the black lifts from here (the camera had time to land on the teammate)
#define DU_SPEC_LIFT_MS 500
#define DU_BACK_MS     250      // back to the death screen: black in, then the tip + card
#define DU_FINAL_AT    1.0f     // remaining seconds: fade to black (the countdown shows 1)
#define DU_FINAL_MS    350
#define DU_RESPAWN_HOLD 150     // after the respawn our black stays this long (the respawn fade from black is up), then leaves
#define DU_RESPAWN_MS  350
#define DU_END_MS      300      // match end: the whole death screen out
#define DU_SPEC_MIN_S  1.6f     // no spectate offer in the last seconds

// ---- tips: SP death tips (post-death-tips.bin -> eng.common text, MP-fitting, no input-glyph tokens) and our Factions tips on an SP picture
static const struct {
    const char *img, *text;
} DU_TIPS[] = {
    { "easy-target",
     "Enemies are deadly accurate when you're charging towards them." },
    { "use-molotov", NULL },    // MF_TIPS[0] molotov
    { "cover-from-gunfire",
     "Stay crouched behind cover to protect from gunfire." },
    { "sprint-to-avoid",
     "Enemies are less accurate while you're sprinting away or to the side."
     },
    { "weapon-shotgun", NULL }, // MF_TIPS[1] shotguns
    { "general-flank-them",
     "Don't approach enemies directly, use the environment to flank them."
     },
    { "buddy-rescue", NULL },   // MF_TIPS[7] revive a downed teammate
    { "escape-sneak",
     "Your foes will investigate your last known location. Don't be there."
     },
    { "crouch-prone-aim",
     "Aim more accurately by crouching, or even more accurately by going prone."
     },
    { "ellie-in-backpack", NULL },      // MF_TIPS[4] crafting before the fight
    { "general-dont-get-flanked",
     "Look around to track enemy movements to ensure you aren't caught off guard."
     },
    { "silent-bow", NULL },     // MF_TIPS[3] the bow
    { "use-health-kit",
     "Using a health kit takes time. Make sure you're in a safe place." },
    { "overwhelmed", NULL },    // MF_TIPS[9] stick together
    { "hide",
     "In combat, use your environment to lose your enemies and slip back into stealth."
     },
    { "stand-aim-rifle",
     "Moving while aiming reduces your accuracy. Stand still to steady your aim."
     },
    { "scavenge-items", NULL }, // MF_TIPS[5] supply boxes
    { "stealth-explosives",
     "Arc-throw a Molotov while in low cover to stay hidden while throwing."
     },
    { "grass-crouch-prone", NULL },     // MF_TIPS[10] grass
    { "general-vary-your-tactics",
     "If your usual strategy isn't working, vary your tactics. Try a new weapon or crafting recipe."
     },
    { "weapon-bullet-drop",
     "Bullets drop with gravity. Aim higher to compensate." },
    { "shooting-armor-legs", NULL },    // MF_TIPS[6] armor
    { "melee-weapon-types",
     "One-handed melee weapons are fast and durable. Two-handed melee weapons are slower but deal massive damage."
     },
    { "sprint-base",
     "Buy yourself time to heal wounds or craft by running away from combat."
     },
    { "use-pipe-bombs",
     "Pipe bombs will land at an opponent's feet when quick-thrown." },
    { "vulnerable-enemies-5-6", NULL }, // MF_TIPS[12] finishing a downed enemy
    { "player-upgrade-stun-bomb-smoke",
     "Smoke bomb clouds completely block enemy vision, hiding your movements and preventing them from firing on you."
     },
    { "full-draw-bow",
     "Arrows do more damage when the bow is fully drawn." },
    { "limited-supplies", NULL },       // MF_TIPS[14] the clan eats
    { "upgraded-melee",
     "Upgraded melee weapons are restored to full durability and have greatly increased damage."
     },
    { "glass-break",
     "Breaking glass windows can open up new routes in combat, but the sound will draw enemy attention."
     },
    { "crouch-beside-enemy",
     "You have the choice to engage your opponents quietly, by force, or sometimes avoid combat altogether."
     },
    { "buddy-rescue", "Marking a downed teammate and running towards them doesn't deplete your stamina limit." },       // helpcall.c hc_rush_towards
};

#define DU_NTIPS (int)(sizeof DU_TIPS / sizeof DU_TIPS[0])
static const signed char DU_TIP_MF[DU_NTIPS] =
    { -1, 0, -1, -1, 1, -1, 7, -1, -1, 4, -1, 3, -1, 9, -1, -1, 5, -1, 10,
-1, -1, 6, -1, -1, -1, 12, -1, -1, 14, -1, -1, -1, -1 };
_Static_assert(sizeof DU_TIP_MF == DU_NTIPS,
               "DU_TIP_MF out of step with DU_TIPS");
static const char *DuTipText(int i)
{
    return DU_TIPS[i].text ? DU_TIPS[i].text : MF_TIPS[DU_TIP_MF[i] >=
                                                       0 ? DU_TIP_MF[i] :
                                                       0][1];
}

// ---- killed-by weapon: name + icon
static int DuRow(uint8_t *row, uint64_t w, const char **name,
                  const char **icon)
{
    if (!row || IsBadReadPtr(row, 0x18))
        return -1;
    const char *nm = *(const char **) row, *ic =
        *(const char **) (row + 8), *id = *(const char **) (row + 0x10);
    if (!id || IsBadReadPtr(id, 4) || !nm || IsBadReadPtr(nm, 2) || !ic
        || IsBadReadPtr(ic, 4))
        return -1;
    if (id[0] < 'a' || id[0] > 'z' || FNV64(id) != w)
        return 0;
    *name = nm;
    *icon = ic;
    return 1;
}

static int DuWeapon(uint64_t w, const char **name, const char **icon,
                     const char **src)
{
    static const struct {
        const char *id, *name, *icon;
    } X[] = {
        // unarmed / blades / explosives (2013 names)
        { "t2r-fall", "FALL", NULL },   // ours: a fall with no hit (death ui), no icon
        { "shiv", "SHIV", "hud/weapons/consumables/shiv.png" },
        { "molotov", "MOLOTOV", "hud/weapons/consumables/molotov.png" },
        { "pipe-bomb", "NAIL BOMB",
         "hud/weapons/consumables/pipe-bomb.png" },
        { "smoke-bomb", "SMOKE BOMB",
         "hud/weapons/consumables/smoke-bomb.png" },
        { "trap-mine", "TRAP MINE",
         "hud/weapons/consumables/trap-bomb.png" },
        // melee pickups (MELEE_DROPS / keep-melee ids), the game's pickup icons
        { "crowbar", "CROWBAR",
         "hud/weapons/melee/pickup/crowbar-one-hand.png" },
        { "lead-pipe", "PIPE", "hud/weapons/melee/pickup/lead-pipe.png" },
        { "short-pipe", "PIPE", "hud/weapons/melee/pickup/lead-pipe.png" },
        { "pipe-wrench", "PIPE WRENCH",
         "hud/weapons/melee/pickup/pipe-wrench.png" },
        { "hammer-modern", "HAMMER",
         "hud/weapons/melee/pickup/hammer-modern.png" },
        { "hammer-scar", "HAMMER",
         "hud/weapons/melee/pickup/hammer-scar.png" },
        { "club-1h-police", "BATON",
         "hud/weapons/melee/pickup/club-1h-police.png" },
        { "club-1h-scar", "CLUB",
         "hud/weapons/melee/pickup/club-1h-scar.png" },
        { "machete", "MACHETE",
         "hud/weapons/melee/pickup/machete-wood.png" },
        { "machete-scar", "MACHETE",
         "hud/weapons/melee/pickup/machete-scar.png" },
        { "tomahawk", "HATCHET", "hud/weapons/melee/pickup/tomahawk.png" },
        { "hatchet-modern", "HATCHET",
         "hud/weapons/melee/pickup/hatchet-modern.png" },
        { "hatchet", "HATCHET", "hud/weapons/melee/pickup/hatchet-modern.png" },        // (SP's weapon id; its weapon-art icon is this one)
        { "two-by-four", "2X4",
         "hud/weapons/melee/pickup/two-by-four.png" },
        { "baseball-bat", "BASEBALL BAT",
         "hud/weapons/melee/pickup/baseball-bat.png" },
        { "fireman-axe", "AXE",
         "hud/weapons/melee/pickup/fireman-axe.png" },
        // guns outside the GEAR UP tables (also the fallback before the tables are installed) - to1/death-banner's icons
        { "bow-ellie", "BOW", "hud/weapons/guns/bow-ellie/bow.png" },
        { "bow-explosive", "EXPLOSIVE BOW", "hud/weapons/consumables/arrow-explosive.png" },    // the 2013 Launcher (expbow2013.c): TLOU2's explosive arrow icon
        { "crossbow", "CROSSBOW",
         "hud/weapons/guns/crossbow/crossbow.png" },
        { "flamethrower", "FLAMETHROWER",
         "hud/weapons/guns/flamethrower/flamethrower.png" },
#if defined(T2R_COLT_BUILT) && T2R_COLT_BUILT
        { "pistol-beretta", "9MM PISTOL", "streaming/t2r/icons/colt.png" },
#else
        { "pistol-beretta", "9MM PISTOL",
         "hud/weapons/guns/pistol-beretta/beretta.png" },
#endif
        { "pistol-glock", "BURST PISTOL",
         "hud/weapons/guns/pistol-glock/glock.png" },
        { "pistol-revolver-taurus", "REVOLVER",
         "hud/weapons/guns/pistol-revolver-taurus/revolver-brl.png" },
        { "pistol-xcaliber", "HUNTING PISTOL",
         "hud/weapons/guns/pistol-xcaliber/xcaliber.png" },
        { "rifle-assault-firefly", "ASSAULT RIFLE",
         "hud/weapons/guns/rifle-assault-firefly/rifle-assault-firefly.png"
         },
        { "rifle-galil", "SPECTER",
         "hud/weapons/guns/rifle-galil/galil.png" },
        { "rifle-assault-firefly-ellie-player", "ASSAULT RIFLE", "hud/weapons/guns/rifle-assault-firefly/rifle-assault-firefly.png" },  // 2013 Assault Rifle = the game's No Return rifle
        { "rifle-mpx5", "FULL-AUTO RIFLE",
         "hud/weapons/guns/rifle-mpx5/mpx5.png" },
        { "rifle-remington-bolt", "HUNTING RIFLE",
         "hud/weapons/guns/rifle-remington-bolt/remington-bolt.png" },
        { "rifle-vepr", "VARIABLE RIFLE",
         "hud/weapons/guns/rifle-vepr/vepr-scp.png" },
        { "shotgun-baserri-over-under", "DOUBLE BARREL",
         "streaming/t2r/icons/double-barrel.png" },
        { "shotgun-remington-pump", "PUMP SHOTGUN",
         "hud/weapons/guns/shotgun-remington-pump/remington-pump.png" },
        { "shotgun-tactical", "TACTICAL SHOTGUN",
         "streaming/t2r/icons/tac-shotgun.png" },
        { "rifle-m14-semi", "SEMI-AUTO RIFLE",
         "streaming/t2r/icons/m14.png" },
        { "rifle-burst", "BURST RIFLE",
         "streaming/t2r/icons/burst-rifle.png" },
        { "pistol-cz75-enforcer", "ENFORCER",
         "streaming/t2r/icons/cz75.png" },
        { "pistol-el-diablo", "EL DIABLO",
         "streaming/t2r/icons/el-diablo.png" },
        { "shotgun-shorty", "SHORTY", "streaming/t2r/icons/shorty.png" },
        { "rifle-frontier", "FRONTIER RIFLE",
         "streaming/t2r/icons/frontier.png" },
        { "rifle-milsniper", "MILITARY SNIPER",
         "streaming/t2r/icons/milsniper.png" },
    };
    static const char *const FISTS[] =
        { "abby-unarmed", "ellie-unarmed", "unarmed", "fists", "fist" };
    *name = NULL;
    *icon = NULL;
    *src = "-";
    if (w == SID_PIPE_BOMB_EXPLOSION)
        w = FNV64("pipe-bomb");
    for (size_t i = 0; i < sizeof FISTS / sizeof *FISTS; i++)
        if (w == FNV64(FISTS[i])) {
            *name = "FISTS";
            *icon = "streaming/t2r/icons/fist.png";
            *src = "fists";
            return 1;
        }
    // the GEAR UP tables (install_weapon_tables: our large / purchasable tables, winmm's small one grown in place): exact id first
    // (a suppressed row is its own id), then the base gun
    for (int pass = 0; pass < 2; pass++) {
        uint64_t k = pass ? BaseWeapon(w) : w;
        if (pass && k == w)
            break;
        uint8_t *lt =
            g_largeTab ? g_largeTab : (uint8_t *)
            T2R_SYM(kLargeWeaponTab), *pt = LoPurchTab(), *st =
            (uint8_t *) T2R_SYM(kSmallWeaponTab);
        for (int r = 0; r < 16; r++) {
            int x = DuRow(lt + r * 0x38, k, name, icon);
            if (x < 0)
                break;
            if (x > 0) {
                *src = "large table";
                return 1;
            }
        }
        for (int r = 0; r < 16; r++) {
            int x = DuRow(pt + r * 0x40, k, name, icon);
            if (x < 0)
                break;
            if (x > 0) {
                *src = "purchasable table";
                return 1;
            }
        }
        for (int r = 0; r < 8; r++) {
            int x = DuRow(st + r * 0x38, k, name, icon);
            if (x < 0)
                break;
            if (x > 0) {
                *src = "small table";
                return 1;
            }
        }
    }
    uint64_t b = BaseWeapon(w);
    for (size_t i = 0; i < sizeof X / sizeof *X; i++)
        if (FNV64(X[i].id) == w || FNV64(X[i].id) == b) {
            *name = X[i].name;
            *icon = X[i].icon;
            *src = "list";
            return 1;
        }
    return 0;
}

static int DuIsMeleeWeapon(uint64_t w)
{
    static const char *const M[] =
        { "crowbar", "lead-pipe", "short-pipe", "hammer-modern",
"hammer-scar", "club-1h-police", "club-1h-scar",
        "machete", "machete-scar", "tomahawk", "hatchet-modern", "hatchet",
            "two-by-four", "baseball-bat", "fireman-axe",
        "pipe-wrench"
    };
    for (size_t i = 0; i < sizeof M / sizeof *M; i++)
        if (FNV64(M[i]) == w)
            return 1;
    return 0;
}

// ---- alpha tracks: every change ramps from the value on screen (no pops), real ms
typedef struct {
    float from, to;
    uint64_t t0;
    int d0, d1;
} DuTrT;
static float DuVal(const DuTrT *t, uint64_t now)
{
    return t->from + (t->to - t->from) * RampMs(now >
                                                 t->t0 ? now - t->t0 : 0,
                                                 t->d0, t->d1);
}

static void DuGo(DuTrT *t, float to, int delay, int ms, uint64_t now)
{
    if (t->to == to)
        return;
    t->from = DuVal(t, now);
    t->to = to;
    t->t0 = now;
    t->d0 = delay;
    t->d1 = delay + (ms > 0 ? ms : 1);
}

static void DuSet(DuTrT *t, float v, uint64_t now)
{
    t->from = t->to = v;
    t->t0 = now;
    t->d0 = 0;
    t->d1 = 1;
}
enum { DT_BLACK, DT_TIP, DT_CARD, DT_COUNT, DT_SPEC, DT_SPEC_A, DT_SPEC_B,
        DT_BAR, DT_SWITCH, DT_N };
static const char *const DU_PATH[DT_N] =
    { "t2rDeath.dBlack", "t2rDeath.dTip", "t2rDeath.dCard",
"t2rDeath.dCount", "t2rDeath.dSpec",
    "t2rDeath.dSpec.dSpecLbl", "t2rDeath.dSpec.dSpecLblB",
        "t2rDeath.dSpecBar", "t2rDeath.dSpecBar.dSwitch"
};

static struct {
    int phase;                  // 0 idle, 1 dead (death screen), 2 respawned (black leaving), 3 match over (all leaving)
    uint64_t t0, ph_t0;         // death edge / phase start (GetTickCount64)
    int lead_fit, spec, final, sting, filled, logged_black, cnt, ui_in,
        seen_dead, black_wait;
    uint64_t edge_frame;
    uint64_t spec_t0, mate_lost_t0, final_t0;
    int auto_spec;              // out of reinforcements: the one automatic spectate offer of this death was made
    DuTrT tr[DT_N];
    float wrote[DT_N];
    int killer;
    uint64_t weapon;
    const char *wsrc;
} g_du;
// immersion.c's downed / execution vignettes: gone the moment the death screen runs (phase 1) or this death's kill signal is fresh (the black has not
// started yet - DU_DELAY), back off again at respawn
static int DuDeadNow(void)
{
    return g_du.phase == 1 || (g_du_edge_ms
                               && GetTickCount64() - g_du_edge_ms < 1500
                               && MatchSubstate() == 6);
}

static int g_duTip;            // rotation index (per death)
static int g_duDelay = DU_DELAY_MS;    // this death: kill frame -> fade to black starts
static int g_duCut = DU_CUT_MS;        // this death: the fade itself
static int g_duUi = DU_UI_MS;  // this death: death screen widgets fade-in
static int g_duLeadCfg = DU_LEAD_MS;  // t2r-death-screen-delay.txt
static int g_duSoft;           // this death: soft mode (stinger at the edge)

static uint64_t DuW(const char *rel)
{
    static uint64_t cache[24];
    static const char *key[24];
    for (int i = 0; i < 24; i++)
        if (key[i] == rel)
            return cache[i];
    uint64_t h = Hudw(rel);
    if (h)
        for (int i = 0; i < 24; i++)
            if (!key[i]) {
                key[i] = rel;
                cache[i] = h;
                break;
            }
    return h;
}

static void DuText(const char *rel, const char *t)
{
    uint64_t w = DuW(rel);
    if (w)
        ((fn_settext_t) T2R_SYM(kFnSetText)) (w, t);
}

static void DuImage(const char *rel, const char *img)
{
    uint64_t w = DuW(rel);
    if (w)
        ((fn_settext_t) T2R_SYM(kFnSetImage)) (w, img);
}

static void DuTipImage(int i)
{
    char b[96];
    snprintf(b, sizeof b, "streaming/large/death-tips/%s.png",
             DU_TIPS[i].img);
    DuImage("t2rDeath.dTip.dTipImg", b);
}

static int DuMateAlive(int *slot)
{                               // a teammate winmm's spectate would accept (alive, camera record usable)
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        if (s == me || !IsTeammate(s))
            continue;
        uint8_t *p = ((fn_peer) T2rPeer) (s);
        if (!p || !p[4] || p[5] || g_peerDowned[s])
            continue;
        if (SpectateCamOk(p) != 0) {
            if (slot)
                *slot = (int) s;
            return 1;
        }
    }
    return 0;
}

static int DuButton(void)
{                               // E / the interact command, edge
    static int held;
    uint64_t c = FNV64("interact");
    uint64_t r = DcCall(FNV64("joypad-command-active?"), 1, &c);
    int on = (r != ~0ULL && (r & 0xff)) || (GameFocused()
                                            && (GetAsyncKeyState('E') &
                                                0x8000));
    int e = on && !held;
    held = on;
    return e;
}

// ---- spectating bar: who we watch (winmm's target = lobby +0x2c4c while +0x2c44 is set) and LMB / RT = the next living teammate.
// winmm re-tests its target every 3 s (spectate_ok keeps a live one) and reads +0x2c4c every frame for the camera, so writing the slot is the switch.
static int DuMates(int *out, int max)
{                               // living teammates winmm's spectate accepts, ascending slot
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    int n = 0;
    for (uint32_t s = 0; s < T2R_MAX_PEERS && n < max; s++) {
        if (s == me || !IsTeammate(s))
            continue;
        uint8_t *p = ((fn_peer) T2rPeer) (s);
        if (!p || !p[4] || p[5] || g_peerDowned[s])
            continue;
        if (SpectateCamOk(p) != 0)
            out[n++] = (int) s;
    }
    return n;
}

// returns +1 (next: LMB / F / RT / D / right arrow / RB), -1 (previous: RMB / LT / A / left arrow / LB) on the press edge, else 0;
// prime = just remember the state (no edge)
static int DuSwitchButton(int prime)
{
    typedef DWORD(WINAPI * fn_xs) (DWORD, void *);
    static int held_n, held_p, xi_tried;
    static fn_xs xs;
    if (!xi_tried) {
        xi_tried = 1;
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (m)
            xs = (fn_xs) GetProcAddress(m, "XInputGetState");
    }
    int fg = GameFocused();
    int nx = fg && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000)
                    || (GetAsyncKeyState('F') & 0x8000)
                    || (GetAsyncKeyState('D') & 0x8000)
                    || (GetAsyncKeyState(VK_RIGHT) & 0x8000));
    int pv = fg && ((GetAsyncKeyState(VK_RBUTTON) & 0x8000)
                    || (GetAsyncKeyState('A') & 0x8000)
                    || (GetAsyncKeyState(VK_LEFT) & 0x8000));
    if (xs) {
        struct {
            DWORD pk;
            WORD b;
            BYTE lt, rt;
            SHORT lx, ly, rx, ry;
        } st;                   // XINPUT_STATE
        for (DWORD u = 0; u < 4; u++)
            if (xs(u, &st) == 0) {
                if (st.rt > 160 || (st.b & 0x0200))
                    nx = 1;     // RT, RB
                if (st.lt > 160 || (st.b & 0x0100))
                    pv = 1;     // LT, LB
            }
    }
    if (!nx) {
        uint64_t c = FNV64("fire-press");
        uint64_t r = DcCall(FNV64("joypad-command-active?"), 1, &c);
        nx = r != ~0ULL && (r & 0xff);
    }
    int d = prime ? 0 : (nx && !held_n) ? 1 : (pv && !held_p) ? -1 : 0;
    held_n = nx;
    held_p = pv;
    return d;
}

static uint64_t g_duDeathId;  // t0 of the death screen running (log once per death)
static void DuWinmmBannerOff(void)
{                               // winmm's to1/death-banner (KILLED BY / RESPAWNING IN) stays hidden under ours
    typedef char (*fn_shown)(uint64_t);
    typedef void (*fn_hide)(uint64_t, int);
    uint64_t page = *(uint64_t *) T2R_SYM(kUiPageAlt);
    if (page && ((fn_shown) T2R_SYM(kFnWidgetShown)) (page)) {
        ((fn_hide) T2R_SYM(kFnWidgetHide)) (page, 0);
        static uint64_t logged_for;
        if (logged_for != g_duDeathId) {
            logged_for = g_duDeathId;
            Xlog("death ui: winmm death banner hidden (ours is up; re-hidden every frame)");
        }
    }
}

// the killer: an E execution on us (psync victim timeline) names the attacker and its weapon; else winmm's own kill credit -
// the downer (lobby +0x2c18 / +0x2c20), else the last hitter (+0x2c2c / +0x2c30) - read live, or from the last frames before
// winmm reset them at the death; else our own receive records
static struct {
    int down, hit;
    uint64_t down_w, hit_w, downed_ms, hit_ok_ms;
    int gx_slot, gx_melee, gx_shiv;
    uint64_t gx_w, gx_ms;
    uint64_t self_w, self_ms;   // suicide: our own explosive took our hp (no hit message, our throw within OWN_THROW_WINDOW)
    float prev_hp, ys[32];
    uint64_t y_ms;
    int yn, yi;                 // fall: our height every 100 ms over the last 3.2 s
} g_dk;
static void DuDkReset(void)
{
    memset(&g_dk, 0, sizeof g_dk);
    g_dk.down = g_dk.hit = g_dk.gx_slot = -1;
    g_dk.prev_hp = -1.0f;
}

static void DuReadLobby(uint8_t *lc, uint64_t now)
{                               // winmm's credit fields, with its own staleness rule for the hitter
    if (!lc || IsBadReadPtr(lc + 0x2c18, 0x80))
        return;
    int32_t d = *(int32_t *) (lc + 0x2c18), h = *(int32_t *) (lc + 0x2c2c);
    if (d >= 0 && d < T2R_MAX_PEERS) {
        g_dk.down = d;
        g_dk.down_w = *(uint64_t *) (lc + 0x2c20);
    }
    if (h >= 0 && h < T2R_MAX_PEERS) {
        g_dk.hit = h;
        g_dk.hit_w = *(uint64_t *) (lc + 0x2c30);
        float thr = *(float *) T2R_SYM(kKillWindow);    // winmm: a hit counts for the kill while clock - hit time <= this
        if (*(float *) (lc + 0x2c90) - *(float *) (lc + 0x2c3c) <=
            (thr > 0.0f && thr < 120.0f ? thr : 10.0f))
            g_dk.hit_ok_ms = now;
    }
    if (g_downed)
        g_dk.downed_ms = now;
}

static void DuTrackSelf(uint64_t now)
{
    // our own throw (send_hook, type 2) still live and an hp loss with no hit message from a peer: our own explosive did it (the
    // molotov's fire kill comes through ext_lethal_hit; this catches the pipe bomb / impact blast the engine applies locally)
    // an ENEMY molotov's fire burned us to death 1.5 s after our own throw, and every burn tick (no hit message) was taken
    // for our own explosive - the card said YOURSELF. Hp lost while enemy fire is on us (a tick in the last 1.5 s) is never ours.
    float hp = LocalHp();
    int enemy_fire = g_fire_enemy_ms && now - g_fire_enemy_ms < 1500;
    if (g_dk.prev_hp > 0.0f && hp < g_dk.prev_hp - 0.5f
        && g_frame - g_lastEnemyHit > 3 && !enemy_fire
        && g_ownThrowFrame
        && g_t60 - g_ownThrowFrame < OWN_THROW_WINDOW
        && g_ownThrowSid) {
        g_dk.self_w = g_ownThrowSid;
        g_dk.self_ms = now;
        Xlog("death ui: self damage %.1f (hp %.1f -> %.1f) from our own %llx thrown %.1f s ago", g_dk.prev_hp - hp, g_dk.prev_hp, hp, (unsigned long long) g_ownThrowSid, (g_t60 - g_ownThrowFrame) / 60.0f);
    }
    g_dk.prev_hp = hp;
    if (now - g_dk.y_ms >= 100) {
        float b[16];
        const float *pl = PlayerLocator(b);
        if (pl && __builtin_fabsf(pl[1]) < 100000.0f) {
            g_dk.y_ms = now;
            g_dk.ys[g_dk.yi] = pl[1];
            g_dk.yi = (g_dk.yi + 1) % 32;
            if (g_dk.yn < 32)
                g_dk.yn++;
        }
    }
}

static float DuFallHeight(void)
{                               // metres fallen in the last ~3 s (highest sample - where we are now)
    float b[16];
    const float *pl = PlayerLocator(b);
    if (!pl || !g_dk.yn)
        return 0.0f;
    float hi = pl[1];
    for (int i = 0; i < g_dk.yn; i++)
        if (g_dk.ys[i] > hi)
            hi = g_dk.ys[i];
    return hi - pl[1];
}

static void DuTrackKiller(uint64_t now)
{
    DuReadLobby(LobbyObj(), now);
    DuTrackSelf(now);
    if (GxVictimActive()) {
        typedef uint8_t *(*fn_peer)(uint32_t);
        g_dk.gx_slot = g_gx.slot;
        g_dk.gx_melee = g_gx.melee;
        g_dk.gx_shiv = g_gx.shiv;
        g_dk.gx_ms = now;
        uint8_t *p = g_gx.slot >= 0
            && g_gx.slot <
            T2R_MAX_PEERS ? ((fn_peer) T2rPeer) ((uint32_t) g_gx.
                                                  slot) : NULL;
        g_dk.gx_w =
            g_gx.melee ? (p ? *(uint64_t *) (p + 0x91) : 0) : g_gx.weapon;
    }
}

// who killed us with what (the death card; the kill feed asks the same at our death and sends it to everyone: killfeed.c)
static void DuResolve(uint64_t now, int *pk, uint64_t *pw,
                       const char **pwhy)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    int k = -1;
    uint64_t w = 0;
    const char *why = "-";
    // gx_ms is the last frame the paired move was seen, so a downed victim SHOT to death within 5 s after a survived
    // finisher was credited "melee execution" with the bat (and the +50 Execution award). A hit received after the move ended wins.
    if (g_dk.gx_ms && now - g_dk.gx_ms < 5000 && g_dk.gx_slot >= 0
        && !(g_lastEnemyHitMs > g_dk.gx_ms + 150)) {
        k = g_dk.gx_slot;
        if (g_dk.gx_shiv) {
            w = FNV64("shiv");
            why = "shiv kill";
        } else if (g_dk.gx_melee) {
            w = DuIsMeleeWeapon(g_dk.gx_w) ? g_dk.gx_w : FNV64("fists");
            why = "melee execution";
        } else {
            w = g_dk.gx_w;
            why = "gun execution";
        }
    } else if (g_dnLethal.ms && now - g_dnLethal.ms < 3000
               && g_dnLethal.w && g_dnLethal.slot < T2R_MAX_PEERS) {
        // (downed by a bow, finished by a pistol shot - log 16525 'death card says <bow> (last hitter
        // (winmm credit))' while winmm's own kill weapon was the pistol): winmm's hitter record still held the downing hit's weapon, so the shot
        // that finished the downed body names the weapon
        k = (int) g_dnLethal.slot;
        w = g_dnLethal.w;
        why = "the shot that finished us while downed";
    } else if (g_extLethal.ms && now - g_extLethal.ms < 3000) {       // the ext's own lethal hit (molotov burn): killer -1 = our own fire
        k = g_extLethal.killer >= 0
            && g_extLethal.killer <
            T2R_MAX_PEERS ? g_extLethal.killer : (int) me;
        w = g_extLethal.w;
        why =
            k ==
            (int) me ? "our own fire (ext lethal hit)" : "ext lethal hit";
    } else if (g_dk.self_ms && now - g_dk.self_ms < 1500) {
        k = (int) me;
        w = g_dk.self_w;
        why = "our own explosive (self damage at the death)";
    } else if (g_dk.down >= 0 && g_dk.downed_ms
               && now - g_dk.downed_ms < 4000) {
        k = g_dk.down;
        w = g_dk.down_w;
        why = "downer (winmm credit)";
    } else if (g_dk.hit >= 0 && g_dk.hit_ok_ms
               && now - g_dk.hit_ok_ms < 1500) {
        k = g_dk.hit;
        w = g_dk.hit_w;
        why = "last hitter (winmm credit)";
    } else if (g_downHit.w && g_down_slot < T2R_MAX_PEERS
               && g_dk.downed_ms && now - g_dk.downed_ms < 4000) {
        k = (int) g_down_slot;
        w = g_downHit.w;
        why = "our downing hit";
    } else if (g_lastHitSlot < T2R_MAX_PEERS && g_lastEnemyHitMs
               && now - g_lastEnemyHitMs < 2000) {
        k = (int) g_lastHitSlot;
        why = "our last received hit (no weapon)";
        // (2 of 32 deaths showed FISTS for a gun kill): the card used to fall to fists with no weapon; the killer's held weapon (net state, peer+0x91) is the best guess
        uint8_t *kp =
            k != (int) me ? ((fn_peer) T2rPeer) ((uint32_t) k) : NULL;
        uint64_t hw = kp
            && !IsBadReadPtr(kp + 0x91, 8) ? *(uint64_t *) (kp + 0x91) : 0;
        if (hw && hw != ~0ULL && BaseWeaponIsGun(hw)) {
            w = hw;
            why = "our last received hit (killer's held gun)";
        }
    } else if (DuFallHeight() > 4.0f) {
        k = (int) me;
        w = FNV64("t2r-fall");
        why = "fall (no hit, dropped this far)";
    }
    *pk = k;
    *pw = w;
    *pwhy = why;
}

static void DuFillCard(uint64_t now)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    int k = -1;
    uint64_t w = 0;
    const char *why = "-";
    DuResolve(now, &k, &w, &why);
    g_du.killer = k;
    g_du.weapon = w;
    const char *name = NULL, *icon = NULL, *src = "-";
    int found = w ? DuWeapon(w, &name, &icon, &src) : 0;
    if (!found && k >= 0 && k != (int) me
        && (!w || !BaseWeaponIsGun(w))) {
        found = DuWeapon(FNV64("fists"), &name, &icon, &src);
        src = "unknown weapon -> fists";
    }
    g_du.wsrc = src;
    char kn[48] = "";
    if (k >= 0 && k < T2R_MAX_PEERS && k != (int) me) {
        uint8_t *p = ((fn_peer) T2rPeer) ((uint32_t) k);
        const char *s = p
            && !IsBadReadPtr(p + 6, 2) ? (const char *) (p + 6) : NULL;
        snprintf(kn, sizeof kn, "%.40s", s && s[0] ? s : "UNKNOWN");
    } else if (k == (int) me)
        snprintf(kn, sizeof kn, "YOURSELF");
    DuText("t2rDeath.dCard.dKilledBy", kn[0] ? "KILLED BY" : "YOU DIED");
    DuText("t2rDeath.dCard.dKiller", kn);
    WVisible(DuW("t2rDeath.dCard.dKiller"), kn[0] != 0);
    if (found) {
        DuText("t2rDeath.dCard.dWName", name);
        if (icon)
            DuImage("t2rDeath.dCard.dWIcon", icon);
    }
    WVisible(DuW("t2rDeath.dCard.dWName"), found);
    WVisible(DuW("t2rDeath.dCard.dWIcon"), found && icon);
    Xlog("death ui: killed by slot %d '%s' (%s), weapon %llx -> %s / %s (%s)", k, kn, why, (unsigned long long) w, found ? name : "none", found && icon ? icon : "-", src);
}

static void DuCount(uint8_t *lc, int force)
{                               // RESPAWNING IN n (winmm's ceil of the remaining time), double-buffered
    int run = lc && lc[0x2bf8 + 8];
    float rem = lc ? *(float *) (lc + 0x2bf8 + 4) : 0.0f;
    int s = run ? (int) __builtin_ceilf(rem) : -1;
    // timer gone: out of reinforcements ONLY when the team really has none. With lives left the countdown has ended and the host's grant is
    // still on its way (request lost / spawn picker holding it back; it showed "no reinforcements" with many left) = "RESPAWNING".
    int lives = lc ? ((int (*)(void *)) T2R_SYM(kFnTeamLives)) (lc) : 1;
    int nolives = s < 0 && lives <= 0 && lives > -1000;
    static int s_lbl_nolives = -1;
    if (force)
        s_lbl_nolives = -1;
    if (s == g_du.cnt && !force && nolives == s_lbl_nolives)
        return;
    if ((s < 0) != (g_du.cnt < 0) || force || nolives != s_lbl_nolives) {
        DuText("t2rDeath.dCount.dCountLbl",
                s >=
                0 ? "RESPAWNING IN" : nolives ? "NO REINFORCEMENTS" :
                "RESPAWNING");
        DbVisible(DuW("t2rDeath.dCount.dCountNum"),
                   DuW("t2rDeath.dCount.dCountNumB"), s >= 0);
    }
    s_lbl_nolives = nolives;
    g_du.cnt = s;
    if (s >= 0) {
        char b[16];
        snprintf(b, sizeof b, "%d", s);
        DB_TEXT_W(DuW("t2rDeath.dCount.dCountNum"),
                  DuW("t2rDeath.dCount.dCountNumB"), b);
        Xlog("death ui: countdown %d (remaining %.2f s)", s, rem);
    } else if (nolives)
        Xlog("death ui: no respawn timer running - no reinforcements left (team lives %d)", lives);
    else
        Xlog("death ui: respawn timer ended, team lives %d - waiting for the host's grant (label RESPAWNING)", lives);
}

// ---- the MP HUD under the black (the score banner at the top and the weapon / health widget bottom left stayed
// visible over the death screen - hudRoot children are not drawn in depth order). As the title card does it (hud_fade_ordered):
// every canvas.hudRoot child except our t2r* overlays - here hudMenuStack too (winmm's score banner / currency HUD pages live in it)
// - gets alpha = its own alpha x (1 - black); a value the game sets meanwhile becomes the new base; the base goes back when the
// death screen is over (the black's own fade out = the HUD's fade in). Not while the intro HUD fade runs (feature_hud_fade owns it).
static struct {
    uint64_t path;
    float base, wrote;
} g_dh[32];
static int g_dhN = -1;         // -1 = not built for this death
static void DuHudBuild(void)
{
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    uint64_t c = 0;
    char b[800];
    int bl = 0;
    g_dhN = 0;
    ((void (*)(uint64_t *, uint64_t)) (exe + RVA_GUI2_CHILD)) (&c,
                                                               FNV64
                                                               ("canvas.hudRoot"));
    for (int guard = 0; c && g_dhN < 32 && guard < 64; guard++) {
        uint8_t *w = G2Find(c);
        const char *nm = w ? G2Name(w) : "-";
        if (w && strncmp(nm, "t2r", 3)) {
            g_dh[g_dhN].path = c;
            g_dh[g_dhN].base = *(float *) (w + 0x3bc);
            g_dh[g_dhN].wrote = -1.0f;
            g_dhN++;
            if (bl < (int) sizeof b - 40)
                bl +=
                    snprintf(b + bl, SNLEFT(b, bl), " %s(%.2f)", nm,
                             *(float *) (w + 0x3bc));
        }
        uint64_t nx = 0;
        ((void (*)(uint64_t *, uint64_t)) (exe + RVA_GUI2_SIBLING)) (&nx,
                                                                     c);
        if (nx == c)
            break;
        c = nx;
    }
    // winmm's own HUD pages by their spawn handles (a handle is what g2_find takes - winmm's FUN_1800620d0 does the same): the score
    // banner (DAT_180189b90) and to1/currency-hud (DAT_180189ba0: parts counter, the 2013 team figures teamFigs figU / figE, armour
    // icons) - menu-stack pages need not inherit hudMenuStack's alpha, so they are faded themselves
    static const uint64_t PAGES[2] = { 0x180189B90, 0x180189BA0 };
    static const char *const PNAME[2] =
        { "score banner page", "currency-hud page (team figures)" };
    for (int k = 0; k < 2 && g_dhN < 32; k++) {
        uint64_t page = *(uint64_t *) T2R_LEGACY_VA(PAGES[k]);
        uint8_t *w = page ? G2Find(page) : NULL;
        int dup = 0;
        for (int i = 0; i < g_dhN; i++)
            if (g_dh[i].path == page)
                dup = 1;
        if (!w || dup) {
            if (bl < (int) sizeof b - 60)
                bl +=
                    snprintf(b + bl, SNLEFT(b, bl), " [%s: %s]", PNAME[k],
                             w ? "dup" : "none");
            continue;
        }
        g_dh[g_dhN].path = page;
        g_dh[g_dhN].base = *(float *) (w + 0x3bc);
        g_dh[g_dhN].wrote = -1.0f;
        g_dhN++;
        if (bl < (int) sizeof b - 60)
            bl +=
                snprintf(b + bl, SNLEFT(b, bl), " [%s %s(%.2f)]", PNAME[k],
                         G2Name(w), *(float *) (w + 0x3bc));
    }
    Xlog("death ui: HUD under the black - %d widgets faded with it:%s",
         g_dhN, g_dhN ? b : " -");
}

static void DuHudApply(float f)
{
    if (g_hudFadeT0 || !G2Ok())
        return;                 // the intro's HUD fade is running: it owns these alphas
    if (g_dhN < 0) {
        if (f >= 0.999f)
            return;
        DuHudBuild();
    }
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    for (int i = 0; i < g_dhN; i++) {
        uint8_t *w = G2Find(g_dh[i].path);
        if (!w)
            continue;
        float cur = *(float *) (w + 0x3bc);
        if (g_dh[i].wrote >= 0.0f
            && __builtin_fabsf(cur - g_dh[i].wrote) > 1e-3f)
            g_dh[i].base = cur; // the game set it
        float v = g_dh[i].base * f;
        if (g_dh[i].wrote >= 0.0f
            && __builtin_fabsf(v - g_dh[i].wrote) < 1e-4f)
            continue;
        ((void (*)(uint8_t *, float)) (exe + RVA_WIDGET_ALPHA)) (w, v);
        g_dh[i].wrote = v;
    }
}

static void DuHudRestore(void)
{
    if (g_dhN > 0 && G2Ok()) {
        uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
        for (int i = 0; i < g_dhN; i++) {
            uint8_t *w = G2Find(g_dh[i].path);
            if (!w)
                continue;
            float cur = *(float *) (w + 0x3bc);
            if (g_dh[i].wrote >= 0.0f
                && __builtin_fabsf(cur - g_dh[i].wrote) > 1e-3f)
                continue;       // the game owns it again
            ((void (*)(uint8_t *, float)) (exe + RVA_WIDGET_ALPHA)) (w,
                                                                     g_dh
                                                                     [i].
                                                                     base);
        }
        Xlog("death ui: HUD alphas restored (%d children)", g_dhN);
    }
    g_dhN = -1;
}

// The Esc menu could be opened on the death screen but drew under it. The death screen is baked into canvas.hudRoot,
// the pause menu is a menu-stack page - the HUD canvas draws over it (black + tip + card). While to1/pause-menu is on the menu stack every
// widget of ours (and the HUD fade it drives) is scaled by g_du_pf, which eases to 0 (menu up) and back to 1 (menu closed): the menu is
// the only thing on screen, the death screen returns when it closes. g_du_pf is the one multiplier for black, widgets, text and HUD.
static float g_duPf = 1.0f;
static uint64_t g_duPfT;
static void DuPauseFactor(uint64_t now)
{
    int paused = PageOnStack(0x27fd8e283ee18ea2ULL);  // fnv64("to1/pause-menu")
    float dt = g_duPfT ? (float) (now - g_duPfT) : 16.0f;
    g_duPfT = now;
    float step = dt / 120.0f;   // ~120 ms either way
    float pf = g_duPf + (paused ? -step : step);
    if (pf < 0.0f)
        pf = 0.0f;
    if (pf > 1.0f)
        pf = 1.0f;
    static int lastp = -1;
    if (paused != lastp) {
        lastp = paused;
        Xlog("death ui: pause menu %s - death screen %s (draw order: HUD canvas is over menu pages)", paused ? "on stack" : "closed", paused ? "hidden under it" : "back");
    }
    g_duPf = pf;
}

static void DuApply(uint64_t now, int force)
{
    float pf0 = g_duPf;
    DuPauseFactor(now);
    if (g_duPf != pf0)
        force = 1;
    for (int i = 0; i < DT_N; i++) {
        float v = DuVal(&g_du.tr[i], now) * g_duPf;
        if (!force && __builtin_fabsf(v - g_du.wrote[i]) < 0.002f)
            continue;
        WAlpha(DuW(DU_PATH[i]), v);
        g_du.wrote[i] = v;
    }
    float b = DuVal(&g_du.tr[DT_BLACK], now) * g_duPf;
    g_dui_text_alpha = (uint8_t) (255.0f * (1.0f - b) + 0.5f);
    g_duiBlack = b;            // our own attachable widgets (buddy ring, world prompt) follow it
    DuHudApply(1.0f - b);
}

static void DuReset(int hide)
{
    // (no execmus_fade here: du_reset(0) runs at the death EDGE = the execution kill frame, which cut the music while it was still
    // rising - nothing was heard; it fades when the death screen shows / the match ends / at the cap)
    DuHudRestore();
    if (hide)
        WVisible(DuW("t2rDeath"), 0);
    memset(&g_du, 0, sizeof g_du);
    g_duPf = 1.0f;
    g_duPfT = 0;
    for (int i = 0; i < DT_N; i++)
        g_du.wrote[i] = -1.0f;
    g_dui_text_alpha = 255;
    g_duiBlack = 0.0f;
}

static void FeatureDeathUi(void)
{
    ExecmusTick();
    DthTick(g_du.phase, g_du.final);
    static int off = -1, probed, found, armed, tries, dead_was;
    static uint64_t sig_used;   // g_du_edge_ms of the signal already turned into a death screen
    static uint64_t chk;
    uint64_t now = GetTickCount64();
    if (MatchSubstate() != 6) {
        if (g_du.phase) {
            Xlog("death ui: match state left 6 - death screen off");
            DuReset(found);
        }
        probed = found = armed = tries = dead_was = 0;
        g_dui_text_alpha = 255;
        g_duiBlack = 0.0f;
        DuDkReset();
        return;
    }
    if (!probed) {              // once per match: the baked widgets are there?
        uint64_t h = Hudw("t2rDeath");
        uint8_t *o = G2Ok() && h ? G2Find(h) : NULL;
        if (!o && ++tries < 300)
            return;
        probed = 1;
        found = o != NULL;
        Xlog("death ui: baked death screen %s (t2rDeath %p)",
             found ? "found in canvas.hudRoot" :
             "NOT FOUND (gui2-widgets.pak without it) - off, winmm's death banner stays",
             (void *) o);
        if (found) {
            DuReset(1);
            DuDkReset();
            {
                FILE *df = ExeSideOpen("t2r-death-delay.txt", "r");
                int v;
                g_duDelay = DU_DELAY_MS;
                if (df) {
                    if (fscanf(df, "%d", &v) == 1 && v >= 0 && v <= 3000)
                        g_duDelay = v;
                    fclose(df);
                }
                Xlog("death ui: instant mode: black %d ms after the kill frame (fade %d ms)%s", g_duDelay, DU_CUT_MS, df ? " (t2r-death-delay.txt)" : "");
                FILE *sf =
                    ExeSideOpen("t2r-death-screen-delay.txt", "r");
                double Sv;
                g_duLeadCfg = 0;      // reverted to the instant death screen + stinger; soft mode only with t2r-death-screen-delay.txt (secs)
                if (sf) {
                    if (fscanf(sf, "%lf", &Sv) == 1 && Sv >= 0.0
                        && Sv <= 6.0)
                        g_duLeadCfg = (int) (Sv * 1000.0 + 0.5);
                    fclose(sf);
                }
                Xlog("death ui: soft mode: stinger at the death edge, body death visible %d ms (clip length if longer, max %d), fade %d ms, widgets %d ms%s", g_duLeadCfg, DU_LEAD_MAX_MS, DU_SOFT_CUT_MS, DU_SOFT_UI_MS, sf ? " (t2r-death-screen-delay.txt)" : "");
            }
            if (!g_duTip)
                g_duTip = (int) (GetTickCount64() / 1000 % DU_NTIPS) + 1;      // 1-based: 0 = not seeded
            DuTipImage((g_duTip - 1) % DU_NTIPS);    // the next tip's picture streams in while we are alive (the group is hidden)
        }
    }
    if (!found)
        return;
    if (OvOff("t2r-no-death-ui.txt", &off, &chk)) {
        if (g_du.phase) {
            Xlog("death ui: t2r-no-death-ui.txt - off");
            DuReset(1);
        }
        return;
    }
    uint8_t *lc = LobbyObj();
    uint8_t *me = VdfPlayer();
    float hp = LocalHp();
    int death_state = me && VdfState(me) == VDF_DEATH_STATE;
    int dead = (hp > -0.5f && hp <= 0.0f) || death_state;
    int alive = hp > 0.0f && !death_state;
    // (log 13281 / 13306): an E execution on us showed the engine's Death state 25 frames BEFORE its kill frame (hp 1, the
    // victim half playing) - the black came before the gore. While our execution's kill is still pending (g_exec_until) only hp 0
    // or the kill frame's own signal (downed_death_noanim("execution") -> du_edge_note) start the death screen.
    int dead_edge = (hp > -0.5f && hp <= 0.0f) || (death_state
                                                   && !g_execUntil);
    int ended = lc && !IsBadReadPtr(lc + 0x2ca4, 1) && lc[0x2ca4];
    if (alive)
        armed = 1;              // alive once this match: a death edge counts
    if (alive && g_du.phase == 0)
        DuTrackKiller(now);
    // perf (death-edge stalls of 220-510 ms): the tip picture (streaming/large PNG) was only set at the match probe and at the edge itself, so every later death of a match
    // made the engine read a new large PNG synchronously on the tick. While alive, the picture of the NEXT death is set once per tip number into the hidden group: it streams in meanwhile.
    {
        static int pre_tip;
        if (alive && g_du.phase == 0 && g_duTip && pre_tip != g_duTip) {
            pre_tip = g_duTip;
            DuTipImage((g_duTip - 1) % DU_NTIPS);
        }
    }
    if (g_du.phase == 1
        && now - g_du.t0 < (uint64_t) (DU_DELAY_MS + DU_CUT_MS + 100))
        DuTrackKiller(now);   // the kill frame's own records (winmm resets them soon)

    // ---- the death edge = the frame the lethal hit was decided on our side: the earliest of the ext's own signal (du_edge_note: a
    // downed death - shot / melee while downed, bleed-out -, the execution kill frame, the molotov burn's lethal hit) and hp 0 / the
    // engine's Death state (a standing kill: the hit takes the hp in its own frame). A downed death can show in hp / state late.
    int sig = g_du_edge_ms && g_du_edge_ms != sig_used
        && now - g_du_edge_ms < 1500;
    if ((sig || (dead_edge && !dead_was)) && armed && !ended
        && (g_du.phase == 0 || g_du.phase == 2)) {
        DuReset(0);
        DuTrackKiller(now);   // live values first (the death may land in the frame of the killing hit)
        uint64_t t0 = sig ? g_du_edge_ms : now;
        g_du.phase = 1;
        g_du.t0 = t0;
        g_du.ph_t0 = now;
        g_du.cnt = -2;
        g_duDeathId = now;
        g_du.edge_frame = sig ? g_du_edge_frame : g_frame;
        g_du.seen_dead = dead;
        if (sig)
            sig_used = g_du_edge_ms;
        {
            static int off_inst = -1;
            static uint64_t chk_inst;
            int inst =
                OvOff("t2r-death-screen-instant.txt", &off_inst,
                       &chk_inst);
            int exec = GxVictimActive() || (g_xcamOn && !g_mqCam)
                || g_execUntil || (sig && g_duEdgeSrc
                                    && strstr(g_duEdgeSrc, "xecution"));
            g_duSoft = !inst && !exec && g_duLeadCfg > 0;
            if (g_duSoft) {
                int lead = g_duLeadCfg;
                if (g_dr.phase == 1 && g_dr.dur > 0.2f) {
                    int cl = (int) (g_dr.dur * 1000.0f);
                    if (cl > DU_LEAD_MAX_MS)
                        cl = DU_LEAD_MAX_MS;
                    if (cl > lead)
                        lead = cl;
                }
                g_duDelay = lead;
                g_duCut = DU_SOFT_CUT_MS;
                g_duUi = DU_SOFT_UI_MS;
            } else {
                g_duDelay = DU_DELAY_MS;
                g_duCut = DU_CUT_MS;
                g_duUi = DU_UI_MS;
            }
            g_du.lead_fit = 0;
            int shiv44 = 0;     // ps44 T6: the shiv rule below set the delay (T6 keeps it)
            // The shock sting must not be cut short by the death screen: a SHIV victim's black waits for the
            // shock sting to play out + 0.5 s (at least 1 s after the kill frame, at most 2.5 s); the death sting / theme follow it. t2r-no-shiv-death-delay.txt = instant
            if (!inst && (g_gx.shiv || g_gx.gshiv) && g_gx.role == 2) {
                static int off_sd = -1;
                static uint64_t chk_sd;
                if (!OvOff
                    ("t2r-no-shiv-death-delay.txt", &off_sd, &chk_sd)) {
                    int d = ShivstingRemainingMs() + 500;
                    if (d < 1000)
                        d = 1000;
                    if (d > 2500)
                        d = 2500;
                    g_duDelay = d;
                    // the death screen for shivs must not come after the full anim ends (static victim), it cuts while there is still motion: the
                    // black is full at least 0.35 s before our victim half ends (log: fist-insta-shiv-stand-front, half over 12 frames before the black)
                    int left = PsyncVicLeftMs();
                    if (left >= 0) {
                        int cap = left - g_duCut - 350;
                        if (cap < 250)
                            cap = 250;
                        if (d > cap)
                            d = cap;
                        g_duDelay = d;
                    }
                    shiv44 = 1;
                    Xlog("death ui: shiv victim - the black waits %d ms (shock sting left %d ms + 0.5 s, victim half left %d ms)", d, ShivstingRemainingMs(), left);
                }
            }
            // ps44 T6 (B black at 23.63 s while A's execution runs to ~24.5; A black 76.03 while B's last bat strike lands 76.3-76.5; B 96.97; A 159.47 before any
            // fall): an execution / finisher victim's black came DU_DELAY_MS after the kill frame - the victim never saw the end of the move that killed it (SP death screens
            // show it). A death inside a paired move we play the VICTIM half of: the fade waits until that half
            // has ended or DU_PAIR_HOLD_MS after the kill frame, whichever is first. Only the picture: the respawn timer is not touched. t2r-death-pair-hold-old.txt = old.
#define DU_PAIR_HOLD_MS 1200    // [TUNING ps44] a chosen value, labelled
            // (the shiv rule holds here too: the black is FULL while the body still moves - the fade starts at most left - cut - 350 ms after the kill
            //  frame, never earlier than the old delay; a shiv victim's delay set above is kept as it is)
            if (!inst && !shiv44 && g_ps.role == 2
                && g_ps.ent_vic == SID_PLAYER) {
                static int off44 = -1;
                static uint64_t chk44;
                int left = PsyncVicLeftMs();
                if (left > 0) {
                    int old44 =
                        OvOff("t2r-death-pair-hold-old.txt", &off44,
                               &chk44);
                    int d =
                        left < DU_PAIR_HOLD_MS ? left : DU_PAIR_HOLD_MS,
                        cap44 = left - g_duCut - 350;
                    // ps45 (A black card 39.20 s / 74.27 s, B 95.23 s = 0.34 s before the execution clip's end, both screens; log "our half has 904 ms
                    // left: the fade starts after 484 ms" + "black at 563 ms" = the -350 ms shiv margin above): an EXECUTION victim's black is full on the last
                    // frame of our victim half (the attacker's strikes run to the end; the static-victim shiv rule is about a still body, an execution
                    // victim is struck until the end). Finishers keep the margin (finisher deaths read right). t2r-death-exec-end-ps45-old.txt = ps44.
                    {
                        static int off45 = -1;
                        static uint64_t chk45;
                        if (exec
                            && !OvOff("t2r-death-exec-end-ps45-old.txt",
                                       &off45, &chk45)) {
                            cap44 = left - g_duCut;
                            Xlog("death ui: ps45 execution victim - the black is full on our half's last frame (cap %d ms after the kill frame, was %d; t2r-death-exec-end-ps45-old.txt = old)", cap44, cap44 - 350);
                        }
                    }
                    if (d > cap44)
                        d = cap44;      // (below the old delay: the old delay stays - see the d > g_du_delay test)
                    Xlog("death ui: died inside paired move u%08x (kind %d) as its victim - our half has %d ms left: the fade starts after %d ms, not %d (ps44 T6, hold max %d ms%s)", g_ps.uid, g_ps.kind, left, old44 || d <= g_duDelay ? g_duDelay : d, g_duDelay, DU_PAIR_HOLD_MS, old44 ? "; OLD: t2r-death-pair-hold-old.txt" : "");
                    if (!old44 && d > g_duDelay)
                        g_duDelay = d;
                }
            }
            Xlog("death ui: %s mode (%s)", g_duSoft ? "soft" : "instant",
                 inst ? "t2r-death-screen-instant.txt" : exec ?
                 "execution death, its own camera" : g_duLeadCfg >
                 0 ? "default" : "delay 0");
        }
        if (g_cw_deaths < 65535)
            g_cw_deaths++;      // match stats DEATHS (t2r-ext.c report label 22)
        Xlog("death ui: death edge from %s at frame %llu (hp/state %s at frame %llu%s)", sig ? g_duEdgeSrc : "hp 0 / Death state", (unsigned long long) g_du.edge_frame, dead ? "dead" : "not dead yet", (unsigned long long) g_frame, sig ? ", the signal's own ms is the clock" : "");
        for (int i = 0; i < DT_N; i++)
            DuSet(&g_du.tr[i], 0.0f, now);
        DuSet(&g_du.tr[DT_SPEC_A], 1.0f, now);
        // a signalled death that the engine has not confirmed yet (hp 0 / Death state not seen): the black waits for it - a refused hit
        // must not black the screen of a living player. Confirmed within the delay = exactly the old timing; later = the black starts then
        // (the timeline shifts); never within 600 ms = the screen is dropped (see black_wait below)
        if (dead)
            DuGo(&g_du.tr[DT_BLACK], 1.0f, g_duDelay, g_duCut, now);
        else
            g_du.black_wait = 1;
        WVisible(DuW("t2rDeath.dSpec.dSpecLbl"), 1);
        WVisible(DuW("t2rDeath.dSpec.dSpecLblB"), 1);
        DuApply(now, 1);
        WVisible(DuW("t2rDeath"), 1);
        int ti = (g_duTip - 1 + DU_NTIPS) % DU_NTIPS;
        DuTipImage(ti);
        DuText("t2rDeath.dTip.dTipText", DuTipText(ti));
        g_duTip = g_duTip % DU_NTIPS + 1;
        {                       // where we died (a player fell through collision and died right after a pipe finisher - the log had no position)
            float lb[16];
            const float *l = PlayerLocator(lb);
            Xlog("death ui: died (hp %.1f, state %s, downed %d, execution victim %d, xcam %d) at %.2f %.2f %.2f - fade to black after %d ms over %d ms; tip #%d %s \"%.60s\"", hp, death_state ? "Death" : "other", g_downed, GxVictimActive(), g_xcamOn, l ? l[0] : 0.0f, l ? l[1] : 0.0f, l ? l[2] : 0.0f, g_duDelay, g_duCut, ti, DU_TIPS[ti].img, DuTipText(ti));
        }
    }
    if (sig && g_du.phase == 1 && g_du_edge_ms != sig_used)
        sig_used = g_du_edge_ms;        // (a signal during a running death screen)
    dead_was = dead_edge;
    if (!g_du.phase)
        return;
    if (g_du.phase == 1 && !g_du.seen_dead && dead) {
        g_du.seen_dead = 1;
        Xlog("death ui: hp/state dead seen at frame %llu - %llu frames / %llu ms after the death edge (frame %llu)", (unsigned long long) g_frame, (unsigned long long) (g_frame - g_du.edge_frame), (unsigned long long) (now - g_du.t0), (unsigned long long) g_du.edge_frame);
    }
    DuWinmmBannerOff();
    uint64_t el = now - g_du.t0;
    if (g_du.black_wait) {
        if (dead) {
            g_du.black_wait = 0;
            if (el > (uint64_t) g_duDelay) {
                g_du.t0 = now - (uint64_t) g_duDelay;
                el = (uint64_t) g_duDelay;
            }
            DuGo(&g_du.tr[DT_BLACK], 1.0f,
                  (int) ((uint64_t) g_duDelay - el), g_duCut, now);
            Xlog("death ui: death confirmed %llu ms after the edge - black follows", (unsigned long long) (now - g_du.t0));
        } else if (el > 600 || ended) {
            Xlog("death ui: the signalled death (%s) was not confirmed in %llu ms (hit refused / undone) - no black, death screen dropped", g_duEdgeSrc, (unsigned long long) el);
            DuReset(1);
            return;
        } else {
            DuApply(now, 0);
            return;
        }
    }

    // ---- match end: the VICTORY / DEFEAT banner takes priority, the death screen leaves
    if (ended && g_du.phase != 3) {
        g_du.phase = 3;
        g_du.ph_t0 = now;
        for (int i = 0; i < DT_N; i++)
            DuGo(&g_du.tr[i], 0.0f, 0, DU_END_MS, now);
        Xlog("death ui: match ended %llu ms into the death screen - fading out for the end banner", (unsigned long long) el);
    }
    if (g_du.phase == 3) {
        DuApply(now, 0);
        if (now - g_du.ph_t0 > DU_END_MS + 50) {
            DuReset(1);
            Xlog("death ui: off (match end)");
        }
        return;
    }
    // ---- respawn: our black leaves under the respawn's own fade from black
    if (g_du.phase == 1 && !g_du.seen_dead && now - g_du.t0 > 4000)     // the signalled death never came (refused / undone): leave
    {
        Xlog("death ui: the signalled death (%s) never showed in hp / state within 4 s - death screen off", g_duEdgeSrc);
        g_du.seen_dead = 1;
    }
    if (alive && g_du.phase == 1 && g_du.seen_dead) {
        g_du.phase = 2;
        g_du.ph_t0 = now;
        for (int i = DT_TIP; i < DT_N; i++)
            DuGo(&g_du.tr[i], 0.0f, 0, 120, now);
        DuGo(&g_du.tr[DT_BLACK], 0.0f, DU_RESPAWN_HOLD, DU_RESPAWN_MS,
              now);
        Xlog("death ui: respawned %llu ms after the death (black %.2f) - black out over %d ms after %d ms", (unsigned long long) el, DuVal(&g_du.tr[DT_BLACK], now), DU_RESPAWN_MS, DU_RESPAWN_HOLD);
    }
    if (g_du.phase == 2) {
        DuApply(now, 0);
        if (now - g_du.ph_t0 > DU_RESPAWN_HOLD + DU_RESPAWN_MS + 50) {
            DuReset(1);
            DuDkReset();      // a new life: no killer records from the last one
            DuTipImage((g_duTip - 1) % DU_NTIPS);    // stream the next tip's picture
            ExecmusFade("respawned");
            Xlog("death ui: off (respawned)");
        }
        return;
    }
    // ---- phase 1: dead
    if (!g_du.logged_black && el >= (uint64_t) DU_LAND) {
        g_du.logged_black = 1;
        Xlog("death ui: black at %llu ms", (unsigned long long) el);
    }
    // soft mode: the stinger (and with it the music cut) plays at the death edge; the world stays visible for the body's death
    if (g_duSoft && !g_du.black_wait && !g_du.lead_fit) {      // the clip may start a frame after the edge: its length extends the lead once; the countdown caps it
        if (g_dr.phase == 1 && g_dr.dur > 0.2f && el < 500) {
            g_du.lead_fit = 1;
            int cl = (int) (g_dr.dur * 1000.0f);
            if (cl > DU_LEAD_MAX_MS)
                cl = DU_LEAD_MAX_MS;
            if (cl > g_duDelay && el < (uint64_t) g_duDelay) {
                g_duDelay = cl;
                DuSet(&g_du.tr[DT_BLACK], 0.0f, now);
                DuGo(&g_du.tr[DT_BLACK], 1.0f, (int) (g_duDelay - el),
                      g_duCut, now);
                Xlog("death ui: death clip %s plays %.2f s - fade to black starts at %d ms", g_dr.clip ? g_dr.clip : "?", g_dr.dur, g_duDelay);
            }
        } else if (el >= 500)
            g_du.lead_fit = 1;
    }
    if (g_duSoft && !g_du.final && !g_du.spec && lc && lc[0x2bf8 + 8] && el < (uint64_t) g_duDelay) { // respawn timing never gets longer: the screen fits before the countdown's 1
        float rm = *(float *) (lc + 0x2bf8 + 4);
        int room = (int) ((rm - DU_FINAL_AT) * 1000.0f) - g_duCut - g_duUi;   // ms of lead the countdown allows from now
        int want = (int) el + (room > 0 ? room : 0);
        if (want < g_duDelay) {
            g_duDelay = want;
            DuSet(&g_du.tr[DT_BLACK], 0.0f, now);
            DuGo(&g_du.tr[DT_BLACK], 1.0f,
                  g_duDelay > (int) el ? g_duDelay - (int) el : 0,
                  g_duCut, now);
            static uint64_t nl_;
            if (now - nl_ > 1000) {
                nl_ = now;
                Xlog("death ui: countdown has %.2f s left - body-death lead shortened to %d ms", rm, g_duDelay);
            }
        }
    }
    if (g_duSoft && !g_du.sting && !g_du.black_wait) { // an unconfirmed signalled death waits for the engine's confirmation
        g_du.sting = 1;
        ShivstingCut(40, "death sting");
        PullstingCut(40, "death sting");       // (shiv_sting.c / pullout_sting.c) the shock / pull-out stings make way for the death sting
        int ok =
            PcmPlayStereo(ds_blob, ds_blob + DS_N, DS_N, 1.0f, DS_RATE);
        Xlog("death ui: sting t2-death-stinger-shorter (%.2f s, PCM mixer 2D) %s at the death edge (%llu ms), music now %llx", DS_N / (float) DS_RATE, ok ? "playing" : "NOT played (mixer off / voices busy)", (unsigned long long) el, (unsigned long long) MusicCurrent());
        DthArm(ok ? DS_N * 1000 / DS_RATE : 0, 0);     // death drone (death_theme.c): starts now under the sting (t2r-no-death-drone.txt = off)
    }
    if (!g_du.sting && el >= (uint64_t) DU_LAND) {
        g_du.sting = 1;
        ShivstingCut(40, "death sting");
        PullstingCut(40, "death sting");       // (shiv_sting.c / pullout_sting.c) the shock / pull-out stings make way for the death sting
        int xcue = ExmusLive() || g_exmusPend.at != 0;
        int ok = ExmusLive()? (Xlog("death ui: no death sting - the execution music plays"), 0) : PcmPlayStereo(ds_blob, ds_blob + DS_N, DS_N, 1.0f, DS_RATE);       // special execution deaths have no sting
        Xlog("death ui: sting t2-death-stinger-shorter (%.2f s, PCM mixer 2D) %s at %llu ms, music now %llx", DS_N / (float) DS_RATE, ok ? "playing" : "NOT played (mixer off / voices busy)", (unsigned long long) el, (unsigned long long) MusicCurrent());
        DthArm(ok ? DS_N * 1000 / DS_RATE : 0, xcue);  // death drone (death_theme.c): starts now, under the sting / the execution victim cue
    }
    if (!g_du.filled && el + 100 >= (uint64_t) DU_LAND) {
        g_du.filled = 1;
        DuFillCard(now);
        DuCount(lc, 1);
    }
    if (g_du.filled)
        DuCount(lc, 0);
    if (el >= (uint64_t) DU_LAND && !g_du.ui_in && !g_du.final && !g_du.spec) { // the death screen comes up over the black
        g_du.ui_in = 1;
        // (the execution music keeps playing over the death screen - its best moment is mid-cue; it ends at respawn)
        DuGo(&g_du.tr[DT_TIP], 1.0f, 0, g_duUi, now);
        DuGo(&g_du.tr[DT_CARD], 1.0f, 0, g_duUi, now);
        DuGo(&g_du.tr[DT_COUNT], 1.0f, 0, g_duUi, now);
        Xlog("death ui: tip + KILLED BY + countdown fading in at %llu ms",
             (unsigned long long) el);
    }
    int run = lc && lc[0x2bf8 + 8];
    float rem = lc ? *(float *) (lc + 0x2bf8 + 4) : 0.0f;
    // ---- countdown at 1: fade to black (the respawn's own fade from black follows)
    // On the last respawn, if you died at the same time as someone else you got stuck on a black screen at 0. The
    // team's last reinforcement went to the teammate whose countdown ended first (winmm spends it at the respawn): ours then never
    // respawns, but this fade to black (a respawn's own fade follows) had already started and nothing ever lifted it. No countdown
    // fade once the team has no lives; and a fade that was started, with the timer gone and no respawn, is taken back (below).
    int lives = lc ? ((int (*)(void *)) T2R_SYM(kFnTeamLives)) (lc) : 1;
    int nolives = lives <= 0 && lives > -1000;
    if (!g_du.final && run && rem <= DU_FINAL_AT
        && el >= (uint64_t) DU_LAND && !nolives) {
        g_du.final = 1;
        g_du.final_t0 = now;
        DuGo(&g_du.tr[DT_BLACK], 1.0f, 0, DU_FINAL_MS, now);
        for (int i = DT_TIP; i < DT_N; i++)
            if (i != DT_SPEC_A && i != DT_SPEC_B)
                DuGo(&g_du.tr[i], 0.0f, 0, DU_FINAL_MS - 50, now);
        Xlog("death ui: countdown at 1 (remaining %.2f s, %s) - fade to black over %d ms", rem, g_du.spec ? "spectating" : "death screen", DU_FINAL_MS);
    }
    if (g_du.final && !run && !alive && now - g_du.final_t0 > 3000) {   // the countdown ended and no respawn came (no lives, or the grant is late)
        g_du.final = 0;
        g_du.ui_in = 0;
        DuGo(&g_du.tr[DT_BLACK], 1.0f, 0, 100, now);
        Xlog("death ui: countdown ended with no respawn (team lives %d) - death screen back (%s), spectate offered", lives, nolives ? "NO REINFORCEMENTS" : "RESPAWNING");
    }
    // ---- spectate a teammate (button), and back
    int mate = -1;
    int avail = !g_du.final && el >= (uint64_t) (DU_LAND + g_duUi)
        && DuMateAlive(&mate) && (!run || rem > DU_SPEC_MIN_S);
    if (!g_du.final)
        DuGo(&g_du.tr[DT_SPEC], avail ? 1.0f : 0.0f, 0, 250, now);
    int press = DuButton();
    if (avail && !g_du.spec && !g_du.auto_spec && !run && nolives) {    // 2013: out of reinforcements = you watch your team (no button needed)
        g_du.auto_spec = 1;
        press = 1;
        Xlog("death ui: no reinforcements left - spectating teammate slot %d automatically", mate);
    }
    if (avail && press && now - g_du.spec_t0 > 400) {
        g_du.spec = !g_du.spec;
        g_du.spec_t0 = now;
        if (g_du.spec) {
            if (lc && !lc[0x2c44]) {    // winmm's spectate (as its out-of-lives path sets it), first pick on its next tick
                lc[0x2c44] = 1;
                *(float *) (lc + 0x2c48) = 2.9f;
                *(int32_t *) (lc + 0x2c4c) = -1;
                *(int32_t *) (lc + 0x2c50) = 0;
                lc[0x2c54] = 0;
                *(volatile uint8_t *) T2R_SYM(kUiRefreshFlag) = 0;
            }
            DuGo(&g_du.tr[DT_TIP], 0.0f, 0, DU_SPEC_OUT_MS, now);
            DuGo(&g_du.tr[DT_CARD], 0.0f, 0, DU_SPEC_OUT_MS, now);
            DuGo(&g_du.tr[DT_BLACK], 0.0f, DU_SPEC_LIFT, DU_SPEC_LIFT_MS,
                  now);
            DuGo(&g_du.tr[DT_SPEC_A], 0.0f, 0, 200, now);
            DuGo(&g_du.tr[DT_SPEC_B], 1.0f, 150, 200, now);
            Xlog("death ui: spectate ON at %llu ms (teammate slot %d alive; winmm spectate %d, target %d) - tip + card out, black lifts at +%d ms", (unsigned long long) el, mate, lc ? lc[0x2c44] : -1, lc ? *(int32_t *) (lc + 0x2c4c) : -9, DU_SPEC_LIFT);
        } else {
            DuGo(&g_du.tr[DT_BLACK], 1.0f, 0, DU_BACK_MS, now);
            DuGo(&g_du.tr[DT_TIP], 1.0f, DU_BACK_MS, DU_UI_MS, now);
            DuGo(&g_du.tr[DT_CARD], 1.0f, DU_BACK_MS, DU_UI_MS, now);
            DuGo(&g_du.tr[DT_SPEC_A], 1.0f, 150, 200, now);
            DuGo(&g_du.tr[DT_SPEC_B], 0.0f, 0, 200, now);
            Xlog("death ui: spectate OFF at %llu ms - back to the death screen", (unsigned long long) el);
        }
    }
    if (g_du.spec && !g_du.final) {     // the teammate died / left while we watch: back to the death screen
        if (avail || (run && rem <= DU_SPEC_MIN_S))
            g_du.mate_lost_t0 = 0;
        else if (!g_du.mate_lost_t0)
            g_du.mate_lost_t0 = now;
        else if (now - g_du.mate_lost_t0 > 600) {
            g_du.spec = 0;
            g_du.spec_t0 = now;
            g_du.mate_lost_t0 = 0;
            DuGo(&g_du.tr[DT_BLACK], 1.0f, 0, DU_BACK_MS, now);
            DuGo(&g_du.tr[DT_TIP], 1.0f, DU_BACK_MS, DU_UI_MS, now);
            DuGo(&g_du.tr[DT_CARD], 1.0f, DU_BACK_MS, DU_UI_MS, now);
            DuGo(&g_du.tr[DT_SPEC_A], 1.0f, 150, 200, now);
            DuGo(&g_du.tr[DT_SPEC_B], 0.0f, 0, 200, now);
            g_du.auto_spec = 0;
            Xlog("death ui: no teammate to watch any more - back to the death screen");
        }
    }
    // ---- spectating bar: who we watch + the switch prompt; LMB / RT = next living teammate
    {
        static int shown_slot = -2;
        int cur = g_du.spec && lc && lc[0x2c44]
            && !IsBadReadPtr(lc + 0x2c4c,
                             4) ? *(int32_t *) (lc + 0x2c4c) : -1;
        int ml[T2R_MAX_PEERS], nm =
            g_du.spec ? DuMates(ml, T2R_MAX_PEERS) : 0, valid = 0;
        for (int i = 0; i < nm; i++)
            if (ml[i] == cur)
                valid = 1;
        if (!g_du.spec) {
            DuSwitchButton(1);
            shown_slot = -2;
        } else {
            int dir = DuSwitchButton(0);
            static uint64_t last_dbg;
            if (dir && now - last_dbg > 300) {
                last_dbg = now;
                Xlog("death ui: switch press dir %d (cur %d, %d living teammates, since-on %llu ms, pf %.2f, final %d)", dir, cur, nm, (unsigned long long) (now - g_du.spec_t0), g_duPf, g_du.final);
            }
            if (dir && now - g_du.spec_t0 > 500 && g_duPf > 0.99f
                && nm > 0 && !g_du.final) {
                int nx = dir > 0 ? ml[0] : ml[nm - 1];
                if (dir > 0) {
                    for (int i = 0; i < nm; i++)
                        if (ml[i] > cur) {
                            nx = ml[i];
                            break;
                        }
                }               // the next slot up, wrapping
                else {
                    for (int i = nm - 1; i >= 0; i--)
                        if (ml[i] < cur) {
                            nx = ml[i];
                            break;
                        }
                }               // the next slot down, wrapping
                if (nx != cur && lc && lc[0x2c44]) {
                    // winmm drives the spectate camera (FUN_180039090) only while its timer +0x2c48 is >= the 3 s period (decomp_spec3.c:
                    // "if (timer < period) goto skip"); zeroing it (the old code) froze the camera on the old view for 3 s = "switch does
                    // nothing". Set it past the period: the next tick re-tests the new target (spectate_ok) and drives its camera.
                    *(int32_t *) (lc + 0x2c4c) = nx;
                    *(float *) (lc + 0x2c48) =
                        SPEC_PERIOD_MS / 1000.0f + 0.1f;
                    *(volatile uint8_t *) T2R_SYM(kUiRefreshFlag) = 0;
                    Xlog("death ui: spectate switch %d -> %d (%d living teammates)", cur, nx, nm);
                    cur = nx;
                    valid = 1;
                } else if (nx == cur)
                    Xlog("death ui: switch press ignored: only teammate %d is watchable", cur);
            }
        }
        if (valid) {
            typedef uint8_t *(*fn_peer)(uint32_t);
            uint8_t *p = ((fn_peer) T2rPeer) ((uint32_t) cur);
            const char *nmx = p
                && !IsBadReadPtr(p + 6, 2) ? (const char *) (p + 6) : NULL;
            char b[48];
            snprintf(b, sizeof b, "%.40s", nmx
                     && nmx[0] ? nmx : "TEAMMATE");
            DB_TEXT_W(DuW("t2rDeath.dSpecBar.dSpecNm"),
                      DuW("t2rDeath.dSpecBar.dSpecNmB"), b);
            if (cur != shown_slot) {
                shown_slot = cur;
                Xlog("death ui: spectating slot %d '%s'", cur, b);
            }
        }
        if (!g_du.final) {
            DuGo(&g_du.tr[DT_BAR], g_du.spec
                  && valid ? 1.0f : 0.0f, 0, g_du.spec
                  && valid ? 250 : 150, now);
            DuGo(&g_du.tr[DT_SWITCH], g_du.spec && valid
                  && nm >= 2 ? 1.0f : 0.0f, 0, 200, now);
        }
    }
    DuApply(now, 0);
}

// the victim / down camera holds until the black is full (xcam weights)
static int DuCamHold(void)
{
    return g_du.phase == 1 && !g_du.logged_black && !g_du.spec;
}
