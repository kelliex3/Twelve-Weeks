#pragma once

#include <cstddef>
#include <cstdint>

// Ported from tools/ext/melee_hitreact.c's timing / state core: the SP hit reaction a victim
// plays when a melee blow lands and the "attacker's next swing waits for the impact part" lock
// it creates on every other screen. Ported pieces:
//   * MHR_CUT_MS {360, 440, 520} by strength, MhrStrength's fist / one-hand / two-hand rule;
//   * the MSG_HITREACT2 0x9A wire shape (bit0 reaction, bit1 landed, bit2 pain + str / sharp,
//     bit6 punch, bit7 left-reaction) and its cut range validation [150, 600] ms;
//   * the victim's graze window (1 s) and replace rules, reaction start / natural cut;
//   * the attacker's provisional swing lock (max 480 ms, 900 ms confirmation window) and its
//     release on a no-reaction notice;
//   * the 50 ms predicted puppet reaction (arm / fire / confirm / undo).
//
// This is the PURE core. Everything that needs the game -- the SP clip table + root motion, the
// body pin / animation state machine, pain VO, punch SFX, blade / blunt FX, the camera, the
// weapon-class lookup and the "busy / duel / dodge / downed" gates -- stays behind HitReactSink.
// The clip table (MHR_POOL / MHR_2H) is generated/private, so PickClip / ExitMsOf are honest
// gates the caller fills from the ext until that data is ported. With no sink installed the
// module only tracks state and is inert (the cutover is still driven by t2r-*.txt in the ext).
//
// Wire (MSG_HITREACT2, ext layout, 11 bytes):
//   [u8 flags][u64 clip sid][u16 cut ms]
// The event id is reserved as NEVT_HITREACT2 in net-protocol.hpp.

namespace gameplay::melee_hit_react {

    inline constexpr uint16_t kCutMs[3]      = { 360, 440, 520 }; // ext MHR_CUT_MS
    inline constexpr uint16_t kAtkMaxMs      = 480;   // ext MHR_ATK_MAX_MS (lock cap)
    inline constexpr uint16_t kGrazeMs       = 1000;  // ext MHR_GRAZE_MS
    inline constexpr uint16_t kPredictGrace  = 50;    // ext MHR_PRED_GRACE_MS
    inline constexpr uint16_t kMinCutMs      = 150;   // ext MhrRecv reaction validation
    inline constexpr uint16_t kMaxCutMs      = 600;
    inline constexpr uint16_t kConfirmMs     = 900;   // ext: lock / prediction confirm window
    inline constexpr size_t   kMessageSize   = 11;
    inline constexpr int      kMaxPeers      = 8;

    // Blow strength (ext str 0/1/2: light / medium / heavy).
    enum Strength : int { kLight = 0, kMedium = 1, kHeavy = 2 };

    // The portable essence of MhfxClass + MeleeClassOf + MhrStrength: the caller maps its
    // weapon id to one of these three classes; the rule below is the ext's exact derivation.
    enum BlowClass : uint8_t { kFist = 0, kOneHanded = 1, kTwoHanded = 2 };

    // MSG_HITREACT2 byte 0 flags (ext layout).
    enum Flags : uint8_t {
        kFReaction = 0x01,  // clip sid is non-zero and a reaction plays
        kFLanded   = 0x02,  // the blow landed (attacker plays its impact sound)
        kFPain     = 0x04,  // pain grunt follows; strength in bits 3..4, sharp in bit 5
        kFPunch    = 0x40,  // a fist blow (attacker's punch sound plays)
        kFLeft     = 0x80,  // victim left its reaction early (blend our clip out on its puppet)
    };
    inline uint8_t PainStrength(uint8_t flags) { return static_cast<uint8_t>((flags >> 3) & 3); }
    inline bool    PainSharp(uint8_t flags)    { return (flags & 0x20) != 0; }

    struct Blow {
        BlowClass cls    = kFist;
        float     damage = 0.0f;
        int       dir    = 0;      // 0 front, 1 behind, 2 left, 3 right (attacker around victim)
        bool      sharp  = false;  // blade -> pain is sharp
        bool      fistSfx = false; // fist weapon -> punch sound on the victim's head
        uint64_t  weapon = 0;      // sid, for the caller's clip pick
    };

    struct VictimGates {
        bool busy             = false;  // downed / stun / execution / paired move / intro
        bool crouched         = false;
        bool duelOwnsReaction = false;  // a duel's synced -hit pair IS the reaction
        bool plainDuelBlow    = false;  // replace a running reaction; bypass the graze window
    };

    // The clip table and the engine-facing effects. Every method is an honest gate: until a
    // real port installs them, PickClip returns sid 0 (no reaction) and the rest are no-ops.
    struct Sink {
        virtual ~Sink() = default;
        virtual uint64_t NowMs() = 0;

