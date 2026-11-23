// Native home for the post-match / menu latches that hold requeue's settle, and
// the page lifecycle for the two clan screens the mod now owns:
// to1/clan-postmatch and to1/clan-event. Producers ported from tools/ext/t2r-ext.c
// (g_pm.pending / g_ev.pending via net/online/clan-state.*, GEAR UP g_loState,
// g_mmBack's party + Steam gates), clan13.c (Cl13PostActive) and tools/ext/requeue.c's
// post-match screen gate. The ext is still loaded and ticked before this module;
// its page handlers may run first while it is the active bridge. Every push here
// is behind IS_PAGE_ON_STACK, so an ext push is adopted instead of duplicated.

#include "postmatch-screens.hpp"

#include "log.hpp"           // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/frame.hpp"
#include "engine/locale.hpp"
#include "engine/script.hpp" // MENU2 / MenuStack / DC::Call
#include "engine/widget.hpp" // PageId / Page / SetText
#include "net/match/match-liveness.hpp"
#include "net/online/clan-state.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace net::ui::postmatch {

    namespace {

        // ext: `if (g_mmBack && ++g_mmBack > 3) { g_mmBack = 0; ... pop ... }` -- a
        // refused FIND MATCH holds for a few frames, then the search page is popped.
        constexpr int  kMmBackFrames = 3;
        // The mod's own find-match flow (net-gui2.cpp) already pops the page it
        // refuses; popping it again here would eat the page underneath. The ext's
        // menu2-pop-page call is kept as a gated seam for a future cutover.
        constexpr bool kPopOnMmBack  = false;
        constexpr uint64_t kLegacyFmPage = 0x9E33B6D92E193B4DULL; // ext's old matchmaking page

        // ext PM_* / EV_* / CLAN_DOTS constants (tools/features/clan_widgets.py).
        constexpr int kPmSlots = 24;
        constexpr int kPmFade = 1200, kPmTotAt = 1400, kPmCntAt = 2200;
        constexpr int kPmOut = 5;
        constexpr int kDots = 60, kActors = 24;
        constexpr float kEvRx = 1.7f, kEvRy = 0.78f, kEvEdge = 205.0f;
        constexpr int64_t kEvTGo = 1700, kEvTHit = 3300, kEvTEnd = 4000;

        struct State {
            // cached page ids (stable for the session; 0 until the first Tick)
            StringId64 cl13x = 0, postmatch = 0, event = 0;
            StringId64 gearup13 = 0, loadoutMenu = 0;
            StringId64 pickers[4]{};
            StringId64 findMatch = 0, mainMenu = 0;

            // sampled latches
            bool cl13 = false, post = false, eventUp = false;
            bool wasPostTop = false, wasEventTop = false; // page-handler edges
            LoState lo = LoState::None;
            int  mmBack = 0;

            // mod push bookkeeping (the ext pushes first when its latch is set).
            // pmSeenSentAt detects a freshly armed report so the retry counter
            // starts over on a new match (ext PmReport memsets g_pm).
            uint64_t pmPushAt = 0, pmSeenSentAt = 0;
            int      pmPushes = 0;
        } g;

        // ---- widget helpers (ext UiText / UiTextDb / UiImage / UiVisible / UiW) ---
        // The ext's settext hook translated every write; the mod owns its own
        // writes, so translation is applied here through engine::locale instead.
        void UiText(const char* path, const char* text)
        {
            if (!path) return;
            const uint64_t w = engine::widget::Page(path);
            if (!w) return;
            char b[1536];
            std::snprintf(b, sizeof b, "%s", engine::locale::Tr(text ? text : ""));
            engine::widget::SetText(w, b);
        }

        void UiTextDb(const char* path, const char* text)
        {
            UiText(path, text);
            char pb[160];
            if (!path || std::strlen(path) + 1 >= sizeof pb) return;
            std::snprintf(pb, sizeof pb, "%sB", path);
            const uint64_t w = engine::widget::Page(pb);
            if (!w) return;
            char b[1536];
            std::snprintf(b, sizeof b, "%s", engine::locale::Tr(text ? text : ""));
            engine::widget::SetText(w, b);
        }

        void UiImage(const char* path, const char* image)
        {
            if (!path || !image) return;
            engine::widget::SetImage(engine::widget::Page(path), image);
        }

        void UiVisible(const char* path, bool on)
        {
            engine::widget::Visible(engine::widget::Page(path), on);
        }

        void UiAlpha(const char* path, float a)
        {
            engine::widget::Alpha(engine::widget::Page(path), a);
        }

        void PopPage()
        {
            DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
        }

        bool MenuEdge(StringId64 command, bool& held)
        {
            const bool now = DC::JOYPAD_COMMAND_ACTIVE(command);
            const bool edge = now && !held;
            held = now;
            return edge;
        }

        // ext PmText's change filter: the rolling numbers were written to a gui2
        // text every frame and stopped drawing (layout one frame behind), so the
        // ext wrote them every 50 ms / on the first and last value.
        char g_pmLast[kPmSlots][128]{};

        void PmText(int slot, const char* path, const char* text)
        {
            if (slot >= 0 && slot < kPmSlots) {
                if (!std::strcmp(g_pmLast[slot], text ? text : ""))
                    return;
                std::snprintf(g_pmLast[slot], sizeof g_pmLast[slot], "%s", text ? text : "");
            }
            UiTextDb(path, text); // ext PmText writes the double-buffered "...B" twin too
        }

        void PmNum(int slot, const char* path, int v)
        {
            char b[32];
            std::snprintf(b, sizeof b, "%d", v);
            PmText(slot, path, b);
        }

        void PmNumDb(int slot, const char* path, int v)
        {
            char b[32];
            std::snprintf(b, sizeof b, "%d", v);
            PmText(slot, path, b); // same filter / same double-buffered write as ext PmNum
        }

        int PmCount(int v, uint64_t t0, uint64_t now, int ms)
        {
            if (now <= t0) return 0;
            if (now >= t0 + static_cast<uint64_t>(ms)) return v;
            float t = 1.0f - static_cast<float>(now - t0) / static_cast<float>(ms);
            return static_cast<int>(v * (1.0f - t * t * t) + 0.5f);
        }

        // ext ClanDots: a camp circle of drifting survivor dots.
        struct Dots {
            uint64_t g[kDots]{}, c[kDots][3]{};
            float px[kDots]{}, py[kDots]{}, vx[kDots]{}, vy[kDots]{};
            int   state[kDots]{};     // ext ClanDotsT.state: -1 until a dot is first shown
            int   shown = -1;
            uint64_t last = 0;
            float r = 0.0f, mid = 0.0f;
        };

        void RunDots(Dots& d, const net::clan::State& c, bool reset)
        {
            static uint32_t rng = 0x2013f;
            auto RND = [&]() -> uint32_t {
                rng = rng * 1664525u + 1013904223u;
                return (rng >> 8) & 0xffffu;
            };
            const int n = c.h + c.hu + c.s;
            const int dn = n < kDots ? n : kDots;
            int dh = n ? (c.h * dn + n / 2) / n : 0;
            int du = n ? (c.hu * dn + n / 2) / n : 0;
            if (dh + du > dn) du = dn - dh;
            if (c.s && dh + du >= dn && dn) {
                if (du) --du; else --dh;
            }
            if (reset) {
                d.shown = -1;
                for (int k = 0; k < kDots; ++k) d.state[k] = -1;
            }
            for (int k = 0; k < kDots; ++k) {
                const int st = k >= dn ? -1 : k < dh ? 0 : k < dh + du ? 1 : 2;
                if (d.shown < 0 || st != d.state[k]) {
                    if (st >= 0 && (d.shown < 0 || d.state[k] < 0)) {
                        const float a = RND() / 65536.0f * 6.2832f;
                        const float rr = d.r * std::sqrt(RND() / 65536.0f);
                        d.px[k] = rr * std::cos(a);
                        d.py[k] = rr * std::sin(a);
                        const float va = RND() / 65536.0f * 6.2832f;
                        const float sp = 7.0f + RND() / 65536.0f * 9.0f;
                        d.vx[k] = sp * std::cos(va);
                        d.vy[k] = sp * std::sin(va);
                    }
                    engine::widget::Visible(d.g[k], st >= 0);
                    for (int j = 0; j < 3; ++j)
                        engine::widget::Visible(d.c[k][j], st == j);
                    d.state[k] = st;
                }
            }
            d.shown = dn;
            const uint64_t now = GetTickCount64();
            const float dt = d.last && now - d.last < 200
                ? static_cast<float>(now - d.last) / 1000.0f : 0.016f;
            d.last = now;
            for (int k = 0; k < dn; ++k) {
                if (!(RND() & 63)) {
                    const float t = (RND() / 65536.0f - 0.5f) * 1.2f;
                    const float cs = std::cos(t), sn = std::sin(t);
                    const float x = d.vx[k] * cs - d.vy[k] * sn;
                    d.vy[k] = d.vx[k] * sn + d.vy[k] * cs;
                    d.vx[k] = x;
                }
                d.px[k] += d.vx[k] * dt;
                d.py[k] += d.vy[k] * dt;
                const float d2 = d.px[k] * d.px[k] + d.py[k] * d.py[k];
                if (d2 > d.r * d.r) {
                    const float dd = std::sqrt(d2);
                    const float nx = d.px[k] / dd, ny = d.py[k] / dd;
                    const float dot = d.vx[k] * nx + d.vy[k] * ny;
                    if (dot > 0) {
                        d.vx[k] -= 2.0f * dot * nx;
                        d.vy[k] -= 2.0f * dot * ny;
                    }
                    d.px[k] = nx * d.r;
                    d.py[k] = ny * d.r;
                }
                engine::widget::Pivot(d.g[k], d.mid + d.px[k], d.mid + d.py[k]);
            }
        }

        // ---- to1/clan-postmatch -------------------------------------------------
        struct PmUi {
            int stage = 0, started = 0, fin = 0, skipped = 0;
            int refill = 0, sndCnt = 0;
            bool hSel = true, hBack = true;
            uint64_t t0 = 0, backAt = 0, outAt = 0, autoAt = 0, setAt = 0;
            float curA = 0.0f;
            uint64_t lastFrame = 0;
            Dots dots;
        } g_pm;

        const char* const kSideFull[3] = {
            "", "WASHINGTON LIBERATION FRONT", "SERAPHITES",
        };

        int PmFirstRow(int n)
        {
            return engine::widget::Page("anim-group.earned.pmHeroBack") ? 0 : net::clan::kPmRows - n;
        }

        void UiPrompt(bool next);

        void PmFill(const net::clan::State& c)
        {
            std::memset(g_pmLast, 0, sizeof g_pmLast);
            const int day = net::clan::PostDay();
            const int d = day < 84 ? (day < 0 ? 0 : day) : 83;
            const int fac = c.fac >= 1 && c.fac <= 2 ? c.fac : 1;
            char b[128], p[128];
            std::snprintf(b, sizeof b, "WEEK %d: DAY %d", d / 7 + 1, d % 7 + 1);
            UiText("anim-group.pmTitle", b);
            UiText("anim-group.pmFaction", kSideFull[fac]);
            UiText("anim-group.earned.pmPartsLabel", "PARTS EARNED");
            UiText("anim-group.convert.pmConvLabel", "CONVERTED:");
            UiText("anim-group.result.pmDailyLabel", "DAILY SUPPLIES:");
            UiText("anim-group.result.pmGainLabel", "SURVIVORS GAINED:");
            UiImage("anim-group.pmEmblemW", "streaming/t2r/clan/fig-wlf.png");
            UiImage("anim-group.pmEmblemS", "streaming/t2r/clan/fig-ser.png");
            UiVisible("anim-group.pmEmblemW", fac == 1);
            UiVisible("anim-group.pmEmblemS", fac == 2);
            static const char* const kIcons[][2] = {
                { "anim-group.earned.pmPartsGear", "gear" },
                { "anim-group.earned.pmPartsIcon", "gear" },
                { "anim-group.convert.pmConvGear", "gear" },
                { "anim-group.convert.pmConvIcon", "supply" },
                { "anim-group.result.pmDailyIcon", "supply" },
                { "anim-group.result.pmGainIcon", "person" },
            };
            for (const auto& icon : kIcons) {
                std::snprintf(b, sizeof b, "streaming/t2r/icons/%s.png", icon[1]);
                UiImage(icon[0], b);
            }
            int n = 0;
            const net::clan::PmRow* rows = net::clan::PmRows(&n);
            const int first = PmFirstRow(n);
            for (int k = 0; k < net::clan::kPmRows; ++k) {
                std::snprintf(p, sizeof p, "anim-group.earned.rows.pmRow%d", k);
                UiVisible(p, k >= first);
                std::snprintf(p, sizeof p, "anim-group.earned.rows.pmRow%d.pmRow%dIcon", k, k);
                UiImage(p, "streaming/t2r/icons/gear.png");
                if (k < first) continue;
                const net::clan::PmRow& r = rows[k - first];
                std::snprintf(p, sizeof p, "anim-group.earned.rows.pmRow%d.pmRow%dLabel", k, k);
                UiText(p, r.label);
                std::snprintf(p, sizeof p, "anim-group.earned.rows.pmRow%d.pmRow%dCount", k, k);
                std::snprintf(b, sizeof b, "x%d", r.count);
                UiText(p, b);
                std::snprintf(p, sizeof p, "anim-group.earned.rows.pmRow%d.pmRow%dNum", k, k);
                UiText(p, "0");
            }
            PmNumDb(net::clan::kPmRows, "anim-group.earned.pmPartsNum", 0);
            PmNumDb(net::clan::kPmRows + 2, "anim-group.convert.pmConvNum", 0);
            UiAlpha("anim-group", 0.0f);
            UiAlpha("pmBg", 1.0f);
            UiAlpha("anim-group.earned.rows", 1.0f);
            UiVisible("anim-group.earned", true);
            UiVisible("anim-group.convert", true);
            UiVisible("anim-group.result", false);
            UiVisible("anim-group.convert.pmWait", false);
            UiVisible("anim-group.convert.pmLeft", false);
            UiPrompt(true);
            g_pm.dots.r = 190.0f;
            g_pm.dots.mid = 215.0f;
            for (int k = 0; k < kDots; ++k) {
                std::snprintf(p, sizeof p, "anim-group.result.pmCircle.pmDot%d", k);
                g_pm.dots.g[k] = engine::widget::Page(p);
                for (int j = 0; j < 3; ++j) {
                    std::snprintf(p, sizeof p, "anim-group.result.pmCircle.pmDot%d.pmDot%d%c",
                                  k, k, "HUS"[j]);
                    g_pm.dots.c[k][j] = engine::widget::Page(p);
                }
            }
            g_pm.dots.shown = -1;
        }

        void UiPrompt(bool next)
        {
            UiVisible("anim-group.pmNextIcon", !next);
            UiVisible("anim-group.pmNextText", !next);
            UiVisible("anim-group.pmContIcon", next);
            UiVisible("anim-group.pmContText", next);
        }

        void PmResult(const net::clan::State& c)
        {
            char out[kPmOut][128]{};
            int k = 0;
#define PM_LINE(rgb, ...) do { if (k < kPmOut) { char t_[96]; std::snprintf(t_, sizeof t_, __VA_ARGS__); \
        std::snprintf(out[k++], sizeof out[0], "<font color=\"#%s\">%s</font>", rgb, t_); } } while (0)
            if (c.st == 2) PM_LINE("e04848", "YOUR CLAN IS GONE");
            if (c.st == 3) PM_LINE("5dc0dc", "YOUR CLAN SURVIVED 12 WEEKS");
            if (c.lleft)
                PM_LINE("e04848", "YOU LEFT EARLY - NO SUPPLIES TODAY");
            else if (c.lsup >= c.lneed)
                PM_LINE("5dc0dc", "THE CLAN IS FED");
            else
                PM_LINE("e04848", "SHORT BY %d SUPPLIES", c.lneed - c.lsup);
            const int died = c.ldied - (c.levk == 2 ? c.levl : 0);
            if (died > 0) PM_LINE("fd6d51", "%d SURVIVORS DIED", died);
            if (c.lsick) PM_LINE("fd6d51", "%d SURVIVORS GOT SICK", c.lsick);
            if (c.lhungry) PM_LINE("9c9a9a", "%d SURVIVORS GOT HUNGRY", c.lhungry);
            if (c.lheal) PM_LINE("5dc0dc", "%d SURVIVORS GOT HEALTHY", c.lheal);
