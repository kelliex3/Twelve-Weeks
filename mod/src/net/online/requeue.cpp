// ported from tools/ext/requeue.c (RqOff / RqpWorker / RqpAsk / RqpParse / RequeueTick)

#include "requeue.hpp"

#include <winsock2.h>
#include <windows.h>

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "net-protocol.hpp"
#include "engine/flag.hpp"
#include "engine/script.hpp"
#include "engine/widget.hpp"
#include "daemon-rpc.hpp"
#include "net-account.hpp"
#include "net-gui2.hpp"
#include "net-matchmaking.hpp"
#include "net/match/kick-state.hpp"
#include "net/match/match-liveness.hpp"
#include "net/social/friends13.hpp"
#include "net/ui/postmatch-screens.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace net::requeue {

    namespace {

        // ext RQ_ARM_MS / RQ_SETTLE_MS / RQ_POLL_MS
        constexpr uint64_t kArmMs    = 300000;   // the daemon's ticket TTL
        constexpr uint64_t kSettleMs = 1500;     // MP menu on top before the push
        constexpr uint64_t kPollMs   = 1500;     // waiting-text poll on the search page
        constexpr uint64_t kFlagMs   = 5000;     // ext: sidecar re-checked every 5 s
        constexpr uint64_t kAskWaitMs = 8000;    // ext: give up if the daemon never answers
        constexpr uint64_t kLeftFmMs = 3000;     // ext: grace for the pushed page to appear

        constexpr size_t kTokenLen = 64;
        constexpr size_t kReqLen   = 66;        // token + sub 0 + mode 0
        constexpr size_t kRepCap   = 160;
        constexpr uint8_t kOpPoll  = static_cast<uint8_t>(NetDaemonOp::kRequeuePoll);
        constexpr uint8_t kOpReply = static_cast<uint8_t>(NetDaemonOp::kRequeueReply);

        enum AskResult : int {
            kAskNone = 0,
            kAskOk,
            kAskNoReply,
            kAskNoDaemon,
        };

        struct State {
            uint64_t armed = 0, endAt = 0, menuSince = 0, askedAt = 0, pollAt = 0, chk = 0;
            uint64_t pushed = 0;
            int      off = -1, ansRq = -1, ansW = 0, asked = 0, waiting = 0;
            uint32_t gen = 0;                    // drops an answer from a previous match
        } g_rq;

        std::atomic<int>      g_busy{ 0 };
        std::atomic<int>      g_have{ 0 };
        std::atomic<uint32_t> g_repGen{ 0 };
        uint8_t               g_req[kReqLen]{};
        char                  g_rep[kRepCap]{};
        char                  g_tok[kTokenLen + 1] = { 0 };

        bool Off()
        {
            const uint64_t now = GetTickCount64();
            if (g_rq.off < 0 || now - g_rq.chk > kFlagMs) {
                g_rq.chk = now;
                const int o = engine::flag::Present("t2r-no-requeue.txt",
                    static_cast<int>(kFlagMs * 60 / 1000)) ? 1 : 0;
                if (o != g_rq.off) {
                    g_rq.off = o;
                    TO1_LOG("requeue: %s\n",
                        o ? "off (t2r-no-requeue.txt)"
                          : "on (the queue stays after a match)");
                }
            }
            return g_rq.off != 0;
        }

        // ---- async 0xBC poll (ext RqpWorker / RqpAsk) -------------------------------
        DWORD WINAPI Worker(LPVOID arg)
        {
            const uint32_t gen = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
            uint8_t req[kReqLen];
            memcpy(req, g_req, sizeof req);      // the next Ask may overwrite the shared buffer

            int res = kAskNoDaemon;
            sockaddr_in a{};
            if (net::daemon::Addr(&a)) {
                uint8_t frame[3 + kReqLen];
                const int n = static_cast<int>(kReqLen) + 1;
                frame[0] = static_cast<uint8_t>(n);
                frame[1] = 0;
                frame[2] = kOpPoll;
                memcpy(frame + 3, req, kReqLen);

                char rep[kRepCap + 4];
                int sent = 0;
                const int got = net::daemon::Rpc(a, frame, n + 2, rep,
                    static_cast<int>(sizeof rep) - 1, 2000, 2000, 2000, &sent);
                if (sent)
                    res = kAskNoReply;

                const int ln = got >= 3
                    ? (rep[0] & 0xff) + ((rep[1] & 0xff) << 8) : 0;
                if (ln >= 1 && got >= ln + 2 && static_cast<uint8_t>(rep[2]) == kOpReply
                    && static_cast<size_t>(ln - 1) < kRepCap) {
                    memcpy(g_rep, rep + 3, static_cast<size_t>(ln - 1));
                    g_rep[ln - 1] = 0;
                    res = kAskOk;
                    net::daemon::MarkAlive();
                }
            }

            g_repGen.store(gen);
            g_have.store(res);
            g_busy.store(0);
            return 0;
        }

        bool Ask()
        {
            if (!g_tok[0]) {
                const char* tok = net::account::Token();
                if (!tok || strlen(tok) < kTokenLen)
                    return false;                // no account token yet: try next tick
                memcpy(g_tok, tok, kTokenLen);
                g_tok[kTokenLen] = 0;
            }
            if (g_have.load() || g_busy.load())
                return false;

            memcpy(g_req, g_tok, kTokenLen);
            g_req[kTokenLen] = 0;
            g_req[kTokenLen + 1] = 0;             // sub 0 = mode-count poll, no side effect

            g_busy.store(1);
            HANDLE h = CreateThread(nullptr, 0, Worker,
                reinterpret_cast<LPVOID>(static_cast<uintptr_t>(g_rq.gen)), 0, nullptr);
            if (!h) {
                g_busy.store(0);
                return false;
            }
            CloseHandle(h);
            return true;
        }

        void Parse(const char* t, int* rq, int* w)
        {
            // "... rq=<0|1> rqw=<n>" (an old daemon sends neither: no ticket)
            char k[16];
            int v = 0, n = 0;
            *rq = 0;
            *w = 0;
            while (sscanf(t, " %15[^= ]=%d%n", k, &v, &n) == 2) {
                if (!strcmp(k, "rq"))
                    *rq = v;
                else if (!strcmp(k, "rqw"))
                    *w = v;
                t += n;
            }
        }

        void LabelRefresh(const char* text)
        {
            // ext: UiW("anim-group.statusLabel") -> kFnSetText. The mod writes the label after
            // the engine's own text set (boot::FrameTick runs after UpdateMenu), so a re-assert
            // every frame has the same visible effect as the ext's settext_hook swap. The
            // handle is cached per page like the ext's UiW cache.
            static uint64_t w = 0, page = 0;
            const uint64_t tp = engine::widget::TopPage();
            if (tp != page) {
                page = tp;
                w = engine::widget::Page("anim-group.statusLabel");
            }
            engine::widget::SetText(w, text);
        }

        void Disarm(const char* why)
        {
            if (g_rq.armed)
                TO1_LOG("requeue: %s\n", why);
            g_rq.armed = 0;
            g_rq.menuSince = 0;
            g_rq.asked = 0;
        }

        // ---- honest gates for producers still owned by the ext ---------------------
        bool KickedSinceArm()
        {
            // ext `g_kickMs > g_rq.armed`: the kick text (admin / AFK / ping removal,
            // or a team move) arrived after we armed on the match end. The stamp is
            // now native -- net::match::kick is fed by the notice kind-'p' line, the
            // mod's stand-in for the ext's op 0x11 session note. The mod still tears
            // down its relay session on every end, so Role() alone cannot tell a kick
            // from a normal finish; this text is the signal.
            return net::match::kick::Since(g_rq.armed);
        }

        bool PostMatchScreensDone()
        {
            // ext requeue.c: `!g_pm.pending && !Cl13PostActive() && !g_ev.pending
            // && g_loState == LO_NONE && !g_mmBack`. All five latches are now native
            // (net/ui/postmatch-screens.*), sampled from the menu stack once per frame.
            return net::ui::postmatch::ScreensDone();
        }

    }

    void Tick()
    {
        static StringId64 pMm = 0, pFm = 0;
        if (!pMm) {
            pMm = engine::widget::PageId("main-menu");
            pFm = engine::widget::PageId("find-match-menu");
        }

        const uint64_t now = GetTickCount64();
        const bool off = Off();
        const bool matchEndHold = net::match::liveness::MatchEndHold();  // shared ext `sub == 6` read

        // 1. arm when the match ended while we were in it
        if (!off && matchEndHold) {
            if (!g_rq.armed) {
                TO1_LOG("requeue: match over - we search again by ourselves once the post-match screens are done (if the server keeps our ticket)\n");
                ++g_rq.gen;                      // an answer from the older match is stale
            }
            g_rq.armed = g_rq.endAt = now;
            g_rq.pushed = 0;
            g_rq.asked = 0;
            g_rq.menuSince = 0;
        }

        // consume a finished 0xBC poll
        const int have = g_have.exchange(0);
        if (have) {
            if (have == kAskOk && g_repGen.load() == g_rq.gen) {
                int rq = 0, w = 0;
                Parse(g_rep, &rq, &w);
                g_rq.ansRq = rq;
                g_rq.ansW = rq == 1 ? w : 0;
            }
            else {
                g_rq.ansRq = have == kAskNoReply ? -2 : -1;
                g_rq.ansW = 0;
            }
        }

        if (g_rq.armed && KickedSinceArm())
            Disarm("kicked from the match - no requeue");
        if (g_rq.armed && now - g_rq.armed > kArmMs)
            Disarm("no MP menu within 300 s of the match end - given up");

        const auto st = net::matchmaking::GetState();
        const uint64_t top = engine::widget::TopPage();

        // 3. WAITING FOR PLAYERS... while the daemon holds our search for the last match
        const bool onFm = !off && top == pFm
            && st == net::matchmaking::State::Searching
            && g_rq.pushed && now - g_rq.pushed < kArmMs;
        if (onFm && !g_busy.load() && now >= g_rq.pollAt) {
            g_rq.pollAt = now + kPollMs;
            Ask();
        }
        const bool waiting = onFm && g_rq.ansRq == 1 && g_rq.ansW > 0;
        if (!onFm && g_rq.pushed && top != pFm && now - g_rq.pushed > kLeftFmMs)
            g_rq.pushed = 0;                     // cancelled or matched; the push needs a few frames

        if (waiting != (g_rq.waiting != 0)) {
            g_rq.waiting = waiting ? 1 : 0;
            TO1_LOG("requeue: search page reads %s%s\n",
                waiting ? "WAITING FOR PLAYERS..." : "FINDING MATCH...",
                waiting ? "" : " again");
            if (top == pFm)
                LabelRefresh(waiting ? "WAITING FOR PLAYERS..." : "FINDING MATCH...");
        }
        if (waiting)                             // re-assert after the engine's own label write
            LabelRefresh("WAITING FOR PLAYERS...");

        // 2. back on the MP menu, every post-match screen done: search again
        if (!g_rq.armed || g_rq.pushed)
            return;
        if (off) {
            Disarm("off");
            return;
        }
        // After a match the matchmaker stays in Matched until the next queue / cancel; that is
        // the mod's "on the MP menu, still known to the daemon" state (ext session state 2).
        const bool onMenu = st == net::matchmaking::State::Connected
            || st == net::matchmaking::State::Matched;
        if (matchEndHold || top != pMm || !onMenu || !PostMatchScreensDone()) {
            g_rq.menuSince = 0;
            g_rq.asked = 0;
            return;
        }
        if (net::friends13::IsMember()) {
            Disarm("party member - the leader's search takes us along (ready state kept)");
            return;
        }
        if (!g_rq.menuSince) {
            g_rq.menuSince = now;
            g_rq.ansRq = -1;
            return;
        }
        if (now - g_rq.menuSince < kSettleMs)
            return;
        if (!g_rq.asked) {
            if (!g_busy.load() && Ask()) {
                g_rq.asked = 1;
                g_rq.askedAt = now;
                g_rq.ansRq = -2;
            }
            return;
        }
        if (g_rq.ansRq == -2) {
            if (now - g_rq.askedAt > kAskWaitMs)
                Disarm("the server did not answer the ticket check - press FIND MATCH");
            return;
        }
        if (g_rq.ansRq != 1) {
            Disarm(g_rq.ansRq == 0
                ? "the server has no ticket for us (cancelled / timed out / older server) - nothing pushed"
                : "no answer from the server - nothing pushed");
            return;
        }

        g_rq.armed = 0;
        g_rq.pushed = now;
        g_rq.pollAt = now + 500;
        // This is our own search push: let the friends13 search gate know so a party
        // member's follow-the-leader request is not blocked. The on-stack check also
        // keeps the ext's own push during the bridge from stacking a second page.
        net::friends13::NoteAutoPush();
        if (!MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, pFm))
            MENU2::PUSH_PAGE(MenuStack::hudMenuStack, "to1/find-match-menu");
        TO1_LOG("requeue: post-match screens done, %.1f s after the match end - search page pushed (same mode; the lobby waits for the others of the last match)\n",
            static_cast<double>(now - g_rq.endAt) / 1000.0);
    }

    void Reset()
    {
        ++g_rq.gen;                              // a reply from the old match is dropped
        g_rq.armed = 0;
        g_rq.endAt = 0;
        g_rq.menuSince = 0;
        g_rq.askedAt = 0;
        g_rq.pollAt = 0;
        g_rq.pushed = 0;
        g_rq.asked = 0;
        g_rq.ansRq = -1;
        g_rq.ansW = 0;
        g_rq.waiting = 0;
        g_have.store(0);
        // g_busy is owned by the worker; it clears itself when the request finishes.
    }

}
