// supplybox.c - 2013 Factions supply box ("loot crate") contents for t2r-ext. Included from
// t2r-ext.c after skills.c. Everything a box gives comes from here (the F2013 supply
// box drop logic is used for everything); winmm's fill FUN_18004ce10 only provides the slots / xforms.
//
// Source: TLOU1 PS3 patch1.psarc bin/dc1/ss-multiplayer/ss-region-spawn-pickups.bin (the per-player
// box script, t1final/t1/src/game/script/ss-multiplayer/ss-region-spawn-pickups.dc), disassembled
// with tools/re/t1lam.py; ss-region-respawn-check.bin (refill timer); net-10.bin *net-item-pickups* /
// *net-player-recipes* (pickup sizes, recipe costs). Tables + how they were read: re/supply-box-2013.md.
//
// One box opening (2013 spawn-items state, in source order):
//   1. crafting: first-life starter groups, else the recipe-gap rule, topped up to 3 random items,
//      then capped by the "final group choice" (KDR tier +/- team situation) and Lucky Break;
//   2. health: health kit at <= 50 % health, food cans (+20 hp each) up to 90 %;
//   3. comeback item (bomb / molotov) and comeback melee when losing badly;
//   4. melee: from the match start, one in every box while none is held (2013's 2x4 rule: t2r-box-melee-2013.txt);
//   5. ammo: tops each carried gun up to an ideal count (by KDR), in big + small pickups.
// Skills: the only 2013 skill the box script reads is the 'supply box skill' booster = Lucky
// Break (pistol ammo x1.5, II/III long ammo x1.25 + higher crafting caps). Skills hand their
// numbers in through box_mods_t (sk_box_mods in skills.c), filled at every box.
// Box contents are per player (2013 too: the script runs per local player).

#define B13_MAX 24
#define B13_MELEE_GAP 4         // wiki: since 1.09 melee weapons are rarer from supply boxes; the pool rule alone gave 2 in 18 boxes (old rule only: t2r-box-melee-2013.txt)
#define B13_MELEE_NONE_PCT 60   // if you hold none it is still only a chance (was guaranteed)
#define B13_MELEE_SPARE_PCT 30  // A box opened while a melee weapon is held still gives another one (swap / other type) this often
typedef struct {
    uint64_t item;
    uint32_t count;
    uint8_t kind;
} B13EntryT;
enum { B13_K_ING, B13_K_AMMO, B13_K_KIT, B13_K_MELEE, B13_K_THROW };    // box_spawn: 3 = melee
// lms.c (2013 Last One Standing): the uber-box flag and the team / late-join adjustments of the final group choice
static int g_lmsBox;           // the next box we open is the Last Man Standing box (set by lms.c once per match)
static int g_b13Uber;          // filling that box now
static int LmsFgcAdjust(int *team_out, int *late_out);
static void LmsLateFgcUsed(void);
static int LmsLateApply(int fgc, int late);

// ---- data (2013 values; TLOU2 items they map to)
// Ammo: 2013 per-weapon pickup sizes (ammo-count-per-box / -small) and ideal reserve
// (ideal-ammo-count) for KDR <= 0.75 / <= 2.5 / > 2.5. 'pistol' = 2013 pistol slot (x1.5 Lucky Break).
static const struct {
    const char *gun;
    uint8_t pistol, big, small, ideal[3];
    const char *t1;
} B13_AMMO[] = {
    { "pistol-beretta", 1, 6, 3, { 10, 8, 5}, "pistol-9mm" },
    { "pistol-glock", 1, 8, 3, { 13, 10, 7}, "pistol-burst" },
    { "pistol-revolver-taurus", 1, 5, 2, { 9, 6, 4}, "pistol-revolver" },
    { "pistol-cz75-enforcer", 1, 5, 2, { 9, 6, 4}, "deagle (Enforcer)" },
    { "shotgun-shorty", 1, 4, 2, { 5, 4, 2}, "pistol-shotgun (Shorty)" },
    { "rifle-remington-bolt", 0, 3, 2, { 7, 4, 3}, "rifle-bolt" },
    { "rifle-vepr", 0, 5, 2, { 10, 7, 5}, "variable-rifle" },
    { "rifle-m14-semi", 0, 4, 2, { 8, 6, 4},
     "assault-rifle-semi-automatic" },
    { "rifle-burst", 0, 8, 3, { 13, 10, 7},
     "rifle-improvised-assault (Burst Rifle)" },
    { "rifle-mpx5", 0, 8, 3, { 13, 10, 7}, "ak-74 (Full-Auto)" },
    { ASSAULT_ID, 0, 3, 3, { 6, 5, 4},
     "Assault Rifle (was missing = no kill drops; 'boxes should not give that much ammo': 3 per pickup, box tops up to 4 at most)"
     },
    { "shotgun-baserri-over-under", 0, 4, 2, { 6, 4, 2},
     "shotgun-hunting" },
    { "bow-ellie", 0, 3, 2, { 4, 3, 2}, "bow" },
    { "crossbow", 0, 3, 2, { 4, 3, 2},
     "crossbow (net10 94b0435b has a def + ammo item, max carried 15; the box pickup sizes / ideal reserve are not in the data: the bow's row)"
     },
    { "shotgun-tactical", 0, 4, 2, { 6, 4, 2},
     "tactical shotgun (box ammo like the hunting shotgun)" },
    { "rifle-frontier", 0, 4, 2, { 10, 6, 4},
     "rifle-short (Frontier; reserve cap 15; pickup sizes not in the data: between the hunting rifle and the M14)"
     },
    // not assessed in 2013 (power weapons / purchasables): El Diablo, pump + tactical shotgun
};

// Recipe gaps: holding none of the crafted item and none of an ingredient -> the box spawns one
// missing ingredient (both missing: a coin flip), and with 'all_pct' % every missing one.
// 2013 checked molotov = alcohol + rag, smoke = sugar + explosive, bomb = explosive + blade,
// shiv = binding + blade; the bomb row uses OUR pipe bomb's ingredients (it is what a player crafts here).
static const struct {
    const char *item;
    uint64_t ing[3];
    uint8_t all_pct;
} B13_GAPS[] = {
    { "molotov", { ING_ALCOHOL, ING_RAG, 0}, 0 },
    { "smoke-bomb", { ING_EXPLOSIVE, ING_CANISTER, 0}, 10 },
    { "pipe-bomb", { ING_EXPLOSIVE, ING_CANISTER, ING_ALCOHOL}, 10 },
    { "shiv", { ING_BINDING, ING_BLADE, 0}, 10 },
    // A player was not getting rags, medkits or snacks despite being low constantly: rags only ever came from the
    // molotov gap, so a hurt player holding a molotov never got one. Not a 2013 row: only while we are hurt (B13_GAP_HURT).
    { "health-kit", { ING_ALCOHOL, ING_RAG, 0}, 10 },
};

#define B13_GAP_HURT "health-kit"
// First-life starter groups (Supply Raid item-group-1/2/3-tdm): boxes 1-3 opened before the
// player's first death, team not outnumbered. 2013 sugar = our canister, 2013 container = rag.
static const uint64_t B13_START[3][4] = {
    { ING_CANISTER, ING_ALCOHOL, ING_EXPLOSIVE, ING_BLADE },
    { ING_CANISTER, ING_BINDING, ING_BINDING, ING_BLADE },
    { ING_EXPLOSIVE, ING_EXPLOSIVE, ING_RAG, 0 },
};

#define B13_HEAL_CAN 20         // pickup-health-large-1 = *net-item-pickups* health-large: 20 hp

// ---- state (per match, local player)
static struct {
    B13EntryT e[B13_MAX];
    int n, next;
    uint64_t box;
    int from_open;
    int opened;                 // boxes this player opened this match (regions-visited)
    int visits_cb;              // ... opened while doing badly (regions-visited-cb)
    int last_melee_box;         // box number of the last 2x4 (rarer melee since 2013's 1.09: at least B13_MELEE_GAP boxes apart)
    int n2x4, initial2x4, next_pool;    // 2x4 count, first one spawned, pool threshold for the next
    const char *last_pick;      // the last melee weapon a box gave (the next pick avoids it)
    int lives_start;
    int cb_item_life, cb_wpn_life;      // comeback item / weapon given this life
    int32_t deaths_seen;
    int dirty;
} g_b13 = {.dirty = 1 };

// g_b13_stock (t2r-box-stock.txt: winmm's own fill, no 2013 plan) lives in t2r-ext.c
static BoxModsT g_boxMods;

static void B13Add(uint64_t item, uint32_t count, int kind)
{
    if (g_b13.n < B13_MAX && item && count)
        g_b13.e[g_b13.n++] = (B13EntryT) {
        item, count, (uint8_t) kind};
}

// Units: TLOU2's crafting-system.bin costs every ingredient 4 per recipe (shared amount array @0x17d8: 4). 2013
// recipes cost 4 too, but its box pickups give a whole ingredient (the HUD unit; the box's crafting-item-*-1
// entities). One 2013 box ingredient = one recipe's worth here = 4 units, and the 2013 "has none of it" tests
// read "less than one recipe's worth" (boxes gave 1-4 units and nothing was craftable).
#define B13_ING_UNITS 4
static uint32_t B13IngSize(uint64_t ing)
{
    (void) ing;
    return B13_ING_UNITS;
}

