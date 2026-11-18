#pragma once

// Ported from tools/ext/shiv_sting.c: the shock sting when WE are the victim of a shiv move.
// A ~1 s TLOU2 SP one-shot (shiv_sting.pcm, a pool of candidates picked at random without
// repeats) plays 2D through the PCM mixer at the music volume on the move's first frame.
//
// NOT YET WIRED / GATED (each is an honest TODO, see the .cpp):
//  - Trigger: the ext fires from xcam_start role 2 with a shiv move name. The mod has no
//    xcam hook; Start() is the entry a future xcam (or execution) port calls.
//  - Music: the ext also cuts the combat music and plays a drum cue under the sting
//    (ShivdrumStart / Cut / Tick), which needs the ext's music layer (exec_music.c:
//    MusicPlay, g_ms_hold_until, g_ms_resume_at, g_ms_nowatch_until). Not ported; the sting
//    plays alone for now.
//  - Stun: the alert sting on a back-shiv is gated on "the victim was not stunned" (the ext's
//    g_stun_until). Stun is not ported, so the gate is absent and the alert plays on every
//    stealth-back shiv -- the ext's behaviour when no stun is active.
//  - Data: shiv_sting.h / shiv_drums.h are private generators (PHASE1.md item 7). Without
//    T2R_DATA_SHIV_STING the module compiles but Start() is a no-op.
//
// Opt-out: t2r-no-shiv-sting.txt next to the exe. Logs "[SHIVSTING] ...".

namespace net::audio::shivsting {

    // The shiv move started and we are its victim (ext ShivstingStart). `name` is the move's
    // name; the caller only reaches here for a name containing "shiv". Deduped, in-match only.
    void Start(const char* name);

    // Fade the running sting out (ms <= 0 = immediate) and forget it (ext ShivstingCut).
    void Cut(int fadeMs, const char* why);

    // ms the running sting still has to play (0 = none running) (ext ShivstingRemainingMs).
    int  RemainingMs();

    // Per frame from boot::FrameTick: forget a finished sting, cut it when the match ends
    // (ext ShivstingTick, minus the drum half).
    void Tick();

}
