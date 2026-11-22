// Native port of the ext's 2013 CLAN server state and the g_pm.pending /
// g_ev.pending producers (tools/ext/t2r-ext.c). Same wire, same byte order, same
// decision table; only the widget page itself stays ext-owned.

#include "clan-state.hpp"

#include "net-protocol.hpp"
#include "daemon-rpc.hpp"
#include "net-account.hpp"
#include "log.hpp"

#include <windows.h>

#include <cstring>

namespace net::clan {

    namespace {

        constexpr uint8_t kOpRead   = static_cast<uint8_t>(NetDaemonOp::kClanStateRead);
        constexpr uint8_t kOpReply  = static_cast<uint8_t>(NetDaemonOp::kClanStateReply);
        constexpr uint8_t kOpReport = static_cast<uint8_t>(NetDaemonOp::kClanReport);
        constexpr size_t  kTokenLen = 64;
        constexpr int     kCap      = 65536;     // ext ClanWorker's reply buffer
        constexpr int     kConnMs   = 2500;      // ext T2rRpc timeouts
        constexpr int     kIoMs     = 3500;
        constexpr int     kReplyMs  = 4000;
        constexpr uint64_t kPollMs       = 3000; // ext's first read: repeat every 3 s until answered
        constexpr uint64_t kPendingPollMs = 5000;
        constexpr uint64_t kIdlePollMs   = 30000;

        struct Post {
            int pending = 0, shown = 0, day = 0, lmh0 = 0;
            uint32_t parts = 0;
            uint64_t sentAt = 0, pollAt = 0;
        };

        struct Runtime {
            State state;
            bool  have = false;
            uint64_t nextGet = 0;
            uint32_t seq = 0;
            int   fails = 0;
            uint64_t hold = 0;               // GetTickCount() stamp of the backoff end
            PmRow rows[kPmRows];
            int   nRows = 0;
            Post  post;
            EventBook ev;
            int    evPushes = 0;
            uint64_t evPushedAt = 0;
            // ext rep_* : a report the server did not acknowledge is re-sent with
            // the same sequence (the daemon dedupes match ids).
            bool      repPend = false;
            uint8_t   repPl[48]{};
            int       repLen = 0;
            uint32_t  repParts = 0;
            State     repBefore;
            uint32_t  acks = 0, repAck0 = 0;
            bool      repWait = false;
            int       repTries = 0;
            uint64_t  repRetryAt = 0;
        } g;

        bool BusyHeld()
        {
            return g.hold && static_cast<int32_t>(GetTickCount() - static_cast<uint32_t>(g.hold)) < 0;
        }

        void Fail(bool ok)
        {
            if (ok) {
                g.fails = 0;
                g.hold = 0;
                net::daemon::MarkAlive();
                return;
            }
            ++g.fails;
            g.hold = GetTickCount() + net::daemon::BackoffMs(g.fails, 1500, 10000);
        }

        // One bounded [u16 len][op][payload] RPC. Returns true only on the expected
        // reply opcode; `reply` receives the text after the op byte.
        bool Ask(uint8_t op, const uint8_t* pl, int plen, uint8_t want,
                 char* reply, int cap)
        {
            sockaddr_in a{};
            if (!net::daemon::Addr(&a) || !net::account::HaveToken())
                return false;
            uint8_t frame[128];
            const size_t tok = std::strlen(net::account::Token());
            if (tok != kTokenLen || plen < 0 || 1 + plen > static_cast<int>(sizeof frame) - 2)
                return false;
            const int n = 1 + plen;
            frame[0] = static_cast<uint8_t>(n);
            frame[1] = static_cast<uint8_t>(n >> 8);
            frame[2] = op;
            std::memcpy(frame + 3, net::account::Token(), kTokenLen);
            if (plen > 0)
                std::memcpy(frame + 3 + kTokenLen, pl, static_cast<size_t>(plen));

            char buf[kCap];
            int sent = 0;
            const int got = net::daemon::Rpc(a, frame, n + 2, buf, cap < kCap ? cap : kCap,
                                             kConnMs, kIoMs, kReplyMs, &sent);
            if (got < 3)
                return false;
            const int ln = static_cast<uint8_t>(buf[0]) + (static_cast<uint8_t>(buf[1]) << 8);
            if (ln < 1 || ln + 2 > got)
                return false;
            if (static_cast<uint8_t>(buf[2]) != want)
                return false;
            const int ntext = ln - 1;
            if (ntext < 0 || ntext >= cap)
                return false;
            std::memcpy(reply, buf + 3, static_cast<size_t>(ntext));
            reply[ntext] = 0;
            return true;
        }

