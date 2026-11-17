// skills.h - TLOU 2013 Factions SURVIVAL SKILLS for t2r-ext (implementation: skills.c).
// Included at the top of t2r-ext.c (ids, value encoding, the few hooks the existing code calls);
// skills.c is included near the end (it uses the ext's helpers).
//
// A skill in a loadout is a 64-bit value, stored where winmm keeps its 3 skill bits (lobby object
// +0x2c78/+0x2c80/+0x2c88) and, for the 4th 2013 skill row, in the ext (g_sk4_cur):
//   SK_TAG | id << 40 | level << 36            (low bits stay 0: winmm's own perk bits)
// winmm's skill record lookup FUN_18001d420 is replaced (sk_lookup), so its GEAR UP edit page and
// LP total read our records (name with level, icon, LP cost). Old winmm skill bits (0x1 Hawk-Eyed,
// 0x2 Head Hunter ...) in saved loadouts are mapped to the closest 2013 skill and migrated.
#pragma once
#define SK_TAG 0x4000000000000000ULL
#define SK_V(id, lv) (SK_TAG | ((uint64_t)(id) << 40) | ((uint64_t)(lv) << 36))
enum {
    SK_NONE,
    // 2013 basic skills
    SK_ZOOM, SK_EXPLO, SK_REVIVER, SK_BRAWLER, SK_COVERT, SK_EARS,
        SK_STRAT, SK_HAWK, SK_CRAFTER,
    SK_FAT, SK_SHARP, SK_MARATHON, SK_COLLECTOR,
    // 2013 DLC skills (Professional, Situational, Risk Management)
    SK_EXEC, SK_DMGMARK, SK_GUNSLINGER, SK_BOMB, SK_SCAV, SK_AWARE,
        SK_FORT, SK_AGILITY,
    SK_LONEWOLF, SK_SECOND, SK_JACK, SK_LUCKY, SK_LETHAL,
    // melee skills (re/melee-skills-1102.md): built on the duel / dodge counter / pin / pull-out / blade-down mechanics
    SK_RIPOSTE, SK_GRIP, SK_BLADE, SK_HEAVY, SK_STEADY,
    SK_FOOTWORK,                // Boxer's Footwork - shorter dodge cooldown
    SK_COUNT
};
#define SK_PICKER_ROWS 32       // rows Skill0..Skill31 in to1/picker-skill (widgets_patch.py add_skill_ui)
// peer messages (ours; winmm's own types are below 0x20, the ext's others are 0x41..0x46)
#define MSG_SK_SYNC   0x50      // [u8 id x4][u8 level x4]: the sender's skills (Covert Training, Reviver ...)
#define MSG_SK_MARKED 0x51      // [u8 slot]: to that slot - you have been marked (Strategist)
#define MSG_SK_HEAL   0x52      // [u8 hp]: to a teammate - First-Aid Training heal tick
#define MSG_SK_HEALFX 0x54      // [u8 healed slot]: to everyone - a First-Aid heal tick happened (observers play its 3D sound on the sender)
#define MSG_SK_HEALANIM 0x55    // [u8 healed slot][u8 1 on / 0 off]: to everyone - the sender's heal hold (observers play the hand-out gesture on its puppet)
#define MSG_SK_GIFT   0x53      // [u8 kind]: giver -> teammate - a Crafter II/III gift (kind = gift.c GIFT_K: health kit / molotov / smoke bomb / nail bomb)
#define MSG_SK_GIFTSTOCK 0x56   // [u8 mask]: to everyone - bit k: this player has room for one more of gift kind k (gift.c)
#define MSG_SK_GIFTACK 0x57     // [u8 1 taken / 0 refused]: receiver -> giver (gift.c)

static int Sk4Cost(const void *lo);    // LP of the 4th skill of a loadout_t (by its address)
static void Sk4OnWrite(const void *lo);       // a loadout_t was copied into the lobby object
static const char *Sk4Name(const void *lo);    // 4th skill's name (NULL = none)
static void R2MirrorFill(const void *lo, int active); // UI round 2 gear up: fills the slot column of the picker pages (loMirror), active = slot row 0..6 framed, -1 none
static void R2MirrorCur(int active);  // the same for the loadout being edited (the lobby object)
static int SkPickerPage(char entered);        // to1/picker-skill opened from the edit page
static void SkillsTick(void);
static void SkillsOnSpawn(void);
static int SkillsRecv(uint32_t slot, uint32_t type, const uint8_t * buf,
                       uint64_t len);
