// Native port of tools/ext/chat.c (builds 199..201): the in-game MATCH / TEAM / PARTY /
// LOBBY line stores, their editor and the HUD / ESC / FIND MATCH panels.
//
// The ext-module seams this deliberately does NOT duplicate:
//   - no WH_KEYBOARD_LL / raw-input thunk: text_input.cpp is the one hook-free sampler
//     and the one typing gate (friends13 shares it);
//   - no settext hook: every widget write goes through engine::widget;
//   - no second AFK / pad path, no ext toggle gate (only the ext's t2r-no-chat.txt
//     opt-out is read, through engine::flag);
//   - no winmm / net-smooth / send-gate / root-vec4 patch;
//   - no engine state write: the native death screen and escscore keep their authority.
//
// Wire facts stay in net-protocol.hpp (MSG_CHAT 0xC7 and the 0xD8 chat sub-ops). The
// packet / filter / JSON core is chat-core.hpp with its own standalone test.

#include "chat.hpp"
#include "chat-core.hpp"

#include "log.hpp"          // TO1_LOG / AUDIO::PLAY_SOUND must see it before engine/script.hpp
#include "net-protocol.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/lobby.hpp"
#include "engine/locale.hpp"
#include "engine/script.hpp"
#include "engine/text-input.hpp"
#include "engine/widget.hpp"
#include "engine/window.hpp"
#include "daemon-rpc.hpp"
#include "net-account.hpp"
#include "net-player.hpp"
#include "net-session.hpp"
#include "net/ui/net-gui2.hpp"
#include "net/match/match-liveness.hpp"
#include "net/social/friends-state.hpp"
#include "net/social/friends13.hpp"
#include "net/social/rank-badge.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace net::chat {

    namespace {

        using core::Channel;
        using core::kLineBytes;

        constexpr int    kChannels   = 4;       // MATCH / TEAM / PARTY / LOBBY
        constexpr int    kKeep       = 40;      // ext CH_KEEP
        constexpr int    kTabCount   = 3;       // HUD / ESC tabs (LOBBY is menu-only)
        constexpr uint64_t kIdleMs   = 8000;
        constexpr uint64_t kFadeMs   = 600;
        constexpr uint64_t kPollMs   = 2000;
        constexpr float  kFit        = 0.94f;
        constexpr float  kFitField   = 0.98f;
        constexpr float  kTabGap     = 1.5f;
        constexpr int    kRows       = 8;
        constexpr size_t kRowBytes   = kLineBytes + 96;
        constexpr size_t kArgCap     = kLineBytes + 4;
        constexpr size_t kReplyCap   = 24000;
        constexpr size_t kQueueCap   = 6;

        const char* const kTabEn[kChannels] = { "MATCH", "TEAM", "PARTY", "LOBBY" };
        const char* const kTabPl[kChannels] = { "MECZ", "DRUŻYNA", "GRUPA", "LOBBY" };
        const char* const kCol[kChannels]   = { "#D7D7D7", "#48A8D8", "#D9D86A", "#8FB0C8" };

        uint64_t Now() { return ::GetTickCount64(); }
        bool Pl() { return engine::locale::IsPolish(); }
        const char* Tr(const char* en, const char* pl) { return engine::locale::Tr(en, pl); }
        void Sfx(const char* name) { AUDIO::PLAY_SOUND(SID(name)); (void)name; }

        // ext nr_cwf * pt * 1.06, with the native estimator's fixed 30pt/1.08 gain removed.
        float Width(const char* s, float pt) { return engine::widget::TextWidth(s, pt / 32.4f); }

        // Bounded form for the wrapping loops (TextWidth stops only at the NUL).
        float WidthN(const char* s, size_t n, float pt)
        {
            char tmp[kRowBytes];
            const size_t k = std::min(n, sizeof(tmp) - 1);
            std::memcpy(tmp, s, k);
            tmp[k] = 0;
            return Width(tmp, pt);
        }

        const char* TabName(int c) { return Pl() ? kTabPl[c] : kTabEn[c]; }

        // ---- line stores ----------------------------------------------------------------
        struct Line {
            char     who[48]{};
            char     text[kLineBytes]{};
            const char* col = "#E6E6E6";
            uint64_t ms = 0;
        };

        struct Store {
            std::array<Line, kKeep> lines[kChannels]{};
            int    n[kChannels]{};
            int    tab = Channel::kMatch;
            int    dot[kChannels]{};
            uint64_t lastMs[kChannels]{};
            unsigned gen = 0;
        };

        Store g_store;

        void AddLine(int channel, const char* who, const char* col, const char* text, uint64_t now)
        {
            if (channel < 0 || channel >= kChannels || !text || !text[0]) return;
            if (g_store.n[channel] >= kKeep) {
                std::memmove(&g_store.lines[channel][0], &g_store.lines[channel][1],
                    sizeof(Line) * static_cast<size_t>(kKeep - 1));
                g_store.n[channel] = kKeep - 1;
            }
            Line& l = g_store.lines[channel][g_store.n[channel]++];
            std::snprintf(l.who, sizeof l.who, "%s", who ? who : "");
            std::snprintf(l.text, sizeof l.text, "%s", text);
            core::Censor(l.who);
            core::Censor(l.text);
            l.col = col ? col : "#E6E6E6";
            l.ms = g_store.lastMs[channel] = now;
            if (channel != g_store.tab) g_store.dot[channel] = 1;
            g_store.gen++;
        }

        void ClearMatchLines()
        {
            g_store.n[Channel::kMatch] = g_store.n[Channel::kTeam] = 0;
            g_store.dot[Channel::kMatch] = g_store.dot[Channel::kTeam] = 0;
            g_store.lastMs[Channel::kMatch] = g_store.lastMs[Channel::kTeam] = 0;
            g_store.gen++;
        }

        const char* NameColor(const char* who, bool own, const char* base)
        {
            // The ext takes OUR role from the 0xE1 clan state, which the native port
            // does not read; the shared 0xBF table is the honest substitute for both.
            (void)own;
            const int role = net::rank_badge::RoleOf(who ? who : "");
            const char* hex = net::social::RankRoleHex(role);
            return hex ? hex : base;
        }

        void OwnName(char* out, size_t cap)
        {
            const char* src = nullptr;
            if (const NetPlayer* me = NetPlayers::Local()) src = me->name;
            if (!src || !*src) src = net::account::Name();
            if (!src || !*src) src = Pl() ? "TY" : "YOU";
            core::Clean(src, std::strlen(src), out, cap, 24);
        }

        // ---- rows (a long line wraps into several rows, bottom aligned) -------------------
        using Row = char[kRowBytes];

        int LineRows(const Line& l, float maxw, float pt, Row* out)
        {
            char pre[64] = "";
            if (l.who[0]) std::snprintf(pre, sizeof pre, "%s: ", l.who);
            const char* t = l.text;
            const int n = static_cast<int>(std::strlen(t));
            int at = 0, nr = 0;
            while (at < n && nr < kRows) {
                const float room = maxw - (nr ? 0.0f : Width(pre, pt));
                int end = at, brk = -1;
                while (end < n) {
                    int nx = end + 1;
                    while (nx < n && (static_cast<unsigned char>(t[nx]) & 0xC0) == 0x80) nx++;
                    if (WidthN(t + at, static_cast<size_t>(nx - at), pt) > room) break;
                    if (t[end] == ' ') brk = end;
                    end = nx;
                }
                if (end < n) {
                    if (brk > at) end = brk;
                    else if (end == at) {
                        end++;
                        while (end < n && (static_cast<unsigned char>(t[end]) & 0xC0) == 0x80) end++;
                    }
                }
                if (!nr && l.who[0])
                    std::snprintf(out[nr], kRowBytes,
                        "<font color=%c%s%c>%s:</font> %.*s", 34, l.col, 34, l.who,
                        end - at, t + at);
                else
                    std::snprintf(out[nr], kRowBytes, "%.*s", end - at, t + at);
                nr++;
                at = end;
                while (at < n && t[at] == ' ') at++;
            }
            if (!nr) {
                if (l.who[0]) std::snprintf(out[0], kRowBytes,
                    "<font color=%c%s%c>%s:</font>", 34, l.col, 34, l.who);
                else out[0][0] = 0;
                nr = 1;
            }
            return nr;
        }

        void TabRows(int tab, float maxw, float pt, Row* rows, int nrows)
        {
            for (int k = 0; k < nrows; k++) rows[k][0] = 0;
            Row local[kRows];
            int k = nrows;
            for (int idx = g_store.n[tab] - 1; idx >= 0 && k > 0; idx--) {
                const int m = LineRows(g_store.lines[tab][idx], maxw, pt, local);
                for (int r = m - 1; r >= 0 && k > 0; r--)
                    std::memcpy(rows[--k], local[r], kRowBytes);
            }
        }

        void TabsText(char out[kTabCount][96])
        {
            for (int c = 0; c < kTabCount; c++) {
                const bool on = c == g_store.tab;
                std::snprintf(out[c], 96, "<font color=%c%s%c>%s%s</font>", 34,
                    on ? kCol[c] : "#6E7074", 34, TabName(c),
                    !on && g_store.dot[c] ? " *" : "");
            }
        }

        // Right-packed tab names; returns the left edge of the leftmost one.
        float TabsPack(char names[kTabCount][96], float right, float pt,
                       float* x, float* w)
        {
            for (int c = kTabCount - 1; c >= 0; c--) {
                x[c] = right;
                w[c] = Width(TabName(c), pt);
                right -= Width(names[c], pt) + (c ? kTabGap * pt : 0.0f);
            }
            return right;
        }

        // ---- the one editor --------------------------------------------------------------
        enum class EditResult { None, Send, Cancel, Tab };

        struct Editor {
            bool     on = false;
            char     buf[kLineBytes]{};
            int      len = 0, caret = 0, nch = 0, scroll = 0;
            uint64_t t0 = 0, edit = 0;
        };

        int g_where = 0;      // 1 HUD, 2 ESC menu, 3 FIND MATCH / MP menu
        Editor g_ed;

        int PrevCh(const Editor& e, int i)
        {
            if (i > 0) i--;
            while (i > 0 && (static_cast<unsigned char>(e.buf[i]) & 0xC0) == 0x80) i--;
            return i;
        }

        int NextCh(const Editor& e, int i)
        {
            if (i < e.len) i++;
            while (i < e.len && (static_cast<unsigned char>(e.buf[i]) & 0xC0) == 0x80) i++;
            return i;
        }

        void Cut(Editor& e, int from, int to)
        {
            from = std::max(from, 0);
            to = std::min(to, e.len);
            if (from >= to) return;
            std::memmove(e.buf + from, e.buf + to,
                static_cast<size_t>(e.len - to) + 1);
            e.len -= to - from;
            e.caret = from;
            e.nch = 0;
            for (int i = 0; i < e.len; i++)
                if ((static_cast<unsigned char>(e.buf[i]) & 0xC0) != 0x80) e.nch++;
            e.edit = Now();
        }

        void Put(Editor& e, uint32_t cp)
        {
            if (!core::Drawable(cp) || e.nch >= static_cast<int>(core::kMaxChars)) return;
            char u[4];
            size_t k = 0;
            if (!core::PutUtf8(u, sizeof u, &k, cp) || e.len + static_cast<int>(k) >= static_cast<int>(kLineBytes) - 1)
                return;
            std::memmove(e.buf + e.caret + k, e.buf + e.caret,
                static_cast<size_t>(e.len - e.caret) + 1);
            std::memcpy(e.buf + e.caret, u, k);
            e.len += static_cast<int>(k);
            e.caret += static_cast<int>(k);
            e.nch++;
            e.edit = Now();
        }

        void Paste(Editor& e)
        {
            char text[512];
            if (!engine::text_input::Paste(text, sizeof text)) return;
            const unsigned char* s = reinterpret_cast<const unsigned char*>(text);
            const size_t n = std::strlen(text);
            for (size_t i = 0; i < n;) {
                int l = 0;
                uint32_t cp = core::Utf8Next(s + i, n - i, &l);
                if (l <= 0) break;
                i += static_cast<size_t>(l);
                if (cp == '\n' || cp == '\r' || cp == '\t') cp = ' ';
                Put(e, cp);
            }
        }

        void StartEditor(int where, uint64_t now)
        {
            g_ed = Editor{};
            g_ed.on = true;
            g_ed.t0 = g_ed.edit = now;
            g_where = where;
            if (!engine::text_input::Begin(engine::text_input::Owner::Chat)) {
                // The FRIENDS line owns the shared gate: do not steal its keys.
                g_ed.on = false;
                g_where = 0;
                return;
            }
            engine::text_input::Key key;      // keys before the field opened are dropped
            while (engine::text_input::Next(key)) {}
            g_store.dot[g_store.tab] = 0;
            g_store.gen++;
            TO1_LOG("chat: typing (%s, tab %s)\n",
                where == 1 ? "HUD" : where == 2 ? "ESC menu" : "MP menu", TabName(g_store.tab));
        }

        void StopEditor(const char* why)
        {
            (void)why;
            if (!g_ed.on) return;
            g_ed.on = false;
            g_where = 0;
            engine::text_input::End(engine::text_input::Owner::Chat);
            g_store.gen++;
            TO1_LOG("chat: typing closed (%s)\n", why);
        }

        void SwitchTab(int dir)
        {
            g_store.tab = (g_store.tab + kTabCount + dir) % kTabCount;
            g_store.dot[g_store.tab] = 0;
            g_store.gen++;
            Sfx("sfx-cycle");
        }

        // ---- server RPC (chat-sized 0xD8 argument, one request at a time) ------------------
        struct Job {
            uint8_t req[64 + 3 + kArgCap]{};
            int     n = 0;
            uint8_t sub = 0;
        };

        std::atomic<int> g_busy{ 0 }, g_have{ 0 }, g_replyLen{ 0 };
        Job              g_job;
        char             g_reply[kReplyCap + 16]{};

        DWORD WINAPI Worker(LPVOID)
        {
            Job job = g_job;
            int result = 3;
            sockaddr_in a{};
            if (net::daemon::Addr(&a)) {
                uint8_t frame[2 + 1 + sizeof(job.req)]{};
                const int n = job.n + 1;
                frame[0] = static_cast<uint8_t>(n);
                frame[1] = static_cast<uint8_t>(n >> 8);
                frame[2] = static_cast<uint8_t>(NetDaemonOp::kFriendsRequest);
                std::memcpy(frame + 3, job.req, static_cast<size_t>(job.n));
                char rep[kReplyCap + 16];
                int sent = 0;
                const int got = net::daemon::Rpc(a, frame, n + 2, rep,
                    static_cast<int>(sizeof rep) - 1, 2500, 3000, 3000, &sent);
                if (sent) result = 2;
                const int len = got >= 3
                    ? (static_cast<unsigned char>(rep[0]) | (static_cast<unsigned char>(rep[1]) << 8))
                    : 0;
                if (len >= 2 && got >= len + 2
                    && static_cast<uint8_t>(rep[2]) == static_cast<uint8_t>(NetDaemonOp::kFriendsReply)
                    && static_cast<size_t>(len - 1) < kReplyCap) {
                    std::memcpy(g_reply, rep + 3, static_cast<size_t>(len) - 1);
                    g_replyLen.store(len - 1);
                    result = 1;
                }
            }
            g_have.store(result);
            g_busy.store(0);
            return 0;
        }

        bool Request(uint8_t sub, const char* arg)
        {
            int expected = 0;
            if (!g_busy.compare_exchange_strong(expected, 1)) return false;
            const char* tok = net::account::Token();
            if (!tok || std::strlen(tok) < 64) {
                g_busy.store(0);
                return false;
            }
            Job job;
            std::memcpy(job.req, tok, 64);
            job.req[64] = 1;                                  // packet version
            job.req[65] = static_cast<uint8_t>(Pl() ? 1 : 0); // language
            job.req[66] = sub;
            const size_t al = arg ? std::min(std::strlen(arg), kArgCap) : 0;
            if (al) std::memcpy(job.req + 67, arg, al);
            job.n = 67 + static_cast<int>(al);
            job.sub = sub;
            g_job = job;
            HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
            if (!h) {
                g_busy.store(0);
                return false;
            }
            CloseHandle(h);
            return true;
        }

        struct Queued {
            uint8_t sub = 0;
            char    arg[kArgCap]{};
        };
        std::array<Queued, kQueueCap> g_queue;
        int g_queueN = 0;

        void Queue(uint8_t sub, const char* arg)
        {
            if (static_cast<size_t>(g_queueN) >= kQueueCap) {
                std::memmove(&g_queue[0], &g_queue[1], sizeof(Queued) * (kQueueCap - 1));
                g_queueN = static_cast<int>(kQueueCap) - 1;
            }
            g_queue[g_queueN].sub = sub;
            std::snprintf(g_queue[g_queueN].arg, kArgCap, "%s", arg ? arg : "");
            g_queueN++;
        }

        int  g_partyLast = -1, g_lobbyLast = -1, g_lobbyKey = 0;
        uint64_t g_pollAt = 0, g_lobbyPollAt = 0;
        bool g_menuUp = false;

        bool InParty() { return net::friends13::InParty(); }

        bool InLobby()
        {
            // ext: requeue's held ticket, or the FIND MATCH page with session state 3.
            // The requeue latch is not exposed to native callers; the engine's own
            // party status is the shared source for both cases here.
            return engine::lobby::PartyStatus() == 3;
        }

        void LobbyLines(const core::PartyReply& d, uint64_t now)
        {
            if (d.lk && d.lk != g_lobbyKey) {
                g_lobbyKey = d.lk;
                g_lobbyLast = -1;
                g_store.n[Channel::kLobby] = 0;
                g_store.gen++;
            }
            for (int i = 0; i < d.n; i++) {
                const core::PartyLine& l = d.lines[i];
                if (l.id <= g_lobbyLast) continue;
                char me[40];
                const bool own = l.me != 0;
                if (own) OwnName(me, sizeof me);
                AddLine(Channel::kLobby, own ? me : l.name,
                    NameColor(l.name, own, own ? kCol[Channel::kParty] : "#E6E6E6"),
                    l.text, now);
            }
            if (d.last > g_lobbyLast) g_lobbyLast = d.last;
        }

        void PartyLines(const core::PartyReply& d, uint64_t now)
        {
            for (int i = 0; i < d.n; i++) {
                const core::PartyLine& l = d.lines[i];
                if (l.id <= g_partyLast) continue;
                char me[40];
                const bool own = l.me != 0;
                if (own) OwnName(me, sizeof me);
                AddLine(Channel::kParty, own ? me : l.name,
                    NameColor(l.name, own, own ? kCol[Channel::kParty] : "#E6E6E6"),
                    l.text, now);
            }
            if (d.last > g_partyLast) g_partyLast = d.last;
        }

        void NetTick(uint64_t now, bool inMatch)
        {
            const int have = g_have.exchange(0);
            if (have == 1) {
                core::PartyReply d;
                if (core::ParsePartyReply(d, g_reply, static_cast<size_t>(g_replyLen.load()))) {
                    const uint8_t sub = g_job.sub;
                    const bool lobby = sub == static_cast<uint8_t>(NetDaemonChatSub::kLSay)
                                    || sub == static_cast<uint8_t>(NetDaemonChatSub::kLChat);
                    if (lobby && !d.ok && d.err[0]) {
                        const bool rate = !std::strcmp(d.err, "rate");
                        AddLine(Channel::kLobby, "", nullptr,
                            rate ? Tr("TOO MANY MESSAGES - WAIT A MOMENT", "ZA DUŻO WIADOMOŚCI - POCZEKAJ CHWILĘ")
                                 : Tr("NOT SENT - THE LOBBY IS OVER", "NIE WYSŁANO - LOBBY SIĘ SKOŃCZYŁO"), now);
                    }
                    if (lobby) {
                        if (d.has) LobbyLines(d, now);
                    } else if (!d.ok && d.err[0]) {
                        TO1_LOG("chat: server refused sub %u: %s\n", static_cast<unsigned>(sub), d.err);
                        if (sub == static_cast<uint8_t>(NetDaemonChatSub::kPSay)) {
                            const char* msg = !std::strcmp(d.err, "rate")
                                ? Tr("TOO MANY MESSAGES - WAIT A MOMENT", "ZA DUŻO WIADOMOŚCI - POCZEKAJ CHWILĘ")
                                : !std::strcmp(d.err, "noparty")
                                ? Tr("YOU ARE NOT IN A PARTY", "NIE MASZ GRUPY")
                                : Tr("NOT SENT", "NIE WYSŁANO");
                            AddLine(Channel::kParty, "", nullptr, msg, now);
                        }
                    }
                    if (d.has && !lobby) PartyLines(d, now);
                }
            }
            if (g_busy.load()) return;
            if (g_queueN) {
                const Queued q = g_queue[0];
                if (Request(q.sub, q.arg)) {
                    std::memmove(&g_queue[0], &g_queue[1],
                        sizeof(Queued) * static_cast<size_t>(g_queueN - 1));
                    g_queueN--;
                }
                return;
            }
            if (g_menuUp && !inMatch && InLobby() && now >= g_lobbyPollAt) {
                g_lobbyPollAt = now + kPollMs;
                char a[16];
                std::snprintf(a, sizeof a, "%d", g_lobbyLast < 0 ? 0 : g_lobbyLast);
                if (Request(static_cast<uint8_t>(NetDaemonChatSub::kLChat), a)) return;
            }
            if ((inMatch || g_menuUp) && InParty() && now >= g_pollAt) {
                g_pollAt = now + kPollMs;
                char a[16];
                std::snprintf(a, sizeof a, "%d", g_partyLast < 0 ? 0 : g_partyLast);
                Request(static_cast<uint8_t>(NetDaemonChatSub::kPChat), a);
            }
        }

        void QueueLog(int channel, const char* text)
        {
            char a[kLineBytes + 4];
            std::snprintf(a, sizeof a, "%c\n%s", channel == Channel::kTeam ? 't' : 'm', text);
            Queue(static_cast<uint8_t>(NetDaemonChatSub::kMLog), a);
        }

        // ---- peer send / receive -----------------------------------------------------------
        uint16_t g_seq = 0;
        core::Resender g_resend;

        void SendPeer(int channel, const char* text, uint64_t now)
        {
            core::WirePacket pkt[core::kMaxFragments];
            const size_t n = core::BuildPackets(pkt, core::kMaxFragments,
                static_cast<uint8_t>(channel), ++g_seq, text);
            if (!n) return;
            char me[40];
            OwnName(me, sizeof me);
            AddLine(channel, me, NameColor(me, true, kCol[Channel::kParty]), text, now);
            if (!net::match::liveness::Live()) {
                TO1_LOG("chat: %s line kept local (no match session)\n", kTabEn[channel]);
                return;
            }
            for (size_t i = 0; i < n; i++) {
                net::session::SendEventRaw(static_cast<uint8_t>(NetEventType::NEVT_CHAT_C7),
                    pkt[i].data, pkt[i].size);
                g_resend.Add(pkt[i], now);
            }
            QueueLog(channel, text);
            TO1_LOG("chat: sent %s line (%d bytes, seq %u)\n", kTabEn[channel],
                static_cast<int>(std::strlen(text)), static_cast<unsigned>(g_seq));
        }

        void Send(int tab, const char* text, uint64_t now)
        {
            char clean[kLineBytes];
            if (core::Clean(text, std::strlen(text), clean, sizeof clean,
                            static_cast<int>(core::kMaxChars)) <= 0) return;
            if (tab == Channel::kLobby) {
                Queue(static_cast<uint8_t>(NetDaemonChatSub::kLSay), clean);
                g_lobbyPollAt = now + kPollMs;
                return;
            }
            if (tab == Channel::kParty) {
                if (!InParty()) {
                    AddLine(Channel::kParty, "", nullptr,
                        Tr("YOU ARE NOT IN A PARTY - INVITE A FRIEND ON THE FRIENDS PAGE",
                           "NIE MASZ GRUPY - ZAPROŚ ZNAJOMEGO NA STRONIE ZNAJOMYCH"), now);
                    return;
                }
                Queue(static_cast<uint8_t>(NetDaemonChatSub::kPSay), clean);
                g_pollAt = now + kPollMs;
                return;
            }
            SendPeer(tab, clean, now);
        }

        core::Receiver g_receiver;

        EditResult EditTick(uint64_t now)
        {
            if (!g_ed.on) return EditResult::None;
            EditResult result = EditResult::None;
            const bool ctrl = engine::text_input::Down(VK_CONTROL);
            engine::text_input::Key key;
            while (result == EditResult::None && engine::text_input::Next(key)) {
                switch (key.vk) {
                case VK_RETURN: result = EditResult::Send; break;
                case VK_ESCAPE: result = EditResult::Cancel; break;
                case VK_TAB:    result = EditResult::Tab; break;
                case VK_BACK:
                    if (ctrl) {
                        int to = g_ed.caret;
                        while (to > 0 && g_ed.buf[to - 1] == ' ') to--;
                        while (to > 0 && g_ed.buf[to - 1] != ' ') to--;
                        Cut(g_ed, to, g_ed.caret);
                    } else Cut(g_ed, PrevCh(g_ed, g_ed.caret), g_ed.caret);
                    break;
                case VK_DELETE: Cut(g_ed, g_ed.caret, NextCh(g_ed, g_ed.caret)); break;
                case VK_LEFT:  g_ed.caret = PrevCh(g_ed, g_ed.caret); g_ed.edit = now; break;
                case VK_RIGHT: g_ed.caret = NextCh(g_ed, g_ed.caret); g_ed.edit = now; break;
                case VK_HOME:  g_ed.caret = 0; g_ed.edit = now; break;
                case VK_END:   g_ed.caret = g_ed.len; g_ed.edit = now; break;
                default:
                    if (ctrl && key.vk == 'V') Paste(g_ed);
                    else if (!ctrl && key.ch >= 0x20) Put(g_ed, key.ch);
                    break;
                }
            }
            return result;
        }

        bool HandleEditor(uint64_t now)
        {
            const EditResult r = EditTick(now);
            if (r == EditResult::Send) {
                if (g_ed.len) Send(g_store.tab, g_ed.buf, now);
                StopEditor("Enter");
                return true;
            }
            if (r == EditResult::Cancel) {
                StopEditor("Esc");
                return true;
            }
            if (r == EditResult::Tab) {
                if (g_where != 3) SwitchTab(engine::text_input::Down(VK_SHIFT) ? -1 : 1);
                else if (InParty() && InLobby()) {
                    g_store.tab = g_store.tab == Channel::kLobby ? Channel::kParty : Channel::kLobby;
                    g_store.dot[g_store.tab] = 0;
                    g_store.gen++;
                    Sfx("sfx-cycle");
                }
            }
            if (g_ed.on) g_store.gen++;   // caret blink
            return g_ed.on;
        }

        void EditorView(char* out, size_t cap, uint64_t now, float maxw, float pt)
        {
            maxw *= kFitField;
            const float cw = Width("|", pt);
            if (g_ed.scroll > g_ed.caret) g_ed.scroll = g_ed.caret;
            if (g_ed.scroll > g_ed.len) g_ed.scroll = g_ed.len;
            while (g_ed.scroll < g_ed.caret
                && WidthN(g_ed.buf + g_ed.scroll, static_cast<size_t>(g_ed.caret - g_ed.scroll), pt) + cw > maxw)
                g_ed.scroll = NextCh(g_ed, g_ed.scroll);
            while (g_ed.scroll > 0) {
                const int p = PrevCh(g_ed, g_ed.scroll);
                if (WidthN(g_ed.buf + p, static_cast<size_t>(g_ed.len - p), pt) + cw > maxw) break;
                g_ed.scroll = p;
            }
            int end = g_ed.caret;
            while (end < g_ed.len) {
                const int nx = NextCh(g_ed, end);
                if (WidthN(g_ed.buf + g_ed.scroll, static_cast<size_t>(nx - g_ed.scroll), pt) + cw > maxw) break;
                end = nx;
            }
            const bool caret = now - g_ed.edit < 450 || ((now - g_ed.edit) / 530) % 2 == 0;
            std::snprintf(out, cap, "%.*s%s%.*s", g_ed.caret - g_ed.scroll,
                g_ed.buf + g_ed.scroll, caret ? "|" : "<font color=\"#262626\">|</font>",
                end - g_ed.caret, g_ed.buf + g_ed.caret);
        }

        // ---- flicker-free text leaves (ext chtw_t, overlapped swap) ------------------------
        void SetTextSafe(uint64_t w, const char* s)
        {
            if (w) engine::widget::SetText(w, s && *s ? s : " ");  // an empty write leaves the old text
        }

        class Twin {
        public:
            void Bind(uint64_t a, uint64_t b, const char* init, bool visible, int lead)
            {
                m_a = a; m_b = b; m_vis = visible; m_lead = lead;
                m_shb = m_pend = false;
                m_at = 0;
                std::snprintf(m_want, sizeof m_want, "%s", init ? init : "");
                SetTextSafe(a, m_want);
                if (b) SetTextSafe(b, m_want);
                Show();
            }
            uint64_t A() const { return m_a; }
            uint64_t B() const { return m_b; }
            bool Has() const { return m_a != 0; }

            void Text(const char* s)
            {
                if (!m_a || !s || !std::strcmp(m_want, s)) return;
                std::snprintf(m_want, sizeof m_want, "%s", s);
                if (!m_b) { SetTextSafe(m_a, m_want); return; }
                if (!m_pend) Write();
            }
            void Visible(bool on)
            {
                if (!m_a || m_vis == on) return;
                m_vis = on;
                Show();
            }
            void Tick()
            {
                if (!m_a || !m_b || !m_pend
                    || engine::frame::Count() - m_at < static_cast<uint64_t>(m_lead)) return;
                m_pend = false;
                m_shb = !m_shb;
                Show();
                if (std::strcmp(m_want, m_written)) Write();
            }
            void Reset() { *this = Twin{}; }

        private:
            void Show()
            {
                if (!m_a) return;
                if (!m_b) { engine::widget::Visible(m_a, m_vis); return; }
                engine::widget::Visible(m_shb ? m_b : m_a, m_vis);
                engine::widget::Visible(m_shb ? m_a : m_b, m_vis && m_pend);
            }
            void Write()
            {
                SetTextSafe(m_shb ? m_a : m_b, m_want);
                std::snprintf(m_written, sizeof m_written, "%s", m_want);
                m_pend = true;
                m_at = engine::frame::Count();
                Show();
            }
            uint64_t m_a = 0, m_b = 0, m_at = 0;
            bool     m_shb = false, m_pend = false, m_vis = true;
            int      m_lead = 2;
            char     m_want[kRowBytes]{}, m_written[kRowBytes]{};
        };

        // ---- HUD leaves -------------------------------------------------------------------
        enum HLeaf {
            kBox, kBg, kBgI, kLbl, kT0, kT1, kT2, kU0, kU1, kU2,
            kL0, kL1, kL2, kL3, kL4, kFld, kFu0, kFu1, kFu2, kFtx, kFch, kHint, kIdl, kN
        };
        const char* const kHLeaf[kN] = {
            nullptr, "chBg", "chBgI", "chLbl", "chT0", "chT1", "chT2", "chU0", "chU1", "chU2",
            "chL0", "chL1", "chL2", "chL3", "chL4", "chFld", "chFu0", "chFu1", "chFu2",
            "chFtx", "chFch", "chHint", "chIdl"
        };

        uint64_t g_hudLeaf[kN]{};
        Twin     g_hudTwin[kN];
        bool     g_hudResolved = false;
        bool     IsTwin(int i) { return (i >= kL0 && i <= kL4) || i == kFtx; }

        uint64_t HudLeaf(const char* base, const char* leaf)
        {
            char rel[96];
            if (leaf) std::snprintf(rel, sizeof rel, "%s.%s", base, leaf);
            else      std::snprintf(rel, sizeof rel, "%s", base);
            uint64_t w = engine::widget::Hud(rel);
            if (w) return w;
            // Older widget paks had the box under currency-hud. Both resolve through
            // the shared seam; neither is written when the definitions are absent.
            if (leaf) std::snprintf(rel, sizeof rel, "chatHud.chatBox.%s", leaf);
            else      std::snprintf(rel, sizeof rel, "chatHud.chatBox");
            return engine::widget::Hud(rel);
        }

        void ResolveHud()
        {
            if (g_hudResolved) return;
            uint64_t box = engine::widget::Hud("t2rChat.chatBox");
            const char* base = "t2rChat.chatBox";
            if (!box) {
                box = engine::widget::Hud("chatHud.chatBox");
                base = "chatHud.chatBox";
            }
            if (!box) return;
            g_hudLeaf[kBox] = box;
            for (int i = 1; i < kN; i++) {
                g_hudLeaf[i] = HudLeaf(base, kHLeaf[i]);
                if (!g_hudLeaf[i] || !IsTwin(i)) continue;
                char twin[96];
                std::snprintf(twin, sizeof twin, "%sB", kHLeaf[i]);
                g_hudTwin[i].Bind(g_hudLeaf[i], HudLeaf(base, twin), "", false, 2);
            }
            g_hudResolved = true;
        }

        void HudText(int i, const char* s)
        {
            if (!g_hudLeaf[i]) return;
            if (IsTwin(i)) { g_hudTwin[i].Text(s); return; }
            engine::widget::SetText(g_hudLeaf[i], s && *s ? s : " ");
        }

        void HudVisible(int i, bool on)
        {
            if (!g_hudLeaf[i]) return;
            if (IsTwin(i)) g_hudTwin[i].Visible(on);
            else engine::widget::Visible(g_hudLeaf[i], on);
        }

        constexpr float kHLinePt = 40.0f, kHLineW = 1012.0f;
        constexpr float kHFldX = 36.0f, kHChR = 1040.0f, kHChPt = 28.0f;
        constexpr float kHTabsR = 1044.0f, kHTabPt = 30.0f, kHTabY = 44.0f;
        constexpr float kHUnderY = 70.0f, kHChGap = 28.0f, kHRuleW = 200.0f;
        constexpr float kHRowY0 = 116.0f, kHRowDy = 62.0f, kHIdleDy = 36.0f;

        float FieldWidth(float x0, float nameRight, float namePt, int tab)
        {
            const float w = nameRight - Width(TabName(tab), namePt) - kHChGap - x0;
            return w > 0.0f ? w : 0.0f;
        }

        void HudTick(uint64_t now, bool show)
        {
            if (!show) {
                if (g_hudLeaf[kBox]) HudVisible(kBox, false);
                return;
            }
            ResolveHud();
            if (!g_hudLeaf[kBox]) return;
            for (int i = kL0; i < kN; i++) if (IsTwin(i)) g_hudTwin[i].Tick();

            const bool typing = g_ed.on && g_where == 1;
            const int tab = g_store.tab;
            const uint64_t age = g_store.lastMs[tab] ? now - g_store.lastMs[tab] : ~0ULL;
            float a = typing ? 1.0f
                : age < kIdleMs ? 1.0f
                : age < kIdleMs + kFadeMs ? 1.0f - static_cast<float>(age - kIdleMs) / static_cast<float>(kFadeMs)
                : 0.0f;
            if (!typing && !g_store.n[tab]) a = 0.0f;
            const int q = static_cast<int>(a * 20.0f + 0.5f);
            engine::widget::Alpha(g_hudLeaf[kBox], q / 20.0f);
            HudVisible(kBox, q > 0);
            if (q <= 0) return;

            for (int i = kBg; i < kN; i++) {
                bool on = true;
                if (i == kBgI || i == kIdl) on = !typing;
                else if (i == kBg || i == kLbl || (i >= kT0 && i <= kT2)
                      || i == kFld || i == kFtx || i == kFch || i == kHint) on = typing;
                else if (i >= kU0 && i <= kU2) on = typing && i - kU0 == tab;
                else if (i >= kFu0 && i <= kFu2) on = typing && i - kFu0 == tab;
                else if (i == kL0 || i == kL1) on = typing;
                HudVisible(i, on);
            }

            const int nl = typing ? 5 : 3;
            Row rows[5];
            TabRows(tab, kHLineW * kFit, kHLinePt, rows + 5 - nl, nl);
            for (int k = 2; k < 5; k++) {
                const float y = kHRowY0 + kHRowDy * static_cast<float>(k)
                    + (typing ? 0.0f : kHIdleDy);
                if (g_hudTwin[kL0 + k].A()) engine::widget::Pivot(g_hudTwin[kL0 + k].A(), 32.0f, y);
                if (g_hudTwin[kL0 + k].B()) engine::widget::Pivot(g_hudTwin[kL0 + k].B(), 32.0f, y);
            }
            for (int k = 0; k < 5; k++) HudText(kL0 + k, k < 5 - nl ? "" : rows[k]);

            if (typing) {
                char names[kTabCount][96];
                TabsText(names);
                float x[kTabCount], w[kTabCount];
                TabsPack(names, kHTabsR, kHTabPt, x, w);
                for (int c = 0; c < kTabCount; c++) {
                    HudText(kT0 + c, names[c]);
                    if (g_hudLeaf[kT0 + c]) engine::widget::Pivot(g_hudLeaf[kT0 + c], x[c], kHTabY);
                }
                if (g_hudLeaf[kU0 + tab]) {
                    const float px = x[tab] - Width(names[tab], kHTabPt);
                    engine::widget::Pivot(g_hudLeaf[kU0 + tab], px, kHUnderY);
                    engine::widget::Scale(g_hudLeaf[kU0 + tab], w[tab] / kHRuleW, 1.0f);
                }
                HudText(kLbl, Pl() ? "CZAT" : "CHAT");
                char ed[kLineBytes + 96];
                EditorView(ed, sizeof ed, now,
                    FieldWidth(kHFldX, kHChR, kHChPt, tab), kHLinePt);
                HudText(kFtx, ed);
                char b[kRowBytes];
                std::snprintf(b, sizeof b, "<font color=%c%s%c>%s</font>",
                    34, kCol[tab], 34, TabName(tab));
                HudText(kFch, b);
                HudText(kHint, Pl()
                    ? "<font color=\"#E8E8E8\">ENTER</font> WYŚLIJ      <font color=\"#E8E8E8\">TAB</font> ZMIEŃ KANAŁ      <font color=\"#E8E8E8\">ESC</font> ZAMKNIJ"
                    : "<font color=\"#E8E8E8\">ENTER</font> SEND      <font color=\"#E8E8E8\">TAB</font> SWITCH TAB      <font color=\"#E8E8E8\">ESC</font> CLOSE");
            } else {
                char b[kRowBytes];
                std::snprintf(b, sizeof b, "<font color=%c%s%c>%s</font>",
                    34, kCol[tab], 34, TabName(tab));
                HudText(kIdl, b);
            }
        }

        // ---- ESC menu block ---------------------------------------------------------------
        constexpr float kPTabR = 1060.0f, kPTabPt = 25.5f, kPTabY = 30.0f;
        constexpr float kPUnderY = 58.0f, kPLinePt = 31.5f, kPLineW = 1004.0f;
        constexpr float kPFldPt = 27.0f, kPFldX = 34.0f, kPChR = 1034.0f, kPChPt = 24.0f;

        uint64_t g_pauseGrp = 0;
        Twin     g_pauseTwin[5];
        unsigned g_pauseUiGen = 0;

        void PageText(const char* rel, const char* s)
        {
            const uint64_t w = engine::widget::Page(rel);
            if (w) engine::widget::SetText(w, s && *s ? s : " ");
        }
        void PageVisible(const char* rel, bool on)
        {
            const uint64_t w = engine::widget::Page(rel);
            if (w) engine::widget::Visible(w, on);
        }
        void PagePivot(const char* rel, float x, float y)
        {
            const uint64_t w = engine::widget::Page(rel);
            if (w) engine::widget::Pivot(w, x, y);
        }

        bool PauseOwns() { return engine::flag::Absent("t2r-no-chat.txt"); }

        void PauseBind()
        {
            char p[80], pb[88];
            for (int k = 0; k < 5; k++) {
                std::snprintf(p, sizeof p, k < 4 ? "anim-group.pzChat.pcL%d" : "anim-group.pzChat.pcFtx", k);
                std::snprintf(pb, sizeof pb, "%sB", p);
                g_pauseTwin[k].Bind(engine::widget::Page(p), engine::widget::Page(pb),
                    "", true, 4);
            }
        }

        void PauseTick(uint64_t now, bool onPause)
        {
            const uint64_t g = onPause && PauseOwns() ? engine::widget::Page("anim-group.pzChat") : 0;
            if (!g) {
                g_pauseGrp = 0;
                if (g_where == 2) StopEditor("ESC menu left");
                return;
            }
            for (Twin& t : g_pauseTwin) t.Tick();
            const unsigned uiGen = static_cast<unsigned>(engine::frame::Count() >> 8);
            if (g != g_pauseGrp || uiGen != g_pauseUiGen) {
                g_pauseGrp = g;
                g_pauseUiGen = uiGen;
                PauseBind();
                PageText("anim-group.pzChat.pcSec", Pl() ? "CZAT" : "CHAT");
                engine::widget::Visible(g, true);
            }
            const bool typing = g_ed.on && g_where == 2;
            const int tab = g_store.tab;
            char names[kTabCount][96];
            TabsText(names);
            float x[kTabCount], w[kTabCount];
            TabsPack(names, kPTabR, kPTabPt, x, w);
            for (int c = 0; c < kTabCount; c++) {
                char p[80];
                std::snprintf(p, sizeof p, "anim-group.pzChat.pcT%d", c);
                PageText(p, names[c]);
                PagePivot(p, x[c], kPTabY);
                std::snprintf(p, sizeof p, "anim-group.pzChat.pcU%d", c);
                PageVisible(p, c == tab);
                std::snprintf(p, sizeof p, "anim-group.pzChat.pcFu%d", c);
                PageVisible(p, c == tab);
            }
            Row rows[4];
            TabRows(tab, kPLineW * kFit, kPLinePt, rows, 4);
            for (int k = 0; k < 4; k++) g_pauseTwin[k].Text(rows[k]);
            if (typing) {
                char b[kLineBytes + 96];
                EditorView(b, sizeof b, now,
                    FieldWidth(kPFldX, kPChR, kPChPt, tab), kPFldPt);
                g_pauseTwin[4].Text(b);
            } else {
                g_pauseTwin[4].Text(Pl()
                    ? "<font color=\"#6E7074\">ENTER - PISZ,  TAB - ZMIEŃ</font>"
                    : "<font color=\"#6E7074\">ENTER to type,  TAB switch</font>");
            }
            char b[kRowBytes];
            std::snprintf(b, sizeof b, "<font color=%c%s%c>%s</font>",
                34, kCol[tab], 34, TabName(tab));
            PageText("anim-group.pzChat.pcFch", b);

            // P FRIENDS unread badge: the ext's prompt layout is owned by clancard; the
            // native port only drives the authored badge leaves when they exist.
            if (net::friends13::Enabled()) {
                const int unread = net::friends13::UnreadTotal();
                PageVisible("anim-group.pzPBadge", unread > 0);
                if (unread > 0) {
                    char t[8];
                    std::snprintf(t, sizeof t, "%d", std::min(unread, 9));
                    PageText("anim-group.pzPBadge.pzPBadgeTx", t);
                }
            } else PageVisible("anim-group.pzPBadge", false);
        }

        // ---- MP / FIND MATCH panel ---------------------------------------------------------
        constexpr float kMTabR = 1126.0f, kMTabPt = 30.0f;
        constexpr float kMLinePt = 30.0f, kMLineW = 1070.0f;
        constexpr float kMHdrH = 88.0f, kMUnderY = kMHdrH / 2.0f + 24.0f;

        uint64_t g_menuGrp = 0;
        Twin     g_menuTwin[5];
        unsigned g_menuUiGen = 0;
        int      g_menuTab = Channel::kParty;

        void MenuBind()
        {
            char p[80], pb[88];
            for (int k = 0; k < 5; k++) {
                std::snprintf(p, sizeof p, k < 4 ? "anim-group.mnChat.mcL%d" : "anim-group.mnChat.mcFtx", k);
                std::snprintf(pb, sizeof pb, "%sB", p);
                g_menuTwin[k].Bind(engine::widget::Page(p), engine::widget::Page(pb),
                    "", true, 4);
            }
        }

        void MenuTicks(uint64_t now, bool inMatch)
        {
            const uint64_t top = engine::widget::TopPage();
            const bool enabled = !inMatch && engine::flag::Absent("t2r-no-chat.txt")
                && (top == engine::widget::PageId("main-menu")
                    || top == engine::widget::PageId("find-match-menu"));
            const uint64_t g = enabled ? engine::widget::Page("anim-group.mnChat") : 0;
            g_menuUp = g != 0;
            const bool foc = engine::window::Focused();
            const bool party = g_menuUp && InParty();
            const bool lobby = g_menuUp && InLobby();

            // Enter is consumed here while the panel can open, exactly like the ext's
            // Enter-only catcher: SPACE / the pad still select the menu row.
            const bool enter = engine::text_input::TakeEdge(VK_RETURN);
            if (g_ed.on && g_where == 3) {
                if (!enabled) StopEditor("MP menu left");
                else if (!foc) StopEditor("game lost focus");
                else if (!party && !lobby) StopEditor("not in a party or a lobby any more");
                else HandleEditor(now);
            } else if (enter && (party || lobby)) {
                g_store.tab = g_menuTab == Channel::kLobby && lobby ? Channel::kLobby
                    : party ? Channel::kParty : Channel::kLobby;
                StartEditor(3, now);
            }
            if (!g) { g_menuGrp = 0; return; }

            static bool wasLobby = false;
            if (g_ed.on && g_where == 3)
                g_menuTab = g_store.tab == Channel::kLobby && lobby ? Channel::kLobby : Channel::kParty;
            else if (lobby != wasLobby) g_menuTab = lobby ? Channel::kLobby : Channel::kParty;
            else if (!lobby) g_menuTab = Channel::kParty;
            wasLobby = lobby;

            for (Twin& t : g_menuTwin) t.Tick();
            const unsigned uiGen = static_cast<unsigned>(engine::frame::Count() >> 8);
            if (g != g_menuGrp || uiGen != g_menuUiGen) {
                g_menuGrp = g;
                g_menuUiGen = uiGen;
                MenuBind();
                PageText("anim-group.mnChat.mcHdr", Pl() ? "CZAT" : "CHAT");
                engine::widget::Visible(g, true);
            }

            const float r = kMTabR;
            const float wl = Width(TabName(Channel::kLobby), kMTabPt);
            const float wp = Width(TabName(Channel::kParty), kMTabPt);
            const float rp = lobby ? r - wl - kTabGap * kMTabPt : r;
            char t[160];
            std::snprintf(t, sizeof t, "<font color=%c%s%c>%s</font>", 34,
                g_menuTab == Channel::kParty ? kCol[Channel::kParty] : "#6E7074", 34,
                TabName(Channel::kParty));
            PageText("anim-group.mnChat.mcT0", t);
            PagePivot("anim-group.mnChat.mcT0", rp, kMHdrH / 2.0f);
            PageVisible("anim-group.mnChat.mcU0", g_menuTab == Channel::kParty);
            PageVisible("anim-group.mnChat.mcT1", lobby);
            PageVisible("anim-group.mnChat.mcU1", lobby && g_menuTab == Channel::kLobby);
            if (lobby) {
                std::snprintf(t, sizeof t, "<font color=%c%s%c>%s</font>", 34,
                    g_menuTab == Channel::kLobby ? kCol[Channel::kLobby] : "#6E7074", 34,
                    TabName(Channel::kLobby));
                PageText("anim-group.mnChat.mcT1", t);
            }
            if (g_menuTab == Channel::kLobby)
                PagePivot("anim-group.mnChat.mcU1", r - wl, kMUnderY);
            else
                PagePivot("anim-group.mnChat.mcU0", rp - wp, kMUnderY);

            const bool typing = g_ed.on && g_where == 3;
            Row rows[4];
            TabRows(g_menuTab, kMLineW * kFit, kMLinePt, rows, 4);
            for (int k = 0; k < 4; k++) g_menuTwin[k].Text(rows[k]);
            PageVisible("anim-group.mnChat.mcFu", typing);
            if (typing) {
                char b[kLineBytes + 96];
                EditorView(b, sizeof b, now, FieldWidth(kPFldX, kPChR, kPChPt, g_menuTab), kMLinePt);
                g_menuTwin[4].Text(b);
                std::snprintf(b, sizeof b, "<font color=%c%s%c>%s</font>",
                    34, kCol[g_menuTab], 34, TabName(g_menuTab));
                PageText("anim-group.mnChat.mcFch", b);
            } else {
                if (g_menuTab == Channel::kLobby)
                    g_menuTwin[4].Text(Pl() ? "<font color=\"#6E7074\">ENTER - PISZ DO LOBBY</font>"
                                            : "<font color=\"#6E7074\">ENTER to chat with the lobby</font>");
                else if (party)
                    g_menuTwin[4].Text(Pl() ? "<font color=\"#6E7074\">ENTER - PISZ DO GRUPY</font>"
                                            : "<font color=\"#6E7074\">ENTER to chat with your party</font>");
                else
                    g_menuTwin[4].Text(Pl() ? "<font color=\"#6E7074\">BRAK GRUPY - P ZNAJOMI, BY ZAPROSIĆ</font>"
                                            : "<font color=\"#6E7074\">NO PARTY - P FRIENDS TO INVITE</font>");
                PageText("anim-group.mnChat.mcFch", "");
            }
        }

        void ClearCaches()
        {
            for (int i = 0; i < kN; i++) g_hudTwin[i].Reset();
            for (Twin& t : g_pauseTwin) t.Reset();
            for (Twin& t : g_menuTwin) t.Reset();
            std::memset(g_hudLeaf, 0, sizeof g_hudLeaf);
            g_hudResolved = false;
            g_pauseGrp = g_menuGrp = 0;
            g_pauseUiGen = g_menuUiGen = 0;
        }

    } // namespace

    // ---- public API -----------------------------------------------------------------------
    void Tick(uint64_t now)
    {
        const bool inMatch = net::match::liveness::Live();
        static bool wasIn = false;
        if (inMatch != wasIn) {
            wasIn = inMatch;
            if (inMatch) {
                ClearMatchLines();
                g_receiver.Reset();
            } else {
                if (g_where == 3) StopEditor("match over");
            }
        }

        g_resend.Tick(now, [](const core::WirePacket& p) {
            if (net::match::liveness::Live())
                net::session::SendEventRaw(static_cast<uint8_t>(NetEventType::NEVT_CHAT_C7),
                    p.data, p.size);
        });
        NetTick(now, inMatch);

        const bool enabled = engine::flag::Absent("t2r-no-chat.txt");
        if (!enabled || !inMatch) {
            if (g_where != 3) StopEditor("chat off / not in a match");
            HudTick(now, false);
            MenuTicks(now, inMatch);   // the menu panel also arms / drops Enter
            return;
        }
        if (g_where == 3) StopEditor("match started");
        g_menuUp = false;
        if (g_store.tab >= kTabCount) g_store.tab = 0;

        const uint64_t top = engine::widget::TopPage();
        const bool onPause = top == engine::widget::PageId("pause-menu");
        // The box stays above the death / end screens; it never hides or edits them.
        const bool hud = !net::gui2::MenuUp()
            || (net::match::liveness::MatchEndHold() && !onPause
                && !net::friends13::PanelOpen());
        const bool foc = engine::window::Focused();

        const bool enter = engine::text_input::TakeEdge(VK_RETURN);
        if (g_ed.on) {
            if (!foc) StopEditor("game lost focus");
            else if ((g_where == 1 && !hud) || (g_where == 2 && !onPause)) StopEditor("page changed");
            else HandleEditor(now);
        } else if (enter && (hud || onPause)) {
            StartEditor(hud ? 1 : 2, now);
        }
        if (onPause && !g_ed.on && foc) {
            const bool p = engine::text_input::TakeEdge('P');
            if (p && !engine::text_input::Down('E') && net::friends13::Enabled()) {
                Sfx("sfx-select");
                net::friends13::OpenFromPause();
            }
        }
        HudTick(now, hud);
        PauseTick(now, onPause);
    }

    void NotePeer(uint8_t slot, const uint8_t* data, uint8_t size)
    {
        if (!engine::flag::Absent("t2r-no-chat.txt")) return;
        core::Reassembled line;
        if (!g_receiver.Accept(slot, data, size, line)) return;
        const bool mate = NetPlayers::SlotIsFriendly(slot);
        if (line.channel == Channel::kTeam && !mate) {
            TO1_LOG("chat: TEAM line from enemy slot %u dropped\n", static_cast<unsigned>(slot));
            return;
        }
        const NetPlayer& p = NetPlayers::BySlot(slot);
        char name[40];
        core::Clean(p.name[0] ? p.name : "PLAYER", std::strlen(p.name[0] ? p.name : "PLAYER"),
            name, sizeof name, 24);
        AddLine(line.channel, name, NameColor(name, false, mate ? "#48A8D8" : "#E04848"),
            line.text, Now());
        TO1_LOG("chat: %s line from slot %u (%s)\n", kTabEn[line.channel],
            static_cast<unsigned>(slot), name);
    }

    void Reset()
    {
        g_store = Store{};
        g_ed = Editor{};
        g_where = 0;
        engine::text_input::End(engine::text_input::Owner::Chat);
        g_receiver.Reset();
        g_resend.Reset();
        g_seq = 0;
        g_partyLast = g_lobbyLast = g_lobbyKey = -1;
        g_pollAt = g_lobbyPollAt = 0;
        g_busy.store(0);
        g_have.store(0);
        g_replyLen.store(0);
        g_job = Job{};
        g_reply[0] = 0;
        g_queue.fill(Queued{});
        g_queueN = 0;
        g_menuUp = false;
        g_menuTab = Channel::kParty;
        ClearCaches();
    }

} // namespace net::chat
