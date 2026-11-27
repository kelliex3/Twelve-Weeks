// skills.c - TLOU 2013 Factions SURVIVAL SKILLS (re/factions2013/Survival_Skills.wiki) for t2r-ext.
// Included from t2r-ext.c just before t2r_tick (ids / encoding / hook prototypes: skills.h).
//
// What lives here:
//  - SK[]: every 2013 skill (13 basic + 13 DLC), levels I-III, LP costs, 2013 descriptions, icon,
//    and which levels have an effect in this mod (others show LOCKED in the picker).
//  - sk_lookup: replaces winmm's skill record lookup FUN_18001d420 -> winmm's GEAR UP edit page,
//    the LP total (lo_cost) and the loadout list read our records.
//  - the 4th skill slot (2013 had 4): g_sk4[] per custom loadout (t2r-skills.txt), CLASSES[].s4 for
//    the pre-made classes, g_sk4_cur for the lobby loadout; to1/loadout-menu row "SkillFour".
//  - sk_picker_page: to1/picker-skill opened from the edit page = the 2013 skill picker: all 31 (26 + the 5 melee skills)
//    skills, A/D picks the level (I/II/III boxes in the DETAILS panel), select equips into the
//    skill row that opened it; 13 LP budget.
//  - effects (skills_tick / skills_on_spawn / hooks), see SK[].note for what is not in yet:
//    * winmm's own perk code through the perk mask ctx+0x2c70, now written from the equipped
//      skills: 0x80 health kit speed (First-Aid Training), 0x08 crouch-walk x1.3 + fast climb
//      (Agility), 0x20 no weapon sway (Sharpshooter III);
//    * the game's own single-player upgrades (player-upgrade-settings.bin *player-upgrades-defs-map*)
//      granted for the match with give-player-upgrade-by-id and taken back after (only the ones we
//      granted): crafting-speed-increase, health-kit-speed-increase, listen-mode-speed-increase(-2),
//      aim-wobble-reduce / aim-sway-reduce-while-moving / aim-hold-breath, pipe-bomb / mine /
//      molotov-radius-increase, shiv-craft-double / -triple, crouch-run-quiet,
//      melee-upgrade-durability, stealth-kill-speed-increase;
//    * winmm's per-hit perk gate FUN_18001d7d0 (Head Hunter) replaced: headshot heal 15 / 25
//      (Sharpshooter II / III), melee hit heal 10 (Brawler), damage marks (Damage Marker);
//    * winmm's parts perk gate FUN_18001d820 (Collector) replaced: +10% / +20% parts;
//    * ext mechanisms: listen meter (Sharp Ears), listen-mode visibility (Covert Training II via
//      g_peer_covert2), marks (Hawk-eyed / Covert III / Strategist), radar (Strategist II), revive
//      (Reviver), downed bleed-out (Fortitude), spawn kit (Covert shiv, Gunslinger, Fortitude kit,
//      Second Chance ammo), awards (Collector, Reviver, Executioner, Lone Wolf).
//  - peer sync: MSG_SK_SYNC (every 2 s + at spawn), MSG_SK_MARKED, MSG_SK_HEAL (skills.h).

// drawn 2013-style skill badges (tools/models/skill_icons.py), staged by build_dll with the other t2r icons
#define SK_ICON(n) "streaming/t2r/icons/skill-" n ".png"
typedef struct {
    const char *name, *icon;
    uint8_t nlv, cost[3];
    uint8_t avail;                 // bit k: level k+1 does something here (else LOCKED)
    const char *desc[3];           // 2013 (Survival_Skills.wiki)
    const char *note;              // what is not in yet / how it is done here (NULL: as 2013)
} sk_def_t;
static const sk_def_t SK[SK_COUNT] = {
    [SK_ZOOM] = {"PISTOL AUTO-ZOOM", SK_ICON("zoom"), 2, {1, 2}, 3,
        {"Zoom in while aiming a 9mm, Revolver, Enforcer or Burst Pistol.",
         "Zoom in further while aiming a 9mm, Revolver, Enforcer or Burst Pistol."},
        "The game has no pistol aim zoom: the camera FOV is narrowed while aiming a pistol (tangent space /1.25 / /1.5) over the 2013 auto-zoom duration of 1 s. The 2013 FOV factors live in net-10 scripts that are not in the extracts, so 1.25 / 1.5 stay estimates."},
    [SK_EXPLO] = {"EXPLOSION EXPERT", SK_ICON("explosion"), 2, {1, 3}, 3,
        {"The radius of your explosives is 20% bigger. The radius of your Molotovs is 10% bigger.",
         "The radius of your explosives is 40% bigger. The radius of your Molotovs is 20% bigger."},
        "Exact 2013 ring scale 1.2 / 1.4 (Molotov 1.1 / 1.2) applied by the ext itself to every ring / trigger sphere of the THROWER's bombs (r13_expl_mul, ex13_scale, placedbomb2013.c pb_owner_radius); no engine upgrade."},
    [SK_REVIVER] = {"REVIVER", SK_ICON("reviver"), 3, {1, 3, 4}, 7,
        {"Revive fallen teammates 30% faster and with 20% more health.",
         "Revive fallen teammates 45% faster and with 25% more health. Get 25% more Parts from revives.",
         "Revive fallen teammates 55% faster and with 30% more health. Get 50% more Parts from revives."}, NULL},
    [SK_BRAWLER] = {"BRAWLER", SK_ICON("brawler"), 2, {2, 3}, 3,
        {"Craft modded weapons in half the time and gain 10 health back with each melee hit.",
         "Craft modded weapons in half the time, they have an additional hit, and gain 10 health back with each melee hit."},
        "Half-time crafting of modded weapons: the engine crafting-speed upgrade runs while a melee upgrade is being crafted (not tuned to exactly half)."},
    [SK_COVERT] = {"COVERT TRAINING", SK_ICON("covert"), 3, {2, 4, 5}, 7,
        {"Start with a shiv when you spawn.",
         "You won't show up in the enemy's listen mode when crouch walking, and start with a shiv when you spawn.",
         "You can't be marked, won't show up in the enemy's listen mode when crouch walking, and start with a shiv when you spawn."}, NULL},
    [SK_EARS] = {"SHARP EARS", SK_ICON("ears"), 3, {1, 2, 3}, 7,
        {"The listen mode meter regenerates 15% faster and you can listen for 20% longer.",
         "The listen mode meter regenerates 30% faster and you can listen for 20% longer.",
         "Move quickly while in listen mode. The meter regenerates 35% faster and you can listen for 30% longer."},
        "III: moving in listen mode uses the game's listen-mode speed upgrades."},
    [SK_STRAT] = {"STRATEGIST", SK_ICON("strategist"), 3, {1, 4, 5}, 7,
        {"Know when you've been marked.",
         "Know when you've been marked and see nearby enemies on the radar.",
         "Know when you've been marked, see nearby enemies on the radar, and respawn on an ally several seconds early."},
        "Radar: enemies within 8 m. III: respawns 3 s early on the safest living ally (the host picks the spawner next to him; there is no ally chooser)."},
    [SK_HAWK] = {"HAWK-EYED", SK_ICON("hawk"), 3, {2, 3, 4}, 7,
        {"Your marks last 3 seconds longer. Aim at an enemy and press |gen:flashlight| to mark.",
         "Your marks last 3 seconds longer and marked enemies glow.",
         "Your marks last 3 seconds longer, marked enemies glow, and enemies close to your target are marked too."},
        "Marked enemies glow for every player in this mod. Marks last 5 s (8 s with this skill), group mark radius 3 m."},
    [SK_CRAFTER] = {"CRAFTER", SK_ICON("crafter"), 3, {2, 4, 6}, 7,
        {"Craft items 75% faster.",
         "Craft items 75% faster. Every 3 items you craft give you a gift box for an ally.",
         "Craft items 75% faster. Every 2 items you craft give you a gift box for an ally."},
        "Uses the game's crafting speed upgrade (a fixed engine speed-up, not exactly 75%). The gift box goes straight to the nearest teammate (a crafting ingredient)."},
    [SK_FAT] = {"FIRST-AID TRAINING", SK_ICON("firstaid"), 3, {2, 4, 6}, 7,
        {"Use health kits 75% faster.",
         "Use health kits 75% faster. Hold E next to an injured teammate to heal them 10 health every second.",
         "Use health kits 75% faster. Hold E next to an injured teammate to heal them 20 health every second."},
        "Kit use animation rate = exact 2013 health-kit-speed 1.75 (sk_kit_speed writes the engine's kit-rate global); heal-ally ticks 10 / 20 hp."},
    [SK_SHARP] = {"SHARPSHOOTER", SK_ICON("sharpshooter"), 3, {2, 4, 5}, 7,
        {"Scope sway is reduced by 25% and damage wobble by 20%.",
         "Scope sway is reduced by 37% and damage wobble by 35%. Get 15 health back for each headshot.",
         "Scope sway is reduced by 50% and damage wobble by 65%. Get 25 health back for each headshot."},
        "Exact 2013 factors written into the engine's own numbers (sk_aim_values): damage wobble x0.8 / 0.65 / 0.45 (upgrade record float), sway x0.75 / 0.625 / 0.5 (*upgraded-aim-sway*); headshot heal 15 / 25 by code."},
    [SK_MARATHON] = {"MARATHON RUNNER", SK_ICON("marathon"), 2, {1, 3}, 3,
        {"Sprint for 15% longer and sprint regenerates 15% faster.",
         "Sprint for 30% longer and sprint regenerates 30% faster."},
        "2013 limited sprint (7 s, then you run until the meter is back to 30%); Marathon Runner drains it 15 / 30% slower and refills it 15 / 30% faster."},
    [SK_COLLECTOR] = {"COLLECTOR", SK_ICON("collector"), 2, {3, 5}, 3,
        {"Increase the number of Parts you earn by 10%.",
         "Increase the number of Parts you earn by 20%. (Not Parts from healing teammates.)"}, NULL},
    [SK_EXEC] = {"EXECUTIONER", SK_ICON("executioner"), 3, {1, 3, 4}, 7,
        {"Earn double the Parts for shiv kills.",
         "Earn 50% more Parts for special executions, double the Parts for shiv kills, and get 1 extra shiv use every time you craft a shiv.",
         "Earn double the Parts for special executions and shiv kills, and get 2 extra shiv uses every time you craft a shiv."},
        "Shiv kills (2013 insta-shiv on a stunned enemy / from behind) pay 75 Parts, doubled by this skill. Interrogations pay x1.5 (II) / x2 (III). Extra shiv uses: +1 / +2 shivs added by code when a crafted shiv lands in the pack (sk_exec_shiv_uses)."},
    [SK_DMGMARK] = {"DAMAGE MARKER", SK_ICON("damage-marker"), 2, {2, 4}, 3,
        {"Any damage you deal marks the target.",
         "Earn double the Parts for marking, and any damage you deal marks the target."},
        "II: Parts for marking are doubled."},
    [SK_GUNSLINGER] = {"GUNSLINGER", SK_ICON("gunslinger"), 2, {2, 4}, 3,
        {"Pistol ammo is increased by 50% at spawn.", "Pistol ammo is doubled at spawn."}, NULL},
    [SK_BOMB] = {"BOMB EXPERT", SK_ICON("bomb"), 3, {2, 3, 5}, 7,
        {"Smoke bombs have a very short fuse.",
         "Placed bombs, thrown smoke bombs and bombs have an even shorter fuse.",
         "You are immune to the stun of your own smoke bombs. Placed bombs, smoke bombs and bombs have a very short fuse."},
        "Fuse 1.5 s -> smoke 1.0 / 0.8 / 0.6 s, pipe bombs 1.5 / 0.9 / 0.7 s (II, III). Your own smoke bombs stun you like anyone's (own_stun_check -> burst_queue -> stun_local, 2013 radius + line of sight); only Bomb Expert III is immune (sk_stun_immune = buff b8a0c297)."},
    [SK_SCAV] = {"SCAVENGER", SK_ICON("scavenger"), 3, {2, 4, 5}, 7,
        {"Enemies drop 50% more ammo.",
         "Automatically pick up ammo and supplies from kills, and enemies drop double the ammo.",
         "Enemies sometimes drop a crafting item, automatically pick up ammo and supplies from kills, and enemies drop double the ammo."},
        "Enemies drop nothing here: when you down or execute one, the ammo (and III: crafting item) goes straight into your pack."},
    [SK_AWARE] = {"AWARENESS", SK_ICON("awareness"), 2, {1, 3}, 3,
        {"Enemy nametags remain on screen for 5 seconds when you take damage.",
         "Enemy nametags appear at twice the normal distance and remain on screen for 10 seconds when you take damage."},
        "Enemy nametags only show for enemies you have line of sight to (as everywhere here)."},
    [SK_FORT] = {"FORTITUDE", SK_ICON("fortitude"), 2, {1, 3}, 3,
        {"Bleed out time is increased by 25%, you crawl a little faster while downed, and earn 25 Parts after healing more than half your health.",
         "Start with a health kit and carry up to 4. Bleed out time +50%, more downed health, much faster downed crawling, 50 Parts after healing more than half your health."},
        "In: bleed out time, downed health, downed crawl speed, Parts after a big heal and the starting health kit. Carrying 4 kits: the kit cap is 3, 4 with II (sk_kit_cap)."},
    [SK_AGILITY] = {"AGILITY", SK_ICON("agility"), 2, {2, 4}, 3,
        {"Crouch walk very quickly.",
         "You are nearly impossible to hear while moving. Walk, climb and crouch walk very quickly."},
        "Exact 2013 numbers: crouch x1.15 / x1.19, walk x1.09 (motion-match natives), climb x1.35 (winmm's perk-0x08 climb rate constant patched 1.4 -> 1.35), silent running for observers (footsteps.c via the synced effective level). No engine upgrade."},
    [SK_LONEWOLF] = {"LONE WOLF", SK_ICON("lone-wolf"), 2, {2, 3}, 3,
        {"Away from your partners (or with them downed): Agility II, Sharpshooter II, and 30 bonus Parts when you down or execute a foe.",
         "Away from your partners (or with them downed): Agility II, Sharpshooter II, Sharp Ears III, and 60 bonus Parts when you down or execute a foe."},
        NULL},
    [SK_SECOND] = {"SECOND CHANCE", SK_ICON("second-chance"), 2, {1, 2}, 3,
        {"Receive a health kit when you have none and are below 50% health. Get 25% more ammo after dying twice without a down or execution (stacks twice).",
         "Receive a health kit when you have none and are below 50% health. Get 50% more ammo and 25% cheaper armor after dying twice without a down or execution (stacks twice)."},
        "Armour costs 25% less per two deaths without a down / execution (II, stacks twice)."},
    [SK_JACK] = {"JACK OF ALL TRADES", SK_ICON("jack"), 2, {5, 10}, 3,
        {"Contains Brawler I, Covert Training I, Sharp Ears I, Strategist I and Crafter I (8 LP).",
         "Contains Explosion Expert I, Reviver I, Brawler I, Covert Training I, Sharp Ears I, Strategist I, Crafter I, First-Aid Training I, Scavenger I and Second Chance I (16 LP)."},
        "Each part works as far as that skill is in."},
    [SK_LUCKY] = {"LUCKY BREAK", SK_ICON("lucky"), 3, {2, 3, 4}, 7,
        {"Upon opening a supply box, receive 50% extra pistol ammo.",
         "Upon opening a supply box, receive 50% extra pistol ammo, 25% extra long gun ammo and at least 2 crafting ingredients.",
         "Upon opening a supply box, receive 50% extra pistol ammo, 25% extra long gun ammo and at least 3 crafting ingredients."},
        "2013 box logic: extra ammo from the box top-up; II/III more crafting items per box."},
    [SK_LETHAL] = {"LETHAL EFFICIENCY", SK_ICON("lethal"), 2, {2, 3}, 3,
        {"Do a faster, low-to-the-ground neck-snap that replaces special executions.",
         "Do an even faster, low-to-the-ground neck-snap that replaces special executions."},
        "2013 fast (I) / fastest (II) neck-break clips from net-melee (fist-execute-neckbreak-fast 1.633 s / -fastest 0.8 s) picked by level in the execution system; no engine upgrade."},
    // ---- melee skills (this mod's own, re/melee-skills-1102.md; modelled on the 2013 skills named in each note)
    [SK_RIPOSTE] = {"COUNTER FIGHTER", SK_ICON("counter"), 2, {2, 3}, 3,
        {"Your counter attack after a dodge can be started 40% longer.",
         "Your counter attack after a dodge can be started 40% longer, also after a late dodge, and it is ready again sooner."},
        "Modelled on Agility. I: the dodge counter's melee-press window 600 -> 850 ms (dcounter.c DC_WINDOW_MS). II: the counter also arms from a late (not perfect, > 200 ms before the contact) dodge of a melee weapon swing, and the counter cooldown is 2.2 -> 1.5 s. Decided on the dodger's own machine."},
    [SK_GRIP] = {"IRON GRIP", SK_ICON("grip"), 2, {2, 3}, 3,
        {"You have 50% more time to break out of a pin or a pull-out.",   // (the body shield grab line went with BODYSHIELD, skills.h)
         "You have 50% more time to break out of a pin or a pull-out. Once per life the first pin or pull-out on you is broken for you."},
        "Modelled on Fortitude. I: the pin escape press window (the last 330 ms of the blow's window -> 495 ms, as far as the blow's 480 ms allows) and the pull-out counter window 400 -> 600 ms. II: once per life the first pin or pull-out QTE on us is passed for us the moment its press window opens. Judged on the victim's machine, which owns the skill."},
    [SK_BLADE] = {"BLADE DISCIPLINE", SK_ICON("blade"), 2, {1, 3}, 3,
        {"Your hatchets and tomahawks last 1 hit longer.",
         "Your hatchets and tomahawks last 1 hit longer. You start with a hatchet when you spawn without a melee weapon."},
        "Modelled on Brawler. I: +1 durability on a picked up 1h blade weapon (the HUD dashes follow: weapon def max + 1). The machete never breaks. II: the spawn kit gives a hatchet with 3 uses (one blade kill: blades kill outright now) when you carry no melee weapon."},
    [SK_HEAVY] = {"HEAVY HANDS", SK_ICON("heavy"), 2, {2, 6}, 3,
        {"Enemies you down with fists or a blunt weapon bleed out 30% faster.",
         "Enemies you down with fists or a blunt weapon bleed out 30% faster. A blunt weapon finisher kills outright."},
        "Modelled on Lethal Efficiency. I: the victim's bleed-out x0.7 for a fist / blunt melee down by a player with the skill (read from the synced skills of the downer, MSG_SK_SYNC). II: a down that comes out of an SP finisher (mf- pair) with a blunt weapon is lethal like a blade down (melee_lethal_down_now)."},
    [SK_STEADY] = {"STEADY STANCE", SK_ICON("steady"), 2, {1, 2}, 3,
        {"You recover at once when your blow is parried: the parrier's head start is gone.",
         "You recover at once when your blow is parried. Contextual weapon takedowns do not wear your weapon."},
        "Modelled on Agility II / Marathon Runner. I: the 300 ms block on our melee button after the hand-off of a parry pair (melee_duel_parry.c MDP_INIT_MS) is waived; a clash has none. II: a contextual weapon takedown (wall / low cover / combo-hit pair) costs no durability point (ctxdown.c cd_wear_begin). Parry / clash / dodge counter blows never wear a weapon in this build."},
    // Boxer's Footwork skill: lowers the dodge cooldown by 10% at 2 LP, 15% at 4 LP, 20% at 6 LP
    [SK_FOOTWORK] = {"BOXER'S FOOTWORK", SK_ICON("footwork"), 3, {2, 4, 6}, 7,
        {"Your dodge is ready again 10% sooner.",
         "Your dodge is ready again 15% sooner.",
         "Your dodge is ready again 20% sooner."},
        "Modelled on Marathon Runner. The dodge cooldown (melee_dodge.c MD_COOLDOWN_MS, also the 2-charge refill and the cooldown bar) x0.9 / 0.85 / 0.8. Decided on the dodger's own machine."},
#if T2R_BODYSHIELD
#include "skills_bodyshield.h"
#endif
};
static const char *const SK_ROMAN[4] = {"", "I", "II", "III"};

// ---- values
static int sk_id(uint64_t v) { int id = (int)((v >> 40) & 0xff); return (v & SK_TAG) && id > 0 && id < SK_COUNT ? id : 0; }
static int sk_lv(uint64_t v) {
    int id = sk_id(v), lv = (int)((v >> 36) & 0xf);
    return id && lv >= 1 && lv <= SK[id].nlv ? lv : 0;
}
static uint64_t sk_legacy(uint64_t v) {          // winmm's old skill bits -> the closest 2013 skill
    switch (v) {
    case 0x1: return SK_V(SK_HAWK, 1);           // HAWK-EYED
    case 0x2: return SK_V(SK_SHARP, 2);          // HEAD HUNTER (+15 hp per headshot)
    case 0x4: return SK_V(SK_EARS, 3);           // SHARP EARS (2013 III text)
    case 0x8: return SK_V(SK_AGILITY, 1);        // AGILITY
    case 0x10: return SK_V(SK_MARATHON, 1);      // MARATHON RUNNER
    case 0x20: return SK_V(SK_SHARP, 1);         // STEADY HANDS
    case 0x40: return SK_V(SK_MARATHON, 1);      // ADRENALINE (not 2013)
    case 0x80: return SK_V(SK_FAT, 1);           // FIRST AID TRAINING
    case 0x200: return SK_V(SK_COLLECTOR, 1);    // COLLECTOR
    }
    return 0;
}
static uint64_t sk_norm(uint64_t v) { if (v && !(v & SK_TAG)) v = sk_legacy(v); return sk_lv(v) ? v : 0; }

// winmm-compatible skill records (FUN_18001d420 result: +0 value, +8 name, +0x10 description,
// +0x18 icon, +0x20 LP cost)
typedef struct { uint64_t v; const char *name, *desc, *icon; uint32_t cost, pad; uint64_t z[6]; } sk_rec_t;
static sk_rec_t g_sk_rec[SK_COUNT][3];
static char g_sk_recname[SK_COUNT][3][40];
static const uint8_t *sk_lookup(uint64_t v) {
    v = sk_norm(v);
    if (!v) return NULL;
    int id = sk_id(v), lv = sk_lv(v);
    sk_rec_t *r = &g_sk_rec[id][lv - 1];
    if (!r->v) {
        snprintf(g_sk_recname[id][lv - 1], sizeof g_sk_recname[0][0], "%s %s", SK[id].name, SK_ROMAN[lv]);
        r->name = g_sk_recname[id][lv - 1];
        r->desc = SK[id].desc[lv - 1];
        r->icon = SK[id].icon;
        r->cost = SK[id].cost[lv - 1];
        r->v = v;
    }
    return (const uint8_t *)r;
}
static int sk_cost(uint64_t v) { const uint8_t *r = sk_lookup(v); return r ? (int)((const sk_rec_t *)r)->cost : 0; }

// ---- the 4th skill (2013 had 4 skill rows; winmm's lobby object holds 3)
// the pre-made classes' 4th skill = CLASSES[].s4 (t2r-ext.c)
static uint64_t g_sk4[N_CUSTOM_MAX];           // by custom slot (lo_slot; t2r-skills.txt: one line per slot, an older ext reads the first six)
static uint64_t g_sk4_cur;                  // the lobby loadout's 4th skill
static int g_sk4_loaded;
static void sk4_path(char *p, size_t n) {
    snprintf(p, n, "%s", g_loadout_path);
    char *s = strrchr(p, '\\');
    if (s) snprintf(s + 1, n - (size_t)(s + 1 - p), "t2r-skills.txt");
}
static void sk4_save(void) {
    char p[MAX_PATH];
    if (!g_loadout_path[0]) return;
    sk4_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "T2RSK1\n");
    for (int i = 0; i < N_CUSTOM_MAX; i++) fprintf(f, "%llx\n", (unsigned long long)g_sk4[i]);
    fclose(f);
}
static void sk4_load(void) {
    if (g_sk4_loaded || !g_loadout_path[0]) return;
    g_sk4_loaded = 1;
    char p[MAX_PATH], tag[16] = {0};
    sk4_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (fscanf(f, "%15s", tag) == 1 && !strcmp(tag, "T2RSK1"))
        for (int i = 0; i < N_CUSTOM_MAX; i++) {
            unsigned long long v;
            if (fscanf(f, "%llx", &v) != 1) break;
            g_sk4[i] = sk_norm(v);
        }
    fclose(f);
}
static uint64_t *sk4_slot(const void *lo) {     // which loadout_t this is (array element / lobby copy)
    static uint64_t cls[sizeof CLASSES / sizeof *CLASSES];
    sk4_load();
    const char *p = (const char *)lo;
    if (p >= (const char *)&g_lo[0] && p < (const char *)&g_lo[N_CUSTOM_MAX])
        return &g_sk4[(p - (const char *)&g_lo[0]) / (ptrdiff_t)sizeof g_lo[0]];
    const char *c0 = (const char *)&g_lo_res[0];    // a resolved class (lo_of): its own 4th skill, not the lobby's
    if (p >= c0 && p < c0 + N_CLASSES * (ptrdiff_t)sizeof g_lo_res[0] && !((p - c0) % (ptrdiff_t)sizeof g_lo_res[0])) {
        int k = (int)((p - c0) / (ptrdiff_t)sizeof g_lo_res[0]);
        cls[k] = CLASSES[k].s4;
        return &cls[k];
    }
    return &g_sk4_cur;                          // a copy of the lobby object (lo_read_obj)
}
static int sk4_cost(const void *lo) { return sk_cost(*sk4_slot(lo)); }
// Over-budget migration of saved custom `idx` (lo_load; bow 2 -> 3 LP): the guns stay. While over 13: drop the cheapest skill that covers the
// overshoot (3rd, 2nd, 1st, then the 4th row on ties = the last one a player set), else the dearest skill; with no skill left the purchasable goes.
static void sk4_fit_budget(loadout_t *l, int idx) {
    sk4_load();
    uint64_t *s4 = &g_sk4[idx];
    int start = lo_cost(l) - sk4_cost(l) + sk_cost(*s4), cost = start;
    if (cost <= 13) return;
    int what[8], n = 0;
    while (cost > 13 && n < 8) {
        int over = cost - 13, best = -1, bc = 99, dear = -1, dc = 0;
        for (int j = 0; j < 4; j++) {
            uint64_t v = j == 3 ? *s4 : l->k[2 - j];
            int c = v ? sk_cost(v) : 0;
            if (c <= 0) continue;
            if (c >= over && c < bc) { best = j; bc = c; }
            if (c > dc) { dear = j; dc = c; }
        }
        int j = best >= 0 ? best : dear;
        if (j < 0) {
            if (l->w[2] >= ((const uint32_t *)VMA(0x1800A0DA4))[2]) break;
            cost -= *(const int *)(lo_purch_tab() + 0x18 + l->w[2] * 0x40);
            l->w[2] = 0xffffffffu; what[n++] = -1;
            continue;
        }
        cost -= j == 3 ? sk_cost(*s4) : sk_cost(l->k[2 - j]);
        if (j == 3) *s4 = 0; else l->k[2 - j] = 0;
        what[n++] = j;
    }
    char b[64] = "";
    for (int i = 0; i < n && i < 6; i++) snprintf(b + strlen(b), sizeof b - strlen(b), " %s", what[i] < 0 ? "purchasable" : what[i] == 3 ? "4th-skill" : "skill");
    xlog("loadout: custom %d was %d LP with the 3 LP bow / machete (max 13): dropped%s -> %d LP", idx + 1, start, b, cost);
    sk4_save();
}
static void sk4_on_write(const void *lo) { g_sk4_cur = sk_norm(*sk4_slot(lo)); }
static const char *sk4_name(const void *lo) {
    const uint8_t *r = sk_lookup(*sk4_slot(lo));
    return r ? ((const sk_rec_t *)r)->name : NULL;
}

