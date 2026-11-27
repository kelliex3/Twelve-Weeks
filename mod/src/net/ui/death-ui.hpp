#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

// Native owner of the victim-POV death screen (ported from tools/ext/death_ui.c). This
// module owns the whole feature: timeline, widgets, edge signals, timing config, the
// killed-by resolver, the black-under-HUD fade, sting playback, spectate UI + lobby
// fields, pause factor and the winmm death-banner hide. The ext is still loaded first
// and mirrors the same widgets, but every death-UI decision and write is made here.
//
// The pure `core` half has no engine / Windows dependency and is unit-tested by
// tests/death_ui_core_test.cpp. The engine-facing half is in death-ui.cpp. The low-level
// GUI2 RVA walk lives in engine/gui2-low.*; the spectate lobby fields in engine/lobby.*.
//
// Natively owned:
//   - death edge / phase machine (idle, dead, respawned, match over), all ext ms constants
//   - nine alpha tracks, rotating SP tip, KILLED BY card, countdown, pause-menu factor
//   - NoteEdge producers for downed lethal / execution kill frame / molotov burn
//   - soft/instant timing + shiv / paired-victim tuned delays (core::ResolveTiming)
//   - GEAR UP-style weapon resolver: exact SID, "-supp" base fallback, missing rows
//   - HUD-under-black child walk (native GUI2 RVA + legacy VA pages) and restore
//   - spectate availability / switch / auto-offer, lobby fields +0x2c44..+0x2c54
//   - opt-out / tuning sidecars read through engine::flag / engine::exe_side
//   - death-banner suppression via net::gui2::SetDeathBannerSuppressed
//
// The raw PCM death sting is compiled in only when TO1_DEATH_STING_PCM is defined and a
// private death_sting_data.hpp supplies the buffer (see net/ui/death-sting.hpp). Without
// it the sink is a no-op: no ext data, no ext PCM path and no fake audio is introduced.

namespace net::ui::death_ui {

    namespace core {

        // ext DU_* constants, exact values.
        inline constexpr uint64_t kDelayMs       = 83;
        inline constexpr uint64_t kCutMs         = 70;
        inline constexpr uint64_t kSoftCutMs     = 700;
        inline constexpr uint64_t kSoftUiMs      = 400;
        inline constexpr uint64_t kUiMs          = 180;
        inline constexpr uint64_t kSpecOutMs     = 300;
        inline constexpr uint64_t kSpecLift      = 350;
        inline constexpr uint64_t kSpecLiftMs    = 500;
        inline constexpr uint64_t kBackMs        = 250;
        inline constexpr float    kFinalAt       = 1.0f;
        inline constexpr uint64_t kFinalMs       = 350;
        inline constexpr uint64_t kRespawnHoldMs = 150;
        inline constexpr uint64_t kRespawnMs     = 350;
        inline constexpr uint64_t kEndMs         = 300;
        inline constexpr float    kSpecMinS      = 1.6f;

        inline constexpr uint64_t kLand(uint64_t delay, uint64_t cut) { return delay + cut; }

        // ---- alpha track -----------------------------------------------------------
        // ext DuTrT: every change ramps from the value on screen, with smoothstep timing.
        inline float RampMs(uint64_t el, int from, int to) noexcept
        {
            if (static_cast<int64_t>(el) <= from) return 0.0f;
            if (static_cast<int64_t>(el) >= to)   return 1.0f;
            const float x = float(int64_t(el) - from) / float(to - from);
            return x * x * (3.0f - 2.0f * x);
        }

        struct Track {
            float    from = 0.0f, to = 0.0f;
            uint64_t t0 = 0;
            int      d0 = 0, d1 = 1;
        };

        inline float Val(const Track& t, uint64_t now) noexcept
        {
            return t.from + (t.to - t.from) *
                RampMs(now > t.t0 ? now - t.t0 : 0, t.d0, t.d1);
        }

        inline void Set(Track& t, float v, uint64_t now) noexcept
        {
            t.from = t.to = v;
            t.t0 = now;
            t.d0 = 0;
            t.d1 = 1;
        }

        inline void Go(Track& t, float to, int delay, int ms, uint64_t now) noexcept
        {
            if (t.to == to) return;
            t.from = Val(t, now);
            t.to = to;
            t.t0 = now;
            t.d0 = delay;
            t.d1 = delay + (ms > 0 ? ms : 1);
        }

        // ext DT_* / DU_PATH order.
        enum TrackId : int {
            Black = 0, Tip, Card, Count, Spec, SpecA, SpecB, Bar, Switch,
            TrackCount
        };
        inline constexpr const char* kTrackPath[TrackCount] = {
            "t2rDeath.dBlack", "t2rDeath.dTip", "t2rDeath.dCard",
            "t2rDeath.dCount", "t2rDeath.dSpec",
            "t2rDeath.dSpec.dSpecLbl", "t2rDeath.dSpec.dSpecLblB",
            "t2rDeath.dSpecBar", "t2rDeath.dSpecBar.dSwitch",
        };