        bool AskState()
        {
            char text[kCap];
            if (!Ask(kOpRead, nullptr, 0, kOpReply, text, sizeof text))
                return false;
            if (!ParseState(text, g.state))
                return false;
            g.have = true;
            TO1_LOG("clan: 0xE0 -> %s\n", text);
            return true;
        }

        bool SendReport()
        {
            char text[kCap];
            if (!Ask(kOpReport, g.repPl, g.repLen, kOpReply, text, sizeof text))
                return false;
            ++g.acks;
            return true;
        }

        void ArmReport()
        {
            PmRow sorted[kPmRows];
            int n = 0;
            for (int i = 0; i < g.nRows; ++i) sorted[n++] = g.rows[i];
            // ext PmReport's stable "most parts first" sort.
            for (int i = 1; i < n; ++i)
                for (int j = i; j > 0 && sorted[j].parts > sorted[j - 1].parts; --j) {
                    const PmRow t = sorted[j];
                    sorted[j] = sorted[j - 1];
                    sorted[j - 1] = t;
                }
            for (int i = 0; i < kPmRows; ++i) g.rows[i] = sorted[i];
            g.nRows = n;

            g.post.pending = 1;
            g.post.day = g.state.day;
            g.post.lmh0 = g.state.lmh;
            g.post.parts = g.repParts;
            g.post.sentAt = GetTickCount64();
            g.post.pollAt = g.post.sentAt + kPollMs;
            // ext PmReport also resets the event result bookkeeping for this day.
            g.ev.outDay = g.state.day;
            g.ev.outDone = 0;
            TO1_LOG("clan: post-match page due (%u parts, %d award rows, day %d)\n",
                    g.post.parts, g.nRows, g.post.day);
        }

    } // namespace

    void Reset()
    {
        g = Runtime{};
    }

    void Tick()
    {
        const uint64_t now = GetTickCount64();
        if (BusyHeld())
            return;

        // The report is the critical op: retry the exact same payload / sequence
        // until the daemon acknowledges (ext rep_wait / rep_pend).
        if (g.repPend) {
            if (SendReport()) {
                g.repPend = false;
                g.repWait = true;
                g.repAck0 = g.acks;
                g.repTries = 0;
                g.repRetryAt = now + 6000;
                ArmReport();
            } else {
                Fail(false);
                return;
            }
        }
        if (g.repWait) {
            if (g.acks != g.repAck0) {
                g.repWait = false;
            } else if (now >= g.repRetryAt) {
                if (g.repTries >= 10) {
                    g.repWait = false;
                    TO1_LOG("clan: match report never acknowledged - gave up after %d re-sends\n", g.repTries);
                } else if (SendReport()) {
                    ++g.repTries;
                    g.repAck0 = g.acks;
                    g.repRetryAt = now + net::daemon::BackoffMs(g.repTries + 1, 6000, 30000);
                }
            }
        }

        // ext's 0xE0 read: 3 s until the first answer, then slowly while a day is
        // pending (the report's stage 1 waits for the server to book the match),
        // and only occasionally once the state is settled.
        const uint64_t interval = !g.have ? kPollMs
                                : (g.post.pending || (g.ev.outDay >= 0 && !g.ev.outDone))
                                  ? kPendingPollMs : kIdlePollMs;
        if (now < g.nextGet)
            return;
        g.nextGet = now + interval;
        if (!AskState())
            Fail(false);
        else
            Fail(true);
    }

