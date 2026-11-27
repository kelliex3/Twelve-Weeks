// Native owner of tools/ext/mfnr.c and feature_matchflow (t2r-ext.c): the map
// vote (0xEA / 0xEC / 0xEB), the booster choice (0xE8 / 0xEE / 0xE9) and the
// loading screen. Pure rules live in mfnr-core.hpp; this module owns the daemon
// RPC, the to1/matchflow page, the generated widget writes, the rank badges,
// the click gate, the loading-screen hook and the checked winning-map write.
// It yields while t2r-ext.dll is loaded; the ext path is deprecated, not a
// dependency. Widget writes are inert when the GUI pak does not carry
// matchflow_widgets.py / nr_style.py (engine::widget::Page returns 0).

#include "mfnr.hpp"
#include "mfnr-core.hpp"
#include "nrui.hpp"

#include "net-protocol.hpp"
#include "daemon-rpc.hpp"   // winsock2 before engine/script.hpp's windows.h
#include "net-account.hpp"
#include "clan-state.hpp"
#include "rank-badge.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/lobby.hpp"
#include "engine/locale.hpp"
#include "engine/menu-input.hpp"
#include "engine/ptr.hpp"
#include "log.hpp"          // TO1_LOG must precede engine/script.hpp
#include "engine/script.hpp"
#include "engine/widget.hpp"
#include "engine/window.hpp"
#include "sid/sid.hpp"