static const char *B13Name(uint64_t item)
{
    static const char *const N[] =
        { "alcohol", "rag", "canister", "explosive", "binding", "blade",
"health-kit",
        "pipe-bomb", "molotov", "smoke-bomb", "shiv", "crowbar", "machete",
            "tomahawk", "hatchet", "two-by-four", "baseball-bat",
        "lead-pipe", "pipe-wrench", "hammer-modern", "hammer-scar",
            "club-1h-police", "club-1h-scar", "health-small",
            "ammo-pistol", "ammo-revolver", "ammo-revolver-joel",
            "ammo-rifle", "ammo-rifle-vepr", "ammo-rifle-mpx5",
            "ammo-arrow",
        "ammo-crossbow", "ammo-shotgun", "ammo-shotgun-baserri", "parts"
    };
    for (size_t i = 0; i < sizeof N / sizeof *N; i++)
        if (FNV64(N[i]) == item)
            return N[i];
    return "?";
}

static uint64_t B13RandomIng(void)
{                               // 2013 group-item-type: a random crafting item
    return BOX_INGREDIENTS[T2rRand() %
                           (sizeof BOX_INGREDIENTS /
                            sizeof *BOX_INGREDIENTS)].item;
}

static int B13Lives(void)
{
    typedef int (*fn_lives)(void *);
    uint8_t *lo = LobbyObj();
    int l = lo ? ((fn_lives) T2R_SYM(kFnTeamLives)) (lo) : -1000;
    return l > -1000 ? l : -1000;
}

// 2013 kill-to-death-ratio: score = 0.25 x kills (executions) + 0.75 x downs; with 0 or 1 deaths
// max(score, 1); 2 deaths and score <= 1: 0.75 (Supply Raid); else score / deaths.
static float B13Kdr(int32_t *deaths_out)
{
    uint32_t me = SkMe();
    if (me >= T2R_ENGINE_PEERS) {
        *deaths_out = 0;
        return 1.0f;
    }
    int32_t downs = *(int32_t *) (SB_STATS + me * 12), deaths =
        *(int32_t *) (SB_STATS + me * 12 + 4);
    float score =
        0.25f * (float) g_sbExec[me * 3] + 0.75f * (float) downs;
    *deaths_out = deaths;
    if (deaths <= 1)
        return score <= 1.0f ? 1.0f : score;
    if (score <= 1.0f && deaths == 2)
        return 0.75f;
    return score / (float) deaths;
}

static int B13TeamDiff(void)
{                               // enemy players - our players (spawned, present)
    int ally = 1, enemy = 0;
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        uint8_t *p = SkPeerPtr(s);
        if (s == SkMe() || !p || !p[4])
            continue;
        if (IsTeammate(s))
            ally++;
        else
            enemy++;
    }
    return enemy - ally;
}

static void B13Tick(void)
{
    if (MatchSubstate() != 6) {
        g_b13.dirty = 1;
        return;
    }
    if (g_b13.dirty) {
        g_b13.dirty = 0;
        g_b13.opened = g_b13.visits_cb = g_b13.n2x4 = g_b13.initial2x4 =
            g_b13.last_melee_box = 0;
        g_b13.last_pick = NULL;
        g_b13.next_pool = 36;   // 2013: first 2x4 once both teams used 4 of 40 lives
        g_b13.lives_start = B13Lives();
        g_b13.cb_item_life = g_b13.cb_wpn_life = 0;
        g_b13.deaths_seen = -1;
    }
    int32_t d;
    B13Kdr(&d);
    if (g_b13.deaths_seen >= 0 && d > g_b13.deaths_seen)
        g_b13.cb_item_life = g_b13.cb_wpn_life = 0;
    g_b13.deaths_seen = d;
}

// both teams' remaining lives, estimated from ours (2013 compared the sum of the two respawn pools)
static int B13PoolSum(void)
{
    int now = B13Lives();
    if (now <= -1000 || g_b13.lives_start <= 0)
        return 0;
    return 40 * (now < 0 ? 0 : now) / g_b13.lives_start;
}

// ---- 1. crafting
static void B13Crafting(int fgc, int first_life_box)
{
    int lb = g_boxMods.lucky - 1;      // 2013 booster level: -1 none, 0..2 = I..III
    if (first_life_box >= 1 && first_life_box <= 3) {
        for (int i = 0; i < 4; i++)
            if (B13_START[first_life_box - 1][i]) {
                uint64_t ing = B13_START[first_life_box - 1][i];
                B13Add(ing, B13IngSize(ing), B13_K_ING);
            }
        Xlog("supply box 2013: starter group %d (first life, %d item(s))",
             first_life_box, g_b13.n);
        return;
    }
    int start = g_b13.n;
    for (size_t r = 0; r < sizeof B13_GAPS / sizeof *B13_GAPS; r++) {
        if (InvQty(FNV64(B13_GAPS[r].item)))
            continue;
        if (!strcmp(B13_GAPS[r].item, B13_GAP_HURT)
            && SkHp() >= SkHpMax())
            continue;
        uint64_t miss[3];
        int nm = 0;
        for (int i = 0; i < 3 && B13_GAPS[r].ing[i]; i++)
            if (InvQty(B13_GAPS[r].ing[i]) < B13_ING_UNITS)
                miss[nm++] = B13_GAPS[r].ing[i];
        if (!nm)
            continue;
        if ((int) (T2rRand() % 100) < (int) B13_GAPS[r].all_pct) {
            for (int i = 0; i < 3 && B13_GAPS[r].ing[i]; i++)
                B13Add(B13_GAPS[r].ing[i],
                        B13IngSize(B13_GAPS[r].ing[i]), B13_K_ING);
        } else {
            uint64_t ing = miss[T2rRand() % nm];
            B13Add(ing, B13IngSize(ing), B13_K_ING);
        }
        Xlog("supply box 2013: %s gap -> %d missing ingredient(s)",
             B13_GAPS[r].item, nm);
    }
    while (g_b13.n - start < 3) {
        uint64_t ing = B13RandomIng();
        B13Add(ing, B13IngSize(ing), B13_K_ING);
    }
    // reduce-items caps (Supply Raid); Lucky Break II/III raise them (2013 item-bonus)
    int lo = 3, hi = 4, extra = 0, parts = 0, bonus = 0;
    if (fgc == 0) {
        lo = 3;
        hi = 4;
    } else if (fgc <= -2) {
        extra = 2;
        parts = 1;
    } else if (fgc == -1) {
        lo = hi = 3;
    } else if (fgc >= 2) {
        if ((int) (T2rRand() % 100) < 44) {
            lo = hi = lb == 2 ? 3 : 2;
            bonus = lb == 2;
        } else {
            lo = hi = lb == 1 ? 2 : lb == 2 ? 3 : 1;
            bonus = lb >= 1;
        }
    } else {
        lo = hi = 3;
    }                           // fgc == 1
    lo += g_boxMods.craft_bonus;
    hi += g_boxMods.craft_bonus;
    int precap = g_b13.n - start;
    int cap = lo + (hi > lo ? (int) (T2rRand() % (hi - lo + 1)) : 0);
    while (g_b13.n - start > cap) {     // kill random items until the cap
        int k = start + (int) (T2rRand() % (g_b13.n - start));
        g_b13.e[k] = g_b13.e[--g_b13.n];
    }
    for (int i = 0; i < extra; i++) {
        uint64_t ing = B13RandomIng();
        B13Add(ing, B13IngSize(ing), B13_K_ING);
    }
    // Per supplies.md and the 2013 box script, a box gives NO supplies item and no satchel. Its only clan income is the parts pickup
    // pickup-parts-mp-medium-1 = 100 PARTS, and only for a player whose rating is fgc <= -2 (losing). Parts are parts in the match; the
    // post-match day turns them into supplies on its own (2013 ProcessClanDay: bottles + parts / 50).
    if (parts && g_b13.from_open)
        Award(100, SkMe(), "Supplies");        // (2013 pickup text of pickup-parts-mp-medium-1)
    if (bonus && g_b13.from_open)
        MsgHook("Lucky Break: crafting bonus", 2.5f, 0, 0);
    Xlog("supply box 2013: crafting %d item(s) (from %d, fgc %d, cap %d-%d -> %d, +%d random%s, lucky %d)", g_b13.n - start, precap, fgc, lo, hi, cap, extra, parts ? ", +100 parts" : "", g_boxMods.lucky);
}

// ---- 2. health
static int b13_food;            // food cans of this box (healed on open)
static void B13Health(void)
{
    int hp = SkHp(), mx = SkHpMax();
    b13_food = 0;
    if (hp <= 0 || mx <= 0 || hp >= mx) {
        if (hp >= mx && mx > 0)
            Xlog("supply box 2013: health %d/%d (full) -> nothing", hp,
                 mx);
        return;
    }
    float pct = (float) hp / (float) mx + g_boxMods.health_pct_bias;
    // the snack bars (inventory health-small, level actor health-pickup-base) replace the low-health medkit; the medkit still
    // drops, rarely (15 %); never at full health.
    // Every snack bar pickup was REFUSED by spawn-item-pickup ("-> pickup handle 0", the MP level has no
    // spawnable snack object), so a box at <= 50 % gave nothing 85 % of the time. Back to the 2013 rule (a health kit at <= 50 %) until a
    // snack spawn is proven; t2r-box-snacks.txt = the snack bars again (testing).
    static int nosnack = -1;
    if (nosnack < 0) {
        FILE *f = ExeSideOpen("t2r-box-snacks.txt", "r");
        nosnack = f == NULL;
        if (f)
            fclose(f);
    }
    const char *what = "nothing";
    if (pct <= 0.5f) {
        if (nosnack || T2rRand() % 100 < 15) {
            B13Add(FNV64("health-kit"), 1, B13_K_KIT);
            what = "health kit";
        } else {
            B13Add(FNV64("health-small"), 1, B13_K_KIT);
            B13Add(FNV64("health-small"), 1, B13_K_KIT);
            what = "2 snack bars";
        }
    } else if (pct < 0.8f) {
        b13_food = 2;
        what = "2 food cans";
    } else if (pct < 0.9f) {
        b13_food = 1;
        what = "1 food can";
    }
    Xlog("supply box 2013: health %d/%d -> %s", hp, mx, what);
}