// ---- UI round 2 (gear up B2): the slot column of the picker pages. tools/features/loadout_r2_widgets.py adds the static group
// anim-group.loMirror (rows loSl<i>Bg / Ic / Lb / Nm / Co / Fr) to picker-primary / -secondary / -purchase / -skill; this fills it with a
// loadout (the one being edited, or the focused one on the loadout list) and frames the slot being picked. The page without that group (old
// look, T2R_LOADOUT_R2=0) has no widgets, ui_w is 0 and nothing happens.
static int r2_on(void) { return ui_w("anim-group.loMirror") != 0; }
static void r2_mirror_fill(const void *lo, int active) {
    if (!r2_on()) return;
    const loadout_t *l = (const loadout_t *)lo;
    static const char *const MLBL[7] = {"PRIMARY", "SECONDARY", "SKILL 1", "SKILL 2", "SKILL 3", "SKILL 4", "PURCHASABLE"};
    const uint32_t *cnt = (const uint32_t *)VMA(0x1800A0DA4);
    char p[72], b[24];
    for (int i = 0; i < 7; i++) {
        const char *nm = NULL, *ic = NULL;
        int cost = -1;
        if (i == 0 || i == 1 || i == 6) {
            int slot = i == 6 ? 2 : i;
            nm = lo_weapon_name(slot, l->w[slot], &ic);
            if (nm && l->w[slot] < cnt[slot]) {
                if (slot == 0) cost = g_large_tab ? *(const int *)(g_large_tab + 0x18 + l->w[0] * 0x38) : 2;
                else if (slot == 1) cost = *(const int *)(small_tab() + 0x18 + l->w[1] * 0x38);
                else cost = *(const int *)(lo_purch_tab() + 0x18 + l->w[2] * 0x40);
            } else nm = NULL;
        } else {
            const uint8_t *s = i < 5 ? lo_skill(l->k[i - 2]) : sk_lookup(*sk4_slot(l));
            if (s) { nm = *(const char **)(s + 8); ic = *(const char **)(s + 0x18); cost = *(const int *)(s + 0x20); }
        }
        snprintf(p, sizeof p, "anim-group.loMirror.loSl%dLb", i); ui_text(p, MLBL[i]);
        snprintf(p, sizeof p, "anim-group.loMirror.loSl%dNm", i); ui_text(p, nm ? nm : (i == 0 ? "NO PRIMARY" : i == 1 ? "NO SIDEARM" : i == 6 ? "NO PURCHASABLE" : "NO SKILL"));
        snprintf(p, sizeof p, "anim-group.loMirror.loSl%dIc", i); ui_image(p, nm && ic ? ic : "streaming/t2r/icons/blocked.png");
        if (cost > 0) snprintf(b, sizeof b, "%d LP", cost); else snprintf(b, sizeof b, nm ? "FREE" : "");
        snprintf(p, sizeof p, "anim-group.loMirror.loSl%dCo", i); ui_text(p, b);
        snprintf(p, sizeof p, "anim-group.loMirror.loSl%dFr", i); w_alpha(ui_w(p), i == active ? 1.0f : 0.0f);
        float na = lo_r3() ? (active < 0 ? 0.75f : i == active ? 1.0f : 0.45f) : active < 0 || i == active ? 1.0f : 0.55f;
        snprintf(p, sizeof p, "anim-group.loMirror.loSl%dNm", i); w_alpha(ui_w(p), na);
        snprintf(p, sizeof p, "anim-group.loMirror.loSl%dIc", i); w_alpha(ui_w(p), na);
    }
}
// UI round 2: the edit page's DETAILS panel (loadout_r2_widgets._edit_details, hidden until here): the focused slot row's item - gun / skill / purchasable -
// icon, name, LP, description, and the stat bars for the two guns. Re-filled when the focused row changes (and every 90 frames: widgets may appear late).
static int sk_focused(const char *path);
static void r3_preview(int f, const loadout_t *l);   // below (needs sk_in_slot)
static void r3_edit_detail(int f, const loadout_t *l);
static void r2_edit_detail(void) {
    static const char *const ROW[7] = {"PrimaryWeapon", "SecondaryWeapon", "SkillOne", "SkillTwo", "SkillThree", "SkillFour", "Purchasable1"};
    static int last = -2;
    static uint64_t last_w, last_f;
    if (!ui_w("anim-group.detailPanel.perkName")) return;           // the old look: the edit page has no panel
    char p[96];
    int f = -1;
    for (int i = 0; i < 7; i++) { snprintf(p, sizeof p, "anim-group.slotList.%s", ROW[i]); if (sk_focused(p)) { f = i; break; } }
    uint64_t pw = ui_w("anim-group");
    if (f < 0) return;
    if (f == last && pw == last_w && g_frame - last_f < 90) return;
    if (f != last && pw == last_w && lo_r3()) { r3_move_in("anim-group.t3Prev"); r3_move_in("anim-group.detailPanel"); }   // launcher: another slot row = columns B + D m-in
    last = f; last_w = pw; last_f = g_frame;
    ui_visible("anim-group.detailPanel", 1);
    loadout_t l;
    memset(&l, 0xff, sizeof l.w);
    memset(l.k, 0, sizeof l.k);
    lo_read_obj(&l);
    if (lo_r3()) { r3_edit_detail(f, &l); r3_preview(f, &l); return; }
    const uint32_t *cnt = (const uint32_t *)VMA(0x1800A0DA4);
    const char *nm = NULL, *ic = NULL, *ds = "";
    int lp = 0, gun = 0, v[4] = {0, 0, 0, 0};
    static char sd[512];
    if (f <= 1) {
        uint32_t idx = l.w[f];
        if (idx < cnt[f]) {
            const uint8_t *row = weapon_row(f, idx);
            nm = *(const char **)row; ic = *(const char **)(row + 8); lp = (int)*(const uint32_t *)(row + 0x18);
            ds = *(const char **)(row + 0x28) ? *(const char **)(row + 0x28) : "";
            if (nm && !strncmp(nm, "SUPPRESSED ", 11)) { snprintf(sd, sizeof sd, "%s%sHides shots from appearing on enemy radar.", ds, ds[0] ? "\n" : ""); ds = sd; }
            stat_values(*(const char **)(row + 0x10), v);
            gun = 1;
        }
    } else if (f == 6) {
        uint32_t idx = l.w[2];
        if (idx < cnt[2]) {
            const uint8_t *row = lo_purch_tab() + idx * 0x40;
            nm = *(const char **)row; ic = *(const char **)(row + 8); lp = (int)*(const uint32_t *)(row + 0x18);
            const char *d0 = *(const char **)(row + 0x30);
            ds = d0 && !IsBadReadPtr(d0, 1) ? d0 : "";
        }
    } else {
        const uint8_t *s = f < 5 ? lo_skill(l.k[f - 2]) : sk_lookup(*sk4_slot(&l));
        if (s) { nm = *(const char **)(s + 8); ds = *(const char **)(s + 0x10); ic = *(const char **)(s + 0x18); lp = *(const int *)(s + 0x20); if (!ds) ds = ""; }
    }
    char b[48];
    detail_title(nm ? nm : (f == 0 ? "NO PRIMARY" : f == 1 ? "NO SIDEARM" : f == 6 ? "NO PURCHASABLE" : "NO SKILL"));
    ui_image("anim-group.detailPanel.perkIcon", nm && ic ? ic : "streaming/t2r/icons/blocked.png");
    lo_cost_str(b, sizeof b, lp > 0 ? (unsigned)lp : 0);
    ui_text("anim-group.detailPanel.perkCost", b);
    if (gun) { ui_text("anim-group.detailPanel.perkDesc", ds); detail_stats_shift(loc_tr(ds)); }
    else detail_desc(ds, 170.0f, 1100.0f);
    for (int s = 0; s < 4; s++) {
        snprintf(p, sizeof p, "anim-group.detailPanel.statLabel%d", s); ui_visible(p, gun);
        for (int k = 0; k < 10; k++) {
            snprintf(p, sizeof p, "anim-group.detailPanel.statE%d_%d", s, k); ui_visible(p, gun);
            snprintf(p, sizeof p, "anim-group.detailPanel.statF%d_%d", s, k); ui_visible(p, gun && k < v[s]);
        }
    }
}
static void r2_mirror_cur(int active) {
    loadout_t l;
    memset(&l, 0xff, sizeof l.w);
    memset(l.k, 0, sizeof l.k);
    lo_read_obj(&l);
    r2_mirror_fill(&l, active);
}

// ---- the equipped skills
static uint64_t sk_slot_get(int s) {
    uint8_t *o = lobby_obj();
    if (s == 3) return g_sk4_cur;
    return o && s >= 0 && s < 3 ? *(uint64_t *)(o + 0x2c78 + 8 * s) : 0;
}
static void sk_slot_set(int s, uint64_t v) {
    uint8_t *o = lobby_obj();
    if (s == 3) {
        g_sk4_cur = v;
        if (g_lo_state != LO_NONE) lj_user_edit();      // loadjson.c: a launcher file not applied yet is older than this edit
        if (lo_slot(g_lo_active) >= 0) { g_sk4[lo_slot(g_lo_active)] = v; sk4_save(); }
        return;
    }
    if (o && s >= 0 && s < 3) *(uint64_t *)(o + 0x2c78 + 8 * s) = v;   // feature_loadout_persist saves it
}
static uint8_t g_sk_eff[SK_COUNT];          // effective level per skill (equipped, Jack of All Trades, Lone Wolf)
static int g_sk_alone;                      // Lone Wolf: no teammate around
static uint8_t g_sk_upg_eff[SK_COUNT];      // levels for the ENGINE upgrades: Lone Wolf counted as alone (see sk_upg_wanted)
static void sk_expand(uint8_t *eff, int id, int lv, int alone) {
    if (lv > eff[id]) eff[id] = (uint8_t)lv;
    if (id == SK_JACK) {
        static const uint8_t J1[] = {SK_BRAWLER, SK_COVERT, SK_EARS, SK_STRAT, SK_CRAFTER};
        static const uint8_t J2[] = {SK_EXPLO, SK_REVIVER, SK_FAT, SK_SCAV, SK_SECOND};
        for (size_t i = 0; i < sizeof J1; i++) if (!eff[J1[i]]) eff[J1[i]] = 1;
        if (lv >= 2) for (size_t i = 0; i < sizeof J2; i++) if (!eff[J2[i]]) eff[J2[i]] = 1;
    }
    if (id == SK_LONEWOLF && alone) {
        if (eff[SK_AGILITY] < 2) eff[SK_AGILITY] = 2;
        if (eff[SK_SHARP] < 2) eff[SK_SHARP] = 2;
        if (lv >= 2 && eff[SK_EARS] < 3) eff[SK_EARS] = 3;
    }
}
static void sk_compute(void) {
    uint8_t eff[SK_COUNT] = {0};
    for (int s = 0; s < 4; s++) {
        uint64_t v = sk_norm(sk_slot_get(s));
        if (v && (SK[sk_id(v)].avail >> (sk_lv(v) - 1) & 1)) sk_expand(eff, sk_id(v), sk_lv(v), g_sk_alone);
    }
    uint8_t upg[SK_COUNT] = {0};
    for (int s = 0; s < 4; s++) {
        uint64_t v = sk_norm(sk_slot_get(s));
        if (v && (SK[sk_id(v)].avail >> (sk_lv(v) - 1) & 1)) sk_expand(upg, sk_id(v), sk_lv(v), 1);
    }
#ifndef T2R_RELEASE
    {   // dev (A/B tests): t2r-skill-dev.txt next to the exe = "<skill id> <level>" pairs (SK_* enum numbers) forced on top, re-read every 2 s
        static uint8_t force[SK_COUNT]; static uint64_t poll;
        if (!poll || g_frame - poll >= 120) {
            poll = g_frame ? g_frame : 1; memset(force, 0, sizeof force);
            FILE *f = exe_side_open("t2r-skill-dev.txt", "r");
            int id, lv;
            while (f && fscanf(f, "%d %d", &id, &lv) == 2) if (id > 0 && id < SK_COUNT && lv >= 0 && lv <= 3) force[id] = (uint8_t)lv;
            if (f) fclose(f);
        }
        for (int i = 1; i < SK_COUNT; i++) if (force[i]) eff[i] = force[i];
    }
#endif
    memcpy(g_sk_eff, eff, sizeof eff);
    for (int i = 0; i < SK_COUNT; i++) g_sk_upg_eff[i] = upg[i] > eff[i] ? upg[i] : eff[i];
}
static int skl(int id) { return g_sk_eff[id]; }
// peers' skills (MSG_SK_SYNC)
static uint8_t g_peer_skid[T2R_MAX_PEERS][4], g_peer_sklv[T2R_MAX_PEERS][4];
static uint64_t g_peer_sk_frame[T2R_MAX_PEERS];
static uint8_t g_peer_agi[T2R_MAX_PEERS];       // the peer's EFFECTIVE Agility level (MSG_SK_SYNC byte 8: includes Lone Wolf's alone-grant)
static int sk_peer(uint32_t slot, int id) {
    if (slot > T2R_MAX_PEERS - 1 || !g_peer_sk_frame[slot]) return 0;
    uint8_t eff[SK_COUNT] = {0};
    for (int i = 0; i < 4; i++) {
        int pid = g_peer_skid[slot][i], plv = g_peer_sklv[slot][i];
        if (pid > 0 && pid < SK_COUNT && plv >= 1 && plv <= SK[pid].nlv) sk_expand(eff, pid, plv, 0);
    }
    if (id == SK_AGILITY && g_peer_agi[slot] > eff[id]) return g_peer_agi[slot];
    return eff[id];
}

// ---- values used by the ext's existing code (skills.h)
// 2013 data (rules/skills.md, booster 767250b3): listen deplete x0.85 / 0.80 / 0.70, regen x1.20 / 1.30 / 1.35 (the wiki's rounder
// 15 / 30 / 35 % text differs slightly; "use the data")
static float sk_listen_deplete_mul(void) { static const float D[4] = {1.0f, 0.85f, 0.80f, 0.70f}; return D[skl(SK_EARS) & 3]; }
static float sk_listen_recharge_mul(void) { static const float R[4] = {1.0f, 1.20f, 1.30f, 1.35f}; return R[skl(SK_EARS) & 3]; }
static float sk_revive_time_mul(void) { static const float T[4] = {1.0f, 0.70f, 0.55f, 0.45f}; return T[skl(SK_REVIVER) & 3]; }
static float sk_molotov_mul(void) { static const float M[3] = {1.0f, 1.10f, 1.20f}; return M[skl(SK_EXPLO) % 3]; }

// ---- small helpers
typedef uint8_t *(*sk_fn_peer)(uint32_t);
static uint8_t *sk_peer_ptr(uint32_t slot) { return slot < T2R_MAX_PEERS ? ((sk_fn_peer)t2r_peer)(slot) : NULL; }
static uint32_t sk_me(void) { return *(uint32_t *)VMA(0x180138090); }
static const float *sk_peer_pos(uint32_t slot, float *buf) {
    typedef void *(*fn_proc)(uint64_t);
    typedef const float *(*fn_loc)(void *, float *);
    uint8_t *peer = sk_peer_ptr(slot);
    uint64_t h = peer ? *(uint64_t *)(peer + 0x60) : 0;
    uint8_t *proc = h ? ((fn_proc)VMA(0x18000A200))(h) : NULL;
    return proc ? ((fn_loc)((uint8_t *)GetModuleHandleW(NULL) + 0x133db30))(proc + 0x100, buf) : NULL;
}
static int sk_hp(void) { typedef int (*fn)(void); return ((fn)VMA(0x18000B890))(); }
static int sk_hp_max(void) { typedef int (*fn)(void); return ((fn)VMA(0x18000B8F0))(); }
static void sk_hp_set(int hp) { typedef void (*fn)(int); ((fn)VMA(0x18000B900))(hp); }
static void sk_heal(int amount, const char *why) {
    int hp = sk_hp(), mx = sk_hp_max();
    if (g_downed || hp <= 0 || hp >= mx) return;
    int n = hp + amount > mx ? mx : hp + amount;
    sk_hp_set(n);
    static int nl;
    if (LOGCAP(nl, 40)) xlog("skills: %s +%d hp (%d -> %d)", why, amount, hp, n);
}
static void sk_sound(int ok) { typedef void (*fn)(uint64_t); ((fn)VMA(0x18000B560))(ok ? 0x70b52a347247b71ULL : 0x352db84afb541a92ULL); }
// mark an enemy slot here and for our teammates (winmm msg 9 = [slot], what its own marking sends)
static uint8_t g_mk_hawk[T2R_MAX_PEERS], g_mk_by[T2R_MAX_PEERS];       // per marked enemy: Hawk-eyed level of whoever MARKED it (2013: the perk is the marker's own); g_mk_by = teammate slot + 1 of the last mark message
static uint64_t g_mk_by_frame[T2R_MAX_PEERS];
static int sk_mark_hawk_lv(uint32_t slot) { return slot < T2R_MAX_PEERS ? g_mk_hawk[slot] : 0; }   // t2r-ext.c feature_mark_style: the outline needs the marker's Hawk-eyed II
static uint64_t g_sk_selfmark[T2R_MAX_PEERS], g_sk_grp[T2R_MAX_PEERS];   // g_sk_grp: marks made by the Hawk-eyed III group chain
static void sk_mark(uint32_t slot) {
    uint8_t *peer = sk_peer_ptr(slot);
    if (!peer || !peer[4] || peer[5] || is_teammate(slot)) return;
    if (sk_peer(slot, SK_COVERT) >= 3) return;              // Covert Training III: can't be marked
    *(float *)(peer + 0xa00) = 0.0f;                        // (re)start the mark
    if (peer[0x9fc]) return;
    peer[0x9fc] = 1;
    g_sk_selfmark[slot] = g_frame;
    uint8_t b = (uint8_t)slot;
    for (uint32_t t = 0; t < T2R_MAX_PEERS && g_send_tramp; t++) {
        uint8_t *tp = sk_peer_ptr(t);
        if (t != sk_me() && tp && tp[4] && is_teammate(t)) g_send_tramp(9, &b, 1, t);
    }
}

// ---- engine upgrades (the game's single-player skill tree), granted for the match
// One row per engine upgrade the mod still switches on: name, the skill that wants it and from which level. (crafting-speed-increase is also
// held while Brawler crafts a modded weapon, see sk_craft_start.) Every other effect is done by the ext itself with the exact 2013 numbers.
static const struct { const char *name; uint8_t skill, min_lv; } SK_UPG_T[] = {
    {"crafting-speed-increase", SK_CRAFTER, 1}, {"health-kit-speed-increase", SK_FAT, 1},
    {"listen-mode-speed-increase", SK_EARS, 3}, {"listen-mode-speed-increase-2", SK_EARS, 3},
    {"aim-wobble-reduce", SK_SHARP, 0}, {"aim-sway-reduce-while-moving", SK_SHARP, 1},
    {"melee-upgrade-durability", SK_BRAWLER, 2}};
