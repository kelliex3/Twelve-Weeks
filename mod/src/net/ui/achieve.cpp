// Native port of tools/ext/achieve.c + achieve_ui.c (build 191). Ported with
// the ext's exact wire bytes, counters, table format and card timings; the
// pure decision / parse / state code lives in achieve-core.hpp so it is
// testable without Windows. Camera-less, PCM-less and widget-less builds keep
// the state machine running but make every visual / sound seam inert.

#include "achieve.hpp"

#include "customize.hpp"

#include <winsock2.h>
#include <windows.h>

#include "boot.hpp"
#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/locale.hpp"
#include "engine/menu-input.hpp"
#include "log.hpp"             // TO1_LOG must precede engine/script.hpp
#include "engine/script.hpp"   // AUDIO::PLAY_SOUND / MENU2
#include "engine/widget.hpp"
#include "engine/window.hpp"
#include "net/base/net-account.hpp"
#include "net/match/match-liveness.hpp"
#include "net/online/daemon-rpc.hpp"
#include "net/online/clan-state.hpp"
#include "net/protocol/net-protocol.hpp"

#include <algorithm>
#include <ctime>
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

        // nrui.c / nr_style.py: only the pieces the ACHIEVEMENTS page writes.
        constexpr float kSectionPt  = 36.0f;
        constexpr float kCountPt    = 33.0f;
        constexpr float kHeadPt     = 54.0f;
        constexpr float kTabPt      = 36.0f;
        constexpr float kTabY       = 270.0f;
        constexpr float kChipPt     = 30.0f;
        constexpr float kBgPt       = 42.0f;
        constexpr float kPromptPt   = 36.0f;
        constexpr float kPromptKeyPt = 24.0f;
        constexpr float kPanelW0    = 100.0f;
        constexpr float kPromptW0   = 400.0f;

        constexpr int kMeTab = 5;  // nr_clan_tab_of_page("clanach")
        static const char* const kClanTabs[6] = {
            "OVERVIEW", "SURVIVORS", "RAIDS", "JOURNAL", "MISSIONS", "ACHIEVEMENTS"
        };

        struct PromptItem {
            const char* key;
            const char* label;
            bool on;
        };

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
            bool      seenLoaded = false;

            // t2r-ach-ui-seen.txt is the UI NEW-dot baseline: read on each
            // page open, written on leave. It is data, not an opt-in toggle.
            uint32_t  uiSeenT = 0;
            bool      uiEntered = false;
            uint64_t  f0 = 0;
            uint64_t  uiKey = 0;
            int       hoverSig = -1;
            bool      lastPad = false;
            float     pbX0[3]{}, pbX1[3]{};
            int       pbN = 0;

            uint64_t  cardLast = 0;
            bool      built = false;
            bool      hUp = true, hDown = true, hLeft = true, hRight = true;
            bool      hPrev = true, hNext = true, hSel = true, hBack = true;
            bool      hEsc = true, hF = true, hQ = true, hE = true;
            uint64_t  tUp = 0, tDown = 0, tLeft = 0, tRight = 0;
        } g;

        bool Disabled()
        {
            return boot::ExtLoaded() || engine::flag::Present("t2r-no-achievements.txt");
        }
        uint64_t Page() { return engine::widget::PageId("clanach"); }

        void ResetHeld()
        {
            g.hUp = g.hDown = g.hLeft = g.hRight = true;
            g.hPrev = g.hNext = g.hSel = g.hBack = true;
            g.hEsc = g.hF = g.hQ = g.hE = true;
            g.tUp = g.tDown = g.tLeft = g.tRight = ~uint64_t{ 0 };
            g.f0 = engine::frame::Count();
        }

        // The card baseline is read once: re-reading a stale sidecar after
        // g.seenT advances would re-queue the same unlock cards.
        void ReadCardSeen()
        {
            if (g.seenLoaded) return;
            g.seenLoaded = true;
            if (FILE* f = engine::exe_side::Open("t2r-ach-seen.txt", "r")) {
                unsigned v = 0;
                if (std::fscanf(f, "%u", &v) == 1) g.seenT = v;
                std::fclose(f);
            }
        }

        void ReadUiSeen()
        {
            g.uiSeenT = 0;
            if (FILE* f = engine::exe_side::Open("t2r-ach-ui-seen.txt", "r")) {
                unsigned v = 0;
                if (std::fscanf(f, "%u", &v) == 1) g.uiSeenT = v;
                std::fclose(f);
            }
        }

        void SaveUiSeen()
        {
            uint32_t newest = g.uiSeenT;
            for (int i = 0; i < g.table.n; ++i)
                newest = std::max(newest, g.table.rows[i].t);
            if (newest == g.uiSeenT) return;
            if (FILE* f = engine::exe_side::Open("t2r-ach-ui-seen.txt", "w")) {
                std::fprintf(f, "%u\n", newest);
                std::fclose(f);
            }
            g.uiSeenT = newest;
        }

        void EnterPage()
        {
            g.uiEntered = true;
            ReadUiSeen();
            ResetHeld();
            g.uiKey = 0;
            g.hoverSig = -1;
            g.built = false;
            g.ui.hvR = g.ui.hvC = g.ui.hvTab = g.ui.hvPb = -1;
            TO1_LOG("ach: clanach page up (ui seen %u)\n", g.uiSeenT);
        }

        void LeavePage(const char* how, bool pop)
        {
            (void)how;
            if (!g.uiEntered) return;
            g.uiEntered = false;
            SaveUiSeen();
            if (pop && engine::widget::TopPage() == Page())
                DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
            TO1_LOG("ach: clanach left (%s)\n", how);
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
            g.liveAt[i] = now;
            if (!ok) return;
            for (int k = 0; k < kLabN; ++k)
                g.liveBase[k] = g.cnt[k] > 65535 ? 65535 : static_cast<uint16_t>(std::max(0, g.cnt[k]));
            ApplyTable(reply);
        }

        // ---- the nrui.c frame the ext builds over the clanach page ------------

        void Header(const net::clan::State& c)
        {
            const char* parent = engine::locale::Tr(c.ok && c.fac == 2 ? "SERAPHITE CLAN" : "WLF CLAN");
            const char* page = engine::locale::Tr("ACHIEVEMENTS");
            customize::ui::RawText("anim-group.hdPar", parent);
            const float x = kX0 + customize::ui::TextWidth(parent, kHeadPt) + 52.0f;
            customize::ui::Pivot("anim-group.hdDiv", x, 168.0f - 34.0f);
            customize::ui::Pivot("anim-group.hdPage", x + 52.0f, 168.0f);
            customize::ui::RawText("anim-group.hdPage", page);
            const float x2 = x + 52.0f + customize::ui::TextWidth(page, kHeadPt) + 52.0f;
            customize::ui::Rule("anim-group.hdRule", x2, 168.0f, kNR1, kRuleW0);
        }

        void Tabs(int hover)
        {
            float x = kX0 + 108.0f, ax = x, aw = 100.0f;
            char path[64], buf[128];
            for (int k = 0; k < 8; ++k) {
                std::snprintf(path, sizeof path, "anim-group.tabs.tb%d", k);
                customize::ui::Vis(path, k < 6);
                std::snprintf(path, sizeof path, "anim-group.tabs.tbHit%d", k);
                customize::ui::Vis(path, k < 6);
                if (k >= 6) continue;
                const char* lb = engine::locale::Tr(kClanTabs[k]);
                const float w = customize::ui::TextWidth(lb, kTabPt) + 8.0f;
                if (k == kMeTab || k == hover)
                    std::snprintf(buf, sizeof buf, "%s", lb);
                else
                    std::snprintf(buf, sizeof buf, "<font color=\"#8C8C8C\">%s</font>", lb);
                std::snprintf(path, sizeof path, "anim-group.tabs.tb%d", k);
                customize::ui::RawText(path, buf);
                customize::ui::Pivot(path, x, kTabY);
                std::snprintf(path, sizeof path, "anim-group.tabs.tbHit%d", k);
                customize::ui::Pivot(path, x - 30.0f, kTabY - 60.0f);
                if (k == kMeTab) { ax = x; aw = w; }
                x += w + 80.0f;
            }
            const float ex = x - 80.0f + 28.0f;
            customize::ui::Pivot("anim-group.tabs.tbE", ex + 30.0f, kTabY);
            customize::ui::Pivot("anim-group.tabs.tbEHit", ex + 30.0f, kTabY);
            customize::ui::Pivot("anim-group.tabs.tbU", ax, kTabY + 38.0f);
            customize::ui::Scale("anim-group.tabs.tbU", aw / kPanelW0, 1.0f);
        }

        void Week(const net::clan::State& c)
        {
            const bool on = c.ok != 0;
            for (const char* n : { "wkDay", "wkSv", "wkBarT", "wkBarF" }) {
                char path[64];
                std::snprintf(path, sizeof path, "anim-group.%s", n);
                customize::ui::Vis(path, on);
            }
            if (!on) return;
            // The clan-page view the ext reads is built from the day count;
            // mfnr / nrui use day/7+1 and day%7+1 for the header.
            const int done = std::max(0, c.day);
            char buf[96];
            std::snprintf(buf, sizeof buf, "WEEK %d: DAY %d", done / 7 + 1, done % 7 + 1);
            customize::ui::Text("anim-group.wkDay", buf);
            std::snprintf(buf, sizeof buf, "DAYS SURVIVED %d | 84", done);
            customize::ui::Text("anim-group.wkSv", buf);
            customize::ui::Bar("anim-group.wkBarF", kNR1 - 760.0f, kNR1, 352.0f + 3.0f,
                               done / 84.0f);
        }

        void Section(const char* base, float x0, float x1, float y,
                     const char* title, const char* right)
        {
            char path[96];
            std::snprintf(path, sizeof path, "%sTx", base);
            customize::ui::Text(path, title);
            std::snprintf(path, sizeof path, "%sRt", base);
            customize::ui::Text(path, right);
            const float xa = x0 + customize::ui::TextWidth(title, kSectionPt) + 40.0f;
            const float xb = x1 - (right && right[0]
                                   ? customize::ui::TextWidth(right, kCountPt) + 40.0f
                                   : 0.0f);
            std::snprintf(path, sizeof path, "%sRl", base);
            customize::ui::Rule(path, xa, y, xb, kRuleW0);
        }

        void Chip(const char* base, const char* text, float x, float y)
        {
            char path[96];
            const float w = customize::ui::TextWidth(text, kChipPt) + 32.0f;
            float px = 0.0f, py = y;
            std::snprintf(path, sizeof path, "%sTx", base);
            customize::ui::RawText(path, text);
            const uint64_t tw = customize::ui::W(path);
            if (tw && engine::widget::PivotGet(tw, &px, &py))
                customize::ui::Pivot(path, x + 16.0f, py);
            else
                customize::ui::Pivot(path, x + 16.0f, y);
            std::snprintf(path, sizeof path, "%sBg", base);
            const uint64_t bw = customize::ui::W(path);
            if (bw && engine::widget::PivotGet(bw, &px, &py))
                customize::ui::Pivot(path, x, py);
            else
                customize::ui::Pivot(path, x, y);
            customize::ui::Scale(path, w / kChipW0, 1.0f);
        }

        void FrameAt(float x, float y, float w, float h, bool on)
        {
            static const char S[4] = { 'T', 'B', 'L', 'R' };
            char path[64];
            for (int k = 0; k < 4; ++k) {
                std::snprintf(path, sizeof path, "anim-group.rows.foc%c", S[k]);
                customize::ui::Vis(path, on);
                if (!on) continue;
                if (k == 0) {
                    customize::ui::Pivot(path, x, y + 2.0f);
                    customize::ui::Scale(path, w / kPanelW0, 1.0f);
                } else if (k == 1) {
                    customize::ui::Pivot(path, x, y + h - 2.0f);
                    customize::ui::Scale(path, w / kPanelW0, 1.0f);
                } else {
                    customize::ui::Pivot(path, k == 2 ? x : x + w - 4.0f, y);
                    customize::ui::Scale(path, 1.0f, h / kPanelW0);
                }
            }
        }

        void HorizontalScroll(int r, int first, int shown, int n)
        {
            char path[64];
            const bool on = n > shown;
            std::snprintf(path, sizeof path, "anim-group.rows.rs%dT", r);
            customize::ui::Vis(path, on);
            std::snprintf(path, sizeof path, "anim-group.rows.rs%dH", r);
            customize::ui::Vis(path, on);
            if (!on) return;
            const float a = static_cast<float>(first) / static_cast<float>(n);
            float b = static_cast<float>(first + shown) / static_cast<float>(n);
            if (b > 1.0f) b = 1.0f;
            std::snprintf(path, sizeof path, "anim-group.rows.rs%dH", r);
            customize::ui::Pivot(path, kX0 + (kX1 - kX0) * a,
                                 TileY(r) + kBarDy + 31.0f);
            customize::ui::Scale(path, (kX1 - kX0) * (b - a) / kRuleW0, 1.0f);
        }

        void VerticalScroll(int first, int shown, int n)
        {
            const bool on = n > shown && n > 0;
            customize::ui::Vis("anim-group.rows.sbVT", on);
            customize::ui::Vis("anim-group.rows.sbVH", on);
            if (!on) return;
            const float a = static_cast<float>(first) / static_cast<float>(n);
            const float span = kSidebarY1 - kSidebarY0;
            customize::ui::Pivot("anim-group.rows.sbVH", kSidebarX - 2.0f,
                                 kSidebarY0 + span * a);
            customize::ui::Scale("anim-group.rows.sbVH", 1.0f,
                                 std::max(8.0f, span * static_cast<float>(shown)
                                                      / static_cast<float>(n)) / span);
        }

        // nr_prompts(): declared left to right, drawn right to left. The
        // prompt index is the declaration index (tabs=0, rows=1, back=2).
        void Prompts(bool rowsOn)
        {
            const bool pad = engine::menu_input::PadMode();
            PromptItem items[3] = {
                { "menu:prev+menu:next", "CLAN TABS", true },
                { "W / S", "ROWS", rowsOn },
                { "menu:back", "BACK", true },
            };
            int decl[3] = { 2, 1, 0 };
            g.pbN = PromptCount(pad);
            if (pad) {
                items[1] = items[2];        // nrui.c: pr[1] = pr[2], then 2 prompts
                decl[0] = 1;
                decl[1] = 0;
                customize::ui::Vis("anim-group.pbar.pb2", false);
            }
            float x = kNR1;
            char path[96], k1[48], k2[48];
            for (int k = 0; k < g.pbN; ++k) {
                std::snprintf(path, sizeof path, "anim-group.pbar.pb%d", k);
                customize::ui::Vis(path, true);
                const PromptItem& q = items[decl[k]];
                const char* key = q.key ? q.key : "";
                k1[0] = k2[0] = 0;
                const bool glyph = std::strncmp(key, "menu:", 5) == 0;
                if (glyph) {
                    const char* plus = std::strchr(key, '+');
                    if (plus) {
                        std::snprintf(k1, sizeof k1, "%.*s", static_cast<int>(plus - key), key);
                        std::snprintf(k2, sizeof k2, "%s", plus + 1);
                    } else {
                        std::snprintf(k1, sizeof k1, "%s", key);
                    }
                }
                const float kw = glyph ? (k2[0] ? 136.0f : 64.0f)
                                       : std::max(60.0f, customize::ui::TextWidth(key, kPromptKeyPt) + 28.0f);
                const float lw = customize::ui::TextWidth(q.label, kPromptPt);
                const float tot = 18.0f + kw + 22.0f + lw + 26.0f;
                g.pbX0[k] = x - tot;
                g.pbX1[k] = x;
                customize::ui::Pivot(path, x - tot, kPromptY - kPromptH * 0.5f);
                const float a = q.on ? 70.0f / 255.0f : 30.0f / 255.0f;
                static const char S4[4] = { 'T', 'B', 'L', 'R' };
                for (int s = 0; s < 4; ++s) {
                    std::snprintf(path, sizeof path, "anim-group.pbar.pb%d.pb%d%c", k, k, S4[s]);
                    customize::ui::Vis(path, true);
                    customize::ui::Alpha(path, a);
                    if (s < 2) customize::ui::Scale(path, tot / kPromptW0, 1.0f);
                    if (s == 3) customize::ui::Pivot(path, tot - 4.0f, 0.0f);
                }
                std::snprintf(path, sizeof path, "anim-group.pbar.pb%d.pb%dKg", k, k);
                customize::ui::Vis(path, glyph);
                if (glyph) {
                    customize::ui::Image(path, k1);
                    customize::ui::Alpha(path, q.on ? 1.0f : 0.4f);
                }
                std::snprintf(path, sizeof path, "anim-group.pbar.pb%d.pb%dKg2", k, k);
                customize::ui::Vis(path, glyph && k2[0]);
                if (glyph && k2[0]) {
                    customize::ui::Image(path, k2);
                    customize::ui::Alpha(path, q.on ? 1.0f : 0.4f);
                }
                std::snprintf(path, sizeof path, "anim-group.pbar.pb%d.pb%dKb", k, k);
                customize::ui::Vis(path, !glyph);
                std::snprintf(path, sizeof path, "anim-group.pbar.pb%d.pb%dKt", k, k);
                customize::ui::Vis(path, !glyph);
                if (!glyph) {
                    customize::ui::RawText(path, key);
                    customize::ui::Pivot(path, 18.0f + kw * 0.5f - 50.0f, 16.0f);
                }
                std::snprintf(path, sizeof path, "anim-group.pbar.pb%d.pb%dTx", k, k);
                char lb[128];
                const char* label = engine::locale::Tr(q.label);
                if (q.on)
                    std::snprintf(lb, sizeof lb, "%s", label);
                else
                    std::snprintf(lb, sizeof lb, "<font color=\"#585858\">%s</font>", label);
                customize::ui::RawText(path, lb);
                customize::ui::Pivot(path, 18.0f + kw + 22.0f, kPromptH * 0.5f);
                x -= tot + 28.0f;
            }
        }

        // ach.<id>.title / ach.<id>.desc, falling back to the daemon's English.
        const char* AchText(const Row& r, bool desc, char* out, size_t n)
        {
            char key[96];
            LocKey(r, desc, key, sizeof key);
            const char* t = engine::locale::Tr(key);
            if (!t || std::strcmp(t, key) == 0)
                std::snprintf(out, n, "%s", desc ? r.desc : r.title);
            else
                std::snprintf(out, n, "%s", t);
            return out;
        }

        // ---- page fill ---------------------------------------------------------

        void DrawPage(uint64_t now)
        {
            if (!g.built) {
                BuildGroups(g.ui, g.table);
                g.built = true;
            }
            g.ui.seenT = g.uiSeenT;
            ClampFocus(g.ui);

            const net::clan::State& c = net::clan::Get();
            Header(c);
            Tabs(g.ui.hvTab);
            Week(c);
            Prompts(g.ui.groups > kRows);

            const bool ok = g.table.ok && g.table.n > 0;
            customize::ui::Vis("anim-group.ldTx", !ok);
            if (!ok) customize::ui::Text("anim-group.ldTx", "LOADING...");
            customize::ui::Vis("anim-group.rows", ok);
            customize::ui::Vis("anim-group.det", ok);
            if (!ok) return;

            bool focusOn = false;
            for (int r = 0; r < kRows; ++r) {
                const int ri = g.ui.row0 + r;
                const bool on = ri < g.ui.groups;
                char path[96];
                std::snprintf(path, sizeof path, "anim-group.rows.rw%dTx", r);
                customize::ui::Vis(path, on);
                std::snprintf(path, sizeof path, "anim-group.rows.rw%dRt", r);
                customize::ui::Vis(path, on);
                std::snprintf(path, sizeof path, "anim-group.rows.rw%dRl", r);
                customize::ui::Vis(path, on);
                if (on) {
                    int unlocked = 0;
                    for (int k = 0; k < g.ui.g[ri].n; ++k)
                        if (g.table.rows[g.ui.g[ri].item[k]].t) ++unlocked;
                    char right[32];
                    std::snprintf(right, sizeof right, "%d | %d", unlocked, g.ui.g[ri].n);
                    char base[64];
                    std::snprintf(base, sizeof base, "anim-group.rows.rw%d", r);
                    Section(base, kX0, kX1, kRowY0 + kRowPitch * r,
                            g.ui.g[ri].label, right);
                }
                HorizontalScroll(r, on ? g.ui.off[ri] : 0, kCols, on ? g.ui.g[ri].n : 0);

                for (int col = 0; col < kCols; ++col) {
                    const int k = on ? g.ui.off[ri] + col : 0;
                    const bool vis = on && k < g.ui.g[ri].n;
                    std::snprintf(path, sizeof path, "anim-group.rows.t%d_%d", r, col);
                    customize::ui::Vis(path, vis);
                    std::snprintf(path, sizeof path, "anim-group.rows.t%d_%dBT", r, col);
                    customize::ui::Vis(path, false);
                    if (!vis) continue;
                    const Row& row = g.table.rows[g.ui.g[ri].item[k]];
                    const bool locked = row.t == 0;
                    const bool hidden = row.hidden && locked;
                    const bool focus = ri == g.ui.fr && k == g.ui.fc;
                    const bool bar = TileBar(row);
                    char bp[64];
                    std::snprintf(bp, sizeof bp, "anim-group.rows.t%d_%dBT", r, col);
                    customize::ui::Vis(bp, bar);
                    char bf[64];
                    std::snprintf(bf, sizeof bf, "anim-group.rows.t%d_%dBF", r, col);
                    customize::ui::Vis(bf, bar);
                    if (bar) {
                        const float x = TileX(col);
                        customize::ui::Bar(bf, x, x + kTileW, TileY(r) + kBarDy + 2.0f,
                                           TileBarFraction(row));
                        customize::ui::Alpha(bf, focus ? 1.0f : 150.0f / 255.0f);
                    }
                    char child[96];
                    char icon[96];
                    IconPath(row, icon, sizeof icon);
                    std::snprintf(child, sizeof child, "anim-group.rows.t%d_%d.t%d_%dP", r, col, r, col);
                    customize::ui::Image(child, icon);
                    customize::ui::Alpha(child, TileAlpha(focus || (ri == g.ui.hvR && k == g.ui.hvC),
                                                          locked, hidden));
                    std::snprintf(child, sizeof child, "anim-group.rows.t%d_%d.t%d_%dL", r, col, r, col);
                    customize::ui::Vis(child, locked && !hidden);
                    if (locked && !hidden)
                        customize::ui::Alpha(child, focus ? 1.0f : 170.0f / 255.0f);
                    std::snprintf(child, sizeof child, "anim-group.rows.t%d_%d.t%d_%dBg", r, col, r, col);
                    customize::ui::Alpha(child, static_cast<float>(focus ? 215 : 175) / 255.0f);
                    std::snprintf(child, sizeof child, "anim-group.rows.t%d_%d.t%d_%dN", r, col, r, col);
                    customize::ui::Vis(child, NewDot(row, g.uiSeenT));
                    if (focus) {
                        float fx = 0.0f, fy = 0.0f, fw = 0.0f, fh = 0.0f;
                        FocusFrame(col, r, fx, fy, fw, fh);
                        FrameAt(fx, fy, fw, fh, true);
                        focusOn = true;
                    }
                }
            }
            if (!focusOn) FrameAt(0.0f, 0.0f, 0.0f, 0.0f, false);
            VerticalScroll(g.ui.row0, kRows, g.ui.groups);

            const int total = g.table.n;
            const int unlocked = UnlockedCount(g.table);
            char buf[160];
            std::snprintf(buf, sizeof buf, "%d | %d  (%d%%)", unlocked, total,
                          total ? unlocked * 100 / total : 0);
            Section("anim-group.det.dTot", kDetailX0, kDetailX1, kRowY0, "UNLOCKED", buf);
            customize::ui::Bar("anim-group.det.dTotBF", kDetailX0, kDetailX1,
                               kRowY0 + 34.0f + 3.0f,
                               total ? static_cast<float>(unlocked) / static_cast<float>(total) : 0.0f);

            const int idx = FocusedIndex(g.ui);
            customize::ui::Vis("anim-group.det.dArt", idx >= 0);
            if (idx < 0) {
                customize::ui::Vis("anim-group.det.dUnl", false);
                return;
            }
            const Row& row = g.table.rows[idx];
            const bool locked = row.t == 0;
            const bool hidden = row.hidden && locked;
            char icon[96];
            IconPath(row, icon, sizeof icon);
            customize::ui::Image("anim-group.det.dArt", icon);
            customize::ui::Alpha("anim-group.det.dArt", hidden ? 140.0f / 255.0f : 1.0f);
            customize::ui::Vis("anim-group.det.dLock", locked && !hidden);

            customize::ui::Text("anim-group.det.dGrp", g.ui.g[g.ui.fr].label);
            char at[160];
            if (hidden)
                customize::ui::Fit("anim-group.det.dTitle", "?", kDetailX1 - kDetailX0, kFitPt);
            else
                customize::ui::Fit("anim-group.det.dTitle", AchText(row, false, at, sizeof at),
                                   kDetailX1 - kDetailX0, kFitPt);
            if (hidden)
                customize::ui::Text("anim-group.det.dDesc", "Hidden achievement. Keep playing.");
            else
                customize::ui::Text("anim-group.det.dDesc", AchText(row, true, at, sizeof at));

            const bool bar = !hidden;
            customize::ui::Vis("anim-group.det.dBarT", bar);
            customize::ui::Vis("anim-group.det.dBarF", bar);
            customize::ui::Vis("anim-group.det.dCnt", bar);
            if (bar) {
                int prog = 0, goal = 0;
                DetailProgress(row, prog, goal);
                std::snprintf(buf, sizeof buf, "%d | %d", prog, goal);
                customize::ui::Text("anim-group.det.dCnt", buf);
                customize::ui::Bar("anim-group.det.dBarF", kDetailX0, kDetailX1 - 300.0f,
                                   kTitleY + 230.0f + 3.0f,
                                   static_cast<float>(prog) / static_cast<float>(goal));
            }

            char status[160];
            if (row.t) {
                const std::time_t tt = static_cast<std::time_t>(row.t);
                std::tm lt{};
#if defined(_WIN32)
                localtime_s(&lt, &tt);
#else
                if (std::tm* p = std::localtime(&tt)) lt = *p;
#endif
                std::snprintf(status, sizeof status, "%s %02d %s %d",
                              engine::locale::Tr("UNLOCKED"), lt.tm_mday,
                              engine::locale::Tr(MonthKey(lt.tm_mon % 12)),
                              lt.tm_year + 1900);
            } else {
                std::snprintf(status, sizeof status, "%s",
                              engine::locale::Tr(StatusKey(row)));
            }
            const int ci = StatusOf(row);
            for (int i = 0; i < 3; ++i) {
                const char* chip = StatusChip(i);
                char path[96];
                std::snprintf(path, sizeof path, "%sBg", chip);
                customize::ui::Vis(path, i == ci);
                std::snprintf(path, sizeof path, "%sTx", chip);
                customize::ui::Vis(path, i == ci);
            }
            customize::ui::Text("anim-group.det.dSt", "Status:");
            Chip(StatusChip(ci), status, kDetailX0 + 170.0f, 0.0f);

            char u[160];
            UnlocksFor(row.id, u, sizeof u);
            const bool unlocksOn = !hidden && u[0];
            customize::ui::Vis("anim-group.det.dUnl", unlocksOn);
            if (unlocksOn) {
                std::snprintf(buf, sizeof buf, "UNLOCKS: %s", u);
                customize::ui::Text("anim-group.det.dUnl", buf);
            }
            (void)now;
        }

        // ---- mouse / input -----------------------------------------------------

        int TabHover()
        {
            const uint64_t q = customize::ui::W("anim-group.tabs.tbQHit");
            const uint64_t e = customize::ui::W("anim-group.tabs.tbEHit");
            if (q && engine::widget::MouseOver(q, true)) return -2;
            if (e && engine::widget::MouseOver(e, true)) return -3;
            for (int k = 0; k < 6; ++k) {
                char path[64];
                std::snprintf(path, sizeof path, "anim-group.tabs.tbHit%d", k);
                const uint64_t w = customize::ui::W(path);
                if (w && engine::widget::MouseOver(w, true)) return k;
            }
            return -1;
        }

        int PromptHover(float cx, float cy)
        {
            if (engine::menu_input::PadMode()) return -1;
            if (cy < kPromptY - kPromptH * 0.5f - 6.0f
                || cy > kPromptY + kPromptH * 0.5f + 6.0f) return -1;
            for (int k = 0; k < g.pbN; ++k)
                if (cx >= g.pbX0[k] && cx <= g.pbX1[k]) return g.pbN - 1 - k;
            return -1;
        }

        void Scan(float cx, float cy, bool haveCursor)
        {
            g.ui.hvR = g.ui.hvC = -1;
            g.ui.hvTab = TabHover();
            g.ui.hvPb = PromptHover(cx, cy);
            int row = -1, col = -1;
            if (haveCursor && HoverCell(cx, cy, g.ui, row, col)) {
                g.ui.hvR = row;
                g.ui.hvC = g.ui.off[row] + col;
            }
        }

        void PageTick(uint64_t now)
        {
            float cx = 0.0f, cy = 0.0f;
            const bool haveCursor = engine::window::CursorCanvas(&cx, &cy);
            const bool pad = engine::menu_input::PadMode();
            const int wheel = engine::menu_input::WheelNotches();
            const int wr = WheelRows(wheel);

            bool up = engine::menu_input::CommandRepeat(SID("menu-up"), g.hUp, g.tUp, now);
            bool down = engine::menu_input::CommandRepeat(SID("menu-down"), g.hDown, g.tDown, now);
            const bool left = engine::menu_input::CommandRepeat(SID("menu-left"), g.hLeft, g.tLeft, now);
            const bool right = engine::menu_input::CommandRepeat(SID("menu-right"), g.hRight, g.tRight, now);
            if (wr < 0) up = true;
            if (wr > 0) down = true;
            const bool back = engine::menu_input::CommandEdge(SID("menu-back"), g.hBack);
            engine::menu_input::CommandEdge(SID("menu-select"), g.hSel);
            const bool prev = engine::menu_input::CommandEdge(SID("menu-prev-tab"), g.hPrev);
            const bool next = engine::menu_input::CommandEdge(SID("menu-next-tab"), g.hNext);

            const bool escNow = engine::window::KeyDown(VK_ESCAPE);
            const bool fNow = engine::window::KeyDown('F');
            const bool qNow = engine::window::KeyDown('Q');
            const bool eNow = engine::window::KeyDown('E');
            const bool esc = escNow && !g.hEsc;
            const bool f = fNow && !g.hF;
            const bool tabL = (qNow && !g.hQ) || prev;
            const bool tabR = (eNow && !g.hE) || next;
            g.hEsc = escNow;
            g.hF = fNow;
            g.hQ = qNow;
            g.hE = eNow;

            const bool click = engine::menu_input::ClickEdge();
            Scan(cx, cy, haveCursor);
            const int sig = HoverSignature(g.ui);
            bool dirty = !g.built || sig != g.hoverSig || pad != g.lastPad;

            const bool early = engine::frame::Count() <= g.f0 + 10;
            const int backPb = pad ? 1 : 2;   // nr_prompts() drops ROWS on pad
            if (!early && (back || esc || f || (click && g.ui.hvPb == backPb))) {
                LeavePage(back ? "menu-back" : esc ? "Esc" : f ? "F" : "BACK clicked", !back);
                return;
            }
            if (!early) {
                if (tabL || (click && g.ui.hvPb == 0) || (click && g.ui.hvTab == -2)) {
                    LeavePage("prev tab", true);
                    return;
                }
                if (tabR || (click && g.ui.hvTab == -3)) {
                    LeavePage("next tab", true);
                    return;
                }
                if (click && g.ui.hvTab >= 0 && g.ui.hvTab != kMeTab) {
                    LeavePage("tab clicked", true);
                    return;
                }
            }

            const int ofr = g.ui.fr, ofc = g.ui.fc;
            if (up) MoveRow(g.ui, -1);
            if (down) MoveRow(g.ui, 1);
            if (left) MoveCol(g.ui, -1);
            if (right) MoveCol(g.ui, 1);
            if (click && g.ui.hvR >= 0 && !early)
                FocusHover(g.ui, g.ui.hvR, g.ui.hvC);
            if (g.ui.fr != ofr || g.ui.fc != ofc) {
                dirty = true;
                customize::ui::Sfx("sfx-cycle");
            }

            if (dirty || engine::frame::Count() % 30 == 0) {
                DrawPage(now);
                g.hoverSig = sig;
                g.lastPad = pad;
            }
        }

        // achc_w in achieve_ui.c: the card lives on the match HUD while a match
        // runs, otherwise under anim-group on one of four menu pages.
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
                if (g.card.active) { g.card.t0 += dt; engine::widget::Visible(root, false); }
                return;
            }
            if (!g.card.active) {
                if (g.card.n <= 0 || !root) return;
                StartCard(g.card, now, place);
                engine::widget::Visible(root, false);
                WriteCard(hud);
                g.card.t0 += dt;
                return;
            }
            if (!root) { g.card.t0 += dt; return; }
            if (g.card.page != place) {
                engine::widget::Visible(root, false);
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
            engine::widget::Visible(root, true);
            if (!g.card.snd) {
                g.card.snd = true;
                customize::ui::Sfx("sfx-select");
            }
            if (AdvanceCard(g.card, now)) {
                engine::widget::Visible(root, false);
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
        if (Disabled()) {
            if (g.uiEntered) LeavePage("disabled", false);
            return;
        }
        ReadCardSeen();

        if (net::match::liveness::MatchEndHold()) ArmReport(now);
        else if (!net::match::liveness::Live()) g.endLatched = false;
        TickReport(now);

        const uint64_t page = Page();
        const bool onPage = engine::widget::TopPage() == page;
        if (onPage && !g.uiEntered) EnterPage();
        else if (!onPage && g.uiEntered) LeavePage("page popped", false);
        if (onPage && now >= g.tableAt) {
            g.tableAt = AskTable(now) ? now + kPageAskMs : now + kAskRetryMs;
        } else if (!onPage) {
            g.tableAt = 0;
        }
        TickLive(now);
        if (onPage) PageTick(now);
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
