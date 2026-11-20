#pragma once

#include <cstddef>
#include <cstdint>

// Ported from tools/ext/t2r-ext.c's 2013 downed state (feature_downed / DownedSet /
// RevivedRecv / RvSet / RvProg / the MSG_DOWNED and MSG_REVIVE peer handlers). The ext
// spreads this state across ~40 call sites (fall clips, camera, HUD bar, puppet handling,
// gore, hitbox, music, VO). This module owns ONLY the state + rules that can be reasoned
// about without the engine:
//
//   * down / dying / up state, epoch and the downer slot;
//   * the downed pool (50 hp), the 1 s down-protection window and hits while downed;
//   * the real-time bleed-out clock (base mode seconds, shortening per down/revive, pause
//     while a teammate is reviving, hit-shortening of the remaining time);
//   * multi-reviver progress (every attached reviver contributes elapsed x rate / time,
//     rates 1 / 0.6 / 0.4, capped at 2x) and the first-attached completion rank;
//   * per-peer downed state with epoch-based reordering protection and resend handling;
//   * the exact MSG_DOWNED (0x43, 6 bytes) and MSG_REVIVE (0x44, 1 byte) wire layouts.
//
// This is the PURE core. Everything that touches the engine, transport, other features or
// the game's data is behind DownedSink; with no sink installed the module is inert. The
// integration seam is net/match/downed-bridge.* (master gate false), so the ext stays the
// behaviour owner and no t2r-*.txt cutover is flipped. Wire IDs are reserved in
// net-protocol.hpp at their exact ext values; no low 0..18 event is touched.
//
// Wire (ext layout, payload after the message id):
//   MSG_DOWNED [u8 state 0 up/1 down/2 dying][u8 downer 0xFE = revived, not a death]
//              [u8 knockDir + 1][u8 supine / dieDir + 1][u8 bleedSecs + 0.5][u8 epoch]
//   MSG_REVIVE [u8 who]  (the reviver's slot; sent to everyone, not just the victim)

namespace net::match::downed {

    inline constexpr int      kMaxPeers           = 8;
    inline constexpr uint8_t  kStateUp            = 0;
    inline constexpr uint8_t  kStateDown          = 1;
    inline constexpr uint8_t  kStateDying         = 2;
    inline constexpr uint8_t  kRevivedDowner      = 0xFE; // ext's "up, but a revive" marker
    inline constexpr float    kDownHp             = 50.0f;
    inline constexpr float    kReviveHp           = 40.0f;
    inline constexpr float    kBleedSecs          = 15.0f; // Supply Raid base
    inline constexpr float    kIntBleedSecs       = 7.0f;  // Interrogation base
    inline constexpr uint64_t kProtectMs          = 1000;  // first second ignores hits
    inline constexpr uint64_t kDownResendFrames   = 60;    // ext: !(g_frame % 60)
    inline constexpr uint64_t kUpResendMs         = 300;
    inline constexpr int      kUpResendCount      = 3;     // lost single-shot "up" copies
    inline constexpr uint64_t kEpochWindowMs      = 10000; // older epoch = reordered, ignore
    inline constexpr uint64_t kStaleReviveFrames  = 120;   // late "downed" after our revive
    inline constexpr uint64_t kReviveGraceMs      = 3000;  // revive start + this = timeout
    inline constexpr float    kCoRate2            = 0.6f;  // second reviver
    inline constexpr float    kCoRate3            = 0.4f;  // third (fourth adds nothing)

    // Why the down ended. The wire distinguishes a revive (downer 0xFE, not a death)
    // from every death path (bleed-out, shot / blow while down, execution).
    enum class End : uint8_t { kNone = 0, kRevived, kDied };

    struct Config {
        float bleedSecs = kBleedSecs; // mode base; producers also feed per-down multipliers
        float coRate2 = kCoRate2;     // t2r-revive-corate.txt second rate
        float coRate3 = kCoRate3;     // third rate (fourth is capped)
        bool  oldRules = false;       // legacy death check while a reviver is attached
    };

    // Everything the ext reached through its engine calls, send trampoline, skills,
    // kill-feed / awards, camera, animations and the other (unported) features. The
    // bridge supplies a sink; the pure core never calls the engine itself.
    struct Sink {
        virtual ~Sink() = default;
        virtual uint64_t NowMs() = 0;                    // ext GetTickCount64
        virtual uint64_t Frame() = 0;                    // ext g_frame (60 Hz counter)
        virtual uint8_t  LocalSlot() = 0;                // ext kMySlot
        virtual bool     InMatch() = 0;                  // ext MatchSubstate() == 6
        virtual bool     PeerActive(uint8_t slot) = 0;   // ext T2rPeer(slot)[4] && handle
        virtual float    LocalHp() = 0;                  // ext LocalHp()
        virtual float    BleedMul() = 0;                 // Fortitude x Heavy Hands; 1 = none
        virtual float    DownHpMul() = 0;                // Fortitude II pool bonus; 1 = none
        virtual void     SendDowned(const uint8_t body[6]) = 0; // to 0xff (all peers)
        virtual void     SendRevive(uint8_t who) = 0;          // to 0xff
        virtual void     Log(const char* text) = 0;