#define SK_NUPG (int)(sizeof SK_UPG_T / sizeof *SK_UPG_T)
#define SK_UPG(i) SK_UPG_T[i].name
static int g_sk_brawler_craft;                 // Brawler: a modded weapon (melee-upgrade) is being crafted right now (sk_craft_start)
static int sk_upg_wanted(int i) {
    // Bad stutters during a melee sequence: each give / remove of an engine upgrade stalls the frame 15-95 ms, and Lone Wolf flipped
    // Sharpshooter II's every time a teammate came near or left; the upgrades now hold the Lone Wolf (alone) levels for the whole life, the cheap
    // aim values (sk_aim_values) still follow Lone Wolf live
    return g_sk_upg_eff[SK_UPG_T[i].skill] >= SK_UPG_T[i].min_lv || (i == 0 && g_sk_brawler_craft);
}
static uint8_t g_sk_granted[SK_NUPG];        // granted by us (taken back after the match)
// The upgrade natives are TYPED bindings: rec+8 is a descriptor, rec+0x10 the C++ impl (dcGivePlayerUpgradeById(StringId64) in
// player-script-funcs.cpp: tlou-ii.exe+0xcb3060 fetches the player itself and posts the give to the player's upgrade controller;
// +0xcb42f0 is the remove; the id comes in rcx, there is no player argument). The old route called rec+8 through dc_call, which is
// not code for these, so sk_native_ok said "no" at every match start (log: "native give-player-upgrade-by-id rec ... +8 -
// +0x10 code") and NO upgrade was ever granted: Crafter / Sharp Ears III / Sharpshooter wobble / Explosion Expert radius /
// Executioner shiv uses / Agility II quiet / Brawler durability / Lethal Efficiency / First-Aid speed did nothing at all.
// Now: rec+8 code -> the DcCallFunc route, else rec+0x10 code -> the impl with the id in rcx (dc_direct).
static int sk_upg_route(const char *name) {   // 0 = unusable, 1 = DcCallFunc (rec+8), 2 = typed impl (rec+0x10)
    static uint64_t sids[6]; static int route[6], n;
    uint64_t sid = fnv64(name);
    for (int i = 0; i < n; i++) if (sids[i] == sid) return route[i];
    uint8_t *rec = (uint8_t *)((fn_lookup)VMA(0x18000A190))(sid);
    int r = !rec ? 0 : is_code(*(void **)(rec + 8)) ? 1 : is_code(*(void **)(rec + 0x10)) ? 2 : 0;
    xlog("skills: native %s rec %p -> %s", name, (void *)rec,
         r == 1 ? "DcCallFunc (rec+8)" : r == 2 ? "typed impl (rec+0x10, id in rcx)" : "NOT CALLABLE");
    if (n < 6) { sids[n] = sid; route[n] = r; n++; }
    return r;
}
static int sk_upg_do(const char *name, uint64_t id) {
    int r = sk_upg_route(name);
    if (r == 1) dc_call(fnv64(name), 1, &id);
    else if (r == 2) dc_direct(fnv64(name), id, 0, 0);
    return r != 0;
}
// First-Aid Training I-III: health kits are used 1.75x faster (2013 booster 112c9e7a health-kit-speed = 1.75, all levels). The engine's kit-use state
// (PC exe FUN_140bb6e70, and 140bb7900 for the anim set) sets the bandage animation RATE of the use state = (HasUpgrade(health-kit-speed-increase) ?
// *(float *)0x144104340 : 1.0); that global is written by nobody in the exe (bss) - winmm's perk 0x80 filled it with 2.5 (0x40200000, FUN_18001c1e0), a
// 2.5x kit. The ext now writes the exact 1.75 itself (and the perk bit 0x80 is no longer set, so winmm never writes 2.5); the upgrade flag stays on
// as the switch that makes the engine read the global.
static void sk_kit_speed(void) {
    float *g = (float *)((uint8_t *)GetModuleHandleW(NULL) + 0x4104340);
    float want = skl(SK_FAT) ? 1.75f : 1.0f;
    if (*g == want) return;
    DWORD old; VirtualProtect(g, 4, PAGE_READWRITE, &old);
    xlog("skills: First-Aid Training %d: kit use rate %g -> %g (2013 health-kit-speed 1.75)", skl(SK_FAT), (double)*g, (double)want);
    *g = want;
    VirtualProtect(g, 4, old, &old);
}
// Sharpshooter I-III (2013 booster 67b9c974): scope sway x0.75 / 0.625 / 0.5, damage wobble x0.8 / 0.65 / 0.45 (rules/skills.md [data]).
// Engine numbers (PC exe + game DC data, re/gamedec/player-upgrade-settings.bin.asm):
//  * damage wobble: FUN_140aea660 scales the hit-reaction aim wobble by (found ? upgrade record(aim-wobble-reduce)+0x14 : 1.0): the record's float is
//    the multiplier itself (stock 0.50). Record lookup = FUN_140d2d2e0(sid) (exe+0xd2d2e0).
//  * sway: the upgrade's +0x20 sid names the DC object *upgraded-aim-sway* (type aim-sway-reduction, weapon-sway-settings.bin: float 0.65, float 0.80),
//    looked up by FUN_141357750(sid) (exe+0x1357750); both floats are the sway multipliers.
// Both objects live in the loaded DC data: the exact 2013 factor is written into them while the skill is equipped (the upgrades stay granted as the
// switch that makes the engine read them) and the stock numbers are put back outside a match. A value that is not the stock one (another writer, a
// different game build) is never overwritten.
static float *sk_aim_rec_wobble(void) {
    typedef uint8_t *(*fn)(uint64_t);
    uint8_t *r = ((fn)((uint8_t *)GetModuleHandleW(NULL) + 0xd2d2e0))(fnv64("aim-wobble-reduce"));
    return r && !IsBadReadPtr(r + 0x14, 4) ? (float *)(r + 0x14) : NULL;
}
static float *sk_aim_sway_obj(void) {
    typedef uint8_t *(*fn)(uint64_t);
    uint8_t *o = ((fn)((uint8_t *)GetModuleHandleW(NULL) + 0x1357750))(fnv64("*upgraded-aim-sway*"));
    return o && !IsBadReadPtr(o, 8) ? (float *)o : NULL;
}
static int sk_aim_put(float *p, float stock, float v, const char *what, float *mine) {
    if (!p) return 0;
    if (*p == v) { *mine = v; return 1; }
    if (*p != stock && *p != *mine) {                        // not ours and not stock: leave it
        static int nl; if (LOGCAP(nl, 6)) xlog("skills: %s is %g (stock %g) - not touched", what, (double)*p, (double)stock);
        return 0;
    }
    DWORD old; VirtualProtect(p, 4, PAGE_READWRITE, &old);
    *p = v; *mine = v;
    VirtualProtect(p, 4, old, &old);
    return 1;
}
// The aim punch was too strong: the crosshair could not be held on targets even with Sharpshooter III. The engine multiplies the
// hit-reaction aim wobble by the aim-wobble-reduce record float ONLY while the upgrade is granted; with no Sharpshooter the engine factor is 1.0 (full
// TLOU2 wobble), Sharpshooter I-III (0.8 / 0.65 / 0.45) took 20 / 35 / 55 % off that. 2013's flinch was small and its base is not TLOU2's: the upgrade is
// now granted to EVERYONE in a match and the record carries SK_WOB_BASE x the exact 2013 skill factor (none 0.35, I 0.28, II 0.2275, III 0.1575).
// (The engine already refreshes one wobble spring per id - FUN_141f23310 finds the live entry by sid - so hits do not stack; one wobble at a time.)
#define SK_WOB_BASE 0.675f   // midpoint of the old 1.0 and 0.35
float g_sk_wobble_now = 1.0f;                       // the multiplier the engine applies now (feel.c logs it per hit)
static void sk_aim_values(int in_match) {
    static const float WF[4] = {1.0f, 0.8f, 0.65f, 0.45f}, SWAY[4] = {0.65f, 0.75f, 0.625f, 0.5f};   // [0] = no skill
    static int touched; static float mine[3] = {-1, -1, -1};
    int lv = in_match ? skl(SK_SHARP) & 3 : 0;
    if (!in_match && !touched) return;
    float wob = in_match ? SK_WOB_BASE * WF[lv] : 0.5f;          // out of a match: the stock record float
    float *w = sk_aim_rec_wobble(), *s = sk_aim_sway_obj();
    int ok = sk_aim_put(w, 0.5f, wob, "aim-wobble-reduce", &mine[0]);
    ok &= sk_aim_put(s, 0.65f, lv ? SWAY[lv] : 0.65f, "upgraded-aim-sway[0]", &mine[1]);
    if (s) ok &= sk_aim_put(s + 1, 0.80f, lv ? SWAY[lv] : 0.80f, "upgraded-aim-sway[1]", &mine[2]);
    if (w && in_match) g_sk_wobble_now = *w;
    static int last = -1;
    if (last != lv + (in_match ? 4 : 0)) { last = lv + (in_match ? 4 : 0); xlog("skills: Sharpshooter %d: damage wobble x%g (engine factor, no skill = x%g), sway x%g/%g (%s)", lv, w ? (double)*w : -1.0, (double)SK_WOB_BASE, s ? (double)s[0] : -1.0, s ? (double)s[1] : -1.0, ok ? "ok" : "not all written"); }
    touched = in_match;
}
static void sk_upgrades(int in_match) {
    sk_kit_speed();
    sk_aim_values(in_match);
    if (!sk_upg_route("give-player-upgrade-by-id") || !sk_upg_route("remove-player-upgrade-by-id")) return;
    // Crafter only worked on the first life: a respawn builds a new player whose upgrade controller is empty, and g_sk_granted still
    // said "given" - every engine-upgrade skill (Crafter speed, First-Aid kit speed, Sharp Ears III, Sharpshooter, Brawler durability) worked only
    // in the first life. A new life (g_spawned_frame) grants them again.
    static uint64_t life;
    if (in_match && g_spawned_frame && g_spawned_frame != life && g_frame - g_spawned_frame >= 60 && local_hp() > 0.0f) {   // 1 s after the spawn kit (the new player is up)
        if (life) for (int i = 0; i < SK_NUPG; i++) if (g_sk_granted[i]) { g_sk_granted[i] = 0; xlog("skills: new life - engine upgrade %s granted again", SK_UPG(i)); }
        life = g_spawned_frame;
    }
    if (!in_match) life = 0;
    for (int i = 0; i < SK_NUPG; i++) {
        uint64_t id = fnv64(SK_UPG(i));
        int want = in_match && sk_upg_wanted(i);
        if (want && !g_sk_granted[i]) {
            sk_upg_do("give-player-upgrade-by-id", id);
            g_sk_granted[i] = 1;
            xlog("skills: %s %d: engine upgrade %s granted", SK[SK_UPG_T[i].skill].name, skl(SK_UPG_T[i].skill), SK_UPG(i));
        } else if (!want && g_sk_granted[i]) {
            sk_upg_do("remove-player-upgrade-by-id", id);
            g_sk_granted[i] = 0;
            xlog("skills: %s: engine upgrade %s taken back", SK[SK_UPG_T[i].skill].name, SK_UPG(i));
        }
    }
}
// Brawler I / II (2013 booster f308c660, c9b59a9f 0.5): modded weapons (the melee upgrade) take half the time to craft. The crafting
// menu starts a craft with player-start-crafting-behavior(item) and ends it with player-stop-crafting-behavior (tlou-ii.exe+0xcb3ce0
// / +0xcb3d40, item id in rcx). While a melee-upgrade craft runs Brawler holds the engine's crafting-speed upgrade (the one Crafter
// uses) - for that item only, so health kits / bombs craft at the normal pace.
static uint64_t (*g_sk_craft_start_orig)(uint64_t, uint64_t, uint64_t);
static uint64_t (*g_sk_craft_stop_orig)(uint64_t, uint64_t, uint64_t);
// 2013 Craft +30 (scoring.md, stat 30b0d190): paid when the crafted item shows up in the pack (not at the start, not on a cancel). The start
// arg is the item id; its quantity before the start is compared after the stop (the item may land a moment after the stop call).
static uint64_t g_sk_cr_item, g_sk_cr_qty0, g_sk_cr_until;
static int g_sk_cr_done;      // player-craft-item ran since the start = the craft completed (a modded melee weapon / a shiv at its cap adds no pack quantity but is a craft: Craft +30 and the Crafter count)
// Crafter II / III (2013 booster c8bef697 8dcd3a70 3 / 2): every 3rd / 2nd item crafted gives a gift box (gift.c): a small box with a random craftable
// item (never a shiv / modded melee), in its own HUD slot; selected, it is handed to a teammate with E (priority over the First-Aid heal). With a gift
// already carried the count waits (no second box). Progress is lost in a class without Crafter II / III (gift_tick). t2r-no-crafter-gift.txt: off.
static int g_sk_craft_n;
static int gift_grant(int lv);
static int gift_recv(uint32_t slot, uint32_t type, const uint8_t *buf, uint64_t len);
static void gift_tick(void);
static void dpadlog_tick(void);      // dpadlog.c: the weapon cross / wheel logger
static void gift_on_spawn(void);
static int g_gift_block_heal;
static uint64_t g_gift_anim_until;
static void sk_crafter_gift(void) {
    int lv = skl(SK_CRAFTER);
    if (lv < 2) return;
    FILE *f = exe_side_open("t2r-no-crafter-gift.txt", "r");
    if (f) { fclose(f); return; }
    int need = lv >= 3 ? 2 : 3;
    if (++g_sk_craft_n < need) return;
    if (!gift_grant(lv)) { g_sk_craft_n = need; return; }      // a gift is already carried: the count waits
    g_sk_craft_n = 0;
}
// Executioner II / III (2013 text: "get 1 / 2 extra shiv uses every time you craft a shiv"; a shiv's uses are its count in MP): when a crafted shiv lands
// in the pack the extra uses are added by code - exactly +1 / +2 per craft. (The SP shiv-craft-double / -triple upgrades only changed the recipe
// output count and were not 2013's numbers.)
static void sk_exec_shiv_uses(uint64_t item) {
    int lv = skl(SK_EXEC), extra = lv >= 3 ? 2 : lv >= 2 ? 1 : 0;
    uint64_t shiv = fnv64("shiv");
    if (!extra || (item != shiv && item != fnv64("recipe-shiv"))) return;
    uint64_t before = inv_qty(shiv);
    dc_direct(SID_ADD_QTY, shiv, (uint64_t)extra, 0);
    xlog("skills: Executioner %d: crafted a shiv, +%d shiv use(s) (%llu -> %llu)", lv, extra, (unsigned long long)before, (unsigned long long)inv_qty(shiv));
}
static void sk_craft_award(void) {
    if (!g_sk_cr_item) return;
    if (inv_qty(g_sk_cr_item) > g_sk_cr_qty0 || g_sk_cr_done) {
        xlog("award2013: Craft +30 (item %llx %llu -> %llu)", (unsigned long long)g_sk_cr_item, (unsigned long long)g_sk_cr_qty0, (unsigned long long)inv_qty(g_sk_cr_item));
        uint64_t crafted = g_sk_cr_item;
        g_sk_cr_item = 0;
        sk_exec_shiv_uses(crafted);
        sk_crafter_gift();
        award(30, sk_me(), "Craft");
    } else if (g_sk_cr_until && GetTickCount64() > g_sk_cr_until) {
        xlog("craft: item %llx not in the pack (%llu) - cancelled, no Craft award", (unsigned long long)g_sk_cr_item, (unsigned long long)inv_qty(g_sk_cr_item));
        g_sk_cr_item = 0;
    }
}
static uint64_t sk_craft_start(uint64_t a, uint64_t b, uint64_t c) {
    static int n;
    g_sk_cr_item = a; g_sk_cr_qty0 = inv_qty(a); g_sk_cr_until = 0; g_sk_cr_done = 0;
    int melee = a == 0x4c2683c96aa8ba7aULL /* melee-upgrade item */ || a == 0xcac6064c0673ce33ULL /* recipe-melee-upgrade */;
    if (LOGCAP(n, 30)) xlog("skills: crafting started (arg %llx%s)", (unsigned long long)a, melee ? " = melee upgrade" : "");
    if (melee && skl(SK_BRAWLER) && !g_sk_brawler_craft) {
        g_sk_brawler_craft = 1;
        xlog("skills: Brawler %d: modded weapon craft - crafting speed on", skl(SK_BRAWLER));
        sk_upgrades(1);
    }
    return g_sk_craft_start_orig(a, b, c);
}
static uint64_t sk_craft_stop(uint64_t a, uint64_t b, uint64_t c) {
    if (g_sk_brawler_craft) {
        g_sk_brawler_craft = 0;
        xlog("skills: Brawler: craft ended - crafting speed back");
        sk_upgrades(1);
    }
    uint64_t r0 = g_sk_craft_stop_orig(a, b, c);
    if (g_sk_cr_item) { g_sk_cr_until = GetTickCount64() + 3000; sk_craft_award(); }
    return r0;
}
static void sk_craft_hooks(void) {
    static int done;
    if (done) return;
    done = 1;
    if (sk_upg_route("player-start-crafting-behavior") != 2 || sk_upg_route("player-stop-crafting-behavior") != 2) {
        hook_fail("skills: Brawler craft hooks not installed (the crafting natives are not typed impls)");
        return;
    }
    g_sk_craft_start_orig = hook_rec(fnv64("player-start-crafting-behavior"), (fn_learn)sk_craft_start);
    g_sk_craft_stop_orig = hook_rec(fnv64("player-stop-crafting-behavior"), (fn_learn)sk_craft_stop);
    xlog("skills: Brawler craft hooks %s", g_sk_craft_start_orig && g_sk_craft_stop_orig ? "installed" : "FAILED");
}

// ---- crafting keeps the weapon in hand (knowingly against SP: crafting does not switch to the crafted item upon leaving the tab).
// The crafting menu (ss-menu2-crafting, craft-complete lambda) finishes every craft with
//     player-craft-item(recipe-id, !!(player-get-weapon-by-ammo-name(recipe-id) == 0))
// - its 2nd argument is "equip the result": 0 for ammo (arrows, shells), 1 for everything else (molotov, shiv, bombs, health kit, melee upgrade).
// Native (typed impl, exe+0xcb3580): inventory = player+0x2ec8, recipe = inv->vf[0x1a8](id), inv->vf[0x190](recipe, equip, 1) = exe+0xbc4460, which
// adds the items and - only when equip != 0 - calls exe+0xbde7c0(player+0x2f60 weapon controller, item, ...) for the crafted weapon-slot item:
// the draw request that is carried out when the backpack closes (the "switch upon leaving the tab"). The menu is the only caller of
// player-craft-item, and no script listens for player-exit-crafting except companion vox, so passing equip = 0 stops the request at its root:
// the item only goes into the pack, as an arrow craft does, and no weapon change ever reaches the net. The health-key heal from the menu
// (player-instant-draw-weapon-by-name(health-kit), explicit select-health press) is a separate call and is left alone.
// t2r-craft-autoequip.txt: SP behaviour (equip the crafted item).
static uint64_t (*g_craft_item_orig)(uint64_t, uint64_t, uint64_t);
static const char *craft_sid_name(uint64_t s) {
    static const char *const N[] = {"health-kit", "molotov", "smoke-bomb", "pipe-bomb", "stun-bomb", "trap-mine", "melee-upgrade", "shiv",
        "recipe-health-kit", "recipe-molotov", "recipe-smoke-bomb", "recipe-pipe-bomb", "recipe-melee-upgrade", "recipe-shiv", "arrow", "arrow-explosive",
        "machete", "hatchet", "pipe", "bat", "crowbar", "hammer", "board", "knife-survival-mel-player", "dagger-scar-lev-player"};
    if (!s || s == ~0ULL) return NULL;
    for (size_t i = 0; i < sizeof N / sizeof *N; i++) if (s == fnv64_rt(N[i])) return N[i];
    const char *const *snd;
    const char *g = gx_weapon_name(s, &snd);
    if (!g) g = gx_weapon_name(base_weapon(s), &snd);
    if (g) return g;
    g = dc_name(s);
    return g && strcmp(g, "?") ? g : NULL;
}
static uint64_t craft_item_hook(uint64_t recipe, uint64_t equip, uint64_t c) {
    static int n;
    g_sk_cr_done = 1;
    if (!(equip & 0xff)) return g_craft_item_orig(recipe, equip, c);
    FILE *f = exe_side_open("t2r-craft-autoequip.txt", "r");
    if (f) {
        fclose(f);
        if (LOGCAP(n, 40)) xlog("craft: %llx crafted, engine equips it (t2r-craft-autoequip.txt = SP behaviour)", (unsigned long long)recipe);
        return g_craft_item_orig(recipe, equip, c);
    }
    uint64_t w = current_weapon();
    uint64_t r = g_craft_item_orig(recipe, equip & ~0xffULL, c);
    if (LOGCAP(n, 40)) {
        char ib[24], wb[24];
        const char *in = craft_sid_name(recipe), *wn = craft_sid_name(w);
        if (!in) { snprintf(ib, sizeof ib, "%llx", (unsigned long long)recipe); in = ib; }
        if (!wn) { if (!w || w == ~0ULL) wn = "nothing (empty hands)"; else { snprintf(wb, sizeof wb, "%llx", (unsigned long long)w); wn = wb; } }
        xlog("craft: %s crafted, kept %s in hand (engine wanted to equip %s)", in, wn, in);
    }
    return r;
}
static void craft_keep_hook(void) {
    static int done;
    if (done) return;
    done = 1;
    uint8_t *rec = (uint8_t *)((fn_lookup)VMA(0x18000A190))(fnv64("player-craft-item"));
    if (!rec || is_code(*(void **)(rec + 8)) || !is_code(*(void **)(rec + 0x10))) {
        hook_fail("craft: player-craft-item is not a typed impl (rec %p) - crafted items will still be equipped", (void *)rec);
        return;
    }
    void *impl = *(void **)(rec + 0x10);
    g_craft_item_orig = (uint64_t (*)(uint64_t, uint64_t, uint64_t))hook_rec(fnv64("player-craft-item"), (fn_learn)craft_item_hook);
    xlog("craft: keep-weapon hook %s (player-craft-item impl exe+%llx, expected exe+cb3580)", g_craft_item_orig ? "installed" : "FAILED",
         (unsigned long long)((uintptr_t)impl - (uintptr_t)GetModuleHandleW(NULL)));
}

// ---- winmm hooks
// FUN_18001d7d0(hit *h): winmm's per-hit perk gate (Head Hunter: +15 hp on a headshot). Called
// when one of our hits lands on an enemy (FUN_18002cd10, after the damage message): h+0 slot,
// +0x10 headshot flag.
static void sk_hit_hook(const uint8_t *h) {
    if (!h || match_substate() != 6) return;
    uint32_t slot = *(const uint32_t *)h;
    int head = h[0x10] != 0;
    if (slot > T2R_MAX_PEERS - 1 || is_teammate(slot)) return;
    if (head && skl(SK_SHARP) >= 2) sk_heal(skl(SK_SHARP) >= 3 ? 25 : 15, "Sharpshooter headshot");
    uint64_t w = current_weapon();
    if (skl(SK_BRAWLER) && w != ~0ULL && !base_weapon_is_gun(w)) sk_heal(10, "Brawler melee hit");
    if (skl(SK_DMGMARK)) sk_mark(slot);
}
// FUN_18001d820(amount): winmm's parts perk gate (Collector +10%), called for our own awards
static int g_sk_no_collector;
static void sk_parts_hook(int amount) {
    int lv = skl(SK_COLLECTOR);
    if (amount <= 0 || !lv || g_sk_no_collector) return;
    int bonus = (int)__builtin_ceil(amount * (lv >= 2 ? 0.20 : 0.10) / 5.0) * 5;  // winmm rounds to 5s too
    uint8_t *o = (uint8_t *)VMA(0x180135388);
    *(int32_t *)(o + 0x2ba0 + sk_me() * 0xc) += bonus;
    xlog("skills: Collector +%d parts (of %d)", bonus, amount);
}
static void sk_jmp(uint64_t at, const uint8_t *expect, int n, void *to, const char *what) {
    uint8_t *f = (uint8_t *)VMA(at);
    if (memcmp(f, expect, n)) { hook_miss(f, n, "skills: %s prologue differs, not hooked", what); return; }
    DWORD old;
    VirtualProtect(f, 14, PAGE_EXECUTE_READWRITE, &old);
    f[0] = 0xFF; f[1] = 0x25; *(uint32_t *)(f + 2) = 0;
    *(uint64_t *)(f + 6) = (uint64_t)to;
    VirtualProtect(f, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 14);
    xlog("skills: %s hooked", what);
}
static void sk_install(void) {
    static const uint8_t e_lookup[10] = {0x48, 0x8d, 0x41, 0xff, 0x48, 0x83, 0xf8, 0x3f, 0x77, 0x18};
    static const uint8_t e_hit[8] = {0x56, 0x48, 0x83, 0xec, 0x20, 0x48, 0x89, 0xce};
    static const uint8_t e_parts[7] = {0x56, 0x48, 0x83, 0xec, 0x20, 0x89, 0xce};
    // Agility II climb: winmm's perk 0x08 multiplies the climb animation rate by the float at winmm+0xa1140 (mulss at 0x18001bd72, its only user) = 1.4;
    // 2013 climb-speed (booster c34fe2e2 level II) = 1.35 (rules/skills.md)
    {
        float *k = (float *)VMA(0x1800A1140);
        if (*k == 1.4f) { DWORD old; VirtualProtect(k, 4, PAGE_READWRITE, &old); *k = 1.35f; VirtualProtect(k, 4, old, &old); xlog("skills: Agility II climb rate constant 1.4 -> 1.35 (2013 climb-speed)"); }
        else xlog("skills: Agility climb constant is %g, left alone", (double)*k);
    }
    sk_jmp(0x18001D420, e_lookup, 10, (void *)sk_lookup, "skill record lookup (FUN_18001d420)");
    sk_jmp(0x18001D7D0, e_hit, 8, (void *)sk_hit_hook, "per-hit perk gate (FUN_18001d7d0)");
    sk_jmp(0x18001D820, e_parts, 7, (void *)sk_parts_hook, "parts perk gate (FUN_18001d820)");
}

