#pragma once

#include <cstddef>
#include <cstdint>

// Ported from tools/ext/t2r-ext.c's "reliable one-shot messages (hitrel)" section. The UDP
// relay does not retransmit; a peer that announced MSG_HIT_REL (op 0 hello, every 1.5 s in a
// match) receives each type-3 hit / MSG_ARMOR_HIT / molotov burst ONLY as a reliable message,
// sent at {0, 60, 150, 300, 450, 600} ms after the first copy, with a per-destination sequence
// number. The receiver ACKs every copy, applies the first one through the same receive path the
// plain message would use, and drops the rest with a 64-entry aged ring per sender. Peers
// without the capability keep the plain message; a capable peer never gets both.
//
// This is the PURE CORE: sequence numbers, the capability latch, the retransmit queue, the
// dedupe ring, the ACK handling and the delivery statistics. All engine / transport contact is
// through HitRelSink, which the caller supplies. With no sink installed the module is inert
// (the honest gate while the ext remains the transport bridge). The wire opcodes are reserved
// in net-protocol.hpp (NEVT_HIT_REL = 0x6C, NEVT_ARMOR_HIT = 0x4A).
//
// Wire (MSG_HIT_REL, ext layout): [u8 op][u16 seq][u8 victim][payload]
//   op 0  hello / capability           (4 bytes, victim 0)
//   op 1  a type-3 hit                 (payload 0x16 bytes)
//   op 2  an armour hit                (payload 11 bytes)
//   op 3  a molotov burst              (payload 20 bytes)
//   op 4  ACK of one seq               (4 bytes)
// Callers hand us the bytes AFTER the ext message id; the core adds the four header bytes.

namespace net::match::hitrel {

    inline constexpr int      kRingSize        = 64;
    inline constexpr int      kQueueSize       = 96;
    inline constexpr uint64_t kCapMs           = 6000;    // hello older than this = not capable
    inline constexpr uint64_t kHelloEveryMs    = 1500;
    inline constexpr uint64_t kLogEveryMs      = 10000;
    inline constexpr uint16_t kRetransmitSchedule[] = { 60, 150, 300, 450, 600 };

    // ext HitrelTick's g_hr_tx / g_hr_st counters, plus the sender-side delivery estimate.
    struct Stats {
        uint32_t sent = 0;       // reliable messages queued for this destination
        uint32_t acked = 0;      // ACKed before the 600 ms give-up
        uint32_t retransmits = 0;// copies beyond the original x3 burst
        uint32_t never = 0;      // gave up unacknowledged
        uint32_t sentBySeq = 0;  // their originals estimated from the seq gaps
        uint32_t delivered = 0;  // first copies we applied
        uint32_t duplicates = 0; // copies we dropped
        uint32_t originals = 0;  // plain messages we saw from them (while g_hrApplying == 0)
        uint32_t sentReliable = 0;  // our own hits sent reliably / plainly (log only)
        uint32_t sentPlain = 0;
    };

    // Everything the ext reached through g_sendTramp / T2rPeer / RecvHook / armor_hit_recv /
    // BombBurstRecv / MatchSubstate. The bridge implements these; the core never calls the
    // engine directly.
    struct Sink {
        virtual ~Sink() = default;
        virtual uint64_t NowMs() = 0;                 // ext GetTickCount64
        virtual bool     InMatch() = 0;               // ext MatchSubstate() == 6
        virtual bool     PeerActive(uint8_t slot) = 0;// ext T2rPeer(slot)[4] && handle
        virtual uint8_t  LocalSlot() = 0;             // ext kMySlot
        virtual void     SendWire(uint8_t dest, const uint8_t* msg, uint8_t len) = 0;
        virtual void     SendPlainHit(uint8_t dest, const uint8_t hit[0x16]) = 0;
        virtual void     SendPlainArmor(uint8_t dest, const uint8_t armor[11]) = 0;
        virtual void     SendPlainBurst(uint8_t dest, const uint8_t* buf, uint8_t len) = 0;
        virtual void     ApplyHit(uint8_t from, const uint8_t hit[0x16]) = 0;
        virtual void     ApplyArmor(uint8_t from, const uint8_t armor[11]) = 0;
        virtual void     ApplyBurst(uint8_t from, const uint8_t* buf, uint8_t len) = 0;
        virtual void     Log(const char* text) = 0;
    };

    void SetSink(Sink* sink);

    // true while `slot`'s last hello is younger than kCapMs (ext HrCap).
    bool IsCapable(uint8_t slot);

    // ext HrSendHit / HrSendArmor / HrSendBurst. SendHit returns 0 when the reliable path was
    // taken (the ext's send trampoline is not called), matching the ext's return contract.
    uint64_t SendHit(uint8_t* buf, uint64_t len, uint64_t slot);
    void     SendArmor(const uint8_t* buf, uint64_t len);
    void     SendBurst(const uint8_t* buf, uint8_t len);

    // ext HrNoteOriginal: the send hook saw a plain hit from this slot (not one of our copies).
    void NoteOriginal(uint8_t from);

    // ext HrRecv: one MSG_HIT_REL body. Decodes the op, stamps capability / ACKs, dedupes and
    // calls the Sink's Apply* on the first copy only.
    void Receive(uint8_t from, const uint8_t* buf, uint64_t len);

    // ext HitrelTick: hello, retransmits, slot-reuse cleanup and the 10 s stats log.
    void Tick();

    // ext's "left match / slot reused" cleanup for one slot.
    void OnPeerLeft(uint8_t slot);

    // Clears every queue, ring, capability latch and counter (session reset / leaving the
    // live state). `full` also clears the receive rings, as the ext does on the 6 -> !6 edge.
    void Reset(bool full);

    Stats GetStats(uint8_t slot);

} // namespace net::match::hitrel