// ---- 3. comeback item / weapon, 4. melee (2x4)
// The 2013 rules only decide WHEN a melee weapon drops (its "2x4"); WHICH one is any of MELEE_DROPS at random
// (all melee weapons keep dropping, not just the 2x4).
// Variety: the style first (1h blunt / 1h blade = the tomahawk hatchet / 2h blunt, a third each - a uniform pick over MELEE_DROPS gave the
// six 1h blunt clubs 60 % and the tomahawk 10 %), then a weapon of that style's pool (MELEE_DROP_POOL), never the same weapon twice in a row.
static const char *B13MeleeName(void)
{
    static unsigned next;
    static const char *const STYLE[3] =
        { "1h blunt", "1h blade", "2h blunt" };
    const size_t n = sizeof MELEE_DROPS / sizeof *MELEE_DROPS;
    const char *pick = NULL;
    int style = -1;
    if (g_meleeAlways)
        pick = MELEE_DROPS[next++ % n];
    else
        for (int tries = 0;
             tries < 4 && (!pick || pick == g_b13.last_pick); tries++) {
            // blades now kill outright (t2r-ext.c melee_lethal_down_now), so sharp weapons are rarer: 1h blade 12 %, 1h blunt 50 %, 2h blunt 38 %
            // (was a third each; t2r-blade-common.txt = the old thirds)
            {
                static int old = -1;
                if (old < 0) {
                    old =
                        GetFileAttributesA("t2r-blade-common.txt") !=
                        INVALID_FILE_ATTRIBUTES;
                }
                unsigned r = T2rRand() % 100;
                style = old ? (int) (r % 3) : r < 12 ? 1 : r < 62 ? 0 : 2;
            }
            int k = 0;
            while (MELEE_DROP_POOL[style][k])
                k++;
            pick =
                MELEE_DROP_POOL[style][k >
                                       1 ? T2rRand() % (unsigned) k : 0];
        }
    Xlog("supply box: melee pick %s (style %s, last %s)", pick,
         style >= 0 ? STYLE[style] : "cycle (t2r-melee-always)",
         g_b13.last_pick ? g_b13.last_pick : "none");
    g_b13.last_pick = pick;
    return pick;
}

static void B13AddMelee(void)
{
    if (g_macheteOwned || MacheteCarrying())
        return;                 // machete owner: no melee from boxes (the box gives its other items)
    const char *n = B13MeleeName();
    B13Add(FNV64(n), MeleeDurName(n), B13_K_MELEE);  // count = the weapon's 2013 durability
}

static int B13HoldsMelee(void)
{
    uint64_t none = 0, h = DcCall(SID_HAS_MELEE_WEAPON, 0, &none);
    return (h != ~0ULL && (h & 0xff)) || g_macheteOwned || MacheteCarrying(); // a bought / held machete = no other melee from boxes
}

// the 2013 2x4 rule (the old rule): none until both teams used 4 of 40 lives, then by pool thresholds, B13_MELEE_GAP boxes apart.
// t2r-box-melee-2013.txt next to the exe brings it back.
static void B13Melee2013(int fgc)
{
    // 2x4: allowed once the remaining lives of both teams are down to the threshold
    if (B13HoldsMelee() || g_b13.next_pool < 0
        || B13PoolSum() > g_b13.next_pool)
        return;
    int c = 0, v = g_b13.opened;
    if (g_b13.last_melee_box && v - g_b13.last_melee_box < B13_MELEE_GAP) {
        Xlog("supply box 2013: no melee (last one %d boxes ago, rarer since 1.09)", v - g_b13.last_melee_box);
        return;
    }
    if (!g_b13.initial2x4) {
        if (v <= 1)
            c = fgc < -1 ? 50 : 0;
        else if (v == 2)
            c = fgc <= -3 ? 100 : fgc < 2 ? 70 : 0;
        else if (v == 3)
            c = fgc < 3 ? 100 : 0;
        else
            c = 100;
    } else
        c = fgc >= 0 ? 50 : fgc == -1 ? 60 : fgc == -2 ? 70 : 100;
    c = (int) (c * g_boxMods.melee_mul);
    if (!c || (int) (1 + T2rRand() % 100) > c) {
        Xlog("supply box 2013: no melee (chance %d)", c);
        return;
    }
    B13AddMelee();
    g_b13.last_melee_box = v;
    g_b13.initial2x4 = 1;
    int n = ++g_b13.n2x4;       // next one when the pools are down to:
    // a bit more often but not too often: later drops unlock earlier than 2013's 30/24/12 (losing) and 24/12 (else), one more
    // drop each, box gap 4 (2013 1.09: 6)
    if (fgc < -2)
        g_b13.next_pool =
            n == 1 ? 32 : n == 2 ? 26 : n == 3 ? 18 : n == 4 ? 10 : -1;
    else if (n == 1)
        g_b13.next_pool = fgc > 2 ? -1 : 30;
    else if (n == 2)
        g_b13.next_pool = fgc <= 2 ? 20 : -1;
    else
        g_b13.next_pool = n == 3 && fgc <= 1 ? 10 : -1;
    Xlog("supply box 2013: melee (chance %d, #%d, next at pool %d)", c, n,
         g_b13.next_pool);
}

static void B13ComebackAndMelee(int fgc)
{
    if (!g_b13.from_open && !g_meleeAlways)
        return;                 // rolled when the player opens the box (2013: region visit)
    int lives = B13Lives(), diff = B13TeamDiff(), chance = 0;
    // 2013 @935c fgc -2 branch: Supply Raid enemy - our team count > 1, Interrogation our interrogations behind by >= 1 (10 % / 60 %)
    int ta, tl, td = LmsFgcAdjust(&ta, &tl);
    int cb_behind = R13IntMode()? td <= -1 : -td > 1;
    (void) diff;
    if (lives > -1000 && lives <= 0) {  // sudden death (our respawn pool empty)
        if (fgc == -2 && diff > 1)
            chance = 60;
        else if (fgc == -3)
            chance = 70;
        else if (fgc <= -4)
            chance = 100;
    } else if (g_b13.visits_cb == 1) {
        if (fgc <= -4)
            chance = 10;
    } else if (g_b13.visits_cb == 2) {
        if (fgc == -2 && cb_behind)
            chance = 10;
        else if (fgc == -3)
            chance = 20;
        else if (fgc <= -4)
            chance = 30;
    } else if (g_b13.visits_cb >= 3) {
        if (fgc == -2 && cb_behind)
            chance = R13IntMode()? 60 : 30;
        else if (fgc <= -3)
            chance = 70;
    }
    chance = (int) (chance * g_boxMods.comeback_mul);
    int wpn = 0;
    if (chance && !g_b13.cb_item_life
        && (int) (1 + T2rRand() % 100) <= chance) {
        if (InvQty(FNV64("pipe-bomb")) < 2)
            B13Add(FNV64("pipe-bomb"), 1, B13_K_THROW);
        else if (InvQty(FNV64("molotov")) < 2)
            B13Add(FNV64("molotov"), 1, B13_K_THROW);
        g_b13.cb_item_life = 1;
        Xlog("supply box 2013: comeback item (chance %d)", chance);
    }
    if (chance && !g_b13.cb_wpn_life
        && (int) (1 + T2rRand() % 100) <= chance && !B13HoldsMelee()) {
        B13AddMelee();
        g_b13.cb_wpn_life = wpn = 1;
        Xlog("supply box 2013: comeback melee (chance %d)", chance);
    }
    if (g_meleeAlways && !wpn) {
        B13AddMelee();
        return;
    }
    if (wpn)
        return;
    // Melee weapons were too hard to find: they needed 10% of lives lost by the team to spawn, and after you picked one up and
    // lost it, it was very hard to find another one even after looting all supply boxes; they are now more prominent. The 2013 2x4
    // rule (b13_melee_2013: none until both teams used 4 of 40 lives, pool thresholds, B13_MELEE_GAP boxes apart) is off by default: from the
    // match start every box a player WITHOUT a melee weapon opens gives one (guaranteed - a lost weapon is replaced at the next box), a player
    // who holds one gets another one (a swap / a different type) with B13_MELEE_SPARE_PCT %. The machete owner still gets none (as before).
    // t2r-box-melee-2013.txt next to the exe = the old 2013 rule.
    static int old_rule = -1;
    if (old_rule < 0) {
        FILE *f = ExeSideOpen("t2r-box-melee-2013.txt", "r");
        old_rule = f != NULL;
        if (f)
            fclose(f);
        Xlog("supply box: melee rule %s",
             old_rule ? "2013 2x4 (t2r-box-melee-2013.txt)" :
             "PT127 - from the match start, 60 % while none is held, 30 % spare chance otherwise");
    }
    if (old_rule) {
        B13Melee2013(fgc);
        return;
    }
    int v = g_b13.opened;
    if (g_macheteOwned || MacheteCarrying()) {
        Xlog("supply box: melee roll box #%d -> none (machete owner)", v);
        return;
    }
    int held = B13HoldsMelee(), c =
        held ? B13_MELEE_SPARE_PCT : B13_MELEE_NONE_PCT;
    c = (int) (c * g_boxMods.melee_mul);
    if (c > 100)
        c = 100;
    int r = (int) (1 + T2rRand() % 100);
    if (r > c) {
        Xlog("supply box: melee roll box #%d -> none (holds one, spare chance %d, roll %d)", v, c, r);
        return;
    }
    B13AddMelee();
    g_b13.last_melee_box = v;
    int n = ++g_b13.n2x4;
    Xlog("supply box: melee roll box #%d -> %s (%s, chance %d, roll %d, melee #%d from boxes this match)", v, g_b13.last_pick ? g_b13.last_pick : "?", held ? "spare - holds one" : "none held", c, r, n);
}