#undef PM_LINE
            for (int i = 0; i < kPmOut; ++i) {
                char p[64];
                std::snprintf(p, sizeof p, "anim-group.result.pmOut%d", i);
                UiText(p, out[i]);
            }
            char b[160];
            std::snprintf(b, sizeof b, "SUPPLIES GATHERED: <font color=\"#3fa9f5\">%d</font>", c.lsup);
            UiText("anim-group.result.pmGathered", b);
            std::snprintf(b, sizeof b, "NEEDED PER DAY: <font color=\"#3fa9f5\">%d</font>", c.lneed);
            UiText("anim-group.result.pmNeeded", b);
            std::snprintf(b, sizeof b, "%d SURVIVORS", c.h + c.hu + c.s);
            UiText("anim-group.result.pmCount", b);
            std::snprintf(b, sizeof b, "HEALTHY: %d", c.h);
            UiText("anim-group.result.pmHealthy", b);
            std::snprintf(b, sizeof b, "HUNGRY: %d", c.hu);
            UiText("anim-group.result.pmHungry", b);
            std::snprintf(b, sizeof b, "SICK: %d", c.s);
            UiText("anim-group.result.pmSick", b);
            // pmMis0..2 / pmMisLine come from the daemon's 0xE8 mission meta
            // (ext PmMission). No meta port exists in mod/ yet, so these stay at
            // their widget defaults instead of being guessed from 0xE1.
        }

        void PmClose(const char* why)
        {
            net::clan::NotePostClosed(std::strcmp(why, "continue") == 0);
            g_pm.outAt = GetTickCount64();
            g_pm.backAt = engine::frame::Count();
            TO1_LOG("postmatch: page closed (%s)\n", why);
        }

        void PmTick(bool entered, const net::clan::State& c, uint64_t now)
        {
            const uint64_t frame = engine::frame::Count();
            const bool ready = net::clan::PmReady(c, net::clan::PostDay(), net::clan::PostLmh0());
            if (entered) {
                g_pm = PmUi{};
                g_pm.t0 = now + 300;
                g_pm.hSel = g_pm.hBack = true;
                g_pm.refill = 10;
                g_pm.dots.r = 190.0f;
                g_pm.dots.mid = 215.0f;
                PmFill(c);
                TO1_LOG("postmatch: page up (%d parts, day %d, %s)\n",
                        net::clan::PostParts(), net::clan::PostDay(),
                        ready ? "in" : "pending");
            } else if (g_pm.refill && !--g_pm.refill && g_pm.stage == 0) {
                PmFill(c);
            }

            const bool sel = MenuEdge(SID("menu-select"), g_pm.hSel);
            const bool back = MenuEdge(SID("menu-back"), g_pm.hBack);
            if (back && !g_pm.backAt) PmClose("back");

            if (g_pm.outAt) {
                float f = 1.0f - static_cast<float>(now - g_pm.outAt) / 300.0f;
                if (f < 0.0f) f = 0.0f;
                UiAlpha("anim-group", f < g_pm.curA ? f : g_pm.curA);
                UiAlpha("pmBg", f);
                if (now - g_pm.outAt >= 300 && frame - g_pm.backAt > 6) {
                    g_pm.outAt = 0;
                    PopPage();
                }
                return;
            }

            char p[128];
            if (g_pm.stage == 0) {
                int64_t el = static_cast<int64_t>(now - g_pm.t0);
                int cdur = net::clan::PostParts() * 15;
                cdur = cdur < 700 ? 700 : cdur > 1800 ? 1800 : cdur;
                if (!ready && el > kPmCntAt) {
                    const bool late = now - net::clan::PostSentAt() > 45000;
                    g_pm.t0 = now - kPmCntAt;
                    el = kPmCntAt;
                    PmText(net::clan::kPmRows + 1, "anim-group.convert.pmWait",
                           late ? "NO WORD FROM THE CAMP - SEE THE MENU LATER"
                                : "WAITING FOR THE CAMP...");
                    UiVisible("anim-group.convert.pmWait", true);
                    if (late) UiPrompt(true);
                    if (sel && late) {
                        PmClose("no day from the server");
                        PopPage();
                        return;
                    }
                } else if (ready && !g_pm.started) {
                    g_pm.started = 1;
                    UiVisible("anim-group.convert.pmWait", false);
                    UiVisible("anim-group.convert.pmLeft", c.lleft != 0);
                }
                bool selUse = sel;
                if (selUse && ready && !g_pm.fin) {
                    g_pm.skipped = 1;
                    el = kPmCntAt + cdur;
                    g_pm.t0 = now - static_cast<uint64_t>(el);
                    selUse = false;
                }
                const float pa = el <= 0 ? 0.0f
                    : el >= kPmFade ? 1.0f : static_cast<float>(el) / kPmFade;
                const float ta = el <= kPmTotAt ? 0.0f
                    : el >= kPmTotAt + 600 ? 1.0f : static_cast<float>(el - kPmTotAt) / 600.0f;
                const float ca = el <= kPmCntAt - 300 ? 0.0f
                    : el >= kPmCntAt ? 1.0f : static_cast<float>(el - (kPmCntAt - 300)) / 300.0f;
                UiAlpha("anim-group", pa);
                g_pm.curA = pa;
                static const char* const kTot[] = {
                    "pmEarnLine", "pmPartsGear", "pmPartsLabel", "pmPartsNum",
                    "pmPartsIcon", "pmPartsNumB", "pmHeroBack", "pmHeroBar", "pmHeroFrame",
                };
                for (const char* name : kTot) {
                    std::snprintf(p, sizeof p, "anim-group.earned.%s", name);
                    UiAlpha(p, ta);
                }
                UiAlpha("anim-group.convert", ca);
                int n = 0;
                const net::clan::PmRow* rows = net::clan::PmRows(&n);
                const int first = PmFirstRow(n);
                for (int k = 0; k < n; ++k) {
                    std::snprintf(p, sizeof p,
                                  "anim-group.earned.rows.pmRow%d.pmRow%dNum",
                                  first + k, first + k);
                    PmNum(k, p, rows[k].parts);
                }
                float f = !ready || el <= kPmCntAt ? 0.0f
                    : el >= kPmCntAt + cdur ? 1.0f
                    : static_cast<float>(el - kPmCntAt) / static_cast<float>(cdur);
                if (f <= 0.0f || f >= 1.0f || now - g_pm.setAt >= 50) {
                    g_pm.setAt = now;
                    PmNumDb(net::clan::kPmRows, "anim-group.earned.pmPartsNum",
                            net::clan::PostParts() - static_cast<int>(net::clan::PostParts() * f + 0.5f));
                }
                PmNumDb(net::clan::kPmRows + 2, "anim-group.convert.pmConvNum",
                        ready ? static_cast<int>(c.lsup * f + 0.5f) : 0);
                if (ready && f >= 1.0f && !g_pm.fin) {
                    g_pm.fin = 1;
                    g_pm.autoAt = g_pm.skipped ? 0 : now + 1200;
                }
                if (g_pm.fin && g_pm.autoAt && now >= g_pm.autoAt) selUse = true;
                if (!selUse || !g_pm.fin) return;
                g_pm.stage = 2;
                g_pm.t0 = now + 200;
                g_pm.started = 1;
                UiAlpha("anim-group", 1.0f);
                g_pm.curA = 1.0f;
                UiVisible("anim-group.earned", false);
                UiVisible("anim-group.convert", false);
                UiVisible("anim-group.result", true);
                PmResult(c);
                RunDots(g_pm.dots, c, true);
                UiPrompt(true);
                return;
            }

            const int dur = 1000;
            const int diff = c.lsup - c.lneed;
            const int v = PmCount(diff < 0 ? -diff : diff, g_pm.t0, now, dur);
            char b[96];
            if (diff >= 0) std::snprintf(b, sizeof b, "+%d", v);
            else std::snprintf(b, sizeof b, "<font color=\"#e04848\">-%d</font>", v);
            PmText(net::clan::kPmRows + 3, "anim-group.result.pmDailyNum", b);
            PmNumDb(net::clan::kPmRows + 4, "anim-group.result.pmGainNum",
                    PmCount(c.lgain, g_pm.t0, now, dur));
            RunDots(g_pm.dots, c, false);
            if (!sel) return;
            if (!g_pm.started || now < g_pm.t0 + static_cast<uint64_t>(dur)) {
                g_pm.t0 = now - 100000;
                return;
            }
            PmClose("continue");
        }

        // ---- to1/clan-event -----------------------------------------------------
        struct EvUi {
            int refill = 0, skipped = 0;
            bool hSel = true, hBack = true;
            uint64_t t0 = 0, outAt = 0, outFrame = 0;
            bool gone = false;
            Dots dots;
            struct Actor {
                uint64_t w = 0, c[3]{};
                bool wasOn = false;
            } act[kActors];
        } g_ev;

        float EvRamp(int64_t el, int64_t t0, int64_t dur)
        {
            if (el <= t0) return 0.0f;
            if (el >= t0 + dur) return 1.0f;
            return static_cast<float>(el - t0) / static_cast<float>(dur);
        }

        int EvLossPct(int day, int tier)
        {
            static const int E[4] = { 60, 40, 20, 10 }, A[4] = { 100, 60, 40, 20 };
            const int* t = day == 15 || day == 30 ? E : A;
            if (tier < 0) tier = 0;
            if (tier > 3) tier = 3;
            return t[tier];
        }

        // ext MisGrowth: ceil(size * 5 / 7 / 10 %) in float32. The mission-meta
        // goal table is not ported, so the announcement chips fall back to this
        // exact survivor count (and the tier percentages only when size is 0).
        int MisGrowth(int size, int tier)
        {
            static const float P[3] = { 0.05f, 0.07f, 0.10f };
            const float v = static_cast<float>(size) * P[tier < 0 ? 0 : tier > 2 ? 2 : tier];
            const int n = static_cast<int>(v);
            return static_cast<float>(n) < v ? n + 1 : n;
        }

        // ext EvChipBig: without the mission-meta goal (unported) the big text is
        // the tier number under its own "TIER n" label.
        void EvChipBig(char* b, size_t n, int k)
        {
            std::snprintf(b, n, "%d", k + 1);
        }

