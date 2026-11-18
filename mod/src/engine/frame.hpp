#pragma once

#include <cstdint>

// Ported from tools/ext/t2r-ext.c (g_frame / g_t60 / g_frameMs, advanced at the top of
// t2r_tick, which runs once per game frame). Two clocks come out of one tick, and the
// ext's features mix them on purpose:
//
//   Count()   the frame counter (g_frame). Monotonic, once per frame, cheap. Used for
//             "every N frames" pacing and short intra-frame deadlines (a hitch stretches
//             wall time but not the frame count). Note: this is the ext's own counter,
//             NOT the engine's get-current-frame-count -- the two are unrelated.
//   T60()     real time in 1/60 s units (g_t60 = GetTickCount64() * 60 / 1000). Stored
//             on deadlines that must last the same wall time at 30, 60 or 144 fps and
//             across hitches: keep-alives, TTLs, back-offs.
//   DeltaMs() smoothed real frame time in ms (g_frameMs), used to convert an authored
//             duration in seconds into a frame count.
//
// boot::FrameTick calls Tick() first, so every feature below sees the same values for
// the whole frame.

namespace engine::frame {

    void     Tick();       // advance Count(), refresh T60(), smooth DeltaMs() (once per frame)
    uint64_t Count();      // frame counter              (ext: g_frame)
    uint64_t T60();        // wall clock in 1/60 s units (ext: g_t60)
    float    DeltaMs();    // smoothed frame time, ms    (ext: g_frameMs)

    // Test / level-reload helper.
    void     Reset();

}