// ---- GEAR UP: the edit page's 4th row and the skill picker
static int g_sk_edit_slot;                  // skill row (0..3) the picker was opened from
static int sk_focused(const char *path) { typedef char (*fn)(uint64_t); uint64_t w = ui_w(path); return w && ((fn)VMA(0x180068BF0))(w); }
static int sk_action(const char *a) { typedef char (*fn)(uint64_t); return ((fn)VMA(0x180068AF0))(fnv64(a)) != 0; }
static void sk_edit_page(void) {
    r2_edit_detail();
    static const char *const ROW[4] = {"SkillOne", "SkillTwo", "SkillThree", "SkillFour"};
    char p[96];
    for (int s = 0; s < 4; s++) {
        snprintf(p, sizeof p, "anim-group.slotList.%s", ROW[s]);
        if (sk_focused(p)) g_sk_edit_slot = s;
    }
    // Opening Loadout 1 sometimes flickered VEPR RIFLE on skill slot 4. The 4th row is a clone of a weapon row whose template text is
    // "VEPR RIFLE"; it was only rewritten every 10th frame (and the row's widgets may not exist yet on the first frames), so the template text
    // showed for up to 10 frames after the page opened. ui_text writes only when the string differs (per widget handle, reset when a page is
    // rebuilt), so asserting it EVERY frame costs nothing and the first frame the widget exists already carries the right name. The icon is
    // re-set when the row's widget handle or the picked skill changed (ui_image has no cache).
    const uint8_t *r = sk_lookup(g_sk4_cur);
    static uint64_t icon_w, icon_page; static const char *icon_img;
    const char *img = r ? ((const sk_rec_t *)r)->icon : "hud/weapon-cross/empty-slot.svg";
    uint64_t iw = ui_w("anim-group.slotList.SkillFour.weaponIcon"), pg = ui_w("anim-group");
    if (iw && (iw != icon_w || pg != icon_page || img != icon_img || !(g_frame % 60))) { ui_image("anim-group.slotList.SkillFour.weaponIcon", img); icon_w = iw; icon_page = pg; icon_img = img; }
    ui_text("anim-group.slotList.SkillFour.weaponName", r ? ((const sk_rec_t *)r)->name : "NOTHING");
    if (r && ((const sk_rec_t *)r)->cost) snprintf(p, sizeof p, "%u LP", ((const sk_rec_t *)r)->cost); else snprintf(p, sizeof p, "FREE");
    ui_text("anim-group.slotList.SkillFour.slotCost", p);
    // the panel header kept the template "LOADOUT 1" for every loadout, and it and the row labels (template texts of the
    // loadout-menu widget) were never translated. Written every frame (ui_text skips unchanged strings).
    if (lo_r3()) ui_text("anim-group.loadoutName", "EQUIPPED");      // R3: column A's heading (the loadout's name is the page title)
    else if (lo_slot(g_lo_active) >= 0) { snprintf(p, sizeof p, "LOADOUT %d", lo_slot(g_lo_active) + 1); ui_text("anim-group.loadoutName", p); }
    static const char *const LBL[7][2] = {{"PrimaryWeapon", "PRIMARY"}, {"SecondaryWeapon", "SECONDARY"}, {"SkillOne", "SKILL"}, {"SkillTwo", "SKILL"},
                                          {"SkillThree", "SKILL"}, {"SkillFour", "SKILL"}, {"Purchasable1", "PURCHASABLE"}};
    for (int i = 0; i < 7; i++) { snprintf(p, sizeof p, "anim-group.slotList.%s.label0", LBL[i][0]); ui_text(p, LBL[i][1]); }
}
static int sk_cost_with(int slot, uint64_t v) {   // loadout LP with skill row `slot` set to v
    loadout_t l;
    lo_read_obj(&l);
    return lo_cost(&l) - sk_cost(sk_slot_get(slot)) + sk_cost(v);
}
static int sk_in_slot(int id) {                   // skill row holding skill id (any level), -1 = none
    for (int s = 0; s < 4; s++) if (sk_id(sk_norm(sk_slot_get(s))) == id) return s;
    return -1;
}
// R3 edit page: DETAILS of the focused slot row - a gun with its LOUD / QUIET lines (the equipped one bright) and stat bars, a skill with its
// three level lines (the equipped level bright), a purchasable / an empty row with its text only.
static void r3_edit_detail(int f, const loadout_t *l) {
    const uint32_t *cnt = (const uint32_t *)VMA(0x1800A0DA4);
    char b[48];
    static char c0[24], c1[24];
    r3_hide_tiles();
    ui_visible("anim-group.detailPanel", 1);
    if (f <= 1 && l->w[f] < cnt[f]) {
        uint32_t idx = l->w[f], bi = idx;
        int j = -1;
        if (idx < g_picker_base[f]) j = supp_row(f, idx);
        else for (uint32_t i = 0; i < g_picker_base[f]; i++) if (supp_row(f, i) == (int)idx) { bi = i; j = (int)idx; break; }
        const uint8_t *row = weapon_row(f, idx), *base = weapon_row(f, bi);
        detail_title(*(const char **)row);
        ui_image("anim-group.detailPanel.perkIcon", *(const char **)(row + 8));
        lo_cost_str(b, sizeof b, *(const uint32_t *)(row + 0x18));
        ui_text("anim-group.detailPanel.perkCost", b);
        int v[4] = {0, 0, 0, 0};
        stat_values(*(const char **)(base + 0x10), v);
        lo_cost_str(c0, sizeof c0, *(const uint32_t *)(base + 0x18));
        if (j >= 0) lo_cost_str(c1, sizeof c1, *(const uint32_t *)(weapon_row(f, (uint32_t)j) + 0x18)); else c1[0] = 0;
        const char *ro[2] = {"LOUD", "QUIET"}, *tx[2] = {R3_LOUD_TXT, R3_QUIET_TXT}, *co[2] = {c0, c1};
        const char *ds = *(const char **)(base + 0x28) ? *(const char **)(base + 0x28) : "";
        r3_detail(ds, j >= 0 ? 2 : 0, ro, tx, co, j >= 0 && (int)idx == j, 1, v);
        return;
    }
    if (f >= 2 && f <= 5) {
        uint64_t sv = sk_norm(sk_slot_get(f - 2));
        int id = sk_id(sv), lv = sk_lv(sv);
        if (id > 0 && id < SK_COUNT && lv >= 1) {
            detail_title(SK[id].name);
            ui_image("anim-group.detailPanel.perkIcon", SK[id].icon);
            snprintf(b, sizeof b, "%s  %u LP", SK_ROMAN[lv], SK[id].cost[lv - 1]);
            ui_text("anim-group.detailPanel.perkCost", b);
            static char co[3][24];
            const char *ro[3], *tx[3], *cp[3];
            int n = SK[id].nlv > 3 ? 3 : SK[id].nlv;
            for (int k = 0; k < n; k++) {
                ro[k] = SK_ROMAN[k + 1]; tx[k] = SK[id].desc[k] ? SK[id].desc[k] : "";
                snprintf(co[k], sizeof co[k], "%u LP", SK[id].cost[k]); cp[k] = co[k];
            }
            r3_detail("", n, ro, tx, cp, lv - 1, 0, NULL);
            return;
        }
    }
    if (f == 6 && l->w[2] < cnt[2]) {
        const uint8_t *row = lo_purch_tab() + l->w[2] * 0x40;
        const char *d0 = *(const char **)(row + 0x30);
        detail_title(*(const char **)row);
        ui_image("anim-group.detailPanel.perkIcon", *(const char **)(row + 8));
        lo_cost_str(b, sizeof b, *(const uint32_t *)(row + 0x18));
        ui_text("anim-group.detailPanel.perkCost", b);
        r3_detail(d0 && !IsBadReadPtr(d0, 1) ? d0 : "", 0, NULL, NULL, NULL, -1, 0, NULL);
        return;
    }
    detail_title(f == 0 ? "NO PRIMARY" : f == 1 ? "NO SIDEARM" : f == 6 ? "NO PURCHASABLE" : "NO SKILL");
    ui_image("anim-group.detailPanel.perkIcon", "streaming/t2r/icons/blocked.png");
    ui_text("anim-group.detailPanel.perkCost", "FREE");
    r3_detail("SELECT to choose one.", 0, NULL, NULL, NULL, -1, 0, NULL);
}
// R3 edit page column B (t3Prev): what the focused slot row offers, dimmed, the equipped one bright - the picker's rows without the focus.
#define R3_PREV_ROWS 14
static void r3_preview(int f, const loadout_t *l) {
    static const char *const HEAD[7] = {"LARGE FIREARM", "SMALL FIREARM", "SURVIVAL SKILL 1", "SURVIVAL SKILL 2", "SURVIVAL SKILL 3",
                                        "SURVIVAL SKILL 4", "PURCHASABLE"};
    if (f < 0 || f > 6 || !ui_w("anim-group.t3Prev")) return;
    const uint32_t *cnt = (const uint32_t *)VMA(0x1800A0DA4);
    struct { const char *nm, *ic; char co[48]; int eq, np; char pt[3][16]; int ps[3]; } R[R3_PREV_ROWS];
    int n = 0;
    memset(R, 0, sizeof R);
    ui_text("anim-group.t3Prev.t3PrevHead", HEAD[f]);
    if (f <= 1) {
        if (f == 0) { R[n].nm = "NO PRIMARY"; R[n].ic = "streaming/t2r/icons/blocked.png"; snprintf(R[n].co, sizeof R[n].co, "FREE"); R[n].eq = l->w[0] == 0xffffffffu; n++; }
        for (uint32_t i = 0; i < g_picker_base[f] && n < R3_PREV_ROWS; i++, n++) {
            const uint8_t *row = weapon_row(f, i);
            int j = supp_row(f, i);
            R[n].nm = *(const char **)row; R[n].ic = *(const char **)(row + 8);
            R[n].eq = l->w[f] == i || (j >= 0 && l->w[f] == (uint32_t)j);
            if (j >= 0) {
                uint32_t a = *(const uint32_t *)(row + 0x18), q = *(const uint32_t *)(weapon_row(f, (uint32_t)j) + 0x18);
                R[n].np = 2;
                if (a) snprintf(R[n].pt[0], 16, "LOUD %u", a); else snprintf(R[n].pt[0], 16, "LOUD FREE");
                snprintf(R[n].pt[1], 16, "QUIET %u", q);
                R[n].ps[0] = l->w[f] == i ? 3 : 1; R[n].ps[1] = l->w[f] == (uint32_t)j ? 3 : 1;
            } else {
                uint32_t a = *(const uint32_t *)(row + 0x18);
                if (R[n].eq) snprintf(R[n].co, sizeof R[n].co, "%s", R3_EQUIPPED); else if (a) snprintf(R[n].co, sizeof R[n].co, "%u LP", a); else snprintf(R[n].co, sizeof R[n].co, "FREE");
            }
        }
    } else if (f <= 5) {
        int slot = f - 2;
        uint64_t cur = sk_norm(sk_slot_get(slot));
        int first = sk_id(cur) > R3_PREV_ROWS ? sk_id(cur) - R3_PREV_ROWS : 0;     // the equipped skill stays in the window
        for (int id = 1 + first; id < SK_COUNT && n < R3_PREV_ROWS; id++, n++) {
            int in = sk_in_slot(id);
            R[n].nm = SK[id].name; R[n].ic = SK[id].icon; R[n].eq = in == slot;
            if (in >= 0 && in != slot) { snprintf(R[n].co, sizeof R[n].co, "IN SKILL %d", in + 1); continue; }
            R[n].np = SK[id].nlv > 3 ? 3 : SK[id].nlv;
            for (int k = 0; k < R[n].np; k++) {
                snprintf(R[n].pt[k], 16, "%s %u", SK_ROMAN[k + 1], SK[id].cost[k]);
                R[n].ps[k] = !(SK[id].avail >> k & 1) ? 0 : in == slot && sk_lv(cur) == k + 1 ? 3 : 1;
            }
        }
    } else {
        for (uint32_t i = 0; i < cnt[2] && n < R3_PREV_ROWS; i++, n++) {
            const uint8_t *row = lo_purch_tab() + i * 0x40;
            uint32_t a = *(const uint32_t *)(row + 0x18);
            R[n].nm = *(const char **)row; R[n].ic = *(const char **)(row + 8); R[n].eq = l->w[2] == i;
            if (R[n].eq) snprintf(R[n].co, sizeof R[n].co, "%s", R3_EQUIPPED); else if (a) snprintf(R[n].co, sizeof R[n].co, "%u LP", a); else snprintf(R[n].co, sizeof R[n].co, "FREE");
        }
    }
    char p[96], pre[64];
    for (int k = 0; k < R3_PREV_ROWS; k++) {
        int on = k < n;
        snprintf(p, sizeof p, "anim-group.t3Prev.t3Pv%dNm", k); ui_text(p, on && R[k].nm ? R[k].nm : ""); if (on) w_alpha(ui_w(p), R[k].eq ? 1.0f : 0.45f);
        snprintf(p, sizeof p, "anim-group.t3Prev.t3Pv%dCo", k); ui_text(p, on ? R[k].co : ""); if (on) w_alpha(ui_w(p), R[k].eq ? 1.0f : 0.6f);
        snprintf(p, sizeof p, "anim-group.t3Prev.t3Pv%dIc", k); ui_visible(p, on && R[k].ic);
        if (on && R[k].ic) { ui_image(p, R[k].ic); w_alpha(ui_w(p), R[k].eq ? 1.0f : 0.45f); }
        if (k) { snprintf(p, sizeof p, "anim-group.t3Prev.t3Pv%dRule", k); ui_visible(p, on); }
        snprintf(pre, sizeof pre, "anim-group.t3Prev.t3Pv%dP", k);
        for (int q = 0; q < 3; q++) r3_pill(pre, q, on && q < R[k].np, R[k].pt[q], R[k].ps[q]);
    }
}
static int sk_picker_page(char entered) {
    static int lvl[SK_COUNT], last_focus = -1, held_sel, held_l, held_r;
    char p[96], b[600];
    // (the picker showed SURVIVAL SKILL 1 / NO SKILL / IN SKILL 2 for 3 frames, then SKILL 2, then SKILL 3): the slot was read every frame
    // from the edit page's focused row, which jumps around while pages swap. It is latched when the picker opens: the edit page's row that has the focus
    // then (the row whose SELECT opened it), else the last one sk_edit_page saw.
    static int latched = -1;
    if (entered) {
        static const char *const ROW[4] = {"SkillOne", "SkillTwo", "SkillThree", "SkillFour"};
        latched = g_sk_edit_slot & 3;
        for (int s = 0; s < 4; s++) { snprintf(p, sizeof p, "anim-group.slotList.%s", ROW[s]); if (sk_focused(p)) { latched = s; break; } }
        g_sk_edit_slot = latched;
    }
    int slot = latched >= 0 ? latched : g_sk_edit_slot & 3;
    uint64_t cur = sk_norm(sk_slot_get(slot));
    if (entered || !(g_frame % 15)) r2_mirror_cur(2 + slot);      // gear up B2: the slot column beside the skill list (the skill row being picked framed)
    if (lo_r3() && (entered || !(g_frame % 30))) {                 // R3: the list to column B, the slot column (EQUIPPED) to A - the template is the list mode
        w_pivot(ui_w("anim-group.list"), R3_COL_B, R3_ROW0 - R3_LIST_TOP);
        w_pivot(ui_w("anim-group.loMirror"), R3_COL_A, R3_ROW0);
        w_pivot(ui_w("anim-group.detailPanel.perkDesc"), 0.0f, R3_DESC_Y);
        ui_visible("anim-group.detailPanel.perkIcon", 1);
        ui_text("anim-group.t3HeadA", "EQUIPPED");
        snprintf(b, sizeof b, "SURVIVAL SKILL %d", slot + 1);
        ui_text("anim-group.pickerTitle", b);
    }
    if (entered && lo_r3()) {
        char nb[48];
        ui_text("anim-group.chrome.menuTitle", lo_name(g_lo_active, nb, sizeof nb));
    } else if (entered) {
        snprintf(b, sizeof b, "SURVIVAL SKILL %d", slot + 1);
        ui_text("anim-group.chrome.menuTitle", b);
        ui_text("anim-group.pickerTitle", "SELECT SKILL");
    }
    if (entered) {
        for (int id = 1; id < SK_COUNT; id++) {
            lvl[id] = 1;
            for (int k = 0; k < SK[id].nlv; k++) if (SK[id].avail >> k & 1) { lvl[id] = k + 1; break; }
            if (sk_id(cur) == id) lvl[id] = sk_lv(cur);
        }
        picker_focus_begin();
        last_focus = -1;
        held_sel = 1;                               // the press that opened the page
    }
    if (sk_id(cur) > 0 && sk_id(cur) < SK_COUNT) picker_focus_tick("picker-skill", "anim-group.list.contents.Skill%d", SK_PICKER_ROWS, sk_id(cur) - 1, SK[sk_id(cur)].name);
    int focus = -1;
    for (int r = 0; r < SK_PICKER_ROWS && r + 1 < SK_COUNT; r++) {
        snprintf(p, sizeof p, "anim-group.list.contents.Skill%d", r);
        if (sk_focused(p)) { focus = r; break; }
    }
    // Skill levels could not be clicked: the level boxes sit in the DETAILS panel, off the list. The rows are hoverable, the panel is not,
    // so the engine drops the list focus as the cursor moves from a row to a box (or on the click on one) - focus read -1, fid 0, and the
    // box code below (and the highlighted row) went with it. The last focused row is kept, and its focus given back to the engine.
    static int keep_focus = -1;
    if (entered) keep_focus = -1;
    if (focus >= 0) keep_focus = focus;
    else if (keep_focus >= 0 && !entered) {
        snprintf(p, sizeof p, "anim-group.list.contents.Skill%d", keep_focus);
        uint64_t fw = ui_w(p), fa[1] = {fw};
        if (fw) dc_call(fnv64("menu2-set-focus-to-component"), 1, fa);
        focus = keep_focus;
    }
    int fid = focus >= 0 ? focus + 1 : 0;
    int changed = entered || focus != last_focus;
    if (!entered && focus >= 0 && last_focus >= 0 && focus != last_focus && lo_r3()) r3_move_in("anim-group.detailPanel");   // launcher: another option = DETAILS m-in
    last_focus = focus;
    // A / D: level of the focused skill
    int l_on = sk_action("menu-left"), r_on = sk_action("menu-right");
    if (fid && SK[fid].nlv > 1) {
        if (l_on && !held_l && lvl[fid] > 1) { lvl[fid]--; changed = 1; }
        if (r_on && !held_r && lvl[fid] < SK[fid].nlv) { lvl[fid]++; changed = 1; }
    }
    held_l = l_on; held_r = r_on;
    // mouse on the level boxes: hover highlights that level's frame, a click sets it (the click's
    // menu-select below then equips that level, as SELECT does after A / D)
    static int lvl_hov = -1, held_lb;
    int hv = -1;
    if (fid && SK[fid].nlv > 1)
        for (int k = 0; k < SK[fid].nlv && k < 3; k++) {
            char hp[64];
            snprintf(hp, sizeof hp, lo_r3() ? "anim-group.detailPanel.t3Lv%dTx" : "anim-group.detailPanel.skLvlBg%d", k);
            if (ui_mouse_over(hp, 0)) { hv = k; break; }
        }
    if (hv < 0 && lo_r3() && focus >= 0 && fid && SK[fid].nlv > 1)       // R3 rows' I / II / III pills of the focused row, as the DETAILS lines
        for (int k = 0; k < SK[fid].nlv && k < 3; k++) {
            char hp[72];
            if (!(SK[fid].avail >> k & 1)) continue;
            snprintf(hp, sizeof hp, "anim-group.list.contents.Skill%d.t3Pl%dTx", focus, k);
            if (ui_mouse_over(hp, 0)) { hv = k; break; }
        }
    if (hv != lvl_hov) { lvl_hov = hv; changed = 1; }
    g_sk_chip_hov = hv;                              // the click gate lets a click on a level box through as a confirm (equips that level)
    int lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (lb && !held_lb) {
        static int ncl;
        if (LOGCAP(ncl, 40)) xlog("skills: click - focus row %d (kept %d) fid %d, level box under the cursor %d", focus, keep_focus, fid, hv);
    }
    if (hv >= 0 && lb && !held_lb && lvl[fid] != hv + 1) {
        lvl[fid] = hv + 1; changed = 1;
        xlog("skills: %s level -> %s (mouse click)", SK[fid].name, SK_ROMAN[lvl[fid]]);
    }
    held_lb = lb;
    // select: equip / change level / unequip
    int sel = sk_action("menu-select");
    if (sel && !held_sel && fid) {
        uint64_t v = SK_V(fid, lvl[fid]);
        int other = sk_in_slot(fid);
        int ok = 1;
        char dm[160];
        dm[0] = 0;
        if (!(SK[fid].avail >> (lvl[fid] - 1) & 1)) { ok = 0; snprintf(dm, sizeof dm, "%s %s is not available yet.", SK[fid].name, SK_ROMAN[lvl[fid]]); }   // not in yet
        else if (other >= 0 && other != slot) { ok = 0; snprintf(dm, sizeof dm, "%s is already in Survival Skill %d.", SK[fid].name, other + 1); }          // in another skill row
        else if (cur == v) sk_slot_set(slot, 0);                               // selected again: unequip
        else if (sk_cost_with(slot, v) > 13) {                                 // over 13 LP: the launcher's wording, points left with this row emptied
            ok = 0;
            ui_deny_lp(SK[fid].name, (int)SK[fid].cost[lvl[fid] - 1], 13 - sk_cost_with(slot, 0));
        }
        else sk_slot_set(slot, v);
        if (ok) sk_sound(1);
        else if (dm[0]) ui_deny(dm);                                           // every refusal: sfx-menu-error + a notice (was winmm's own error sound, no message)
        xlog("skills: row %d %s %s -> %s", slot + 1, SK[fid].name, SK_ROMAN[lvl[fid]], ok ? "ok" : "refused");
        cur = sk_norm(sk_slot_get(slot));
        changed = 1;
    }
    held_sel = sel;
    lp_left_text(b, sizeof b);                      // points LEFT, red at none - the same counter as the weapon pickers
    ui_text("anim-group.lpTotal", b);
    if (!changed && (g_frame % 20)) return 1;
    for (int r = 0; r < SK_PICKER_ROWS; r++) {
        int id = r + 1;
        snprintf(p, sizeof p, "anim-group.list.contents.Skill%d", r);
        ui_visible(p, id < SK_COUNT);
        if (id >= SK_COUNT) continue;
        snprintf(p, sizeof p, "anim-group.list.contents.Skill%d.skillIcon", r);
        ui_image(p, SK[id].icon);
        snprintf(p, sizeof p, "anim-group.list.contents.Skill%d.label0", r);
        ui_text(p, SK[id].name);
        int in = sk_in_slot(id);
        if (lo_r3()) {                          // R3: I / II / III pills with their LP (the equipped level chosen); IN SKILL n instead when another row has it
            char pre[64], t[24];
            snprintf(pre, sizeof pre, "anim-group.list.contents.Skill%d.t3Pl", r);
            for (int k = 0; k < 3; k++) {
                int on = k < SK[id].nlv && !(in >= 0 && in != slot);
                snprintf(t, sizeof t, "%s %u", SK_ROMAN[k + 1], SK[id].cost[k]);
                int st = !(SK[id].avail >> k & 1) ? 0 : in == slot && sk_lv(cur) == k + 1 ? 3 : id == fid && lvl[id] == k + 1 ? 2 : 1;
                r3_pill(pre, k, on, t, st);
            }
            snprintf(p, sizeof p, "anim-group.list.contents.Skill%d.costText", r);
            if (in >= 0 && in != slot) snprintf(b, sizeof b, "IN SKILL %d", in + 1); else b[0] = 0;
            ui_text(p, b);
            snprintf(p, sizeof p, "anim-group.list.contents.Skill%d.t3Lbl", r); ui_text(p, "");
            snprintf(p, sizeof p, "anim-group.list.contents.Skill%d.skillIcon", r); ui_visible(p, 1);
            snprintf(p, sizeof p, "anim-group.list.contents.Skill%d.t3Rule", r); ui_visible(p, id + 1 < SK_COUNT);
            continue;
        }
        if (in == slot) snprintf(b, sizeof b, "EQUIPPED %s", SK_ROMAN[sk_lv(cur)]);
        else if (in >= 0) snprintf(b, sizeof b, "IN SKILL %d", in + 1);
        else if (!(SK[id].avail >> (lvl[id] - 1) & 1)) snprintf(b, sizeof b, "LOCKED");
        else snprintf(b, sizeof b, "%s  %u LP", SK_ROMAN[lvl[id]], SK[id].cost[lvl[id] - 1]);
        snprintf(p, sizeof p, "anim-group.list.contents.Skill%d.costText", r);
        ui_text(p, b);
        // locked (the shown level does nothing here yet): the whole row greyed out
        snprintf(p, sizeof p, "anim-group.list.contents.Skill%d", r);
        uint64_t w = ui_w(p);
        if (w) {
            union { float f; uint32_t u; } al = {(SK[id].avail >> (lvl[id] - 1) & 1) ? 1.0f : 0.35f};
            uint64_t args[2] = {w, al.u};
            dc_call(0x48d6c1859a4eba13ULL /* gui2-set-widget-alpha */, 2, args);
        }
    }
    if (!fid) return 1;
    // DETAILS: name + level, icon, cost, 2013 description (+ what is not in yet), level boxes
    int lv = lvl[fid], av = SK[fid].avail >> (lv - 1) & 1;
    if (lo_r3()) {                              // R3: the three level lines (the shown one bright + the accent bar), no boxes
        r3_hide_tiles();
        ui_text("anim-group.detailPanel.perkName", SK[fid].name);
        ui_image("anim-group.detailPanel.perkIcon", SK[fid].icon);
        if (av) snprintf(b, sizeof b, "%s  %u LP", SK_ROMAN[lv], SK[fid].cost[lv - 1]); else snprintf(b, sizeof b, "NOT AVAILABLE YET");
        ui_text("anim-group.detailPanel.perkCost", b);
        static char co[3][24];
        const char *ro[3], *tx[3], *cp[3];
        int n = SK[fid].nlv > 3 ? 3 : SK[fid].nlv;
        for (int k = 0; k < n; k++) {
            ro[k] = SK_ROMAN[k + 1]; tx[k] = SK[fid].desc[k] ? SK[fid].desc[k] : "";
            if (SK[fid].avail >> k & 1) snprintf(co[k], sizeof co[k], "%u LP", SK[fid].cost[k]); else snprintf(co[k], sizeof co[k], "LOCKED");
            cp[k] = co[k];
        }
        r3_detail("", n, ro, tx, cp, lv - 1, 0, NULL);
        return 1;
    }
    snprintf(b, sizeof b, "%s %s", SK[fid].name, SK_ROMAN[lv]);
    ui_text("anim-group.detailPanel.perkName", b);
    ui_image("anim-group.detailPanel.perkIcon", SK[fid].icon);
    if (av) lo_cost_str(b, sizeof b, SK[fid].cost[lv - 1]);
    else snprintf(b, sizeof b, "NOT AVAILABLE YET");
    ui_text("anim-group.detailPanel.perkCost", b);
    snprintf(b, sizeof b, "%s", SK[fid].desc[lv - 1]);   // the 2013 text only (SK[].note = dev notes, not shown)
    detail_desc(b, 360.0f, 1040.0f);   // t2r-ext.c: shrink to fit above the level buttons (skLvlHint at 1120; box top 740); also resets the scale the loadout list set on this shared widget
    ui_visible("anim-group.detailPanel.skLvlHint", SK[fid].nlv > 1);
    for (int k = 0; k < 3; k++) {
        int on = k < SK[fid].nlv;
        uint64_t v = SK_V(fid, k + 1);
        int afford = on && (cur == v || sk_cost_with(slot, v) <= 13) && (SK[fid].avail >> k & 1);
        const char *parts[4] = {"skLvlBg%d", "skLvlNum%d", "skLvlCost%d", "skLvlCostR%d"};
        for (int q = 0; q < 4; q++) {
            char nm[48];
            snprintf(nm, sizeof nm, parts[q], k);
            snprintf(p, sizeof p, "anim-group.detailPanel.%s", nm);
            ui_visible(p, on && (q < 2 || (q == 2 ? afford : !afford)));
        }
        snprintf(p, sizeof p, "anim-group.detailPanel.skLvlFrame%d", k);
        ui_visible(p, on && (k + 1 == lv || k == lvl_hov));
        if (!on) continue;
        snprintf(b, sizeof b, "%u", SK[fid].cost[k]);
        snprintf(p, sizeof p, "anim-group.detailPanel.skLvlCost%d", k);
        ui_text(p, b);
        snprintf(p, sizeof p, "anim-group.detailPanel.skLvlCostR%d", k);
        ui_text(p, (SK[fid].avail >> k & 1) ? b : "-");
    }
    return 1;
}