// the gun of loadout slot 0 (primary), 1 (secondary) or 2 (the PURCHASABLE: Assault Rifle, Specter, ...). The rule that gives
// 3 Assault Rifle ammo from a box when you have 4 or less did not work: the Assault Rifle is a purchasable (row of lo_purch_tab), and
// every ammo loop here only looked at slots 0 and 1, so a purchased gun never got box ammo or kill drops.
static const char *B13LoadoutGun(const LoadoutT *l, int slot)
{
    const uint32_t *cnt = (const uint32_t *) T2R_SYM(kLargeWeaponCounts);
    if (l->w[slot] == 0xffffffffu || l->w[slot] >= cnt[slot])
        return NULL;
    // (a crossbow in the purchasable slot that was never bought dropped bolts - only a BOUGHT purchasable counts)
    if (slot == 2)
        return ((char (*)(void)) T2R_SYM(kFnPurchNotBought)) ()? *(const
                                                                   char
                                                                   **)
            (LoPurchTab() + l->w[2] * 0x40 + 0x10) : NULL;
    if (slot == 0 && !g_largeTab)
        return NULL;
    return *(const char **) (WeaponRow(slot, l->w[slot]) + 0x10);
}

// ---- 5. ammo
static void B13Ammo(float kdr)
{
    LoadoutT l;
    memset(&l, 0xff, sizeof l.w);
    LoReadObj(&l);
    const uint32_t *cnt = (const uint32_t *) T2R_SYM(kLargeWeaponCounts);
    uint64_t done[3] = { 0, 0, 0 };
    int lucky_msg = 0;
    for (int slot = 0; slot < 3; slot++) {
        (void) cnt;
        const char *gun = B13LoadoutGun(&l, slot);
        const char *ammo = gun && *gun ? AmmoOfGun(gun) : NULL;
        if (!gun || !ammo)
            continue;
        uint64_t g = FNV64(gun), gb = BaseWeapon(g), a = FNV64(ammo);
        if (g_b13Uber && slot == 2) {  // 2013 LMS box: the purchasable as a full weapon pickup = its full reserve here
            int cap = ReserveCapOf(g), have = (int) InvQty(a);
            if (a != done[0] && a != done[1] && cap > have) {
                B13Add(a, (uint32_t) (cap - have), B13_K_AMMO);
                done[slot] = a;
            }
            Xlog("supply box 2013: LMS box - purchasable %s %s have %d, full reserve %d%s", gun, ammo, have, cap, cap > have ? " -> topped up" : " - nothing");
            continue;
        }
        int r = -1;
        for (size_t i = 0; i < sizeof B13_AMMO / sizeof *B13_AMMO && r < 0;
             i++)
            if (FNV64(B13_AMMO[i].gun) == g
                || FNV64(B13_AMMO[i].gun) == gb)
                r = (int) i;
        if (r < 0 || a == done[0] || a == done[1] || a == done[2]) {
            Xlog("supply box 2013: %s - no box ammo (2013 power weapon / unknown)", gun);
            continue;
        }
        // supply boxes give no ammunition for the assault rifle and El Diablo - boxes only; kill / execution drops keep their rows
        if ((g == FNV64(ASSAULT_ID) || gb == FNV64(ASSAULT_ID)
             || g == FNV64("pistol-el-diablo")
             || gb == FNV64("pistol-el-diablo"))
            && GetFileAttributesA("t2r-box-power-ammo.txt") ==
            INVALID_FILE_ATTRIBUTES) {
            Xlog("supply box 2013: %s - no box ammo (Assault Rifle / El Diablo)", gun);
            continue;
        }
        done[slot] = a;
        int have = (int) InvQty(a), tier =
            kdr <= 0.75f ? 0 : kdr <= 2.5f ? 1 : 2;
        int ideal = B13_AMMO[r].ideal[tier], big = B13_AMMO[r].big, small =
            B13_AMMO[r].small;
        int delta = ideal - have > 0 ? ideal - have : 0, orig = delta;
        float mul =
            B13_AMMO[r].pistol ? g_boxMods.pistol_ammo_mul : g_boxMods.
            long_ammo_mul;
        if (delta > 0 && mul > 1.0f)
            delta = (int) __builtin_ceilf(mul * (float) delta);
        if (!delta) {
            Xlog("supply box 2013: %s has %d (ideal %d) - no ammo", ammo,
                 have, ideal);
            continue;
        }
        int nb = delta / big, ns = (delta - nb * big + small - 1) / small;
        int nb_o = orig / big, rest_o = orig - nb * big;        // 2013 quirk: the original's small count uses the new big count
        int ns_o =
            rest_o > 0 ? (rest_o + small - 1) / small : rest_o / small;
        int inc = 0;
        if (g_boxMods.ammo_round_fix
            && nb * big + ns * small == nb_o * big + ns_o * small
            && ns > 0)
            inc = 1;            // 2013: any Lucky Break level
        for (int i = 0; i < nb; i++)
            B13Add(a, big, B13_K_AMMO);
        for (int i = 0; i < ns; i++)
            B13Add(a, small + inc, B13_K_AMMO);
        if (mul > 1.0f && delta > orig)
            lucky_msg = 1;
        Xlog("supply box 2013: %s (%s) have %d ideal %d (kdr %.2f) -> %d x%d + %d x%d%s", ammo, B13_AMMO[r].t1, have, ideal, kdr, nb, big, ns, small + inc, mul > 1.0f ? " (Lucky Break)" : "");
    }
    if (lucky_msg && g_b13.from_open)
        MsgHook("Lucky Break: extra ammo", 2.5f, 0, 0);
}

// ---- kill drop, 2013 ss-respawn-intro (PS4 v1.11 lambda 0x1e40, the per-player kill / execution event; notes
// re/factions2013/ps4/logic/awards-vox-matchflow.md). Event args (victim, downer, executer, point); the script runs on EVERY client but
// only spawns for the local player, so a pickup is only ever seen by its earner:
//  - the DOWNER (arg1 == local, victim != local): pistol ammo when the pistol is the weapon in hand (`0x10738a1a(0)` == the pistol's
//    name), otherwise the long gun's ammo; no long gun -> the pistol's ammo again. One pickup, the weapon's default pickup entity;
//  - the EXECUTER (arg2 == local, arg1 != local: someone else downed the victim): a coin flip, random 1..100 <= 50 pistol ammo else long;
//  - booster "extra ammo" (Scavenger): level I = the 'increased' pickup (x1.5), level II+ = the 'double' pickup (x2); level III also
//    spawns a crafting item every OTHER kill (sk_scav_pay).
// The weapons are the owned ones (2013 player-has-weapon?: pistol chain 9mm / revolver / Shorty / Enforcer / burst pistol, long chain
// bolt / burst / AK / semi / variable / short / hunting shotgun / ...). Ours: loadout slots 0, 1 AND the purchasable (slot 2).
// mode 0 = downer, 1 = executer. 0 = nothing valid to drop. The pack = the weapon's 'small' pickup (ammo-count-small).
static int B13DropPick(int mode, uint64_t *item, uint64_t *count)
{
    LoadoutT l;
    memset(&l, 0xff, sizeof l.w);
    LoReadObj(&l);
    int pr = -1, lr = -1;
    uint64_t pa = 0, la = 0;
    static const int ORDER[3] = { 1, 0, 2 };    // the pistol slot first; long guns: primary, then the purchasable
    for (int k = 0; k < 3; k++) {
        const char *gun = B13LoadoutGun(&l, ORDER[k]);
        const char *ammo = gun && *gun ? AmmoOfGun(gun) : NULL;
        if (!gun || !ammo)
            continue;
        uint64_t g = FNV64(gun), gb = BaseWeapon(g);
        int r = -1;
        for (size_t i = 0; i < sizeof B13_AMMO / sizeof *B13_AMMO && r < 0;
             i++)
            if (FNV64(B13_AMMO[i].gun) == g
                || FNV64(B13_AMMO[i].gun) == gb)
                r = (int) i;
        if (r < 0)
            continue;
        if (B13_AMMO[r].pistol) {
            if (pr < 0) {
                pr = r;
                pa = FNV64(ammo);
            }
        } else if (lr < 0) {
            lr = r;
            la = FNV64(ammo);
        }
    }
    int held_pistol = 0;
    if (pr >= 0) {
        uint64_t h = CurrentWeapon(), hb = h
            && h != ~0ULL ? BaseWeapon(h) : 0, pg =
            FNV64(B13_AMMO[pr].gun);
        held_pistol = h && h != ~0ULL && (h == pg || hb == pg);
    }
    int use_pistol;
    // executions / takedowns too follow the gun in hand (2013's executer coin flip dropped bolts with the revolver out)
    (void) mode;
    use_pistol = pr >= 0 && (held_pistol || lr < 0);
    if (use_pistol) {
        *item = pa;
        *count = B13_AMMO[pr].small;
    } else if (lr >= 0) {
        *item = la;
        *count = B13_AMMO[lr].small;
    } else
        return 0;
    Xlog("drops: 2013 %s ammo spawn: %s x%llu (pistol %d long %d, pistol in hand %d)", mode ? "executer" : "downer", B13Name(*item), (unsigned long long) *count, pr, lr, held_pistol);
    return 1;
}

static int B13KillDrop(uint64_t *item, uint64_t *count)
{
    return B13DropPick(0, item, count);
}

// 1 = our own player carries a gun that takes this ammo (loadout slots 0/1, the gun in hand, the last gun held): a pickup of it shows
// its icon. 0 = TLOU2 draws the pickup as the '?' unknown ammo (no carried gun uses it).
static int B13AmmoForWeapon(uint64_t w, uint64_t * item,
                               uint64_t * count, const char **gun_out);
