// Native port of tools/ext/achieve.c + achieve_ui.c (build 191). Ported with
// the ext's exact wire bytes, counters, table format and card timings; the
// pure decision / parse / state code lives in achieve-core.hpp so it is
// testable without Windows. Camera-less, PCM-less and widget-less builds keep
// the state machine running but make every visual / sound seam inert.

#include "achieve.hpp"

#include <winsock2.h>
#include <windows.h>

#include "boot.hpp"
#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/locale.hpp"
#include "engine/menu-input.hpp"
#include "log.hpp"             // TO1_LOG must precede engine/script.hpp
#include "engine/script.hpp"   // AUDIO::PLAY_SOUND / MENU2
#include "engine/widget.hpp"
#include "net/base/net-account.hpp"
#include "net/match/match-liveness.hpp"
#include "net/online/daemon-rpc.hpp"
#include "net/protocol/net-protocol.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace net::ui::achieve {

    namespace {

        using namespace core;

        constexpr size_t kTokenLen = 64;
        constexpr int    kReplyCap = 65536;
        constexpr int    kConnMs   = 2500;
        constexpr int    kIoMs     = 3500;
        constexpr int    kReplyMs  = 4000;

        struct Runtime {
            Table     table;
            int       cnt[kLabN]{};

            uint8_t   rep[kPayloadCap]{};
            int       repLen = 0;
            uint32_t  seq = 0;
            int       repTries = 0;
            uint64_t  repAt = 0;
            bool      repPend = false;
            bool      endLatched = false;

            uint64_t  tableAt = 0;

            uint8_t   liveTries[kLiveN]{};
            uint64_t  liveAt[kLiveN]{};
            uint16_t  liveBase[kLabN]{};
            uint64_t  liveNext = 0;

            UiState   ui;
            CardState card;
            uint32_t  seenT = kSeenUnread;
            bool      seenUi = false;
            bool      seenLoaded = false;   // the sidecar is a one-time baseline, not a live store

            uint64_t  page = 0;
            uint64_t  lastTop = 0;
            uint64_t  cardLast = 0;
            bool      built = false;
            int       hvR = -1, hvC = -1, hvTab = -1, hvPb = -1;
            bool      hUp = true, hDown = true, hLeft = true, hRight = true;
            bool      hPrev = true, hNext = true, hSel = true, hBack = true;
            uint64_t  tUp = 0, tDown = 0, tLeft = 0, tRight = 0;
        } g;

        bool Disabled()
        {
            return boot::ExtLoaded() || engine::flag::Present("t2r-no-achievements.txt");
        }

        // Read-only compatibility baseline: loaded once. Re-reading a stale
        // sidecar after g.seenT advances would re-queue the same unlock cards.
        // No file leaves kSeenUnread; the first table then baselines itself.
        void ReadSeen()
        {
            if (g.seenLoaded) return;
            g.seenLoaded = true;
            if (FILE* f = engine::exe_side::Open("t2r-ach-seen.txt", "r")) {
                unsigned v = 0;
                if (std::fscanf(f, "%u", &v) == 1) g.seenT = v;
                std::fclose(f);
            }
            if (FILE* f = engine::exe_side::Open("t2r-ach-ui-seen.txt", "r")) {
                unsigned v = 0;
                g.seenUi = std::fscanf(f, "%u", &v) == 1 && v != 0;
                std::fclose(f);
            }
        }

        // Every successful table reply (0xAA / 0xA9 / 0xAB) runs the ext's
        // ach_parse path: baseline on the first table, then queue unlocks/cards.
        bool ApplyTable(const char* text)
        {
            Table t;
            bool live = false;
            if (!ParseTable(text, t, live)) return false;
            g.table = t;
            Toast q[kToastQ];
            int dropped = 0;
            uint32_t newest = 0;
            const int n = QueueNew(t, g.seenT, q, kToastQ, newest, &dropped);
            if (g.seenT == kSeenUnread) {
                g.seenT = newest;   // no sidecar: first table is the baseline
            } else {
                if (newest > g.seenT) g.seenT = newest;
                for (int i = 0; i < n; ++i) EnqueueCard(g.card, q[i]);
            }
            g.built = false;        // rebuild on the next page tick
            (void)dropped;
            return true;
        }

        // One bounded [u16 len][op][payload] RPC, byte-for-byte the ext frame.
        // `want` is the opcode whose payload is returned as text (or a table).
        bool Ask(uint8_t op, const uint8_t* pl, int plen, uint8_t want,
                 char* reply, int cap)
        {
            sockaddr_in addr{};
            if (!net::daemon::Addr(&addr) || !net::account::HaveToken())
                return false;
            const size_t tok = std::strlen(net::account::Token());
            if (tok != kTokenLen || plen < 0 || 1 + plen > 4096 - 3)
                return false;
            uint8_t frame[4096];
            const int body = 1 + plen;
            frame[0] = static_cast<uint8_t>(body);
            frame[1] = static_cast<uint8_t>(body >> 8);
            frame[2] = op;
            std::memcpy(frame + 3, net::account::Token(), kTokenLen);
            if (plen > 0) std::memcpy(frame + 3 + kTokenLen, pl, static_cast<size_t>(plen));

            char buf[kReplyCap];
            int sent = 0;
            const int got = net::daemon::Rpc(addr, frame, body + 2, buf,
                                             cap < kReplyCap ? cap : kReplyCap,
                                             kConnMs, kIoMs, kReplyMs, &sent);
            if (got < 3 || !sent) return false;
            const int len = static_cast<uint8_t>(buf[0]) + (static_cast<uint8_t>(buf[1]) << 8);
            if (len < 1 || len + 2 > got || static_cast<uint8_t>(buf[2]) != want)
                return false;
            const int ntext = std::min(len - 1, cap - 1);
            std::memcpy(reply, buf + 3, static_cast<size_t>(ntext));
            reply[ntext] = 0;
            net::daemon::MarkAlive();
            return true;
        }

        bool AskTable(uint64_t now)
        {
            char text[kReplyCap];
            if (!Ask(static_cast<uint8_t>(NetDaemonOp::kAchTable), nullptr, 0,
                     static_cast<uint8_t>(NetDaemonOp::kAchTable), text, sizeof text))
                return false;
            const bool ok = ApplyTable(text);
            (void)now;
            return ok;
        }

        void ArmReport(uint64_t now)
        {
            if (g.endLatched) return;
            g.endLatched = true;
            g.repPend = true;
            g.repTries = 0;
            g.repLen = 0;
            g.repAt = now + kReportFirstMs;   // after the 0xE2 match report
        }

        void TickReport(uint64_t now)
        {
            if (!g.repPend || now < g.repAt) return;
            uint16_t mis[kLabN]{};            // mission counters: no native producer yet
            if (g.repLen == 0)
                g.repLen = BuildReport(g.cnt, mis, kLabN, g.seq, g.rep, sizeof g.rep);
            char reply[kReplyCap];
            const bool ok = Ask(static_cast<uint8_t>(NetDaemonOp::kAchReport),
                                g.rep, g.repLen,
                                static_cast<uint8_t>(NetDaemonOp::kAchReport),
                                reply, sizeof reply);
            if (ok) {
                ApplyTable(reply);
                g.repPend = false;
                g.repLen = 0;
                ++g.seq;
                g.endLatched = false;
                return;
            }
            ++g.repTries;
            if (g.repTries >= kReportMaxTries) {
                g.repPend = false;
                g.endLatched = false;
                TO1_LOG("ach: 0xA9 gave up after %d tries\n", g.repTries);
                return;
            }
            // Failed connect retries quickly; an unanswered send waits the
            // ext's full 8 s before the next attempt.
            g.repAt = now + (g.repTries == 1 ? kReportFailMs : kReportRetryMs);
        }

        void TickLive(uint64_t now)
        {
            if (!g.table.live || !net::match::liveness::Live() || now < g.liveNext)
                return;
            g.liveNext = now + kLivePeriodMs;
            uint16_t mis[kLabN]{};
            const int i = NextLive(g.table, g.cnt, mis, kLabN, g.liveBase,
                                   g.liveTries, g.liveAt, now);
            if (i < 0) return;
            uint8_t pl[kPayloadCap];
            const int len = BuildLive(g.cnt, mis, kLabN, pl, sizeof pl);
            char reply[kReplyCap];
            const bool ok = Ask(static_cast<uint8_t>(NetDaemonOp::kAchLive), pl, len,
                                static_cast<uint8_t>(NetDaemonOp::kAchLive),
                                reply, sizeof reply);
            g.liveTries[i] = ok ? kLiveMaxTries : static_cast<uint8_t>(g.liveTries[i] + 1);
            g.liveAt[i] = now;   // core gates the next try by kLiveRetryMs
            if (!ok) return;
            for (int k = 0; k < kLabN; ++k)
                g.liveBase[k] = g.cnt[k] > 65535 ? 65535 : static_cast<uint16_t>(std::max(0, g.cnt[k]));
            ApplyTable(reply);
        }

        void Text(const char* rel, const char* text)
        {
            const uint64_t w = engine::widget::Page(rel);
            if (w) engine::widget::SetText(w, engine::locale::Tr(text ? text : ""));
        }

        void Vis(const char* rel, bool on)
        {
            const uint64_t w = engine::widget::Page(rel);
            if (w) engine::widget::Visible(w, on);
        }

        void Vis(uint64_t w, bool on)
        {
            if (w) engine::widget::Visible(w, on);
        }

        void DrawGrid()
        {
            if (!g.built) {
                BuildGroups(g.ui, g.table);
                g.built = true;
            }
            const uint64_t top = engine::widget::TopPage();
            if (top != g.lastTop) {
                g.lastTop = top;
                g.hUp = g.hDown = g.hLeft = g.hRight = true;
                g.hPrev = g.hNext = g.hSel = g.hBack = true;
            }
            const bool up = engine::menu_input::CommandEdge(SID("menu-up"), g.hUp);
            const bool down = engine::menu_input::CommandEdge(SID("menu-down"), g.hDown);
            const bool left = engine::menu_input::CommandEdge(SID("menu-left"), g.hLeft);
            const bool right = engine::menu_input::CommandEdge(SID("menu-right"), g.hRight);
            const bool prev = engine::menu_input::CommandEdge(SID("menu-prev-tab"), g.hPrev);
            const bool next = engine::menu_input::CommandEdge(SID("menu-next-tab"), g.hNext);
            const bool sel = engine::menu_input::CommandEdge(SID("menu-select"), g.hSel);
            engine::menu_input::CommandEdge(SID("menu-back"), g.hBack);
            if (up) MoveRow(g.ui, -1);
            if (down) MoveRow(g.ui, 1);
            if (left) MoveCol(g.ui, -1);
            if (right) MoveCol(g.ui, 1);
            if (sel) ClampFocus(g.ui);
            (void)prev; (void)next;

            // The group headers and tiles are only written when the GUI pak
            // carries them; a missing widget is simply skipped.
            for (int r = 0; r < kRows; ++r) {
                const int gr = g.ui.row0 + r;
                const bool have = gr < g.ui.groups;
                char rel[64];
                std::snprintf(rel, sizeof rel, "anim-group.rows.rw%dRt", r);
                Vis(rel, have);
                if (have) Text(rel, g.ui.g[gr].label);
                for (int c = 0; c < kCols; ++c) {
                    std::snprintf(rel, sizeof rel, "anim-group.rows.t%d_%d", r, c);
                    Vis(rel, have && c < g.ui.g[gr].n);
                    if (!have || c >= g.ui.g[gr].n) continue;
                    const int idx = FocusedIndex(g.ui);
                    const Row& row = g.table.rows[g.ui.g[gr].item[c]];
                    char buf[160];
                    std::snprintf(buf, sizeof buf, "%s", row.title);
                    Text(rel, buf);
                    std::snprintf(rel, sizeof rel, "anim-group.rows.t%d_%dT", r, c);
                    Vis(rel, row.t != 0);
                    (void)idx;
                }
            }

            const int idx = FocusedIndex(g.ui);
            if (idx >= 0) {
                const Row& row = g.table.rows[idx];
                Text("anim-group.det.dGrp", row.group);
                Text("anim-group.det.dTitle", row.title);
                Text("anim-group.det.dDesc", row.desc);
                char cnt[64];
                std::snprintf(cnt, sizeof cnt, "%d | %d", row.prog, row.goal);
                Text("anim-group.det.dCnt", cnt);
            }
        }

        // achc_w in achieve_ui.c: the card lives on the match HUD while a match
        // runs, otherwise under anim-group on one of four menu pages. Nowhere
        // else: the timer stands still when none of those places is on top.
        constexpr const char* kCardPageNames[kCardPages] = {
            "main-menu", "find-match-menu", "clan13x", "clanach"
        };

        uint64_t CardWidget(const char* rel, bool hud)
        {
            char p[128];
            std::snprintf(p, sizeof p, "%s%s", hud ? "achNt" : "anim-group.achNt", rel);
            return hud ? engine::widget::Hud(p) : engine::widget::Page(p);
        }

        int CardPlace(bool& hud)
        {
            hud = false;
            if (net::match::liveness::Live()) {
                hud = CardWidget("", true) != 0;
                if (hud) return kCardPages;
            }
            const uint64_t top = engine::widget::TopPage();
            if (!top) return -1;
            for (int i = 0; i < kCardPages; ++i)
                if (top == engine::widget::PageId(kCardPageNames[i])) return i;
            return -1;
        }

        void WriteCard(bool hud)
        {
            if (g.card.n <= 0) return;
            const CardItem& it = g.card.q[0];
            char name[256];
            std::snprintf(name, sizeof name, "Unlocked Achievement: %s",
                          engine::locale::Tr(it.title));
            const uint64_t nw = CardWidget(".achNtFd.achNtGr.achNtTg.achNtName", hud);
            if (nw) {
                const float tw = engine::widget::TextWidth(name, 41.0f);
                const float sc = std::clamp(1000.0f / std::max(tw, 1.0f), 0.6f, 1.0f);
                engine::widget::Scale(nw, sc, sc);
                engine::widget::SetText(nw, name);
                g.card.tw = std::min(tw, 1000.0f);
            }
            const uint64_t aw = CardWidget(".achNtFd.achNtGr.achNtTg.achNtAct", hud);
            if (aw) {
                const char* desc = engine::locale::Tr(it.desc);
                engine::widget::SetText(aw, desc);
                g.card.tw = std::max(g.card.tw, std::min(engine::widget::TextWidth(desc, 41.0f), 740.0f));
            }
            g.card.tw = std::max(1.0f, kCardTextK * g.card.tw);
            char img[128];
            std::snprintf(img, sizeof img, "streaming/t2r/ach/%s.png", it.id);
            const uint64_t iw = CardWidget(".achNtFd.achNtGr.achNtIc", hud);
            if (iw) engine::widget::SetImage(iw, img);
        }

        void DrawCard(uint64_t now)
        {
            bool hud = false;
            const int place = CardPlace(hud);
            const uint64_t root = CardWidget("", hud);
            const uint64_t dt = g.cardLast ? now - g.cardLast : 0;
            g.cardLast = now;
            if (place < 0) {
                if (g.card.active) { g.card.t0 += dt; Vis(root, false); }
                return;
            }
            if (!g.card.active) {
                if (g.card.n <= 0 || !root) return;
                StartCard(g.card, now, place);
                Vis(root, false);
                WriteCard(hud);          // written while hidden, shown next frame
                g.card.t0 += dt;
                return;
            }
            if (!root) { g.card.t0 += dt; return; }
            if (g.card.page != place) {
                Vis(root, false);
                WriteCard(hud);
                g.card.page = place;
                g.card.t0 += dt;
                return;
            }
            const float a = CardAlpha(now, g.card.t0);
            const float slide = CardSlide(now, g.card.t0);
            const float left = kCardIconL, right = kCardTextL + g.card.tw;
            const float cc = (left + right) * 0.5f;
            const uint64_t bg = CardWidget(".achNtFd.achNtBg", hud);
            if (bg) {
                const float bw = std::max(900.0f, (right - left) / 0.6f);
                engine::widget::Scale(bg, bw / kCardBoxW, 1.0f);
                engine::widget::Pivot(bg, 32.0f + cc - kCardBoxW * 0.5f, 0.0f);
            }
            engine::widget::Scale(root, kCardS, kCardS);
            engine::widget::Pivot(root, kCardCX - kCardS * cc - 90.0f * slide, kCardY);
            engine::widget::Alpha(root, a);
            Vis(root, true);
            if (!g.card.snd) {
                g.card.snd = true;
                AUDIO::PLAY_SOUND(SID("sfx-select"));
            }
            if (AdvanceCard(g.card, now)) {
                Vis(root, false);
                g.card.snd = false;
                if (g.card.active) WriteCard(hud);
            }
        }

    } // namespace

    void Note(int label, int amount)
    {
        if (Disabled() || !net::match::liveness::Live()) return;
        ::net::ui::achieve::core::Note(g.cnt, label, amount);
    }

    void Tick(uint64_t now)
    {
        if (Disabled()) return;
        ReadSeen();

        // Match end -> the 1.5 s-delayed 0xA9 report.
        if (net::match::liveness::MatchEndHold()) ArmReport(now);
        else if (!net::match::liveness::Live()) g.endLatched = false;
        TickReport(now);

        const uint64_t page = engine::widget::PageId("clanach");
        g.page = page;
        const bool onPage = engine::widget::TopPage() == page;
        if (onPage && now >= g.tableAt) {
            g.tableAt = AskTable(now) ? now + kPageAskMs : now + kAskRetryMs;
        } else if (!onPage) {
            g.tableAt = 0;
        }
        TickLive(now);
        if (onPage) DrawGrid();
        DrawCard(now);
    }

    bool Active() { return !Disabled(); }
    const core::Table& CurrentTable() { return g.table; }

    void Reset()
    {
        g = Runtime{};
        g.seenT = kSeenUnread;
    }

} // namespace net::ui::achieve
