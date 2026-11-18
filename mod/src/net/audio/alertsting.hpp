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
// NOT YET WIRED / GATED (see the .cpp):
//  - The combat path needs the music layer's combat tier (ext g_msTier == 0 = calm) and its
//    "no enemy-caused music input" clock; neither is ported. Combat() is the entry a future
//    music port calls (the ext calls it from the damage hook and the immersion near-miss).
//  - Downed / dead are not ported (the ext's g_downed / g_msDead gates).
//  - Data: alert_sting.h is a private generator. Without T2R_DATA_ALERT_STING, Play() is a
//    no-op.
//
// Opt-out: t2r-no-alert-sting.txt next to the exe. Logs "[ALERTSTING] ...".

namespace net::audio::alertsting {

    // Play the sting at `level` (ext AlertstingPlay). Deduped (ALS_GAP_MS), in-match only.
    void Play(float level, const char* why);

    // We were suddenly shot at while it was quiet (ext AlertstingCombat). GATED on the music
    // tier: inert until the music layer is ported.
    void Combat(const char* why);

    // Per frame from boot::FrameTick (ext AlertstingTick, minus the stun / crossbow / cough
    // sub-ticks).
    void Tick();

}