    bool HaveState() { return g.have; }
    const State& Get() { return g.state; }

    void Report(uint32_t parts, uint8_t flags,
                const uint8_t* labels, const uint16_t* counts, int n)
    {
        if (n < 0) n = 0;
        if (n > 12) n = 12;                  // ext pl[48]: 10 + 3 * n
        std::memset(g.repPl, 0, sizeof g.repPl);
        ++g.seq;
        std::memcpy(g.repPl, &g.seq, 4);
        std::memcpy(g.repPl + 4, &parts, 4);
        g.repPl[8] = flags;
        g.repPl[9] = static_cast<uint8_t>(n);
        for (int i = 0; i < n; ++i) {
            g.repPl[10 + 3 * i] = labels ? labels[i] : 0;
            const uint16_t c = counts ? counts[i] : 0;
            std::memcpy(g.repPl + 11 + 3 * i, &c, 2);
        }
        g.repLen = 10 + 3 * n;
        g.repParts = parts;
        g.repBefore = g.state;
        if (SendReport()) {
            ++g.acks;
            g.repWait = true;
            g.repAck0 = g.acks;
            g.repTries = 0;
            g.repRetryAt = GetTickCount64() + 6000;
            ArmReport();
        } else {
            g.repPend = true;                // busy / outage: keep the exact payload
            Fail(false);
        }
    }

    void NoteAward(const char* label, int amount)
    {
        NoteAwardRow(g.rows, g.nRows, label, amount);
    }

    void Offer(bool menuTop, bool navBusy, uint64_t now)
    {
        if (!menuTop)
            return;
        if (g.ev.outDay >= 0 && !g.ev.outDone && g.state.ok
            && g.state.lday == g.ev.outDay && !g.state.levk)
            g.ev.outDone = 1;                // nothing happened that day

        const int want = g.ev.pending ? g.ev.mode : EventWanted(g.state, g.ev);
        if (want <= 0)
            return;
        if (g.ev.pending) {
            if (g.ev.shown)
                return;
            if (g.evPushes >= 3 && now - g.evPushedAt > 1000) {
                if (g.ev.mode == 1) g.ev.annDay = g.state.day;
                else                g.ev.outDone = 1;
                g.ev.pending = 0;
                g.evPushes = 0;
                return;
            }
            if (g.evPushes >= 3 || now - g.evPushedAt <= 1500)
                return;
        }
        if (navBusy || g.post.pending)
            return;                          // ext EvOffer's g_loState / g_pm gate
        if (!g.ev.pending) {
            g.ev.pending = 1;
            g.ev.mode = want;
            g.evPushes = 0;
            g.ev.shown = 0;
        }
        // NOTE: the ext pushes to1/clan-event here. The mod has no such widget
        // page, so that call stays with the ext; the latch is the ported part.
        ++g.evPushes;
        g.evPushedAt = now;
    }

    void NotePostShown() { g.post.shown = 1; }
    void NotePostClosed(bool continued)
    {
        g.post.pending = 0;
        g.post.shown = 0;
        if (!continued)
            g.ev.outDone = 1;                // never seen: no event result either
        g.nextGet = GetTickCount64() + 200;  // the day has just landed: re-read soon
    }
    void NoteEventShown() { g.ev.shown = 1; }
    void NoteEventClosed()
    {
        if (g.ev.mode == 2)
            g.ev.outDone = 1;
        g.ev.pending = 0;
        g.ev.shown = 0;
    }

    bool PostPending() { return g.post.pending != 0; }
    bool PostShown()   { return g.post.shown != 0; }
    bool EventPending(){ return g.ev.pending != 0; }
    int  EventMode()   { return g.ev.mode; }
    bool Blocking()    { return g.post.pending != 0 || g.ev.pending != 0; }

} // namespace net::clan
