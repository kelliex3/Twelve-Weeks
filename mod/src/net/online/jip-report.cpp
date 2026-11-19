// ported from tools/ext/t2r-ext.c (FeatureJoinInProgress, JipAsk, JipWorker; the
// g_srvTestFlags read in the 0xE5 reply section). Wire / clock / server settings only.

#include "jip-report.hpp"

#include <winsock2.h>
#include <windows.h>

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/flag.hpp"
#include "engine/script.hpp"
#include "daemon-rpc.hpp"
#include "net-account.hpp"
#include "net-gui2.hpp"
#include "net-session.hpp"
#include "net-timer.hpp"
#include "afk.hpp"
#include "match.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace net::jip {

    namespace {

        // ---- wire (tools/daemon.py) ------------------------------------------------
        constexpr uint8_t  kOpClock = 0xE3;
        constexpr uint8_t  kOpReply = 0xE5;
        constexpr size_t   kReqCap  = 80;                            // token 64 + header 9 + afk 4
        constexpr size_t   kRepPayload = 2 + 8 * 0x32 + 1 + 2 + 8 * 2; // roster + wave + swap block
        constexpr size_t   kRepCap  = 3 + kRepPayload;                // [u16 len][op][payload]

        constexpr uint8_t  kClockValid = 0x01;   // rem / dur fields are usable
        constexpr uint8_t  kMatchOver  = 0x02;   // end banner / fade hold
        // 0x10 (we apply the server's team-swap block) and 0x20 (we requeue ourselves)
        // are deliberately NOT sent: those features are unported. See the header.

        constexpr uint64_t kHostMs   = 2000;     // ext: hosts poll every 2 s
        constexpr uint64_t kClientMs = 3000;     // ext: clients every 3 s
        constexpr uint64_t kRetryMs  = 20000;    // ext: after 3 unanswered asks, retry slowly

        std::atomic<int>      g_busy{ 0 };       // one worker at a time
        std::atomic<int>      g_have{ 0 };       // 1 = valid reply, 2 = reachable / no answer, 3 = no connection
        std::atomic<uint32_t> g_repGen{ 0 };     // match generation the published reply belongs to
        uint32_t              g_gen = 0;         // current match generation (bumped on (re)entry)
        uint8_t               g_req[kReqCap]{};  // static like the ext: the worker copies it
        uint8_t               g_rep[kRepCap]{};  // published reply (g_have publishes it to the tick)
        int                   g_repN = 0;
        char                  g_tok[65] = { 0 };
        uint64_t              g_nextAsk = 0;
        int                   g_fails = 0;
        int                   g_off = 0;         // server answered enabled = 0: stop for this match
        int                   g_inMatch = 0;
        uint8_t               g_lastTestFlags = 0xFF;

        bool Off()
        {
            // ext: g_noJip, read at startup. engine::flag keeps the same file cached,
            // re-read every ~5 s so a toggle change during a session is honoured.
            return !engine::flag::Absent("t2r-no-jip.txt");
        }

        // ---- async request (ext JipWorker) ----------------------------------------
        DWORD WINAPI Worker(LPVOID arg)
        {
            const uint32_t gen = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
            uint8_t req[kReqCap];
            memcpy(req, g_req, sizeof req);      // own copy: the next Ask may overwrite g_req

            int res = 3;                         // no daemon connection
            sockaddr_in a{};
            if (net::daemon::Addr(&a)) {
                uint8_t frame[3 + kReqCap];
                const int n = static_cast<int>(kReqCap) + 1;
                frame[0] = static_cast<uint8_t>(n);
                frame[1] = 0;
                frame[2] = kOpClock;
                memcpy(frame + 3, req, kReqCap);

                char rep[kRepCap];
                int sent = 0;
                const int got = net::daemon::Rpc(a, frame, n + 2, rep,
                    static_cast<int>(sizeof rep), 3000, 1500, 1500, &sent);
                if (sent)
                    res = 2;                     // reachable, but no usable answer yet

                const int want = got >= 3
                    ? 2 + (rep[0] & 0xff) + ((rep[1] & 0xff) << 8) : 0;
                if (got >= 5 && got >= want && static_cast<uint8_t>(rep[2]) == kOpReply
                    && want >= 5 && want <= static_cast<int>(sizeof rep)) {
                    memcpy(g_rep, rep + 3, static_cast<size_t>(want - 3));
                    g_repN = want - 3;
                    res = 1;
                    net::daemon::MarkAlive();
                }
            }

            // ext publishes the generation for every finished request, successful or
            // not: a failure from the previous match must not be mistaken for this
            // match's answer when the tick consumes g_have.
            g_repGen.store(gen);
            g_have.store(res);
            g_busy.store(0);
            return 0;
        }

        void Ask(uint64_t now, uint8_t flags, float rem, float dur,
                 int myTeam, int ally, int enemy)
        {
            if (!g_tok[0]) {
                const char* tok = net::account::Token();
                if (!tok || strlen(tok) < 64)
                    return;                      // no account token yet: try next tick
                memcpy(g_tok, tok, 64);
                g_tok[64] = 0;
            }

            memcpy(g_req, g_tok, 64);
            g_req[64] = flags;
            memcpy(g_req + 65, &rem, 4);
            memcpy(g_req + 69, &dur, 4);
            g_req[73] = myTeam >= 0 && myTeam < 250 ? static_cast<uint8_t>(myTeam) : 0xFF;
            g_req[74] = ally   >= 0 && ally   < 250 ? static_cast<uint8_t>(ally)   : 0xFF;
            g_req[75] = enemy  >= 0 && enemy  < 250 ? static_cast<uint8_t>(enemy)  : 0xFF;
            net::afk::ReqTail(g_req + 76);       // [0xAF][u16 idle s][flags]

            const uint64_t cadence = net::session::IsHost() ? kHostMs : kClientMs;
            g_nextAsk = now + cadence;
            if (g_fails >= 3)
                g_nextAsk = now + kRetryMs;

            g_busy.store(1);
            HANDLE h = CreateThread(nullptr, 0, Worker,
                reinterpret_cast<LPVOID>(static_cast<uintptr_t>(g_gen)), 0, nullptr);
            if (!h) {
                g_busy.store(0);
                return;
            }
            CloseHandle(h);
        }

        // ---- reply (ext FeatureJoinInProgress's reply section) ----------------------
        void ApplyReply()
        {
            if (g_repN < 2)
                return;

            // Newer servers append one byte after the roster: 0x80 present, bit0 wave
            // respawn, bits 1-4 the test-server flags. Older servers send no such byte;
            // 0 then means "default" exactly as in the ext.
            const int n = g_rep[1] > 8 ? 8 : static_cast<int>(g_rep[1]);
            const int at = 2 + n * 0x32;
            const uint8_t tf = g_repN > at && (g_rep[at] & 0x80)
                ? static_cast<uint8_t>(g_rep[at] & 0x78) : 0;
            if (tf != g_lastTestFlags) {
                TO1_LOG("test server: flags %02x (free camera %s, Supply Raid clock %s, lives %s, AFK removal %s)\n",
                    tf, tf & 0x08 ? "allowed" : "off", tf & 0x10 ? "x2" : "normal",
                    tf & 0x20 ? "x2" : "normal", tf & 0x40 ? "OFF" : "on");
                g_lastTestFlags = tf;
                net::daemon::SetTestFlags(tf);
            }

            if (!g_rep[0]) {
                g_off = 1;
                TO1_LOG("jip: server has join in progress off - not asking again this match\n");
            }
            // TODO(jip): the 0xE5 roster merge into the lobby object, the trailing
            // [0xA7][k][slot, team] in-match swap block and ghost peer reaping are
            // separate unported features. The ext still owns that engine state while
            // it is the bridge; only the server settings are consumed here.
        }

    }

    void Tick()
    {
        if (Off())
            return;

        const uint64_t now = GetTickCount64();
        const bool live = net::session::Role() != NetRole::None && net::gui2::InMatch();
        if (!live) {
            if (g_inMatch)
                TO1_LOG("jip: left the match - clock report stopped\n");
            g_inMatch = 0;
            g_off = 0;
            g_nextAsk = 0;
            g_fails = 0;
            g_have.store(0);
            return;
        }
        if (!g_inMatch) {
            g_inMatch = 1;
            ++g_gen;
            g_off = 0;
            g_nextAsk = 0;
            g_fails = 0;
            g_have.store(0);
            TO1_LOG("jip: clock report on (%u s, AFK trailer via afk.c)\n",
                static_cast<unsigned>(net::session::IsHost() ? kHostMs / 1000 : kClientMs / 1000));
        }

        const int have = g_have.exchange(0);
        if (have == 1 && g_repGen.load() == g_gen) {
            g_fails = 0;
            ApplyReply();
        } else if (have == 1) {
            g_fails++;                            // reply from the previous match
        } else if (have == 2 || have == 3) {
            // ext: only an "enabled 0" answer stops the reports. A dead tunnel is
            // retried, slowly after three misses, so the daemon can come back.
            if (++g_fails == 3)
                TO1_LOG("jip: server did not answer 3 times - retrying every 20 s\n");
        }

        if (g_off || now < g_nextAsk || g_busy.load())
            return;

        const NetTimerSync t = net::gui2::MatchSnapshot();
        uint8_t flags = 0;
        if (t.duration > 0.0f && t.duration < 36000.0f
            && t.remaining >= 0.0f && t.remaining <= t.duration + 1.0f)
            flags |= kClockValid;
        if (net::gui2::ScoreboardShowing() || net::gui2::MatchExpired())
            flags |= kMatchOver;

        const int myTeam = net::match::Get().TeamOf(net::session::MySlot());
        Ask(now, flags, t.remaining, t.duration,
            myTeam, net::gui2::GetBlueLives(), net::gui2::GetRedLives());
    }

    void Reset()
    {
        ++g_gen;                                  // a reply from the old match is dropped
        g_inMatch = 0;
        g_off = 0;
        g_fails = 0;
        g_nextAsk = 0;
        g_repN = 0;
        g_have.store(0);
        g_repGen.store(0);
        // g_busy is owned by the worker; it clears itself when the request finishes.
    }

}
