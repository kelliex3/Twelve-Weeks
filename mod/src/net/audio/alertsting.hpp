#pragma once

// Ported from tools/ext/alert_sting.c, the alert sting only: TLOU2's
// t2-hud-stealth-notification (alert_sting.pcm), 2D through the PCM mixer at the music
// volume.
//   (a) combat: when it is quiet and we are suddenly shot at (in our direction or hit), the
//       sting plays;
//   (b) shiv: a prominent sting for the victim of a back-shiv that was not stunned, called by
//       net::audio::shivsting::Start on top of the shock sting.
//
// The file's other three sub-features -- the stun tone, the crossbow HUD sounds and the stun
// cough -- are separate features and are NOT ported here (each needs stun / downed / crossbow
// state the mod does not have yet).
//
// Wired to the music tier state (net/audio/music-tier.*):
//  - The calm clock (ext g_als_calm_ms) is refreshed by Tick() on every frame the combat
//    tier is 0, and Combat() requires it fresh -- the "shot at out of the quiet" gate.
//    The tier's enemy-aware inputs are honest TODOs, so until they land the tier is driven
//    by our own actions and hits only. Combat() is called from the damage event (the ext's
//    recv_hit_hook); the immersion near-miss producer is not ported.
//  - Dead / downed use music_tier::Dead() (ext g_msDead) and LocalDowned() (ext g_downed,
//    itself an honest stub until the downed feature lands).
//  - Data: alert_sting.h is a private generator. Without T2R_DATA_ALERT_STING, Play() is a
//    no-op.
//
// Opt-out: t2r-no-alert-sting.txt next to the exe. Logs "[ALERTSTING] ...".

namespace net::audio::alertsting {

    // Play the sting at `level` (ext AlertstingPlay). Deduped (ALS_GAP_MS), in-match only.
    void Play(float level, const char* why);

    // We were suddenly shot at while it was quiet (ext AlertstingCombat). Requires a fresh
    // calm clock (combat music tier 0), alive and not downed; called from the damage event.
    void Combat(const char* why);

    // Per frame from boot::FrameTick (ext AlertstingTick, minus the stun / crossbow / cough
    // sub-ticks).
    void Tick();

}
