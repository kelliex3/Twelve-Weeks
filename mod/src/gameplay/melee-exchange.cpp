// ported from tools/ext/melee_exchange.c (the state machine / wire core)

#include "melee-exchange.hpp"

#include <algorithm>
#include <cstring>

namespace gameplay::melee_exchange {

    namespace {

        Sink* g_sink = nullptr;
        Tune  g_tune;

        struct State {
            Role    role = kIdle;
            Beat    beat = kNone;
            Verdict verdict = kNoneVerdict;
            uint8_t slot = 255;
            uint8_t blow = 0;
            uint8_t wcls = 0;
            uint16_t uid = 0;
            unsigned ctr = 0;

            bool acked = false;
            bool contact = false;
            bool missDone = false;
            bool hitGot = false;
            bool burnt = false;
            bool fired = false;
            bool swingSeen = false;
            bool resPending = false;
            bool startPending = false;

            uint64_t tBegin = 0, t0 = 0, winHi = 0, rdue = 0;
            uint64_t hardDl = 0, verdictDue = 0, startAt = 0, resAt = 0;
            uint64_t firedAt = 0, comboUntil = 0, contactMs = 0;
            int      lat = 0, visMs = 0, tcMs = 0;

            uint64_t pairCool[kMaxPeers] = {};
        } g;

        // The QTE tail (bytes 10..12) is optional; old builds read 10 bytes and ignore it.
        uint8_t TailLen() { return 3; }

        void SendStart()
        {
            if (!g_sink)
                return;
            const uint64_t now = g_sink->NowMs();
            uint16_t in = g.t0 > now ? static_cast<uint16_t>(g.t0 - now) : 0;
            if (in > 600)
                in = 600;
            uint8_t b[13] = {};
            uint8_t len = BuildStart(b, g.uid, g.slot, g.blow, g.wcls, in,
                                     static_cast<uint16_t>(g.tcMs));
            b[10] = static_cast<uint8_t>(g_sink->QteFlags());
            const uint16_t a = g_sink->QtePressOffset();
            b[11] = static_cast<uint8_t>(a);
            b[12] = static_cast<uint8_t>(a >> 8);
            g_sink->Send(g.slot, b, len);
        }

        void SendAck(uint8_t dest, uint16_t uid, bool ok, uint8_t reason)
        {
            if (!g_sink)
                return;
            uint8_t b[6] = { 2, static_cast<uint8_t>(uid),
                             static_cast<uint8_t>(uid >> 8),
                             static_cast<uint8_t>(ok), reason,
                             static_cast<uint8_t>(ok ? g_sink->QteCap() : 0) };
            g_sink->Send(dest, b, 6);
        }

        void SendRes()
        {
            if (!g_sink)
                return;
            const uint64_t now = g_sink->NowMs();
            uint16_t in = (g.verdict == kHit && g.rdue > now)
                ? static_cast<uint16_t>(g.rdue - now) : 0;
            if (in > 1500)
                in = 1500;
            uint8_t b[7] = { 3, static_cast<uint8_t>(g.uid),
                             static_cast<uint8_t>(g.uid >> 8),
                             static_cast<uint8_t>(g.verdict), g.blow,
                             static_cast<uint8_t>(in),
                             static_cast<uint8_t>(in >> 8) };
            g_sink->Send(g.slot, b, 7);
        }

        void DoMiss()
        {
            if (!g.missDone) {
                g_sink->PlayMiss();
                g.missDone = true;
            }
        }

        // ext MexAfterHit: replay the held hit, advance to COMBO (or end after blow 3).
        void AfterHit(uint64_t now)
        {
            g_sink->ReplayHit(g.blow);
            g.contact = false;
            if (g.blow < kMaxBlow) {
                g.beat = kCombo;
                g.comboUntil = now + kComboMs;
            } else {
                g_sink->Log("mex: exchange ended after the final blow");
                g_sink->EndQte();
                g.role = kIdle;
                g.beat = kNone;
            }
        }

    } // namespace

    void SetSink(Sink* sink) { g_sink = sink; }
    void SetTune(const Tune& tune) { g_tune = tune; }
    Tune GetTune() { return g_tune; }

    int LatencyBufferMs(float onewayMs)
    {
        int l = static_cast<int>(onewayMs + 40.5f);
        return l < 120 ? 120 : l > 260 ? 260 : l;
    }

