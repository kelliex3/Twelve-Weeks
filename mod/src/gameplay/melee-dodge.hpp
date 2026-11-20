#pragma once

#include <cstddef>
#include <cstdint>

// Ported from tools/ext/melee_dodge.c's TIMING / STATE core: the dodge window (MD_WIN_MS,
// the 480 -> 260 -> 180 -> 130 ms streak, MD_REACT_MS, MD_WIN_HARD_MS), i-frames (260 ms),
// the two dodge charges and their cooldown, and the three peer messages (0x97 swing, 0x98
// result, 0x99 press). The numbers are the ext's [TUNING ps*] constants.
//
// This is the pure core only. Everything that needs the game -- the virtual pad, contact
// hooks, contextual dodge clips / root motion, the animation state machine, the HUD bar and
// the camera -- stays gated; the caller (a future dodge port or the ext bridge) feeds the
// observable events in and receives wire sends through DodgeSink. With no sink installed the
// module only tracks state.
//
// Honest gates (TODO): melee_tell's shortening (MtellWinMs / MtellReactMs), melee_exchange's
// window override, melee_duel's fair window, the geometry test (MdgGeomOk) and the late-press
// hold all need their own ports. Window() here is the base ext rule; those producers narrow it
// once ported. The wire opcodes are reserved in net-protocol.hpp.

namespace gameplay::melee_dodge {

    inline constexpr uint16_t kWindowMs      = 480;   // ext MD_WIN_MS
    inline constexpr uint16_t kWindow2Ms     = 260;   // ext MD_WIN2_MS
    inline constexpr uint16_t kReactMs       = 380;   // ext MD_REACT_MS
    inline constexpr uint16_t kWindowHardMs  = 1000;  // ext MD_WIN_HARD_MS
    inline constexpr uint16_t kStreakMs      = 6000;  // ext MD_STREAK_MS
    inline constexpr uint16_t kIframeMs      = 260;   // ext MD_IFRAME_MS
    inline constexpr uint16_t kChainMs       = 380;   // ext MD_DODGE_CHAIN_MS
    inline constexpr uint16_t kCoverMs       = 60;    // ext MDG49_COVER_MS (i-frames past contact)
    inline constexpr uint16_t kBufferMs      = 250;   // ext MDG49_BUF_MS (early heavy press)
    inline constexpr uint16_t kClipMaxMs     = 680;   // ext MD_CLIP_MAX_MS
    inline constexpr float    kClipFrac      = 0.85f; // ext MD_CLIP_FRAC
    inline constexpr float    kHeavyRate     = 0.8f;  // ext MDG_HEAVY_RATE
    inline constexpr float    kHeavyRampS    = 0.15f; // ext MDG_HEAVY_RAMP_S
    inline constexpr int      kCharges       = 2;     // ext MDG_CHARGES
    inline constexpr int      kMaxPeers      = 8;

    // Movement directions (body frame): 0 front, 1 left, 2 right, 3 back.
    enum Direction : uint8_t { kFront = 0, kLeft = 1, kRight = 2, kBack = 3 };

    struct Sink {
        virtual ~Sink() = default;
        virtual void SendSwing(uint8_t dest) = 0;    // MSG_DODGE_SWING 0x97, [u8 1] armed
        virtual void SendPress(uint8_t dest, uint8_t immune, uint8_t dir, uint64_t clipSid) = 0;
        virtual void SendResult(uint8_t dest) = 0;   // MSG_DODGE_RES 0x98, empty
        virtual void Log(const char* text) = 0;
    };

    void SetSink(Sink* sink);

    // ---- swing tracking (the attacker's side feeds these) -------------------------------
    // A remote swing began at `nowMs` with its sequence number (ext g_mdg.sw[s]).
    void NoteSwing(int slot, uint64_t nowMs, uint16_t seq);
    // The blow of that swing landed / was dodged (ext landed_ms): nothing left to react to.
    void NoteSwingLanded(int slot, uint64_t nowMs);
    // A late blow is held for the grace window (ext g_mdh): keeps the window open.
    void HoldSwing(int slot, uint64_t untilMs);

    // ext MdgWindow: the best open window (slot, or -1); *winMs gets the current window length.
    // Producers that narrow the window are gated (see the header note).
    int  Window(uint64_t nowMs, uint16_t* winMs);
    uint64_t WindowEnd();

    // ---- our own press (the defender's side) --------------------------------------------
    // ext MdgtPressNow / mdg_start: consumes a charge, starts the streak and the i-frames and
    // sends MSG_DODGE_PRESS. Returns false when no charge is left or a press is too recent.
    bool Press(uint64_t nowMs, Direction dir, uint64_t clipSid);
    bool Iframes(uint64_t nowMs);           // ext MdgIframes
    int  Charges() noexcept;                // ext g_mdgCh
    void NoteHit(uint64_t nowMs, float hp); // ext MdgHitWatch (a drop cuts the charges to one)
    void SetStrictCooldown(bool strict);    // ext MwStrict(): 2500 ms vs 1500 ms

    // A blow we dodged (ext MdgSendRes): sends MSG_DODGE_RES to the attacker.
    void NoteDodged(uint64_t nowMs, int attackerSlot);

    // ---- receive side (the pure decode; the animation half is gated) --------------------
    bool RecvSwing(int slot, uint64_t nowMs, const uint8_t* buf, size_t len);  // ext MdgRecvSwing
    bool RecvPress(int slot, uint64_t nowMs, const uint8_t* buf, size_t len);  // ext MdgRecvPress
    bool RecvResult(int slot, uint64_t nowMs, const uint8_t* buf, size_t len); // ext MdgRecvRes

    // A remote dodge's immune bit, for contact suppression (ext MdgSlotImmune). `now` is the
    // receiver's clock; the press is phase-advanced by the one-way latency by the caller.
    bool SlotImmune(int slot, uint64_t nowMs);

    void Tick(uint64_t nowMs);
    void Reset();

} // namespace gameplay::melee_dodge
