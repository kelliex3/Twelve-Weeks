#pragma once

#include <cstddef>
#include <cstdint>

// Ported from tools/ext/melee_exchange.c's state machine and wire core: the melee EXCHANGE --
// a turn-based sequence between two standing enemies on a shared clock. A press at a nearby
// enemy (ENGAGE) sends START {uid, blow, weapon class, T0, contact time}; the defender answers
// ACK; at T0 the attacker's swing fires and the defender's dodge window opens (T0 .. T0+tc+vis);
// the defender's dodge press decides the verdict (HIT / DODGED), sent as RES {verdict, R}. The
// attacker HOLDs its engine contact until the verdict is due (HIT replays it, DODGED plays the
// miss). A HIT opens a 600 ms COMBO press; a DODGED hands off to dcounter (gated).
//
// Ported pieces (the pure core):
//   * the constants (MX_COOL_MS 1200, MX_COMBO_MS 600, MX_MAX_BLOW 3, MX_ACK_GRACE_MS 600,
//     MX_REACH 2.0, MX_CONE_COS 0.5) and the tune defaults {440, 520, 60};
//   * Lat/Vis/Tc derivation and the shared-clock window math (T0, win_hi, R / rdue);
//   * the MSG_MEX 0x9C wire shape (op 1 START / 2 ACK / 3 RES / 4 END) and its validation;
//   * the role / beat state machine, the verdict logic and the contact / combo / cooldown
//     bookkeeping, including the idempotent START / ACK / RES repeats.
//
// This is the PURE core. The engine half -- press injection (padinj), firing the engine swing,
// replaying / suppressing contact, the dodge window feed (melee-dodge), dcounter, melee_seq,
// melee_qte, camera, prompts and the "busy / down / paired" gates -- stays behind MexSink.
// With no sink installed the module only tracks state and is inert. Wire opcodes are reserved
// in net-protocol.hpp (NEVT_MEX = 0x9C).
//
// Wire (MSG_MEX, ext layout, payload after the message id):
//   [u8 op][u16 uid], then
//   op 1 START [u8 target][u8 blow 1..3][u8 wcls][u16 T0-in][u16 tc][u8 qflags][u16 qa]
//   op 2 ACK   [u8 ok][u8 reason][u8 qcap]
//   op 3 RES   [u8 verdict 1=hit/2=dodged][u8 blow][u16 R-in]
//   op 4 END   [u8 reason]

namespace gameplay::melee_exchange {

    inline constexpr uint16_t kCoolMs      = 1200;
    inline constexpr uint16_t kComboMs     = 600;
    inline constexpr uint8_t  kMaxBlow     = 3;
    inline constexpr uint16_t kAckGraceMs  = 600;
    inline constexpr float    kReachM      = 2.0f;
    inline constexpr float    kConeCos     = 0.5f;
    inline constexpr uint16_t kHardDeadlineMs = 9000;
    inline constexpr int      kMaxPeers    = 8;

    // ext MXB_* beats.
    enum Beat : uint8_t { kNone = 0, kEngage = 1, kAttack = 2, kCombo = 3, kCounter = 4 };
    enum Role : uint8_t { kIdle = 0, kAttacker = 1, kDefender = 2 };
    enum Verdict : uint8_t { kNoneVerdict = 0, kHit = 1, kDodged = 2 };

    struct Tune {
        uint16_t fistTc = 440;      // tune[0]
        uint16_t weaponTc = 520;    // tune[1]
        uint16_t maxVis = 60;       // tune[2]
    };

    // Everything the ext reached through PsyncOneway / MtellTc / the pad, the engine swing,
    // the dodge / counter / seq / qte modules, the camera and the send trampoline.
    struct Sink {
        virtual ~Sink() = default;
        virtual uint64_t NowMs() = 0;
        virtual uint8_t  LocalSlot() = 0;
        virtual float    OnewayMs(uint8_t slot) = 0;  // ext PsyncOneway
        virtual uint16_t TellTc(int wcls, uint16_t authored) = 0; // ext MtellTc
        virtual uint16_t QteFlags() = 0;              // ext MqTxFlags
        virtual uint16_t QtePressOffset() = 0;        // ext MqTxA
        virtual uint8_t  QteCap() = 0;                // ext MqCap
        virtual bool     PredBlocked(uint8_t slot) = 0;// busy / down / paired / not an enemy in reach
        virtual void     Send(uint8_t dest, const uint8_t* body, uint8_t len) = 0;
        virtual void     Log(const char* text) = 0;

        // Engine / other-feature effects (gated until those ports land).
        virtual void PlayFreeSwing() = 0;             // the press delivered as a normal swing
        virtual void FireSwing() = 0;                 // the engine swing starts at T0
        virtual void ReplayHit(uint8_t blow) = 0;     // the held contact replays on HIT
        virtual void PlayMiss() = 0;                  // the defender dodged (mdg_attacker_missed)
        virtual void BeginQte(uint8_t role) = 0;      // ext MqBegin
        virtual void EndQte() = 0;                    // ext MqEnd / MqPairEnd
    };

    void SetSink(Sink* sink);
    void SetTune(const Tune& tune);
    Tune GetTune();

    // ext MexLat / MexVis / MexTcOf / MexTcRaw, as pure functions of the one-way latency.
    int LatencyBufferMs(float onewayMs);
    int VisualLagMs(float onewayMs);
    int ContactMs(int wcls);
    // Build the START body for a local engage (10 bytes for old builds; 13 with the QTE tail).
    uint8_t BuildStart(uint8_t* out, uint16_t uid, uint8_t target, uint8_t blow,
                       uint8_t wcls, uint16_t t0In, uint16_t tc);

    // ---- local attacker -------------------------------------------------------------------
    // ext MexEngage (the candidate / reach / cone / "ready" gates are supplied by the caller).
    // `target` is the peer slot, `wcls` the weapon class (0..3), `onewayMs` that peer's RTT/2.
    bool Engage(uint64_t now, uint8_t target, uint8_t wcls, float onewayMs);
    // ext MexContact: an engine contact on `slot` while we are the attacker. Returns true when
    // the contact was consumed (held / dropped), false when the normal hit path should run.
    bool Contact(uint64_t now, uint8_t slot, uint8_t blow, bool lethal);
    // ext MexDefVerdict (defender side): decide HIT (1) / DODGED (2) and send RES.
    void DefVerdict(uint64_t now, Verdict verdict, const char* why);
    // ext MexNoteDodge: the defender's dodge press inside the window; `immune` is its i-frames.
    void NoteDodge(uint64_t now, bool immune);
    // ext MexNoteEarlyPress: a dodge press before the window opened burns this beat.
    void NoteEarlyPress();
    // ext MexWindowEnd: feed melee-dodge's window query (0 = not ours, 1 = *end set, 2 = closed).
    int WindowEnd(uint8_t slot, uint64_t* end);

    // ---- wire -----------------------------------------------------------------------------
    // ext MexRecv: decode and apply one MSG_MEX body (`buf` starts at op). Returns false for a
    // malformed / stale message.
    bool Receive(uint8_t from, const uint8_t* buf, size_t len);

    // ---- per-frame -------------------------------------------------------------------------
    // ext MexTick's state machine (the pad / engine / busy gates are supplied by the caller via
    // `inputPressed` and Sink::PredBlocked).
    void Tick(uint64_t now, bool inputPressed);
    void Reset(const char* why, bool notify, bool cool);
    void OnLeftMatch();

    Role CurrentRole() noexcept;
    Beat CurrentBeat() noexcept;
    Verdict CurrentVerdict() noexcept;

} // namespace gameplay::melee_exchange