static int B13LocalUsesAmmo(uint64_t ammo)
{
    if (!ammo)
        return 0;
    LoadoutT l;
    memset(&l, 0xff, sizeof l.w);
    LoReadObj(&l);
    const uint32_t *cnt = (const uint32_t *) T2R_SYM(kLargeWeaponCounts);
    for (int slot = 0; slot < 3; slot++) {
        (void) cnt;
        const char *gun = B13LoadoutGun(&l, slot);
        const char *a = gun && *gun ? AmmoOfGun(gun) : NULL;
        if (a && FNV64(a) == ammo)
            return 1;
    }
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    uint64_t ws[2] =
        { CurrentWeapon(), me < T2R_MAX_PEERS ? g_lastGun[me] : 0 };
    for (int i = 0; i < 2; i++) {
        uint64_t it = 0, c = 0;
        if (ws[i] && ws[i] != ~0ULL
            && B13AmmoForWeapon(ws[i], &it, &c, NULL) && it == ammo)
            return 1;
    }
    return 0;
}

// kill drop for a given weapon id (an execution: the executioner's LAST HELD gun): the same small pack as b13_kill_drop. 0 = no box ammo for it.
static int B13AmmoForWeapon(uint64_t w, uint64_t *item, uint64_t *count,
                               const char **gun_out)
{
    if (!w || w == ~0ULL)
        return 0;
    uint64_t wb = BaseWeapon(w);
    for (size_t i = 0; i < sizeof B13_AMMO / sizeof *B13_AMMO; i++) {
        uint64_t g = FNV64(B13_AMMO[i].gun);
        if (g != w && g != wb)
            continue;
        const char *ammo = AmmoOfGun(B13_AMMO[i].gun);
        if (!ammo)
            return 0;
        *item = FNV64(ammo);
        *count = B13_AMMO[i].small;
        if (gun_out)
            *gun_out = B13_AMMO[i].gun;
        return 1;
    }
    return 0;
}

// ---- the box: plan at fill begin, winmm's slots take the entries, the rest is placed by us
static void B13FillBegin(uint64_t box, int from_open)
{
    g_b13.n = g_b13.next = 0;
    g_b13.box = box;
    g_b13.from_open = from_open;
    b13_food = 0;
    if (g_b13Stock)
        return;
    B13Tick();
    memset(&g_boxMods, 0, sizeof g_boxMods);
    g_boxMods.pistol_ammo_mul = g_boxMods.long_ammo_mul =
        g_boxMods.melee_mul = g_boxMods.comeback_mul = 1.0f;
    SkBoxMods(&g_boxMods);   // skills.c: Lucky Break
    int32_t deaths;
    float kdr = B13Kdr(&deaths);
    float k = kdr;
    int gc = k <= 0.4f ? -2 : k <= 0.75f ? -1 : k == 1.0f ? 0 : k <= 2.5f ? 1 : 2;      // 2013 order (0.8 -> 1)
    int diff = B13TeamDiff(), fgc = gc - (diff >= 1
                                            && diff <= 3 ? diff : 0);
    // 2013 team-performance-items: the team adjustment (Supply Raid team counts / Interrogation interrogations, ours - theirs >= 5 / 3 / <= -3 / -5:
    // +2 / +1 / -1 / -2; behind = the lower group, more loot) and once per match a late joiner's step (2-8 minutes gone: down to -1..-3, -4)
    int tadj = 0, late = 0, tdiff = LmsFgcAdjust(&tadj, &late), fgc0 =
        fgc;
    fgc += tadj;
    if (from_open && late) {
        fgc = LmsLateApply(fgc, late);
        LmsLateFgcUsed();
    }
    if (tadj || (from_open && late))
        Xlog("supply box 2013: group %d -> %d (team count diff %d -> %+d%s)", fgc0, fgc, tdiff, tadj, from_open && late ? ", late join step" : "");
    if (from_open) {
        g_b13.opened++;
        if (fgc <= -2)
            g_b13.visits_cb++;
    }
    if (from_open && g_lmsBox) {
        // 2013 last-man-standing-spawn: the player's purchasable (full), a health kit, a molotov, a smoke bomb, 2 nail bombs, 2 random crafting
        // items and the normal ammo top-up. No parts, no caps, no Lucky Break. Once: the next boxes give their normal plan again.
        g_lms_box = 0;
        g_b13Uber = 1;
        Xlog("supply box 2013: LAST MAN STANDING box (box #%d)",
             g_b13.opened);
        B13Add(FNV64("health-kit"), 1, B13_K_KIT);
        B13Add(FNV64("molotov"), 1, B13_K_THROW);
        B13Add(FNV64("smoke-bomb"), 1, B13_K_THROW);
        B13Add(FNV64("pipe-bomb"), 1, B13_K_THROW);
        B13Add(FNV64("pipe-bomb"), 1, B13_K_THROW);
        for (int i = 0; i < 2; i++) {
            uint64_t ing = B13RandomIng();
            B13Add(ing, B13IngSize(ing), B13_K_ING);
        }
        B13Ammo(kdr);
        g_b13Uber = 0;
        for (int i = 0; i < g_b13.n; i++)
            Xlog("supply box 2013:   + %s x%u (have %llu)",
                 B13Name(g_b13.e[i].item), g_b13.e[i].count,
                 (unsigned long long) InvQty(g_b13.e[i].item));
        return;
    }
    int starter = from_open && deaths == 0 && diff <= 0 ? g_b13.opened : 0;
    Xlog("supply box 2013: plan (kdr %.2f, deaths %d, group %d, team diff %d -> %d, box #%d)", kdr, deaths, gc, diff, fgc, g_b13.opened);
    B13Crafting(fgc, starter);
    B13Health();
    B13ComebackAndMelee(fgc);
    B13Ammo(kdr);
    for (int i = 0; i < g_b13.n; i++)
        Xlog("supply box 2013:   + %s x%u (have %llu)",
             B13Name(g_b13.e[i].item), g_b13.e[i].count,
             (unsigned long long) InvQty(g_b13.e[i].item));
    if (!g_b13.n)
        Xlog("supply box 2013:   (empty)");
}

static int B13Take(uint64_t *item, uint64_t *count, int *kind)
{
    if (g_b13.next >= g_b13.n)
        return 0;
    B13EntryT *e = &g_b13.e[g_b13.next++];
    *item = e->item;
    *count = e->count;
    *kind = e->kind;
    return 1;
}

// patch winmm's slot count before its fill: every slot gets a spawn call (rand%4+3 -> all slots);
// stock when a box has more slots than its item list can index (2 x 8 entries)
static void B13SlotCount(uint64_t box)
{
    uint8_t *disp = (uint8_t *) T2R_CODE(kCode4CE9D);   // lea ecx, [rdi + rax + 3] (8d 4c 07 03)
    if (disp[-3] != 0x8D || disp[-2] != 0x4C || disp[-1] != 0x07)
        return;
    int64_t nslots =
        (*(int64_t *) (box + 0x20) - *(int64_t *) (box + 0x18)) >> 3;
    uint8_t want = g_b13Stock || nslots > 16 ? 3 : 0x7F;
    if (*disp == want)
        return;
    DWORD old;
    VirtualProtect(disp, 1, PAGE_EXECUTE_READWRITE, &old);
    *disp = want;
    VirtualProtect(disp, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), disp, 1);
}

static void B13FillEnd(uint64_t box)
{
    if (g_b13Stock)
        return;
    int left = g_b13.n - g_b13.next, placed = 0;
    uint64_t *b = *(uint64_t **) (box + 0x18), *e =
        *(uint64_t **) (box + 0x20);
    int nslots = b && e > b ? (int) (e - b) : 0;
    for (int k = 0; nslots && g_b13.next < g_b13.n; k++) {
        B13EntryT *it = &g_b13.e[g_b13.next++];
        uint64_t obj =
            ((uint64_t(*)(uint64_t)) T2R_SYM(kFnLocate)) (b[k % nslots]);
        if (!obj)
            continue;
        float pos[4] = { 0 }, rot[4] = { 0, 0, 0, 1 };
        ((void (*)(float *, uint64_t)) T2R_SYM(kFnObjectPosition)) (pos,
                                                                    obj);
        ((void (*)(float *, uint64_t)) T2R_SYM(kFnObjectRotation)) (rot,
                                                                    obj);
        int layer = 1 + k / nslots;     // the slots are taken: beside them
        pos[0] += 0.16f * (float) layer *((k & 1) ? -1.0f : 1.0f);
        pos[2] += 0.10f * (float) layer;
        pos[1] += 0.02f * (float) layer;
        uint64_t xf =
            ((uint64_t(*)(const float *, const float *))
             T2R_SYM(kFnXformCompose)) (pos, rot);
        uint64_t ret[4] = { 0 };
        BoxSpawn(ret, it->item, xf, it->count, it->kind, 1, 0, 0, 0, 0);
        uint64_t h = ret[0];
        if (!h || h == ~0ULL)
            continue;
        placed++;
        uint64_t **v = (uint64_t **) (box + 0x30);      // the box's spawned list (std::vector<u64>)
        if (v[1] != v[2]) {
            *v[1] = h;
            v[1]++;
        } else
            ((void (*)(void *, uint64_t *, uint64_t *))
             T2R_SYM(kFnVecAssign)) (v, v[1], &h);
    }
    if (left)
        Xlog("supply box 2013: %d pickup(s) beside the %d slot(s): %d placed", left, nslots, placed);
    if (g_b13.from_open && b13_food) {
        for (int i = 0; i < b13_food; i++)
            SkHeal(B13_HEAL_CAN, "supply box food can");
        // (no "Food: +N health" HUD text - a test leftover; the 2013 heal stays silent like 2013's)
    }
}

