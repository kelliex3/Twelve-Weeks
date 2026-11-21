#pragma once

#include <cstdint>

// Ported from tools/ext/t2r-ext.c's COMBAT MUSIC TIER STATE: MusicEvent (the event heat)
// and the tier derivation inside FeatureCombatMusic (by_heat / the up rule / the NR fight-end
// rule). This is the part other features read -- ext g_msTier (0 calm, 1..3 combat) and
// g_msDead -- so they can gate themselves against the fight. It is deliberately NOT the
// music itself: the risers / loops / beds, the theme / set / v5 rotation, the grid-quantised
// cut timing and the stinger layer all need the private generated PCM blobs and the
// music_families.h / music_pools.h tables, which are not shipped. Those stay in the ext.
//
// Ported inputs (ext FeatureCombatMusic, the ones this client can observe):
//   * MusicEvent + the heat decay + the awareness / contact clocks (note_event);
//   * 'we fired' (player-is-firing? rising edge), 'we melee' (character-in-melee-attack?
//     rising edge), 'paired move' (LocalInPairedMove rising edge), 'health dropped'
//     (local HP polled every 6th frame), and 'we took a hit' (the damage event calls
//     NoteEvent directly, in the ext's order relative to alertsting::Combat);
//   * the local death cut (g_msDead from our own HP, ext alive peer + HP <= 0.5).
//
// Ported floors (ext FeatureCombatMusic): the enemy-aware floor_heat and its 6 s hold (enemy
// marked / in sight within 30 m / 15 m, far standoff, live enemy within 12 m, close quarters,
// down / execution pending, low health in a fight), the match final-30 / final-10 floors, and
// the lives tiers 4 (team out of lives) / 5 (last man standing) with the "lives came back"
// exit. The world inputs come from the mod's own peer positions, team, mark and death flags,
// team lives and match clock -- no new hook is needed for those.
//
// Enemy-aware inputs (ported):
//   * the camera-frustum + line-of-sight "seen" test (ext MusicSlotSeen / TagScreen) and the
//     5-8 m forward cone (MusicSlotCos) are real sampler seams (SetSightSampler /
//     SetConeSampler); net/audio/music-sight.* supplies a native implementation from the
//     camera-through-reticle locator + async static-world ray (gated, see that module).
//   * "we hit an enemy" (ext hit-tick hook) is fed from the outgoing puppet-hit hook;
//     "spotted by an enemy (spot call)" from the received Hawk Eyed / spot-call vox line;
//     "enemy shot heard" / "enemy fire continues" from the peer shot-count radar;
//     "enemy fired near" from the peer firing-byte rising edge; close quarters, down / revive
//     and stun from the world scan.
//   * "we are stunned" (6.0) is fed from gameplay/stun-lock.*; "enemy stunned" (5.0) is fed
//     from SetPeerStunned(slot, true) by two producers -- the NEVT_STUNNED (ext MSG_STUNNED
//     0x45) receiver and a per-frame `npc-is-stunned?` poll of the peer puppet in ScanWorld
//     (ext's ShivStunned call). Ext uses only the replicated message; the native poll is a
//     documented divergence that makes the input live without a local stun broadcaster.
// The default No Return rules are used; t2r-music-nr-rules-old.txt selects the ext's earlier
// 3-step heat ladder (that path is portable and is included).
//
// Opt-outs next to the exe (ext): t2r-no-music.txt (all music), t2r-music-nr-rules-old.txt.
// Logs "[MUSIC-TIER] ...".

namespace net::audio::music_tier {

    // ---- ext enemy-aware event values (pure, no engine state) -------------------------------

    // ext t2r-ext.c's shot radar: the first shot of a volley counts full heat (2.0 near / 1.2
    // beyond 45 m), each further shot 1.0 near / 0.6 far (SHOT_RADAR_MS = 2500 breaks a volley).
    constexpr float ShotHeat(bool first, float distM) noexcept
    {
        return first ? (distM < 45.0f ? 2.0f : 1.2f) : (distM < 45.0f ? 1.0f : 0.6f);
    }
    constexpr float NearFireHeat(bool mate) noexcept { return mate ? 0.75f : 1.5f; }
    constexpr float CloseHeat() noexcept { return 3.0f; }       // enemy at close quarters
    constexpr float DownHeat() noexcept { return 4.0f; }       // enemy / teammate down near
    constexpr float ReviveHeat() noexcept { return 4.0f; }     // enemy revived
    constexpr float StunHeat() noexcept { return 5.0f; }       // enemy stunned
    constexpr float WeStunnedHeat() noexcept { return 6.0f; }  // ext: "we are stunned"

    // ext "enemy stunned" producer seam. No native peer-stun state is replicated, so nothing
    // calls this yet; the world scan fires StunHeat() on a false->true rising edge per slot.
    void SetPeerStunned(int slot, bool stunned) noexcept;

    // ext MusicEvent's awareness list (the causes that stamp nr_hot_at). Our own fire / melee
    // and "health dropped" are deliberately absent. Inline so the pure helper is usable from
    // standalone harnesses without the game-coupled .cpp.
    inline bool IsAwareCause(const char* cause) noexcept
    {
        if (!cause)
            return false;
        static const char* const kAware[] = {
            "spotted by an enemy (spot call)", "we took a hit", "we hit an enemy",
            "enemy shot heard", "enemy fire continues", "enemy fired near",
            "enemy at close quarters", "we are stunned", "enemy stunned",
            "enemy revived",
        };
        for (const char* a : kAware) {
            const char* x = a;
            const char* y = cause;
            while (*x && *x == *y) { ++x; ++y; }
            if (!*x && !*y)
                return true;
        }
        return false;
    }

    // ---- injectable enemy sight / close-cone samplers --------------------------------------

    // Both mirror the ext's per-slot world queries. A sampler answers true for `slot`; it may
    // use its own ctx (positions, camera, ray jobs). The tier owns the 500 ms sight dwell and
    // the 300 ms break (ext vis_since / vis_last), and the 5 / 8 m close-quarters rule.
    // With no sampler installed the ext's camera-frustum/LOS and forward-cone inputs are
    // conservatively treated as "not seen" -- an honest gate, not a behaviour change.
    using SightSampler = bool (*)(int slot, void* ctx);
    using ConeSampler  = bool (*)(int slot, void* ctx);

    void SetSightSampler(SightSampler fn, void* ctx) noexcept;
    void SetConeSampler(ConeSampler fn, void* ctx) noexcept;

    // ext MusicEvent: add `add` heat for `cause`; refresh the event / contact / awareness
    // clocks. A no-op outside a live match (ext MatchSubstate() != 6). Call sites use the
    // ext's exact cause strings: only the ones in the IsAware list feed the NR fight clock.
    void NoteEvent(float add, const char* cause);

    // Per frame from boot::FrameTick (the tier part of ext FeatureCombatMusic). Decays the
    // heat, samples the portable local producers, and moves the tier up / down. Must run
    // BEFORE alertsting::Tick so the alert sting reads the same frame's tier.
    void Tick();

    // ext g_msTier: 0 calm, 1..3 combat, 4 team out of lives, 5 last man standing.
    int Tier();

    // ext g_msDead: our death cut is up (local HP at / below zero in a live match).
    bool Dead();

    // Session / level reset: clear the heat, the tier and every clock.
    void Reset();

}
