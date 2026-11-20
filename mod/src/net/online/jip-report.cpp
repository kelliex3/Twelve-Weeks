// ported from tools/ext/t2r-ext.c (FeatureJoinInProgress, JipAsk, JipWorker; the
// g_srvTestFlags read in the 0xE5 reply section). Wire / clock / server settings only.

#include "jip-report.hpp"

#include <winsock2.h>
#include <windows.h>

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/flag.hpp"
#include "engine/lobby.hpp"
#include "engine/script.hpp"
#include "net/anim/anim.hpp"
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
        constexpr uint8_t  kTeamSwap   = 0x10;   // server sends the trailing [0xA7] block
        constexpr uint8_t  kRequeue    = 0x20;   // net::requeue is ported: keep our ticket

        // Cutover gates. The roster merge / swap / reaper cores are native and observable,
        // but the ext still owns the engine lobby and the winmm peer table while it is the
        // bridge. kWriteLobby would write the three own-team fields (ctx+0x1c, roster +0x31,
        // record +0x48) and raise the swap banners; kApplyGhostPeers would call the session
        // drop path; kRequestTeamSwap would ask the daemon for the [0xA7] block. All stay
        // false until the feature is reviewed and cut over per t2r-*.txt sidecar.
        constexpr bool kWriteLobby      = false;
        constexpr bool kApplyGhostPeers = false;
        constexpr bool kRequestTeamSwap = false;

        constexpr uint8_t  kOpSwap     = 0xA7;
        constexpr uint64_t kGhostSilentMs = 15000;  // ext GHOST_SILENT_MS
        constexpr uint64_t kListedMaxMs   = 10000;  // ext: stale listed mask is ignored

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

        // Native roster shadow. The ext merges 0xE5 straight into LobbyObj(); we seed a
        // local copy from the read-only engine view each reply and merge there, so no
        // engine state is touched until kWriteLobby is flipped.
        struct RosterShadow {
            int     count = 0;
            uint8_t entry[engine::lobby::kRosterMax][engine::lobby::kEntryBytes]{};
        } g_shadow;
        uint8_t  g_own[engine::lobby::kEntryBytes]{}; // our saved roster entry
        bool     g_ownHave = false;
        uint32_t g_listedMask = 0;                    // slots in the newest server roster
        uint64_t g_listedAt = 0;
        int      g_swapOwnTeam = -1;                  // own team from the 0xA7 block
        uint32_t g_ghostLogged = 0;                   // one candidate log per slot

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

        // ---- 0xE5 roster shadow (ext merge / TeamSwapSeen / TeamSwapApply) -----------
        constexpr size_t kEntry = engine::lobby::kEntryBytes; // 0x32

        void SeedShadow()
        {
            // The lobby is read-only here: this is the ext's current table, copied into
            // our shadow. A menu-time / unreadable lobby yields an empty shadow.
            g_shadow.count = 0;
            std::memset(g_shadow.entry, 0, sizeof g_shadow.entry);
            const int n = engine::lobby::RosterCount();
            for (int i = 0; i < n; ++i) {
                const uint8_t* e = engine::lobby::RosterEntry(i);
                if (e)
                    std::memcpy(g_shadow.entry[g_shadow.count++], e, kEntry);
            }
        }

        int FindSlot(int slot)
        {
            for (int i = 0; i < g_shadow.count; ++i)
                if (g_shadow.entry[i][0] == static_cast<uint8_t>(slot))
                    return i;
            return -1;
        }

        int TeamOfSlotLocal(int slot)
        {
            if (slot == net::session::MySlot() && g_swapOwnTeam >= 0)
                return g_swapOwnTeam;
            const int at = FindSlot(slot);
            return at >= 0 ? (g_shadow.entry[at][0x31] & 1) : -1;
        }

        // A roster entry for the same name whose team changed: the banner the ext raises
        // from the merge, before the entry is overwritten.
        void TeamSwapSeen(const uint8_t* cur, const uint8_t* e, int me)
        {
            const int nowTeam = e[0x31] & 1;
            if (e[0] == static_cast<uint8_t>(me)
                || (cur[0x31] & 1) == nowTeam
                || std::memcmp(cur + 1, e + 1, 0x30) || !e[1])
                return;
            const int mine = TeamOfSlotLocal(me);
            char text[96];
            std::snprintf(text, sizeof text,
                mine == nowTeam ? "%.24s JOINED YOUR TEAM"
                                : "%.24s MOVED TO THE ENEMY TEAM",
                reinterpret_cast<const char*>(e + 1));
            TO1_LOG("jip team swap: slot %u '%.24s' team %u -> %u (our team %d) - "
                    "banner '%s'%s\n", e[0], reinterpret_cast<const char*>(e + 1),
                cur[0x31], e[0x31], mine, text,
                kWriteLobby ? "" : " [engine banner gated]");
            if (kWriteLobby)
                HUD::SHOW_NOTIFICATION(text, 4.0f);
        }

        // ext's 0xE5 merge: prune entries the server no longer lists, replace known slots,
        // append new ones, keep our own entry and never take a fallback name over a real one.
        void ApplyRoster(int n)
        {
            const int me = net::session::MySlot();
            if (n > static_cast<int>(engine::lobby::kRosterMax))
                n = static_cast<int>(engine::lobby::kRosterMax);
            if (n < 0 || g_repN < 2 + static_cast<size_t>(n) * kEntry)
                return;

            // Listed mask / timestamp feed the ghost reaper; an empty reply is ignored so
            // an early server answer cannot wipe the shadow.
            uint32_t mask = 0;
            for (int i = 0; i < n; ++i) {
                const uint8_t sl = g_rep[2 + static_cast<size_t>(i) * kEntry];
                if (sl < 32)
                    mask |= 1u << sl;
            }
            g_listedMask = n > 0 ? mask : 0;
            g_listedAt = n > 0 ? GetTickCount64() : 0;

            if (me >= 0 && me < static_cast<int>(engine::lobby::kRosterMax)) {
                for (int k = 0; k < g_shadow.count; ++k) {
                    if (g_shadow.entry[k][0] == static_cast<uint8_t>(me)) {
                        std::memcpy(g_own, g_shadow.entry[k], kEntry);
                        g_ownHave = true;
                        break;
                    }
                }
            }

            if (me >= 0 && me < static_cast<int>(engine::lobby::kRosterMax) && n > 0) {
                for (int k = 0; k < g_shadow.count;) {
                    const uint8_t sl = g_shadow.entry[k][0];
                    bool listed = sl == static_cast<uint8_t>(me)
                        || (g_ownHave && !std::memcmp(g_shadow.entry[k] + 1, g_own + 1, 0x30));
                    for (int i = 0; i < n && !listed; ++i)
                        listed = g_rep[2 + static_cast<size_t>(i) * kEntry] == sl;
                    if (listed) {
                        ++k;
                        continue;
                    }
                    TO1_LOG("jip: roster slot %u '%.24s' not in the server's roster "
                            "- removed (shadow)\n", sl,
                        reinterpret_cast<const char*>(g_shadow.entry[k] + 1));
                    const int tail = g_shadow.count - k - 1;
                    if (tail > 0)
                        std::memmove(g_shadow.entry[k], g_shadow.entry[k + 1],
                            static_cast<size_t>(tail) * kEntry);
                    std::memset(g_shadow.entry[g_shadow.count - 1], 0, kEntry);
                    --g_shadow.count;
                }
            }

            for (int i = 0; i < n; ++i) {
                const uint8_t* e = g_rep + 2 + static_cast<size_t>(i) * kEntry;
                if (e[0] >= engine::lobby::kRosterMax || e[0] == static_cast<uint8_t>(me))
                    continue;
                int at = FindSlot(e[0]);
                if (at < 0) {
                    if (g_shadow.count >= static_cast<int>(engine::lobby::kRosterMax))
                        continue;
                    at = g_shadow.count++;
                    TO1_LOG("jip: roster +slot %u '%.24s' team %u (%d entries now)\n",
                        e[0], reinterpret_cast<const char*>(e + 1), e[0x31], g_shadow.count);
                } else if (std::memcmp(g_shadow.entry[at], e, kEntry)) {
                    uint8_t* cur = g_shadow.entry[at];
                    // The daemon's fallback name must not replace a real name we hold.
                    if (!std::strncmp(reinterpret_cast<const char*>(e + 1), "PLAYER ", 7)
                        && cur[1]
                        && std::strncmp(reinterpret_cast<const char*>(cur + 1), "PLAYER ", 7)) {
                        TO1_LOG("jip: roster slot %u name '%.24s' kept - server sent its "
                                "fallback '%.24s'\n", e[0],
                            reinterpret_cast<const char*>(cur + 1),
                            reinterpret_cast<const char*>(e + 1));
                        uint8_t keep[0x30];
                        std::memcpy(keep, cur + 1, sizeof keep);
                        std::memcpy(cur, e, kEntry);
                        std::memcpy(cur + 1, keep, sizeof keep);
                        continue;
                    }
                    TeamSwapSeen(cur, e, me);
                    TO1_LOG("jip: roster slot %u '%.24s' team %u updated\n", e[0],
                        reinterpret_cast<const char*>(e + 1), e[0x31]);
                } else {
                    continue;
                }
                std::memcpy(g_shadow.entry[at], e, kEntry);
            }

            // Put our own entry back if an early / partial reply removed it.
            if (g_ownHave && g_own[0] == static_cast<uint8_t>(me)
                && FindSlot(me) < 0
                && g_shadow.count < static_cast<int>(engine::lobby::kRosterMax)) {
                std::memcpy(g_shadow.entry[g_shadow.count++], g_own, kEntry);
                TO1_LOG("jip: our own entry (slot %u '%.24s' team %u) was missing - "
                        "put back (%d entries now)\n", me,
                    reinterpret_cast<const char*>(g_own + 1), g_own[0x31], g_shadow.count);
            }
        }

        // Trailing [0xA7][k][k x (slot, team)] block. Only our own slot applies, and only
        // to the three places winmm keeps our team; all three writes stay gated here.
        void ParseSwap(int n)
        {
            if (n > static_cast<int>(engine::lobby::kRosterMax))
                n = static_cast<int>(engine::lobby::kRosterMax);
            const size_t at = 2 + static_cast<size_t>(n) * kEntry + 1; // after the wave byte
            if (g_repN < at + 2 || g_rep[at] != kOpSwap)
                return;
            int k = g_rep[at + 1] > 8 ? 8 : g_rep[at + 1];
            if (g_repN < at + 2 + static_cast<size_t>(k) * 2)
                k = static_cast<int>((g_repN - at - 2) / 2);
            if (k <= 0)
                return;

            const int me = net::session::MySlot();
            if (me < 0 || me >= static_cast<int>(engine::lobby::kRosterMax)
                || engine::lobby::MatchOver())
                return;                         // teams stay as they ended
            const uint8_t* blk = g_rep + at + 2;
            for (int i = 0; i < k; ++i) {
                const int slot = blk[i * 2];
                const int team = blk[i * 2 + 1];
                if (slot != me || (team != 0 && team != 1))
                    continue;
                const int pos = FindSlot(me);
                uint8_t* rec = engine::lobby::PeerRecord(me);
                const int have = rec ? *reinterpret_cast<int32_t*>(rec + 0x48) : -2;
                const int oldCtx = static_cast<int>(engine::lobby::OwnTeam());
                const int oldRos = pos >= 0 ? g_shadow.entry[pos][0x31] : -1;
                const int to0 = engine::lobby::TeamOf(me);
                if (oldCtx == team && (pos < 0 || oldRos == team)
                    && (have < 0 || have == team))
                    return;                     // already on that team

                if (pos >= 0)
                    g_shadow.entry[pos][0x31] = static_cast<uint8_t>(team);
                if (g_ownHave && g_own[0] == static_cast<uint8_t>(me))
                    g_own[0x31] = static_cast<uint8_t>(team);
                g_swapOwnTeam = team;
                const int to1 = engine::lobby::TeamOf(me);
                TO1_LOG("jip team swap: WE moved to team %d (slot %u): ctx+0x1c %d -> %u, "
                        "roster[%d][0x31] %d -> %d, record +0x48 %d -> %d, TeamOf %d -> %d"
                        "%s\n", team, me, oldCtx, kWriteLobby ? team : oldCtx, pos, oldRos,
                    pos >= 0 ? team : -1, have, have >= 0 && kWriteLobby ? team : have,
                    to0, to1, kWriteLobby ? "" : " [engine writes gated]");
                if (!kWriteLobby)
                    continue;
                if (uint8_t* lo = engine::lobby::Object())
                    lo[0x1C] = static_cast<uint8_t>(team);
                if (pos >= 0)
                    g_shadow.entry[pos][0x31] = static_cast<uint8_t>(team);
                if (rec && have >= 0)
                    *reinterpret_cast<int32_t*>(rec + 0x48) = team;
                if (oldCtx != team)
                    HUD::SHOW_NOTIFICATION("YOU WERE MOVED TO THE OTHER TEAM", 5.0f);
            }
        }

        // ---- ghost peer reaping (ext GhostPeerReap) --------------------------------------
        void ReapTick(uint64_t now)
        {
            static int off = -1;
            static uint64_t check = 0;
            static uint64_t since[engine::lobby::kRosterMax]{};
            static uint32_t lastStamp[engine::lobby::kRosterMax]{};
            static uint32_t reaped = 0;

            if (now - check > 2000) {
                check = now;
                const int o = engine::flag::Absent("t2r-ghost-peer-old.txt") ? 0 : 1;
                if (o != off) {
                    TO1_LOG("ghost peer: %s\n", o
                        ? "off (t2r-ghost-peer-old.txt)" : "on");
                    off = o;
                }
            }
            if (off || !g_listedAt || now - g_listedAt > kListedMaxMs
                || !engine::lobby::InMatch()) {
                std::memset(since, 0, sizeof since);
                return;
            }
            const int me = net::session::MySlot();
            for (int s = 0; s < static_cast<int>(engine::lobby::kRosterMax); ++s) {
                if (s == me)
                    continue;
                const bool listed = (g_listedMask >> s) & 1u;
                if (listed)
                    reaped &= ~(1u << s);
                const uint32_t stamp = NetAnimRxStampMs(s);
                const bool active = net::session::PeerActive(s);
                // The current host is never a ghost (slot 0 in this build's winmm table);
                // no active peer / a fresh anim publish also clears the silence clock.
                if (!active || s == 0 || listed || (reaped >> s) & 1u
                    || stamp != lastStamp[s]) {
                    lastStamp[s] = stamp;
                    since[s] = 0;
                    continue;
                }
                if (!since[s]) {
                    since[s] = now;
                    if (!((g_ghostLogged >> s) & 1u)) {
                        g_ghostLogged |= 1u << s;
                        TO1_LOG("ghost peer: slot %u is not in the server's roster and has "
                                "not published an anim - candidate (%.0f s silence window)%s\n",
                            s, static_cast<double>(kGhostSilentMs) / 1000.0,
                            kApplyGhostPeers ? "" : " [drop gated]");
                    }
                    continue;
                }
                if (now - since[s] < kGhostSilentMs)
                    continue;
                TO1_LOG("ghost peer: slot %u silent %.0f s - %s (puppet removed%s)\n", s,
                    static_cast<double>(now - since[s]) / 1000.0,
                    kApplyGhostPeers ? "dropped" : "drop gated (ext owns the peer table)",
                    net::session::IsHost() ? ", peer entry cleared" : "");
                if (kApplyGhostPeers)
                    net::session::DropPeerForReaper(s, "ghost roster");
                since[s] = 0;
                reaped |= 1u << s;
            }
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
                return;                        // ext: no roster in a disabled reply
            }

            // The ext merges the reply into LobbyObj(). We do the identical algorithm
            // against a native shadow (seeded from the read-only engine view), then parse
            // the optional in-match team-swap block after the wave byte.
            SeedShadow();
            const int rosterN = n > static_cast<int>(engine::lobby::kRosterMax)
                ? static_cast<int>(engine::lobby::kRosterMax) : n;
            ApplyRoster(rosterN);
            ParseSwap(rosterN);
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

        // ext calls GhostPeerReap() every in-match tick, before its `off` early return:
        // a server with JIP disabled can still leave a stale peer behind after a handover.
        ReapTick(now);

        if (g_off || now < g_nextAsk || g_busy.load())
            return;

        const NetTimerSync t = net::gui2::MatchSnapshot();
        uint8_t flags = 0;
        if (t.duration > 0.0f && t.duration < 36000.0f
            && t.remaining >= 0.0f && t.remaining <= t.duration + 1.0f)
            flags |= kClockValid;
        if (net::gui2::ScoreboardShowing() || net::gui2::MatchExpired())
            flags |= kMatchOver;
        // The server only issues a requeue ticket to clients that asked for it with the clock
        // report. Same sidecar file requeue.c reads, so a cutover disables both at once.
        if (engine::flag::Absent("t2r-no-requeue.txt", 300))
            flags |= kRequeue;
        // Asking for the [0xA7] swap block is a cutover: until kRequestTeamSwap flips, the
        // parser above only sees a block if an older / third-party daemon sends one anyway.
        if (kRequestTeamSwap && engine::flag::Absent("t2r-no-team-swap.txt", 300))
            flags |= kTeamSwap;

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
        g_shadow = RosterShadow{};
        g_ownHave = false;
        std::memset(g_own, 0, sizeof g_own);
        g_listedMask = 0;
        g_listedAt = 0;
        g_swapOwnTeam = -1;
        g_ghostLogged = 0;
        // g_busy is owned by the worker; it clears itself when the request finishes.
    }

    // ---- read-only view of the merged shadow ---------------------------------------------
    int RosterCount()
    {
        return g_shadow.count;
    }

    const uint8_t* RosterEntry(int index)
    {
        return index >= 0 && index < g_shadow.count
            ? g_shadow.entry[index] : nullptr;
    }

    int TeamOfSlot(int slot)
    {
        return TeamOfSlotLocal(slot);
    }

    int OwnTeam()
    {
        return g_swapOwnTeam;
    }

    uint32_t ListedSlots()
    {
        return g_listedMask;
    }

    uint64_t ListedMs()
    {
        return g_listedAt;
    }

}