// 2013 refill: 110-140 s after the open (ss-region-respawn-check; it also paused the timer while
// the player stood in the box's region - not done here). winmm set 120 s at the open.
// Loot was too frequent with more supply boxes: 2013 levels had exactly 4 boxes (winmm's table is 4 hard-coded
// entries), our maps carry 5-10. The refill is scaled by the boxes the map really has, so the whole map gives the supply per second of a
// 4-box 2013 level: delay x (boxes / 4), never below 1 (a map with 4 or fewer boxes = the exact 2013 110-140 s). The box count is read from
// winmm's table (records that hold item positions), so more / fewer boxes in a pak and the later loot spawn spots follow with no code change.
// t2r-box-refill-2013.txt next to the exe = the flat 110-140 s again.
#define B13_2013_BOXES 4
static int B13BoxCount(void)
{
    uint8_t *beg = *(uint8_t **) T2R_SYM(kPeerListBegin), *end =
        *(uint8_t **) T2R_SYM(kPeerListEnd);
    if (!beg || end <= beg || (end - beg) / 0x48 > 64
        || IsBadReadPtr(beg, (size_t) (end - beg)))
        return 0;               // (a vector freed at a level change reads as unreadable, not a crash)
    int n = 0;
    for (uint8_t * e = beg; e < end; e += 0x48)
        if (*(uint64_t *) (e + 0x18) != *(uint64_t *) (e + 0x20))
            n++;
    return n;
}

static void B13AfterOpen(uint64_t *box, int was_opened)
{
    if (g_b13Stock || was_opened || !*((uint8_t *) box + 0x14))
        return;
    uint8_t *lo = LobbyObj();
    if (!lo)
        return;
    static int flat = -1;
    if (flat < 0) {
        FILE *f = ExeSideOpen("t2r-box-refill-2013.txt", "r");
        flat = f != NULL;
        if (f)
            fclose(f);
    }
    int nb = B13BoxCount();
    float scale = !flat
        && nb >
        B13_2013_BOXES ? (float) nb / (float) B13_2013_BOXES : 1.0f;
    float now = *(float *) (lo + 0x2c90), t =
        (110.0f + (float) (T2rRand() % 3001) / 100.0f) * scale;
    *(float *) ((uint8_t *) box + 0x10) = now + t;
    Xlog("supply box 2013: refills in %.0f s (%d boxes on the map, x%.2f of 2013's 110-140 s%s)", t, nb, scale, flat ? ", flat" : "");
}

// ---- the opening gesture (a non-locking opening animation, so boxes do not just open on their own)
// The box opens the moment E is pressed (box_open_gate -> FUN_18004ee40: lid, latch / lid sounds, the fill). The player's arm now reaches for it: TLOU2's own
// pickup GESTURE (the SP scav pickup reach, "...-right-arm--move-fw-part--gesture": a PARTIAL arm clip layered over whatever the legs / camera / aim do, the same
// mechanism as the revive hand-out, the mark point and the stun cough - it never touches input, movement or aim, so nothing is locked). Standing: the 50 cm
// reach (a crate is knee / hip high), crouched: the 60 cm crouch reach; the next loaded clip of the row when the first is missing, a logged no-op when none is.
// Own body = the request from bo_tick (the open call itself runs inside the engine's per-frame box check: the request is queued, not made there); every other screen:
// MSG_BOXOPEN 0x95 [u8 1 start][u8 stance 0 standing / 1 crouched] -> the same gesture on our puppet there, [u8 0] = it was cancelled (clear-gesture, 0.25 s blend).
// Cancels cleanly on both screens: sprinting (> 4.8 m/s), aiming down sights, damage taken, downed / dead, a paired move or a menu; the gesture otherwise ends itself
// (0.87 s clip). Not played while sprinting / aiming at the moment of the open either (the box still opens). t2r-no-box-gesture.txt next to the exe = off.
// Logs: "boxreach: ..." (own request, puppet request, answer of gesture_, cancel reason); expected on one open: own request line + "own reach over".
#define MSG_BOXOPEN 0x95
// Candidate reaches (SP trace: No Return chests play ellie-scav-tense-[crouch-]pickup-<h>cm-<arm>-arm--move-*-part--gesture as a partial gesture,
// Abby crouched adds abby-pickup-crouch-idle-{low|floor}-forward-{l|r}-hand-gesture): height class by the box vs our feet, arm by the box's side.
// Table index + 1 is sent to peers (MSG_BOXOPEN byte 3) so they play the same reach.
static const struct {
    const char *gesture, *clip;
    int stance, h, left;
} BOXC[] = {
#define EC(st, hh, arm, side) {"ellie-scav-tense-" st "pickup-" #hh "cm-" arm "-arm--move-fw-part--gesture", "ellie-scav-tense-" st "pickup-" #hh "cm-" arm "-arm--move-fw-part", st[0] ? 1 : 0, hh, side}
    EC("", 50, "right", 0), EC("", 50, "left", 1), EC("", 110, "right", 0),
        EC("", 110, "left", 1),
    EC("crouch-", 60, "right", 0), EC("crouch-", 60, "left", 1),
        EC("crouch-", 0, "right", 0), EC("crouch-", 0, "left", 1),
    { "abby-pickup-crouch-idle-low-forward-r-hand-gesture",
     "abby-pickup-crouch-idle-low-forward-r-hand-gesture", 1, 40, 0 },
    { "abby-pickup-crouch-idle-low-forward-l-hand-gesture",
     "abby-pickup-crouch-idle-low-forward-l-hand-gesture", 1, 40, 1 },
    { "abby-pickup-crouch-idle-floor-forward-r-hand-gesture",
     "abby-pickup-crouch-idle-floor-forward-r-hand-gesture", 1, 0, 0 },
    { "abby-pickup-crouch-idle-floor-forward-l-hand-gesture",
     "abby-pickup-crouch-idle-floor-forward-l-hand-gesture", 1, 0, 1 },
};

#define BOXC_N ((int)(sizeof BOXC / sizeof BOXC[0]))
#define BOXG_SPRINT_SPEED 4.8f
#define BOXG_BLEND_OUT 0.25f
// The open itself is now DEFERRED to the hand's contact. box_open_gate no longer opens at
// once: bo_defer() parks the box, bo_tick decides (reach possible?) and plays the reach at BOXG_SPEED x; the real open (lid + latch sounds, the fill) runs at the
// hand contact (BOXG_CONTACT of the clip / speed, at most BOXG_MAX_DELAY_MS after E), or at once when no reach is played. The reach is cut at BOXG_END of the
// clip with a 0.25 s blend. Refused open (engine said no): the reach is cleared and the next E goes straight through for BOXG_COOL_MS.
// The SP crate clip (t2r-abby-scav-low-crate-open-start, full-body script-AP) is NOT used: SP-only, would lock the body.
#define BOXG_SPEED 1.0f         // was 1.8 - fluidity over fast: no forced clip rates above 1.0 (contact still lands at 0.30 s, inside BOXG_MAX_DELAY_MS)
#define BOXG_CONTACT 0.30f
#define BOXG_END 0.80f
#define BOXG_MAX_DELAY_MS 300
#define BOXG_OPEN_MS 350        // SP nr-1102: press -> crate open clip + sounds = +0.35 s (was the hand contact, 0.30 s of the clip)
#define BOXG_COOL_MS 1500
static struct {
    int pending, on;
    uint64_t t0, until;
    uint64_t ent;
    float hp0;
    int stance;
    int used, sp_left;
    uint64_t sp_next;
} g_bog;
static struct {
    uint64_t h, until;
    int used, sp_left;
    uint64_t sp_next;
} g_bog_pup[T2R_MAX_PEERS];
static struct {
    uint64_t *box;
    int decide;
    uint64_t open_at, cool, t_press;
} g_bod;
// ps46: t2r-ext.c female_knife_item - a melee press while a box is parked / its reach plays / within 2 s of the E press is no dead knife press
static int FemBoxBusy(void)
{
    uint64_t now = GetTickCount64();
    return g_bod.box != NULL || g_bog.on || (g_bod.t_press
                                             && now - g_bod.t_press <
                                             2000);
}

static void BogSpeed(uint64_t ent, int used, float sp)
{
    static const int L[] = { -1, 3, 4, 5, 6, 7, 8, 1, 0 };
    if (used < 1 || used > BOXC_N)
        return;
    for (unsigned k = 0; k < sizeof L / sizeof *L; k++) {
        uint64_t b[4] =
            { ent, 0, (uint64_t) (int64_t) L[k],
    FNV64(BOXC[used - 1].clip) };
        memcpy(&b[1], &sp, 4);
        DcCall(FNV64("set-animation-speed"), 4, b);
    }
}

static uint64_t BogMs(float dur, float frac)
{
    return (uint64_t) (dur * frac / BOXG_SPEED * 1000.0f);
}

static void BogCancelOwn(const char *why);
static int BogOff(void)
{
    static int off = -1;
    static uint64_t chk;
    uint64_t now = GetTickCount64();
    if (!chk || now - chk > 5000) {
        chk = now;
        FILE *f = ExeSideOpen("t2r-no-box-reach.txt", "r"), *f2 =
            ExeSideOpen("t2r-no-box-gesture.txt", "r");
        int o = f != NULL || f2 != NULL;
        if (f)
            fclose(f);
        if (f2)
            fclose(f2);
        if (o != off)
            Xlog("boxreach: %s", o ? "OFF (t2r-no-box-reach.txt)" : "on");
        off = o;
    }
    return off;
}