// ---- effects in the match
static void sk_sync_send_to(uint32_t dst) {      // dst 0xff = everybody (the 2 s keep-alive), else one slot (peer resync)
    if (!g_send_tramp) return;
    uint8_t b[9] = {0};
    for (int s = 0; s < 4; s++) { uint64_t v = sk_norm(sk_slot_get(s)); b[s] = (uint8_t)sk_id(v); b[4 + s] = (uint8_t)sk_lv(v); }
    b[8] = (uint8_t)skl(SK_AGILITY);     // effective (Lone Wolf alone = II): what observers' footsteps / listen mode need
    g_send_tramp(MSG_SK_SYNC, b, sizeof b, (uint8_t)dst);
}
static void sk_sync_send(void) { sk_sync_send_to(0xff); }
static uint32_t g_sk_revived_by = 99;          // MSG_REVIVE for us: Reviver's extra health a moment later
static uint64_t g_sk_revived_at;
static void sk_note_revive(uint32_t slot, uint32_t type, const uint8_t *buf, uint64_t len) {
    if (type == 0x44 /* MSG_REVIVE */ && g_downed && (!buf || !len || buf[0] == sk_me())) {
        g_sk_revived_by = slot; g_sk_revived_at = g_t60;
    }
}
// ---- First-Aid Training heal feedback: the bandaging foley + voice lines. Log prefix "fat:".
// Sound: TLOU2's gbl-crafting.bnk crafting-health-kit-fast-01 (rag / tape wrapping; tools/rip_t2_heal_sfx.py) through the PCM mixer, 2D, one 1 s chunk per
// heal tick on BOTH ends (the healer when the tick goes out, the healed player when it arrives): loops with the hold, ends within a second of it.
// (the SP use-foley is anim-event driven and bank-less in MP; the engine events are silent here without their bank). t2r-no-fat-sound.txt = off.
// Voice: healer = the revive-start kind (VX_REVIVE: "I've got you!" / "Hold on!" ...), healed player = the revive-thanks kind (VX_THANKS) once the
// heal ends (no tick for 1.5 s). Team voice system: own faction voice + female variants, queued behind a busy line. Cooldowns: healer line once per
// 10 s per target, thank-you once per 10 s per healer and once per heal. t2r-no-fat-voice.txt = off.
static int feel_flag(const char *file, uint64_t *at, int *val, int every);   // feel.c
#include "sp_heal.h"
#include "skheal.h"
__asm__(".section .rdata,\"dr\"\n.balign 4\n.globl hl_blob\nhl_blob:\n.incbin \"sp_heal.pcm\"\n.text\n");
extern const int16_t hl_blob[];
#define FAT_LINE_CD_MS 10000     // once per 10 s (was 20 s)
static uint64_t g_fat_said[T2R_MAX_PEERS], g_fat_thx[T2R_MAX_PEERS];               // healer line per target / thank-you per healer: last time
static struct { uint64_t last; uint32_t from; int ticks; } g_fat_in;     // heal ticks we receive
// 3D: the sound sits on a body (`ent`: a puppet handle; the mixer follows it, 2013 range curve 3 m full .. 30 m silent).
// healer: on the healed teammate's puppet; healed: on the healer's puppet; everybody else: on the healer's puppet from MSG_SK_HEALFX. ent 0 = plain 2D.
static void fat_sound(const char *who, uint64_t ent, uint32_t key) {
    static uint64_t fat_at; static int fat_val = -1, nl;
    if (feel_flag("t2r-no-fat-sound.txt", &fat_at, &fat_val, 180)) return;
    // 0.55 was too loud, and ticks arriving closer than the 1 s chunk stacked copies (louder still): 0.22, one chunk at a time
    static uint64_t last[T2R_MAX_PEERS + 1];
    uint64_t now = GetTickCount64();
    uint32_t k = key < T2R_MAX_PEERS ? key : T2R_MAX_PEERS;
    // the engine's own event on the body (muffling / reverb / mix come with it); its one-shot is ~1.4 s, so ticks closer than 1.3 s are skipped
    // (the PCM chunk keeps its 950 ms spacing).  A refused event falls through to the PCM wave.
    int eng_on = ent && !esfx_off();
    if (now - last[k] < (eng_on ? 2300u : 950u)) return;
    last[k] = now;
    // the game's real bandaging foley (gbl-hud sfx-bandage-ellie-lt-bicep-2-wraps, 2.3 s) instead of the crafting wave
    if (eng_on && esfx_play("sfx-bandage-ellie-lt-bicep-2-wraps", ent, "spined") && (esfx_play("sfx-bandage-ellie-lt-bicep-2-wraps", ent, "spined"), 1)) /* layered twice: +6 dB */ { if (nl < 12) { nl++; xlog("fat: heal sound (%s) engine event", who); } return; }
    int ok = ent ? pcm_play_at(hl_blob, HEAL_SFX_N, 0.22f, HEAL_SFX_RATE, ent) : pcm_play_lr(hl_blob, HEAL_SFX_N, 0.22f, 0.22f, HEAL_SFX_RATE);
    if (nl < 12) { nl++; xlog("fat: heal sound (%s) %s%s", who, ent ? "3D " : "", ok ? "playing" : "mixer full"); }
}
static uint64_t sk_peer_body(uint32_t slot) { uint8_t *p = sk_peer_ptr(slot); return p ? *(uint64_t *)(p + 0x60) : 0; }
static int fat_voice_on(void) {
    static uint64_t fat_at; static int fat_val = -1;
    return !g_vx_off && !feel_flag("t2r-no-fat-voice.txt", &fat_at, &fat_val, 180);
}
// Healing uses the revive hand animation, points the healer towards the healed, and cuts the anim when they move too far:
// the healer plays the revive's hand-out gesture (reach_gesture: the arm-only partial clip over a STANDING body - the kneeling 2013 pair would drag the healed
// down with it, so it is not used), puts the weapon away (rv_unarm_*, drawn again at the end), pivots to the healed smoothly (revive_face: 45 ms steps, easing,
// never a snap) and has the revive's action lock (camera / interact only). Puppets: MSG_SK_HEALANIM [healed slot][on] = the same gesture on the healer's puppet
// (its facing arrives with its position; a lost "off" ends after 2.5 s). The anim lives exactly as long as the heal hold: released, target out of the keep range
// (SKH_KEEP_XZ), wall between, down / dead / full health, or the healer hit / downed / executed. t2r-no-heal-anim.txt next to the exe = off (heals as before).
static void rv_unarm_begin(void);
static void rv_unarm_end(void);
static struct { int on, unarm, crouch; uint32_t tgt; uint64_t t0, sent, block; } g_ha;
static int ha_enabled(void) {
    static uint64_t at; static int val = -1;
    return !feel_flag("t2r-no-heal-anim.txt", &at, &val, 180);
}
static void ha_send(uint32_t tgt, int on) {
    uint8_t b[2] = {(uint8_t)tgt, (uint8_t)on};
    if (g_send_tramp) g_send_tramp(MSG_SK_HEALANIM, b, 2, 0xff);
    g_ha.sent = GetTickCount64();
}
static void ha_stop(const char *why) {
    if (!g_ha.on) return;
    uint32_t tg = g_ha.tgt; int un = g_ha.unarm;
    reach_gesture(T2R_US, SID_PLAYER, 0);                 // clear-gesture, 0.4 s blend: the body's own locomotion takes over
    // (reach_gesture(off) arms the revive's post-crouch: kept only for a healer who was crouched when the heal began: keep the pose)
    g_reach_crouch_until = g_ha.crouch ? g_frame + secs_frames(0.75f) : 0;
    if (un) rv_unarm_end();                               // the weapon is drawn again next frame (gunexec_tick)
    ha_send(tg, 0);
    xlog("fat: heal anim off (slot %u, %s)", tg, why);
    uint64_t blk = g_ha.block;
    memset(&g_ha, 0, sizeof g_ha); g_ha.block = blk;
}
static uint64_t buddy_widget_show(uint64_t h);
static void heal_ring(uint32_t slot, uint64_t h) {         // the downed + widget over the healed teammate, its ring = their health (white)
    uint8_t *peer = sk_peer_ptr(slot);
    if (!peer || !h) return;
    float hp = *(float *)(peer + 0x9dc), loc = ((float (*)(uint64_t))VMA(0x18000B2A0))(h), mx = (float)sk_hp_max();
    if (loc > 0.0f && (hp < 0.0f || loc < hp)) hp = loc;
    float f = mx > 1.0f ? hp / mx : 0.0f, z = 0.0f;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    uint64_t wdg = buddy_widget_show(h);
    if (!wdg) return;
    uint64_t pa[9] = {wdg, (uint64_t)"progress", 0, 0, 0, 0, 0, 0, 0};
    uint64_t path = dc_call(fnv64("gui2-build-widget-path"), 9, pa);
    if (!path || path == ~0ULL) return;
    uint64_t pb[4] = {path, 0, 0, 12};
    memcpy(&pb[1], &f, 4); memcpy(&pb[2], &z, 4);
    dc_call(fnv64("gui2-set-progress-bar-percent"), 4, pb);
    uint64_t tn[2] = {path, fnv64("t2-white")};
    dc_call(fnv64("gui2-set-widget-palette-tint"), 2, tn);
    static int nl; if (nl < 4) { nl++; xlog("fat: heal ring over slot %u at %.2f", slot, f); }
}
static void ha_begin(uint32_t tgt) {
    uint64_t none = 0, pr = dc_call(fnv64("player-is-prone?"), 0, &none);
    if (g_ha.on || !ha_enabled() || g_unarm_exec || psync_active() || g_exec_until || (pr != ~0ULL && (pr & 0xff))) return;      // (a prone healer heals without the anim, as the prone revive keeps its pose)
    if (GetTickCount64() < g_ha.block) return;
    uint64_t cr = dc_call(fnv64("player-is-crouched?"), 0, &none);
    rv_unarm_begin();
    g_ha.unarm = g_unarm_exec;
    g_ha.crouch = cr != ~0ULL && (cr & 0xff);
    reach_gesture(T2R_US, SID_PLAYER, 1);
    g_ha.on = 1; g_ha.tgt = tgt; g_ha.t0 = GetTickCount64();
    ha_send(tgt, 1);
    xlog("fat: heal anim on (slot %u, revive hand-out gesture, weapon %s)", tgt, g_ha.unarm ? "put away" : "none to put away");
}
static void ha_update(uint32_t tgt, uint64_t body) {      // every frame of the hold
    if (!g_ha.on) return;
    if (g_rv_hit_frame && g_frame - g_rv_hit_frame < 3) { g_ha.block = GetTickCount64() + 1200; ha_stop("healer was hit"); return; }
    // (a healer got stuck after healing): the hold's action lock kept the healer on the spot with the stick pushed (downed there).  Pushing
    // the stick / WASD ends the anim and its lock (the heal itself then ends by range); no new anim for 3 s
    if (GetTickCount64() - g_ha.t0 > 350 && game_focused() && (((GetAsyncKeyState('W') | GetAsyncKeyState('A') | GetAsyncKeyState('S') | GetAsyncKeyState('D')) & 0x8000) || pad_stick_moving())) {
        g_ha.block = GetTickCount64() + 3000; ha_stop("healer moved the stick"); return; }
    uint64_t ex[3] = {fnv64("camera"), fnv64("interact"), fnv64("interact-hold")};
    dc_call(fnv64("joypad-disable-all-commands-except"), 3, ex);      // re-issued every frame (frame-scoped native, as the revive hold)
    if (g_ha.crouch) g_reach_crouch_until = g_frame + 3;    // (the healer was stood up then crouched into the heal): a crouched healer stays crouched
    revive_face(tgt, body, GetTickCount64() - g_ha.t0 < 60);
    if (GetTickCount64() - g_ha.sent > 800) ha_send(tgt, 1);           // keep-alive for the puppets
}
static void fat_healer_line(uint32_t target) {              // we start healing `target`
    static int nl;
    uint64_t now = GetTickCount64();
    if (!fat_voice_on() || target >= T2R_MAX_PEERS || (g_fat_said[target] && now - g_fat_said[target] < FAT_LINE_CD_MS)) return;
    if (!vx_ready(VX_REVIVE, now)) return;                  // busy / gap: skip (never queued -> no late line)
    g_fat_said[target] = now;
    vx_event(VX_REVIVE, "First-Aid heal start");
    if (LOGCAP(nl, 20)) xlog("fat: healer line to slot %u", target);
}
static void fat_healed_poll(void) {                          // every frame: a received heal that has stopped = completed
    static int nl;
    if (!g_fat_in.ticks || GetTickCount64() - g_fat_in.last < 1500) return;
    uint32_t from = g_fat_in.from; int ticks = g_fat_in.ticks;
    g_fat_in.ticks = 0;
    uint64_t now = GetTickCount64();
    if (!fat_voice_on() || g_downed || from >= T2R_MAX_PEERS || (g_fat_thx[from] && now - g_fat_thx[from] < FAT_LINE_CD_MS)) return;
    g_fat_thx[from] = now;
    vx_respond(VX_THANKS, 300, -1);
    if (LOGCAP(nl, 20)) xlog("fat: heal from slot %u done (%d ticks) - thanks queued", from, ticks);
}
static int skills_recv(uint32_t slot, uint32_t type, const uint8_t *buf, uint64_t len) {
    static uint64_t marked_at;
    // a mark a TEAMMATE made reaches us as winmm msg 9 [enemy slot]; note who made it (sk_marks gives the mark that marker's Hawk-eyed, not ours)
    if (type == 9 && buf && len >= 1 && buf[0] < T2R_MAX_PEERS && slot < T2R_MAX_PEERS && is_teammate(slot)) { g_mk_by[buf[0]] = (uint8_t)(slot + 1); g_mk_by_frame[buf[0]] = g_frame ? g_frame : 1; }
    sk_note_revive(slot, type, buf, len);
    if (type == MSG_SK_SYNC) {
        if (slot < T2R_MAX_PEERS && buf && len >= 8) {
            // peer msg trust: the declared skills must fit the loadout rules (levels the skill has, at most 13 LP + 1 slack): an
            // all-level-III claim (Covert III = never marked, Explosion Expert III ...) is dropped, the slot keeps its last honest set
            {   int lp = 0, bad = 0;
                for (int k = 0; k < 4; k++) {
                    int pid = buf[k], plv = buf[4 + k];
                    if (!pid && !plv) continue;
                    if (pid <= 0 || pid >= SK_COUNT || plv < 1 || plv > SK[pid].nlv) { bad = 1; break; }
                    lp += SK[pid].cost[plv - 1];
                }
                if ((bad || lp > 14) && pt_deny(PTK_sk_valid, slot)) {
                    static uint64_t bad_ms; uint64_t tn = GetTickCount64();
                    if (pt_log_due(&bad_ms, tn)) xlog("skills: MSG_SK_SYNC from slot %u dropped - %s (%d LP: %d/%d %d/%d %d/%d %d/%d)", slot, bad ? "a skill id / level that does not exist" : "over the 13 LP budget", lp,
                                                     buf[0], buf[4], buf[1], buf[5], buf[2], buf[6], buf[3], buf[7]);
                    return 1;
                }
            }
            uint8_t agi = len >= 9 ? (buf[8] > 3 && pt_deny(PTK_wire, slot) ? 3 : buf[8]) : 0;
            if (agi != g_peer_agi[slot]) { g_peer_agi[slot] = agi; xlog("skills: slot %u effective Agility %d", slot, agi); }
            if (memcmp(g_peer_skid[slot], buf, 4) || memcmp(g_peer_sklv[slot], buf + 4, 4) || !g_peer_sk_frame[slot]) {
                memcpy(g_peer_skid[slot], buf, 4); memcpy(g_peer_sklv[slot], buf + 4, 4);
                g_peer_sk_frame[slot] = g_frame ? g_frame : 1;
                g_peer_covert2[slot] = sk_peer(slot, SK_COVERT) >= 2;
                xlog("skills: slot %u has %d/%d %d/%d %d/%d %d/%d", slot, buf[0], buf[4], buf[1], buf[5], buf[2], buf[6], buf[3], buf[7]);
            }
        }
        return 1;
    }
    if (type == MSG_SK_MARKED) {
        if (skl(SK_STRAT) && g_t60 - marked_at > 180) { marked_at = g_t60; msg_tag("You've been marked!", 2.5f, 0); }   // msg_tag: nametag look
        return 1;
    }
    if (type == MSG_SK_GIFT || type == MSG_SK_GIFTSTOCK || type == MSG_SK_GIFTACK) return gift_recv(slot, type, buf, len);     // gift.c
    if (type == MSG_SK_HEAL) {
        // any peer could send any amount byte: validate against the First-Aid rules (skheal.h) - teammate, up, skill level's tick value, in range, rate-limited
        static uint64_t last_ok[T2R_MAX_PEERS], bad_log[T2R_MAX_PEERS];
        if (!buf || len < 1) return 1;
        {
            float pb[16], mb[16];
            int sl_ok = slot < T2R_MAX_PEERS;
            const float *sp = sl_ok ? sk_peer_pos(slot, pb) : NULL, *mp = sp ? player_locator(mb) : NULL;
            int have_pos = sp && mp;
            uint64_t now = GetTickCount64();
            int why = skh_check(sl_ok, is_teammate(slot), sl_ok && g_peer_downed[slot], buf[0], sl_ok && g_peer_sk_frame[slot] != 0, sl_ok ? sk_peer(slot, SK_FAT) : 0,
                                have_pos, have_pos ? sp[0] - mp[0] : 0.0f, have_pos ? sp[1] - mp[1] : 0.0f, have_pos ? sp[2] - mp[2] : 0.0f, now, sl_ok ? last_ok[slot] : 0);
            if (why != SKH_OK) {
                if (sl_ok && now - bad_log[slot] > 5000) { bad_log[slot] = now; xlog("skills: MSG_SK_HEAL from slot %u (amount %u) dropped: reason %d", slot, buf[0], why); }
                return 1;
            }
            last_ok[slot] = now;
        }
        sk_heal(buf[0], "First-Aid Training from a teammate");
        // the healed player is told (a tick comes every 2 s while the hold lasts: one message covers it)
        static uint64_t said;
        if (GetTickCount64() - said > 2500) { said = GetTickCount64(); msg_hook("Teammate is healing you, stay close!", 2.5f, 0, 0); }
        if (g_fat_in.ticks && g_fat_in.from != slot) g_fat_in.ticks = 0;     // another healer: a new session
        g_fat_in.from = slot; g_fat_in.last = GetTickCount64(); g_fat_in.ticks++;
        fat_sound("healed", sk_peer_body(slot), slot);
        return 1;
    }
    if (type == MSG_SK_HEALANIM) {            // a healer's hand-out gesture on its puppet: a teammate healing a teammate, kept alive by the sender's repeats
        if (buf && len >= 2 && slot < T2R_MAX_PEERS && is_teammate(slot) && buf[0] < T2R_MAX_PEERS && is_teammate(buf[0]) && ha_enabled() && !g_peer_downed[slot]) {
            uint64_t h = sk_peer_body(slot);
            if (h && buf[1] && !psync_busy_slot(slot) && !rv_reviver_slot(slot)) {
                if (!g_rg[slot].on) reach_gesture((int)slot, h, 1);
                g_uc4_reach_pup_until[slot] = g_t60 + 150;      // refreshed by the keep-alive; a lost "off" ends 2.5 s later (uc4_reach_tick)
            } else if (h && !buf[1] && g_rg[slot].on) { reach_gesture((int)slot, h, 0); g_uc4_reach_pup_until[slot] = 0; }
        }
        return 1;
    }
    if (type == MSG_SK_HEALFX) {              // an observer: a heal tick between two other players (the healed one got MSG_SK_HEAL itself)
        if (buf && len >= 1 && buf[0] != sk_me() && slot != sk_me()) fat_sound("heard", sk_peer_body(slot), slot);
        return 1;
    }
    return 0;
}
static void sk_marks(void) {
    static uint8_t was[T2R_MAX_PEERS];
    static float prev[T2R_MAX_PEERS];
    int hawk = skl(SK_HAWK);
    float pb[16], qb[16];
    for (uint32_t slot = 0; slot < T2R_MAX_PEERS; slot++) {
        uint8_t *peer = sk_peer_ptr(slot);
        if (!peer || !peer[4] || is_teammate(slot)) { was[slot] = 0; prev[slot] = 0; g_mk_hawk[slot] = 0; continue; }
        if (sk_peer(slot, SK_COVERT) >= 3 && peer[0x9fc]) peer[0x9fc] = 0;   // Covert Training III
        int on = peer[0x9fc] != 0;
        float *age = (float *)(peer + 0xa00);
        if (on && !was[slot]) {
            // who marked: a teammate (msg 9 just before) or us (own marking / damage marker / group chain)
            uint32_t who = g_mk_by[slot] && g_frame - g_mk_by_frame[slot] <= 90 && g_frame - g_sk_grp[slot] > 2 ? (uint32_t)g_mk_by[slot] - 1 : sk_me();
            g_mk_hawk[slot] = (uint8_t)(who == sk_me() ? hawk : sk_peer(who, SK_HAWK));
            g_mk_by[slot] = 0;
            { static int nl; if (LOGCAP(nl, 30)) xlog("skills: mark on slot %u made by %s (slot %u), Hawk-eyed %d (ours %d)", slot, who == sk_me() ? "us" : "a teammate", who, g_mk_hawk[slot], hawk); }
            uint8_t b = (uint8_t)slot;                                  // Strategist: tell the target
            if (g_send_tramp) g_send_tramp(MSG_SK_MARKED, &b, 1, slot);
            // Hawk-eyed III: enemies close to the one marked are marked too
            // (a mark from Damage Marker procs this too - wiki: "Marking from damage will proc effects from Hawk-eyed"; only the group chain's own marks do not)
            if (g_mk_hawk[slot] >= 3 && who == sk_me() && g_frame - g_sk_grp[slot] > 2) {
                const float *a = sk_peer_pos(slot, pb);
                for (uint32_t o = 0; o < 4 && a; o++) {
                    if (o == slot) continue;
                    const float *c = sk_peer_pos(o, qb);
                    if (!c) continue;
                    float dx = a[0] - c[0], dy = a[1] - c[1], dz = a[2] - c[2];
                    if (dx * dx + dy * dy + dz * dz < 3.0f * 3.0f) { g_sk_grp[o] = g_frame ? g_frame : 1; sk_mark(o); }       // 2013 group-mark-dist 3
                }
            }
            prev[slot] = *age;
        }
        // 2013 mark time (*net-marker-settings* 5 s, Hawk-eyed extra-mark-time +3 = 8 s); winmm ends a mark at age 6.0, so the age runs at 6/5 speed
        // (6/8 with Hawk-eyed). t2r-no-mark-time-2013.txt: the old 6 s / 9 s.
        if (on && *age > prev[slot] && *age - prev[slot] < 0.5f) {
            static int old_t = -1; static uint64_t old_at;
            if (old_t < 0 || GetTickCount64() - old_at > 5000) { old_at = GetTickCount64(); FILE *f = exe_side_open("t2r-no-mark-time-2013.txt", "r"); old_t = f != NULL; if (f) fclose(f); }
            int mh = g_mk_hawk[slot];      // the MARKER's Hawk-eyed, not ours
            float k = old_t ? (mh ? 6.0f / 9.0f : 1.0f) : mh ? 6.0f / 8.0f : 6.0f / 5.0f;
            *age = prev[slot] + (*age - prev[slot]) * k;
        }
        prev[slot] = on ? *age : 0;
        was[slot] = (uint8_t)on;
        if (!on) g_mk_hawk[slot] = 0;
    }
}
static void sk_radar(void) {                    // Strategist II: enemies within 8 m on the radar (2013 radar-see-all-near-radius 8)
    if (skl(SK_STRAT) < 2) return;
    float mb[16], pb[16];
    const float *me = player_locator(mb);
    for (uint32_t slot = 0; slot < T2R_MAX_PEERS && me; slot++) {
        uint8_t *peer = sk_peer_ptr(slot);
        if (!peer || !peer[4] || peer[5] || is_teammate(slot)) continue;
        const float *p = sk_peer_pos(slot, pb);
        if (!p) continue;
        float dx = p[0] - me[0], dy = p[1] - me[1], dz = p[2] - me[2];
        if (dx * dx + dy * dy + dz * dz < 8.0f * 8.0f && g_shot_until[slot] < GetTickCount64() + 300)
            g_shot_until[slot] = GetTickCount64() + 300;
    }
}
static void sk_alone(void) {                    // Lone Wolf: no standing teammate within 20 m
    float mb[16], pb[16];
    const float *me = player_locator(mb);
    int alone = 1;
    for (uint32_t slot = 0; slot < T2R_MAX_PEERS && me; slot++) {
        uint8_t *peer = sk_peer_ptr(slot);
        if (slot == sk_me() || !peer || !peer[4] || peer[5] || !is_teammate(slot) || g_peer_downed[slot]) continue;
        const float *p = sk_peer_pos(slot, pb);
        if (!p) continue;
        float dx = p[0] - me[0], dz = p[2] - me[2];
        if (dx * dx + dz * dz < 20.0f * 20.0f) alone = 0;
    }
    if (alone != g_sk_alone) xlog("skills: Lone Wolf %s", alone ? "active (alone)" : "off (teammate near)");
    g_sk_alone = alone;
}
// 2013 Lone Wolf HUD (lonewolf.png): while Lone Wolf is active (alone) a row of the granted skills' round icons (Agility, Sharpshooter, +
// Sharp Ears at Lone Wolf II) sits above the armour icons / health ring with "Lone Wolf Active" under it. Widgets wolfHud/wolfHudIcon0..2
// + wolfHudText of to1/currency-hud (clan_widgets.add_wolf_hud; icons skill_icons.py skill-*-hud.png); visibility via gui2-set-widget-visible,
// a 0.25 s fade via gui2-set-widget-alpha. Hidden while dead / downed / outside a running match (the page itself is hidden in menus).
// t2r-no-wolf-hud.txt next to the exe: off. Logs "wolf hud: ...".
static void feature_wolf_hud(void) {
    static uint64_t pg, hw[4], t0;
    static int vis[4] = {-1, -1, -1, -1}, aq[4] = {-1, -1, -1, -1}, off = -1, retry, on_prev;
    static const char *const ROW[4] = {"wolfHudIcon0", "wolfHudIcon1", "wolfHudIcon2", "wolfHudText"};
    if (off < 0) { FILE *f = exe_side_open("t2r-no-wolf-hud.txt", "r"); off = f != NULL; if (f) fclose(f); if (off) xlog("wolf hud: off (t2r-no-wolf-hud.txt)"); }
    uint64_t page = *(uint64_t *)VMA(0x180189BA0);
    if (page != pg) { pg = page; for (int k = 0; k < 4; k++) vis[k] = aq[k] = -1, hw[k] = 0; }
    if (!page) return;
    if (retry > 0) { retry--; return; }
    int lw = skl(SK_LONEWOLF);
    // ("Lone Wolf Active" drew over the store UI - the HUD page sits above the store's page; hidden while the store is open)
    // (it still drew over the store and faded in / out on every store tab switch - a tab switch draws no item rows for ~14 frames, so the
    // 15-frame store test let the HUD fade in between tabs. Any page above the match's base page = a menu is up (store, pause, loadout, options);
    // the row test holds 200 frames for a store that is not its own page. A menu hides the HUD at once, no fade.)
    uint64_t tp = top_page();
    int menu = tp && tp != 0xebfa61ff33c5e658ULL;
    // (leaving the store from its store tab kept the HUD hidden 2-3 s too long - the row hold ran on after the page closed. The
    // hold is for a store drawn without a page of its own; once rows are seen with a page up, the page test alone decides.)
    static int store_paged = -1;
    if (g_shop_draw_frame && g_frame - g_shop_draw_frame < 2 && store_paged != menu) { store_paged = menu; xlog("wolf hud: the store %s", menu ? "is a page (page test only)" : "has no page (row hold 200 frames)"); }
    int shop = menu || hud_menu_up() || (store_paged != 1 && g_shop_draw_frame && g_frame - g_shop_draw_frame < 200);      // (frames: ~1.4 s at 144 fps)
    int on = !off && lw > 0 && g_sk_alone && match_substate() == 6 && !g_downed && !g_down_dying && local_hp() > 0.0f && !shop;
    uint64_t now = GetTickCount64();
    if (on != on_prev) { t0 = (!on && shop) ? now - 250 : now; on_prev = on; xlog("wolf hud: %s (Lone Wolf %d%s)", on ? "fade in" : "fade out", lw, !on && shop ? ", menu up: at once" : ""); }
    float f = now - t0 >= 250 ? 1.0f : (now - t0) / 250.0f;
    if (!on) f = 1.0f - f;
    for (int k = 0; k < 4; k++) {
        int want = f > 0.0f && (k < 2 || k == 3 || lw >= 2);     // Sharp Ears (icon 2) only from Lone Wolf II
        int q = want ? (int)(f * 100.0f + 0.5f) : 0;
        if (want == (vis[k] > 0) && q == aq[k]) continue;
        if (!want && vis[k] <= 0) { vis[k] = 0; aq[k] = 0; continue; }
        uint64_t w = hw[k];
        if (!w) {
            w = hud_widget("wolfHud", ROW[k], NULL);
            if (!w) { if (want) { retry = 60; xlog("wolf hud: widget wolfHud/%s not found (old widget pak?)", ROW[k]); } continue; }
            hw[k] = w;
        }
        if (q != aq[k]) {                                           // alpha before visibility: no frame at the old alpha
            union { float f; uint32_t u; } al = {q / 100.0f};
            uint64_t a[2] = {w, al.u};
            dc_call(0x48d6c1859a4eba13ULL /* gui2-set-widget-alpha */, 2, a);
            aq[k] = q;
        }
        if (want != (vis[k] > 0)) {
            uint64_t a[2] = {w, (uint64_t)want};
            dc_call(0xccf99aa7589d1af9ULL /* gui2-set-widget-visible */, 2, a);
            vis[k] = want;
        }
    }
}
// Healing a teammate with First-Aid Training II was too slow: a tick every 1 s (2013: 2 s) - II 10 hp/s, III 20 hp/s
#define FAT_TICK_MS 1000
static uint64_t g_fat_last_tick;                // last First-Aid tick paid (any hold, any teammate)
static void sk_fat_heal(void) {                 // First-Aid Training II/III: hold E next to a teammate
    static uint64_t held_t0, next_tick;     // real time (GetTickCount64): 'every 2 s' was 120 frames = 0.83 s at 144 fps, 4 s at 30 fps
    static uint32_t target = 99;
    int lv = skl(SK_FAT);
    if (g_gift_anim_until || g_gift_block_heal) { held_t0 = 0; return; }      // a selected gift that can be given takes E (and its hand-out gesture) from the heal hold (gift.c)
    if (lv < 2 || g_downed || sk_hp() <= 0 || psync_role() == 1 || psync_role() == 2) {
        static uint64_t ll;
        if (lv == 1 && (GetAsyncKeyState(0x45) & 0x8000) && GetTickCount64() - ll > 5000) { ll = GetTickCount64(); xlog("skills: First-Aid Training I - healing teammates needs level II"); }
        held_t0 = 0; ha_stop("healer down / dead / in a move"); return;
    }   // (dead / spectating / in an execution: no heal)
    float mb[16], pb[16];
    const float *me = player_locator(mb);
    uint32_t nbr = 99;
    for (uint32_t slot = 0; slot < T2R_MAX_PEERS && me; slot++) {
        uint8_t *peer = sk_peer_ptr(slot);
        if (slot == sk_me() || !peer || !peer[4] || peer[5] || !is_teammate(slot) || g_peer_downed[slot]) continue;
        const float *p = sk_peer_pos(slot, pb);
        if (!p) continue;
        float dx = p[0] - me[0], dy = p[1] - me[1], dz = p[2] - me[2];
        // range: SKH_HOLD_XZ / _Y to start (was 2.0 / 1.5 m: too far for the hand-out to look like it reaches them), SKH_KEEP_XZ / _Y to keep a running hold
        float rxz = held_t0 && slot == target ? SKH_KEEP_XZ : SKH_HOLD_XZ, ry = held_t0 && slot == target ? SKH_KEEP_Y : SKH_HOLD_Y;
        if (dx * dx + dz * dz < rxz * rxz && dy > -ry && dy < ry) {
            // 2013: First-Aid Training pays 20 / 2 s for HEALING - a teammate at full health gets no heal and pays nothing
            // (their peer hp is synced at peer+0x9dc; -1 = a re-apply in flight, treated as unknown)
            float php = *(float *)(peer + 0x9dc);
            // (first aid could not heal - the synced value can sit at full while the puppet's own health
            // shows the wound: the lower of the two decides, as melee_would_kill does)
            uint64_t ph = *(uint64_t *)(peer + 0x60);
            float loc = ph ? ((float (*)(uint64_t))VMA(0x18000B2A0))(ph) : -1.0f;
            if (loc > 0.0f && (php < 0.0f || loc < php)) php = loc;
            if (php >= (float)sk_hp_max() - 0.5f) {
                static uint64_t fl;
                if ((GetAsyncKeyState(0x45) & 0x8000) && GetTickCount64() - fl > 2000) { fl = GetTickCount64(); xlog("skills: First-Aid: slot %u next to us is at full health (%.0f) - no heal", slot, php); }
                continue;
            }
            if (!los_gate(ph, me, p)) continue;     // a wall between us: no prompt, no heal
            nbr = slot; break;
        }
    }
    // the interact hold (E / the controller's interact button, as reviving) with the game's own world prompt "Heal" on the teammate
    // (the prompt was gone and the heal took ~1.5 s to start): the first +HP comes 0.3 s into the hold, then every 2 s
    uint8_t *nbp = nbr != 99 ? sk_peer_ptr(nbr) : NULL;
    uint64_t nh = nbp ? *(uint64_t *)(nbp + 0x60) : 0;
    int held = nbr != 99 && interact_down();
    uint64_t now = GetTickCount64();
    if (!held || nbr != target) {
        ha_stop(!held ? (nbr == 99 ? "hold ended / target out of range, hidden, down or healed" : "hold ended") : "target changed");
        held_t0 = 0; target = nbr;
        if (nbr != 99 && nh) world_prompt(nh, "Heal", 0.0f);
        return;
    }
    // Each new hold paid its first tick 0.3 s in, so taps healed every
    // ~0.3 s instead of every FAT_TICK_MS. The tick interval now runs across holds: a new hold pays no sooner than FAT_TICK_MS after the last tick.
    if (!held_t0) { held_t0 = now; next_tick = now + 300; if (g_fat_last_tick && g_fat_last_tick + FAT_TICK_MS > next_tick) next_tick = g_fat_last_tick + FAT_TICK_MS; }
    // taps on E (a box next to the teammate, impatience) started and stopped the anim every few frames (stand / crouch flicker):
    // the anim starts once the hold has lasted 250 ms. While healing, the + ring over the healed shows their health (FAT2 bar or not).
    if (nh && !g_ha.on && now - held_t0 >= 250) ha_begin(nbr);
    ha_update(nbr, nh);
    if (nh) heal_ring(nbr, nh);
    if (nh) { float p = 1.0f - (float)(next_tick > now ? next_tick - now : 0) / (next_tick - held_t0 < FAT_TICK_MS && next_tick > held_t0 ? (float)(next_tick - held_t0) : (float)FAT_TICK_MS); world_prompt(nh, "Heal", p < 0 ? 0 : p > 1 ? 1 : p); }
    if (now < next_tick) return;
    next_tick = now + FAT_TICK_MS;              // (no catch-up payout after a hitch)
    g_fat_last_tick = now;
    uint8_t hp = lv >= 3 ? 20 : 10;             // every FAT_TICK_MS
    if (g_send_tramp) g_send_tramp(MSG_SK_HEAL, &hp, 1, nbr);
    fat_sound("healer", sk_peer_body(nbr), sk_me());
    if (g_send_tramp) { uint8_t t = (uint8_t)nbr; g_send_tramp(MSG_SK_HEALFX, &t, 1, 0xff); }     // everyone else hears it too (3D, on us)
    if (held_t0 && now - held_t0 < 600) fat_healer_line(nbr);       // the first tick of a hold (0.3 s in)
    g_sk_no_collector = 1;                      // 2013: Collector does not count healing Parts
    {   // 2013 wiki + booster 112c9e7a: 20 parts / 2 s with First-Aid Training 2, 40 / 2 s with 3 (our tick is 1 s: 10 / 20). t2r-no-fa3-parts.txt = flat 10
        static int fa3_off = -1; if (fa3_off < 0) { FILE *ff = exe_side_open("t2r-no-fa3-parts.txt", "r"); fa3_off = ff != NULL; if (ff) fclose(ff); }
        award(r13_off(R13_SCORE) ? 5 : (lv >= 3 && !fa3_off ? 20 : 10), sk_me(), "Heal Teammate");      // 2013 NET_STAT_HEAL_OTHER = 20 per 2 s tick (scoring.md): the same parts per second at the 1 s tick
    }
    g_sk_no_collector = 0;
    xlog("skills: First-Aid Training heal +%d to slot %u", hp, nbr);
    xlog("heal award: +%d parts (Heal Teammate, First-Aid Training %d, slot %u)", r13_off(R13_SCORE) ? 5 : (lv >= 3 ? 20 : 10), lv, nbr);
}
static int g_sk_life_kit;                     // Second Chance kit given this life
static int g_sk_death_streak;                   // deaths without a down / execution (Second Chance)
// 2013 (rules/scoring.md, executions.md): the special-execution stat is 50 / 75 / 100 by Executioner tier ("50% more" at level 2, "double" at
// level 3) -> with the base now 50 the bonus is +25 / +50. Old (t2r-2013-scoring-off.txt): base 75, +13 / +25.
// audit: NOT changed - three sources disagree (wiki "50% more" / "double" = x1.5 / x2, data tiers 50 / 75 / 100 on a base of 50, a rule of
// base 75); the kept compromise is 75 / 88 / 100. Interrogations (wiki: "special executions or interrogations") pay x1.5 / x2 (sk_exec_scale).
static int sk_special_exec_bonus(void) {
    return skl(SK_EXEC) >= 3 ? 25 : skl(SK_EXEC) >= 2 ? 13 : 0;   // base 75 kept: 75 / 88 / 100
}
static int sk_exec_scale(int base) { int lv = skl(SK_EXEC); return lv >= 3 ? base * 2 : lv >= 2 ? base * 3 / 2 : base; }
static void sk_stats(void) {
    static int32_t downs = -1, deaths = -1, execs = -1, revives = -1, specs = 0;
    uint32_t me = sk_me();
    if (me >= T2R_ENGINE_PEERS) return;
    int32_t d = *(int32_t *)(SB_STATS + me * 12), de = *(int32_t *)(SB_STATS + me * 12 + 4);
    int32_t ex = (int32_t)g_sb_exec[me * 3], rv = (int32_t)g_revives[me], sp = (int32_t)g_sb_exec_sp[me];
    if (downs < 0 || d < downs || de < deaths || ex < execs || rv < revives || sp < specs) { downs = d; deaths = de; execs = ex; revives = rv; specs = sp; return; }
    int scored = (d - downs) + (ex - execs);
    if (scored > 0) {
        g_sk_death_streak = 0;
        // Executioner: double Parts for shiv kills - paid with the kill itself (t2r-ext.c gx tick: Shiv Kill 75, x2 with the skill)
        if (skl(SK_LONEWOLF) && g_sk_alone) award(skl(SK_LONEWOLF) >= 2 ? 60 : 30, me, "Lone Wolf");
    }
    // Executioner pays on E (special) executions only (2013 text: "50% more / double the Parts for special executions"; the data tiers
    // 50 / 75 / 100): special base 75 (award_hook) -> 88 at level 2, 100 at level 3. A plain damage kill of a downed enemy stays 50.
    // (paid inside the "Special Execution" award itself now - sk_special_exec_bonus, one popup / tally row)
    if (rv > revives && skl(SK_REVIVER) >= 2) award(skl(SK_REVIVER) >= 3 ? 50 : 25, me, "Reviver");  // revive = +100
    if (de > deaths) g_sk_death_streak += de - deaths;
    downs = d; deaths = de; execs = ex; revives = rv; specs = sp;
}
// Fortitude (2013 text): "earn 25 / 50 Parts after healing more than half your health". A health kit use (the kit count drops) that
// brings the health up by more than half of the maximum within 8 s pays once. (Collector does not count healing Parts.)
// 2013 Heal Self +25 (NET_STAT_HEAL_SELF): a health kit used on ourselves (the kit count drops), paid once the health stops rising (the kit's
// heal is over). Healing parts are not Collector parts. Also ticks the Craft award window.
// The kit count can drop AFTER the engine's heal already raised the health (the use
// animation heals while it plays), so the old 'health at the moment the kit count drops' start point saw no rise and paid nothing. The heal
// start is now the lowest health of the last 6 s (a rolling sample, every tick).
static int g_sk_hp_hist[64]; static uint64_t g_sk_hp_hist_ms[64]; static int g_sk_hp_hist_n;
static void sk_hp_hist_push(void) {
    uint64_t now = GetTickCount64(); int hp = sk_hp();
    if (g_sk_hp_hist_n && now - g_sk_hp_hist_ms[(g_sk_hp_hist_n - 1) & 63] < 100) return;
    g_sk_hp_hist[g_sk_hp_hist_n & 63] = hp; g_sk_hp_hist_ms[g_sk_hp_hist_n & 63] = now; g_sk_hp_hist_n++;
}
static int sk_hp_recent_min(int cur) {
    uint64_t now = GetTickCount64(); int mn = cur;
    for (int i = 1; i <= 63 && i <= g_sk_hp_hist_n; i++) {
        int k = (g_sk_hp_hist_n - i) & 63;
        if (now - g_sk_hp_hist_ms[k] > 6000) break;
        if (g_sk_hp_hist[k] > 0 && g_sk_hp_hist[k] < mn) mn = g_sk_hp_hist[k];
    }
    return mn;
}
// The engine's carry cap for the health kit is data (player-inventory.bin item def +0x88,
// dc_data_patches AMMO_CAPS "health-kit": 4, the 2013 Fortitude II cap). Without Fortitude II the kit count is trimmed back to 3 here.
// The data cap is 4 (Fortitude II), so the engine took a 4th kit from the world and the trim below
// destroyed it. The engine's own max-carried read (exe+ab6b90: binary search of the item-def table that exe+ab6a70 returns -> def+0x88) is mirrored here: the
// def's max is set to OUR cap (3, 4 with Fortitude II) so the pickup is refused at the cap. Written only after the first read proved the offset (the data
// value 4); t2r-no-kit-def.txt = off. The trim stays as the fallback.
static uint8_t *sk_item_def(uint64_t sid) {
    uint8_t *exe = (uint8_t *)GetModuleHandleW(NULL);
    static const uint8_t pro[15] = {0x48,0x83,0xec,0x28,0x65,0x48,0x8b,0x0c,0x25,0x58,0x00,0x00,0x00,0x8b,0x05};
    if (!exe || IsBadReadPtr(exe + 0xab6a70, 15) || memcmp(exe + 0xab6a70, pro, 15)) return NULL;
    uint8_t *tab = ((uint8_t *(*)(void))(exe + 0xab6a70))();
    if (!tab || IsBadReadPtr(tab, 0x18)) return NULL;
    uint32_t n = *(uint32_t *)tab; uint64_t *keys = *(uint64_t **)(tab + 8); uint8_t **defs = *(uint8_t ***)(tab + 0x10);
    if (!n || n > 100000 || !keys || !defs || IsBadReadPtr(keys, n * 8ull) || IsBadReadPtr(defs, n * 8ull)) return NULL;
    for (uint32_t i = 0; i < n; i++) if (keys[i] == sid) return defs[i] && !IsBadReadPtr(defs[i], 0x90) ? defs[i] : NULL;
    return NULL;
}
static void sk_kit_def_cap(int cap) {
    static uint8_t *def; static int state;      // state 0 = not looked up, 1 = proven (max read 4 = data cap), -1 = off / unproven
    const uint64_t kit = 0xa4311670996f650aULL;
    if (state < 0) return;
    if (def && (IsBadReadPtr(def, 0x90) || *(uint64_t *)def != kit)) { def = NULL; state = 0; }
    if (!def) {
        static uint64_t at; uint64_t now = GetTickCount64(); if (at && now - at < 3000) return; at = now;
        if (GetFileAttributesA("t2r-no-kit-def.txt") != INVALID_FILE_ATTRIBUTES) { state = -1; return; }
        def = sk_item_def(kit);
        if (!def) { xlog("medkit def: lookup failed (exe+ab6a70 table) - trim only"); return; }
        uint32_t cur = *(uint32_t *)(def + 0x88);
        xlog("medkit def: health-kit item def at %p, max carried read %u (data cap 4 expected)", (void *)def, cur);
        if (*(uint64_t *)def != kit || cur != 4) { xlog("medkit def: NOT the expected def (sid %llx, max %u) - not written, trim only", (unsigned long long)*(uint64_t *)def, cur); def = NULL; state = -1; return; }
        state = 1;
    }
    if (state == 1 && *(uint32_t *)(def + 0x88) != (uint32_t)cap) {
        xlog("medkit def: max carried %u -> %d", *(uint32_t *)(def + 0x88), cap);
        *(uint32_t *)(def + 0x88) = (uint32_t)cap;
    }
}
static void sk_kit_cap(void) {
    static int last = -1;
    int lv = skl(SK_FORT), cap = lv >= 2 ? 4 : 3;
    if (cap != last) { last = cap; xlog("medkit cap: %d (fortitude %d)", cap, lv); }
    sk_kit_def_cap(cap);
    if (!g_settotal_orig || g_in_spawn || g_downed || sk_hp() <= 0) return;
    int have = (int)inv_qty(0xa4311670996f650aULL /* health-kit */);
    if (have > cap) {
        // set-total-ammo does nothing on the kit, the trim ran every frame and the 4th stayed):
        // player-inventory-consume-item, the call that takes the shiv (shiv_spend, works on live 166).
        for (int i = have; i > cap; i--) dc_direct(fnv64("player-inventory-consume-item"), 0xa4311670996f650aULL, 1, 0);
        int now = (int)inv_qty(0xa4311670996f650aULL /* health-kit */);
        if (now > cap) { uint64_t args[2] = {0xa4311670996f650aULL, (uint64_t)cap}, ret[64] = {0}; g_settotal_orig(ret, 2, args, 0); now = (int)inv_qty(0xa4311670996f650aULL); }
        xlog("medkit cap: %d kits over the cap %d (fortitude %d) -> trimmed to %d%s", have, cap, lv, now, now > cap ? " - STILL OVER" : "");
    }
}
static void sk_heal_self_award(void) {
    static int kits = -1, hp0, peak; static uint64_t t0, peak_ms;
    sk_craft_award();
    sk_hp_hist_push();
    int k = (int)inv_qty(0xa4311670996f650aULL /* health-kit */), hp = sk_hp(), mx = sk_hp_max();
    uint64_t now = GetTickCount64();
    if (kits >= 0 && k < kits && !g_downed && hp > 0) { t0 = now; hp0 = sk_hp_recent_min(hp); peak = hp; peak_ms = now; xlog("heal award: kit used, health %d (start %d)", hp, hp0); }
    kits = k;
    if (!t0) return;
    if (g_downed || hp <= 0 || now - t0 > 12000) { t0 = 0; return; }
    if (hp > peak) { peak = hp; peak_ms = now; }
    if (peak - hp0 >= 10 && (now - peak_ms > 800 || (mx > 0 && peak >= mx))) {
        t0 = 0;
        xlog("heal award: +25 parts (Heal Self, health %d -> %d)", hp0, peak);
        g_sk_no_collector = 1;
        award(25, sk_me(), "Heal Self");
        g_sk_no_collector = 0;
    }
}
static int g_sk_fort_kits = -1, g_sk_fort_hp0;
static uint64_t g_sk_fort_t0;
static void sk_fort_parts(void) {
    int lv = skl(SK_FORT);
    int k = (int)inv_qty(0xa4311670996f650aULL /* health-kit */);
    if (g_sk_fort_kits >= 0 && k < g_sk_fort_kits && lv && !g_downed && sk_hp() > 0) {
        g_sk_fort_hp0 = sk_hp_recent_min(sk_hp()); g_sk_fort_t0 = GetTickCount64();
    }
    g_sk_fort_kits = k;
    if (!g_sk_fort_t0) return;
    uint64_t now = GetTickCount64();
    int hp = sk_hp(), mx = sk_hp_max();
    if (g_downed || hp <= 0 || now - g_sk_fort_t0 > 8000) { g_sk_fort_t0 = 0; return; }
    if (lv && mx > 0 && hp - g_sk_fort_hp0 > mx / 2) {
        g_sk_fort_t0 = 0;
        g_sk_no_collector = 1;
        award(lv >= 2 ? 50 : 25, sk_me(), "Fortitude");
        g_sk_no_collector = 0;
        xlog("heal award: +%d parts (Fortitude %d)", lv >= 2 ? 50 : 25, lv);
        xlog("skills: Fortitude %d: healed %d -> %d (more than half), +%d parts", lv, g_sk_fort_hp0, hp, lv >= 2 ? 50 : 25);
    }
}
static void sk_second_chance(void) {
    if (!skl(SK_SECOND) || g_sk_life_kit || g_downed) return;
    int hp = sk_hp(), mx = sk_hp_max();
    if (hp <= 0 || mx <= 0 || hp * 2 >= mx) return;
    if (inv_qty(0xa4311670996f650aULL /* health-kit */)) return;
    dc_direct(SID_ADD_QTY, 0xa4311670996f650aULL, 1, 0);
    g_sk_life_kit = 1;
    msg_hook("Second Chance: health kit", 2.5f, 0, 0);
    xlog("skills: Second Chance health kit (hp %d/%d)", hp, mx);
}
// Movement multipliers: the game's own motion-match speed natives (all three take a float).
//  - Marathon Runner: the sprint meter (sk_sprint_meter below). Only with t2r-no-sprint-meter.txt it is the old +5 % / +10 % sprint speed.
//  - Agility (2013 net10 booster c34fe2e2, rules/skills.md): I crouch speed x1.15 (ad2f0ee8); II crouch x1.19 + walk x1.09
//    (ab2bc417) + climb x1.35 (winmm's perk 0x08 below; it also latches its own crouch x1.3 once, which this overrides every
//    0.5 s) + silent running (crouch-run-quiet upgrade, footsteps.c).
// winmm's perk 0x08 (FUN_18001bb40) installs its three climb patches once per session (cannot be undone), so it is only switched on
// at Agility II.
static int sk_sprint_meter_off(void);
static void sk_move_set(const char *sid_name, float v) {
    uint64_t a = 0;
    memcpy(&a, &v, 4);
    dc_call(fnv64(sid_name), 1, &a);
}
static float g_sk_mv[3] = {1.0f, 1.0f, 1.0f};         // speeds we set (match end puts them back)