static float SkListenDepleteMul(void);       // Sharp Ears: listen longer
static float SkListenRechargeMul(void);      // Sharp Ears: meter regenerates faster
static float SkReviveTimeMul(void);  // Reviver: revive faster
static int SkPeer(uint32_t slot, int id);      // a peer's effective level of skill id (MSG_SK_SYNC)
static float SkMolotovMul(void);      // Explosion Expert: own molotov kill radius
// Supply box modifiers (supplybox.c, 2013 box script): skills set them in sk_box_mods, called at every box fill
// (defaults: no Lucky Break, multipliers 1). The 2013 box script reads one skill only: Lucky Break.
typedef struct {
    int lucky;                  // Lucky Break level 0..3 (2013 'supply box skill' booster, level I = booster 0)
    float pistol_ammo_mul;      // pistol slot ammo top-up x (Lucky Break I-III: 1.5, rounded up)
    float long_ammo_mul;        // long gun ammo top-up x (Lucky Break II/III: 1.25)
    int ammo_round_fix;         // the small ammo pickup +1 when rounding ate the bonus (Lucky Break, any level)
    int craft_bonus;            // + crafting item cap (2013 skills: none - Lucky Break's caps are in the table)
    float melee_mul;            // melee (2x4) chance x
    float comeback_mul;         // comeback item / weapon chance x
    float health_pct_bias;      // added to the health % the medkit / food thresholds read
} BoxModsT;
static void SkBoxMods(BoxModsT * m);        // skills.c: fill in the equipped skills' box modifiers
static int SkScavPay(uint64_t item, uint64_t * count);        // Scavenger: our own kill's ammo drop (scales count; 1 = auto-picked into the pack, spawn nothing)
static uint64_t SkAwareUntil(void);   // Awareness: enemy nametags stay until this tick (ms), 0 = no
static float SkTagRange(void);        // Awareness II: enemy nametag distance (30 m, 60 m)
static int g_skExhausted;      // sprint meter empty (Marathon Runner / 2013 limited sprint): feel.c's gait code keeps the sprint type on 'run' meanwhile
static int Skl(int id);         // our effective level of skill id
// melee skills (skills.c): the knobs the melee code reads (each fires one "skills: ..." log line when it changes something)
static int SkCounterWindowMs(void);  // Counter Fighter: dcounter.c window after a dodge (600 ms base, 850 ms with the skill)
static int SkCounterCoolMs(void);    // Counter Fighter II: dodge counter cooldown (2200 ms base, 1500 ms)
static int SkCounterLate(void);       // Counter Fighter II: a late (not perfect) dodge also arms the counter
static int SkIronWinMs(int base);    // Iron Grip: the pull-out / pin escape window x1.5 (our own escape, judged on our screen)
static int SkIronFree(const char *what);      // Iron Grip II: 1 once per life - the first pin / pull-out on us is broken for free
static int SkBladeBonus(uint64_t w);  // Blade Discipline: +1 durability on a 1h blade weapon w (0 = none)
static int g_skBladeKitDur;  // Blade Discipline II: the durability the spawn-kit hatchet gets when its pickup is processed (0 = none pending)
static float SkHeavyBleedMul(void);  // Heavy Hands: bleed-out x0.7 when our downer (a synced peer) hit us with fists / a blunt weapon
static int g_skMfVic;         // ctxdown.c: the pair on us is an SP finisher (mf- row); the down it ends in may be lethal (Heavy Hands II)
static int SkHeavyLethal(uint64_t w); // Heavy Hands II: 1 when this blunt-weapon down came out of an SP finisher of a downer with the skill
static int SkSteady(void);
static float SkFootworkMul(void);     // Boxer's Footwork: dodge cooldown x0.9 / 0.85 / 0.8 (melee_dodge.c MD_COOLDOWN_MS)                     // Steady Stance level (clash / parry recovery, weapon takedown wear)
// clan supply perks (clanperk.c, tools/clan_perks.py): the clan's surplus as small match perks
static void CpSetFrom(const char *t); // clan_parse: the perk tail of the state text
static void CpOnSpawn(void);  // skills_on_spawn: free medkit / crafting set / clip
static void CpMatchReset(void);       // the match is being armed: fresh perk counters
static int CpReportLabels(uint8_t * pl, int nl);      // the match report: labels 31 / 32 appended at nl, returns the new nl
static void CpTick(void);      // skills_tick: the member's voice line after the card
static int CpStaminaOn(void); // food surplus: Marathon Runner I sprint recovery
static void CpNoteExhausted(void);    // the sprint meter just emptied (books the stamina perk's use)
static float CpBleedMul(void);        // water surplus: Fortitude I bleed-out time for a player without Fortitude (a use per down)
static const char *CpGearupLine(void);        // "YOUR CLAN SENT: medkit x2 (Tyler)" for the gear up screen ("" = none)
static void FeatureWolfHud(void);     // 2013 Lone Wolf HUD: granted skill icons + "Lone Wolf Active" above the health block (draw tick)
static int SkArmorPrice(int cost);    // Second Chance II: 25 % cheaper armour per two deaths without a down / execution (max 50 %)
static float SkProneMul(void);        // Agility: prone crawl speed (same factor as crouch)
static float SkFortCrawl(void);       // Fortitude: downed crawl speed x1.1 / x1.3
static float SkRespawnEarly(void);    // Strategist III: seconds off our respawn countdown
static int SkStunImmune(uint32_t from);       // Bomb Expert III: our own smoke bomb does not stun us
static float SkOwnFuse(int pipe);     // OUR smoke / stun (pipe = 0) or pipe bomb (pipe = 1) fuse by our Bomb Expert level (own bombs only: t2r-ext.c ob_tick detonates early)
static float SkSmokeFuse(uint32_t slot);      // that thrower's smoke / stun bomb fuse (s): 1.5, Bomb Expert 1.0 / 0.8 / 0.6
