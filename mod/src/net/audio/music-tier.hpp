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
// Honest gates (TODO): the enemy-aware floor (enemy in sight / close / marked, spot calls,
// enemy shots, close quarters, stuns, revives), the "we hit an enemy" input (the ext's hit-tick
// hook is not ported), the lives / last-stand tiers (4 / 5) and the final-30 s floor. Until
// those producers land, the tier is driven by our own actions and hits taken alone. The
// default No Return rules are used; t2r-music-nr-rules-old.txt selects the ext's earlier
// 3-step heat ladder (that path is portable and is included).
//
// Opt-outs next to the exe (ext): t2r-no-music.txt (all music), t2r-music-nr-rules-old.txt.
// Logs "[MUSIC-TIER] ...".

namespace net::audio::music_tier {

    // ext MusicEvent: add `add` heat for `cause`; refresh the event / contact / awareness
    // clocks. A no-op outside a live match (ext MatchSubstate() != 6). Call sites use the
    // ext's exact cause strings: only the ones in the IsAware list feed the NR fight clock.
    void NoteEvent(float add, const char* cause);

    // Per frame from boot::FrameTick (the tier part of ext FeatureCombatMusic). Decays the
    // heat, samples the portable local producers, and moves the tier up / down. Must run
    // BEFORE alertsting::Tick so the alert sting reads the same frame's tier.
    void Tick();

    // ext g_msTier: 0 calm, 1..3 combat. 4 / 5 (last stand / last man standing) are gated
    // on the unported lives input and are never produced here.
    int Tier();

    // ext g_msDead: our death cut is up (local HP at / below zero in a live match).
    bool Dead();

    // Session / level reset: clear the heat, the tier and every clock.
    void Reset();

}