// ---- Agility: the stock PRONE crawl (2013 x1.15 at I, x1.19 at II) --------------------------------------------------------
// The prone crawl is not motion-matched (so the three motion-match natives above do nothing for it), and scaling the
// *player-prone-settings* speed curves (prone_speed_apply) only moved the DOWNED crawl (curves x1.19 logged, crawl
// speed unchanged): the stock clips (ellie-prone-move-fw[-b] ...) move the capsule by their own root motion, so the lever is the
// clips' playback rate.  Every state of anim-player/player-prone.bin has its playback-rate function in the slot at name +0x58:
//   idle / transitions / reloads -> 0x2d18, 0x20d0 ... = f(player-anim-speed-mult(...)), crawl states -> two tiny lambdas
//   s_prone-move, -move-turn-l/r, -idle^move, -strafe-move, -alt-move, -drag-move        -> 0x3658  rate = arg2 + arg3*arg4*X
//   s_prone-new-move-fwd/back/rot-l/rot-r, -alt-move-uw                                  -> 0x2f58  rate = arg2 + arg3*arg4*X'
// (re/gamebin/dc1/anim-player/player-prone.bin; tools/re/dc_lambda_dis.py).  Both are 21 / 23 instructions whose registers r0 (a
// copy of arg0, never read) is free.  Patch (in the loaded DC image, found by scanning for the instruction stream):
//   instr 0: LoadStaticFloatImm r0 = ST[0]     (the lambda's symbol table now points at g_pc_st, ST[0] = the factor)
//   instr 7: Move r7,r2   -> FMul r7 = r2 * r0
//   instr 8: Move r8,r3   -> FMul r8 = r3 * r0
// = rate * factor.  The factor is 1.0 outside Agility and while downed (the downed crawl keeps prone_speed_apply + the UC4 own
// crawl), where the patch is an exact identity (x * 1.0f).  The patch stays in memory; a level reload that re-reads the DC file
// brings the stock lambdas back and a rescan patches them again.  Not touched: the aimed prone strafe (0x2d18, shared with idle).
#define PC_N 2
static const struct { const char *name; uint32_t code_off, hdr_off, n; uint8_t code[23 * 8]; } PC_LAM[PC_N] = {
    {"s_prone-move", 0x4308, 0x3658, 21, {
        0x43, 0x00, 0x31, 0x00, 0, 0, 0, 0, 0x43, 0x01, 0x32, 0x00, 0, 0, 0, 0, 0x43, 0x02, 0x33, 0x00, 0, 0, 0, 0,
        0x43, 0x03, 0x34, 0x00, 0, 0, 0, 0, 0x43, 0x04, 0x35, 0x00, 0, 0, 0, 0, 0x43, 0x05, 0x36, 0x00, 0, 0, 0, 0,
        0x43, 0x06, 0x37, 0x00, 0, 0, 0, 0, 0x43, 0x07, 0x02, 0x00, 0, 0, 0, 0, 0x43, 0x08, 0x03, 0x00, 0, 0, 0, 0,
        0x43, 0x09, 0x04, 0x00, 0, 0, 0, 0, 0x07, 0x08, 0x08, 0x09, 0, 0, 0, 0, 0x43, 0x09, 0x01, 0x00, 0, 0, 0, 0,
        0x5d, 0x09, 0x00, 0x00, 0, 0, 0, 0, 0x0f, 0x09, 0x09, 0x00, 0, 0, 0, 0, 0x5d, 0x09, 0x00, 0x00, 0, 0, 0, 0,
        0x3b, 0x09, 0x09, 0x90, 0, 0, 0, 0, 0x5d, 0x09, 0x00, 0x00, 0, 0, 0, 0, 0x0e, 0x09, 0x09, 0x00, 0, 0, 0, 0,
        0x07, 0x08, 0x08, 0x09, 0, 0, 0, 0, 0x05, 0x07, 0x07, 0x08, 0, 0, 0, 0, 0x00, 0x07, 0x07, 0x00, 0, 0, 0, 0}},
    {"s_prone-new-move", 0x4000, 0x2f58, 23, {
        0x43, 0x00, 0x31, 0x00, 0, 0, 0, 0, 0x43, 0x01, 0x32, 0x00, 0, 0, 0, 0, 0x43, 0x02, 0x33, 0x00, 0, 0, 0, 0,
        0x43, 0x03, 0x34, 0x00, 0, 0, 0, 0, 0x43, 0x04, 0x35, 0x00, 0, 0, 0, 0, 0x43, 0x05, 0x36, 0x00, 0, 0, 0, 0,
        0x43, 0x06, 0x37, 0x00, 0, 0, 0, 0, 0x43, 0x07, 0x02, 0x00, 0, 0, 0, 0, 0x43, 0x08, 0x03, 0x00, 0, 0, 0, 0,
        0x43, 0x09, 0x04, 0x00, 0, 0, 0, 0, 0x07, 0x08, 0x08, 0x09, 0, 0, 0, 0, 0x43, 0x09, 0x01, 0x00, 0, 0, 0, 0,
        0x5d, 0x09, 0x00, 0x00, 0, 0, 0, 0, 0x3b, 0x09, 0x09, 0x08, 0, 0, 0, 0, 0x0f, 0x09, 0x09, 0x00, 0, 0, 0, 0,
        0x5d, 0x09, 0x00, 0x00, 0, 0, 0, 0, 0x3b, 0x09, 0x09, 0x28, 0, 0, 0, 0, 0x5d, 0x09, 0x00, 0x00, 0, 0, 0, 0,
        0x3b, 0x09, 0x09, 0x74, 0, 0, 0, 0, 0x0e, 0x09, 0x09, 0x00, 0, 0, 0, 0, 0x07, 0x08, 0x08, 0x09, 0, 0, 0, 0,
        0x05, 0x07, 0x07, 0x08, 0, 0, 0, 0, 0x00, 0x07, 0x07, 0x00, 0, 0, 0, 0}},
};
static uint64_t g_pc_st[2] __attribute__((aligned(16)));    // the patched lambdas' symbol table: ST[0] = the factor (f32, low dword)
static volatile LONG g_pc_patched;                          // lambdas patched so far (scan thread)
static volatile LONG g_pc_scanning;
static volatile uint8_t *g_pc_probe;                        // one patched stream, to notice a DC reload
static float g_pc_mul = 1.0f;
static uint64_t g_pc_next_scan;                             // g_frame of the next scan attempt
static int g_pc_scans;
static DWORD WINAPI pc_scan_thread(void *arg) {
    (void)arg;
    uint8_t *a = NULL;
    MEMORY_BASIC_INFORMATION mi;
    static uint8_t buf[(1 << 20) + 512];
    int hits = 0;
    while (VirtualQuery(a, &mi, sizeof mi) == sizeof mi) {
        uint8_t *b = (uint8_t *)mi.BaseAddress;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS | 0xF0)) &&      // data only (0xF0 = the execute classes)
            (mi.Protect & (PAGE_READWRITE | PAGE_READONLY))) {
            for (size_t off = 0; off < mi.RegionSize; off += 1 << 20) {
                SIZE_T want = mi.RegionSize - off, got = 0;
                if (want > sizeof buf) want = sizeof buf;                       // 1 MB + the overlap for a stream across the cut
                if (!ReadProcessMemory(GetCurrentProcess(), b + off, buf, want, &got)) continue;
                for (size_t i = 0; i + 8 <= got && i < (1 << 20); i += 8) {
                    if (*(uint64_t *)(buf + i) != *(const uint64_t *)PC_LAM[0].code) continue;
                    for (int k = 0; k < PC_N; k++) {
                        size_t len = PC_LAM[k].n * 8;
                        if (i + len > got || memcmp(buf + i, PC_LAM[k].code, len)) continue;
                        uint8_t *code = b + off + i, *hdr = code - PC_LAM[k].code_off + PC_LAM[k].hdr_off;     // the file's layout is kept
                        uint64_t h[7];
                        SIZE_T hg = 0;
                        if (!ReadProcessMemory(GetCurrentProcess(), hdr, h, sizeof h, &hg) || hg < sizeof h) continue;
                        // lambda header: +0 instruction pointer, +8 symbol table, +0x28 marker, +0x34 instruction count
                        if (h[0] != (uint64_t)code || h[5] != 0xdeadbeef1337f00dULL || (uint32_t)(h[6] >> 32) != PC_LAM[k].n) continue;
                        DWORD o1 = 0, o2 = 0;
                        if (!VirtualProtect(hdr, 0x40, PAGE_READWRITE, &o1)) continue;
                        if (!VirtualProtect(code, len, PAGE_READWRITE, &o2)) { VirtualProtect(hdr, 0x40, o1, &o1); continue; }
                        // order: our symbol table first (instr 0 reads it), then the three instructions (8-byte stores)
                        *(volatile uint64_t *)(hdr + 8) = (uint64_t)g_pc_st;
                        static const uint8_t I0[8] = {0x40, 0, 0, 0, 0, 0, 0, 0}, I7[8] = {0x07, 0x07, 0x02, 0, 0, 0, 0, 0},
                                             I8[8] = {0x07, 0x08, 0x03, 0, 0, 0, 0, 0};
                        *(volatile uint64_t *)(code + 0) = *(const uint64_t *)I0;
                        *(volatile uint64_t *)(code + 7 * 8) = *(const uint64_t *)I7;
                        *(volatile uint64_t *)(code + 8 * 8) = *(const uint64_t *)I8;
                        VirtualProtect(code, len, o2, &o2);
                        VirtualProtect(hdr, 0x40, o1, &o1);
                        hits++;
                        g_pc_probe = code;
                        InterlockedIncrement(&g_pc_patched);
                        xlog("prone crawl: patched the %s rate lambda at %p (header %p, region prot %lx)", PC_LAM[k].name, (void *)code, (void *)hdr, mi.Protect);
                    }
                }
            }
        }
        a = b + mi.RegionSize;
        if ((uint64_t)a >= 0x7fffffff0000ULL) break;
    }
    xlog("prone crawl: scan done, %d lambda(s) patched", hits);
    InterlockedExchange(&g_pc_scanning, 0);
    return 0;
}
// m != 1: install (background scan, retried every 600 frames while nothing is patched, 6 tries) and set the factor
static void prone_crawl_apply(float m, const char *why) {
    // (patches the crawl states' rate lambdas in the loaded DC image after a memory scan. It was OPT-IN (t2r-prone-crawl.txt) and so never ran
    // for players (prone speed was not adjusted by agility 2). ON by default now; t2r-no-prone-crawl.txt = off.)
    { static int off = -1; static uint64_t chk; if (off < 0 || g_frame >= chk) { off = GetFileAttributesA("t2r-no-prone-crawl.txt") != INVALID_FILE_ATTRIBUTES; chk = g_frame + 600; } if (off) m = 1.0f; }
    if (g_pc_patched && !(g_frame % 120)) {                  // a level reload re-reads the DC file: the stock lambdas are back
        volatile uint8_t *pr = g_pc_probe;
        if (!pr || IsBadReadPtr((const void *)pr, 8) || pr[0] != 0x40) {
            xlog("prone crawl: patched lambda gone (DC reloaded), rescan");
            InterlockedExchange(&g_pc_patched, 0); g_pc_scans = 0; g_pc_next_scan = 0;
        }
    }
    if (m != 1.0f && !g_pc_patched && !g_pc_scanning && g_pc_scans < 6 && g_frame >= g_pc_next_scan) {
        g_pc_scans++; g_pc_next_scan = g_frame + 600;
        float one = 1.0f; uint32_t bits; memcpy(&bits, &one, 4); g_pc_st[0] = bits;
        InterlockedExchange(&g_pc_scanning, 1);
        HANDLE th = CreateThread(NULL, 0, pc_scan_thread, NULL, 0, NULL);
        if (th) CloseHandle(th); else InterlockedExchange(&g_pc_scanning, 0);
    }
    if (m == g_pc_mul) return;
    uint32_t bits; memcpy(&bits, &m, 4);
    g_pc_st[0] = bits;                                      // one aligned 8-byte store: the lambdas read it on their next call
    xlog("prone crawl: rate x%.2f -> x%.2f (%s; %ld lambda(s) patched)", g_pc_mul, m, why, (long)g_pc_patched);
    g_pc_mul = m;
}
// diagnostic for the next A/B: the real ground speed while crawling prone (not downed), once per 90 frames at most 40 lines
static void prone_crawl_diag(void) {
    static float px, pz, ema; static uint64_t pf, nl;
    static int prone;
    if (!(g_frame % 6)) {
        uint64_t none = 0, pr = dc_call(fnv64("player-is-prone?"), 0, &none);
        prone = pr != ~0ULL && (pr & 0xff);
    }
    float lb[16]; const float *lp = prone && !g_downed ? player_locator(lb) : NULL;
    if (!lp) { pf = 0; ema = 0; return; }
    if (pf && g_frame > pf && g_frame - pf < 10) {
        float dx = lp[0] - px, dz = lp[2] - pz, v = __builtin_sqrtf(dx * dx + dz * dz) * 1000.0f / ((float)(g_frame - pf) * frame_ms_c());
        ema += (v - ema) * 0.1f;
    } else ema = 0;
    px = lp[0]; pz = lp[2]; pf = g_frame;
    if (ema > 0.1f && !(g_frame % 90) && nl < 40) { nl++; xlog("prone crawl: ground %.2f m/s at rate x%.2f", ema, g_pc_mul); }
}
static float mxs_factor(float *lo);                // melee_exchange.c: our speed factor while in a melee exchange / sequence
static void sk_movement(void) {
    float s_spr = g_sk_mv[0], s_cro = g_sk_mv[1], s_run = g_sk_mv[2];
    static const float M[3] = {1.0f, 1.05f, 1.10f};
    int ag = g_downed ? 0 : skl(SK_AGILITY);
    // Marathon Runner is the sprint meter now (sk_sprint_meter); the 5 / 10 % faster sprint stays only behind t2r-no-sprint-meter.txt
    float spr = g_downed || !sk_sprint_meter_off() ? 1.0f : M[skl(SK_MARATHON) % 3];
    float cro = ag >= 2 ? 1.19f : ag >= 1 ? 1.15f : 1.0f;
    float run = ag >= 2 ? 1.09f : 1.0f;
    { float sd = sd_mul(); spr *= sd; cro *= sd; run *= sd; }
    // slowed (not locked) while in a melee exchange; sprint is brought down to the slowed run so holding sprint gains nothing (sprint ~ 1.75x run)
    float mxs_lo = 1.0f, mxs = mxs_factor(&mxs_lo); static float mxs_prev = 1.0f; int mxs_quiet = mxs != 1.0f || mxs_prev != 1.0f; mxs_prev = mxs;
    if (mxs != 1.0f) { float k = mxs_lo < 1.0f ? (1.0f - mxs) / (1.0f - mxs_lo) : 0.0f; spr *= mxs * (1.0f - 0.435f * k); cro *= mxs; run *= mxs; }      // slowdown.h: -10 % on top of the perk factors (they stay relative)
    int refresh = !(g_frame % 30) && (spr != 1.0f || cro != 1.0f || run != 1.0f);     // winmm's latch / a level reload may undo them
    int idle = !(g_frame % 600);
    // (a recording and a report: the crawl speed itself was fixed but the prone anims were still far too fast): the speed curves
    // below carry Agility's ground speed (stock full stick = 0.80 m/s = the curve top, so the curve looks like the limit). The x1.19 rate patch
    // on top played the crawl clips too fast. The patch now stays at x1.00 (identity). Not measured in game yet: Agility ground speed without it.
    prone_crawl_apply(1.0f, g_downed ? "downed" : "Agility (rate patch off: curves only)");   // the stock prone crawl (rate lambdas); the downed crawl keeps the curves
    // (Agility should speed up the anim itself while not affecting prone speed. The rate patch alone plays the clips faster but the
    // capsule stays at the curve speed; the curves alone left the speed too. The ground speed is the lower of the clips' root motion and
    // the *player-prone-settings* curve (stock: full stick 0.80 m/s = the curve top, prone_crawl_diag), so Agility scales both.
    // Downed keeps its own factor (downed_prone_speed); up again, this puts the Agility factor back on the next frame.)
    if (!g_downed && !g_down_dying) prone_speed_apply(sk_prone_mul(), "Agility");
    prone_crawl_diag();
    if (spr != s_spr || refresh || idle) {
        sk_move_set("player-set-motion-match-sprint-speed-multiplier/f", spr);
        if (spr != s_spr && !mxs_quiet) xlog("skills: Marathon Runner %d: sprint speed x%.2f", skl(SK_MARATHON), spr);
        s_spr = g_sk_mv[0] = spr;
    }
    if (cro != s_cro || refresh || idle) {
        sk_move_set("player-set-motion-match-crouch-speed-multiplier/f", cro);
        if (cro != s_cro && !mxs_quiet) xlog("skills: Agility %d: crouch speed x%.2f", skl(SK_AGILITY), cro);
        s_cro = g_sk_mv[1] = cro;
    }
    if (run != s_run || refresh || idle) {
        sk_move_set("player-set-motion-match-run-speed-multiplier/f", run);
        if (run != s_run && !mxs_quiet) xlog("skills: Agility %d: walk speed x%.2f", skl(SK_AGILITY), run);
        s_run = g_sk_mv[2] = run;
    }
}
// ---- Sprint stamina (2013 limited sprint, NO on-screen meter) + Marathon Runner
// Decoded from the PS4 v1.11 engine (re/factions2013/ps4/logic/movement-camera.md section 1):
//  * pool = 100 units (player-health-settings-online +0x3c); while the player is in the sprint move state the pool drains by 100 / (7 s x
//    sprint-duration booster) per second (net-sprint-settings f0 = 7 s; FUN_0050c730 -> stamina object consume, vf 0x188), so full -> empty in 7 s;
//  * regen: after a DELAY of 2.5 s (x stamina-regen-delay booster) since the last drain tick the pool refills at 20 units / s
//    (x stamina-regen-rate booster) = 0.20 of the pool per second, 5 s from empty (stamina object update FUN_007b2e30);
//  * at EXACTLY 0 the sprint command is disabled (FUN_004fd010: FUN_004740f0(ctrl, 0) while stamina == 0.0); any regen > 0 enables it again,
//    so after hitting empty the sprint is locked for the 2.5 s delay, then it can be started again (a trickle until the pool refills);
//  * Marathon Runner (booster f3489e6b): sprint duration x1.15 / x1.3 (e4a438aa), regen rate x1.15 / x1.3, regen delay x0.85 / x0.7;
//  * no HUD meter in 2013: the only cues are the move itself (sprint drops to the run) and the exhausted breath.
// The T2 engine has the same stamina object (player-get-stamina-percentage, player-settings stamina fields 7 / 3.5 / 1.0 with a 20 s sprint),
// but its sprint-disable at empty is a DC controller (ss-player-sprint-exhaustion-controller) that does not run in MP and it needs data
// patches we cannot verify offline: this meter simulates the 2013 object here and turns it into the move-type natives the wounded gait uses:
// while exhausted the sprint TYPE is set to 'run' (set-sprint-type, feel.c keeps it there through g_sk_exhausted) so holding sprint only runs;
// peers replay our animation so they see the run too. Sprinting = our own horizontal speed > 3.6 m/s (the footsteps tiers: walk 0.5 /
// run 2.6 / sprint 4.6).
// t2r-no-sprint-meter.txt next to the exe: no meter, Marathon Runner is the old +5 / +10 % sprint speed. Logs "skills: sprint meter ...".
#define SPRINT_2013_SECONDS 7.0f        // net-sprint-settings f0
#define SPRINT_2013_DELAY 2.5f          // player-health-settings-online +0x40 (float)
#define SPRINT_2013_REGEN 0.20f         // +0x44 = 20 units / s of a 100 pool
#define SPRINT_2013_REENABLE 0.05f      // 2013: > 0 (any trickle); ours 5 % against per-frame anim flicker
static float g_sk_stam = 1.0f;
static int hc_rush_towards(float vx, float vz);   // helpcall.c: running towards a downed teammate we marked
static int sk_sprint_meter_off(void) {
    static int off = -1; static uint64_t at;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - at > 5000) {
        at = now;
        // 2013 HAD limited sprint (no on-screen meter) - ON again by default, no text: exhaustion is heard (heavy breathing)
        // and felt (sprint drops to the run). t2r-no-sprint-meter.txt = off.
        FILE *f = exe_side_open("t2r-no-sprint-meter.txt", "r");
        int o = f != NULL;
        if (f) fclose(f);
        if (o != off) xlog("skills: sprint meter %s", o ? "OFF (t2r-no-sprint-meter.txt)" : "on (2013 limited sprint)");
        off = o;
    }
    return off;
}
static void sk_sprint_type(int exhausted) {
    uint64_t s = fnv64(exhausted ? "run" : "sprint");
    dc_call(fnv64("set-sprint-type"), 1, &s);
}
static void sk_sprint_meter(int live) {
    static uint64_t t_ms, win_ms, delay_until, asserted;
    static float px, pz, v, vx, vz;
    static int have;
    uint64_t now = GetTickCount64();
    float dt = t_ms ? (float)(now - t_ms) / 1000.0f : 0.0f;
    t_ms = now;
    if (dt > 0.25f) dt = 0.25f;
    if (sk_sprint_meter_off() || !live || g_downed || g_down_dying || psync_active()) {
        if (g_sk_exhausted || g_sk_stam < 1.0f) {
            g_sk_exhausted = 0; g_sk_stam = 1.0f; sk_sprint_type(0);
            xlog("skills: sprint meter reset (%s)", !live ? "not live" : "downed / paired move / off");
        }
        have = 0;
        return;
    }
    float mb[16];
    const float *me = player_locator(mb);
    if (me) {                                     // horizontal speed over >= 0.2 s windows
        if (!have) { px = me[0]; pz = me[2]; win_ms = now; have = 1; v = 0; vx = vz = 0; }
        else if (now - win_ms >= 200) {
            float dx = me[0] - px, dz = me[2] - pz, d2 = dx * dx + dz * dz;
            v = d2 > 36.0f ? 0.0f : __builtin_sqrtf(d2) / ((float)(now - win_ms) / 1000.0f);   // > 6 m in one window = teleport / respawn
            float ws = (float)(now - win_ms) / 1000.0f; vx = d2 > 36.0f ? 0.0f : dx / ws; vz = d2 > 36.0f ? 0.0f : dz / ws;
            px = me[0]; pz = me[2]; win_ms = now;
        }
    }
    int mar = skl(SK_MARATHON);
    float dur = mar >= 2 ? 1.30f : mar >= 1 ? 1.15f : 1.0f;                    // sprint longer
    // The sprint stamina limit is halved when running the tac shotgun - this life's loadout carries the Tactical Shotgun: half the sprint
    { static int tac_last = -1; int tac = gl_life_has_tac(); if (tac) dur *= 0.5f; if (tac != tac_last) { tac_last = tac; xlog("skills: sprint meter %s (Tactical Shotgun %s)", tac ? "halved" : "full", tac ? "in this life's loadout" : "not carried"); } }
    float reg = mar >= 2 ? 1.30f : mar >= 1 ? 1.15f : 1.0f;                    // regenerates faster
    float dly = mar >= 2 ? 0.70f : mar >= 1 ? 0.85f : 1.0f;                    // regen delay
    if (!mar && cp_stamina_on()) { reg = 1.15f; dly = 0.85f; }                   // clan food surplus: Marathon Runner I's recovery (clanperk.c)
    int rush = v > 2.0f && hc_rush_towards(vx, vz);
    if (rush && g_sk_exhausted) {                                                 // marked a downed teammate and running at them: sprint again at once
        g_sk_exhausted = 0; sk_sprint_type(0);
        xlog("skills: sprint meter: rushing to a marked downed teammate -> sprint again (no limit)");
    }
    int sprinting = !g_sk_exhausted && v > 3.6f;
    if (sprinting && rush) delay_until = now + (uint64_t)(1000.0f * SPRINT_2013_DELAY * dly);   // no drain, no regen while rushing
    else if (sprinting) {
        g_sk_stam -= dt / (SPRINT_2013_SECONDS * dur);
        delay_until = now + (uint64_t)(1000.0f * SPRINT_2013_DELAY * dly);       // every drain tick restarts the regen delay
        if (g_sk_stam <= 0.0f) {
            g_sk_stam = 0.0f; g_sk_exhausted = 1; asserted = now;
            cp_note_exhausted();
            sk_sprint_type(1);
            { uint64_t sv[4] = {SID_PLAYER, fnv64("vox-eff-gen-breath-exhausted-heavy"), 2, 0}; dc_call(fnv64("say"), 4, sv); }   // 2013: no text, the exhausted breath (every voice has it)
            xlog("skills: sprint meter empty -> sprint type run (Marathon Runner %d: drain /%.2f, regen x%.2f, delay x%.2f)", mar, (double)dur, (double)reg, (double)dly);
        }
    } else if (now >= delay_until) {
        g_sk_stam += dt * SPRINT_2013_REGEN * reg;
        if (g_sk_stam > 1.0f) g_sk_stam = 1.0f;
        if (g_sk_exhausted && g_sk_stam >= SPRINT_2013_REENABLE) {   // 2013: the moment the pool is above 0; 5 % (0.35 s of sprint) so the sprint / run animation does not flicker per frame
            g_sk_exhausted = 0; sk_sprint_type(0);
            xlog("skills: sprint meter back to %.0f%% -> sprint again", (double)(g_sk_stam * 100.0f)); { uint64_t sv[4] = {SID_PLAYER, fnv64("vox-eff-gen-breath-exhausted-recover"), 2, 0}; dc_call(fnv64("say"), 4, sv); }
        }
    }
    if (g_sk_exhausted && now - asserted > 400) { asserted = now; sk_sprint_type(1); }    // scripts reset the types on mode changes
    static uint64_t lg;
    static int lastq = -1;
    int q = (int)(g_sk_stam * 10.0f);
    if (q != lastq && now - lg > 1500) {
        lg = now; lastq = q;
        xlog("skills: sprint meter %d%% (%.1f m/s%s)", (int)(g_sk_stam * 100.0f), (double)v, g_sk_exhausted ? ", exhausted" : "");
    }
}
static void sk_sprint_meter_reset(void) {
    if (g_sk_exhausted) { g_sk_exhausted = 0; sk_sprint_type(0); xlog("skills: sprint meter: match over, sprint type back"); }
    g_sk_stam = 1.0f;
}