#define EV_FONT(rgb, s) "<font color=\"#" rgb "\">" s "</font>"

        void EvFill(const net::clan::State& c)
        {
            const int mode = net::clan::EventMode();
            const bool atk = mode == 1 ? c.ev == 2 : c.levk == 2;
            const int day = mode == 1 ? c.day : c.lday;
            const int d = day < 0 ? 0 : day < 84 ? day : 83;
            const int fac = c.fac >= 1 && c.fac <= 2 ? c.fac : 1;
            const char* foe = fac == 1 ? "SERAPHITE" : "WLF";
            const char* foes = fac == 1 ? "Seraphites" : "WLF";
            const char* foesU = fac == 1 ? "SERAPHITES" : "WLF";
            static const char* const kGrowth[5] = {
                "FRIENDLY SURVIVORS", "DYSENTERY OUTBREAK", "MALARIA OUTBREAK",
                "RESCUE ALLIES", "MARAUDER ATTACK",
            };
            static const char* const kResult[5] = {
                "FRIENDLY SURVIVORS", "DYSENTERY OUTBREAK", "MALARIA OUTBREAK",
                "RESCUE ALLIES ATTEMPTED", "MARAUDER ATTACK REPELLED",
            };
            int gi = mode == 1 ? c.evn : c.levn;
            if (gi < 0 || gi > 4) gi = 0;
            int tier = mode == 2 ? c.levt : 0;
            if (tier < 0) tier = 0;
            if (tier > 3) tier = 3;
            char b[512], p[128];
            std::memset(g_pmLast, 0, sizeof g_pmLast); // ext EvFill shares the PM text cache
            std::snprintf(b, sizeof b, "WEEK %d: DAY %d", d / 7 + 1, d % 7 + 1);
            UiText("anim-group.evTitle", b);
            UiText("anim-group.evFaction", kSideFull[fac]);
            UiImage("anim-group.evEmblemW", "streaming/t2r/clan/fig-wlf.png");
            UiImage("anim-group.evEmblemS", "streaming/t2r/clan/fig-ser.png");
            UiVisible("anim-group.evEmblemW", fac == 1);
            UiVisible("anim-group.evEmblemS", fac == 2);
            UiText("anim-group.evKind", mode == 1 ? "INCOMING EVENT" : "EVENT RESULT");
            UiText("anim-group.evRisk", "");
            UiText("anim-group.evLine0", "");
            UiText("anim-group.evLine1", "");
            if (atk) {
                if (mode == 1) {
                    std::snprintf(b, sizeof b, "%s ATTACK", foe);
                    UiText("anim-group.evName", b);
                    std::snprintf(b, sizeof b, "RISK: " EV_FONT("e04848", "%d%%") " OF POPULATION",
                                  EvLossPct(day, 0));
                    UiText("anim-group.evRisk", b);
                    std::snprintf(b, sizeof b,
                                  "Scouts have picked up a large group of %s massing nearby for an attack.  "
                                  "Improve the training of the group to help us survive the attack.", foes);
                    UiText("anim-group.evBlurb", b);
                    UiText("anim-group.evLine0", EV_FONT("5dc0dc", "DURING THIS EVENT YOU GAIN 10% MORE PARTS"));
                    UiText("anim-group.evLine1",
                           "PICK A MISSION TO TRAIN FOR IT - THE FURTHER YOU GET IN 3 MATCHES, THE FEWER SURVIVORS FALL");
                } else {
                    std::snprintf(b, sizeof b, "THE %s ATTACKED", foesU);
                    UiText("anim-group.evName", b);
                    if (tier == 0 && c.st == 2)
                        std::snprintf(b, sizeof b,
                                      "You were not able to improve the training of your clan before the %s attacked.  "
                                      "Your clan was wiped out.", foes);
                    else if (tier == 0)
                        std::snprintf(b, sizeof b,
                                      "You were not able to improve the training of your clan before the %s attacked.  "
                                      "Massive casualties were sustained.", foes);
                    else if (tier < 3)
                        std::snprintf(b, sizeof b,
                                      "You were able to partially improve the training of your clan.  "
                                      "You sustained heavy casualties from the %s.", foes);
                    else
                        std::snprintf(b, sizeof b,
                                      "You were able to dramatically improve the training of your clan before the %s attacked.  "
                                      "As a result, a minimum of clan members died in the attack.", foes);
                    UiText("anim-group.evBlurb", b);
                    std::snprintf(b, sizeof b, EV_FONT("fd6d51", "%d CLAN MEMBERS DIED IN THE %s ATTACK"),
                                  c.levl, foe);
                    UiText("anim-group.evLine0", b);
                    if (c.st == 2)
                        std::snprintf(b, sizeof b, "%s", "YOUR CLAN WAS WIPED OUT");
                    else
                        std::snprintf(b, sizeof b, "%d SURVIVORS LEFT", c.h + c.hu + c.s);
                    UiText("anim-group.evLine1", b);
                }
                for (int k = 0; k < 3; ++k) {
                    std::snprintf(p, sizeof p, "anim-group.evChip%d.evChip%dBig", k, k);
                    EvChipBig(b, sizeof b, k);
                    UiText(p, b);
                    std::snprintf(p, sizeof p, "anim-group.evChip%d.evChip%dPct", k, k);
                    std::snprintf(b, sizeof b, EV_FONT("fd6d51", "LOSE %d%%"), EvLossPct(day, k + 1));
                    UiText(p, b);
                }
            } else {
                static const char* const kPct[3] = { "+5%", "+7%", "+10%" };
                UiText("anim-group.evName", mode == 1 ? kGrowth[gi] : kResult[gi]);
                if (mode == 1) {
                    UiText("anim-group.evRisk", "EVENT BUFF: " EV_FONT("5dc0dc", "NEW SURVIVORS"));
                    if (gi == 0)
                        std::snprintf(b, sizeof b,
                                      "Some friendly survivors have been located nearby.  If we can protect them from the %s "
                                      "they'll join our clan.", foes);
                    else if (gi == 1)
                        std::snprintf(b, sizeof b,
                                      "Dysentery is spreading rapidly in the area.  The %s have a supply of antibiotics.  "
                                      "Improve the training of the group so we can mount an attack and steal it.", foes);
                    else if (gi == 2)
                        std::snprintf(b, sizeof b,
                                      "An outbreak of malaria is spreading rapidly throughout the area.  The %s have a supply "
                                      "of antibiotics.  Improve the training of the group so we can mount an attack and steal it.", foes);
                    else if (gi == 3)
                        std::snprintf(b, sizeof b,
                                      "A group of allies trying to reach our camp were captured by the %s.  Improve the "
                                      "training of the group so we can mount a rescue operation.", foes);
                    else
                        std::snprintf(b, sizeof b,
                                      "Marauders have been raiding camps in the area and are heading our way.  Improve the "
                                      "training of the group to repel the attack.");
                    UiText("anim-group.evBlurb", b);
                    UiText("anim-group.evLine1",
                           "PICK A MISSION TO TRAIN FOR IT - THE FURTHER YOU GET IN 3 MATCHES, THE MORE SURVIVORS JOIN");
                } else {
                    if (gi == 0)
                        std::snprintf(b, sizeof b, "%s",
                                      tier == 0 ? "You were not able to protect any friendly survivors.  No one joined your clan."
                                      : tier < 3 ? "You were able to partially improve the training of your clan and protect some "
                                                   "friendlies.  Some new members joined your clan."
                                                 : "You were able to dramatically improve the training of your clan and protect the "
                                                   "friendly survivors.  All the friendlies joined your clan.");
                    else if (gi == 1 || gi == 2)
                        std::snprintf(b, sizeof b, "%s",
                                      tier == 0 ? "You were not able to improve the training of your clan before the raid.  "
                                                  "The antibiotics were not recovered."
                                      : tier < 3 ? "You were able to partially improve the training of your clan.  Some of the "
                                                   "antibiotics were recovered and new members joined your clan."
                                                 : "You were able to dramatically improve the training of your clan.  The antibiotics "
                                                   "were recovered and new members joined your clan.");
                    else if (gi == 4)
                        std::snprintf(b, sizeof b, "%s",
                                      tier == 0 ? "You were not able to improve the training of your clan before the marauders "
                                                  "arrived.  No one joined your clan."
                                      : tier < 3 ? "You were able to partially improve the training of your clan and repel the "
                                                   "marauders.  Some new members joined your clan."
                                                 : "You were able to dramatically improve the training of your clan and repel the "
                                                   "marauders.  Survivors who saw it joined your clan.");
                    else
                        std::snprintf(b, sizeof b, "%s",
                                      tier == 0 ? "You were not able to improve the training of your clan before the rescue operation."
                                      : tier < 3 ? "You were able to partially improve the training of your clan.  Some allies were rescued."
                                                 : "You were able to dramatically improve the training of your clan before the rescue "
                                                   "operation.  All the allies joined your clan.");
                    UiText("anim-group.evBlurb", b);
                    if (c.levg)
                        std::snprintf(b, sizeof b, EV_FONT("5dc0dc", "%d NEW MEMBERS JOINED THE CLAN"),
                                      c.levg);
                    else
                        std::snprintf(b, sizeof b, "%s", "NO ONE JOINED THE CLAN");
                    UiText("anim-group.evLine0", b);
                    std::snprintf(b, sizeof b, "%d SURVIVORS", c.h + c.hu + c.s);
                    UiText("anim-group.evLine1", b);
                }
                // ext EvFill: the running mission's goals (0xE8 meta, unported)
                // override the growth count; without meta the announcement shows
                // the exact survivors each tier brings, the result the tier %.
                const int csz = c.h + c.hu + c.s;
                for (int k = 0; k < 3; ++k) {
                    const int goal = 0;  // no mission-meta producer in mod/ yet
                    const int grow = mode == 1 && csz > 0 ? MisGrowth(csz, k) : 0;
                    std::snprintf(p, sizeof p, "anim-group.evChip%d.evChip%dBig", k, k);
                    if (goal) EvChipBig(b, sizeof b, k);
                    else if (grow > 0) std::snprintf(b, sizeof b, "+%d", grow);
                    else std::snprintf(b, sizeof b, "%s", kPct[k]);
                    UiText(p, b);
                    std::snprintf(p, sizeof p, "anim-group.evChip%d.evChip%dPct", k, k);
                    if (!goal)
                        std::snprintf(b, sizeof b, EV_FONT("5dc0dc", "%s"),
                                      grow == 1 ? "SURVIVOR JOINS" : "SURVIVORS JOIN");
                    else if (grow > 0)
                        std::snprintf(b, sizeof b, EV_FONT("5dc0dc", "+%d JOIN"), grow);
                    else
                        std::snprintf(b, sizeof b, EV_FONT("5dc0dc", "%s JOIN"), kPct[k]);
                    UiText(p, b);
                }
            }
            for (int k = 0; k < 3; ++k) {
                std::snprintf(p, sizeof p, "anim-group.evChip%d.evChip%dFrame", k, k);
                UiVisible(p, mode == 2 && tier == k + 1);
            }
            for (int k = 0; k < kDots; ++k) {
                std::snprintf(p, sizeof p, "anim-group.evScene.evCircle.evDot%d", k);
                g_ev.dots.g[k] = engine::widget::Page(p);
                for (int j = 0; j < 3; ++j) {
                    std::snprintf(p, sizeof p, "anim-group.evScene.evCircle.evDot%d.evDot%d%c", k, k, "HUS"[j]);
                    g_ev.dots.c[k][j] = engine::widget::Page(p);
                }
                if (k < kActors) {
                    std::snprintf(p, sizeof p, "anim-group.evScene.evAct%d", k);
                    g_ev.act[k].w = engine::widget::Page(p);
                    for (int j = 0; j < 3; ++j) {
                        std::snprintf(p, sizeof p, "anim-group.evScene.evAct%d.evAct%d%c", k, k, "RWG"[j]);
                        g_ev.act[k].c[j] = engine::widget::Page(p);
                        engine::widget::Visible(g_ev.act[k].c[j], j == (atk ? 0 : 1));
                    }
                }
            }
            g_ev.dots.shown = -1;
        }

        void EvScene(const net::clan::State& c, int64_t el)
        {
            const int mode = net::clan::EventMode();
            const bool atk = mode == 1 ? c.ev == 2 : c.levk == 2;
            const int day = mode == 1 ? c.day : c.lday;
            const float ts = static_cast<float>(el) / 1000.0f;
            const int nAfter = c.h + c.hu + c.s;
            const int lost = mode == 2 && atk ? c.levl : 0;
            const int join = mode == 2 && !atk ? c.levg : 0;
            float p = mode == 2 ? EvRamp(el, kEvTGo, kEvTHit - kEvTGo) : 0.0f;
            p = p * p * (3.0f - 2.0f * p);
            int na = atk ? 6 + EvLossPct(day, 0) / 8
                 : mode == 1 ? 6 : (join ? (join < 10 ? join : 10) : 4);
            if (na > kActors) na = kActors;
            const float fade = mode == 2 ? 1.0f - EvRamp(el, kEvTHit + 100, 500) : 1.0f;
            for (int k = 0; k < kActors; ++k) {
                const bool on = k < na && fade > 0.0f;
                if (!g_ev.act[k].w) continue;
                if (on != g_ev.act[k].wasOn || el < 100) {
                    engine::widget::Visible(g_ev.act[k].w, on);
                    g_ev.act[k].wasOn = on;
                }
                if (!on) continue;
                float ang = 6.2832f * static_cast<float>(k) / static_cast<float>(na) + 0.55f
                    + (mode == 1 ? 0.10f * std::sin(ts * 0.35f + k * 1.7f) : 0.0f);
                float ring0 = 270.0f + 45.0f * (k % 3)
                    - (mode == 1 ? 14.0f * std::sin(ts * 0.9f + k * 1.3f) : 0.0f);
                float rr = ring0;
                if (mode == 2)
                    rr = (atk || join) ? ring0 + (kEvEdge - ring0) * p : ring0 + 90.0f * p;
                engine::widget::Pivot(g_ev.act[k].w,
                                      850.0f + std::cos(ang) * rr * kEvRx,
                                      330.0f + std::sin(ang) * rr * kEvRy);
                engine::widget::Alpha(g_ev.act[k].w,
                    mode == 2 ? fade * (!atk && !join ? 1.0f - 0.7f * p : 1.0f)
                              : EvRamp(el, 900, 600) * (0.75f + 0.25f * std::sin(ts * 1.4f + k)));
            }
            net::clan::State f = c;
            if (mode == 2 && el < kEvTEnd) {
                if (atk) {
                    f.h = nAfter + lost;
                    f.hu = f.s = 0;
                    if (el >= kEvTHit) { f.h = nAfter; f.s = lost; }
                } else if (join && el < kEvTHit) {
                    f.h = c.h - join;
                    if (f.h < 0) f.h = 0;
                }
            }
            RunDots(g_ev.dots, f, false);
            float ge = 0.0f, gf = 0.0f;
            if (atk)
                ge = mode == 1 ? 0.26f + 0.09f * std::sin(ts * 1.6f)
                    : 0.14f + 0.40f * EvRamp(el, kEvTGo, kEvTHit - kEvTGo)
                        * (1.0f - EvRamp(el, kEvTHit, 700));
            else
                gf = mode == 1 ? 0.20f + 0.06f * std::sin(ts * 1.2f)
                    : 0.12f + (join ? 0.30f * EvRamp(el, kEvTHit, 300)
                                     * (1.0f - EvRamp(el, kEvTHit + 300, 700)) : 0.0f);
            UiAlpha("anim-group.evScene.evGlowEnemy", ge);
            UiAlpha("anim-group.evScene.evGlowFriend", gf);
        }

        void EvClose(const char* why)
        {
            net::clan::NoteEventClosed();
            g_ev.outAt = GetTickCount64();
            g_ev.outFrame = engine::frame::Count();
            TO1_LOG("postmatch: event page closed (%s, mode %d)\n", why, net::clan::EventMode());
        }

        void EvTick(bool entered, const net::clan::State& c, uint64_t now)
        {
            const uint64_t frame = engine::frame::Count();
            if (entered) {
                g_ev = EvUi{};
                g_ev.t0 = now + 200;
                g_ev.hSel = g_ev.hBack = true;
                g_ev.refill = 10;
                g_ev.dots.r = 170.0f;
                g_ev.dots.mid = 210.0f;
                EvFill(c);
                UiAlpha("anim-group", 0.0f);
                UiAlpha("evBg", 1.0f);
                TO1_LOG("postmatch: event page up (mode %d, day %d, kind %d)\n",
                        net::clan::EventMode(),
                        net::clan::EventMode() == 1 ? c.day : c.lday,
                        net::clan::EventMode() == 1 ? c.ev : c.levk);
            } else if (g_ev.refill && !--g_ev.refill) {
                EvFill(c);
            }

            if (g_ev.gone) {
                UiAlpha("anim-group", 0.0f);
                UiAlpha("evBg", 0.0f);
                return;
            }
            const bool sel = MenuEdge(SID("menu-select"), g_ev.hSel);
            const bool back = MenuEdge(SID("menu-back"), g_ev.hBack);
            if ((sel || back) && !g_ev.outAt) {
                const int64_t el = static_cast<int64_t>(now - g_ev.t0);
                if (sel && !back && net::clan::EventMode() == 2 && el > 0
                    && el < kEvTEnd + 300 && !g_ev.skipped) {
                    g_ev.skipped = 1;
                    g_ev.t0 = now - static_cast<uint64_t>(kEvTEnd + 300);
                } else {
                    EvClose(back ? "back" : "continue");
                }
            }
            if (g_ev.outAt) {
                float f = 1.0f - static_cast<float>(now - g_ev.outAt) / 300.0f;
                if (f < 0.0f) f = 0.0f;
                UiAlpha("anim-group", f);
                UiAlpha("evBg", f);
                if (now - g_ev.outAt >= 300 && frame - g_ev.outFrame > 6) {
                    g_ev.outAt = 0;
                    g_ev.gone = true;
                    PopPage();
                }
                return;
            }
            const int64_t el = static_cast<int64_t>(now - g_ev.t0);
            UiAlpha("anim-group", EvRamp(el, 0, 700));
            static const struct { const char* path; int at; } kStages[] = {
                { "anim-group.evKind", 100 }, { "anim-group.evName", 250 },
                { "anim-group.evRisk", 600 }, { "anim-group.evScene", 450 },
                { "anim-group.evFrame", 450 }, { "anim-group.evBlurb", 1000 },
                { "anim-group.evChip0", 1400 }, { "anim-group.evChip1", 1550 },
                { "anim-group.evChip2", 1700 }, { "anim-group.evChipLine0", 1450 },
                { "anim-group.evChipLine1", 1600 }, { "anim-group.evLine0", 0 },
                { "anim-group.evLine1", 1800 }, { "anim-group.evContIcon", 2000 },
                { "anim-group.evContText", 2000 },
            };
            for (size_t i = 0; i < sizeof kStages / sizeof *kStages; ++i) {
                const int64_t at = kStages[i].at ? kStages[i].at : kEvTHit;
                float a = EvRamp(el, at, 500);
                if (net::clan::EventMode() == 2 && i >= 6 && i <= 8)
                    a *= c.levt == static_cast<int>(i) - 5 ? 1.0f : 0.4f;
                UiAlpha(kStages[i].path, a);
            }
            EvScene(c, el);
        }

        void Init()
        {
            if (g.cl13x)
                return;
            g.cl13x       = engine::widget::PageId("clan13x");
            g.postmatch   = engine::widget::PageId("clan-postmatch");
            g.event       = engine::widget::PageId("clan-event");
            g.gearup13    = engine::widget::PageId("gearup13");
            g.loadoutMenu = engine::widget::PageId("loadout-menu");
            g.pickers[0]  = engine::widget::PageId("picker-skill");
            g.pickers[1]  = engine::widget::PageId("picker-primary");
            g.pickers[2]  = engine::widget::PageId("picker-secondary");
            g.pickers[3]  = engine::widget::PageId("picker-purchase");
            g.findMatch   = engine::widget::PageId("find-match-menu");
            g.mainMenu    = engine::widget::PageId("main-menu");
            g_pm.dots.r = 190.0f;
            g_pm.dots.mid = 215.0f;
            g_ev.dots.r = 170.0f;
            g_ev.dots.mid = 210.0f;
        }

        bool OnStack(StringId64 page)
        {
            return page && MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, page);
        }

        bool GearPagesUp()
        {
            return OnStack(g.gearup13) || OnStack(g.loadoutMenu)
                || OnStack(g.pickers[0]) || OnStack(g.pickers[1])
                || OnStack(g.pickers[2]) || OnStack(g.pickers[3]);
        }

        void PushPage(const char* page)
        {
            if (!OnStack(engine::widget::PageId(page + 4))) // PageId takes "clan-postmatch", not "to1/..."
                MENU2::PUSH_PAGE(MenuStack::hudMenuStack, page);
        }

    } // namespace

    void Tick()
    {
        Init();

        const uint64_t now = GetTickCount64();
        const uint64_t top = MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack);
        // ext FeatureClan only argues about pages when the MP menu is the *top*
        // page: the PAGE_FM branch returns at :82023 and the general gate at
        // :82030 is `top != PAGE_MAIN`. Matching that here keeps FIND MATCH and
        // "main menu merely on the stack" from becoming push windows.
        const bool menuTop = top == g.mainMenu;
        const bool gear = GearPagesUp();

        // The page handlers' bools are now backed by the native clan state machine;
        // the stack sample only covers the widget pages the mod still observes.
        g.cl13    = OnStack(g.cl13x);
        g.post    = OnStack(g.postmatch);
        g.eventUp = OnStack(g.event);

        // A new report (wire Report() or the non-wire ArmPost()) re-stamps
        // sentAt; start its 3-push budget over just like ext PmReport's memset.
        if (net::clan::PostSentAt() != g.pmSeenSentAt) {
            g.pmSeenSentAt = net::clan::PostSentAt();
            g.pmPushAt = 0;
            g.pmPushes = 0;
        }

        // ext EvOffer: on the MP menu, with no GEAR UP navigation and no post-match
        // page due, decide whether the event screen is wanted. A true return is a
        // push request; the stack guard below adopts an ext push that landed first.
        const bool evDue = net::clan::Offer(menuTop, gear, g.eventUp, now);

        // ext FeatureClan's post-match push, now issued by the mod. The ext ticks
        // first, so if its own latch pushed the page already, OnStack() above is
        // true and PushPage() is a no-op. The 3-push / 1 s cadence and the
        // top==main-menu / no-GEAR-UP gate match ext :82030-82068 exactly.
        // Honest gate: the ext's `g_pm.sb` / Cl13PostOpen() scoreboard-first
        // ordering (to1/clan13x) stays ext-owned; the mod only refuses to stack
        // on a scoreboard that is already up (`!g.cl13`) and does not sequence
        // one itself.
        if (!g.post && net::clan::PostPending() && menuTop && !gear && !g.cl13
            && !g.eventUp && g.pmPushes < 3 && now - g.pmPushAt > 1000) {
            PushPage("to1/clan-postmatch");
            g.pmPushAt = now;
            ++g.pmPushes;
            g.post = true;
            TO1_LOG("postmatch: to1/clan-postmatch pushed (day %d, %d parts)\n",
                    net::clan::PostDay(), net::clan::PostParts());
        }
        if (evDue && !g.eventUp) {
            PushPage("to1/clan-event");
            g.eventUp = true;
            TO1_LOG("postmatch: to1/clan-event pushed (mode %d)\n", net::clan::EventMode());
        }

        // Page edges: entering marks the native latch shown; leaving closes it.
        // With the ext still loaded its handlers may have closed it already -- the
        // close hooks are idempotent, so this only covers the window we cannot see.
        const bool pmTop = top == g.postmatch;
        const bool evTop = top == g.event;
        const bool pmEntered = pmTop && !g.wasPostTop;
        const bool evEntered = evTop && !g.wasEventTop;
        if (pmEntered)
            net::clan::NotePostShown();
        else if (!pmTop && g.wasPostTop && net::clan::PostPending())
            net::clan::NotePostClosed(true);
        g.wasPostTop = pmTop;
        if (evEntered)
            net::clan::NoteEventShown();
        else if (!evTop && g.wasEventTop && net::clan::EventPending())
            net::clan::NoteEventClosed();
        g.wasEventTop = evTop;

        const net::clan::State& c = net::clan::Get();
        if (pmTop) PmTick(pmEntered, c, now);
        if (evTop) EvTick(evEntered, c, now);

        if (g.pmPushes >= 3 && net::clan::PostPending() && !pmTop
            && now - g.pmPushAt > 1000) {
            TO1_LOG("postmatch: to1/clan-postmatch never came up - dropped\n");
            net::clan::NotePostClosed(true);
            g.pmPushes = 0;
        }

        // ext g_loState: GuEnter sets LO_LIST on the menu (never in substate 6);
        // the mid-match CHANGE LOADOUT sets LO_MATCH, the countdown sets LO_CHOOSE,
        // the editor sets LO_EDIT. The explicit transitions come from net-gui2;
        // this fallback only handles a page the engine pushed by itself.
        if (gear && g.lo == LoState::None) {
            g.lo = net::match::liveness::Live() ? LoState::Match
                                                 : LoState::List;
        } else if (!gear && (g.lo == LoState::List || g.lo == LoState::Edit)) {
            g.lo = LoState::None;
        }

        if (g.mmBack && ++g.mmBack > kMmBackFrames) {
            const StringId64 mmTop = MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack);
            g.mmBack = 0;
            if (kPopOnMmBack && (mmTop == g.findMatch || mmTop == kLegacyFmPage))
                DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
            TO1_LOG("postmatch: refused find-match latch cleared, top %llx%s\n",
                    static_cast<unsigned long long>(mmTop), kPopOnMmBack ? "" : " [page pop gated]");
        }
    }

    void Reset()
    {
        g.cl13 = g.post = g.eventUp = false;
        g.wasPostTop = g.wasEventTop = false;
        g.lo = LoState::None;
        g.mmBack = 0;
        g.pmPushAt = 0;
        g.pmSeenSentAt = 0;
        g.pmPushes = 0;
        g_pm = PmUi{};
        g_ev = EvUi{};
        std::memset(g_pmLast, 0, sizeof g_pmLast);
    }

    bool Cl13PostActive()  { return g.cl13; }
    bool ClanPostActive()  { return net::clan::PostPending() || g.post; }
    bool ClanEventActive() { return net::clan::EventPending() || g.eventUp; }
    bool GearUpNavActive() { return g.lo != LoState::None; }
    LoState Lo()           { return g.lo; }
    bool MmBackActive()    { return g.mmBack != 0; }

    bool ScreensDone()
    {
        return !g.cl13 && !net::clan::PostPending() && !g.post
            && !net::clan::EventPending() && !g.eventUp
            && g.lo == LoState::None && g.mmBack == 0;
    }

    void NoteLoadoutOpen(bool inMatch)
    {
        if (g.lo == LoState::None)
            g.lo = inMatch ? LoState::Match : LoState::List;
    }
    void NoteLoadoutEdit()
    {
        if (g.lo == LoState::List)
            g.lo = LoState::Edit;
    }
    void NoteLoadoutApplied()
    {
        if (g.lo == LoState::Match)
            g.lo = LoState::None;
    }
    void NoteLoadoutClose()
    {
        if (g.lo != LoState::Match)      // a mid-match pick stays pending to next spawn
            g.lo = LoState::None;
    }
    void NoteCountdownChoose(int phase, int secondsLeft)
    {
        if (phase == 2 && secondsLeft >= 100 && secondsLeft < 130) {
            if (g.lo == LoState::None || g.lo == LoState::List)
                g.lo = LoState::Choose;
        } else if (g.lo == LoState::Choose) {
            g.lo = LoState::None;
        }
    }

    void NoteMmRefused()
    {
        if (!g.mmBack)
            TO1_LOG("postmatch: find match refused (party gate) - hold requeue's settle for a few frames\n");
        g.mmBack = 1;
    }

    void NoteSteamRefused()
    {
        if (!g.mmBack)
            TO1_LOG("postmatch: find match refused (Steam helper not connected) - g_mmBack set\n");
        g.mmBack = 1;
    }

} // namespace net::ui::postmatch