// request the best loaded reach for (stance, wanted height cm, wanted arm) on `ent`; `hint` (table index + 1, from a peer) is tried first. A locked-out /
// refused gesture goes on to the next candidate. *dur = clip length (0 = nothing played), *used = table index + 1.
static int BogPlay(uint64_t ent, int stance, int wh, int wleft, int hint,
                    const char *who, float *dur, int *used)
{
    static unsigned nl;
    static int probed;
    *dur = 0;
    *used = 0;
    float len[BOXC_N];
    for (int i = 0; i < BOXC_N; i++)
        len[i] = AnimLen(ent, BOXC[i].clip);
    if (!probed) {
        probed = 1;
        char b[1024];
        int o = 0;              // SNLEFT: 12 names are ~700 chars; the old 600-byte buffer + "sizeof b - o" ran past its end and smashed bo_tick's stack (first open after launch never opened)
        for (int i = 0; i < BOXC_N; i++)
            o += snprintf(b + o, SNLEFT(b, o), " %s%s",
                          len[i] > 0 ? "+" : "-", BOXC[i].clip);
        Xlog("boxreach: clips loaded on %llx (+ = yes):%s",
             (unsigned long long) ent, b);
    }
    int order[BOXC_N], n = 0, tried[BOXC_N] = { 0 };
    if (hint >= 1 && hint <= BOXC_N && len[hint - 1] > 0)
        order[n++] = hint - 1, tried[hint - 1] = 1;
    for (;;) {
        int best = -1, bc = 1 << 30;
        for (int i = 0; i < BOXC_N; i++) {
            if (tried[i] || len[i] <= 0)
                continue;
            int c =
                (BOXC[i].stance !=
                 stance ? 1000 : 0) + abs(BOXC[i].h - wh) +
                (BOXC[i].left != wleft ? 25 : 0) + (BOXC[i].clip[0] ==
                                                    'a' ? 5 : 0);
            if (c < bc)
                bc = c, best = i;
        }
        if (best < 0)
            break;
        tried[best] = 1;
        order[n++] = best;
    }
    if (!n) {
        if (LOGCAP(nl, 6))
            Xlog("boxreach: %s - no pickup clip is loaded on %llx - no gesture", who, (unsigned long long) ent);
        return 0;
    }
    uint64_t none = 0;
    for (int k = 0; k < n && k < 4; k++) {
        int i = order[k];
        uint64_t pp =
            DcCall(FNV64("alloc-gesture-play-params"), 0, &none);
        if (!pp || pp == ~0ULL || IsBadReadPtr((void *) pp, 0x90)) {
            if (LOGCAP(nl, 6))
                Xlog("boxreach: %s - alloc-gesture-play-params -> %llx, no gesture", who, (unsigned long long) pp);
            return 0;
        }
        ((uint8_t *) pp)[0x20] = 0;
        ((uint8_t *) pp)[0x21] = 0;     // defaults only (the scripts' flags crash outside a script task)
        uint64_t a[3] = { ent, FNV64(BOXC[i].gesture), pp };
        uint64_t r = DcCall(FNV64("gesture_"), 3, a);
        if (LOGCAP(nl, 60))
            Xlog("boxreach: %s - %s (clip %.2f s, want %s %d cm %s arm) on %llx -> %llx (%s)", who, BOXC[i].gesture, len[i], stance ? "crouch" : "stand", wh, wleft ? "left" : "right", (unsigned long long) ent, (unsigned long long) r, r == 0 ? "taken" : r == 0x9417267ed3882d2eULL ? "locked-out" : r == 0x43436c3a4206d93aULL ? "gesture-anims-not-found" : "other");
        // gesture_ answers 0 or the constant 0x9417.. with no real meaning (revive hand-out: it plays either way); only "anims not found" moves on
        if (r != 0x43436c3a4206d93aULL) {
            *dur = len[i];
            *used = i + 1;
            return 1;
        }
    }
    return 0;
}

static void BogClear(uint64_t ent)
{
    uint64_t c[4] = { ent, (uint64_t) - 1LL, 0, 0 };
    float bl = BOXG_BLEND_OUT;
    memcpy(&c[2], &bl, 4);
    DcCall(FNV64("clear-gesture"), 4, c);
}

// box_open_gate: 1 = parked (the tick opens it at the hand contact), 0 = open now as before
static int BoDefer(uint64_t *box)
{
    uint64_t now = GetTickCount64();
    if (BogOff() || MatchSubstate() != 6 || now < g_bod.cool || !box
        || *((uint8_t *) box + 0x14))
        return 0;
    if (g_bod.box)
        return 1;               // E held / already parked
    g_bod.box = box;
    g_bod.decide = 1;
    g_bod.open_at = 0;
    g_bod.t_press = now;
    return 1;
}

// the real open, at the contact (or at once). Same body as box_open_gate's pass-through.
static void BodOpen(void)
{
    uint64_t *box = g_bod.box;
    uint64_t now = GetTickCount64();
    g_bod.box = NULL;
    g_bod.open_at = 0;
    g_bod.decide = 0;
    if (!box || IsBadReadPtr(box, 0x20) || MatchSubstate() != 6) {
        if (g_bog.on)
            BogCancelOwn("box gone");
        return;
    }
    if (g_downed || !ActorCanAct()) {
        Xlog("boxreach: open dropped - we are downed / not able to act");
        return;
    }
    int was = *((uint8_t *) box + 0x14);
    if (!was)
        ((void (*)(uint64_t *)) T2R_SYM(kFnBoxOpen)) (box);
    if (!was && *((uint8_t *) box + 0x14)) {
        Xlog("boxreach: box opened %.2f s after E%s",
             (now - g_bod.t_press) / 1000.0,
             g_bog.on ? " (hand contact)" : " (no reach)");
        B13AfterOpen(box, was);
    } else if (!was) {
        g_bod.cool = now + BOXG_COOL_MS;
        Xlog("boxreach: engine refused the open - reach cleared, direct opens for %d ms", BOXG_COOL_MS);
        if (g_bog.on)
            BogCancelOwn("open refused");
    }
}

static void BogCancelOwn(const char *why)
{
    if (!g_bog.on)
        return;
    g_bog.on = 0;
    if (g_bod.box && g_bod.open_at > GetTickCount64())
        g_bod.open_at = GetTickCount64();       // interrupted before the contact: the box still opens, now
    BogClear(g_bog.ent);
    uint8_t b = 0;
    if (g_sendTramp)
        g_sendTramp(MSG_BOXOPEN, &b, 1, 0xff);
    Xlog("boxreach: own reach cancelled (%s), %.2f s in", why,
         (GetTickCount64() - g_bog.t0) / 1000.0);
}

