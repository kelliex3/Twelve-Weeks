#pragma once

// Ported from tools/ext/shiv_sting.c: the shock sting + struggle drums when WE are the victim
// of a shiv move. A ~1 s TLOU2 SP one-shot (shiv_sting.pcm, a pool picked at random without
// repeats) plays 2D through the PCM mixer at the music volume on the move's first frame.
//
// Wired to the ported cores:
//  - Trigger: ext xcam_start role 2 with a shiv move name. The routing core (net/audio/xcam.*)
//    dispatches to ShivMusicStart() then Start() through a gated bridge; there is no native
//    final render-camera / xcam hook, so no hook calls it yet.
//  - Music: ShivMusicStart() cuts the combat music through net/audio/music-hold.* and holds it
//    cut for SHD_HOLD_MS (the ext's g_msHoldUntil / g_msResumeAt / g_msNowatchUntil). The drum
//    PCM half is behind T2R_DATA_SHIV_DRUMS.
//  - Stun: a back-shiv's alert sting is gated on the victim NOT being stunned
//    (gameplay/stun-lock.*, the ext's g_stun_until); the lock is cut when the shiv half begins.
//  - Data: shiv_sting.h / shiv_drums.h are private generators (PHASE1.md item 7). Without
//    T2R_DATA_SHIV_STING / T2R_DATA_SHIV_DRUMS the module compiles but the PCM halves are no-ops.
//
// Opt-outs: t2r-no-shiv-sting.txt, t2r-no-shiv-music.txt. Logs "[SHIVSTING] ...".

namespace net::audio::shivsting {

    // Shiv victim's combat-music cut + struggle drums (ext ShivdrumStart). Called BEFORE
    // Start() by the xcam dispatch. Deduped, in-match only.
    void ShivMusicStart(const char* name);

    // The shiv move started and we are its victim (ext ShivstingStart). Deduped, in-match only.
    void Start(const char* name);

    // Fade the running shock sting / drums out (ms <= 0 = immediate) and forget them.
    void Cut(int fadeMs, const char* why);
    void ShivMusicCut(int fadeMs, const char* why);

    // ms the running sting still has to play (0 = none running) (ext ShivstingRemainingMs).
    int  RemainingMs();

    // Per frame from boot::FrameTick: forget finished voices, cut them when the match ends.
    void Tick();

    void Reset();

} // namespace net::audio::shivsting