        // ---- tips ------------------------------------------------------------------
        // ext DU_TIPS[33]; image is the SP illustration under
        // streaming/large/death-tips/, text is the SP tip or (NULL) the matching
        // Factions loading-screen tip ported below.
        struct TipRow { const char* img; const char* text; };
        inline constexpr TipRow kTips[] = {
            { "easy-target", "Enemies are deadly accurate when you're charging towards them." },
            { "use-molotov", nullptr },
            { "cover-from-gunfire", "Stay crouched behind cover to protect from gunfire." },
            { "sprint-to-avoid", "Enemies are less accurate while you're sprinting away or to the side." },
            { "weapon-shotgun", nullptr },
            { "general-flank-them", "Don't approach enemies directly, use the environment to flank them." },
            { "buddy-rescue", nullptr },
            { "escape-sneak", "Your foes will investigate your last known location. Don't be there." },
            { "crouch-prone-aim", "Aim more accurately by crouching, or even more accurately by going prone." },
            { "ellie-in-backpack", nullptr },
            { "general-dont-get-flanked", "Look around to track enemy movements to ensure you aren't caught off guard." },
            { "silent-bow", nullptr },
            { "use-health-kit", "Using a health kit takes time. Make sure you're in a safe place." },
            { "overwhelmed", nullptr },
            { "hide", "In combat, use your environment to lose your enemies and slip back into stealth." },
            { "stand-aim-rifle", "Moving while aiming reduces your accuracy. Stand still to steady your aim." },
            { "scavenge-items", nullptr },
            { "stealth-explosives", "Arc-throw a Molotov while in low cover to stay hidden while throwing." },
            { "grass-crouch-prone", nullptr },
            { "general-vary-your-tactics", "If your usual strategy isn't working, vary your tactics. Try a new weapon or crafting recipe." },
            { "weapon-bullet-drop", "Bullets drop with gravity. Aim higher to compensate." },
            { "shooting-armor-legs", nullptr },
            { "melee-weapon-types", "One-handed melee weapons are fast and durable. Two-handed melee weapons are slower but deal massive damage." },
            { "sprint-base", "Buy yourself time to heal wounds or craft by running away from combat." },
            { "use-pipe-bombs", "Pipe bombs will land at an opponent's feet when quick-thrown." },
            { "vulnerable-enemies-5-6", nullptr },
            { "player-upgrade-stun-bomb-smoke", "Smoke bomb clouds completely block enemy vision, hiding your movements and preventing them from firing on you." },
            { "full-draw-bow", "Arrows do more damage when the bow is fully drawn." },
            { "limited-supplies", nullptr },
            { "upgraded-melee", "Upgraded melee weapons are restored to full durability and have greatly increased damage." },
            { "glass-break", "Breaking glass windows can open up new routes in combat, but the sound will draw enemy attention." },
            { "crouch-beside-enemy", "You have the choice to engage your opponents quietly, by force, or sometimes avoid combat altogether." },
            { "buddy-rescue", "Marking a downed teammate and running towards them doesn't deplete your stamina limit." },
        };
        inline constexpr int kTipCount = int(sizeof(kTips) / sizeof(kTips[0]));

        // ext MF_TIPS[n][1], the 11 rows a NULL DU_TIPS entry points at.
        inline constexpr const char* kMfTips[15] = {
            "The Molotov is great for flushing the enemy out of cover.",
            "Shotguns are deadly up close but fall off fast. Keep moving and close the distance.",
            "A suppressor keeps your shots quiet. Pick your fights.",
            "The bow is silent. Pick your arrows back up after the fight.",
            "Crafting takes time. Make medkits and bombs before the fight, not during it.",
            "Supply boxes refill over time. Remember where they are.",
            "Armor soaks up damage from every direction. Buy it early.",
            "A downed teammate can be revived. Stay close and watch their back.",
            "Mark an enemy to show your whole team where they are.",
            "Stick together. A lone survivor is easy prey for the other faction.",
            "Stay low in the grass. A prone survivor is hard to spot.",
            "Listen mode shows you footsteps. Moving slowly keeps you off it.",
            "Finishing a downed enemy earns more parts. Watch for his friends.",
            "Headshots down enemies faster. Take your time and aim.",
            "Every survivor in your clan eats. Gather supplies in matches to keep them fed.",
        };
        // ext DU_TIP_MF, -1 = an SP text (already in kTips).
        inline constexpr signed char kTipMf[kTipCount] = {
            -1, 0, -1, -1, 1, -1, 7, -1, -1, 4, -1, 3, -1, 9, -1, -1, 5, -1, 10,
            -1, -1, 6, -1, -1, -1, 12, -1, -1, 14, -1, -1, -1, -1,
        };
        static_assert(sizeof(kTipMf) == kTipCount, "death-ui tip table out of step");

