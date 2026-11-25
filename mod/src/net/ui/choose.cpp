// Native owner for build-191 CHOOSE YOUR SIDE (tools/ext/choose.c + side_tick).
// Core timing/focus/payoff decisions live in choose-core.hpp. This owner is a
// conservative widget seam: it yields to the ext, never writes camera/mixer,
// and only touches widgets that exist in the loaded GUI pak.

#include "choose.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "engine/locale.hpp"
#include "engine/menu-input.hpp"
#include "log.hpp"             // TO1_LOG must precede engine/script.hpp
#include "engine/script.hpp"
#include "engine/widget.hpp"
#include "net/online/clan-state.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace net::ui::choose {

    namespace {

        using namespace core;

        struct Runtime {
            Page page;
            Art  art;
            Payoff payoff;
            bool entered = false;
            bool hSel = true, hBack = true, hLeft = true, hRight = true;
            uint64_t lastTop = 0;
        } g;

        bool ArtDisabled() { return engine::flag::Present("t2r-no-choose-art.txt"); }
        bool IntroDisabled() { return engine::flag::Present("t2r-no-choose-intro.txt"); }

        // ext resolves these under the current "clan-side" page, not hudRoot
        // (tools/ext/choose.c:28, t2r-ext.c:49564-49575).
        uint64_t SidePage() { return engine::widget::PageId("clan-side"); }
        uint64_t SideRoot() { return engine::widget::Page("anim-group.side"); }
        bool SidePageUp()
        {
            return engine::widget::TopPage() == SidePage() && SideRoot() != 0;
        }

        void Vis(const char* rel, bool on)
        {
            const uint64_t w = engine::widget::Page(rel);
            if (w) engine::widget::Visible(w, on);
        }

        void Alpha(const char* rel, float a)
        {
            const uint64_t w = engine::widget::Page(rel);
            if (w) engine::widget::Alpha(w, std::clamp(a, 0.0f, 1.0f));
        }

        void Text(const char* rel, const char* text)
        {
            const uint64_t w = engine::widget::Page(rel);
            if (w && text) engine::widget::SetText(w, engine::locale::Tr(text));
        }

        void ApplyArt(uint64_t now)
        {
            if (!g.art.on) return;
            const float z = g.art.zoom.Val(now);
            const float p = g.art.pan.Val(now);
            (void)z; (void)p; // camera seam: no track evaluator in native owner yet

            for (int k = 0; k < 2; ++k) {
                const float n = g.art.name[k].Val(now);
                char rel[64];
                std::snprintf(rel, sizeof rel, "anim-group.side.chUi.chName%d", k);
                Alpha(rel, 0.35f + 0.65f * n);
                std::snprintf(rel, sizeof rel, "anim-group.side.chArt.chLogo%d", k);
                const float ls = g.art.logo[k].Val(now);
                const uint64_t lw = engine::widget::Page(rel);
                if (lw) engine::widget::Scale(lw, ls, ls);
            }
            const float ta = TitleAmount(g.art, now, g.page.pickSide, false);
            Alpha("anim-group.side.chUi.chTitle", ta);
            const float ea = ExitAmount(g.page, now);
            Alpha("anim-group.side", ea);
        }

        void ClosePick(uint64_t now)
        {
            Confirm(g.page, g.art.focus >= 0 ? g.art.focus : g.page.focus, g.art.on, false, now);
            if (g.art.on) StartPayoff(g.payoff, g.page.pickSide, true, now);
        }

    } // namespace

    void Tick(uint64_t now)
    {
        if (boot::ExtLoaded()) return;
        const uint64_t page = SidePage();
        if (engine::widget::TopPage() != page || SideRoot() == 0) {
            if (g.entered) Reset();
            return;
        }
        if (!g.entered) {
            g.entered = true;
            const bool art = !ArtDisabled();
            Enter(g.page, false, art, false, now);
            Show(g.art, art, now);
            g.hSel = g.hBack = g.hLeft = g.hRight = true;
            TO1_LOG("choose: native owner entered (art=%d intro=%d)\n",
                    art ? 1 : 0, IntroDisabled() ? 0 : 1);
        }

        const bool sel = engine::menu_input::CommandEdge(SID("menu-select"), g.hSel);
        const bool back = engine::menu_input::CommandEdge(SID("menu-back"), g.hBack)
                       || engine::menu_input::CommandEdge(SID("menu-cancel"), g.hBack);
        const bool left = engine::menu_input::CommandEdge(SID("menu-left"), g.hLeft);
        const bool right = engine::menu_input::CommandEdge(SID("menu-right"), g.hRight);

        if (g.page.stage == kIntro) {
            if (sel && IntroSelect(g.page, now)) {
                Vis("anim-group.side", true);
                Alpha("anim-group.side", 0.0f);
            }
        } else if (g.page.stage == kCrossfade) {
            Alpha("anim-group.side", CrossfadeAlpha(g.page, now));
            if (CrossfadeDone(g.page, now)) Alpha("anim-group.side", 1.0f);
        }

        if (g.page.stage == kPick || g.page.stage == kCrossfade) {
            if (left || right) Input(g.art, left, right, now);
            if (sel && g.art.focus >= 0) ClosePick(now);
            if (back) {
                if (!Back(g.art, g.page.pickSide >= 0, now)) {
                    g.page.stage = kFadeOut;
                    g.page.exitAt = now;
                    g.page.pickSide = -1;
                }
            }
        }

        if (g.page.stage == kPayoff) {
            ApplyArt(now);
            if (PayoffSoundDue(g.payoff, now))
                AUDIO::PLAY_SOUND(SID("sfx-rl-hub-interact-map-select"));
            if (PayoffDone(g.payoff, now)) PayoffEnded(g.page, now);
            return;
        }

        if (g.page.stage == kFadeOut) {
            ApplyArt(now);
            if (FadeOutDone(g.page, now)) {
                const int side = g.page.pickSide;
                Popped(g.page);
                if (side >= 0) {
                    // Authoritative daemon write (0xE4) is owned by the clan/side
                    // runtime; this native owner only records the pick locally.
                    TO1_LOG("choose: pick side=%d (daemon 0xE4 seam deferred)\n", side);
                }
                Reset();
            }
            return;
        }

        ApplyArt(now);
    }

    bool Active() { return !boot::ExtLoaded() && g.entered; }
    int Stage() { return g.page.stage; }

    void Reset()
    {
        g = Runtime{};
    }

} // namespace net::ui::choose