    int VisualLagMs(float onewayMs)
    {
        int v = static_cast<int>(onewayMs + 0.5f);
        if (v < 40)
            v = 40;
        if (v > g_tune.maxVis)
            v = g_tune.maxVis;
        return v;
    }

    int ContactMs(int wcls)
    {
        int raw = wcls == 0 ? g_tune.fistTc : wcls == 1 ? g_tune.fistTc - 40
            : wcls == 2 ? g_tune.weaponTc : g_tune.weaponTc + 80;
        return g_sink ? g_sink->TellTc(wcls, static_cast<uint16_t>(raw)) : raw;
    }

    uint8_t BuildStart(uint8_t* out, uint16_t uid, uint8_t target, uint8_t blow,
                       uint8_t wcls, uint16_t t0In, uint16_t tc)
    {
        out[0] = 1;
        out[1] = static_cast<uint8_t>(uid);
        out[2] = static_cast<uint8_t>(uid >> 8);
        out[3] = target;
        out[4] = blow;
        out[5] = wcls;
        out[6] = static_cast<uint8_t>(t0In);
        out[7] = static_cast<uint8_t>(t0In >> 8);
        out[8] = static_cast<uint8_t>(tc);
        out[9] = static_cast<uint8_t>(tc >> 8);
        // The QTE tail (10..12) is filled by SendStart(); 10 bytes for old builds.
        std::memset(out + 10, 0, TailLen());
        return static_cast<uint8_t>(10 + TailLen());
    }

    // ---- local attacker --------------------------------------------------------------------

    bool Engage(uint64_t now, uint8_t target, uint8_t wcls, float onewayMs)
    {
        if (!g_sink || g.role != kIdle || target >= kMaxPeers
            || g_sink->LocalSlot() == target || target == 255)
            return false;
        if (wcls > 3)
            return false;

        uint16_t uid = static_cast<uint16_t>(((g_sink->LocalSlot() & 7u) << 13)
                                             | (++g.ctr & 0x1fffu));
        if (!uid)
            uid = 1;
        g.role = kAttacker;
        g.beat = kEngage;
        g.slot = target;
        g.blow = 1;
        g.uid = uid;
        g.wcls = wcls;
        g.tcMs = ContactMs(wcls);
        g.lat = LatencyBufferMs(onewayMs);
        g.visMs = VisualLagMs(onewayMs);
        g.tBegin = now;
        g.t0 = now;
        g.hardDl = now + kHardDeadlineMs;
        g.acked = false;
        g.fired = false;
        g.verdict = kNoneVerdict;
        g.hitGot = false;
        g.contact = false;
        g.missDone = false;
        g.firedAt = 0;
        g.resPending = false;
        g.startPending = true;
        g.startAt = now + 70;
        g_sink->BeginQte(kAttacker);
        SendStart();
        return true;
    }

    bool Contact(uint64_t now, uint8_t slot, uint8_t blow, bool lethal)
    {
        if (!g_sink || g.role != kAttacker || slot != g.slot || blow != g.blow)
            return false;
        if (g.beat != kAttack || g.contact) {
            if (g.beat == kCounter && g.verdict == kHit)
                return false;
            g_sink->Log("mex: stray contact dropped - free blows do not leak into an exchange");
            return true;
        }
        g.contact = true;
        g.contactMs = now;
        if (g.verdict == kDodged) {
            DoMiss();
            return true;
        }
        if (g.verdict == kHit && now >= g.rdue) {
            AfterHit(now);
            return false;
        }
        // The contact is held (no send / no sound / no marker) until the verdict is due.
        return true;
    }

    void DefVerdict(uint64_t now, Verdict verdict, const char* why)
    {
        if (!g_sink)
            return;
        g.verdict = verdict;
        const uint64_t close = g.winHi > now ? g.winHi : now;
        g.rdue = verdict == kHit ? close : now;
        SendRes();
        g.resPending = true;
        g.resAt = now + 60;             // RES is sent twice (idempotent on the peer)
        if (why)
            g_sink->Log(why);
    }

    void NoteDodge(uint64_t now, bool immune)
    {
        if (g.role != kDefender || g.beat != kAttack || g.verdict != kNoneVerdict)
            return;
        DefVerdict(now, immune ? kDodged : kHit,
                   immune ? "mex: dodge pressed in the window"
                          : "mex: dodged into the blow - no immunity");
    }