        inline const char* TipText(int i) noexcept
        {
            if (i < 0 || i >= kTipCount) return "";
            if (kTips[i].text) return kTips[i].text;
            const int mf = kTipMf[i];
            return kMfTips[mf >= 0 && mf < int(sizeof(kMfTips) / sizeof(kMfTips[0])) ? mf : 0];
        }

        inline void TipImage(int i, char* out, size_t cap) noexcept
        {
            static constexpr const char* kPrefix = "streaming/large/death-tips/";
            const char* name = (i >= 0 && i < kTipCount) ? kTips[i].img : "";
            size_t n = 0;
            for (const char* p = kPrefix; *p && n + 1 < cap; ++p) out[n++] = *p;
            for (const char* p = name; *p && n + 1 < cap; ++p) out[n++] = *p;
            for (const char* p = ".png"; *p && n + 1 < cap; ++p) out[n++] = *p;
            out[n] = '\0';
        }

        // ---- killed-by weapon (static part of ext DuWeapon) -------------------------
        struct WeaponRow { const char* id; const char* name; const char* icon; };
        inline constexpr WeaponRow kWeapons[] = {
            { "t2r-fall", "FALL", nullptr },
            { "shiv", "SHIV", "hud/weapons/consumables/shiv.png" },
            { "molotov", "MOLOTOV", "hud/weapons/consumables/molotov.png" },
            { "pipe-bomb", "NAIL BOMB", "hud/weapons/consumables/pipe-bomb.png" },
            { "smoke-bomb", "SMOKE BOMB", "hud/weapons/consumables/smoke-bomb.png" },
            { "trap-mine", "TRAP MINE", "hud/weapons/consumables/trap-bomb.png" },
            { "crowbar", "CROWBAR", "hud/weapons/melee/pickup/crowbar-one-hand.png" },
            { "lead-pipe", "PIPE", "hud/weapons/melee/pickup/lead-pipe.png" },
            { "short-pipe", "PIPE", "hud/weapons/melee/pickup/lead-pipe.png" },
            { "pipe-wrench", "PIPE WRENCH", "hud/weapons/melee/pickup/pipe-wrench.png" },
            { "hammer-modern", "HAMMER", "hud/weapons/melee/pickup/hammer-modern.png" },
            { "hammer-scar", "HAMMER", "hud/weapons/melee/pickup/hammer-scar.png" },
            { "club-1h-police", "BATON", "hud/weapons/melee/pickup/club-1h-police.png" },
            { "club-1h-scar", "CLUB", "hud/weapons/melee/pickup/club-1h-scar.png" },
            { "machete", "MACHETE", "hud/weapons/melee/pickup/machete-wood.png" },
            { "machete-scar", "MACHETE", "hud/weapons/melee/pickup/machete-scar.png" },
            { "tomahawk", "HATCHET", "hud/weapons/melee/pickup/tomahawk.png" },
            { "hatchet", "HATCHET", "hud/weapons/melee/pickup/hatchet-modern.png" },
            { "hatchet-modern", "HATCHET", "hud/weapons/melee/pickup/hatchet-modern.png" },
            { "two-by-four", "2X4", "hud/weapons/melee/pickup/two-by-four.png" },
            { "baseball-bat", "BASEBALL BAT", "hud/weapons/melee/pickup/baseball-bat.png" },
            { "fireman-axe", "AXE", "hud/weapons/melee/pickup/fireman-axe.png" },
            { "bow-ellie", "BOW", "hud/weapons/guns/bow-ellie/bow.png" },
            { "bow-explosive", "EXPLOSIVE BOW", "hud/weapons/consumables/arrow-explosive.png" },
            { "crossbow", "CROSSBOW", "hud/weapons/guns/crossbow/crossbow.png" },
            { "flamethrower", "FLAMETHROWER", "hud/weapons/guns/flamethrower/flamethrower.png" },
            { "pistol-beretta", "9MM PISTOL", "hud/weapons/guns/pistol-beretta/beretta.png" },
            { "pistol-glock", "BURST PISTOL", "hud/weapons/guns/pistol-glock/glock.png" },
            { "pistol-revolver-taurus", "REVOLVER", "hud/weapons/guns/pistol-revolver-taurus/revolver-brl.png" },
            { "pistol-xcaliber", "HUNTING PISTOL", "hud/weapons/guns/pistol-xcaliber/xcaliber.png" },
            { "rifle-assault-firefly", "ASSAULT RIFLE", "hud/weapons/guns/rifle-assault-firefly/rifle-assault-firefly.png" },
            { "rifle-assault-firefly-ellie-player", "ASSAULT RIFLE", "hud/weapons/guns/rifle-assault-firefly/rifle-assault-firefly.png" },
            { "rifle-galil", "SPECTER", "hud/weapons/guns/rifle-galil/galil.png" },
            { "rifle-mpx5", "FULL-AUTO RIFLE", "hud/weapons/guns/rifle-mpx5/mpx5.png" },
            { "rifle-remington-bolt", "HUNTING RIFLE", "hud/weapons/guns/rifle-remington-bolt/remington-bolt.png" },
            { "rifle-vepr", "VARIABLE RIFLE", "hud/weapons/guns/rifle-vepr/vepr-scp.png" },
            { "shotgun-baserri-over-under", "DOUBLE BARREL", "streaming/t2r/icons/double-barrel.png" },
            { "shotgun-remington-pump", "PUMP SHOTGUN", "hud/weapons/guns/shotgun-remington-pump/remington-pump.png" },
            { "shotgun-tactical", "TACTICAL SHOTGUN", "streaming/t2r/icons/tac-shotgun.png" },
            { "rifle-m14-semi", "SEMI-AUTO RIFLE", "streaming/t2r/icons/m14.png" },
            { "rifle-burst", "BURST RIFLE", "streaming/t2r/icons/burst-rifle.png" },
            { "pistol-cz75-enforcer", "ENFORCER", "streaming/t2r/icons/cz75.png" },
            { "pistol-el-diablo", "EL DIABLO", "streaming/t2r/icons/el-diablo.png" },
            { "shotgun-shorty", "SHORTY", "streaming/t2r/icons/shorty.png" },
            { "rifle-frontier", "FRONTIER RIFLE", "streaming/t2r/icons/frontier.png" },
            { "rifle-milsniper", "MILITARY SNIPER", "streaming/t2r/icons/milsniper.png" },
        };

