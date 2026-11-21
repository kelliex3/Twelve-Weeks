// Native port of tools/ext/rankbadge.c (the async 0xBF poll + rkb_draw / RankUi).
// The pure table helpers live in net/social/friends-state.*; see rank-badge.hpp.

#include "rank-badge.hpp"

#include "friends-state.hpp"

#include <winsock2.h>
#include <windows.h>

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp
#include "net-protocol.hpp"
#include "engine/widget.hpp"
#include "daemon-rpc.hpp"
#include "net-account.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace net::rank_badge {

    namespace {

        constexpr size_t kTokenLen = 64;
        constexpr uint8_t kOpReq   = static_cast<uint8_t>(NetDaemonOp::kRankTable);
        constexpr uint8_t kOpRep   = static_cast<uint8_t>(NetDaemonOp::kRankTable);
        // 300 entries * ~20 bytes (ext rankbadge.py LIMIT) + the "n=<k>" header.
        constexpr size_t kRepCap   = 16384;

        enum AskResult : int {
            kAskNone = 0,
            kAskOk,
            kAskNoReply,
            kAskNoDaemon,
        };

        std::atomic<int> g_busy{ 0 };
        std::atomic<int> g_reply{ 0 };   // AskResult of the last finished poll
        char             g_rep[kRepCap]{};
        std::atomic<int> g_repLen{ 0 };
        char             g_tok[kTokenLen + 1] = { 0 };

        social::RankTable g_table;       // parsed / read on the main thread only

        uint64_t g_wantAt = 0;           // ext g_rkbWantAt (last DrawBadge lookup)
        uint64_t g_next = 0;             // next allowed ask
        uint64_t g_deadAt = 0;           // when the "older daemon" counter started
        bool     g_miss = false;         // ext g_rkbMiss (last lookup found nothing)
        int      g_dead = 0;             // ext g_rkbDead (unanswered 0xBF tries)

        // ---- async 0xBF poll (ext ClanSend 0xBF / clan_worker / RkbParse) -----------
        DWORD WINAPI Worker(LPVOID)
        {
            uint8_t req[kTokenLen];
            std::memcpy(req, g_tok, kTokenLen);   // a later Ask may overwrite the token

            int res = kAskNoDaemon;
            sockaddr_in a{};
            if (net::daemon::Addr(&a)) {
                // Frame: [u16 len][u8 0xBF][64B account token], no extra payload
                // (ext ClanSend(0xBF, NULL, 0); len counts the op byte + token).
                uint8_t frame[3 + kTokenLen];
                const int n = static_cast<int>(kTokenLen) + 1;
                frame[0] = static_cast<uint8_t>(n);
                frame[1] = static_cast<uint8_t>(n >> 8);
                frame[2] = kOpReq;
                std::memcpy(frame + 3, req, kTokenLen);

                char rep[kRepCap + 4];
                int sent = 0;
                const int got = net::daemon::Rpc(a, frame, n + 2, rep,
                    static_cast<int>(sizeof rep) - 1, 3000, 3500, 4000, &sent);
                if (sent)
                    res = kAskNoReply;

                const int ln = got >= 3
                    ? (rep[0] & 0xff) + ((rep[1] & 0xff) << 8) : 0;
                if (ln >= 1 && got >= ln + 2
                    && static_cast<uint8_t>(rep[2]) == kOpRep
                    && static_cast<size_t>(ln - 1) < kRepCap) {
                    std::memcpy(g_rep, rep + 3, static_cast<size_t>(ln - 1));
                    g_rep[ln - 1] = 0;
                    g_repLen.store(ln - 1);
                    res = kAskOk;
                    net::daemon::MarkAlive();
                }
            }

            g_reply.store(res);
            g_busy.store(0);
            return 0;
        }

        bool Ask()
        {
            if (!g_tok[0]) {
                const char* tok = net::account::Token();
                if (!tok || std::strlen(tok) < kTokenLen)
                    return false;             // no account token yet: try next tick
                std::memcpy(g_tok, tok, kTokenLen);
                g_tok[kTokenLen] = 0;
            }
            if (g_busy.load())
                return false;

            g_busy.store(1);
            HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
            if (!h) {
                g_busy.store(0);
                return false;
            }
            CloseHandle(h);
            return true;
        }

    }   // namespace

    void Tick(uint64_t now)
    {
        // consume a finished 0xBF poll
        const int res = g_reply.exchange(0);
        if (res == kAskOk) {
            social::RankParse(&g_table, g_rep,
                static_cast<size_t>(g_repLen.load()));
            g_dead = 0;
            g_deadAt = 0;
        } else if (res != kAskNone) {
            // An older daemon that answers other ops but ignores 0xBF: give up
            // after two tries (ext RkbTick / the 0xBF branch of clan_worker). An
            // unreachable daemon (AliveWithin false) is an outage, not a "dead op".
            if (net::daemon::AliveWithin(60000))
                ++g_dead;
        }

        // giving up is not for good: retry every 20 s (ext RkbTick)
        if (g_dead >= 2) {
            if (!g_deadAt)
                g_deadAt = now;
            else if (now - g_deadAt > 20000) {
                g_dead = 1;
                g_deadAt = 0;
            }
        } else {
            g_deadAt = 0;
        }

        // poll while a screen asked for a rank in the last 15 s (ext: 2.5 s after
        // a missing name, else 12 s)
        if (g_dead < 2 && g_wantAt && now - g_wantAt < 15000
            && now >= g_next && !g_busy.load()) {
            if (Ask()) {
                g_next = now + (g_miss ? 2500 : 12000);
                g_miss = false;
            }
        }
    }

    void Reset()
    {
        g_table = social::RankTable{};
        g_busy.store(0);
        g_reply.store(0);
        g_repLen.store(0);
        g_rep[0] = 0;
        g_wantAt = g_next = g_deadAt = 0;
        g_miss = false;
        g_dead = 0;
    }

    bool DrawBadge(const char* iconPath, const char* numPath, const char* name,
        bool playerRow)
    {
        int rk = -1, wk = 0;
        bool has = false;
        if (name && name[0]) {
            g_wantAt = ::GetTickCount64();
            if (social::RankLookup(&g_table, name, &rk, &wk)) {
                has = true;
            } else {
                g_miss = true;
                // A real player row the table does not list yet: the lowest rank
                // once the daemon has answered at least once (ext FrSetRow).
                if (playerRow && g_table.ver > 0) {
                    rk = wk = 0;
                    has = true;
                }
            }
        }

        // RankUi (rank13.h): icon shows the symbol image, num the weeks text;
        // an unknown rank (no badge) hides both widgets. A missing widget (older
        // GUI pak) resolves to 0 -- every write is then inert (honest gate).
        if (!has || social::RankSym(rk) < 0) {
            engine::widget::Visible(engine::widget::Page(iconPath), false);
            engine::widget::SetText(engine::widget::Page(numPath), "");
            engine::widget::Visible(engine::widget::Page(numPath), false);
            return false;
        }

        char p[64], t[16];
        social::RankIconPath(p, sizeof p, rk);
        engine::widget::SetImage(engine::widget::Page(iconPath), p);
        engine::widget::Visible(engine::widget::Page(iconPath), true);
        std::snprintf(t, sizeof t, "%d", social::RankWeeks(wk));
        engine::widget::SetText(engine::widget::Page(numPath), t);
        engine::widget::Visible(engine::widget::Page(numPath), true);
        return true;
    }

}   // namespace net::rank_badge
