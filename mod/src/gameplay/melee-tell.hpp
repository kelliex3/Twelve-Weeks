#pragma once

#include <cstdint>

// Ported from tools/ext/melee_tell.c (the timing core only). The ext's four numbers and the
// tuned behaviour are kept verbatim: contact is planned at the tell (<= 320 ms by default),
// the windup is sped up to meet it (x1.0 .. x1.25), observed contacts feed a per-weapon-class
// EMA, and the free-swing dodge window follows the tell (tell + 120 ms window, tell ms react).
//
// This is a PURE core: no exe-side config files, no cue voices / swish PCM, no clip rate call.
// The host supplies the on/off state (the ext reads t2r-no-melee-tell.txt / t2r-tell-ms.txt
// and Mv2On()) through SetEnabled / SetMeleeV2 / SetTellMs. Melee v2 suppresses the tell window
// exactly as in the ext. With the default settings this core mirrors the ext's shipped defaults.

namespace gameplay::melee_tell {

    inline constexpr int kTellMs      = 320;  // ext MT_TELL_MS
    inline constexpr int kWinExtraMs  = 120;  // ext MT_WIN_EXTRA_MS
    inline constexpr int kVoxAtMs     = 80;   // ext MT_VOX_AT_MS (cue timing, data only)
    inline constexpr int kSwishAtMs   = 40;   // ext MT_SWISH_AT_MS (cue timing, data only)
    inline constexpr int kRateMs      = 450;  // ext MT_RATE_MS (cue de-dup, data only)
    inline constexpr int kMinTellMs   = 200;  // ext t2r-tell-ms.txt range
    inline constexpr int kMaxTellMs   = 500;
    inline constexpr int kMinObserved = 100;  // ext MtellNoteContact's accepted observation range
    inline constexpr int kMaxObserved = 1500;
    inline constexpr int kMaxSpeedX100 = 125; // ext caps the windup speed-up at x1.25 (was x1.6)

    // Opt-out: the ext's t2r-no-melee-tell.txt and Mv2On() both force the window off.
    void SetEnabled(bool enabled) noexcept;
    void SetMeleeV2(bool enabled) noexcept;
    bool Enabled() noexcept;

    // Clamped to [kMinTellMs, kMaxTellMs] like the ext's config parser.
    void SetTellMs(int ms) noexcept;
    int  TellMs() noexcept;

    // ext MtellWinMs / MtellReactMs: 0 when the tell (or melee v2's window) is off.
    uint64_t WinMs() noexcept;
    uint64_t ReactMs() noexcept;

    // ext MtellTc / MtellSpeed / MtellNoteContact: the exchange's planned contact and the
    // windup rate feed off the same per-weapon-class contact estimate (EMA 0.5, clamped).
    uint16_t Tc(int wcls, uint16_t raw);
    float    Speed(int wcls, uint16_t raw);
    void     NoteContact(int wcls, int observedMs);

    void Reset();

} // namespace gameplay::melee_tell