        inline constexpr const char* kFistIds[] = {
            "abby-unarmed", "ellie-unarmed", "unarmed", "fists", "fist",
        };

        // ext CLONED_BASE / BaseWeapon(): the loadout's "<gun>-supp" rows share the
        // base gun's name and icon. The dynamic GEAR UP table is ext-owned memory, so
        // the native resolver applies the same mapping over this static table instead.
        inline constexpr const char* kClonedBases[] = {
            "rifle-vepr", "rifle-remington-bolt", "pistol-beretta",
            "pistol-glock", "rifle-mpx5", "rifle-m14-semi",
            "pistol-cz75-enforcer", "shotgun-tactical", "rifle-burst",
            "rifle-frontier",
        };

        // FNV-1a 64, the engine's SID()/ext fnv64. Kept local so the pure core has no
        // dependency on the game's string-id helper.
        constexpr uint64_t Fnv64(const char* s) noexcept
        {
            uint64_t h = 14695981039346656037ULL;
            if (!s) return h;
            for (; *s; ++s) { h ^= static_cast<unsigned char>(*s); h *= 1099511628211ULL; }
            return h;
        }

        inline bool Streq(const char* a, const char* b) noexcept
        {
            if (!a || !b) return false;
            while (*a && *b) { if (*a++ != *b++) return false; }
            return *a == *b;
        }

        struct WeaponInfo {
            const char* name = nullptr;
            const char* icon = nullptr;
            bool found = false;
            bool base = false;   // resolved through the "-supp" -> base-gun fallback
        };

        // ext BaseWeapon(): strip the "-supp" clone suffix by mapping it to its base.
        inline uint64_t BaseWeaponId(uint64_t h) noexcept
        {
            if (!h) return 0;
            for (const char* base : kClonedBases) {
                char clone[64];
                size_t n = 0;
                for (const char* p = base; *p && n + 6 < sizeof clone; ++p) clone[n++] = *p;
                clone[n++] = '-'; clone[n++] = 's'; clone[n++] = 'u';
                clone[n++] = 'p'; clone[n++] = 'p'; clone[n] = '\0';
                if (Fnv64(clone) == h) return Fnv64(base);
            }
            return h;
        }