        // Pick the victim's reaction clip for this blow (the ext's MhrPick / MhrSpPick over
        // MHR_POOL). `avoidSid` is the last clip played (the "no two in a row" rule). Return
        // sid 0 for no clip. `cutMsOut` is filled with kCutMs[strength] unless the clip needs
        // a different authored cut. `strengthOut` may upgrade the derived strength to the
        // nearest loaded clip (the ext's "used" value).
        virtual bool PickClip(const Blow& blow, int strength, bool twoHanded,
                              uint64_t avoidSid, uint64_t* sidOut,
                              uint16_t* cutMsOut, int* strengthOut) = 0;

        // When a peer's clip ends (MhrExitMsOf), for extending a predicted reaction on its
        // puppet once the victim's word confirms the cut.
        virtual uint64_t ExitMsOf(uint64_t sid, uint16_t cutMs) = 0;

        // Effects (all gated). `slot == 255` means the local player / our own body.
        virtual void PlayLocalReaction(uint64_t sid, uint16_t cutMs, int strength) = 0;
        virtual void StopLocalReaction(uint64_t sid, float blendSec) = 0;
        virtual void PlayPuppetReaction(uint8_t slot, uint64_t sid, uint16_t cutMs, uint64_t ageMs) = 0;
        virtual void StopPuppetReaction(uint8_t slot, uint64_t sid, float blendSec) = 0;
        virtual void PainSound(uint8_t slot, int strength, bool sharp) = 0;
        virtual void Punch(uint8_t slot, int strength) = 0;
        virtual void HitFxConfirm(uint8_t slot) = 0;

        // True while this peer's dodge / down / paired / busy state forbids a predicted
        // reaction (ext MhrPredTick's `skip`). The caller owns those separate features.
        virtual bool PredBlocked(uint8_t slot) = 0;

        // The peer's RTT in ms (ext g_psRtt), used to phase the attacker's lock by half an RTT.
        virtual float RttMs(uint8_t slot) = 0;

        virtual void SendMessage(const uint8_t body[kMessageSize]) = 0;
        virtual void Log(const char* text) = 0;
    };

    void SetSink(Sink* sink);

    // ext MhrStrength, over the BlowClass abstraction.
    Strength DeriveStrength(BlowClass cls, float damage, bool* twoHanded);

    // ---- victim side (a blow landed on us; we decide our own reaction) -------------------
    // Returns true when a reaction started. A skip (busy / crouch / graze / no clip) leaves a
    // running reaction untouched, exactly like the ext.
    bool OnBlowTaken(int slot, const Blow& blow, const VictimGates& gates);
    // We left a running reaction early (our input / a dodge took the body): stop it locally and
    // tell the other screens to blend their copy of the clip out (flags 0x80).
    void EndReactionEarly(const char* why);
    // A blow that landed but produced no reaction clip (ext MhrVictimNone2): `landed` 0 = the
    // victim says it did not land, 1 = landed, 2 = landed and hurt us standing (pain grunt).
    void NoteNoReaction(int landed, bool fistSfx);
    // Natural cut (control back). Call every tick; ends the reaction at start + cut.
    void VictimTick();

    bool ReactionActive() noexcept;
    uint64_t ReactionSid() noexcept;

    // ---- attacker side (our blow landed on a peer) --------------------------------------
    // ext MhrAtkHit + MhrPredArm + MpaceNoteVictim: arm the provisional lock and the predicted
    // puppet reaction. `dodgeImmune` is the victim's known dodge (melee-dodge::SlotImmune);
    // `heavy` skips the reaction (the ext's heavy branch).
    void OnBlowDealt(int slot, const Blow& blow, bool heavy, bool dodgeImmune);
    // ext MpaceNoteVictim: the absolute time until which our next swing start is held. 0 = free.
    uint64_t SwingLockUntil() noexcept;

    // ext MhrPredTick: fire any predicted puppet reaction whose 50 ms grace has elapsed.
    void PredTick();

    // Decode and apply a peer's MSG_HITREACT2 body (ext MhrRecv's portable core).
    void Receive(int slot, const uint8_t* buf, size_t len);

    // Pain VO / punch SFX opt-outs (ext t2r-no-melee-pain-vox.txt / MhrPunchOld) and the
    // prediction opt-out (ext t2r-no-melee-predict.txt). Off by default = ext behaviour.
    void SetPainEnabled(bool enabled) noexcept;
    void SetPunchEnabled(bool enabled) noexcept;
    void SetPredictionEnabled(bool enabled) noexcept;

    void Tick();
    void Reset();

} // namespace gameplay::melee_hit_react
