// ported from tools/ext/requeue.c (RqOff / RqpWorker / RqpAsk / RqpParse / RequeueTick)

#include "requeue.hpp"
#include "requeue-core.hpp"

#include <winsock2.h>
#include <windows.h>

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "boot.hpp"
#include "net-protocol.hpp"
#include "engine/flag.hpp"
#include "engine/lobby.hpp"
#include "engine/script.hpp"
#include "engine/widget.hpp"
#include "daemon-rpc.hpp"
#include "menu/loadseq.hpp"
#include "net-account.hpp"
#include "net-gui2.hpp"
#include "net-matchmaking.hpp"
#include "net/match/kick-state.hpp"
#include "net/match/match.hpp"
#include "net/match/match-liveness.hpp"
#include "net/social/friends13.hpp"
#include "net/ui/postmatch-screens.hpp"
#include "net/online/steam-status.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace net::requeue {

    namespace {

        // ext RQ_ARM_MS / RQ_SETTLE_MS / RQ_PRE_MS / RQ_FRESH_MS / RQ_POLL_MS.
        // Build 201 checks the ticket while the post-match screens are still up,
        // so the push needs only 600 ms of settled MP menu and no round trip.
        constexpr uint64_t kArmMs    = 300000;   // the daemon's ticket TTL
        constexpr uint64_t kSettleMs = 600;      // MP menu on top before the push
        constexpr uint64_t kPreMs    = 3000;     // early ticket check while screens are up
        constexpr uint64_t kFreshMs  = 4000;     // a yes this young needs no second check
        constexpr uint64_t kPollMs   = 1500;     // waiting-text poll on the search page
        constexpr uint64_t kFlagMs   = 5000;     // ext: sidecar re-checked every 5 s
        constexpr uint64_t kAskWaitMs = 8000;    // ext: give up if the daemon never answers
        constexpr uint64_t kLeftFmMs = 3000;     // ext: grace for the pushed page to appear

        constexpr size_t kTokenLen = 64;
        constexpr size_t kReqLen   = 66;        // token + sub + mode
        constexpr size_t kRepCap   = 160;
        constexpr uint8_t kOpPoll  = static_cast<uint8_t>(NetDaemonOp::kRequeuePoll);
        constexpr uint8_t kOpReply = static_cast<uint8_t>(NetDaemonOp::kRequeueReply);
        constexpr uint8_t kSubPoll = 0;          // mode-count poll (no side effect)
        constexpr uint8_t kSubClose = 3;         // "my game window is closing"

        enum AskResult : int {
            kAskNone = 0,
            kAskOk,
            kAskNoReply,
            kAskNoDaemon,
        };

        struct State {
            uint64_t armed = 0, endAt = 0, menuSince = 0, askedAt = 0, pollAt = 0, chk = 0;
            uint64_t preAt = 0, ansT = 0, pushed = 0;
            int      off = -1, ansRq = -1, ansW = 0, asked = 0, waiting = 0;
            core::Answer   ans;                  // the last 0xBD rq / rqw / rqs / rqb / rqp
            core::Lobby    lobby;                // local countdown + members (ext g_rq_*)
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

        // ext t2r-ext.c: ms_world_busy() = ms_switching() || g_ms_pick_st. The
        // native port has no menu-scene switch / side-pick reload producer, so use
        // the ownerless signals that do exist: the connecting hand-off, the MP
        // front-end loading phase and an in-flight level load / black fade. This
        // deliberately errs on the side of waiting rather than pushing a search
        // that a scene reload would drop.
        bool WorldBusy()
        {
            if (menu::loadseq::HoldActive())
                return true;
            if (net::match::Get().GetPhase() == net::match::Phase::FrontEndLoading)
                return true;
            const uint64_t ready = DC::Call<"task-want-load-levels-fully-spawned?">();
            if (ready != ~uint64_t{ 0 } && ready == 0)
                return true;
            return HUD::GET_BLACK_FADE() > 0.02f;
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
            g_req[kTokenLen] = kSubPoll;          // mode-count poll, no side effect
            g_req[kTokenLen + 1] = 0;

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

        void Parse(const char* t, core::Answer& out)
        {
            // "... rq=<0|1> rqw=<n> rqs=<s> rqb=<n> rqp=<n>" (an old daemon
            // sends only rq / rqw; the build-201 rqX fields then stay 0).
            core::ParseReply(t, out);
        }

        // ext's settext_hook appends the lobby part to "WAITING FOR PLAYERS...":
        // "   b/(b+p)" and, while rqs is ticking, "   m:ss". We write the finished
        // string directly; requeue::Tick runs after the engine's own label write.
        const char* LabelText()
        {
            static char buf[128];
            const int b = g_rq.lobby.back, p = g_rq.lobby.post;
            const int l = g_rq.lobby.left;
            if (b + p > 0) {
                if (l > 0)
                    std::snprintf(buf, sizeof buf, "WAITING FOR PLAYERS...   %d/%d   %d:%02d",
                        b, b + p, l / 60, l % 60);
                else
                    std::snprintf(buf, sizeof buf, "WAITING FOR PLAYERS...   %d/%d", b, b + p);
            }
            else {
                std::snprintf(buf, sizeof buf, "WAITING FOR PLAYERS...");
            }
            return buf;
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
            (void)why;                           // release builds compile TO1_LOG out
            if (g_rq.armed)
                TO1_LOG("requeue: %s\n", why);
            g_rq.armed = 0;
            g_rq.menuSince = 0;
            g_rq.asked = 0;
        }

        // ---- post-match gates (both producers native) -------------------------------
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

        // ---- build-201 window-close notice (ext close_watch_tick / cw_proc) --------
        // One thread-local WH_CALLWNDPROC hook, never a keyboard hook. The sent
        // SC_CLOSE / WM_CLOSE reaches us before the game window's own procedure, so
        // the synchronous 0xBC sub 3 lands before the session socket drops.
        HHOOK        g_cwHook = nullptr;
        std::atomic<int> g_cwSent{ 0 };
        char         g_cwTok[kTokenLen + 1] = { 0 };
        int          g_cwTried = 0;
        uint64_t     g_cwNext = 0;

        void NotifyClose(const char* what)
        {
            (void)what;                          // release builds compile TO1_LOG out
            // ext: in a match, once. A failed notice clears the latch so WM_CLOSE
            // (which follows SC_CLOSE on Alt+F4) gets one more try.
            if (engine::lobby::Substate() != engine::lobby::kSubstateInMatch
                || !g_cwTok[0]) {
                return;
            }
            int expected = 0;
            if (!g_cwSent.compare_exchange_strong(expected, 1))
                return;

            int ok = 0;
            sockaddr_in a{};
            if (net::daemon::Addr(&a)) {
                uint8_t frame[3 + kReqLen];
                frame[0] = static_cast<uint8_t>(kReqLen + 1); // daemon counts the op byte
                frame[1] = 0;
                frame[2] = kOpPoll;
                memcpy(frame + 3, g_cwTok, kTokenLen);
                frame[3 + kTokenLen] = kSubClose;
                frame[3 + kTokenLen + 1] = 0;

                char rep[512];
                int sent = 0;
                const int got = net::daemon::Rpc(a, frame, static_cast<int>(sizeof frame),
                    rep, static_cast<int>(sizeof rep) - 1, 700, 700, 900, &sent);
                ok = sent && got >= 3
                    && static_cast<uint8_t>(rep[2]) == kOpReply;
                if (ok)
                    net::daemon::MarkAlive();
            }
            TO1_LOG("clan: the game window is closing in a match (%s) - the server was told %s "
                    "(the leave counts as your own)\n", what,
                    ok ? "and answered" : "NOT (no answer)");
            if (!ok)
                g_cwSent.store(0);
        }

        LRESULT CALLBACK CloseProc(int code, WPARAM wp, LPARAM lp)
        {
            if (code == HC_ACTION && lp) {
                const CWPSTRUCT* c = reinterpret_cast<const CWPSTRUCT*>(lp);
                if (c->message == WM_SYSCOMMAND
                    && (static_cast<UINT_PTR>(c->wParam) & 0xFFF0u) == SC_CLOSE)
                    NotifyClose("Alt+F4 / close");
                else if (c->message == WM_CLOSE)
                    NotifyClose("WM_CLOSE");
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        BOOL CALLBACK FindWindowThread(HWND h, LPARAM lp)
        {
            DWORD pid = 0;
            const DWORD tid = GetWindowThreadProcessId(h, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(h)
                || GetWindow(h, GW_OWNER)) {
                return TRUE;
            }
            *reinterpret_cast<DWORD*>(lp) = tid;
            return FALSE;
        }

        void CloseWatchTick()
        {
            if (g_cwHook || g_cwTried > 5)
                return;
            const uint64_t now = GetTickCount64();
            if (now < g_cwNext)
                return;
            g_cwNext = now + 2000;
            if (!g_cwTok[0]) {
                const char* tok = net::account::Token();
                if (!tok || strlen(tok) < kTokenLen)
                    return;                      // no account token yet: try again
                memcpy(g_cwTok, tok, kTokenLen);
                g_cwTok[kTokenLen] = 0;
            }
            HWND fg = GetForegroundWindow();
            DWORD pid = 0;
            DWORD tid = fg ? GetWindowThreadProcessId(fg, &pid) : 0;
            if (pid != GetCurrentProcessId()) {
                // A player who alt-tabs at boot is watched too: any visible
                // unowned top-level window of ours identifies the game thread.
                tid = 0;
                EnumWindows(FindWindowThread, reinterpret_cast<LPARAM>(&tid));
            }
            if (!tid)
                return;
            ++g_cwTried;
            g_cwHook = SetWindowsHookExW(WH_CALLWNDPROC, CloseProc, nullptr, tid);
            TO1_LOG("clan: window-close watch on the window thread %lu %s\n",
                static_cast<unsigned long>(tid), g_cwHook ? "installed" : "FAILED");
        }

    }

    void Tick()
    {
        // The legacy ext ticks requeue first and owns its own scene-busy gate.
        // Running both would push the search page twice and duplicate the 0xBC
        // polls / close watch.
        if (boot::ExtLoaded())
            return;

        static StringId64 pMm = 0, pFm = 0;
        if (!pMm) {
            pMm = engine::widget::PageId("main-menu");
            pFm = engine::widget::PageId("find-match-menu");
        }

        const uint64_t now = GetTickCount64();
        CloseWatchTick();                        // one thread-local close hook, once at boot
        const bool off = Off();
        const bool matchEndHold = net::match::liveness::MatchEndHold();  // shared ext `sub == 6` read
        const uint8_t sub = engine::lobby::Substate();  // ext's raw MatchSubstate()

        // 1. arm when the match ended while we were in it
        if (!off && matchEndHold) {
            if (!g_rq.armed) {
                TO1_LOG("requeue: match over - we search again by ourselves once the post-match screens are done (if the server keeps our ticket)\n");
                ++g_rq.gen;                      // an answer from the older match is stale
                g_rq.ans = core::Answer{};
                g_rq.lobby = core::Lobby{};
                g_rq.ansRq = -1;                 // only once per match: keep a later early answer
                g_rq.ansW = 0;
                g_rq.ansT = 0;
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
                Parse(g_rep, g_rq.ans);
                g_rq.ansRq = g_rq.ans.rq;
                g_rq.ansW = g_rq.ans.rq == 1 ? g_rq.ans.w : 0;
                g_rq.ansT = now;                 // rqs counts down from this instant
            }
            else {
                g_rq.ans = core::Answer{};
                g_rq.ansRq = have == kAskNoReply ? -2 : -1;
                g_rq.ansW = 0;
            }
        }

        // Build 201: check the ticket while the post-match screens are still up,
        // so the push after the settle window needs no round trip of its own.
        if (!off && g_rq.armed && !g_rq.pushed && sub != engine::lobby::kSubstateInMatch
            && !g_busy.load() && !g_have.load() && now >= g_rq.preAt && Ask()) {
            g_rq.preAt = now + kPreMs;
        }

        if (g_rq.armed && KickedSinceArm())
            Disarm("kicked from the match - no requeue");
        if (g_rq.armed && now - g_rq.armed > kArmMs)
            Disarm("no MP menu within 300 s of the match end - given up");

        const auto st = net::matchmaking::GetState();
        const uint64_t top = engine::widget::TopPage();

        // 3. WAITING FOR PLAYERS... while the daemon holds our search for the last match.
        // A player who pressed FIND MATCH themselves is covered too: the answer, not
        // our own page push, decides whether the daemon is holding the lobby.
        const bool onFm = !off && top == pFm
            && st == net::matchmaking::State::Searching
            && sub != engine::lobby::kSubstateInMatch
            && ((g_rq.pushed && now - g_rq.pushed < kArmMs)
                || (g_rq.endAt && now - g_rq.endAt < kArmMs));
        if (onFm && !g_busy.load() && now >= g_rq.pollAt) {
            g_rq.pollAt = now + kPollMs;
            Ask();
        }
        const bool waiting = onFm && g_rq.ansRq == 1 && g_rq.ansW > 0;
        const bool wasWaiting = g_rq.waiting != 0;

        // The persistent lobby counters are the ext's g_rq_left/back/post: rqs
        // ticks down locally between polls, rqb/rqp come straight from the reply.
        core::Lobby next;
        if (waiting)
            next = core::LobbyCounts(g_rq.ans, now, g_rq.ansT);
        if (next.left != g_rq.lobby.left || next.back != g_rq.lobby.back
            || next.post != g_rq.lobby.post) {
            const bool membersChanged = next.back != g_rq.lobby.back
                || next.post != g_rq.lobby.post;
            g_rq.lobby = next;
            if (membersChanged && waiting) {
                TO1_LOG("requeue: lobby %d/%d back, next match in %d s at the latest\n",
                    g_rq.lobby.back, g_rq.lobby.back + g_rq.lobby.post, g_rq.lobby.left);
            }
            if (waiting && wasWaiting && top == pFm)
                LabelRefresh(LabelText());
        }

        if (!onFm && g_rq.pushed && top != pFm && now - g_rq.pushed > kLeftFmMs)
            g_rq.pushed = 0;                     // cancelled or matched; the push needs a few frames

        if (waiting != wasWaiting) {
            g_rq.waiting = waiting ? 1 : 0;
            TO1_LOG("requeue: search page reads %s%s\n",
                waiting ? "WAITING FOR PLAYERS..." : "FINDING MATCH...",
                waiting ? "" : " again");
            if (top == pFm)
                LabelRefresh(waiting ? LabelText() : "FINDING MATCH...");
        }
        if (waiting)                             // re-assert after the engine's own label write
            LabelRefresh(LabelText());

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
        if (matchEndHold || top != pMm || !onMenu || !PostMatchScreensDone()
            || WorldBusy()) {
            g_rq.menuSince = 0;
            g_rq.asked = 0;
            return;
        }
        // ext requeue.c: the Steam-only check sits between the page gate and the party
        // gate. FIND MATCH is refused while the helper is down (net-gui2 shows the
        // toast), so do not arm a self-search that the server would drop anyway.
        if (net::steam::SteamOnly() && !net::steam::Connected()) {
            g_rq.menuSince = 0;
            return;
        }
        if (net::friends13::IsMember()) {
            Disarm("party member - the leader's search takes us along (ready state kept)");
            return;
        }
        if (!g_rq.menuSince) {
            g_rq.menuSince = now;
            return;                              // a fresh early rq=1 survives this frame
        }
        if (now - g_rq.menuSince < kSettleMs)
            return;
        // A fresh yes from the early check (or the page poll) is enough: the
        // build-201 push does not wait for another round trip.
        if (!g_rq.asked && !(g_rq.ansRq == 1 && now - g_rq.ansT < kFreshMs)) {
            if (!g_busy.load() && !g_have.load() && Ask()) {
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

    bool TicketHeld()
    {
        // ext chat's exact LOBBY gate: `g_rq_back + g_rq_post > 0`. Those come
        // from the 0xBD rqb / rqp fields and zero as soon as our search is no
        // longer held, so a released / cancelled lobby stops exposing its lines
        // on the same frame the ext's counters would. A requeue opt-out never
        // polls and so never reports a held ticket.
        return !Off() && (g_rq.lobby.back + g_rq.lobby.post > 0);
    }

    void Reset()
    {
        ++g_rq.gen;                              // a reply from the old match is dropped
        g_rq.armed = 0;
        g_rq.endAt = 0;
        g_rq.menuSince = 0;
        g_rq.askedAt = 0;
        g_rq.pollAt = 0;
        g_rq.preAt = 0;
        g_rq.ansT = 0;
        g_rq.pushed = 0;
        g_rq.asked = 0;
        g_rq.ansRq = -1;
        g_rq.ansW = 0;
        g_rq.ans = core::Answer{};
        g_rq.lobby = core::Lobby{};
        g_rq.waiting = 0;
        g_cwSent.store(0);                       // a new match gets its own notice
        g_have.store(0);
        // g_busy is owned by the worker; it clears itself when the request finishes.
    }

}
