// ported from tools/ext/t2r-ext.c (reliable one-shot messages / the hitrel section)

#include "hit-rel.hpp"

#include "net/protocol/net-protocol.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace net::match::hitrel {

    namespace {

        Sink* g_sink = nullptr;

        struct TxStats {
            uint32_t sent = 0, acked = 0, retx = 0, never = 0;
        };

        struct RecvStats {
            uint16_t hi = 0;
            uint32_t sentEst = 0;
            uint32_t orig = 0, first = 0, dup = 0;
            bool     have = false;
        };

        struct QueueEntry {
            uint8_t  b[32] = {};
            uint8_t  len = 0;
            uint8_t  dest = 0;
            uint8_t  step = 0;
            uint16_t seq = 0;
            uint64_t t0 = 0;
        };

        uint16_t  g_seq[kMaxNetPlayers] = {};
        TxStats   g_tx[kMaxNetPlayers];
        uint64_t  g_capMs[kMaxNetPlayers] = {};
        struct { uint16_t seq = 0; uint32_t ms = 0; } g_ring[kMaxNetPlayers][kRingSize];
        int       g_ringPos[kMaxNetPlayers] = {};
        RecvStats g_recv[kMaxNetPlayers];
        uint32_t  g_sent[kMaxNetPlayers] = {}, g_sentPlain[kMaxNetPlayers] = {};
        QueueEntry g_queue[kQueueSize];
        bool      g_applying = false;

        uint64_t  g_nextHello = 0, g_nextLog = 0;
        bool      g_wasLive = false;
        bool      g_wasActive[kMaxNetPlayers] = {};

        void Queue(uint8_t op, const uint8_t* payload, uint8_t plen,
                   uint8_t victim, uint8_t dest, uint16_t seq, uint64_t now)
        {
            for (QueueEntry& q : g_queue) {
                if (q.len)
                    continue;
                q.b[0] = op;
                std::memcpy(q.b + 1, &seq, 2);
                q.b[3] = victim;
                std::memcpy(q.b + 4, payload, plen);
                q.len = static_cast<uint8_t>(4u + plen);
                q.dest = dest;
                q.seq = seq;
                q.step = 0;
                q.t0 = now;
                return;
            }
            // Full: the first copy already went out, the repeats are dropped (bounded).
        }

        void Wire(uint8_t op, const uint8_t* payload, uint8_t plen,
                  uint8_t victim, uint8_t dest, uint64_t now)
        {
            if (!g_sink || dest >= kMaxNetPlayers || plen > 32u - 4u)
                return;
            uint16_t seq = ++g_seq[dest];
            if (!seq)
                seq = ++g_seq[dest];
            ++g_tx[dest].sent;
            uint8_t msg[32] = {};
            msg[0] = op;
            std::memcpy(msg + 1, &seq, 2);
            msg[3] = victim;
            std::memcpy(msg + 4, payload, plen);
            const uint8_t len = static_cast<uint8_t>(4u + plen);
            g_sink->SendWire(dest, msg, len);
            Queue(op, payload, plen, victim, dest, seq, now);
        }

        void SendHello(uint64_t now)
        {
            if (!g_sink)
                return;
            const uint8_t hello[4] = { 0, 0, 0, 0 };
            g_sink->SendWire(kInvalidSlot, hello, 4);
            (void)now;
        }

        void SendAck(uint8_t from, const uint8_t* buf)
        {
            if (!g_sink)
                return;
            const uint8_t ack[4] = { 4, buf[1], buf[2], 0 };
            g_sink->SendWire(from, ack, 4);
        }

        void ClearSlot(uint8_t slot, bool full)
        {
            if (slot >= kMaxNetPlayers)
                return;
            std::memset(g_ring[slot], 0, sizeof g_ring[slot]);
            g_ringPos[slot] = 0;
            g_capMs[slot] = 0;
            g_recv[slot] = RecvStats{};
            g_seq[slot] = 0;
            g_tx[slot] = TxStats{};
            g_sent[slot] = g_sentPlain[slot] = 0;
            if (full)
                g_wasActive[slot] = false;
            for (QueueEntry& q : g_queue)
                if (q.len && q.dest == slot)
                    q.len = 0;
        }

        void LogStats(uint64_t now)
        {
            if (!g_sink)
                return;
            char line[320];
            for (uint8_t s = 0; s < kMaxNetPlayers; ++s) {
                RecvStats& r = g_recv[s];
                if (r.orig || r.first || r.dup) {
                    std::snprintf(line, sizeof line,
                        "hitrel: slot %u sent %u (hits+armour, by seq), delivered by original %u / by copy %u, duplicates dropped %u, never arrived %d",
                        s, r.sentEst, r.orig, r.first, r.dup,
                        static_cast<int>(r.sentEst) - static_cast<int>(r.first));
                    g_sink->Log(line);
                }
                if (g_sent[s] || g_sentPlain[s]) {
                    std::snprintf(line, sizeof line,
                        "hitrel: to slot %u we sent %u hits reliably, %u plain",
                        s, g_sent[s], g_sentPlain[s]);
                    g_sink->Log(line);
                }
                if (g_tx[s].sent) {
                    std::snprintf(line, sizeof line,
                        "hitrel: to slot %u msgs sent %u, acked %u, retransmits %u, never acked (600 ms) %u",
                        s, g_tx[s].sent, g_tx[s].acked, g_tx[s].retx, g_tx[s].never);
                    g_sink->Log(line);
                }
                g_tx[s] = TxStats{};
                r.orig = r.first = r.dup = r.sentEst = 0;
                g_sent[s] = g_sentPlain[s] = 0;
            }
            (void)now;
        }

    } // namespace

    void SetSink(Sink* sink) { g_sink = sink; }

    bool IsCapable(uint8_t slot)
    {
        if (!g_sink || slot >= kMaxNetPlayers || !g_capMs[slot])
            return false;
        return g_sink->NowMs() - g_capMs[slot] < kCapMs;
    }

    uint64_t SendHit(uint8_t* buf, uint64_t len, uint64_t slot)
    {
        if (!g_sink || !buf)
            return 0;
        uint8_t hit[0x16] = {};
        std::memcpy(hit, buf, std::min<uint64_t>(len, sizeof hit));
        if (slot >= kMaxNetPlayers || len < 0x16) {
            if (slot < kMaxNetPlayers)
                ++g_sentPlain[static_cast<uint8_t>(slot)];
            g_sink->SendPlainHit(static_cast<uint8_t>(slot), hit);
            return 0;
        }
        const uint8_t dest = static_cast<uint8_t>(slot);
        if (!IsCapable(dest)) {
            ++g_sentPlain[dest];
            g_sink->SendPlainHit(dest, hit);
            return 0;
        }
        ++g_sent[dest];
        Wire(1, hit, sizeof hit, dest, dest, g_sink->NowMs());
        return 0;
    }

    void SendArmor(const uint8_t* buf, uint64_t len)
    {
        if (!g_sink || !buf || len < 11)
            return;
        uint8_t armor[11];
        std::memcpy(armor, buf, sizeof armor);
        for (uint8_t s = 0; s < kMaxNetPlayers; ++s) {
            if (!g_sink->PeerActive(s))
                continue;
            if (IsCapable(s))
                Wire(2, armor, sizeof armor, kInvalidSlot, s, g_sink->NowMs());
            else
                g_sink->SendPlainArmor(s, armor);
        }
    }

    void SendBurst(const uint8_t* buf, uint8_t len)
    {
        if (!g_sink || !buf || len > 20)
            return;
        for (uint8_t s = 0; s < kMaxNetPlayers; ++s) {
            if (!g_sink->PeerActive(s))
                continue;
            if (IsCapable(s))
                Wire(3, buf, len, kInvalidSlot, s, g_sink->NowMs());
            else
                g_sink->SendPlainBurst(s, buf, len);
        }
    }

    void NoteOriginal(uint8_t from)
    {
        if (from < kMaxNetPlayers && !g_applying)
            ++g_recv[from].orig;
    }

    void Receive(uint8_t from, const uint8_t* buf, uint64_t len)
    {
        if (!g_sink || from >= kMaxNetPlayers || !buf || len < 4)
            return;
        const uint8_t op = buf[0];
        uint16_t seq = 0;
        std::memcpy(&seq, buf + 1, 2);
        const uint64_t now = g_sink->NowMs();

        if (op == 0) {
            if (!g_capMs[from])
                g_sink->Log("hitrel: peer runs the reliable-hit build");
            g_capMs[from] = now ? now : 1;
            return;
        }
        g_capMs[from] = now ? now : 1;   // a sender of copies is capable too
        if (op == 4) {
            for (QueueEntry& q : g_queue)
                if (q.len && q.dest == from && q.seq == seq) {
                    q.len = 0;
                    ++g_tx[from].acked;
                    return;
                }
            return;
        }
        if ((op == 1 && len < 4 + 0x16) || (op == 2 && len < 4 + 11)
            || (op == 3 && len < 4 + 20) || op > 4)
            return;
        SendAck(from, buf);              // ACK every copy; an ACK itself can be lost

        RecvStats& st = g_recv[from];
        if (!st.have) {
            st.have = true;
            st.hi = static_cast<uint16_t>(seq - 1);
        }
        const uint16_t d = static_cast<uint16_t>(seq - st.hi);
        if (d && d < 1000) {
            st.sentEst += d;
            st.hi = seq;
        }
        for (const auto& e : g_ring[from])
            if (e.ms && e.seq == seq
                && static_cast<uint32_t>(now) - e.ms < 10000) {
                ++st.dup;
                return;
            }
        const int ri = g_ringPos[from]++ % kRingSize;
        g_ring[from][ri].seq = seq;
        g_ring[from][ri].ms = static_cast<uint32_t>(now) ? static_cast<uint32_t>(now) : 1;
        ++st.first;
        if (!g_sink->PeerActive(from))
            return;                      // copies up to 600 ms late are dropped
        if (op == 1) {
            const uint8_t victim = buf[3];
            if (victim != g_sink->LocalSlot())
                return;                  // not for us
            uint8_t hit[0x16];
            std::memcpy(hit, buf + 4, sizeof hit);
            g_applying = true;
            g_sink->ApplyHit(from, hit);
            g_applying = false;
        } else if (op == 3) {
            g_sink->ApplyBurst(from, buf + 4, 20);
        } else {
            uint8_t armor[11];
            std::memcpy(armor, buf + 4, sizeof armor);
            g_sink->ApplyArmor(from, armor);
        }
    }

    void Tick()
    {
        if (!g_sink)
            return;
        const uint64_t now = g_sink->NowMs();
        const bool live = g_sink->InMatch();
        if (!live) {
            for (QueueEntry& q : g_queue)
                q.len = 0;
            if (g_wasLive) {
                for (uint8_t s = 0; s < kMaxNetPlayers; ++s)
                    ClearSlot(s, true);
            }
            g_wasLive = false;
            return;
        }
        g_wasLive = true;

        // Slot reuse (leave / join in progress): none of the old occupant's state carries over.
        for (uint8_t s = 0; s < kMaxNetPlayers; ++s) {
            const bool active = g_sink->PeerActive(s);
            if (g_wasActive[s] && !active)
                OnPeerLeft(s);
            g_wasActive[s] = active;
        }

        if (now >= g_nextHello) {
            g_nextHello = now + kHelloEveryMs;
            SendHello(now);
        }
        for (QueueEntry& q : g_queue) {
            if (!q.len)
                continue;
            const uint64_t due = q.t0 + kRetransmitSchedule[q.step];
            if (now < due)
                continue;
            g_sink->SendWire(q.dest, q.b, q.len);
            if (q.step >= 2 && q.dest < kMaxNetPlayers)
                ++g_tx[q.dest].retx;
            if (++q.step >= static_cast<int>(sizeof kRetransmitSchedule
                                             / sizeof kRetransmitSchedule[0])) {
                if (q.dest < kMaxNetPlayers)
                    ++g_tx[q.dest].never;
                q.len = 0;
            }
        }
        if (now >= g_nextLog) {
            g_nextLog = now + kLogEveryMs;
            LogStats(now);
        }
    }

    void OnPeerLeft(uint8_t slot)
    {
        if (!g_sink || slot >= kMaxNetPlayers)
            return;
        ClearSlot(slot, false);
        g_sink->Log("hitrel: slot left - dedupe ring, capability and queued copies cleared");
    }

    void Reset(bool full)
    {
        for (uint8_t s = 0; s < kMaxNetPlayers; ++s)
            ClearSlot(s, full);
        std::memset(g_queue, 0, sizeof g_queue);
        g_applying = false;
        g_nextHello = g_nextLog = 0;
        g_wasLive = false;
    }

    Stats GetStats(uint8_t slot)
    {
        Stats out;
        if (slot >= kMaxNetPlayers)
            return out;
        out.sent = g_tx[slot].sent;
        out.acked = g_tx[slot].acked;
        out.retransmits = g_tx[slot].retx;
        out.never = g_tx[slot].never;
        out.sentBySeq = g_recv[slot].sentEst;
        out.delivered = g_recv[slot].first;
        out.duplicates = g_recv[slot].dup;
        out.originals = g_recv[slot].orig;
        out.sentReliable = g_sent[slot];
        out.sentPlain = g_sentPlain[slot];
        return out;
    }

} // namespace net::match::hitrel