static void BoTick(void)
{
    uint64_t now = GetTickCount64();
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    if (g_bod.box && g_bod.decide) {
        g_bod.decide = 0;
        g_bog.pending = 1;
        g_bod.open_at = now;
    }
    if (g_bog.pending) {
        g_bog.pending = 0;
        const char *why = NULL;
        uint64_t none = 0;
        float hp = LocalHp();
        if (g_downed || g_downDying || g_execUntil || hp <= 0.0f
            || !ActorCanAct())
            why = "downed / dead / paired move";
        else if (PlayerMenuActive() == 1 || g_tabOpen)
            why = "menu / TAB open";
        else if (BpOwnSpeed() > BOXG_SPRINT_SPEED)
            why = "sprinting";
        else {
            uint64_t aim = DcCall(FNV64("player-is-aiming?"), 0, &none);
            if (aim != ~0ULL && (aim & 0xff))
                why = "aiming down sights";
        }
        if (why) {
            Xlog("boxreach: not played (%s)", why);
        } else {
            uint64_t cr =
                DcCall(FNV64("player-is-crouched?"), 0, &none), pr =
                DcCall(FNV64("player-is-prone?"), 0, &none);
            int stance = (cr != ~0ULL && (cr & 0xff)) ? 1 : 0;
            if (pr != ~0ULL && (pr & 0xff)) {
                Xlog("boxreach: not played (prone)");
                stance = -1;
            }
            // winmm's open (FUN_18004ee40, 0x18004eebb) calls player-force-crouch every time: the body kneels at the open whatever the stance was
            // (a standing reach + the engine's crouch = a standing pickup sinking into a crouch, player left crouched). The reach is
            // therefore always picked from the crouched set, and a standing player is crouched right at the press (the same native the open calls
            // 0.3 s later), so the body kneels with the reach - before, the crouched reach on a still-standing body lifted the hand to the shoulder.
            if (stance == 0) {
                DcCall(FNV64("player-force-crouch"), 0, &none);
                stance = 1;
            }
            float dur;
            int used = 0, wh = stance ? 60 : 50, wleft = 0, Sim = 0;
            {                   // the box vs us: height of its origin over our feet picks the reach height, the side of our left axis picks the arm
                typedef void *(*fn_proc)(uint64_t);
                typedef uint8_t *(*fn_me)(void);
                uint8_t *beg =
                    *(uint8_t **) T2R_SYM(kPeerListBegin), *me_p =
                    ((fn_me) T2R_SYM(kFnLocalPlayer)) ();
                uint8_t *bpr = beg && !IsBadReadPtr(beg, 8)
                    && *(uint64_t *) beg ? ((fn_proc)
                                            T2R_SYM(kFnEntityFromHandle))
                    (*(uint64_t *) beg) : NULL;
                float bp[3], mp3[3];
                if (bpr && me_p && ProcPos(bpr, bp) && ProcPos(me_p, mp3)
                    && !IsBadReadPtr(me_p + 0x100, 0x40)) {
                    typedef const float *(*fn_loc)(void *, float *);
                    float la[16];
                    const float *lc =
                        ((fn_loc)
                         ((uint8_t *) GetModuleHandleW(NULL) +
                          0x133db30)) (me_p + 0x100, la);
                    float dx = bp[0] - mp3[0], dy = bp[1] - mp3[1], dz =
                        bp[2] - mp3[2], side = 0;
                    if (lc) {
                        float qx = lc[4], qy = lc[5], qz = lc[6], qw =
                            lc[7];
                        side = dx * (1.0f - 2.0f * (qy * qy + qz * qz)) + dz * 2.0f * (qx * qz - qw * qy);      // +x of the body is its left
                        wleft = side > 0.12f;
                    }
                    wh = stance ? (dy < 0.05f ? 0 : 60) : (dy > 0.80f ? 110 : 50);      // origin height unverified: only a clearly high box leaves the 50 / 60 cm default (log shows dy)
                    Xlog("boxreach: box %.2f m away, %.2f m above our feet, %.2f m to our %s -> %s %d cm %s arm", __builtin_sqrtf(dx * dx + dz * dz), dy, side < 0 ? -side : side, side > 0 ? "left" : "right", stance ? "crouch" : "stand", wh, wleft ? "left" : "right");
                    // The character did not reach out to open it like No Return in SP: the NR crate's own player part is
                    // player-simulate-pickup(crate locator + (0, 0.5, 0), 1, 0, 0, 1) = the engine's generic pickup gesture picked by that point's height
                    // (SP trace re/nr-supply-box-trace-1102.md: abby-scav-tense-crouch-pickup-40cm-left-arm--forward + the idle-low-forward gesture, free
                    // again 0.92 s after the press). Tried first; a native that is missing / answers ~0 falls through to the old partial-arm reach.
                    // t2r-no-box-simpickup.txt next to the exe = the old reach only.
                    static int nosim = -1;
                    if (nosim < 0) {
                        FILE *fs_ =
                            ExeSideOpen("t2r-no-box-simpickup.txt", "r");
                        nosim = fs_ != NULL;
                        if (fs_)
                            fclose(fs_);
                    }
                    if (!nosim) {
                        static
                            __attribute__((aligned(16))) float sp_p[4],
                            sp_q[4];
                        sp_p[0] = bp[0];
                        sp_p[1] = bp[1] + 0.5f;
                        sp_p[2] = bp[2];
                        sp_p[3] = 1.0f;
                        sp_q[0] = 0.0f;
                        sp_q[1] = 0.0f;
                        sp_q[2] = 0.0f;
                        sp_q[3] = 1.0f;
                        uint64_t bfa[5] =
                            { (uint64_t) sp_p, (uint64_t) sp_q, 0, 0, 0 };
                        uint64_t bf =
                            DcCall(0x871ec52cc86b7427ULL
                                    /* alloc-bound-frame */ , 5, bfa);
                        if (bf && bf != ~0ULL) {
                            uint64_t pa[5] = { bf, 1, 0, 0, 1 };
                            uint64_t pr_ =
                                DcCall(FNV64("player-simulate-pickup"), 5,
                                        pa);
                            Sim = pr_ != ~0ULL;
                            static int nsim;
                            if (LOGCAP(nsim, 12))
                                Xlog("boxreach: player-simulate-pickup(crate +0.5 m) -> %llx (%s)", (unsigned long long) pr_, Sim ? "SP's NR reach plays" : "native missing, old reach");
                        }
                    }
                }
            }
            if (Sim) {          // the engine's pickup gesture runs by itself (0.92 s); only the open timing + the peers' puppet reach are ours
                g_bod.open_at = now + BOXG_OPEN_MS;
                uint8_t b[3] = { 1, (uint8_t) stance, 0 };
                if (g_sendTramp)
                    g_sendTramp(MSG_BOXOPEN, b, 3, 0xff);
            } else
                if (stance >= 0
                    && BogPlay(SID_PLAYER, stance, wh, wleft, 0,
                                "own body", &dur, &used)) {
                g_bog.on = 1;
                g_bog.ent = SID_PLAYER;
                g_bog.t0 = now;
                g_bog.until = now + BogMs(dur, BOXG_END);
                g_bog.hp0 = hp;
                g_bog.stance = stance;
                g_bog.used = used;
                g_bog.sp_left = 4;
                g_bog.sp_next = now;
                {
                    uint64_t d = BogMs(dur, BOXG_CONTACT);
                    if (d > BOXG_MAX_DELAY_MS)
                        d = BOXG_MAX_DELAY_MS;
                    d = BOXG_OPEN_MS;
                    g_bod.open_at = now + d;    // SP trace: crate opens 0.35 s after the press
                    Xlog("boxreach: reach at %.1fx, hand contact / open in %llu ms, reach cut at %.2f s", BOXG_SPEED, (unsigned long long) d, dur * BOXG_END / BOXG_SPEED);
                }
                uint8_t b[3] = { 1, (uint8_t) stance, (uint8_t) used };
                if (g_sendTramp)
                    g_sendTramp(MSG_BOXOPEN, b, 3, 0xff);
            }
        }
    }
    if (g_bog.on) {
        uint64_t none = 0;
        float hp = LocalHp();
        const char *why = NULL;
        if (g_downed || g_downDying || hp <= 0.0f)
            why = "downed / dead";
        else if (hp < g_bog.hp0 - 0.5f)
            why = "damage taken";
        else if (!ActorCanAct())
            why = "paired move";
        else if (PlayerMenuActive() == 1 || g_tabOpen)
            why = "menu / TAB open";
        else if (BpOwnSpeed() > BOXG_SPRINT_SPEED)
            why = "sprinting";
        else {
            uint64_t aim = DcCall(FNV64("player-is-aiming?"), 0, &none);
            if (aim != ~0ULL && (aim & 0xff))
                why = "aiming down sights";
        }
        if (why)
            BogCancelOwn(why);
        else if (now >= g_bog.until) {
            g_bog.on = 0;
            BogClear(g_bog.ent);
            Xlog("boxreach: own reach over (%.2f s)",
                 (now - g_bog.t0) / 1000.0);
        } else if (g_bog.sp_left && now >= g_bog.sp_next) {
            BogSpeed(g_bog.ent, g_bog.used, BOXG_SPEED);
            g_bog.sp_left--;
            g_bog.sp_next = now + 100;
        } else if (hp > g_bog.hp0)
            g_bog.hp0 = hp;
    }
    if (g_bod.box || g_bog.on) {        // diag (the first open after launch played the reach but never opened, no "reach over" either)
        static uint64_t dl;
        static int dn;
        if (now - dl >= 250 && LOGCAP(dn, 60)) {
            dl = now;
            Xlog("boxreach diag: parked %d decide %d open_at %+lld ms, reach on %d until %+lld ms, %llu ms after E", g_bod.box != NULL, g_bod.decide, g_bod.open_at ? (long long) (g_bod.open_at - now) : 0LL, g_bog.on, g_bog.on ? (long long) (g_bog.until - now) : 0LL, (unsigned long long) (now - g_bod.t_press));
        }
    }
    if (g_bod.box && !g_bod.decide && g_bod.open_at
        && now >= g_bod.open_at)
        BodOpen();
    else if (g_bod.box && now - g_bod.t_press > 1500) {
        g_bod.box = NULL;
        g_bod.open_at = 0;
        g_bod.decide = 0;
    }                           // safety: never leave a box parked
    typedef uint8_t *(*fn_peer)(uint32_t);
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        if (!g_bog_pup[s].h || s == me)
            continue;
        uint8_t *pe = ((fn_peer) T2rPeer) (s);
        uint64_t h = pe && pe[4] && !pe[5]
            && !IsBadReadPtr(pe + 0x60, 8) ? *(uint64_t *) (pe + 0x60) : 0;
        if (h != g_bog_pup[s].h || g_peerDowned[s]
            || g_puppetDeadHere[s] || PsyncBusySlot(s)) {
            if (h == g_bog_pup[s].h && h && !g_puppetDeadHere[s])
                BogClear(h);
            g_bog_pup[s].h = 0;
            continue;
        }
        if (now >= g_bog_pup[s].until) {
            BogClear(h);
            g_bog_pup[s].h = 0;
        }                       // cut at BOXG_END (0.25 s blend)
        else if (g_bog_pup[s].sp_left && now >= g_bog_pup[s].sp_next) {
            BogSpeed(h, g_bog_pup[s].used, BOXG_SPEED);
            g_bog_pup[s].sp_left--;
            g_bog_pup[s].sp_next = now + 100;
        }
    }
}

static void BoRecv(uint32_t slot, const uint8_t *buf, uint64_t len)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    if (slot > T2R_MAX_PEERS - 1 || !buf || len < 1 || BogOff()
        || MatchSubstate() != 6 || slot == *(uint32_t *) T2R_SYM(kMySlot))
        return;
    uint8_t *peer = ((fn_peer) T2rPeer) (slot);
    uint64_t h = peer && peer[4] && !peer[5]
        && !IsBadReadPtr(peer + 0x60, 8) ? *(uint64_t *) (peer + 0x60) : 0;
    if (!h)
        return;
    if (buf[0] == 0) {          // cancelled on its screen
        if (g_bog_pup[slot].h == h) {
            BogClear(h);
            Xlog("boxreach: slot %u cancelled - puppet reach cleared",
                 slot);
        }
        g_bog_pup[slot].h = 0;
        return;
    }
    if (len < 2 || buf[1] > 1 || g_peerDowned[slot]
        || g_puppetDeadHere[slot] || PsyncBusySlot(slot))
        return;
    char who[40];
    snprintf(who, sizeof who, "puppet of slot %u", slot);
    float dur;
    int used;
    if (BogPlay
        (h, buf[1], buf[1] ? 60 : 50, 0, len >= 3 ? buf[2] : 0, who, &dur,
         &used)) {
        uint64_t n = GetTickCount64();
        g_bog_pup[slot].h = h;
        g_bog_pup[slot].until = n + BogMs(dur, BOXG_END);
        g_bog_pup[slot].used = used;
        g_bog_pup[slot].sp_left = 4;
        g_bog_pup[slot].sp_next = n;
    }
}
