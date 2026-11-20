// ported from tools/ext/t2r-ext.c (MusicEvent + the tier derivation in FeatureCombatMusic;
// the ext's stinger layer g_msDead read in the surviving-music section)

#include "music-tier.hpp"

#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/local-player.hpp"
#include "engine/script.hpp"

#include "player-status.hpp"

#include "net/match/match.hpp"
#include "net/match/net-player.hpp"

#include "log.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace net::audio::music_tier {

    namespace {

        constexpr float   kHeatCap      = 30.0f;   // ext: g_musHeat clamped to 30
        constexpr uint64_t kHotEndMs     = 900;     // ext NR_END_MS
        constexpr uint64_t kDwellMs      = 6500;    // ext: minimum combat dwell (t2r-music-dwell-old.txt disables)
        constexpr uint64_t kCalmDownMs   = 4000;    // ext: calm 4 s (contact lost), NR-off path
        constexpr uint64_t kHoldMs       = 1500;    // ext: g_msHoldUntil after death

        float    g_heat       = 0.0f;
        uint64_t g_evtMs      = 0;
        uint64_t g_awareMs    = 0;
        uint64_t g_contactMs  = 0;
        uint64_t g_hitRxMs    = 0;   // ext g_musHitRxMs: a relayed hit we took
        uint64_t g_holdUntil  = 0;
        uint64_t g_tierAt     = 0;
        uint64_t g_lastMs     = 0;
        uint64_t g_calmSince  = 0;
        uint64_t g_logMs      = 0;
        int      g_tier       = 0;
        bool     g_dead       = false;
        bool     g_inMatch    = false;
        bool     g_wasFiring  = false;
        bool     g_wasMelee   = false;
        bool     g_wasPaired  = false;
        float    g_lastHp     = -1.0f;
        char     g_cause[64]  = "?";
        char     g_awareWhy[48] = "?";

        // ext FeatureCombatMusic's floor layer (floor_heat / floor_held / floor_held_at).
        // A floor is a heat contribution derived from the world this frame, not from an event;
        // it is held for 6 s so a sight / contact does not blink between frames.
        float    g_floorHeat   = 0.0f;
        uint64_t g_floorHeldAt = 0;
        char     g_floorCause[48] = "";
        uint64_t g_floorAwareMs = 0;    // ext nr_hot_at from aware_now / exec_near (a world sign)
        bool     g_closePrev[kMaxNetPlayers] = {};   // ext close_prev[slot]
        bool     g_lastStand  = false;  // ext last_stand (our team out of lives)
        bool     g_lms        = false;  // ext lms (last man standing)
        float    g_remainSec  = -1.0f;  // ext MusicMatchRem(); -1 = clock unknown / not running

        // ext MusicEvent's awareness list. These set nr_hot_at / the "enemy is aware" clock.
        // Our own shots / melee swings are deliberately NOT in it (ext: "merely being near /
        // a mark / a downed enemy / our own shots is not [awareness]").
        bool IsAwareCause(const char* c)
        {
            static const char* const kAware[] = {
                "spotted by an enemy (spot call)", "we took a hit", "we hit an enemy",
                "enemy shot heard", "enemy fire continues", "enemy fired near",
                "enemy at close quarters", "we are stunned", "enemy stunned",
                "enemy revived",
            };
            for (const char* a : kAware)
                if (std::strcmp(c, a) == 0)
                    return true;
            return false;
        }

        bool NrRules()
        {
            // ext: nr = !t2r-music-nr-rules-old.txt (cached per call site there; the flag cache
            // here does the same without touching the disk every frame).
            return engine::flag::Absent("t2r-music-nr-rules-old.txt");
        }

        void Log(float add, const char* cause)
        {
            const uint64_t now = GetTickCount64();
            if (now - g_logMs <= 250)
                return;
            g_logMs = now;
            TO1_LOG("[MUSIC-TIER] input +%.1f %s (heat %.1f, tier %d)\n",
                add, cause, g_heat, g_tier);
        }

        // The world half of ext FeatureCombatMusic's per-frame peer scan. The mod can observe
        // peer net positions, team, death / mark flags, lives and the clock without any new
        // hook, so the enemy-aware floor below is a real port of the ext's floor_heat rule.
        //
        // HONEST GATE: the ext's `seen` also uses a camera-frustum + line-of-sight ray (its
        // MusicSlotSeen / TagScreen / g_tagHidden) and a forward cone (MusicSlotCos, 1-8 m).
        // The mod has no equivalent camera hook, so `seen` here is the mark flag (the ext
        // counts a marked enemy as seen at any range) plus the within-5 m close quarters test.
        // The d<=8 m forward-cone case is intentionally not produced; an enemy that approaches
        // from outside 5 m without a mark does not raise the floor until contact. A future
        // camera/LOS port feeds that into NoteEvent("enemy at close quarters") / the floor.
        struct WorldScan {
            int   seenNear    = 0;   // alive enemies seen within 30 m
            int   seenClose   = 0;   // ... within 15 m
            int   seenFar     = 0;   // seen / marked beyond 30 m
            int   aliveNear   = 0;   // alive enemies within 30 m
            int   near8       = 0;   // alive enemies in close quarters
            int   downNear    = 0;   // downed enemies within 30 m, finisher pending
            int   execNear    = 0;   // enemies in a paired move within 30 m
            int   matesAlive  = 0;   // teammates not dead (a downed mate can still be revived)
            bool  awareNow    = false; // an enemy sees us / is on us (NR hot sign)
            float nearestAlive = 1e9f;
        };

        void ScanWorld(WorldScan& out)
        {
            const ::net::match::Match& m = ::net::match::Get();
            const NetPlayer* local = NetPlayers::Local();
            if (!local || !local->hasState)
                return;
            const PlayerNetState& ls = local->State();

            for (int slot = 0; slot < kMaxNetPlayers; ++slot)
                g_closePrev[slot] = false;
            for (int slot = 0; slot < kMaxNetPlayers; ++slot) {
                const NetPlayer& p = m.players[slot];
                if (!p.active || p.IsLocal())
                    continue;
                const bool mate = NetPlayers::SlotIsFriendly(slot);
                const bool dead = p.dead || ::net::player_status::PuppetDeadHere(slot);
                const bool down = ::net::player_status::PuppetDowned(slot);
                if (mate) {
                    if (!dead)
                        ++out.matesAlive;
                    continue;
                }
                const PlayerNetState& s = p.State();
                const float dx = s.posX - ls.posX;
                const float dy = s.posY - ls.posY;
                const float dz = s.posZ - ls.posZ;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);

                if (down) {
                    if (d < 30.0f)
                        ++out.downNear;
                    continue;
                }
                if (::net::player_status::LocalInPairedMove() && d < 30.0f) {
                    // A paired move on this puppet (ext PsyncBusySlot) has no mod hook; the
                    // local paired flag is the only observable proximity signal here.
                    ++out.execNear;
                    continue;
                }
                if (dead)
                    continue;

                if (d <= 30.0f)
                    ++out.aliveNear;
                if (d < out.nearestAlive)
                    out.nearestAlive = d;

                const bool marked = p.aimMarkActive;
                const bool close = d <= 5.0f;    // ext: within 5 m, any direction
                if (close) {
                    ++out.near8;
                    out.awareNow = true;         // ext: an enemy on us is an alerted enemy
                    if (!g_closePrev[slot])
                        NoteEvent(3.0f, "enemy at close quarters");
                }
                g_closePrev[slot] = close;

                const bool seen = marked || close;
                if (seen) {
                    if (d <= 30.0f) {
                        ++out.seenNear;
                        if (d < 15.0f)
                            ++out.seenClose;
                    } else {
                        ++out.seenFar;
                    }
                }
            }
        }

    }

    void NoteEvent(float add, const char* cause)
    {
        if (!::net::player_status::MatchLive())
            return;                              // ext: MatchSubstate() != 6
        const uint64_t now = GetTickCount64();
        g_heat = std::min(kHeatCap, g_heat + add);
        g_evtMs = now;
        if (std::strcmp(cause, "we took a hit") == 0)
            g_hitRxMs = now;                     // ext recv_hit_hook: g_musHitRxMs
        if (std::strcmp(cause, "health dropped") != 0
            && std::strstr(cause, "teammate") == nullptr)
            g_contactMs = now;                   // ext g_mus_contact_ms
        if (IsAwareCause(cause)) {
            g_awareMs = now;                     // ext g_musAwareMs / nr_hot_at
            std::snprintf(g_awareWhy, sizeof g_awareWhy, "%s", cause);
        }
        std::snprintf(g_cause, sizeof g_cause, "%s", cause);
        Log(add, cause);
    }

    void Tick()
    {
        const uint64_t now = GetTickCount64();
        const bool live = ::net::player_status::MatchLive();

        // ext g_msDead: the own alive-peer byte + HP <= 0.5. The mod has no peer byte here;
        // our own HP at / below zero in a live match is the equivalent (LocalAlive is > 0).
        g_dead = live && !::net::player_status::LocalAlive();
        if (g_dead)
            g_holdUntil = now + kHoldMs;

        if (!live) {
            if (g_tier || g_heat > 0.0f)
                TO1_LOG("[MUSIC-TIER] match left playing state - tier %d -> 0, heat %.1f\n",
                    g_tier, g_heat);
            g_tier = 0;
            g_heat = 0.0f;
            g_awareMs = g_evtMs = g_contactMs = g_calmSince = g_hitRxMs = 0;
            g_lastMs = 0;
            g_inMatch = false;
            g_wasFiring = g_wasMelee = g_wasPaired = false;
            g_lastHp = -1.0f;
            g_floorHeat = 0.0f;
            g_floorHeldAt = g_floorAwareMs = 0;
            g_floorCause[0] = '\0';
            g_lastStand = g_lms = false;
            g_remainSec = -1.0f;
            std::memset(g_closePrev, 0, sizeof g_closePrev);
            return;
        }
        if (!g_inMatch) {
            g_inMatch = true;
            g_awareMs = 0;   // ext: no "FIGHT START" at spawn off a stale awareness sign
            g_lastMs = now;
            TO1_LOG("[MUSIC-TIER] live (event heat, tiers 2 / 6 / 12; No Return rules %s)\n",
                NrRules() ? "on" : "off");
        }

        const float dt = (g_lastMs && now - g_lastMs < 500)
            ? static_cast<float>(now - g_lastMs) / 1000.0f : 0.016f;
        g_lastMs = now;
        g_heat *= std::pow(0.5f, dt / 3.0f);   // ext: heat halves every 3 s

        // ---- portable local producers (ext FeatureCombatMusic) --------------------------
        const bool firing = DC::Call<"player-is-firing?">() != 0;
        if (firing && !g_wasFiring && !::net::player_status::LocalDowned())
            NoteEvent(1.5f, "we fired");
        g_wasFiring = firing;

        const bool melee = DC::Call<"character-in-melee-attack?">(SID("player")) != 0;
        if (melee && !g_wasMelee && !::net::player_status::LocalDowned())
            NoteEvent(3.0f, "we melee");
        g_wasMelee = melee;

        const bool paired = ::net::player_status::LocalInPairedMove();
        if (paired && !g_wasPaired) {
            NoteEvent(3.0f, "paired move (shiv / execution / finisher)");
            g_awareMs = now;   // ext exec_near: an execution in progress is a hot sign
            std::snprintf(g_awareWhy, sizeof g_awareWhy, "%s", "execution in progress");
        }
        g_wasPaired = paired;

        // ext: own HP polled every 6th frame; a drop is a music input only when contact was
        // recent (the relayed hit itself is already an input, so don't double count it).
        if (::engine::frame::Count() % 6 == 0) {
            const float hp = ::engine::local_player::Health();
            if (hp > 0.0f && g_lastHp > 0.0f && hp < g_lastHp - 0.5f
                && now - g_contactMs <= 10000 && now - g_hitRxMs > 400)
                NoteEvent((g_lastHp - hp) / 25.0f, "health dropped");
            if (hp > 0.0f)
                g_lastHp = hp;
        }
        // ---- enemy-aware / lives / clock floors (ext FeatureCombatMusic) ---------------
        // The ext's floor_heat is a per-frame world contribution, held for 6 s, then folded
        // into the effective heat. All of its inputs that the mod can observe are read here.
        WorldScan w;
        ScanWorld(w);

        const ::net::match::Match& match = ::net::match::Get();
        g_remainSec = match.MatchRunning() ? match.matchTimer.Remaining() : -1.0f;
        const bool final30 = g_remainSec > 0.0f && g_remainSec <= 30.0f;
        const bool final10 = final30 && g_remainSec <= 10.0f;
        const bool fightAbout = w.aliveNear > 0 || g_heat >= 1.0f;
        const float hpNow = ::engine::local_player::Health();
        const bool lowHealth = hpNow > 0.0f && hpNow <= 45.0f
            && (now - g_contactMs <= 10000 || w.near8 > 0)
            && (g_heat >= 1.0f || w.seenNear > 0);

        float floor = 0.0f;
        const char* floorCause = "";
        if (w.seenNear > 0) {
            floor = 3.0f + 2.0f * static_cast<float>(w.seenNear - 1);
            if (w.seenClose > 0 && floor < 6.0f)
                floor = 6.0f;
            floorCause = w.seenClose > 0 ? "enemy in sight within 15 m"
                                         : "enemy in sight within 30 m";
        } else if (w.seenFar > 0) {
            floor = 2.0f;
            floorCause = "enemy in sight, far (standoff)";
        }
        if (w.seenNear == 0 && w.seenFar == 0 && w.nearestAlive < 12.0f
            && (g_tier >= 1 || g_heat >= 1.0f)) {
            floor = 2.0f;
            floorCause = "live enemy within 12 m (fight on)";
        }
        if (floor < 2.0f && (w.downNear > 0 || w.execNear > 0)) {
            floor = 2.0f;
            floorCause = w.execNear > 0 ? "execution in progress"
                                        : "enemy down, finisher pending";
        }
        if (lowHealth) {
            floor += 6.0f;
            floorCause = "low health in a fight";
        }
        if (final30 && fightAbout) {
            floor += 2.0f;
            floorCause = "match final 30 s";
        }
        if (final10 && fightAbout && floor < 6.0f) {
            floor = 6.0f;
            floorCause = "match final 10 s";
        }
        // ext floor_held / floor_held_at: a lower floor is held for 6 s (a sight that blinks
        // between frames does not drop the music).
        if (floor >= g_floorHeat) {
            g_floorHeat = floor;
            g_floorHeldAt = now;
        } else if (now - g_floorHeldAt < 6000) {
            floor = g_floorHeat;
            floorCause = "recent sight / contact (held 6 s)";
        } else {
            g_floorHeat = floor;
        }
        std::snprintf(g_floorCause, sizeof g_floorCause, "%s", floorCause);
        const float eff = g_heat > floor ? g_heat : floor;
        const char* cause = g_heat >= floor ? g_cause : floorCause;

        // NR hot clock: the ext's aware_now (an enemy sees us / is on us) and exec_near both
        // stamp nr_hot_at, as does any awareness MusicEvent (g_musAwareMs).
        if (w.awareNow || w.execNear > 0) {
            g_floorAwareMs = now;
            std::snprintf(g_awareWhy, sizeof g_awareWhy, "%s",
                w.awareNow ? "enemy sees us / on us" : "execution in progress");
        }
        const uint64_t nrHotAt = g_floorAwareMs > g_awareMs ? g_floorAwareMs : g_awareMs;

        // ext last_stand / lms from the match's own team lives (kFnTeamLives on the lobby
        // object). The mod's relay keeps the authoritative lives in Match::teamLives.
        const int lives = match.BlueLives();
        g_lastStand = match.InLiveMatch() && lives <= 0 && lives > -1000;
        g_lms = g_lastStand && w.matesAlive == 0 && !::net::player_status::LocalDowned();

        // ---- tier derivation (ext FeatureCombatMusic) ----------------------------------
        int byHeat;
        if (NrRules()) {
            // ext: one combat level, driven only by nr_hot_at (the floor does not raise a
            // tier in NR -- a seen-but-unaware enemy is not a fight).
            byHeat = (nrHotAt && now - nrHotAt <= kHotEndMs) ? 3 : 0;
        } else {
            // ext: the earlier 3-step ladder on the effective heat (event heat / held floor).
            byHeat = eff >= 12.0f ? 3 : eff >= 6.0f ? 2 : eff >= 2.0f ? 1 : 0;
        }

        int want = g_tier;
        if (g_lastStand) {
            want = g_lms ? 5 : 4;                 // ext: boss loop / hunted countdown
            cause = g_lms ? "last man standing" : "team out of lives";
        } else if (g_tier >= 4) {
            want = byHeat;                        // ext: lives came back, leave the boss loop
            cause = "lives came back";
        } else if (byHeat > g_tier && !::net::player_status::LocalDowned() && !g_dead) {
            want = byHeat;                        // ext: up at once, the frame it crosses
        } else if (g_tier >= 1 && g_tier <= 3 && now >= g_holdUntil && !final30) {
            if (NrRules()) {
                // ext: the fight is over 0.9 s after the last alerted enemy, with a minimum
                // 6.5 s combat dwell (t2r-music-dwell-old.txt removes the dwell).
                const bool dwellOk = engine::flag::Present("t2r-music-dwell-old.txt")
                    || now - g_tierAt >= kDwellMs;
                if (nrHotAt && now - nrHotAt > kHotEndMs && dwellOk) {
                    if (!g_calmSince)
                        g_calmSince = now;
                    // ext then plays the decay + search bed before returning to silence;
                    // with no playlists ported the tier just returns to 0 here.
                    want = 0;
                } else
                    g_calmSince = 0;
            } else {
                // ext (old rules): calm 4 s (contact lost), ease one tier after 5 s quiet.
                const bool quiet = !g_evtMs || now - g_evtMs > 3000;
                if (eff < 1.0f) {
                    if (!g_calmSince)
                        g_calmSince = now;
                    if (now - g_calmSince > kCalmDownMs && now - g_tierAt > 5000)
                        want = 0;
                } else {
                    g_calmSince = 0;
                    if (quiet && now - g_tierAt > 5000) {
                        const float th = g_tier == 1 ? 2.0f : g_tier == 2 ? 6.0f : 12.0f;
                        if (eff < th * 0.75f)
                            want = g_tier - 1;
                    }
                }
            }
        }
        if (eff >= 1.0f && !NrRules())
            g_calmSince = 0;

        if (want != g_tier) {
            const int old = g_tier;
            // ext: the grid-quantised cut (pend_at / MusicBoundary) is a MUSIC-PLAYBACK
            // concern -- with no playlists ported there is no loop to wait on, so the tier
            // moves at once. Documented deviation.
            g_tier = want;
            g_tierAt = now;
            g_calmSince = 0;
            TO1_LOG("[MUSIC-TIER] tier %d -> %d (%s; floor %.1f %s, heat %.1f, last aware %llu ms ago, %s, lives %d, rem %.0f s)\n",
                old, want, cause, floor, g_floorCause,
                g_heat, nrHotAt ? static_cast<unsigned long long>(now - nrHotAt) : 0ULL,
                g_awareWhy, lives, g_remainSec);
        }
    }

    int Tier()   { return g_tier; }
    bool Dead()  { return g_dead; }

    void Reset()
    {
        g_heat = 0.0f;
        g_evtMs = g_awareMs = g_contactMs = g_hitRxMs = g_holdUntil = 0;
        g_tierAt = g_lastMs = g_calmSince = g_logMs = 0;
        g_tier = 0;
        g_dead = false;
        g_inMatch = false;
        g_wasFiring = g_wasMelee = g_wasPaired = false;
        g_lastHp = -1.0f;
        g_floorHeat = 0.0f;
        g_floorHeldAt = g_floorAwareMs = 0;
        g_floorCause[0] = '\0';
        g_lastStand = g_lms = false;
        g_remainSec = -1.0f;
        std::memset(g_closePrev, 0, sizeof g_closePrev);
        std::strcpy(g_cause, "?");
        std::strcpy(g_awareWhy, "?");
    }

}
