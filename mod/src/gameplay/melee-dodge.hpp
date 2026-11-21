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
// The window now also applies the ported melee_tell shortening (MtellWinMs / MtellReactMs),
// the melee_exchange / melee_duel window overrides, and the MdgGeomOk geometry test. Those
// producers are injected through WindowOverrides; with no overrides installed Window() keeps
// the base ext rule. The late-press hold (g_mdh) is the pure grace / replay state; the engine's
// busy / match-state gates are supplied by the caller. Wire opcodes live in net-protocol.hpp.

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

    // ext MD_REACH / MD_COS / MD_TURN_MS and MdgGeomOk's loose-mode tolerances.
    inline constexpr float    kReachM        = 3.6f;
    inline constexpr float    kLooseReachM   = 4.4f;  // MD_REACH + 0.8
    inline constexpr float    kHeightTolM    = 1.3f;
    inline constexpr float    kConeCos       = 0.766f; // ~40 deg
    inline constexpr float    kLooseConeCos  = 0.5f;
    inline constexpr uint64_t kTurnMs        = 450;   // a young swing only needs the front half
    inline constexpr float    kNearDistM     = 0.05f;

    // ext g_mdh: a blow waiting for a late dodge press. The caller owns the engine objects
    // (me / attacker / weapon / ammo) and receives the blow back through Sink::ReplayBlow.
    struct HeldBlow {
        uint64_t me = 0;
        uint64_t attacker = 0;
        uint64_t weapon = 0;
        uint64_t ammo = 0;
        float    damage = 0.0f;
        uint8_t  flag = 0;
        int      slot = -1;
        int      graceMs = 0;
        uint64_t t0 = 0;
        uint64_t until = 0;
    };

    // MdgGeomOk's pure geometry: positions in metres, quaternion in x,y,z,w order. The ext
    // used the engine's puppet locator and the local player's locator; the host supplies both.
    struct Pose {
        float pos[3] = { 0.0f, 0.0f, 0.0f };
        float rot[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    };
    bool GeometryOk(const Pose& self, const Pose& peer, uint64_t swingMs, uint64_t nowMs,
                    bool loose, float* dist = nullptr);

    // Movement directions (body frame): 0 front, 1 left, 2 right, 3 back.
    enum Direction : uint8_t { kFront = 0, kLeft = 1, kRight = 2, kBack = 3 };

    struct Sink {
        virtual ~Sink() = default;
        virtual void SendSwing(uint8_t dest) = 0;    // MSG_DODGE_SWING 0x97, [u8 1] armed
        virtual void SendPress(uint8_t dest, uint8_t immune, uint8_t dir, uint64_t clipSid) = 0;
        virtual void SendResult(uint8_t dest) = 0;   // MSG_DODGE_RES 0x98, empty
        virtual void Log(const char* text) = 0;
        // A held late-press blow is replayed here once its grace ends (or a press covers it).
        // No-op by default so existing inert sinks stay valid.
        virtual void ReplayBlow(const HeldBlow& blow) { (void)blow; }
    };

    void SetSink(Sink* sink);

    // Producers that narrow a peer's window (ext MdgWindow). A callback returning 0 means "not
    // ours", 1 means "*end is the window end", 2 means "closed" (drop this swing). The geometry
    // callback should call GeometryOk with the peer's current net pose.
    struct WindowOverrides {
        int  (*exchange)(int slot, uint64_t* end) = nullptr; // ext MexWindowEnd
        int  (*duel)(int slot, uint64_t nowMs, uint64_t swingMs, uint64_t* end) = nullptr; // MduWindowEnd
        bool (*geometry)(int slot, uint64_t nowMs, bool loose, float* dist) = nullptr;
    };
    void SetWindowOverrides(const WindowOverrides& overrides);

    // ---- swing tracking (the attacker's side feeds these) -------------------------------
    // A remote swing began at `nowMs` with its sequence number (ext g_mdg.sw[s]).
    void NoteSwing(int slot, uint64_t nowMs, uint16_t seq);
    // The blow of that swing landed / was dodged (ext landed_ms): nothing left to react to.
    void NoteSwingLanded(int slot, uint64_t nowMs);
    // A late blow is held for the grace window (ext g_mdh): keeps the window open.
    void HoldSwing(int slot, uint64_t untilMs);

    // ---- late-press hold (ext MdgHitHold / MdgHeldTick) -------------------------------------
    // onewayMs is the ext PsyncOneway(as); playoutMs is the receive buffer's current delay
    // (ext g_ns[as].dcur). The grace is round(oneway + playout), clamped to 40..90 ms -- the
    // ext's current tuning (its older comment still says 60..180). Returns false when this
    // blow must take the normal hit path.
    bool HoldBlow(const HeldBlow& blow, uint64_t nowMs, float onewayMs, float playoutMs);
    void HeldTick(uint64_t nowMs, bool matchLive, bool busy = false);
    bool HeldActive() noexcept;

    // ext MdgWindow: the best open window (slot, or -1); *winMs gets the current window length.
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
