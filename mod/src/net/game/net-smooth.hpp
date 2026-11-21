#pragma once

#include <cstddef>
#include <cstdint>

// Ported from tools/ext/netsmooth.c ("remote players' motion", re/netcode.md). The ext
// wraps winmm's remote-player placement with a per-slot jitter buffer: each msg-5 state
// (pos + quat) is stamped with the sender time of the msg-7 list carrying the same sequence,
// a playout clock runs at real time one adaptive delay behind the least-delayed sender, and
// the puppet's locator is lerped/slerped between samples (with capped extrapolation and a
// decaying error correction instead of a snap). Sender clock rebase + 15.625 ms dequantisation
// keep a long-uptime sender's float timeline exact. "net stats:" every 10 s is the acceptance log.
//
// This is the PURE CORE: the sample ring, msg-5/msg-7 pairing, jitter/loss/delay estimation,
// playout clock, interpolation / extrapolation / correction, stutter metrics and statistics.
// It is engine-free and single-threaded (all feeds run on the game thread), using a double
// millisecond timeline supplied by the caller (native bridge: engine::frame::NowMs()). The
// ext's fixed-VA engine patches, winmm apply/alpha/compose hooks, send cap, relay dedup,
// host-bw gating, anim-clock target and root-vec4 interpolation have no native equivalent and
// are NOT ported; the native anim layer already owns its own 25 ms play clock.
//
// Wire format is unchanged: this reads the pos/quat already decoded from PlayerNetState and
// the sendMs already decoded from AnimCmdPayload. No ext wire ID or packet layout is touched.

namespace net::netsmooth {

    // ext T2R_MAX_PEERS = 8 (slots 0..7, slot 0 = host). kSlots is deliberately independent
    // of the protocol's kMaxNetPlayers so this core stays standalone/testable; the bridge
    // bounds-checks the protocol slot before calling in.
    inline constexpr int kSlots       = 8;
    inline constexpr int kRingSize    = 64;    // NS_RING: msg-5 samples per peer
    inline constexpr int kM7Size      = 32;    // NS_M7: recent msg-7 (seq,t) pairs
    inline constexpr int kDqN         = 24;    // NS_DQ_N: least-squares dequant window
    inline constexpr int kPairWindow  = 24;    // how far back a msg 7 pairs with a msg 5

    inline constexpr double kDelayMinMs  = 16.0;   // NS_DMIN
    inline constexpr double kDelayFloor  = 24.0;   // NS_DFLOOR
    inline constexpr double kDelayMaxMs  = 200.0;  // NS_DMAX
    inline constexpr double kExtrapMaxMs = 100.0;  // NS_EXTRAP_MS
    inline constexpr float  kMaxSpeed    = 0.010f; // NS_MAX_SPEED: 10 m/s, per ms
    inline constexpr float  kTeleportM   = 4.0f;   // NS_TELEPORT_M
    inline constexpr float  kSnapM       = 1.5f;   // NS_SNAP_M
    inline constexpr double kTauMs       = 100.0;  // NS_TAU_MS: correction decay
    inline constexpr double kJitterK     = 1.3;    // NS_JK
    inline constexpr double kJpkTauMs    = 2000.0; // NS_JPK_TAU_MS
    inline constexpr double kDcurDown    = 0.008;  // NS_DCUR_DOWN: 8 ms/s
    inline constexpr double kDcurUpHyst  = 6.0;    // NS_DCUR_UP_HYST
    inline constexpr double kDcurDnHyst  = 12.0;   // NS_DCUR_DN_HYST
    inline constexpr double kDqQ         = 15.625; // NS_DQ_Q: Windows timer tick
    inline constexpr float  kStutterMps  = 3.0f;   // NS_STUTTER_MPS
    inline constexpr double kLeadK       = 0.5;    // ext g_nsLeadK
    inline constexpr double kLeadMaxMs   = 60.0;   // ext g_ns_lead_max
    inline constexpr double kLogEveryMs  = 10000.0;

    // The locator the caller writes (ext ns_vec_interp's final pos/quat).
    struct Transform {
        float pos[3]{ 0.0f, 0.0f, 0.0f };
        float quat[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    };

    // ext NsSlot's 10 s counters (the fields the "net stats:" line actually reads).
    struct Stats {
        uint32_t states = 0, lost5 = 0, anims = 0, lost7 = 0;
        uint32_t frames = 0, extrap = 0, hold = 0, snap = 0, corr = 0;
        uint32_t resync = 0, tele = 0, rebase = 0, dequant = 0;
        uint32_t stutterNative = 0, stutterSmooth = 0, moving = 0;
        double   delaySum = 0, lagSum = 0, corrMax = 0, jitterMax = 0;
        uint32_t lagN = 0;
        // Latest estimates (ext s->sdt / s->si / s->jmean / s->loss) for the log line.
        double   frameMs = 0, sampleMs = 0, jitterMean = 0, lossEwma = 0;
        // Dead-reckoning prediction error: next real sample vs prediction from earlier ones.
        double   predCvSum = 0, predHoldSum = 0, predMaxCv = 0, predMaxHold = 0;
        uint32_t predN = 0;
        double   predGapCvSum = 0, predGapHoldSum = 0;
        uint32_t predGapN = 0;
    };

    // ext NsReset: clear a slot's stream state but keep its sender-time base (a sender that
    // rewinds a little is not a new clock domain). ResetAll() also drops the base/dequantiser.
    void Reset(uint8_t slot);
    void ResetAll();

    // ext NsOnState: one msg-5 sample. `nowMs` is the local receive time (monotonic ms).
    void OnState(uint8_t slot, uint32_t seq, const float pos[3], const float quat[4],
                 double nowMs);

    // ext PaRecvHook's msg-7 pipeline: NsRebase -> NsDequant -> NsOnAnim. `sendMs` is the raw
    // sender clock carried by AnimCmdPayload; `nowMs` is the local arrival time.
    void OnAnim(uint8_t slot, uint32_t seq, uint32_t sendMs, double nowMs);

    // ext NsLocator's pure playout core. Produces the smoothed transform at `nowMs`.
    // `onewayMs` is half the measured RTT (ext PsyncOneway) and feeds the prediction lead;
    // pass 0 for no lead. `handover`, when supplied, is the current native placement: the
    // ext blends from it to the buffered placement over NS_BLEND_MS (100 ms) on a puppet's
    // first frames / after a gap, and uses it for the native-vs-smoothed stutter metric.
    // Returns false while the buffer is unprimed (!synced or < 2 samples) or the samples are
    // unusable — the caller then keeps its existing placement.
    bool Sample(uint8_t slot, double nowMs, double onewayMs, Transform& out,
                const Transform* handover = nullptr);

    // Diagnostics. StatsFor() is valid for every slot; ResetStats() is the 10 s roll-over.
    const Stats& StatsFor(uint8_t slot);
    void ResetStats(uint8_t slot);
    bool Primed(uint8_t slot) noexcept;
    bool Enabled(uint8_t slot) noexcept;   // synced && primed (ext !g_nsOff && !R_ok guard)

} // namespace net::netsmooth