        inline WeaponInfo Weapon(const char* id, uint64_t sid = 0) noexcept
        {
            WeaponInfo r;
            if ((!id || !*id) && !sid) return r;
            const uint64_t h = sid ? sid : Fnv64(id);
            for (const char* f : kFistIds)
                if (Fnv64(f) == h || Streq(id, f))
                    return { "FISTS", "streaming/t2r/icons/fist.png", true, false };

            // Two passes like ext DuWeapon: the exact credit id, then BaseWeapon().
            for (int pass = 0; pass < 2; ++pass) {
                const uint64_t k = pass ? BaseWeaponId(h) : h;
                if (pass && k == h) break;
                for (const WeaponRow& w : kWeapons)
                    if (Fnv64(w.id) == k || Streq(id, w.id))
                        return { w.name, w.icon, true, pass != 0 };
            }
            // ext falls through to FISTS for an unknown non-gun credit.
            return { "FISTS", "streaming/t2r/icons/fist.png", false, false };
        }

        // ---- death edge sources -----------------------------------------------------
        // ext du_edge_note's three private producers, now native. HpState is the
        // always-available hp / Death-state rising edge; the others come from NoteEdge.
        enum class EdgeKind : int {
            HpState = 0,        // hp <= 0 / engine Death state
            DownedLethal,       // shot / melee finished us while downed
            Execution,          // paired-move / execution kill frame
            MolotovBurn,        // ext-style lethal burn
        };

        struct EdgeNote {
            EdgeKind kind = EdgeKind::HpState;
            uint64_t atMs = 0;           // producer clock (monotonic ms)
            int      killerSlot = -1;
            uint64_t weapon = 0;
        };

        // ---- timing decision (pure) --------------------------------------------------
        struct Timing {
            int  delayMs = int(kDelayMs);
            int  cutMs   = int(kCutMs);
            int  uiMs    = int(kUiMs);
            bool soft    = false;
        };

        // ext's death-timing decision with native producers. Soft lead comes from the
        // tuning sidecar / death-reaction clip, execution deaths stay instant, and a
        // shiv / paired victim holds the black so the sting / victim half can finish.
        inline Timing ResolveTiming(bool softMode, int leadMs, bool forceInstant,
                                    bool execution, bool shivVictim, int shivStingMs,
                                    int pairedLeftMs, bool pairHoldOld,
                                    bool execEndNew = true,
                                    int delayMsOverride = -1) noexcept
        {
            Timing t;
            // t2r-death-delay.txt: fixed instant-mode delay override (0..3000 ms).
            // Applied before the soft-mode block so soft mode still wins when enabled.
            if (delayMsOverride >= 0 && !softMode && !forceInstant && !execution) {
                t.delayMs = delayMsOverride > 3000 ? 3000 : delayMsOverride;
            }
            if (softMode && !forceInstant && !execution && leadMs > 0) {
                t.delayMs = leadMs > 3000 ? 3000 : leadMs;
                t.cutMs = int(kSoftCutMs);
                t.uiMs = int(kSoftUiMs);
                t.soft = true;
            }
            // Shiv victim: wait for the shock sting + 0.5 s, clamped 1..2.5 s.
            if (!forceInstant && shivVictim) {
                int d = shivStingMs + 500;
                if (d < 1000) d = 1000;
                if (d > 2500) d = 2500;
                if (d > t.delayMs) t.delayMs = d;
            }
            // Paired-move victim: hold at most 1.2 s, keep a 350 ms margin so the
            // black lands while the body still moves. ps45 execution removes the margin.
            if (!forceInstant && !shivVictim && pairedLeftMs > 0) {
                const int hold = pairedLeftMs < 1200 ? pairedLeftMs : 1200;
                int cap = pairedLeftMs - t.cutMs - (execEndNew ? 0 : 350);
                if (cap < 250) cap = 250;
                const int d = hold < cap ? hold : cap;
                if (!pairHoldOld && d > t.delayMs) t.delayMs = d;
            }
            return t;
        }

        // ---- state machine ----------------------------------------------------------
        struct Inputs {
            bool  matchLive = false;      // engine match substate == 6 / liveness::Live()
            bool  matchOver = false;      // end banner / liveness::MatchOver()
            bool  alive = false;          // hp > 0 and not in Death
            bool  dead = false;           // hp <= 0 or in Death
            bool  deadEdge = false;       // the frame hp reaches 0 / Death begins
            bool  execution = false;      // execution death: instant mode (private camera)
            bool  pauseMenu = false;      // to1/pause-menu on the HUD stack
            bool  softMode = false;       // opt-in producer, default false (no sidecar read)
            int   leadMs = 0;             // soft-mode lead, clamped by the caller
            bool  respawnRunning = false; // mod's own respawn timer
            float respawnRemaining = 0.0f;
            bool  outOfLives = false;     // mod's elimination state
            int   spectateTarget = -1;    // -1 = the mod's spectate owner is not watching

