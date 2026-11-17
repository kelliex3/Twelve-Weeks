// Send-rate policies for the ext's periodic peer messages. Pure C: tests/bwpolicy_test.c compiles this file.
#ifndef BWPOLICY_H
#define BWPOLICY_H
#include <stdint.h>
// MSG_BOMB_TRACK (32 B payload, broadcast, the host relays it to everybody): the stream used to go out every 3 frames - 48 msg/s at 144 fps, and
// for the whole fuse while the bomb lies still (the receiver's copy ignores a resting track that agrees within 0.15 m). Now: at most one per 45 ms of
// real time in flight (= the 3 frames at 60 fps the receiver's 0.025 s latency term is tuned for), one per 200 ms once the bomb has lain still for
// BW_TRACK_STILL_N frames (the receivers' resting-copy steering needs a sample now and then, not 20 per second).
#define BW_TRACK_MIN_FRAMES 3
#define BW_TRACK_FLY_MS 45.0
#define BW_TRACK_REST_MS 200.0
#define BW_TRACK_STILL_N 6
static inline int BwTrackDue(uint64_t frames_since, double ms_since,
                               int still_n)
{
    if (frames_since < BW_TRACK_MIN_FRAMES)
        return 0;
    return ms_since >= (still_n >=
                        BW_TRACK_STILL_N ? BW_TRACK_REST_MS :
                        BW_TRACK_FLY_MS);
}

// A keep-alive on a wall-clock period: frame counters (g_frame % 120) run 2.4x faster at 144 fps than at 60. due = 1 when `period_ms` has passed since *last
// (and sets it); the first call is due.
static inline int BwEveryMs(uint64_t *last, uint64_t now_ms,
                              uint64_t period_ms)
{
    if (*last && now_ms - *last < period_ms)
        return 0;
    *last = now_ms ? now_ms : 1;
    return 1;
}
#endif
