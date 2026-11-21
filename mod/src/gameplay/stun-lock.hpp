#pragma once

#include <cstdint>

// Ported from tools/ext/t2r-ext.c's STUN LOCK state (g_stun_until / g_stunEndedT60 and the
// shiv-victim check around them). Pure core: no animation clips, no character state, no wire.
// The host feeds ms timestamps; the network producer that starts a stun is not ported, so the
// core stays clear until a producer calls Stun().
//
// Ext behaviour kept:
//   * STUN_SECONDS 3.5 s lock, STUN_BLEND_OUT 0.35 s, a 1.2 s cough cadence;
//   * a re-stun restarts the timer but keeps the running clip (Fresh() tells the caller);
//   * the lock ends on its timer or when the match leaves the playing state, stamping the
//     post-end grace (g_stunEndedT60) that the shiv victim check reads for 1.2 s;
//   * the front shiv needs a stunned victim; the back shiv accepts the 2013 arc
//     (dd <= 3.5 m && (ang >= 105 || ang <= -125)) OR a stun;
//   * when the shiv half begins the lock is cut at once (the ext clears g_stun_until there).
//
// The log sink is optional; without one the core is silent.

namespace gameplay::stun_lock {

    inline constexpr float    kStunSeconds   = 3.5f;
    inline constexpr float    kBlendOut      = 0.35f;
    inline constexpr float    kCoughSeconds  = 1.2f;
    inline constexpr uint64_t kGraceMs       = 1200;
    inline constexpr float    kBackRangeM    = 3.5f;
    inline constexpr float    kBackAngleHiDeg = 105.0f;
    inline constexpr float    kBackAngleLoDeg = -125.0f;
    inline constexpr uint32_t kTotalMs       = static_cast<uint32_t>(kStunSeconds * 1000.0f);
    inline constexpr uint32_t kBlendOutMs    = static_cast<uint32_t>(kBlendOut * 1000.0f);
    inline constexpr uint32_t kCoughMs       = static_cast<uint32_t>(kCoughSeconds * 1000.0f);

    struct Sink {
        virtual ~Sink() = default;
        virtual void Log(const char* line) = 0;
    };
    void SetSink(Sink* sink) noexcept;
    Sink* SinkInstance() noexcept;

    // ext's stun start: now + 3.5 s, first cough one period below the start.
    void Stun(uint64_t nowMs, int fromSlot);

    // ext stun_local's phase end: the timer ran out or the match left the playing state.
    // Stamps the 1.2 s grace unless the lock was already off.
    void Tick(uint64_t nowMs, bool inPlayState);

    // Cut the lock the moment the shiv half begins (ext clears g_stun_until there).
    void Cut(uint64_t nowMs);

    bool Active(uint64_t nowMs) noexcept;                    // g_stun_until running now
    bool RecentlyEnded(uint64_t nowMs) noexcept;             // active, or within the 1.2 s grace
    bool VictimOk(bool front, float angleDeg, float distM, uint64_t nowMs) noexcept;

    uint64_t Until() noexcept;
    uint64_t EndedAt() noexcept;
    uint32_t TotalMs() noexcept;
    uint32_t BlendOutMs() noexcept;
    uint32_t CoughMs() noexcept;
    int      FromSlot() noexcept;
    bool     Fresh() noexcept;   // the last Stun() started a new lock rather than restarting one

    void Reset();

} // namespace gameplay::stun_lock