// ---- Pistol Auto-Zoom (2013: "zoom in while aiming a 9mm, Revolver, Enforcer or Burst Pistol", II "further")
// The game has no pistol aim zoom, but the camera FOV can be overridden per frame (override-fov, as the execution camera and winmm's menu lock).
// While aiming a pistol the FOV narrows by 1.25 (I) / 1.5 (II) in tangent space, eased in / out over 0.25 s. The base FOV is what the camera
// reports (camera-get-fov-degrees) unless that equals what we set last frame (then the cached value). Not while an execution camera / paired
// move runs. t2r-no-pistol-zoom.txt next to the exe: off. Logs "skills: Pistol Auto-Zoom ...".
static int sk_pistol_in_hand(void) {
    uint64_t w = base_weapon(current_weapon());
    return w == fnv64("pistol-beretta") || w == fnv64("pistol-glock") || w == fnv64("pistol-revolver-taurus") ||
           w == fnv64("pistol-cz75-enforcer") || w == fnv64("pistol-xcaliber") || w == fnv64("pistol-snubby");
}
static void sk_pistol_zoom(int live) {
    static float s, base = 73.74f, last_set;
    static uint64_t t_ms, off_at, lg;
    static int on, off = -1;
    uint64_t now = GetTickCount64();
    float dt = t_ms ? (float)(now - t_ms) / 1000.0f : 0.0f;
    t_ms = now;
    if (dt > 0.2f) dt = 0.2f;
    if (off < 0 || now - off_at > 5000) { off_at = now; FILE *f = exe_side_open("t2r-no-pistol-zoom.txt", "r"); off = f != NULL; if (f) fclose(f); }
    int lv = skl(SK_ZOOM), want = 0;
    if (lv && live && !off && !g_downed && !g_down_dying && !g_xcam_on && g_xcam_w < 0.001f && !psync_active() && sk_pistol_in_hand()) {
        uint64_t none = 0, r = dc_call(fnv64("player-is-aiming?"), 0, &none);
        want = r != ~0ULL && (r & 0xff);
    }
    if (!want && s <= 0.0f) { if (on) { on = 0; xlog("skills: Pistol Auto-Zoom off"); } return; }
    if (!on) { on = 1; xlog("skills: Pistol Auto-Zoom %d on (x%.2f)", lv, lv >= 2 ? 1.5 : 1.25); }
    s += (want ? dt : -dt) / 1.0f;       // 2013 *net-auto-zoom-duration-seconds* = 1 s (ps4/logic/movement-camera.md); was a 0.25 s guess
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    uint64_t none = 0, fv = dc_call(fnv64("camera-get-fov-degrees"), 0, &none);
    float f = 0.0f;
    if (fv != ~0ULL) memcpy(&f, &fv, 4);
    if (f > 30.0f && f < 110.0f && __builtin_fabsf(f - last_set) > 0.03f) base = f;   // equal to last frame's override = our own value coming back
    float zoom = 1.0f + (lv >= 2 ? 0.5f : 0.25f) * (s * s * (3.0f - 2.0f * s));
    float t = __builtin_tanf(base * 0.5f * 3.14159265f / 180.0f) / zoom;
    float fov = 2.0f * __builtin_atanf(t) * 180.0f / 3.14159265f;
    last_set = fov;
    uint64_t a = 0;
    memcpy(&a, &fov, 4);
    dc_call(fnv64("override-fov"), 1, &a);
    if (now - lg > 2000) { lg = now; xlog("skills: Pistol Auto-Zoom fov %.2f (base %.2f, ease %.2f)", (double)fov, (double)base, (double)s); }
}
static void sk_movement_reset(void) {              // match over: stock speeds again
    if (g_sk_mv[0] != 1.0f || g_sk_mv[1] != 1.0f || g_sk_mv[2] != 1.0f) {
        sk_move_set("player-set-motion-match-sprint-speed-multiplier/f", 1.0f);
        sk_move_set("player-set-motion-match-crouch-speed-multiplier/f", 1.0f);
        sk_move_set("player-set-motion-match-run-speed-multiplier/f", 1.0f);
        xlog("skills: movement speeds back to x1.00");
    }
    g_sk_mv[0] = g_sk_mv[1] = g_sk_mv[2] = 1.0f;
    prone_crawl_apply(1.0f, "match over");
    if (!g_downed) prone_speed_apply(1.0f, "match over");
}

