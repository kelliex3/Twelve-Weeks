#include "clan-page-data.hpp"

#include "net-protocol.hpp"
#include "daemon-rpc.hpp"
#include "net-account.hpp"
#include "log.hpp"

#include <windows.h>

#include <cstring>

namespace net::clan::page {

    namespace {

        constexpr uint8_t kOpReq  = static_cast<uint8_t>(NetDaemonOp::kClanPageReq);
        constexpr uint8_t kOpRep  = static_cast<uint8_t>(NetDaemonOp::kClanPageRep);
        constexpr int     kCap    = 65536;    // ext Cl13Ask's capability byte = 1
        constexpr int     kConnMs = 2500;
        constexpr int     kIoMs   = 3500;
        constexpr int     kReplyMs = 4000;
        constexpr uint64_t kRefreshMs = 30000;

        // ext C13_REQ order: the focused tab's sections first. The mod has no
        // tab, so it cycles all six and always asks a missing one first.
        constexpr int kOrder[6] = { 1, 2, 3, 4, 6, 5 };

        struct Runtime {
            Data data;
            int  ver[8]{};
            uint64_t okAt[8]{};
            uint64_t nextAsk = 0;
            uint8_t lang = 0;
            long seenJid = 0;
            int  seenDay = 0;
            int  statsDay = -1;
            int  week = 0;
        } g;

        bool Ask(int section, char* text, int cap)
        {
            sockaddr_in a{};
            if (!net::daemon::Addr(&a) || !net::account::HaveToken())
                return false;
            const char* tok = net::account::Token();
            if (std::strlen(tok) != 64)
                return false;

            const int day = section == 5 ? (g.statsDay < 0 ? 0 : g.statsDay)
                          : section == 6 ? g.week : g.seenDay;
            uint8_t ex[9];
            ex[0] = g.lang;
            ex[1] = static_cast<uint8_t>(section);
            ex[2] = static_cast<uint8_t>(g.seenJid);
            ex[3] = static_cast<uint8_t>(g.seenJid >> 8);
            ex[4] = static_cast<uint8_t>(g.seenJid >> 16);
            ex[5] = static_cast<uint8_t>(g.seenJid >> 24);
            ex[6] = static_cast<uint8_t>(day);
            ex[7] = static_cast<uint8_t>(day >> 8);
            ex[8] = 1;                       // 64 KB reply capability
            uint8_t frame[128];
            const int len = 1 + 64 + static_cast<int>(sizeof ex);
            frame[0] = static_cast<uint8_t>(len);
            frame[1] = static_cast<uint8_t>(len >> 8);
            frame[2] = kOpReq;
            std::memcpy(frame + 3, tok, 64);
            std::memcpy(frame + 3 + 64, ex, sizeof ex);

            static char buf[kCap];
            int sent = 0;
            const int got = net::daemon::Rpc(a, frame, len + 2, buf, sizeof buf,
                                             kConnMs, kIoMs, kReplyMs, &sent);
            if (got < 3)
                return false;
            const int fl = static_cast<uint8_t>(buf[0]) + (static_cast<uint8_t>(buf[1]) << 8);
            if (fl < 1 || fl + 2 > got || static_cast<uint8_t>(buf[2]) != kOpRep)
                return false;
            const int ntext = fl - 1;
            if (ntext < 0 || ntext >= cap)
                return false;
            std::memcpy(text, buf + 3, static_cast<size_t>(ntext));
            text[ntext] = 0;
            return true;
        }

        int Pick()
        {
            for (int k = 0; k < 6; ++k) {
                const int s = kOrder[k];
                if (!g.data.have[s])
                    return s;
            }
            const uint64_t now = GetTickCount64();
            for (int k = 0; k < 6; ++k) {
                const int s = kOrder[k];
                if (now - g.okAt[s] >= kRefreshMs)
                    return s;
            }
            return 0;
        }

    } // namespace

    void Reset()
    {
        g = Runtime{};
        g.statsDay = -1;
    }

    void Tick()
    {
        const uint64_t now = GetTickCount64();
        if (now < g.nextAsk)
            return;
        const int sec = Pick();
        if (!sec)
            return;
        static char text[kCap];
        if (!Ask(sec, text, sizeof text)) {
            g.nextAsk = now + 2500;
            return;
        }
        if (Parse(g.data, sec, text) < 0) {
            g.nextAsk = now + 2500;
            return;
        }
        ++g.ver[sec];
        g.okAt[sec] = now;
        g.nextAsk = now + (g.data.have[sec] ? 5000 : 2500);
        net::daemon::MarkAlive();
    }

    const Data& Snapshot() { return g.data; }
    bool Have(int section) { return section >= 1 && section <= 6 && g.data.have[section] != 0; }
    int  Version(int section) { return section >= 1 && section <= 6 ? g.ver[section] : 0; }
    bool Busy() { return GetTickCount64() < g.nextAsk; }

    void NoteStatsDay(int day)   { g.statsDay = day; }
    void NoteJournalWeek(int week){ g.week = week; }
    void NoteSeen(long journalId, int day) { g.seenJid = journalId; g.seenDay = day; }
    void SetLang(uint8_t lang)   { g.lang = lang ? 1 : 0; }

} // namespace net::clan::page