    void NoteEarlyPress()
    {
        if (g.role == kDefender && g.beat == kEngage && !g.burnt) {
            g.burnt = true;             // burnt for this beat
            g_sink->Log("mex: dodge pressed before the window opened - burnt for this beat");
        }
    }

    int WindowEnd(uint8_t slot, uint64_t* end)
    {
        if (g.role != kDefender || slot != g.slot)
            return 0;
        if (g.beat == kAttack && g.verdict == kNoneVerdict && !g.burnt
            && g_sink && g_sink->NowMs() <= g.winHi) {
            if (end)
                *end = g.winHi;
            return 1;
        }
        return 2;
    }

    // ---- wire -----------------------------------------------------------------------------

    bool Receive(uint8_t from, const uint8_t* buf, size_t len)
    {
        if (!g_sink || from >= kMaxPeers || from == g_sink->LocalSlot()
            || !buf || len < 4)
            return false;
        const uint64_t now = g_sink->NowMs();
        const uint8_t op = buf[0];
        const uint16_t uid = static_cast<uint16_t>(buf[1] | (buf[2] << 8));

        if (op == 1) {                  // START
            if (len < 10)
                return false;
            const uint8_t target = buf[3], blow = buf[4], wcls = buf[5];
            const uint16_t t0In = static_cast<uint16_t>(buf[6] | (buf[7] << 8));
            const uint16_t tc = static_cast<uint16_t>(buf[8] | (buf[9] << 8));
            if (target != g_sink->LocalSlot() || blow < 1 || blow > kMaxBlow
                || wcls > 3 || t0In > 700 || tc < 150 || tc > 1500)
                return false;
            // A repeat of the same START is ACKed again, never applied twice.
            if (g.role && g.slot == from && g.uid == uid && g.blow == blow) {
                SendAck(from, uid, true, 0);
                return true;
            }
            // The next blow of our combo (we are the defender who just took blow N).
            const bool nextBlow = g.role == kDefender && g.slot == from
                && g.uid == uid && blow == g.blow + 1
                && (g.beat == kCombo || (g.beat == kAttack && g.hitGot));
            if (g.role && !nextBlow) {
                SendAck(from, uid, false, 1);
                return false;           // busy in another exchange / beat
            }
            if (!g.role && g.pairCool[from] > now) {
                SendAck(from, uid, false, 1);
                return false;           // 1.2 s partner cooldown
            }
            if (g_sink->PredBlocked(from)) {
                SendAck(from, uid, false, 2);
                return false;           // busy / down / not an enemy in reach
            }

            const bool cont = g.role == kDefender;
            g.role = kDefender;
            g.slot = from;
            g.uid = uid;
            g.blow = blow;
            g.wcls = wcls;
            g.tcMs = tc;
            g.beat = kEngage;
            g.lat = LatencyBufferMs(g_sink->OnewayMs(from));
            g.visMs = VisualLagMs(g_sink->OnewayMs(from));
            int dly = static_cast<int>(t0In) - static_cast<int>(g_sink->OnewayMs(from));
            if (dly < 20)
                dly = 20;
            g.t0 = now + static_cast<uint64_t>(dly);
            g.winHi = g.t0 + static_cast<uint64_t>(tc + g.visMs);
            if (!cont)
                g.tBegin = now;
            g.hardDl = now + kHardDeadlineMs;
            g.verdict = kNoneVerdict;
            g.burnt = (len >= 11 && (buf[10] & 2)) != 0;
            g.hitGot = false;
            g.acked = true;
            g.contact = false;
            g.missDone = false;
            g.fired = false;
            g.resPending = false;
            if (!cont)
                g_sink->BeginQte(kDefender);
            SendAck(from, uid, true, 0);
            return true;
        }

        if (!g.role || g.slot != from || g.uid != uid)
            return false;
        if (op == 2) {                  // ACK
            if (g.role != kAttacker)
                return false;
            if (buf[3]) {
                g.acked = true;
                g.startPending = false;
                return true;
            }
            // Refused: the pair goes on cooldown and the held press becomes a free swing.
            g.pairCool[from] = now + 1500;
            const bool wasEngage = g.beat == kEngage && !g.fired;
            Reset("mex: refused by the defender - free swing", false, false);
            g.pairCool[from] = now + 1500;
            if (wasEngage)
                g_sink->PlayFreeSwing();
            return true;
        }
        if (op == 3) {                  // RES
            if (g.role != kAttacker || buf[4] != g.blow
                || g.verdict != kNoneVerdict
                || (buf[3] != kHit && buf[3] != kDodged)
                || (g.beat != kEngage && g.beat != kAttack))
                return false;
            const uint16_t rin = static_cast<uint16_t>(buf[5] | (buf[6] << 8));
            if (rin > 1500)
                return false;
            g.acked = true;
            g.verdict = static_cast<Verdict>(buf[3]);
            int dr = static_cast<int>(rin)
                - static_cast<int>(g_sink->OnewayMs(from));
            if (dr < 0)
                dr = 0;
            g.rdue = g.verdict == kHit ? now + static_cast<uint64_t>(dr) : now;
            if (g.verdict == kDodged && g.contact)
                DoMiss();
            return true;
        }
        if (op == 4) {                  // END
            Reset("mex: the partner ended it", false, true);
            return true;
        }
        return false;
    }