            // Native-owned edge / timing / spectate producers (replacing ext gates).
            bool     edgeSignalled = false;  // a NoteEdge signal is newer than the last one used
            uint64_t edgeAtMs = 0;           // its own timestamp (the ext's "first signal wins" clock)
            bool     forceInstant = false;   // t2r-death-screen-instant.txt
            bool     shivVictim = false;     // paired victim of a shiv move
            int      shivStingMs = 0;        // shivsting::RemainingMs()
            int      pairedLeftMs = -1;      // victim-half ms left (<0 = none)
            bool     pairHoldOld = false;    // t2r-death-pair-hold-old.txt
            bool     execEndNew = true;      // !t2r-death-exec-end-ps45-old.txt
            int      spectateCandidates = 0; // living watchable teammates
            bool     spectateButton = false; // interact / E rising edge
            int      spectateSwitch = 0;     // +1 next / -1 prev rising edge
            int      delayMsOverride = -1;   // t2r-death-delay.txt fixed delay (-1 = none)
        };

        struct Outputs {
            bool  active = false;
            bool  hideDeathBanner = false;
            bool  exit = false;             // hide the root, reset native producer notes
            bool  resetRecords = false;     // respawn completed: drop killer records
            bool  setTip = false;
            int   tipIndex = 0;
            bool  fillCard = false;
            bool  setCount = false;
            int   seconds = -1;
            bool  noLives = false;
            bool  stingDue = false;         // no-op sink (private PCM gated)
            bool  spectating = false;
            bool  showSpecBar = false;
            bool  showSwitch = false;
            bool  spectateOn = false;      // rising edge: write the lobby spectate fields on
            bool  spectateOff = false;     // falling edge: restore them
            int   spectateSwitchDir = 0;   // +1 / -1: switch the lobby target
            bool  hideHud = false;         // drive the native HUD-under-black fade
            bool  softStingAtEdge = false; // soft mode: play the sting at the death edge
            float tracks[TrackCount] = {};
            float black = 0.0f;
        };

        struct State {
            int      phase = 0;              // 0 idle, 1 dead, 2 respawned, 3 match over
            uint64_t t0 = 0, phT0 = 0, finalT0 = 0, specT0 = 0;
            uint64_t mateLostT0 = 0, edgeSrcMs = 0;
            bool     armed = false;
            bool     wasDead = false;
            bool     seenDead = false, blackWait = false, leadFit = false;
            bool     spec = false, final = false, sting = false, filled = false;
            bool     loggedBlack = false, uiIn = false, autoSpec = false;
            int      cnt = -2;
            int      tip = 0;                // 1-based rotation, survives Reset()
            float    pf = 1.0f;
            uint64_t pfT = 0;
            int      delayMs = int(kDelayMs), cutMs = int(kCutMs), uiMs = int(kUiMs);
            Track    tr[TrackCount];
        };

        // Full clear (match left / session reset). The tip rotation survives like ext's
        // separate g_duTip; `armed` does not (ext clears it on leaving substate 6).
        inline void Reset(State& s, uint64_t now) noexcept
        {
            const int tip = s.tip;
            s = State{};
            s.tip = tip;
            s.cnt = -2;
            s.pf = 1.0f;
            s.pfT = now;
            for (Track& t : s.tr) Set(t, 0.0f, now);
        }

        // ext DuReset(0) at a death edge: tracks and per-death flags restart, but the
        // match's armed latch and the tip rotation are kept.
        inline void Begin(State& s, uint64_t now) noexcept
        {
            const int tip = s.tip;
            const bool armed = s.armed;
            Reset(s, now);
            s.tip = tip;
            s.armed = armed;
        }

        inline void Finish(const State& s, Outputs& out, uint64_t now) noexcept
        {
            out.active = true;
            out.hideDeathBanner = true;
            out.spectating = s.spec;
            for (int i = 0; i < TrackCount; ++i)
                out.tracks[i] = Val(s.tr[i], now) * s.pf;
            out.black = out.tracks[Black];
            out.hideHud = out.black > 0.001f;
        }

        // A frame that leaves the screen must not also claim it is visible: the native
        // side hides the root + releases the banner latch on `exit`.
        inline void Exit(State& s, Outputs& out, uint64_t now) noexcept
        {
            Reset(s, now);
            out.exit = true;
            out.active = false;
            out.hideDeathBanner = false;
            out.spectating = false;
        }

