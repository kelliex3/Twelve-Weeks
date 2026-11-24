// ported from tools/ext/loadseq.c (build-201 working tree). Fixed-VA helpers in
// the ext are replaced with the mod's DC / MENU2 / widget seams; the private PCM
// stinger goes through the data-guarded sink in loadseq-sting.hpp.

#include "loadseq.hpp"
#include "loadseq-core.hpp"
#include "loadseq-sting.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "engine/lobby.hpp"
#include "engine/pcm.hpp"
#include "engine/script.hpp"
#include "engine/widget.hpp"
#include "net/ui/launch-notice.hpp"
#include "log.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace menu::loadseq {

    namespace {

        struct Widgets {
            uint64_t group = 0, bg = 0, title = 0, prog = 0;
            uint64_t full = 0, bright = 0, feather = 0, barimg = 0;
        } w;

        struct State {
            MenuStack stack = MenuStack::hudMenuStack;
            uint64_t  page = 0;
            uint64_t  t0 = 0, outAt = 0, rsT0 = 0, lastPoll = 0, prevMs = 0;
            uint64_t  fillT0 = 0, lastReadyCheck = 0;
            float     fill = 0.0f, writtenPct = -1.0f;
            float     aBg = -1.0f, aTitle = -1.0f, aBar = -1.0f;
            int       stage = 0;      // 0 black, 1 black done, 2 title up
            bool      active = false, resume = false, hold = false, mute = false;
            bool      snd = false, settled = false, disabled = false;
            bool      fillMode = false;
            uint64_t  lastTop = 0;
        } g;

        // The ext's g_lds_resume: survives the frame where the engine has already
        // popped the connecting page and we re-push it on the title stack.
        bool     gResumePending = false;
        uint64_t gResumeT0 = 0;

        constexpr uint64_t kActivePollMs = 50;
        constexpr uint64_t kIdlePollMs   = 150;
        constexpr uint64_t kResumeGiveUpMs = 4000;
        constexpr uint64_t kResumeCapMs    = 120000;

        uint64_t MainConnectingPage()
        {
            return SID("canvas.menuRoot.main.to1/connecting");
        }

        void Resolve()
        {
            w.group   = engine::widget::PageOnStack("anim-group", g.stack);
            w.bg      = engine::widget::PageOnStack("anim-group.bg", g.stack);
            w.title   = engine::widget::PageOnStack("anim-group.t2rLoad", g.stack);
            w.prog    = engine::widget::PageOnStack("anim-group.progress", g.stack);
            w.full    = engine::widget::PageOnStack("anim-group.t2rLoad.t2rLoadTitleFull", g.stack);
            w.bright  = engine::widget::PageOnStack("anim-group.progress.barFull.t2rBright", g.stack);
            w.feather = engine::widget::PageOnStack("anim-group.progress.barFull.t2rFeather", g.stack);
            w.barimg  = engine::widget::PageOnStack("anim-group.progress.barFull.t2rBarImg", g.stack);
        }

        bool WidgetsReady()
        {
            return w.group && w.bg && w.title && w.prog && w.full
                && w.bright && w.feather && w.barimg;
        }

        void Put(uint64_t widget, float& last, float a)
        {
            if (!widget || (std::fabs(last - a) < 0.004f && a > 0.0f && a < 1.0f))
                return;
            last = a;
            engine::widget::Alpha(widget, a);
        }

        void Pct(float v)
        {
            if (!w.prog || std::fabs(v - g.writtenPct) < core::kFillEps) return;
            g.writtenPct = v;
            DC::Call<"gui2-set-progress-bar-percent">(w.prog, v, 0.0f, 12);
        }

        // Which page / stack currently carries the entry sequence: the hud menu
        // stack during a normal load, the title (main) stack during a hand-off.
        bool FindPage(uint64_t now)
        {
            if (kIdlePollMs && g.lastPoll && now - g.lastPoll < (g.active ? kActivePollMs : kIdlePollMs))
                return g.active;
            g.lastPoll = now;

            const uint64_t hudTop = engine::widget::TopPage();
            if (hudTop == engine::widget::PageId("connecting")) {
                g.stack = MenuStack::hudMenuStack;
                g.page = hudTop;
                return true;
            }
            const uint64_t mainTop = engine::widget::TopPage(MenuStack::main);
            if (mainTop && engine::widget::PageOnStack("anim-group.t2rLoad", MenuStack::main)) {
                g.stack = MenuStack::main;
                g.page = mainTop;
                return true;
            }
            return false;
        }

        void Begin(uint64_t now, bool resumed)
        {
            Resolve();
            g.active = true;
            g.settled = false;
            g.resume = resumed;
            g.hold = false;
            g.mute = false;
            g.snd = false;
            g.outAt = 0;
            g.rsT0 = 0;
            g.fillT0 = 0;
            g.aBg = g.aTitle = g.aBar = -1.0f;
            g.writtenPct = -1.0f;
            g.lastReadyCheck = 0;
            g.stage = 0;
            if (resumed) {
                // Backdate: the title is already fully faded in, do not replay sound.
                g.t0 = now >= core::kTitleAtMs + core::kTitleMs + 100
                    ? now - (core::kTitleAtMs + core::kTitleMs + 100) : 1;
                g.stage = 1;
                g.snd = true;
                g.mute = true;
                g.rsT0 = now;
                g.hold = true;
                g.fillT0 = now;
                engine::widget::Visible(w.group, true);
                engine::widget::Alpha(w.group, 1.0f);
                TO1_LOG("load seq: resumed on the title stack\n");
            }
            else {
                g.t0 = now;
                g.fill = 0.0f;
                TO1_LOG("load seq: page up (%s)\n", WidgetsReady() ? "widgets found" : "sequence widgets are missing - nothing animated");
            }
        }

        void End(const char* why)
        {
            (void)why;   // TO1_LOG compiles out in release
            if (g.mute) engine::pcm::SetSessionVolume(1.0f);
            TO1_LOG("load seq: %s (%.0f ms, %.0f%%)\n", why,
                g.t0 ? static_cast<double>(GetTickCount64() - g.t0) : 0.0,
                static_cast<double>(g.fill) * 100.0);
            g = State{};
            w = Widgets{};
        }

        bool WorldReady()
        {
            const uint64_t r = DC::Call<"task-want-load-levels-fully-spawned?">();
            return r != ~uint64_t{0} && r != 0;
        }

        void Stinger(uint64_t now)
        {
            if (g.snd) return;
            g.snd = true;
            if (g.disabled || g.resume) return;
            if (engine::flag::Present("t2r-no-load-stinger.txt")) {
                TO1_LOG("load seq: no stinger (t2r-no-load-stinger.txt)\n");
                return;
            }
            const float vol = engine::pcm::MusicUserVolume();
            if (!sting::Play(vol))
                TO1_LOG("load seq: stinger NOT played (no data / mixer off / music volume 0)\n");
            else
                TO1_LOG("load seq: stinger played\n");
            (void)now;
        }

        void SetupBarMode()
        {
            if (g.fillMode) return;
            engine::widget::Visible(w.full, true);
            engine::widget::Visible(w.bright, false);
            engine::widget::Visible(w.feather, false);
            engine::widget::Visible(w.barimg, true);
            engine::widget::Pivot(w.prog, 0.0f, 2160.0f - 24.0f);
            engine::widget::Scale(w.prog, 3840.0f / 2800.0f, core::kBarScaleY);
        }

        void Drive(uint64_t now)
        {
            if (!g.t0) return;
            const uint64_t elapsed = now >= g.t0 ? now - g.t0 : 0;
            g.disabled = engine::flag::Present("t2r-no-load-seq.txt");
            g.fillMode = engine::flag::Present("t2r-load-fill.txt");

            if (g.disabled) {
                // The old behaviour: black, title and fill up at once, no sound.
                Put(w.bg, g.aBg, 1.0f);
                Put(w.title, g.aTitle, 1.0f);
                Put(w.prog, g.aBar, 1.0f);
                Pct(1.0f);
                g.snd = true;
                return;
            }
            if (!WidgetsReady()) return;

            const float black = core::Fade(now, g.t0, 0, core::kBlackMs);
            Put(w.bg, g.aBg, black);
            if (black >= 1.0f && g.stage == 0) {
                g.stage = 1;
                TO1_LOG("load seq: black\n");
            }

            const float ta = core::Fade(now, g.t0, core::kTitleAtMs, core::kTitleMs);
            const float ba = g.fillMode ? ta : core::Fade(now, g.t0, core::kBarAtMs, core::kBarMs);
            if (elapsed >= core::kTitleAtMs) Stinger(now);
            if (ta > 0.0f && g.stage < 2) {
                g.stage = 2;
                SetupBarMode();
                engine::widget::Visible(w.title, true);
                engine::widget::Visible(w.prog, true);
                TO1_LOG("load seq: title fading in\n");
            }

            // Completion: a real, non-negative answer from the engine only.
            if (core::ReadyForFade(elapsed, WorldReady(), g.lastReadyCheck, now) && !g.outAt) {
                g.outAt = now + core::kHoldMs;
                TO1_LOG("load seq: world ready - %llu ms hold\n",
                    static_cast<unsigned long long>(core::kHoldMs));
            }

            // Resume has a different release condition: native black has cleared
            // and the menu world is past substate 1 (or a cap fired).
            bool resumeOut = false;
            if (g.resume) {
                const float bfv = HUD::GET_BLACK_FADE();
                const uint8_t sub = engine::lobby::Substate();
                const bool released = (bfv < 0.02f && sub != 1) || sub == 2;
                if ((released && bfv < 0.02f)
                    || (g.rsT0 && now - g.rsT0 > kResumeCapMs)) {
                    if (!g.outAt) {
                        g.outAt = now;
                        resumeOut = true;
                    }
                }
                if (g.rsT0 && now - g.rsT0 > kResumeCapMs && !released)
                    TO1_LOG("load seq: resume release cap reached\n");
            }

            bool holdFade = false;
            if (g.outAt && now >= g.outAt) {
                holdFade = net::ui::launch_notice::HoldFade(now);
                if (!holdFade && !g.settled) {
                    g.settled = true;
                    TO1_LOG("load seq: fading out\n");
                }
            }

            if (!g.fillT0 && (ba > 0.02f || (g.fillMode && ta > 0.0f)))
                g.fillT0 = now;
            const uint64_t fillElapsed = g.fillT0 ? (now >= g.fillT0 ? now - g.fillT0 : 0) : 0;
            const bool fadingOut = g.outAt && now >= g.outAt && !holdFade;
            g.fill = core::Advance(g.fill, fillElapsed,
                g.prevMs ? now - g.prevMs : 16, fadingOut);

            const float out = g.outAt && now >= g.outAt
                ? (g.resume ? core::ResumeMultiplier(now, g.outAt)
                            : core::OutMultiplier(now, g.outAt))
                : 1.0f;
            Put(w.title, g.aTitle, ta * out);
            Put(w.prog, g.aBar, g.fillMode ? ba * out : ba);
            if (g.fillMode) Pct(g.fill);
            else {
                Pct(1.0f);
                engine::widget::Scale(w.prog, core::BottomBarScaleX(g.fill), core::kBarScaleY);
            }

            // Music / whole-session fade around the menu-world hand-off.
            if (g.mute && !g.outAt)
                engine::pcm::SetSessionVolume(core::SessionVolume(now, g.rsT0, false));
            else if (g.settled)
                engine::pcm::SetSessionVolume(core::SessionVolume(now, g.outAt, true));

            if (g.settled && !holdFade && g.outAt
                && now - g.outAt >= (g.resume ? core::kResumeOutMs : core::kOutMs) + 50) {
                if (g.resume && g.stack == MenuStack::main) {
                    if (engine::widget::TopPage(MenuStack::main) == MainConnectingPage())
                        DC::Call<"menu2-pop-page">(MenuStack::main, 0);
                    g.hold = false;
                }
                else {
                    // The engine normally pops this page; don't pull it out from
                    // under a level load, just clear our ownership.
                    g.hold = false;
                }
                End("sequence finished");
                return;
            }
            if (g.resume) {
                // Keep the root forced on during the menu-world load; its own open
                // animation does not run there.
                engine::widget::Visible(w.group, true);
                engine::widget::Alpha(w.group, 1.0f);
            }
            (void)resumeOut;
        }

    } // namespace

    void Tick(uint64_t now)
    {
        g.disabled = engine::flag::Present("t2r-no-load-seq.txt");
        if (boot::ExtLoaded()) return;   // legacy ext owns its own connecting page

        const bool found = FindPage(now);

        if (!found) {
            if (g.active && g.t0 && !g.resume && !g.outAt && !g.disabled
                && engine::lobby::Substate() == 1) {
                // The engine popped the page during a menu-world load: push it
                // back on the title stack for the hand-off fade.
                DC::Call<"menu2-page-stack-enable-sounds">(MenuStack::main, 0);
                MENU2::PUSH_PAGE(MenuStack::main, "to1/connecting");
                gResumePending = true;
                gResumeT0 = now;
                g.hold = true;
                TO1_LOG("load seq: re-pushed to1/connecting on the title stack\n");
                return;
            }
            if (gResumePending && now - gResumeT0 > kResumeGiveUpMs) {
                gResumePending = false;
                gResumeT0 = 0;
                End("pending resume never came up");
                return;
            }
            if (g.active && g.outAt) End("page popped after fade-out");
            else if (g.active) End("page popped");
            return;
        }

        if (!g.active || g.page != g.lastTop || gResumePending) {
            const bool resumePage = gResumePending || (g.stack == MenuStack::main && g.resume);
            Begin(now, resumePage);
            gResumePending = false;
            gResumeT0 = 0;
        }
        g.lastTop = g.page;
        Drive(now);
        g.prevMs = now;
    }

    void Reset()
    {
        gResumePending = false;
        gResumeT0 = 0;
        End("reset");
    }

    bool Active() { return g.active || gResumePending; }
    bool HoldActive() { return (gResumePending || (g.active && g.hold)) && !g.disabled; }

} // namespace menu::loadseq