        // Engine / other-feature effects (gated until their producers land).
        virtual void OnDownBegin(uint8_t downer, float bleedSecs) = 0;
        virtual void OnDownEnd(End why, uint8_t source) = 0;
        virtual void OnBleedOut(uint8_t downer) = 0;      // kill / credit / death clip
        virtual void OnProtectedHit(float dmg) = 0;       // hit ignored in the first second
        virtual void OnDownHit(float dmg, float hpLeft) = 0;
        virtual void OnRevived(uint8_t reviver) = 0;      // award / VX / hp / get-up
        virtual void OnPeerState(uint8_t slot, uint8_t state, uint8_t downer) = 0;
    };

    void SetSink(Sink* sink);
    void SetConfig(const Config& config);
    Config GetConfig();

    // ---- local down state ------------------------------------------------------------------
    bool     IsDown() noexcept;
    uint8_t  DownerSlot() noexcept;
    uint8_t  Epoch() noexcept;
    uint64_t DownMs() noexcept;
    float    BleedSecs() noexcept;
    // 0..1+ fraction of the bleed-out clock actually elapsed (pause subtracted). This is the
    // same clock the ext's downed HUD bar and downed_music stage selection both read.
    float    BleedFraction();
    // ext DownedSet(1). A second down while already down is ignored (no second award or
    // bleed-out restart). Returns true when the down was entered. `knockDir` is the ext's
    // byte-2 fall direction, `supine` its byte-3 lying-on-the-back flag.
    bool     BeginDown(uint8_t downer, uint8_t knockDir = 0, bool supine = false);
    // ext DownedSet(0) + the revive path. `source` is the downer (death) or reviver.
    void     EndDown(End why, uint8_t source);
    // ext's "hit on a downed player" block. Returns true when the hit was lethal and the
    // caller must perform the death; false when it was absorbed / ignored. `hpBefore` is
    // the observed pool before this hit (the caller supplies it so the core stays engine-free).
    bool     HitWhileDown(float dmg, float hpBefore);

    // ---- revive (multi-reviver aggregation) -------------------------------------------------
    // ext RvSet: a hold [target][reviver] starts at startMs (0 = now) with revive time secs.
    void     SetHold(uint8_t target, uint8_t reviver, bool on, float secs, uint64_t startMs);
    void     ClearHolds(uint8_t target);                  // ext RvClear
    float    ReviveProgress(uint8_t target);              // ext RvProg, 0..1+
    int      ReviveRank(uint8_t target, uint8_t reviver); // 0 = first attached, -1 = not
    // ext g_revBy / g_revSecsMe / g_rev_by_ms: a teammate is reviving US (MSG_UC4_REACH
    // producer stays gated; the caller feeds these).
    void     SetLocalReviver(uint8_t slot, float secs);
    void     ClearLocalReviver();
    bool     BeingRevived() noexcept;
    // ext MSG_REVIVE receive / RevivedRecv: someone completed the revive on us.
    void     ReceiveRevive(uint8_t from);
    // ext's reviver-side completion: books the down closed and broadcasts MSG_REVIVE.
    void     CompleteRevive(uint8_t target, uint8_t reviver);

    // ---- wire ------------------------------------------------------------------------------
    // Build a 6-byte MSG_DOWNED body. Returns 6. `state` kState*; `mix` is the byte-3 value
    // (supine flag while down, death direction while dying); `bleedSecs` the down's clock.
    uint8_t  BuildDowned(uint8_t out[6], uint8_t state, uint8_t downer,
                         uint8_t knockDir, uint8_t mix, float bleedSecs);
    // ext's MSG_DOWNED receive: epoch reorder guard, peer state / bleed / direction tracking,
    // stale-resend-after-revive suppression. Returns true when the state actually changed.
    bool     ReceiveDowned(uint8_t from, const uint8_t* buf, size_t len);

    uint8_t  PeerState(uint8_t slot) noexcept;
    uint8_t  PeerKnockDir(uint8_t slot) noexcept;
    uint8_t  PeerDeathDir(uint8_t slot) noexcept;
    bool     PeerSupine(uint8_t slot) noexcept;
    float    PeerBleed(uint8_t slot) noexcept; // seconds (buf[4] - 0.5), 0 = unknown
    uint64_t PeerDownFrame(uint8_t slot) noexcept;

    // ---- per-frame / reset ------------------------------------------------------------------
    // ext's downed tick bookkeeping: down / up resends, revive hold expiry, local reviver
    // timeout, bleed pause accumulation and bleed-out expiry. Engine effects go to the sink.
    void     Tick();
    // Full session reset (6 -> !6 edge and leaving the match).
    void     Reset();
    // Per-life reset of the down / revive counters (ext's spawn path); keeps peer state.
    void     ResetPerLife();
    void     OnPeerLeft(uint8_t slot);

} // namespace net::match::downed