        // Advance one frame. All times are GetTickCount64-style monotonic ms.
        inline void Step(State& s, const Inputs& in, Outputs& out, uint64_t now) noexcept
        {
            out = Outputs{};

            if (!in.matchLive) {
                if (s.phase) out.exit = true;
                Reset(s, now);
                return;
            }

            // ext DuPauseFactor: ~120 ms to hide under the pause menu and back.
            {
                const float dt = s.pfT ? float(now - s.pfT) : 16.0f;
                s.pfT = now;
                float pf = s.pf + (in.pauseMenu ? -dt / 120.0f : dt / 120.0f);
                if (pf < 0.0f) pf = 0.0f;
                if (pf > 1.0f) pf = 1.0f;
                s.pf = pf;
            }

            s.armed = s.armed || in.alive;
            // Native du_edge_note producers win over the hp / Death fallback. The
            // signal's own timestamp starts the clock (ext's "first signal wins").
            const bool signal = in.edgeSignalled && in.edgeAtMs != s.edgeSrcMs;
            const bool edge = (signal || (in.deadEdge && !s.wasDead));
            s.wasDead = in.dead;

            if (edge && s.armed && !in.matchOver && (s.phase == 0 || s.phase == 2)) {
                Begin(s, now);
                s.phase = 1;
                s.t0 = signal && in.edgeAtMs && in.edgeAtMs < now
                    ? in.edgeAtMs : now;
                s.edgeSrcMs = signal ? in.edgeAtMs : 0;
                s.phT0 = now;
                s.cnt = -2;
                s.seenDead = in.dead;

                const Timing t = ResolveTiming(
                    in.softMode, in.leadMs, in.forceInstant,
                    in.execution,
                    in.shivVictim, in.shivStingMs, in.pairedLeftMs,
                    in.pairHoldOld, in.execEndNew, in.delayMsOverride);
                s.delayMs = t.delayMs;
                s.cutMs = t.cutMs;
                s.uiMs = t.uiMs;
                out.softStingAtEdge = t.soft;
                if (t.soft) { s.sting = true; out.stingDue = true; }
                s.leadFit = 0;

                if (in.dead)
                    Go(s.tr[Black], 1.0f, s.delayMs, s.cutMs, now);
                else
                    s.blackWait = true;

                s.tip = s.tip % kTipCount + 1;
                out.setTip = true;
                out.tipIndex = (s.tip - 1 + kTipCount) % kTipCount;
                Finish(s, out, now);
                return;
            }

            if (!s.phase) return;
            if (s.phase == 1 && !s.seenDead && in.dead) s.seenDead = true;

            const uint64_t el = now - s.t0;

            // A signalled-but-unconfirmed death waits up to 600 ms for hp / Death;
            // without the private signal this only runs if the edge saw hp not yet 0.
            if (s.blackWait) {
                if (in.dead) {
                    s.blackWait = false;
                    const uint64_t d = el > uint64_t(s.delayMs) ? uint64_t(s.delayMs) : el;
                    s.t0 = now - d;
                    Go(s.tr[Black], 1.0f, int(uint64_t(s.delayMs) - d), s.cutMs, now);
                } else if (el > 600 || in.matchOver) {
                    Exit(s, out, now);
                    return;
                } else {
                    Finish(s, out, now);
                    return;
                }
            }

            // Match end wins: the whole death screen leaves for the end banner.
            if (in.matchOver && s.phase != 3) {
                s.phase = 3;
                s.phT0 = now;
                for (Track& t : s.tr) Go(t, 0.0f, 0, int(kEndMs), now);
            }
            if (s.phase == 3) {
                if (now - s.phT0 > kEndMs + 50) {
                    Exit(s, out, now);
                } else {
                    Finish(s, out, now);
                }
                return;
            }

            // A signalled death that never showed in hp / state is dropped.
            if (s.phase == 1 && !s.seenDead && now - s.t0 > 4000) s.seenDead = true;

            if (in.alive && s.phase == 1 && s.seenDead) {
                s.phase = 2;
                s.phT0 = now;
                for (int i = Tip; i < TrackCount; ++i) Go(s.tr[i], 0.0f, 0, 120, now);
                Go(s.tr[Black], 0.0f, int(kRespawnHoldMs), int(kRespawnMs), now);
            }
            if (s.phase == 2) {
                if (now - s.phT0 > kRespawnHoldMs + kRespawnMs + 50) {
                    Exit(s, out, now);
                    out.resetRecords = true;
                } else {
                    Finish(s, out, now);
                }
                return;
            }

            const uint64_t land = kLand(uint64_t(s.delayMs), uint64_t(s.cutMs));

            // Death sting at the land (soft mode already fired it at the edge).
            if (!s.sting && el >= land) {
                s.sting = true;
                out.stingDue = true;
            }
            if (!s.filled && el + 100 >= land) {
                s.filled = true;
                out.fillCard = true;
            }
            if (el >= land && !s.uiIn && !s.final && !s.spec) {
                s.uiIn = true;
                Go(s.tr[Tip], 1.0f, 0, s.uiMs, now);
                Go(s.tr[Card], 1.0f, 0, s.uiMs, now);
                Go(s.tr[Count], 1.0f, 0, s.uiMs, now);
            }

            // Countdown from the mod's own respawn timer; the label's "no
            // reinforcements" state only when the mod says the team is eliminated.
            const bool run = in.respawnRunning;
            const float rem = in.respawnRemaining;
            const bool nolives = !run && in.outOfLives;
            out.setCount = true;
            out.seconds = run ? int(std::ceil(rem)) : -1;
            out.noLives = nolives;

            if (!s.final && run && rem <= kFinalAt && el >= land && !nolives) {
                s.final = true;
                s.finalT0 = now;
                Go(s.tr[Black], 1.0f, 0, int(kFinalMs), now);
                for (int i = Tip; i < TrackCount; ++i)
                    if (i != SpecA && i != SpecB)
                        Go(s.tr[i], 0.0f, 0, int(kFinalMs) - 50, now);
            }
            if (s.final && !run && !in.alive && now - s.finalT0 > 3000) {
                s.final = false;
                s.uiIn = false;
                Go(s.tr[Black], 1.0f, 0, 100, now);
            }

            // Spectate: the mod's native owner supplies the target, this core owns the
            // presentation and the on/off + switch edges (native glue writes the lobby).
            const bool watching = in.spectateTarget >= 0;
            const bool prevSpec = s.spec;
            if (s.spec != watching) {
                s.spec = watching;
                s.specT0 = now;
                if (s.spec) {
                    Go(s.tr[Tip], 0.0f, 0, int(kSpecOutMs), now);
                    Go(s.tr[Card], 0.0f, 0, int(kSpecOutMs), now);
                    Go(s.tr[Black], 0.0f, int(kSpecLift), int(kSpecLiftMs), now);
                    Go(s.tr[SpecA], 0.0f, 0, 200, now);
                    Go(s.tr[SpecB], 1.0f, 150, 200, now);
                } else {
                    Go(s.tr[Black], 1.0f, 0, int(kBackMs), now);
                    Go(s.tr[Tip], 1.0f, int(kBackMs), s.uiMs, now);
                    Go(s.tr[Card], 1.0f, int(kBackMs), s.uiMs, now);
                    Go(s.tr[SpecA], 1.0f, 150, 200, now);
                    Go(s.tr[SpecB], 0.0f, 0, 200, now);
                }
            }
            out.showSpecBar = s.spec && watching;
            out.showSwitch = s.spec && in.spectateCandidates >= 2 && !s.final;
            out.spectateOn = s.spec && !prevSpec;
            out.spectateOff = !s.spec && prevSpec;
            out.spectateSwitchDir = (s.spec && now - s.specT0 > 500 &&
                in.spectateSwitch != 0) ? in.spectateSwitch : 0;
            Go(s.tr[Spec], s.spec ? 1.0f : 0.0f, 0, 250, now);
            Go(s.tr[Bar], out.showSpecBar ? 1.0f : 0.0f, 0,
               out.showSpecBar ? 250 : 150, now);
            Go(s.tr[Switch], out.showSwitch ? 1.0f : 0.0f, 0, 200, now);

            Finish(s, out, now);
        }

    } // namespace core