    // ---- per-frame -------------------------------------------------------------------------

    void Tick(uint64_t now, bool inputPressed)
    {
        if (!g_sink || g.role == kIdle)
            return;
        const uint8_t slot = g.slot;
        if (now > g.hardDl || g_sink->PredBlocked(slot)) {
            Reset("mex: timeout / partner gone", true, true);
            return;
        }
        if (g.startPending && now >= g.startAt) {
            g.startPending = false;
            if (!g.acked && g.role == kAttacker)
                SendStart();            // retry; the START is idempotent on the peer
        }
        if (g.resPending && now >= g.resAt) {
            g.resPending = false;
            SendRes();
        }

        if (g.role == kAttacker) {
            if (g.beat == kEngage && now >= g.t0) {
                g_sink->FireSwing();
                g.fired = true;
                g.firedAt = now;
                g.swingSeen = false;
                g.contact = false;
                g.missDone = false;
                g.verdict = kNoneVerdict;
                g.beat = kAttack;
                g.verdictDue = g.t0 + static_cast<uint64_t>(g.tcMs + g.visMs
                    + g.lat + g_sink->OnewayMs(slot) + 220);
            } else if (g.beat == kCombo && inputPressed) {
                // The next punch of the combo (blow+1) becomes a new ENGAGE.
                uint8_t next = g.blow + 1;
                Engage(now, slot, g.wcls, g_sink->OnewayMs(slot));
                if (g.role == kAttacker)
                    g.blow = next;
            } else if (g.beat == kAttack && g.contact && g.rdue && now >= g.rdue
                       && g.verdict == kHit && !g.hitGot) {
                g.hitGot = true;
                AfterHit(now);
            } else if (g.beat == kAttack && g.firedAt && now > g.firedAt + 500
                       && !g.swingSeen && !g.contact) {
                Reset("mex: the press was not taken by the engine", true, true);
            }
        } else if (g.role == kDefender) {
            if (g.beat == kEngage && now >= g.t0) {
                g.beat = kAttack;       // the dodge window is now open (WindowEnd feeds dodge)
            } else if (g.beat == kAttack && now > g.winHi
                       && g.verdict == kNoneVerdict) {
                DefVerdict(now, kHit, "mex: window lapsed - HIT (safe default)");
            }
        }
    }

    void Reset(const char* why, bool notify, bool cool)
    {
        const uint8_t slot = g.slot;
        const unsigned ctr = g.ctr;     // the UID counter survives a reset (ext g_mex.ctr)
        uint64_t pairCool[kMaxPeers];
        std::memcpy(pairCool, g.pairCool, sizeof pairCool);
        if (g.role != kIdle && g_sink) {
            g_sink->EndQte();
            if (why)
                g_sink->Log(why);
        }
        g = State{};
        g.ctr = ctr;
        std::memcpy(g.pairCool, pairCool, sizeof pairCool);
        if (cool && slot < kMaxPeers && g_sink)
            g.pairCool[slot] = g_sink->NowMs() + kCoolMs;
        (void)notify;                   // END notification is sent by the caller if needed
    }

    void OnLeftMatch() { Reset("mex: left the match", false, false); }

    Role CurrentRole() noexcept { return g.role; }
    Beat CurrentBeat() noexcept { return g.beat; }
    Verdict CurrentVerdict() noexcept { return g.verdict; }

} // namespace gameplay::melee_exchange