#include <safetyhook.hpp>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace net::ui::mfnr {

    namespace {

        namespace core = net::ui::mfnr::core;
        namespace nrcore = net::ui::nrui::core;

        // Pinned exe RVAs (tools/ext/t2r-ext.c, re/loading-screen.md). All raw
        // reads go through engine::ptr::Readable(), and a changed prologue makes
        // the hook fail closed instead of calling a moved function.
        constexpr uintptr_t kLsInstRva = 0x35524b0;
        constexpr uintptr_t kLsWrapRva = 0x3d1370;
        constexpr uintptr_t kLsSetEnabledRva = 0x3d13e0;
        constexpr uint8_t kLsPrologue[16] = {
            0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83,
            0xEC, 0x60, 0x48, 0x8B, 0xF9, 0x0F, 0xB6, 0xF2
        };
        constexpr uint8_t kLsWrapperCall[5] = { 0xE8, 0x1F, 0x00, 0x00, 0x00 };
        constexpr size_t kLsWrapperCallOff = 0x4c;
        constexpr uint64_t kLsRequestMs = 6300;   // ext MH_POP_MS: horn asks for LOADING
        constexpr uint64_t kHornMaxMs = 11000;    // ext MH_MAX_MS: envelope end
        constexpr uint64_t kLsWatchdogMs = 30000; // ext's own load watchdog

        constexpr uint8_t kOpRead = static_cast<uint8_t>(NetDaemonOp::kMapVoteRead);
        constexpr uint8_t kOpSend = static_cast<uint8_t>(NetDaemonOp::kMapVoteSend);
        constexpr uint8_t kOpReply = static_cast<uint8_t>(NetDaemonOp::kMapVoteReply);
        constexpr uint8_t kOpMetaRead = static_cast<uint8_t>(NetDaemonOp::kMetaRead);
        constexpr uint8_t kOpBoosterPick = static_cast<uint8_t>(NetDaemonOp::kBoosterPick);
        constexpr uint8_t kOpMetaReply = static_cast<uint8_t>(NetDaemonOp::kMetaReply);
        constexpr size_t  kTokenLen = 64;
        constexpr int     kReplyCap = 8192;
        // ext T2rRpc timeouts for 0xEA (read) and 0xEC (vote). The native call is
        // synchronous and bounded, so one try replaces the ext's three retries.
        constexpr int kReadConnMs = 1200, kReadIoMs = 2500, kReadReplyMs = 2500;
        constexpr int kSendConnMs = 2500, kSendIoMs = 3500, kSendReplyMs = 3500;
        constexpr int kMetaConnMs = 3000, kMetaIoMs = 3500, kMetaReplyMs = 4000;
        constexpr uint64_t kPushGapMs = 700;
        constexpr int kMaxPushes = 4;
        constexpr int kDbSettle = 3;   // ext DB_SETTLE: frames before a twin swap
        constexpr uint64_t kBoRetryMs = 900;   // ext bo_next
        constexpr uint64_t kMetaPollMs = 2500; // ext b_poll

        struct Runtime {
            core::Vote m;
            uint32_t version = 0;
            uint32_t lastVer = ~uint32_t{ 0 };
            uint32_t voteVer = 0;
            int stageShown = -1;
            int focus = -1;
            int lastSecs = -1;
            int loggedW = -1;
            int wantVote = -1;
            int voted = -1;
            int votedMap = -1;
            int resends = 0;
            int lv = -1, lvmap = -1;   // our last click: shown before the reply
            int promptKey = -1;
            bool sawVote = false, active = false, busy = false;
            uint64_t pushAt = 0, pollAt = 0, sendAt = 0, lastTop = 0;
            int pushes = 0;
            bool hSel = true, hLeft = true, hRight = true, hUp = true,
                 hDown = true, hPrev = true, hNext = true;

            int timerMax = 0;
            bool timerShownB = false, timerPending = false;
            uint64_t timerAt = 0;
            char timerText[2][32]{};

            // stage 2: 0xE8 / 0xEE / 0xE9 booster state.
            core::Meta meta;
            bool metaHave = false;
            uint32_t metaVer = 0;
            int bfocus = 0, boN = 1, boIds[8]{};
            int boWant = -1;               // -1 none, 0 = Nothing, 1..7 = booster id
            int hintB = -1;
            uint64_t boNext = 0, bPoll = 0;
            int32_t bVer = -1;

            // booster title twin (the vote timer keeps its own twin above).
            bool boShownB = false, boPending = false;
            uint64_t boAt = 0;
            char boText[2][160]{};

            // stage 3: the engine's LoadingScreen is ours while `lsOwn` is 1
            // (2 = fading out), so the engine's own disable is swallowed until
            // the match intro / menu takes over.
            int lsOwn = 0;
            bool lsWant = false, lsSelf = false, lsTrampOnly = false;
            uint64_t lsReqAt = 0, lsTakeAt = 0, lsRelAt = 0;
            uint64_t lsTipAt = 0, lsDotsAt = 0, lsDoneTake = 0;
            uint64_t lsMetaTry = 0, lsMetaAt = 0;
            int lsTip = -1, lsRot = 0;
            char lsLastPic[64]{};
            core::Meta lsMeta;
            bool lsMetaHave = false;
            bool sawCd = false;            // ext horn's `saw_cd`: a flow stage was seen
            // Shared match-found horn t=0 for menu::sendoff (ext feature_match_horn's
            // `t0`). `hornScene` is sampled once at the trigger; 0 = unknown.
            uint64_t hornAt = 0;
            int hornScene = 0;
        };

        Runtime g;
        void* gModule = nullptr;
        SafetyHookInline gLsHook;
        bool gLsHookTried = false;

        // The loading widget's double-buffered text (ext db_text_wd / DB_LOAD):
        // write the hidden twin on change, then swap it in after DB_SETTLE
        // frames so its font is built before it is shown.
        struct DbSlot {
            uint64_t a = 0, b = 0, hash = 0, at = 0;
            bool shownB = false, pending = false, fresh = true;
        };
        DbSlot gLsTipCat, gLsTip, gLsSpin;

        void ResetRuntime()
        {
            g = Runtime{};
            core::Reset(g.m);
            gLsTipCat = DbSlot{};
            gLsTip = DbSlot{};
            gLsSpin = DbSlot{};
        }

        // The ext derives g_ms_cur from the live menu task subnode. Sample it
        // once when the horn fires so a scene swap under the fade cannot split
        // the birds / send-off across factions. The clan faction is the fallback
        // when no task probe succeeds (no sidecar is read or written).
        int HornScene()
        {
            if (DEBUG::IS_TASK_ACTIVE(SID("starlight-levels"),
                                      SID("starlight-main-menu")))
                return 1;
            if (DEBUG::IS_TASK_ACTIVE(SID("starlight-levels"),
                                      SID("starlight-t2-main-menu")))
                return 2;
            const int fac = net::clan::Get().fac;
            return fac == 1 || fac == 2 ? fac : 0;
        }

        uintptr_t ModuleBase()
        {
            return gModule ? reinterpret_cast<uintptr_t>(gModule)
                           : reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
        }

        // The stage-3 fill reads the last published 0xE9 snapshot even after
        // the vote's runtime was reset on the page close.
        void LsNoteMeta(uint64_t now)
        {
            g.lsMeta = g.meta;
            g.lsMetaHave = g.metaHave;
            g.lsMetaAt = now;
            g.lsMetaTry = now;
        }

        // ------------------------------------------------------------------ RPC

        // One bounded [u16 len][op][64B token][payload] exchange. `expected` is
        // the reply opcode (0xEB for the vote, 0xE9 for the meta state); `reply`
        // receives the text after the op. `sentOut` tells a queued booster pick
        // that the request went out even when the reply was lost.
        bool Ask(uint8_t op, const uint8_t* payload, int plen,
                 char* reply, int cap, bool* sentOut = nullptr,
                 uint8_t expected = kOpReply)
        {
            sockaddr_in a{};
            if (!net::daemon::Addr(&a) || !net::account::HaveToken())
                return false;
            if (std::strlen(net::account::Token()) != kTokenLen
                || plen < 0 || plen > 1)
                return false;
            uint8_t frame[128];
            const int n = 1 + plen;
            frame[0] = static_cast<uint8_t>(n);
            frame[1] = static_cast<uint8_t>(n >> 8);
            frame[2] = op;
            std::memcpy(frame + 3, net::account::Token(), kTokenLen);
            if (plen > 0)
                std::memcpy(frame + 3 + kTokenLen, payload, static_cast<size_t>(plen));

            char buf[kReplyCap];
            int sent = 0;
            const bool poll = op == kOpRead || op == kOpMetaRead;
            const int got = net::daemon::Rpc(a, frame, n + 2, buf, sizeof buf,
                poll ? (op == kOpRead ? kReadConnMs : kMetaConnMs) : kSendConnMs,
                poll ? (op == kOpRead ? kReadIoMs : kMetaIoMs) : kSendIoMs,
                poll ? (op == kOpRead ? kReadReplyMs : kMetaReplyMs) : kSendReplyMs,
                &sent);
            if (sentOut) *sentOut = sent != 0;
            if (got < 3 || !sent)
                return false;
            const int len = static_cast<uint8_t>(buf[0])
                + (static_cast<uint8_t>(buf[1]) << 8);
            if (len < 1 || len + 2 > got)
                return false;
            if (static_cast<uint8_t>(buf[2]) != expected)
                return false;
            const int ntext = len - 1;
            if (ntext < 0 || ntext >= cap)
                return false;
            std::memcpy(reply, buf + 3, static_cast<size_t>(ntext));
            reply[ntext] = 0;
            net::daemon::MarkAlive();
            return true;
        }

        void RpcTick(uint64_t now, int stage)
        {
            const bool open = stage == 1 && g.m.ok && g.m.st == 1;
            const int votedMap = g.voted >= 0 && g.voted < core::kVoteN
                ? g.m.m[g.voted] : -1;

            // A click into a dead tunnel is re-sent when a newer state still
            // says another (or no) vote for the same option -> same map.
            if (core::NeedResend(g.voted, g.wantVote >= 0, open, g.busy,
                                 g.m.my, g.votedMap, votedMap, g.m.n,
                                 g.version, g.voteVer, g.resends)) {
                TO1_LOG("mfnr: option %d is not on the server (says %d) - resend %d\n",
                        g.voted, g.m.my, g.resends + 1);
                g.wantVote = g.voted;
                ++g.resends;
            }

            char text[kReplyCap];
            if (g.wantVote >= 0 && stage == 1 && !g.busy && now >= g.sendAt) {
                const uint8_t option = static_cast<uint8_t>(g.wantVote);
                const uint32_t ver = g.version;
                g.busy = true;
                bool sent = false;
                const bool ok = Ask(kOpSend, &option, 1, text, sizeof text, &sent);
                g.busy = false;
                // ext plays the cue and commits its optimism when the request
                // goes out, not when the state reply happens to come back.
                if (sent) {
                    AUDIO::PLAY_SOUND(SID("sfx-select"));
                    g.voted = g.wantVote;
                    g.votedMap = g.voted < core::kVoteN && g.voted < g.m.n
                        ? g.m.m[g.voted] : -1;
                    g.voteVer = ver;
                    g.wantVote = -1;
                    g.pollAt = now + core::kPollMs;
                    g.lv = g.voted;
                    g.lvmap = g.votedMap;
                    g.lastVer = ~uint32_t{ 0 };
                    TO1_LOG("mfnr: voted for option %d\n", g.voted);
                }
                if (ok) {
                    if (core::Parse(g.m, text)) ++g.version;
                    g.sendAt = 0;
                } else {
                    // Only a daemon that never took the request is retried on
                    // a timer; a lost reply waits for the next state via
                    // NeedResend(), so the same vote is not blindly duplicated.
                    g.sendAt = sent ? 0 : now + core::kResendMs;
                }
            } else if (g.active && now >= g.pollAt && !g.busy
                       && (stage == 1 || (g.sawVote && !core::Decided(g.m)))) {
                g.busy = true;
                const bool ok = Ask(kOpRead, nullptr, 0, text, sizeof text);
                g.busy = false;
                if (ok && core::Parse(g.m, text))
                    ++g.version;
                g.pollAt = now + core::kPollMs;
            }

            if (core::Decided(g.m)) {
                engine::lobby::SetWinningMap(g.m.w);
                if (g.loggedW != g.m.w) {
                    g.loggedW = g.m.w;
                    TO1_LOG("mfnr: vote closed, map %d (%s) -> lobby map byte\n",
                            g.m.w, core::MapName(g.m.w));
                }
            }
        }

        // --------------------------------------------------------------- widgets

        uint64_t W(const char* rel) { return engine::widget::Page(rel); }

        void Vis(const char* rel, bool on)
        {
            engine::widget::Visible(W(rel), on);
        }

        void Txt(const char* rel, const char* text)
        {
            engine::widget::SetText(W(rel), text ? text : "");
        }

        void Img(const char* rel, const char* path)
        {
            engine::widget::SetImage(W(rel), path);
        }

        void Scl(const char* rel, float sx, float sy)
        {
            engine::widget::Scale(W(rel), sx, sy);
        }

        void Bar(const char* rel, float x0, float x1, float y, float frac)
        {
            const uint64_t w = W(rel);
            engine::widget::Visible(w, nrcore::BarVisible(frac));
            if (!nrcore::BarVisible(frac)) return;
            engine::widget::Pivot(w, x0, y);
            engine::widget::Scale(w, nrcore::BarScale(x0, x1, frac), 1.0f);
        }

        // ext nr_fit uses nr_est (no char track), not widget::TextWidth.
        void Fit(const char* rel, const char* text, float box, float pt)
        {
            Txt(rel, text);
            const float e = nrcore::Est(text, pt, engine::locale::Tr);
            const float s = nrcore::FitScale(e, box);
            Scl(rel, s, s);
        }

        void StageFrame(int stage)
        {
            const bool on = stage == 1;
            Vis("anim-group.bgDim", on);
            Vis("anim-group.bgVig", on);
            Vis("anim-group.hdPar", on);
            Vis("anim-group.hdDiv", on);
            Vis("anim-group.hdPage", on);
            Vis("anim-group.hdRule", on);
            nrui::WeekVisible(on);
            Vis("anim-group.pbar", on);
            Vis("anim-group.bgDark", stage >= 2);
            Vis("anim-group.bgLight", stage >= 2);
            Vis("anim-group.vote", on);
            Vis("anim-group.booster", stage == 2);
            Vis("anim-group.mapBg", stage >= 2);
            Vis("anim-group.mapShade", stage >= 2);
            if (on) {
                g.timerMax = 0;
                nrui::Header("FOUND MATCH", "MAP VOTE");
            }
        }

        void TimerSwap(uint64_t frame)
        {
            if (!g.timerPending || frame - g.timerAt < kDbSettle) return;
            g.timerPending = false;
            g.timerShownB = !g.timerShownB;
        }

        void TimerText(int secs, uint64_t frame)
        {
            const int s = core::TimerMax(g.timerMax, secs);
            char t[32];
            std::snprintf(t, sizeof t, "00:%02d", s);
            // ext ui_text_db writes the hidden twin, then db_tick swaps it in
            // after DB_SETTLE frames so the font is built before it is shown.
            const int hidden = g.timerShownB ? 0 : 1;
            if (std::strcmp(t, g.timerText[hidden]) != 0) {
                Txt(hidden ? "anim-group.vote.voteTimerB"
                           : "anim-group.vote.voteTimer", t);
                std::snprintf(g.timerText[hidden], sizeof g.timerText[hidden],
                              "%s", t);
                g.timerPending = true;
                g.timerAt = frame;
            }
            Bar("anim-group.vote.voteHead.vtTmF", core::kLeft, core::kColRight,
                core::kHeaderY + 42.0f + 3.0f,
                core::TimerFrac(g.timerMax, s));
        }

        void TimerVisible(bool on)
        {
            const char* shown = g.timerShownB ? "anim-group.vote.voteTimerB"
                                              : "anim-group.vote.voteTimer";
            const char* hidden = g.timerShownB ? "anim-group.vote.voteTimer"
                                               : "anim-group.vote.voteTimerB";
            Vis(shown, on);
            Vis(hidden, false);
        }

        void Cell(const char* base, const char* pre, int k, int mi,
                  int votes, int pc, bool on, bool mine, bool grid)
        {
            char p[192], b[160];
            const float w = grid ? core::kGridW : core::kTileW;
            const float ih = grid ? core::kGridImgH : core::kTileImgH;
            const float strip = grid ? core::kGridStrip : core::kTileStrip;
            const float pad = grid ? 16.0f : 24.0f;
            const float pt = grid ? 27.0f : 39.0f;
            const float chipPt = grid ? 19.5f : 27.0f;

            std::snprintf(p, sizeof p, "%s.%s%d.%s%dName", base, pre, k, pre, k);
            Fit(p, core::MapName(mi), w - 2.0f * pad - 150.0f, pt);

            std::snprintf(p, sizeof p, "%s.%s%d.%s%dImg", base, pre, k, pre, k);
            core::MapImagePath(b, sizeof b, mi);
            Img(p, b);

            std::snprintf(p, sizeof p, "%s.%s%d.%s%dFo", base, pre, k, pre, k);
            Vis(p, on);
            std::snprintf(p, sizeof p, "%s.%s%d.%s%dShade", base, pre, k, pre, k);
            Vis(p, !on);

            std::snprintf(p, sizeof p, "%s.%s%d.%s%dYv", base, pre, k, pre, k);
            Vis(p, mine);
            if (mine) {
                std::snprintf(b, sizeof b, "%s.%s%dYv", p, pre, k);
                nrui::Chip(b, engine::locale::Tr("YOUR VOTE"), 0.0f, 0.0f, chipPt);
            }

            const bool p1 = core::IsPartOne(mi);
            std::snprintf(p, sizeof p, "%s.%s%d.%s%dP1", base, pre, k, pre, k);
            Vis(p, p1);
            if (p1) {
                std::snprintf(b, sizeof b, "%s.%s%dP1", p, pre, k);
                nrui::Chip(b, "PART I", 0.0f, w - (grid ? 10.0f : 16.0f), chipPt);
            }

            const core::CellBar c = core::CellOf(votes, pc);
            char count[64];
            core::CountText(count, sizeof count, c);
            std::snprintf(p, sizeof p, "%s.%s%d.%s%dCt", base, pre, k, pre, k);
            Txt(p, count);
            std::snprintf(p, sizeof p, "%s.%s%d.%s%dVbF", base, pre, k, pre, k);
            Bar(p, pad, w - pad,
                ih + strip - (grid ? 24.0f : 34.0f) + (grid ? 2.5f : 3.0f),
                c.frac);
        }

        // ext mf_team_tab: the lobby roster entry by name, +0x31 = team;
        // 1 = our team, 0 = unresolved, 2 = the enemy.
        int TeamTab(const char* name)
        {
            if (!name || !name[0]) return 0;
            const int n = engine::lobby::RosterCount();
            if (n <= 0 || n > static_cast<int>(core::kPlayers)) return 0;
            const uint8_t local = engine::lobby::LocalSlot();
            int mine = -1, who = -1;
            for (int i = 0; i < n; ++i) {
                const uint8_t* e = engine::lobby::RosterEntry(i);
                if (!e) continue;
                if (i == static_cast<int>(local)) mine = e[0x31] & 1;
                char nm[48];
                if (engine::lobby::RosterName(i, nm, sizeof nm)
                    && std::strncmp(nm, name, 20) == 0)
                    who = e[0x31] & 1;
            }
            if (mine < 0) {
                const uint8_t t = engine::lobby::OwnTeam();
                if (t != 0xFF) mine = t & 1;
            }
            return who < 0 ? 0 : (who == mine ? 1 : 2);
        }

        void Roster(const core::Vote& m, bool have, bool grid)
        {
            int tabs[core::kPlayers]{};
            for (int i = 0; i < core::kPlayers && i < m.pc; ++i)
                tabs[i] = TeamTab(m.p[i]);
            int ord[core::kPlayers]{};
            const int no = core::RosterOrder(tabs, m.pc, ord);

            const char* RG = grid ? "anim-group.vote.voteGrid.voteGridRoster"
                                  : "anim-group.vote.voteRoster";
            const char* RP = grid ? "gros" : "ros";
            const float mapBox = grid ? core::kGridMapW : core::kRosterMapW;
            const float nameBox = grid ? core::kGridNameW : core::kRosterNameW;
            const float rowW = grid ? core::kGridRowW : core::kRosterRowW;
            const char* own = net::account::Name();

            for (int r = 0; r < core::kPlayers; ++r) {
                char p[176], b[160];
                std::snprintf(p, sizeof p, "%s.%s%d", RG, RP, r);
                Vis(p, have && r < m.pc && r < no);
                if (!have || r >= m.pc || r >= no) continue;
                const int s = ord[r];
                const bool me = core::IsMe(m.p[s], own);

                std::snprintf(p, sizeof p, "%s.%s%d.%s%dName", RG, RP, r, RP, r);
                if (me)
                    std::snprintf(b, sizeof b, "<font color=\"#D9D86A\">%s</font>", m.p[s]);
                else if (tabs[s] == 2)
                    std::snprintf(b, sizeof b, "<font color=\"#8C8C8C\">%s</font>", m.p[s]);
                else
                    std::snprintf(b, sizeof b, "%s", m.p[s]);
                Fit(p, b, nameBox, core::kNamePt);

                // ext rank_ui(rk, wk) hides both leaves for an unresolved rank;
                // DrawMe would map rk < 0 to 0, so do that decision here.
                char ri[176], rn[176];
                std::snprintf(ri, sizeof ri, "%s.%s%d.%s%dRk", RG, RP, r, RP, r);
                std::snprintf(rn, sizeof rn, "%s.%s%d.%s%dRn", RG, RP, r, RP, r);
                if (m.pk[s] < 0) {
                    engine::widget::Visible(W(ri), false);
                    engine::widget::Visible(W(rn), false);
                    engine::widget::SetText(W(rn), "");
                } else {
                    net::rank_badge::DrawMe(ri, rn, m.pk[s], m.pw[s], 0);
                }

                std::snprintf(p, sizeof p, "%s.%s%d.%s%dCk", RG, RP, r, RP, r);
                Vis(p, m.pv[s] != 0 || (me && m.my >= 0));

                const int option = core::VoteOf(m, s, me);
                char mapText[160];
                core::VotedMapText(mapText, sizeof mapText, m, option,
                                   m.pv[s] != 0, me);
                std::snprintf(p, sizeof p, "%s.%s%d.%s%dMap", RG, RP, r, RP, r);
                Fit(p, mapText, mapBox, core::kMapPt);
                if (mapText[0]) {
                    // matchflow_widgets.ck_x: the check sits just before the
                    // right-aligned, possibly shrunk map name.
                    const float e = nrcore::Est(mapText, core::kMapPt,
                                                engine::locale::Tr);
                    const float sc = nrcore::FitScale(e, mapBox);
                    const float tw = nrcore::Tw(mapText, core::kMapPt,
                                                engine::locale::Tr);
                    std::snprintf(p, sizeof p, "%s.%s%d.%s%dCk", RG, RP, r, RP, r);
                    engine::widget::Pivot(W(p),
                        rowW - 16.0f - tw * sc - 16.0f - 38.0f, 22.0f);
                }
                std::snprintf(p, sizeof p, "%s.%s%d.%s%dTabB", RG, RP, r, RP, r);
                Vis(p, tabs[s] == 1);
                std::snprintf(p, sizeof p, "%s.%s%d.%s%dTabR", RG, RP, r, RP, r);
                Vis(p, tabs[s] == 2);
            }

            char voted[64];
            core::VotedText(voted, sizeof voted, m);
            if (grid)
                nrui::Section("anim-group.vote.voteGrid.voteGridRoster.vtGp",
                              core::kRightCol, core::kRightEdge, 1040.0f,
                              "PLAYERS", voted, core::kNamePt);
            else
                nrui::Section("anim-group.vote.voteRoster.vtPl",
                              core::kLeft, core::kColRight, 1210.0f,
                              "PLAYERS", voted, core::kNamePt);
        }

        void VoteFill(const core::Vote& src, int focus)
        {
            core::Vote m = src;
            if (g.lv >= 0)
                core::ApplyLocalVote(m, g.lv, g.lvmap);
            const bool have = core::HaveVote(m);
            const bool grid = core::GridMode(m);

            Vis("anim-group.vote.voteWait", !have);
            Vis("anim-group.vote.voteHead", have);
            Vis("anim-group.vote.voteTiles", have && !grid);
            Vis("anim-group.vote.voteGrid", grid);
            Vis("anim-group.vote.voteRoster", have && !grid);
            TimerVisible(have);
            if (have)
                nrui::Section("anim-group.vote.voteHead.vtMap",
                              core::kLeft, core::kColRight - 250.0f,
                              core::kHeaderY, "VOTE FOR A MAP", "", core::kNamePt);

            const int count = grid ? core::kGrid : core::kTiles;
            const char* base = grid ? "anim-group.vote.voteGrid"
                                    : "anim-group.vote.voteTiles";
            const char* pre = grid ? "g" : "tile";
            char p[96];
            for (int k = 0; k < count; ++k) {
                std::snprintf(p, sizeof p, "%s.%s%d", base, pre, k);
                Vis(p, core::CellVisible(m, k));
                if (!core::CellVisible(m, k)) continue;
                Cell(base, pre, k, m.m[k], m.v[k], m.pc,
                     core::CellFocused(m, k, focus), core::CellMine(m, k), grid);
            }
            Roster(m, have, grid);
        }

        void ClanFill()
        {
            if (!net::clan::HaveState()) return;
            const net::clan::State& c = net::clan::Get();
            if (!c.ok) return;
            const int d = c.day < 84 ? c.day : 83;
            int week = 0, day = 0;
            core::WeekOf(d, week, day);
            nrui::Week(week, day, d);
            Txt("anim-group.vote.voteSub", core::ModeLabel());

            const int v3[3] = { c.h, c.hu, c.s };
            const int tot = core::SurvivorTotal(c.h, c.hu, c.s);
            char b[64];
            std::snprintf(b, sizeof b, "%d", tot);
            nrui::Section("anim-group.vote.vtSv", core::kRightCol,
                          core::kRightEdge, core::kHeaderY,
                          "SURVIVORS", b, core::kNamePt);
            static const char K3[3] = { 'H', 'U', 'S' };
            for (int k = 0; k < 3; ++k) {
                char p[64];
                std::snprintf(p, sizeof p, "anim-group.vote.vs%cN", K3[k]);
                std::snprintf(b, sizeof b, "%d | %d", v3[k], tot);
                Txt(p, b);
                std::snprintf(p, sizeof p, "anim-group.vote.vs%cF", K3[k]);
                Bar(p, core::kRightCol, core::kRightEdge,
                    510.0f + 96.0f * static_cast<float>(k) + 36.0f + 3.0f,
                    core::SurvivorFrac(v3[k], tot));
            }
            std::snprintf(b, sizeof b, "%d", c.need);
            Txt("anim-group.vote.voteNeeded", b);
        }

        void Prompts(const core::Vote& m)
        {
            const int key = core::PromptKeyOf(m, engine::menu_input::PadMode());
            if (g.promptKey == key) return;
            g.promptKey = key;
            const bool grid = (key & 2) != 0, can = (key & 4) != 0;
            const nrui::Prompt items[2] = {
                { grid ? "W A S D" : "A / D", "menu:prev+menu:next", "SWITCH", true },
                { "menu:select", "menu:select", "VOTE", can }
            };
            nrui::Prompts(items, 2);
        }

        // ---------------------------------------------------- stage 2: boosters

        // The dimmed map behind the booster / loading page is the winning map
        // byte the vote wrote (ext mf_backdrop(o[0x18])).
        int LobbyMap() noexcept
        {
            const uint8_t* o = engine::lobby::Object();
            if (!o) return 0;
            const uintptr_t p = reinterpret_cast<uintptr_t>(o) + 0x18;
            return engine::ptr::Readable(p, 1)
                ? static_cast<int>(*reinterpret_cast<const volatile uint8_t*>(p)) : 0;
        }

        void Backdrop()
        {
            char b[192];
            core::MapImagePath(b, sizeof b, LobbyMap());
            Img("anim-group.mapBg", b);
        }

        // winmm's match state (lobby object +0x2b80, NOT netctx+0x2b80):
        // 2 lobby / searching, 3 level loaded, 4 waiting-for-players,
        // 5 ready sent, 6 started. The ext uses this for the loading hold.
        int LobbyMatchState() noexcept
        {
            const uint8_t* o = engine::lobby::Object();
            if (!o) return -1;
            const uintptr_t p = reinterpret_cast<uintptr_t>(o) + 0x2b80;
            return engine::ptr::Readable(p, 1)
                ? static_cast<int>(*reinterpret_cast<const volatile uint8_t*>(p)) : -1;
        }

        void PadHint()
        {
            if (g.hintB == (engine::menu_input::PadMode() ? 1 : 0)) return;
            g.hintB = engine::menu_input::PadMode() ? 1 : 0;
            Vis("anim-group.booster.boHint", g.hintB == 0);
            Vis("anim-group.booster.boHintPad", g.hintB == 1);
        }

        // ui_text_db: write the hidden twin, swap in after DB_SETTLE frames so
        // the font is built before the text is shown.
        void BoosterSwap(uint64_t frame)
        {
            if (!g.boPending || frame - g.boAt < kDbSettle) return;
            g.boPending = false;
            g.boShownB = !g.boShownB;
        }

        void BoosterTimer(int secs, uint64_t frame)
        {
            char t[160];
            core::BoosterTitleText(t, sizeof t, secs);
            const int hidden = g.boShownB ? 0 : 1;
            char* slot = g.boText[hidden];
            if (std::strcmp(t, slot) != 0) {
                Txt(hidden ? "anim-group.booster.boTitleB"
                           : "anim-group.booster.boTitle", t);
                std::snprintf(slot, 160, "%s", t);
                g.boPending = true;
                g.boAt = frame;
            }
            BoosterSwap(frame);
        }

        void BoosterTimerVisible(bool on)
        {
            const char* shown = g.boShownB ? "anim-group.booster.boTitleB"
                                           : "anim-group.booster.boTitle";
            const char* hidden = g.boShownB ? "anim-group.booster.boTitle"
                                            : "anim-group.booster.boTitleB";
            Vis(shown, on);
            Vis(hidden, false);
        }

        // mf_booster_fill: slot 0 = Nothing, then the owned boosters. A queued
        // pick is shown as CHOSEN before the 0xE9 reply confirms it.
        void BoosterFill()
        {
            core::Meta m = g.meta;
            if (g.boWant >= 0 && g.metaHave) m.bp = g.boWant;
            g.boN = core::BoosterList(m, g.boIds);
            int slot = g.bfocus;
            if (slot < 0 || slot >= g.boN) slot = 0;
            const int focusId = core::BoosterSlotId(g.boIds, g.boN, slot);

            char p[128], b[192], confirm[160];
            for (int s = 0; s < 8; ++s) {
                std::snprintf(p, sizeof p, "anim-group.booster.boRow%d", s);
                Vis(p, s < g.boN);
                if (s >= g.boN) continue;
                const int id = g.boIds[s];
                std::snprintf(p, sizeof p, "anim-group.booster.boRow%d.boSel%d", s, s);
                Vis(p, s == slot);
                std::snprintf(p, sizeof p, "anim-group.booster.boRow%d.boName%d", s, s);
                Txt(p, core::kBoosters[id].name);
                std::snprintf(p, sizeof p, "anim-group.booster.boRow%d.boIcon%d", s, s);
                core::BoosterIconPath(b, sizeof b, id);
                Img(p, b);
                if (s) {
                    std::snprintf(p, sizeof p, "anim-group.booster.boRow%d.boCount%d", s, s);
                    core::BoosterCountText(b, sizeof b, m.bc[id]);
                    Txt(p, b);
                }
            }
            Txt("anim-group.booster.boBigName", core::kBoosters[focusId].name);
            Txt("anim-group.booster.boDesc", core::kBoosters[focusId].desc);
            core::BoosterIconPath(b, sizeof b, focusId);
            Img("anim-group.booster.boBig", b);
            core::BoosterConfirmText(confirm, sizeof confirm, m, focusId, g.boN);
            Txt("anim-group.booster.boSelText", confirm);
        }

        // One 0xE8 poll / 0xEE pick exchange, plus the meta-version-driven
        // confirmation and focus preservation from feature_matchflow.
        void BoosterRpc(uint64_t now)
        {
            char text[kReplyCap];
            if (g.boWant >= 0 && now >= g.boNext && !g.busy) {
                const uint8_t id = static_cast<uint8_t>(g.boWant);
                g.busy = true;
                bool sent = false;
                const bool ok = Ask(kOpBoosterPick, &id, 1, text, sizeof text,
                                    &sent, kOpMetaReply);
                g.busy = false;
                if (ok && core::ParseMeta(g.meta, text)) {
                    g.metaHave = true;
                    ++g.metaVer;
                    LsNoteMeta(now);
                }
                // ext bo_next: retry a queued pick every 900 ms until a meta
                // reply carries it; a request that never went out retries now.
                g.boNext = sent ? now + kBoRetryMs : now;
                g.bPoll = now + kMetaPollMs;
            } else if (g.boWant < 0 && now >= g.bPoll && !g.busy) {
                g.busy = true;
                const bool ok = Ask(kOpMetaRead, nullptr, 0, text, sizeof text,
                                    nullptr, kOpMetaReply);
                g.busy = false;
                if (ok && core::ParseMeta(g.meta, text)) {
                    g.metaHave = true;
                    ++g.metaVer;
                    LsNoteMeta(now);
                }
                g.bPoll = now + kMetaPollMs;
            }

            if (static_cast<int32_t>(g.metaVer) != g.bVer) {
                g.bVer = static_cast<int32_t>(g.metaVer);
                if (g.boWant >= 0 && g.metaHave && g.meta.bp == g.boWant) {
                    TO1_LOG("mfnr: booster %d confirmed by the daemon\n", g.boWant);
                    g.boWant = -1;
                }
                core::Meta m = g.meta;
                if (g.boWant >= 0 && g.metaHave) m.bp = g.boWant;
                const int n = core::BoosterList(m, g.boIds);
                const int keep = core::BoosterSlotId(g.boIds, g.boN, g.bfocus);
                g.boN = n;
                g.bfocus = core::BoosterSlotOf(g.boIds, g.boN, keep);
                BoosterFill();
            }
        }

        // ---------------------------------------------------------------- input

        int HoverCell(const core::Vote& m)
        {
            const bool grid = core::GridMode(m);
            const int n = grid ? std::min(m.n, core::kGrid)
                               : std::min(m.n, core::kTiles);
            char p[96];
            for (int k = 0; k < n; ++k) {
                if (grid)
                    std::snprintf(p, sizeof p, "anim-group.vote.voteGrid.g%d", k);
                else
                    std::snprintf(p, sizeof p, "anim-group.vote.voteTiles.tile%d", k);
                if (engine::widget::MouseOver(W(p), true)) return k;
            }
            return -1;
        }

        bool ClickHit(const core::Vote& m)
        {
            if (core::PromptHit(core::PromptKeyOf(m,
                    engine::menu_input::PadMode()), nrui::PromptHover()))
                return true;
            return HoverCell(m) >= 0;
        }

        void Input(const core::Vote& m)
        {
            const bool selRaw = engine::menu_input::CommandEdge(
                SID("menu-select"), g.hSel);
            bool left = engine::menu_input::CommandEdge(SID("menu-left"), g.hLeft)
                || engine::menu_input::CommandEdge(SID("menu-prev-tab"), g.hPrev);
            bool right = engine::menu_input::CommandEdge(SID("menu-right"), g.hRight)
                || engine::menu_input::CommandEdge(SID("menu-next-tab"), g.hNext);
            const bool up = engine::menu_input::CommandEdge(SID("menu-up"), g.hUp)
                || engine::menu_input::CommandEdge(SID("menu-prev-tab"), g.hPrev);
            const bool down = engine::menu_input::CommandEdge(SID("menu-down"), g.hDown)
                || engine::menu_input::CommandEdge(SID("menu-next-tab"), g.hNext);

            // The ext's shared click gate: a mouse-sourced select is dropped
            // unless the cursor is over a tile / grid cell or the VOTE prompt.
            // t2r-no-click-gate.txt is read-only compatibility input.
            const bool gated = engine::flag::Absent("t2r-no-click-gate.txt");
            const bool sel = selRaw
                && !(gated && engine::menu_input::SelectIsMouse() && !ClickHit(m));

            if (m.ok && g.focus < 0 && m.n > 0) {
                g.focus = m.my >= 0 ? m.my : 0;
                g.lastVer = ~uint32_t{ 0 };
            }
            if (m.ok && m.st == 1 && m.n > 0) {
                if (left || right) {
                    g.focus = (g.focus + (right ? 1 : m.n - 1)) % m.n;
                    g.lastVer = ~uint32_t{ 0 };
                }
                if (m.n > core::kTiles && (up || down)) {
                    const int f2 = g.focus + (down ? core::kGridCols
                                                  : -core::kGridCols);
                    if (f2 >= 0 && f2 < m.n) {
                        g.focus = f2;
                        g.lastVer = ~uint32_t{ 0 };
                    }
                }
                const int hv = HoverCell(m);
                if (hv >= 0 && hv < m.n && hv != g.focus) {
                    g.focus = hv;
                    g.lastVer = ~uint32_t{ 0 };
                }
                if (sel && g.focus >= 0) {
                    g.wantVote = g.focus;
                    g.resends = 0;
                    g.sendAt = 0;
                }
            }
            (void)up;
            (void)down;
        }

        int HoverBoosterRow()
        {
            for (int s = 0; s < g.boN && s < 8; ++s) {
                char p[128];
                std::snprintf(p, sizeof p,
                              "anim-group.booster.boRow%d.boName%d", s, s);
                if (engine::widget::MouseOver(W(p), true)) return s;
            }
            return -1;
        }

        // Stage 2 uses the same SWITCH / CONFIRM gate as the vote. `sel` is
        // already click-gated by Input(); booster rows are the clickable area.
        void BoosterInput(bool sel)
        {
            bool up = engine::menu_input::CommandEdge(SID("menu-up"), g.hUp)
                   || engine::menu_input::CommandEdge(SID("menu-prev-tab"), g.hPrev);
            bool down = engine::menu_input::CommandEdge(SID("menu-down"), g.hDown)
                     || engine::menu_input::CommandEdge(SID("menu-next-tab"), g.hNext);
            if (g.boN > 0 && (up || down)) {
                g.bfocus = (g.bfocus + (down ? 1 : g.boN - 1)) % g.boN;
                BoosterFill();
            }
            const int hv = HoverBoosterRow();
            if (hv >= 0 && hv < g.boN) {
                if (hv != g.bfocus) {
                    g.bfocus = hv;
                    BoosterFill();
                }
                // ext mouse highlight override: the hovered row is the one
                // selected, so a click picks the row under the cursor.
                for (int s = 0; s < 8; ++s) {
                    char p[128];
                    std::snprintf(p, sizeof p,
                                  "anim-group.booster.boRow%d.boName%d", s, s);
                    const bool on = s < g.boN && engine::widget::MouseOver(W(p), true);
                    std::snprintf(p, sizeof p,
                                  "anim-group.booster.boRow%d.boSel%d", s, s);
                    Vis(p, on);
                }
            }
            if (sel && g.boN > 0) {
                const int id = core::BoosterSlotId(g.boIds, g.boN, g.bfocus);
                const bool owned = !id || (g.metaHave && g.meta.bc[id] > 0);
                if (owned) {
                    g.boWant = id;
                    g.boNext = 0;
                    AUDIO::PLAY_SOUND(SID("sfx-select"));
                    TO1_LOG("mfnr: booster %d chosen\n", id);
                    BoosterFill();
                }
            }
        }

        // --------------------------------------------------- stage 3: loading

        // Own the engine's level-load screen. Swallowing only the engine's own
        // disable (while we own it) leaves its enable / locked wrapper intact;
        // our calls set `lsSelf` so they pass straight through.
        void __fastcall LsSetEnabledHook(void* inst, uint64_t on)
        {
            const bool swallow = g.lsOwn != 0 && !g.lsSelf && (on & 0xff) == 0;
            if (swallow) return;
            gLsHook.fastcall<void>(inst, on);
        }

        void EnsureLsHook()
        {
            if (gLsHook || gLsHookTried) return;
            // ExtLoaded() settles a frame or two after boot; never stack a
            // second detour on the ext's already-patched prologue.
            if (engine::frame::Count() < 3 || boot::ExtLoaded()) return;
            gLsHookTried = true;
            const uintptr_t fn = ModuleBase() + kLsSetEnabledRva;
            if (!engine::ptr::Readable(fn, sizeof kLsPrologue)
                || std::memcmp(reinterpret_cast<const void*>(fn), kLsPrologue,
                               sizeof kLsPrologue) != 0) {
                TO1_LOG("mfnr: LoadingScreen::SetEnabled prologue differs - native LOADING screen stays off\n");
                return;
            }
            gLsHook = safetyhook::create_inline(
                reinterpret_cast<void*>(fn),
                reinterpret_cast<void*>(&LsSetEnabledHook));
            if (!gLsHook) {
                TO1_LOG("mfnr: LoadingScreen::SetEnabled hook failed - native LOADING screen stays off\n");
                return;
            }
            const uintptr_t call = ModuleBase() + kLsWrapRva + kLsWrapperCallOff;
            if (!engine::ptr::Readable(call, sizeof kLsWrapperCall)
                || std::memcmp(reinterpret_cast<const void*>(call), kLsWrapperCall,
                               sizeof kLsWrapperCall) != 0) {
                // Without the locked wrapper we cannot safely take the screen.
                g.lsTrampOnly = true;
                TO1_LOG("mfnr: SetEnabled wrapper differs - native LOADING screen stays off\n");
            }
            TO1_LOG("mfnr: LoadingScreen::SetEnabled hooked\n");
        }

        void LsEnable(bool on)
        {
            const uintptr_t wrap = ModuleBase() + kLsWrapRva;
            const uintptr_t inst = ModuleBase() + kLsInstRva;
            if (!engine::ptr::Readable(wrap, 16) || !engine::ptr::Readable(inst, 1))
                return;
            g.lsSelf = true;
            if (on) {
                // A new loading widget starts from its template: no twin is
                // shown yet, so the first text of each pair goes on directly.
                gLsTipCat = DbSlot{};
                gLsTip = DbSlot{};
                gLsSpin = DbSlot{};
            }
            reinterpret_cast<void(__fastcall*)(void*, uint64_t)>(wrap)(
                reinterpret_cast<void*>(inst), on ? 1ULL : 0ULL);
            g.lsSelf = false;
        }

        // inst+0x28 slot, inst+0x30 gen, slot+8 widget; the widget's own gen is
        // at +0x94 and its resolved path sid is at +0xc0. Any unreadable link
        // means "no widget" (a game update, or the engine tore it down).
        uint64_t LsRoot()
        {
            const uintptr_t inst = ModuleBase() + kLsInstRva;
            if (!engine::ptr::Readable(inst, 0x38)) return 0;
            const uintptr_t slot = *reinterpret_cast<const uintptr_t*>(inst + 0x28);
            const int32_t gen = *reinterpret_cast<const int32_t*>(inst + 0x30);
            if (!slot || !gen || !engine::ptr::Readable(slot, 0x10)) return 0;
            const uintptr_t w = *reinterpret_cast<const uintptr_t*>(slot + 8);
            if (!w || !engine::ptr::Readable(w, 0xc8)) return 0;
            if (*reinterpret_cast<const int32_t*>(w + 0x94) != gen) return 0;
            return *reinterpret_cast<const uint64_t*>(w + 0xc0);
        }

        uint64_t LsW(const char* rel)
        {
            const uint64_t root = LsRoot();
            if (!root || !rel) return 0;
            const uint64_t w = DC::Call<"gui2-build-widget-path">(
                root, reinterpret_cast<uint64_t>(rel),
                0, 0, 0, 0, 0, 0, 0);
            return w == ~uint64_t{ 0 } ? 0 : w;
        }

        void LsVis(const char* rel, bool on) { engine::widget::Visible(LsW(rel), on); }
        void LsTxt(const char* rel, const char* text) { engine::widget::SetText(LsW(rel), text ? text : ""); }
        void LsImg(const char* rel, const char* path) { engine::widget::SetImage(LsW(rel), path); }

        uint64_t HashText(const char* s)
        {
            uint64_t h = 1469598103934665603ULL;
            for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
                 s && *p; ++p) {
                h ^= *p;
                h *= 1099511628211ULL;
            }
            return h;
        }

        void LsDbText(DbSlot& slot, const char* aName, const char* bName,
                      const char* text, uint64_t frame)
        {
            const uint64_t a = LsW(aName);
            if (!a) return;
            const uint64_t b = LsW(bName);
            if (slot.a != a || slot.b != b) {
                slot = DbSlot{};
                slot.a = a;
                slot.b = b;
            }
            const uint64_t h = HashText(text);
            if (slot.hash != h) {
                slot.hash = h;
                if (slot.fresh || !b) {
                    slot.fresh = false;
                    slot.shownB = false;
                    slot.pending = false;
                    engine::widget::SetText(a, text);
                    engine::widget::Visible(a, true);
                    if (b) engine::widget::Visible(b, false);
                    return;
                }
                engine::widget::SetText(slot.shownB ? a : b, text);
                slot.pending = true;
                slot.at = frame;
            }
            if (slot.pending && frame - slot.at >= static_cast<uint64_t>(kDbSettle)) {
                slot.pending = false;
                slot.shownB = !slot.shownB;
                engine::widget::Visible(slot.shownB ? b : a, true);
                engine::widget::Visible(slot.shownB ? a : b, false);
            }
        }

        // The stage-2 replies already publish the meta state; only an owner
        // that came up without a booster stage needs its own 0xE8 read.
        void LsEnsureMeta(uint64_t now)
        {
            if (g.lsMetaHave && now - g.lsMetaAt < kLsWatchdogMs) return;
            if (now < g.lsMetaTry) return;
            g.lsMetaTry = now + 5000;
            char text[kReplyCap];
            if (Ask(kOpMetaRead, nullptr, 0, text, sizeof text, nullptr,
                    kOpMetaReply) && core::ParseMeta(g.lsMeta, text)) {
                g.lsMetaHave = true;
                g.lsMetaAt = now;
            }
        }

        // mf_loading_fill: the map, the tip and its framed shot, and the
        // running daily mission (or a short next-event line) on the engine's
        // loading widget. All paths are relative to the spawned "loading" root.
        void LsFill(int map, int tip, uint64_t frame)
        {
            char b[192], p[128], q[128];
            core::MapImagePath(b, sizeof b, map);
            LsImg("t2r.mapBg", b);

            // Keep the previous picture up while the next one streams in; the
            // CLAN tip rotates its three framed shots.
            char pic[96];
            core::TipPicPath(pic, sizeof pic, tip, g.lsRot);
            const bool multi = tip >= 0 && tip < core::kTipCount
                && core::kTips[tip].npic > 1;
            if (multi) ++g.lsRot;
            LsImg("t2r.loading.ldPicOld", g.lsLastPic[0] ? g.lsLastPic : pic);
            LsImg("t2r.loading.ldPic", pic);
            std::snprintf(g.lsLastPic, sizeof g.lsLastPic, "%s", pic);

            LsTxt("t2r.loading.ldMap", core::MapName(map));
            LsTxt("t2r.loading.ldMode", core::ModeLabel());
            char cat[64];
            core::TipCatText(cat, sizeof cat, tip);
            LsDbText(gLsTipCat, "t2r.loading.ldTipCat", "t2r.loading.ldTipCatB",
                     cat, frame);
            LsDbText(gLsTip, "t2r.loading.ldTip", "t2r.loading.ldTipB",
                     tip >= 0 && tip < core::kTipCount ? core::kTips[tip].text : "",
                     frame);

            const core::Meta& m = g.lsMetaHave ? g.lsMeta : g.meta;
            int day = -1;
            if (net::clan::HaveState()) {
                const net::clan::State& c = net::clan::Get();
                if (c.ok) day = c.day;
            }
            if (m.ok && core::MissionValid(m.mi)) {
                char line[160];
                core::MissionLine(line, sizeof line, m, m.mi);
                LsTxt("t2r.loading.ldMis1", line);
                core::MissionDayLine(line, sizeof line, m);
                LsTxt("t2r.loading.ldMis2", line);
                const int es = day >= 0 ? core::EventWinStart(day) : -1;
                const int ek = es >= 0 ? core::EventKindAt(es) : 0;
                for (int k = 0; k < 3; ++k) {
                    std::snprintf(p, sizeof p, "t2r.loading.ldTier%d", k);
                    std::snprintf(q, sizeof q, "t2r.loading.ldTier%dPct", k);
                    core::MissionTierLine(b, sizeof b, m, m.mi, k);
                    LsTxt(p, b);
                    std::snprintf(p, sizeof p, "t2r.loading.ldTier%dIcon", k);
                    LsVis(p, true);
                    if (!m.mev || ek == 0)
                        LsTxt(q, "");
                    else if (ek == 1)
                        LsTxt(q, core::EventGrowthPct(k));
                    else {
                        std::snprintf(b, sizeof b, "-%d%%",
                                      core::EventLossPct(es, k + 1));
                        LsTxt(q, b);
                    }
                }
            } else {
                char line[96];
                LsTxt("t2r.loading.ldMis1", "No mission running");
                core::NextEventLine(line, sizeof line, day,
                                    day >= 0 ? core::EventNextStart(day) : -1);
                LsTxt("t2r.loading.ldMis2", line);
                for (int k = 0; k < 3; ++k) {
                    std::snprintf(p, sizeof p, "t2r.loading.ldTier%d", k);
                    LsTxt(p, "");
                    std::snprintf(q, sizeof q, "t2r.loading.ldTier%dPct", k);
                    LsTxt(q, "");
                    std::snprintf(p, sizeof p, "t2r.loading.ldTier%dIcon", k);
                    LsVis(p, false);
                }
            }
        }

        bool LsTake(bool first, uint64_t now, uint64_t frame)
        {
            if (g.lsOwn || !gLsHook || g.lsTrampOnly) return false;
            const uintptr_t inst = ModuleBase() + kLsInstRva;
            g.lsOwn = 1;
            // An enabled instance with no widget means the engine tore it
            // down: disable first so the wrapper can spawn a fresh one.
            if (engine::ptr::Readable(inst, 1)
                && *reinterpret_cast<const uint8_t*>(inst) && !LsRoot())
                LsEnable(false);
            LsEnable(true);
            const uint64_t root = LsRoot();
            if (!root) {
                // The engine refused (its own "no loading screen" conditions):
                // retry every frame until the level-load begin allows it.
                g.lsOwn = 0;
                g.lsWant = true;
                if (first)
                    TO1_LOG("mfnr: SetEnabled(1) gave no loading widget yet - retrying\n");
                return false;
            }
            g.lsWant = false;
            g.lsTakeAt = now;
            g.lsTipAt = now;
            g.lsDotsAt = 0;
            g.lsDoneTake = 0;
            g.lsRot = 0;
            g.lsLastPic[0] = 0;
            g.lsTip = core::TipIndex(now);
            LsEnsureMeta(now);
            LsVis("fader", false);       // stock spinner / progress text
            LsVis("t2r", true);
            engine::widget::Alpha(LsW("t2r"), 0.0f);
            LsFill(LobbyMap(), g.lsTip, frame);
            TO1_LOG("mfnr: LOADING up on the engine screen (map %d, tip %d)\n",
                    LobbyMap(), g.lsTip);
            return true;
        }

        void LsRelease(const char* why, uint64_t now)
        {
            if (g.lsOwn != 1) return;
            g.lsOwn = 2;
            g.lsRelAt = now;
            // Black under the group first, then the group fades out over it.
            DC::Call<"fade-to-black">(static_cast<float>(core::kBlackMs) / 1000.0f,
                                      2.0f, 0.0f, 0.0f);
            TO1_LOG("mfnr: LOADING release (%s)\n", why);
        }

        void LsForceOff()
        {
            g.hornAt = 0;
            g.hornScene = 0;
            if (!g.lsOwn) return;
            g.lsOwn = 0;
            g.lsWant = false;
            g.lsReqAt = 0;
            LsEnable(false);
        }

        void LsMix()
        {
            static bool missing = false;
            if (missing) return;
            const uint64_t r = DC::Call<"set-per-frame-mixer-override">(
                SID("mix-rl-under-black-duck-all-except-music"));
            if (r == ~uint64_t{ 0 }) missing = true;
        }

        void LsTick(uint64_t now, uint64_t frame, int phase, int sub)
        {
            if (!gLsHook || g.lsTrampOnly) return;
            if (g.lsOwn == 0) {
                if (g.lsWant) LsTake(false, now, frame);
                return;
            }
            if (g.lsOwn == 1) {
                if (phase != 2 || sub < 2)
                    LsRelease("back on the menu", now);
                else if (sub >= 4)
                    LsRelease("match started", now);
                else if (now - g.lsTakeAt > kLsWatchdogMs)
                    LsRelease("load watchdog", now);
                else if (!LsRoot()) {
                    // The widget was torn down under the engine's handle.
                    g.lsOwn = 0;
                    g.lsWant = true;
                    LsTake(false, now, frame);
                    return;
                }
            }
            LsMix();
            if (g.lsOwn == 1) {
                // The fade-in writes alpha once: a hitchy load must not leave
                // the group half transparent with the camera showing through.
                if (g.lsDoneTake != g.lsTakeAt) {
                    const uint64_t ft = now - g.lsTakeAt;
                    const float a = ft >= core::kFadeMs ? 1.0f
                        : static_cast<float>(ft) / static_cast<float>(core::kFadeMs);
                    engine::widget::Alpha(LsW("t2r"), a);
                    if (ft >= core::kFadeMs) g.lsDoneTake = g.lsTakeAt;
                }
                if (now - g.lsDotsAt >= core::kSpinMs) {
                    g.lsDotsAt = now;
                    char ld[24];
                    core::SpinText(ld, sizeof ld, now);
                    LsDbText(gLsSpin, "t2r.loading.ldSpin", "t2r.loading.ldSpinB",
                             ld, frame);
                }
                if (now - g.lsTipAt > core::kTipMs) {
                    g.lsTipAt = now;
                    g.lsTip = core::TipNext(g.lsTip);
                    LsFill(LobbyMap(), g.lsTip, frame);
                }
                return;
            }
            const uint64_t t = now - g.lsRelAt;
            engine::widget::Alpha(LsW("t2r"), core::ReleaseAlpha(t));
            if (t >= core::kBlackMs + core::kFadeMs) {
                g.lsOwn = 0;
                g.lsWant = false;
                LsEnable(false);
                TO1_LOG("mfnr: LOADING off (%llu ms after release)\n",
                        static_cast<unsigned long long>(t));
            }
        }

        // ----------------------------------------------------------- page skeleton

        uint64_t Page() { return engine::widget::PageId("matchflow"); }

        bool Disabled()
        {
            return boot::ExtLoaded()
                || engine::flag::Present("t2r-no-matchflow.txt");
        }

        int Code()
        {
            if (Disabled()) return 0;
            if (engine::lobby::MatchPhase() != 2 || engine::lobby::InMatch())
                return 0;
            return engine::lobby::FlowCode();
        }

        void Pop(uint64_t top)
        {
            if (top != Page()) return;
            DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
            g.pushAt = 0;
        }

        // The loadout / redesign pages a user may have opened while searching.
        // The ext closes them one per frame to push the flow page; this owner is
        // deliberately conservative and only waits for them to go away so it
        // cannot pop another owner's live page.
        bool LoadoutTop(uint64_t top)
        {
            static const char* const pages[] = {
                "gearup13", "loadout-menu", "picker-skill", "picker-primary",
                "picker-secondary", "picker-purchase", "customize",
                "customize-challenges", "clan13", "clan13x", "leaderboard",
                "clanach", "pause-menu", "options-menu-multiplayer", "name-entry"
            };
            for (const char* p : pages)
                if (top == engine::widget::PageId(p)) return true;
            return false;
        }

        void EnterPage(uint64_t top)
        {
            if (g.lastTop == top) return;
            g.lastTop = top;
            g.hSel = g.hLeft = g.hRight = g.hUp = g.hDown = true;
            g.hPrev = g.hNext = true;
            g.stageShown = -1;
            g.promptKey = -1;
            engine::widget::Alpha(W("anim-group"), 1.0f);
        }

    } // namespace

    void Install(void* gameModule)
    {
        gModule = gameModule;
    }

    void Tick(uint64_t now)
    {
        if (Disabled()) return;
        EnsureLsHook();

        const int code = Code();
        const int phase = engine::lobby::MatchPhase();
        const int sub = LobbyMatchState();
        const bool preload = phase == 2 && (sub == 2 || sub == 3);
        const uint64_t frame = engine::frame::Count();
        const uint64_t top = engine::widget::TopPage();
        const int stage = core::StageOf(code);

        // Minimal native stand-in for the ext match-horn's LOADING request.
        // The ext fires when the flow's raw +0x1b8 `secs` (having been through
        // a stage >= 100) falls to <= 1 in the pre-load lobby (phase 2, state
        // 2), then waits MH_POP_MS before asking for the engine's screen. No
        // new input hook, horn audio or page is added; a cancel drops it.
        if (phase != 2) g.sawCd = false;
        else if (core::FlowActive(code)) g.sawCd = true;
        if (core::HornWant(g.sawCd, code, phase, sub)
            && !g.lsWant && !g.lsReqAt) {
            g.sawCd = false;
            g.lsReqAt = now + kLsRequestMs;
            g.hornAt = now;
            g.hornScene = HornScene();
            TO1_LOG("mfnr: match horn t=0 (scene %d)\n", g.hornScene);
        }
        // The LOADING latch only survives the code falling to 0; leaving the
        // pre-load lobby still cancels it. The horn envelope is longer: hold it
        // until the match starts or the ext's MH_MAX_MS passes, so the send-off
        // keeps its own 7.6 s timeline without a second trigger.
        if (g.lsReqAt && core::HornCanceled(phase, sub)) g.lsReqAt = 0;
        if (g.hornAt && (phase != 2 || sub < 2
                         || (sub >= 4 && g.lsOwn != 0)
                         || now - g.hornAt > kHornMaxMs)) {
            g.hornAt = 0;
            g.hornScene = 0;
        }
        if (g.lsReqAt && now >= g.lsReqAt) {
            g.lsWant = true;
            g.lsReqAt = 0;
            LsTake(true, now, frame);
        }
        // A server-side code >= 200 is already the LOADING step: take it at
        // once rather than waiting for the local horn-style latch.
        if (stage == 3 && preload && !g.lsWant && !g.lsOwn) {
            g.lsReqAt = 0;
            g.lsWant = true;
            LsTake(true, now, frame);
        }

        LsTick(now, frame, phase, sub);

        // LOADING is the engine's widget, not a menu page: the level load
        // wipes the page stack. Keep polling only until the vote is decided;
        // the runtime (including the meta snapshot) survives until release.
        if (g.lsWant || g.lsOwn != 0 || stage == 3) {
            Pop(top);
            if (g.sawVote && !core::Decided(g.m)) RpcTick(now, 3);
            return;
        }
        // The ext keeps the booster page through the UI fade and only drops it
        // when the LOADING request fires; hold the runtime the same way.
        if (g.lsReqAt != 0) return;
        if (stage == 0) {
            Pop(top);
            ResetRuntime();
            return;
        }

        g.active = true;
        g.sawVote = true;
        if (top != Page()) {
            if (LoadoutTop(top)) return;
            if (g.pushes < kMaxPushes && now - g.pushAt > kPushGapMs) {
                MENU2::PUSH_PAGE(MenuStack::hudMenuStack, "to1/matchflow");
                g.pushAt = now;
                ++g.pushes;
                TO1_LOG("mfnr: to1/matchflow pushed (code %d, stage %d)\n",
                        code, stage);
            }
            return;
        }

        EnterPage(top);
        if (g.stageShown != stage) {
            g.stageShown = stage;
            StageFrame(stage);
            g.lastSecs = -1;
            g.promptKey = -1;
            if (stage == 1) {
                ClanFill();
            } else {
                // stage 2: fresh counts / pick from the server at once.
                g.bfocus = 0;
                g.boWant = -1;
                g.boNext = 0;
                g.bPoll = 0;
                g.bVer = -1;
                g.hintB = -1;
                BoosterFill();
                Backdrop();
            }
        }

        RpcTick(now, stage);
        if (stage == 1) {
            Prompts(g.m);
            if (g.lastVer != g.version) {
                g.lastVer = g.version;
                VoteFill(g.m, g.focus < 0 ? 0 : g.focus);
            }
            Input(g.m);
            if (g.lastSecs != code) {
                g.lastSecs = code;
                TimerText(core::CountdownFor(code, stage),
                          engine::frame::Count());
            }
            // ext db_tick runs every frame: a twin written on a countdown
            // change must still swap in on the three frames that follow.
            TimerSwap(engine::frame::Count());
        } else {
            BoosterTimer(core::CountdownFor(code, stage),
                         engine::frame::Count());
            BoosterTimerVisible(true);
            PadHint();
            const bool selRaw = engine::menu_input::CommandEdge(
                SID("menu-select"), g.hSel);
            const bool gated = engine::flag::Absent("t2r-no-click-gate.txt");
            const bool sel = selRaw
                && !(gated && engine::menu_input::SelectIsMouse()
                     && HoverBoosterRow() < 0);
            BoosterInput(sel);
            BoosterRpc(now);
        }
    }

    HornState Horn()
    {
        if (Disabled()) return {};
        return HornState{g.hornAt != 0, g.hornAt, g.hornScene};
    }

    void Reset()
    {
        LsForceOff();
        ResetRuntime();
    }

} // namespace net::ui::mfnr