    // ---- engine-facing API (death-ui.cpp) ---------------------------------------

    // Once per frame from boot::FrameTick, after liveness and NetTick.
    void Tick(uint64_t nowMs);

    // Session reset (called from net-replication's Reset).
    void Reset();

    // Producers from the mod's own death/respawn/spectate owners. Values are copied.
    void NoteDeath(int killerSlot, uint64_t weaponSid, const char* killerName);
    void NoteRespawn(bool running, float remaining, bool outOfLives);
    void NoteSpectate(int targetSlot, const char* targetName);

    // Native du_edge_note producers (downed lethal / execution kill frame / burn).
    // atMs is the producer's monotonic clock; 0 means "now". The newest note wins and
    // is consumed once. HpState needs no note (the hp / Death edge is always read).
    void NoteEdge(core::EdgeKind kind, uint64_t atMs, int killerSlot, uint64_t weapon);

    // Living, watchable teammate slots (ext DuMates). Drives the spectate offer and
    // the switch prompt; the native glue also uses them to cycle the lobby target.
    void NoteSpectateOptions(const int* slots, int count);

    bool Active() noexcept;

    // Last settled state from the owner, for the death-theme drone: phase 0 idle,
    // 1 dead, 2 respawned, 3 match over; final = respawn countdown fade-to-black.
    int  Phase() noexcept;
    bool Final() noexcept;

    // The last computed HUD-under-black fade (0..1). Consumers such as the icon
    // kill feed need the same value the death UI applied that frame.
    float Black() noexcept;

} // namespace net::ui::death_ui