// ---- Lucky Break (supply box modifiers), Scavenger (kill drops), Awareness (nametags), Bomb Expert (fuse)
// Lucky Break (2013 ss-region-spawn-pickups.dc, booster 'supply box skill'): the box tops pistol ammo up by
// x1.5 (I-III) and long gun ammo by x1.25 (II/III), bumps a small ammo pickup when rounding ate that bonus,
// and II/III raise the crafting item caps of a box (supplybox.c b13_crafting). Applied in the box plan.
static void sk_box_mods(box_mods_t *m) {
    int lv = skl(SK_LUCKY);
    m->lucky = lv;
    if (lv >= 1) { m->pistol_ammo_mul = 1.5f; m->ammo_round_fix = 1; }
    if (lv >= 2) m->long_ammo_mul = 1.25f;
}
// Scavenger (2013 wiki): I "enemies drop 50% more ammo"; II "automatically pick up ammo and supplies from kills, enemies drop double the ammo";
// III + "enemies sometimes drop a random crafting item". The drop is the kill ammo pack (t2r-ext.c kill_drop_gate / xdrop_one, spawned at the
// body, survives the post-death sweep, synced like every kill drop). Called for OUR OWN down / execution only: scales the pack (I x1.5,
// II/III x2); II/III pick it up for us (returns 1 -> the caller spawns no pack) and III sometimes adds a crafting ingredient.
static int g_sk_scav_f1, g_sk_scav_f2;          // Scavenger III's crafting-drop toggle (2013 vars dc6241fb / 02e7fc5e)
static void sk_scav_reset(void) { g_sk_scav_f1 = g_sk_scav_f2 = 0; }
static int sk_scav_pay(uint64_t item, uint64_t *count) {
    int lv = skl(SK_SCAV);
    if (!lv || !count || !*count) return 0;
    uint64_t base = *count;
    *count = lv >= 2 ? base * 2 : (base * 3 + 1) / 2;
    // 2013 ss-respawn-intro (PS4 v1.11): level III's crafting drop is a two-flag toggle, not a dice roll - the 1st kill drops nothing ("NO SOUP FOR
    // YOU! setting guaranteed item to drop next time"), the 2nd drops an item (group 2), the 3rd nothing, the 4th an item (group 1): every OTHER kill
    int ing = 0;
    if (lv >= 3) {
        if (!g_sk_scav_f1) g_sk_scav_f1 = 1;
        else { ing = 1; g_sk_scav_f1 = 0; g_sk_scav_f2 ^= 1; }
    }
    if (lv >= 2) {
        dc_direct(SID_ADD_QTY, item, *count, 0);
        if (ing) dc_direct(SID_ADD_QTY, BOX_INGREDIENTS[t2r_rand() % (sizeof BOX_INGREDIENTS / sizeof *BOX_INGREDIENTS)].item, 4, 0);   // 4 units = one whole ingredient, like a box's
        char m[64]; snprintf(m, sizeof m, ing ? "Scavenger: ammo + supplies" : "Scavenger: ammo");
        msg_hook(m, 2.0f, 0, 0);
    }
    xlog("skill: Scavenger %d: ammo %llx x%llu -> x%llu (%s)%s", lv, (unsigned long long)item, (unsigned long long)base, (unsigned long long)*count,
         lv >= 2 ? "straight into the pack" : "pickup at the body", ing ? ", + crafting item" : "");
    return lv >= 2;
}
// Awareness: taking damage keeps enemy nametags up for 5 s (II: 10 s, and twice the distance)
static uint64_t g_sk_aware_until;
static uint64_t sk_aware_until(void) { return skl(SK_AWARE) ? g_sk_aware_until : 0; }
static float sk_tag_range(void) { return skl(SK_AWARE) >= 2 ? 60.0f : 30.0f; }
static void sk_aware(void) {
    static int last = -1;
    int lv = skl(SK_AWARE), hp = sk_hp();
    if (lv && last > 0 && hp > 0 && hp < last && !g_downed) {
        g_sk_aware_until = GetTickCount64() + (lv >= 2 ? 10000 : 5000);
        xlog("skill: Awareness %d: hit (%d -> %d), enemy nametags for %d s", lv, last, hp, lv >= 2 ? 10 : 5);
    }
    last = hp;
}
// a marked enemy gets the Lev-style outline when a Hawk-Eyed II+ player on our team marks (2013: the glow is the perk's; here for everyone)
static int sk_mark_glow_any(void) {
    if (skl(SK_HAWK) >= 2) return 1;
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) if (s != sk_me() && is_teammate(s) && sk_peer(s, SK_HAWK) >= 2) return 1;
    return 0;
}
static int sk_stun_immune(uint32_t from) { return skl(SK_BOMB) >= 3 && from == sk_me(); }
static float sk_smoke_fuse(uint32_t slot) {     // the fuse sk_bomb_fuse writes on the thrower's client (same table)
    static const float SMOKE[4] = {1.5f, 1.0f, 0.8f, 0.6f};
    return SMOKE[(slot == sk_me() ? skl(SK_BOMB) : sk_peer(slot, SK_BOMB)) & 3];
}
// Bomb Expert: the bullet settings' fuse (+0xb0, patched 1.5 s by dc_data_patches) is GLOBAL per client - writing it changed the
// fuse of every player's bomb copies on that client (observer copies burst early, "freed before the predicted burst"), so it is never written
// any more. Our OWN bombs are detonated early instead (t2r-ext.c ob_tick: engine explode at contact + sk_own_fuse) and the burst point is sent
// to everyone (MSG_BOMB_BURST / MSG_STUN_BURST), whose copies burst there.
static float sk_own_fuse(int pipe) {
    static const float SMOKE[4] = {1.5f, 1.0f, 0.8f, 0.6f}, PIPE[4] = {1.5f, 1.5f, 0.9f, 0.7f};
    int lv = skl(SK_BOMB); if (lv < 0) lv = 0; if (lv > 3) lv = 3;
    return (pipe ? PIPE : SMOKE)[lv];
}
static void sk_bomb_fuse(int on) { (void)on; }
// Fortitude downed crawl speed (2013 data f62b927e: x1.1 at I, x1.3 at II), on top of the mod's own downed crawl (DOWN_CRAWL_SPEED)
static float sk_prone_mul(void) { int a = skl(SK_AGILITY); return a >= 2 ? 1.19f : a >= 1 ? 1.15f : 1.0f; }
static float sk_fort_crawl(void) { int f = skl(SK_FORT); return f >= 2 ? 1.3f : f ? 1.1f : 1.0f; }
// Strategist III: "respawn on an ally ... several seconds early" (2013 can-respawn-on-ally has no number): 3 s off the respawn countdown
static float sk_respawn_early(void) { return skl(SK_STRAT) >= 3 ? 3.0f : 0.0f; }
// Second Chance II: 25 % cheaper armour per two deaths without a down / execution, stacking twice (50 % max); 2013 text
static int sk_armor_price(int cost) {
    int lv = skl(SK_SECOND);
    int stacks = g_sk_death_streak / 2 > 2 ? 2 : g_sk_death_streak / 2;
    if (lv < 2 || stacks <= 0) return cost;
    int c = cost * (100 - 25 * stacks) / 100;
    static int last = -1;
    if (last != c) { last = c; xlog("skills: Second Chance 2: armour %d -> %d parts (%d%% cheaper, %d deaths without a down / execution)", cost, c, 25 * stacks, g_sk_death_streak); }
    return c;
}
// ---- melee skills (re/melee-skills-1102.md). Every knob is read where the decision is made: the dodger's / escaper's / attacker's own machine for
// Counter Fighter, Iron Grip, Blade Discipline and Steady Stance; Heavy Hands is the DOWNER's skill, read by the victim (who owns the bleed-out clock and the
// lethal ruling) from the downer's synced skills (MSG_SK_SYNC -> sk_peer), as Covert Training / Reviver are.
static int g_sk_iron_used;        // Iron Grip II: this life's free escape is spent
static int sk_counter_window_ms(void) { return skl(SK_RIPOSTE) ? 850 : 600; }
static int sk_counter_cool_ms(void) { return skl(SK_RIPOSTE) >= 2 ? 1500 : 2200; }
static int sk_counter_late(void) { return skl(SK_RIPOSTE) >= 2; }
static int sk_iron_win_ms(int base) { return skl(SK_GRIP) ? base * 3 / 2 : base; }
static int sk_iron_free(const char *what) {
    if (skl(SK_GRIP) < 2 || g_sk_iron_used) return 0;
    g_sk_iron_used = 1;
    xlog("skills: IRON GRIP 2: the first %s on us this life is broken for free", what);
    msg_tag("IRON GRIP!", 2.0f, 0);
    return 1;
}
static int sk_blade_bonus(uint64_t w) { return skl(SK_BLADE) && w && w != ~0ULL && melee_class_of(w, NULL) == MC_1HBLADE ? 1 : 0; }
static int sk_heavy_melee_blow(uint64_t w) {     // fists or a blunt melee item (not a gun, a thrown item or a blade)
    if (!w || w == ~0ULL || base_weapon_is_gun(base_weapon(w)) || weapon_is_throwable(w)) return 0;
    return !melee_rule_blade(w);   // (night54: a female's knife = her fists)
}
static float sk_heavy_bleed_mul(void) {
    if (g_down_slot >= T2R_MAX_PEERS || sk_peer(g_down_slot, SK_HEAVY) < 1 || !sk_heavy_melee_blow(g_down_hit.w)) return 1.0f;
    xlog("skills: HEAVY HANDS %d: slot %u downed us with fists / a blunt weapon (%llx) - bleed-out x0.70", sk_peer(g_down_slot, SK_HEAVY), g_down_slot, (unsigned long long)g_down_hit.w);
    return 0.7f;
}
static int sk_heavy_lethal(uint64_t w) {
    static uint64_t logged;
    if (!g_sk_mf_vic || g_down_slot >= T2R_MAX_PEERS || sk_peer(g_down_slot, SK_HEAVY) < 2 || !w) return 0;
    const char *nm = NULL;
    if (fem_unarmed_id(w)) nm = "female knife = fists";
    else if (mhfx_class(w, NULL, &nm) != 1) return 0;
    if (logged != g_down_frame) { logged = g_down_frame; xlog("skills: HEAVY HANDS 2: slot %u finished us with a blunt weapon (%s) - killed at once (no revive)", g_down_slot, nm ? nm : "?"); }
    return 1;
}
static int sk_steady(void) { return skl(SK_STEADY); }
static float sk_footwork_mul(void) { int l = skl(SK_FOOTWORK); return l >= 3 ? 0.80f : l == 2 ? 0.85f : l == 1 ? 0.90f : 1.0f; }
// one line per equipped skill / level with what it does in this build (the playtest log shows every skill that is on)
static void sk_log_effects(void) {
    if (skl(SK_EXPLO)) xlog("skills: Explosion Expert %d: bomb / mine radius +%d%%, Molotov radius +%d%% (all ext rings, no engine upgrade)", skl(SK_EXPLO), 20 * skl(SK_EXPLO), 10 * skl(SK_EXPLO));
    if (skl(SK_REVIVER)) xlog("skills: Reviver %d: revive time x%.2f, revive hp +%d, parts +%d%%", skl(SK_REVIVER), sk_revive_time_mul(), 15 + 5 * skl(SK_REVIVER), skl(SK_REVIVER) >= 3 ? 50 : skl(SK_REVIVER) >= 2 ? 25 : 0);
    if (skl(SK_BRAWLER)) xlog("skills: Brawler %d: +10 hp per melee hit, melee-upgrade craft time (crafting speed upgrade while it crafts)%s", skl(SK_BRAWLER), skl(SK_BRAWLER) >= 2 ? ", extra one-tap on a modded weapon + durability" : "");
    if (skl(SK_EARS)) xlog("skills: Sharp Ears %d: listen drain x%.2f, recharge x%.2f%s", skl(SK_EARS), sk_listen_deplete_mul(), sk_listen_recharge_mul(), skl(SK_EARS) >= 3 ? ", faster listen mode (engine upgrades)" : "");
    if (skl(SK_STRAT)) xlog("skills: Strategist %d: marked warning%s%s", skl(SK_STRAT), skl(SK_STRAT) >= 2 ? ", radar 8 m" : "", skl(SK_STRAT) >= 3 ? ", respawn early near an ally" : "");
    if (skl(SK_HAWK)) xlog("skills: Hawk-eyed %d: marks +3 s%s%s", skl(SK_HAWK), skl(SK_HAWK) >= 2 ? ", marked enemies glow" : "", skl(SK_HAWK) >= 3 ? ", group mark (3 m)" : "");
    if (skl(SK_CRAFTER)) xlog("skills: Crafter %d: craft items faster (engine crafting-speed upgrade)%s", skl(SK_CRAFTER), skl(SK_CRAFTER) >= 2 ? "; gift box to a teammate every 3 / 2 crafts" : "");
    if (skl(SK_FAT)) xlog("skills: First-Aid Training %d: health kits x1.75 (kit use rate global + upgrade switch)%s", skl(SK_FAT), skl(SK_FAT) >= 2 ? ", hold E on a hurt teammate" : "");
    if (skl(SK_SHARP)) xlog("skills: Sharpshooter %d: wobble x%g (x%g base x 2013 factor %g), sway x%g (exact 2013)%s", skl(SK_SHARP), (double)(SK_WOB_BASE * (skl(SK_SHARP) >= 3 ? 0.45 : skl(SK_SHARP) == 2 ? 0.65 : 0.8)), (double)SK_WOB_BASE, skl(SK_SHARP) >= 3 ? 0.45 : skl(SK_SHARP) == 2 ? 0.65 : 0.8, skl(SK_SHARP) >= 3 ? 0.5 : skl(SK_SHARP) == 2 ? 0.625 : 0.75, skl(SK_SHARP) >= 2 ? ", headshot heal" : "");
    if (skl(SK_MARATHON)) xlog("skills: Marathon Runner %d: sprint meter drains /%.2f, refills x%.2f (7 s base)", skl(SK_MARATHON), skl(SK_MARATHON) >= 2 ? 1.30f : 1.15f, skl(SK_MARATHON) >= 2 ? 1.30f : 1.15f);
    if (skl(SK_COLLECTOR)) xlog("skills: Collector %d: +%d%% parts", skl(SK_COLLECTOR), 10 * skl(SK_COLLECTOR));
    if (skl(SK_DMGMARK)) xlog("skills: Damage Marker %d: any damage marks%s", skl(SK_DMGMARK), skl(SK_DMGMARK) >= 2 ? ", mark parts x2" : "");
    if (skl(SK_GUNSLINGER)) xlog("skills: Gunslinger %d: pistol ammo %s at spawn", skl(SK_GUNSLINGER), skl(SK_GUNSLINGER) >= 2 ? "doubled" : "+50%");
    if (skl(SK_AGILITY)) xlog("skills: Agility %d: crouch x%.2f%s", skl(SK_AGILITY), skl(SK_AGILITY) >= 2 ? 1.19f : 1.15f, skl(SK_AGILITY) >= 2 ? ", walk x1.09, climb x1.35 (perk 0x08 constant), quiet footsteps for observers" : "");
    if (skl(SK_SECOND)) xlog("skills: Second Chance %d: health kit below 50%% hp, ammo +%d%% per 2 deaths%s", skl(SK_SECOND), skl(SK_SECOND) >= 2 ? 50 : 25, skl(SK_SECOND) >= 2 ? ", armour 25% cheaper per 2 deaths" : "");
    if (skl(SK_FOOTWORK)) xlog("skills: Boxer's Footwork %d: dodge cooldown x%.2f", skl(SK_FOOTWORK), (double)sk_footwork_mul());
    if (skl(SK_LETHAL)) xlog("skills: Lethal Efficiency %d: neck-snap instead of special executions (I: fast 1.633 s clip, II: fastest 0.8 s clip)", skl(SK_LETHAL));
    if (skl(SK_LONEWOLF)) xlog("skills: Lone Wolf %d: when alone Agility 2 / Sharpshooter 2%s, +%d parts per down / execution", skl(SK_LONEWOLF), skl(SK_LONEWOLF) >= 2 ? " / Sharp Ears 3" : "", 30 * skl(SK_LONEWOLF));
    if (skl(SK_RIPOSTE)) xlog("skills: COUNTER FIGHTER %d: dodge counter window %d ms%s, cooldown %d ms", skl(SK_RIPOSTE), sk_counter_window_ms(), sk_counter_late() ? ", also after a late dodge" : "", sk_counter_cool_ms());
    if (skl(SK_GRIP)) xlog("skills: IRON GRIP %d: pin escape window x1.5 (press window %d ms), pull-out counter window %d ms%s", skl(SK_GRIP), sk_iron_win_ms(360), sk_iron_win_ms(400), skl(SK_GRIP) >= 2 ? ", the first pin / pull-out per life is broken for free" : "");
    if (skl(SK_BLADE)) xlog("skills: BLADE DISCIPLINE %d: 1h blade weapons last 1 hit longer%s", skl(SK_BLADE), skl(SK_BLADE) >= 2 ? ", a spawn hatchet (3 uses) when no melee weapon is carried" : "");
    if (skl(SK_HEAVY)) xlog("skills: HEAVY HANDS %d: our fist / blunt downs bleed out 30%% faster (victim reads our synced level)%s", skl(SK_HEAVY), skl(SK_HEAVY) >= 2 ? "; a blunt weapon finisher kills outright" : "");
    if (skl(SK_STEADY)) xlog("skills: STEADY STANCE %d: no 300 ms recovery block after a parried blow%s", skl(SK_STEADY), skl(SK_STEADY) >= 2 ? "; weapon takedowns cost no durability" : "");
    if (skl(SK_JACK)) xlog("skills: Jack of All Trades %d: contains the lower levels of its skills (see above)", skl(SK_JACK));
    if (skl(SK_ZOOM)) xlog("skills: Pistol Auto-Zoom %d: FOV x%.2f while aiming a pistol (override-fov)", skl(SK_ZOOM), skl(SK_ZOOM) >= 2 ? 1.5f : 1.25f);
}
static void skills_on_spawn(void) {
    sk_compute();
    sk_log_effects();
    gift_on_spawn();
    g_sk_life_kit = 0; g_sk_iron_used = 0; g_sk_mf_vic = 0; g_sk_blade_kit_dur = 0;
    if (skl(SK_BLADE) >= 2 && !g_keep_melee && !g_machete_owned && g_give_orig) {      // Blade Discipline II: a hatchet when we carry no melee weapon (keep_restore ran before this)
        uint64_t none = 0, hw = dc_call(SID_HAS_MELEE_WEAPON, 0, &none);
        if (hw == ~0ULL || !(hw & 0xff)) {
            uint64_t ret[64] = {0}, give[4] = {fnv64("hatchet"), 0, 0, 1};
            g_sk_blade_kit_dur = 3;
            g_give_orig(ret, 4, give, 0);
            xlog("skills: BLADE DISCIPLINE 2: hatchet at spawn (3 uses; durability set when the pickup is processed)");
        }
    }
    if (skl(SK_COVERT) && !inv_qty(fnv64("shiv"))) {
        // a shiv is a weapon-type item (KEEP_UTIL gives it with give-item + set-total), a bare inventory add is not enough: give it the
        // way keep_restore gives the kept stock, then only top the count up if that did not take
        uint64_t sh = fnv64("shiv");
        if (g_give_orig) { uint64_t ret[64] = {0}, give[4] = {sh, 0, 0, 1}; g_give_orig(ret, 4, give, 0); }
        if (!inv_qty(sh)) dc_direct(SID_ADD_QTY, sh, 1, 0);
        if (!inv_qty(sh) && g_settotal_orig) { uint64_t ret[64] = {0}, set[2] = {sh, 1}; g_settotal_orig(ret, 2, set, 0); }
        xlog("skills: Covert Training %d: shiv at spawn (now %llu)", skl(SK_COVERT), (unsigned long long)inv_qty(sh));
    }
    if (skl(SK_COVERT) >= 2) xlog("skills: Covert Training %d: hidden from enemy listen mode while crouch walking (synced to peers)", skl(SK_COVERT));
    if (skl(SK_COVERT) >= 3) xlog("skills: Covert Training 3: cannot be marked (peers refuse our slot in sk_mark / sk_marks)");
    if (skl(SK_FORT) >= 2 && !inv_qty(0xa4311670996f650aULL)) { dc_direct(SID_ADD_QTY, 0xa4311670996f650aULL, 1, 0); xlog("skills: Fortitude 2: starting health kit (now %llu)", (unsigned long long)inv_qty(0xa4311670996f650aULL)); }
    if (skl(SK_FORT)) xlog("skills: Fortitude %d: bleed-out x%.2f, crawl x%.2f, downed hp x%.2f, %d parts after a big heal", skl(SK_FORT), skl(SK_FORT) >= 2 ? 1.5f : 1.25f, sk_fort_crawl(), skl(SK_FORT) >= 2 ? 1.38f : 1.0f, skl(SK_FORT) >= 2 ? 50 : 25);
    if (skl(SK_STRAT) >= 3) xlog("skills: Strategist 3: respawn %.0f s early, on the safest living ally (host picks the spawner near him)", sk_respawn_early());
    // Gunslinger: pistol ammo +50% / x2 of the 2013 starting ammo (9mm 10, Revolver 7, Enforcer 8);
    // Second Chance: +25% / +50% reserve per 2 deaths without a down (max twice)
    int stacks = g_sk_death_streak / 2 > 2 ? 2 : g_sk_death_streak / 2;
    float sc = skl(SK_SECOND) ? stacks * (skl(SK_SECOND) >= 2 ? 0.50f : 0.25f) : 0.0f;
    for (int i = 0; i < g_spawn_ngun && g_settotal_orig; i++) {
        uint64_t gun = base_weapon(g_spawn_guns[i]);
        const char *ammo = NULL; int start = 0;
        if (gun == fnv64("pistol-beretta") || gun == fnv64("pistol-glock")) { ammo = "ammo-pistol"; start = 10; }
        else if (gun == fnv64("pistol-cz75-enforcer")) { ammo = "ammo-pistol"; start = 8; }
        else if (gun == fnv64("pistol-revolver-taurus") || gun == fnv64("pistol-snubby")) { ammo = "ammo-revolver"; start = 7; }
        int extra = 0;
        if (ammo && skl(SK_GUNSLINGER)) extra += skl(SK_GUNSLINGER) >= 2 ? start : (start + 1) / 2;
        const char *any = ammo;
        for (size_t k = 0; !any && k < sizeof SPAWN_AMMO / sizeof *SPAWN_AMMO; k++) {
            uint64_t g = SPAWN_AMMO[k].gun ? SPAWN_AMMO[k].gun : fnv64(SPAWN_AMMO[k].name);
            if (g == gun) any = SPAWN_AMMO[k].ammo;
        }
        // the base reserve the spawn asked for, not the readback: set-total-ammo has not taken yet (spawn_res_want, t2r-ext.c)
        int have = any ? spawn_res_of(fnv64(any), (int)inv_qty(fnv64(any))) : 0;
        if (any && sc > 0) extra += (int)__builtin_ceilf(have * sc);
        if (!any || extra <= 0) continue;
        uint64_t args[2] = {fnv64(any), (uint64_t)(have + extra)};
        uint64_t ret[64] = {0};
        g_settotal_orig(ret, 2, args, 0);
        spawn_res_want(fnv64(any), have + extra);
        xlog("skills: spawn %s reserve %d -> %d (Gunslinger %d, Second Chance x%.2f)", any, have, have + extra, skl(SK_GUNSLINGER), sc);
    }
    if (skl(SK_LUCKY)) xlog("skill: Lucky Break %d (2013 box modifiers)", skl(SK_LUCKY));
    if (skl(SK_SCAV)) xlog("skill: Scavenger %d (2013: kill ammo drops x1.5 / x2, II+ auto pickup, III crafting item)", skl(SK_SCAV));
    if (skl(SK_AWARE)) xlog("skill: Awareness %d (nametags %.0f m, on hit %d s)", skl(SK_AWARE), sk_tag_range(), skl(SK_AWARE) >= 2 ? 10 : 5);
    sk_bomb_fuse(1);       // every spawn (also without the skill: a loadout that dropped Bomb Expert gets the 1.5 s fuses back)
    if (skl(SK_BOMB)) xlog("skill: Bomb Expert %d applied", skl(SK_BOMB));
    if (skl(SK_EXEC)) xlog("skill: Executioner %d (shiv kills double parts)", skl(SK_EXEC));
    cp_on_spawn();         // clan supply perks (clanperk.c): after the spawn routine, the kept stock and the spawn ammo above
    if (match_substate() == 6) sk_sync_send();
}
static int sk_fort_hp(int base) { return skl(SK_FORT) >= 2 ? (int)(base * 1.38f + 0.5f) : base; }     // 2013 health-when-downed-mult 1.38 at level 2
static float sk_fort_mul(void) { int f = skl(SK_FORT); if (!f) return cp_bleed_mul(); xlog("skills: Fortitude %d - longer bleed-out", f); return f >= 2 ? 1.5f : 1.25f; }
static void skills_tick(void) {
    static int installed, was_match;
    if (!installed) { installed = 1; sk_install(); }
    uint8_t *o = lobby_obj();
    // old winmm skill bits in the lobby loadout -> 2013 skills (saved by feature_loadout_persist)
    for (int s = 0; o && s < 3; s++) {
        uint64_t *k = (uint64_t *)(o + 0x2c78 + 8 * s);
        if (*k && !(*k & SK_TAG)) { uint64_t n = sk_norm(*k); xlog("skills: row %d old skill %llx -> %llx", s + 1, (unsigned long long)*k, (unsigned long long)n); *k = n; }
    }
    sk_compute();
    // winmm's perk mask (its perk code: FUN_18001d6b0 every frame) from the 2013 skills
    if (o) *(uint64_t *)(o + 0x2c70) = (skl(SK_AGILITY) >= 2 ? 0x08 : 0);   // (Sharpshooter's old 0x20 'no sway' perk is gone: exact 2013 sway / wobble numbers, sk_aim_values)
    uint64_t top = top_page();
    if (match_substate() != 6) {
        if (g_lo_state == LO_EDIT && top == PAGE_ROOT) sk_edit_page();
        if (was_match) { sk_upgrades(0); sk_bomb_fuse(0); g_sk_aware_until = 0; memset(g_peer_sk_frame, 0, sizeof g_peer_sk_frame); memset(g_peer_agi, 0, sizeof g_peer_agi); memset(g_peer_covert2, 0, sizeof g_peer_covert2); g_sk_death_streak = 0; g_sk_craft_n = 0; g_sk_brawler_craft = 0; g_sk_fort_kits = -1; sk_movement_reset(); sk_sprint_meter_reset(); }
        was_match = 0;
        return;
    }
    if (!was_match) { was_match = 1; g_sk_death_streak = 0; if (sd_off()) xlog("slowdown: off (t2r-no-slowdown.txt)"); else xlog("slowdown: move x%.2f, %d systems calmed", (double)SD_MOVE, SD_SYSTEMS + 1); }
    if (!(g_frame % 30)) { sk_alone(); sk_upgrades(1); sk_bomb_fuse(1); }
    if (!(g_frame % 120)) { static uint64_t sk_last; if (bw_every_ms(&sk_last, GetTickCount64(), 1900)) sk_sync_send(); }   // wall clock (bwpolicy.h): every 120 frames was 0.83 s at 144 fps
    sk_aware();
    sk_marks();
    sk_radar();
    fat_healed_poll();
    gift_tick();
    dpadlog_tick();
    sk_fat_heal();
    sk_stats();
    sk_second_chance();
    sk_craft_hooks();
    craft_keep_hook();
    sk_movement();
    sk_sprint_meter(sk_hp() > 0);
    sk_pistol_zoom(sk_hp() > 0);
    sk_fort_parts();
    sk_heal_self_award();
    sk_kit_cap();
    cp_tick();
    if (g_sk_revived_by < 4 && g_t60 - g_sk_revived_at >= 3 && !g_downed) {   // Reviver: more health
        // 2013 (rules/revive.md, net-revive-settings +0x0c + booster 0x798cc53b): revive hp 40 + 20/25/30 hp = 60/65/70.
        // (older build: 48/50/52, "+20/25/30 %"; t2r-2013-revive-off.txt brings it back)
        static const int HP[4] = {0, 60, 65, 70}, HP_OLD[4] = {0, 48, 50, 52};
        int lv = sk_peer(g_sk_revived_by, SK_REVIVER);
        if (lv) { sk_hp_set(r13_off(R13_REVIVE) ? HP_OLD[lv & 3] : HP[lv & 3]); xlog("skills: revived by a Reviver %d -> hp %d", lv, HP[lv & 3]); }
        g_sk_revived_by = 99;
    } else if (g_sk_revived_by < 4 && g_t60 - g_sk_revived_at > 180) g_sk_revived_by = 99;     // the revive never took (ignored / we died)
}
