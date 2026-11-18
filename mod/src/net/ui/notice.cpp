// ported from tools/ext/notice.c

#include <winsock2.h>
#include <windows.h>

#include "notice.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/locale.hpp"
#include "engine/script.hpp"
#include "net-common.hpp"
#include "net-debug.hpp"
#include "net-account.hpp"
#include "net-matchmaking.hpp"
#include "net-gui2.hpp"
#include "daemon-rpc.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace net::notice {

    namespace {

        // ---- wire (tools/notices.py) ----
        constexpr uint8_t kOpNoticePoll = 0xBA;   // -> [64B token][u8 ver][u8 lang][u8 flags]
        constexpr uint8_t kOpNoticeRep = 0xBB;    // <- "n=<k>\n<id>|<kind>|<param>|<text>\n..."

        constexpr int    kPollMs = 6000;          // base poll cadence
        constexpr int    kPollCapMs = 30000;      // spread the poll out during an outage
        constexpr int    kUpdateCloseS = 60;      // in-match update countdown
        constexpr int    kSeenSlots = 8;          // de-dup ring for shown notices
        constexpr int    kFriendsHold = 6;        // queued kind 'p' lines
        constexpr size_t kRepCap = 1100;          // 0xBB reply buffer
        constexpr size_t kReqCap = 67;            // token + ver + lang + flags

        struct Seen {
            int  id = 0;
            int  band = 0;
            int  ctx = 0;
            bool used = false;
        };

        std::atomic<int> g_have{ 0 };   // 1 = frame, 2 = reachable / no answer, 3 = no connection
        std::atomic<int> g_busy{ 0 };

        char     g_rep[kRepCap] = { 0 };
        int      g_fail = 0;
        bool     g_okOnce = false;
        int      g_off = -1;            // -1 unknown, 0 on, 1 off
        uint64_t g_closeAt = 0;
        int      g_closeLast = -1;
        Seen     g_seen[kSeenSlots];
        int      g_seenNext = 0;
        uint64_t g_nextAsk = 0;
        int      g_streak = 0;
        char     g_tok[65] = { 0 };

        // kind 'p' lines held until the match is over
        char  g_pq[kFriendsHold][300];
        float g_pqs[kFriendsHold] = { 0 };
        int   g_pqn = 0;

        Hooks g_hooks;

        bool Pl() { return engine::locale::IsPolish(); }

        bool InMatch() { return net::gui2::InMatch(); }

        // ---- toast: the ext's ntf_push(NTF_SERVER) / msg_tag fallback ----
        void Toast(const char* text, float secs)
        {
            if (!text || !text[0])
                return;
            HUD::SHOW_NOTIFICATION(text, secs > 0.0f ? secs : 5.0f, 1);
        }

        bool IsKick(const char* t)
        {
            return g_hooks.isKick && g_hooks.isKick(t);
        }

        // ---- text of one notice in the game's language; free text (kind t) and a
        //      reason on a restart pass through ----
        void Render(char kind, int param, const char* text, bool pl, char* out, size_t n)
        {
            int min = (param + 59) / 60;
            if (min < 1)
                min = 1;

            switch (kind) {
            case 'u':
                snprintf(out, n, "%s", pl
                    ? "Dostępna nowa aktualizacja - uruchom grę ponownie po zakończeniu meczu."
                    : "New update available - restart after finishing the match.");
                break;

            case 'v':
                snprintf(out, n, "%s", pl
                    ? "Dostępna nowa aktualizacja - uruchom ponownie launcher, aby ją zainstalować."
                    : "A new update is available - restart the launcher to update.");
                break;

            case 'r':
                if (text && text[0])
                    snprintf(out, n, pl
                        ? "Restart serwera za %d min - %s"
                        : "Server restarting in %d min - %s", min, text);
                else
                    snprintf(out, n, pl
                        ? "Restart serwera za %d min. Dokończ mecz - twój postęp jest zapisany."
                        : "Server restarting in %d min. Finish your match - your progress is saved.", min);
                break;

            case 'p':   // friends + party lines, already in the game's language
                snprintf(out, n, "%s", text ? text : "");
                break;

            case 's':
                snprintf(out, n, "%s", pl
                    ? "Serwer się wyłącza. Trwające mecze mogą się zakończyć."
                    : "Server is shutting down. Matches in progress may end.");
                break;

            default:    // free text from the admin ('SERVER: <text>')
                snprintf(out, n, pl ? "SERWER: %s" : "SERVER: %s", text ? text : "");
                break;
            }
        }

        // ---- in-match update close ----
        bool UpdCloseOff()
        {
            return engine::flag::Present("t2r-no-update-close.txt", 300);
        }

        void UpdCloseToast(int sec)
        {
            char m[300];
            snprintf(m, sizeof m, Pl()
                ? "Wyszła nowa aktualizacja - gra zamknie się za %d s, żeby ją zainstalować. Postęp klanu NIE przepadnie (bez kary za wyjście). Uruchom launcher ponownie."
                : "A new update is out - the game closes in %d s to install it. Your clan progress will NOT be lost (no left-early penalty). Restart through the launcher.",
                sec);
            TO1_LOG("notice: update close countdown %d s\n", sec);
            Toast(m, sec > 8 ? 8.0f : static_cast<float>(sec));
        }

        void UpdCloseTick()
        {
            if (!g_closeAt)
                return;

            const uint64_t now = GetTickCount64();
            const int sec = now >= g_closeAt
                ? 0 : static_cast<int>((g_closeAt - now + 999) / 1000);

            if (sec != g_closeLast && (sec == 30 || sec == 10 || sec == 5))
                UpdCloseToast(sec);
            g_closeLast = sec;

            if (sec > 0)
                return;

            TO1_LOG("notice: update close - closing the game now (build differs from the published update)\n");
            TerminateProcess(GetCurrentProcess(), 0);
        }

        // restart countdown bands: 3 = more than 5 min, 2 = 2-5 min, 1 = 1-2 min,
        // 0 = under 1 min; a notice is shown again when its band drops.
        int Band(int sec)
        {
            return sec > 300 ? 3 : sec > 120 ? 2 : sec > 60 ? 1 : 0;
        }

        // ---- apply one 0xBB reply ----
        void Apply(const char* rep)
        {
            const char* t = rep;
            while (*t && *t != '\n')
                t++;

            const bool ctx = InMatch();
            const bool pl = Pl();

            while (*t) {
                if (*t == '\n')
                    t++;

                char line[400];
                const char* e = strchr(t, '\n');
                size_t ln = e ? static_cast<size_t>(e - t) : strlen(t);
                if (ln >= sizeof line)
                    ln = sizeof line - 1;
                memcpy(line, t, ln);
                line[ln] = 0;
                t = e ? e : t + ln;

                char* p1 = strchr(line, '|');
                char* p2 = p1 ? strchr(p1 + 1, '|') : nullptr;
                char* p3 = p2 ? strchr(p2 + 1, '|') : nullptr;
                if (!p3)
                    continue;
                *p1 = *p2 = *p3 = 0;

                const int id = atoi(line);
                const int param = atoi(p2 + 1);
                const char k = p1[1];
                const char* text = p3 + 1;

                if (k == 'u' && ctx && !g_closeAt && !UpdCloseOff()) {
                    g_closeAt = GetTickCount64() + static_cast<uint64_t>(kUpdateCloseS) * 1000ull;
                    TO1_LOG("notice: an update is out while we play an older build in a match - closing the game in %d s\n",
                        kUpdateCloseS);
                    UpdCloseToast(kUpdateCloseS);
                    continue;
                }

                int slot = -1;
                for (int i = 0; i < kSeenSlots; i++)
                    if (g_seen[i].used && g_seen[i].id == id) {
                        slot = i;
                        break;
                    }

                const int band = k == 'r' ? Band(param) : 0;
                bool show = false;
                if (slot < 0) {
                    slot = g_seenNext++ & (kSeenSlots - 1);
                    g_seen[slot].used = true;
                    g_seen[slot].id = id;
                    g_seen[slot].band = 99;
                    g_seen[slot].ctx = -1;
                    show = true;
                }
                else if (k == 'r') {
                    show = band < g_seen[slot].band;
                }
                else {
                    show = false;   // update / text / shutting down: once per id
                }
                if (!show)
                    continue;

                g_seen[slot].band = band;
                g_seen[slot].ctx = ctx ? 1 : 0;

                char m[300];
                Render(k, param, text, pl, m, sizeof m);
                if (!m[0])
                    continue;

                if (k == 'p' && IsKick(m) && g_hooks.kickSet && g_hooks.kickSet(m)) {
                    TO1_LOG("notice: kick line -> kick notice box\n");
                    continue;
                }
                if (k == 'p' && IsKick(m) && g_hooks.kickShownAt) {
                    const uint64_t at = g_hooks.kickShownAt();
                    if (at && GetTickCount64() - at < 120000ull) {
                        TO1_LOG("notice: kick line already shown through the session note - skipped\n");
                        continue;
                    }
                }
                if (k == 'p') {
                    if (g_hooks.friendsNoteSeen)
                        g_hooks.friendsNoteSeen(m);
                    if (InMatch()) {   // a running match: kept until it is over
                        bool dup = false;
                        for (int q = 0; q < g_pqn; q++)
                            if (!strcmp(g_pq[q], m)) {
                                dup = true;
                                break;
                            }
                        if (!dup) {
                            if (g_pqn >= kFriendsHold) {
                                memmove(g_pq[0], g_pq[1], sizeof g_pq[0] * (kFriendsHold - 1));
                                memmove(&g_pqs[0], &g_pqs[1], sizeof g_pqs[0] * (kFriendsHold - 1));
                                g_pqn = kFriendsHold - 1;
                            }
                            snprintf(g_pq[g_pqn], sizeof g_pq[0], "%s", m);
                            g_pqs[g_pqn] = param >= 2
                                ? static_cast<float>(param > 600 ? 600 : param) : 8.0f;
                            g_pqn++;
                            TO1_LOG("notice: friends line held until the match is over (%d waiting): \"%s\"\n",
                                g_pqn, m);
                        }
                        continue;
                    }
                }

                TO1_LOG("notice: kind %c id %d param %d -> \"%s\"\n", k, id, param, m);

                const float nsecs = (k == 't' || k == 'p') && param >= 2
                    ? static_cast<float>(param > 600 ? 600 : param) : 8.0f;
                if (k == 'p' && g_hooks.friendsToast && g_hooks.friendsToast(m, nsecs))
                    return;   // the friend / party card
                Toast(m, nsecs);
                return;       // one at a time: the next one is shown by the next poll
            }
        }

        // ---- poll worker ----
        DWORD WINAPI Worker(LPVOID arg)
        {
            uint8_t pl[kReqCap];
            memcpy(pl, arg, kReqCap);

            int res = 3;   // 3 = no connection; 2 = connected, no valid answer (older server)
            sockaddr_in a{};
            if (net::daemon::Addr(&a)) {
                uint8_t frame[3 + kReqCap];
                const int n = static_cast<int>(kReqCap) + 1;
                frame[0] = static_cast<uint8_t>(n);
                frame[1] = 0;
                frame[2] = kOpNoticePoll;
                memcpy(frame + 3, pl, kReqCap);

                char rep[kRepCap];
                int sent = 0;
                const int got = net::daemon::Rpc(a, frame, n + 2, rep,
                    static_cast<int>(sizeof rep) - 1, 2500, 2500, 2500, &sent);
                if (sent)
                    res = 2;

                const int ln = got >= 3
                    ? (rep[0] & 0xff) + ((rep[1] & 0xff) << 8) : 0;
                if (ln >= 2 && got >= ln + 2 && static_cast<uint8_t>(rep[2]) == kOpNoticeRep
                    && ln + 2 < static_cast<int>(sizeof g_rep)) {
                    memcpy(g_rep, rep + 3, ln - 1);
                    g_rep[ln - 1] = 0;
                    res = 1;
                    net::daemon::MarkAlive();
                }
            }

            g_have.store(res);
            g_busy.store(0);
            return 0;
        }

        void Ask(uint64_t now)
        {
            // Static, like the ext: the worker copies it on its own thread, and only
            // one worker runs at a time (Ask is gated on g_busy).
            static uint8_t req[kReqCap];
            memcpy(req, g_tok, 64);
            req[64] = 1;
            req[65] = static_cast<uint8_t>(Pl() ? 1 : 0);
            req[66] = (g_hooks.friendsEnabled && g_hooks.friendsEnabled()) ? 1 : 0;

            g_nextAsk = now + kPollMs;
            if (g_streak >= 2)
                g_nextAsk = now + net::daemon::BackoffMs(g_streak - 1, kPollMs, kPollCapMs);

            g_busy.store(1);
            HANDLE h = CreateThread(nullptr, 0, Worker, req, 0, nullptr);
            if (!h) {
                g_busy.store(0);
                return;
            }
            CloseHandle(h);
        }

    } // namespace

    void SetHooks(const Hooks& hooks)
    {
        g_hooks = hooks;
    }

    void Tick()
    {
        if (g_off < 0) {
            FILE* f = engine::exe_side::Open("t2r-no-notice.txt", "r");
            g_off = f != nullptr ? 1 : 0;
            if (f)
                fclose(f);
            TO1_LOG("notice: %s\n", g_off
                ? "off (t2r-no-notice.txt)"
                : "on - server notices polled every 6 s");
        }
        if (g_off)
            return;

        UpdCloseTick();

        if (g_pqn && !InMatch()) {   // the match is over: the held friends / party lines
            for (int q = 0; q < g_pqn; q++) {
                if (g_hooks.friendsToast && g_hooks.friendsToast(g_pq[q], g_pqs[q]))
                    continue;
                Toast(g_pq[q], g_pqs[q]);
            }
            TO1_LOG("notice: %d held friends line(s) shown after the match\n", g_pqn);
            g_pqn = 0;
        }

        const int have = g_have.exchange(0);
        const uint64_t now = GetTickCount64();

        if (have == 1) {
            g_fail = 0;
            g_okOnce = true;
            g_streak = 0;
            Apply(g_rep);
        }
        else if (have == 2 || have == 3) {
            g_streak++;
            // "older server" = reachable, never answers 0xBA. A dead tunnel / restarting
            // daemon looks the same on one socket, so it only counts while the daemon has
            // answered something else lately -- an outage no longer ends notices for the
            // session.
            if (have == 2 && net::daemon::AliveWithin(60000) && ++g_fail >= 3 && !g_okOnce) {
                g_off = 1;
                TO1_LOG("notice: the server does not answer 0xBA (older server) - not asking again\n");
                return;
            }
        }

        if (now < g_nextAsk || g_busy.load())
            return;

        if (!g_tok[0]) {
            const char* tok = net::account::Token();
            if (!tok || strlen(tok) < 64)
                return;
            memcpy(g_tok, tok, 64);
            g_tok[64] = 0;
        }
        Ask(now);
    }

    void Reset()
    {
        g_off = -1;
        g_closeAt = 0;
        g_closeLast = -1;
        g_pqn = 0;
        g_fail = 0;
        g_okOnce = false;
        g_streak = 0;
        g_nextAsk = 0;
        g_seenNext = 0;
        for (auto& s : g_seen)
            s = Seen{};
        g_tok[0] = 0;
        g_have.store(0);
        g_busy.store(0);
    }

}
