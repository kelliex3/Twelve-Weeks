// ported from tools/ext/radar_box.c (build-201 working tree). The ext captured
// winmm's box blips with a detour on a text call and placed 12 pooled crates plus
// optional floor arrows / rim arcs. The mod already draws its own radar from
// NetPlayers::supplyBoxes (net/ui/kill-feed.cpp), so the same centres are fed to
// this owner with no call patch and no VA write. The ext's t2r-*.txt files are
// read-only here; no toggle is created or flipped.

#include "radar-box.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/lobby.hpp"
#include "engine/widget.hpp"
#include "log.hpp"
#include "net/ui/net-gui2.hpp"

#include <windows.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace net::ui::radar_box {

    namespace {

        using core::Blip;
        using core::Floor;

        struct Handles {
            uint64_t page = 0;
            bool     resolved = false;
            bool     onPage = false;
            bool     floor = false;
            bool     arcs = false;
            uint64_t lastCheck = 0;
            uint64_t firstFrame = 0;
            int      vis[core::kIcons]{};
            bool     av[2][core::kArcSteps]{};
            std::array<uint64_t, core::kIcons> crate{}, up{}, down{};
            std::array<std::array<uint64_t, core::kArcSteps>, 2> arc{};
        };

        Handles g;
        Blip    gBlips[core::kIcons];
        int     gN = 0;
        bool    gActive = false;

        bool Disabled()
        {
            return boot::ExtLoaded() ||
                   engine::flag::Present("t2r-old-radar-box.txt") ||
                   engine::lobby::Substate() < 6 || !net::gui2::InMatch();
        }

        void Name(char* out, size_t cap, char kind, int i)
        {
            std::snprintf(out, cap, "t2rRbx%c%d", kind, i);
        }

        bool Lookup(bool onPage, char kind, int i, uint64_t& w)
        {
            char name[24];
            Name(name, sizeof name, kind, i);
            w = onPage ? engine::widget::Page(name) : engine::widget::Hud(name);
            return w != 0;
        }

        bool LookupAll(bool onPage)
        {
            for (int i = 0; i < core::kIcons; ++i)
                if (!Lookup(onPage, 0, i, g.crate[i])) return false;
            return true;
        }

        void Resolve(uint64_t now)
        {
            const uint64_t top = engine::widget::TopPage();
            if (g.page && top != g.page) {
                const bool had = g.resolved;
                g = Handles{};
                g.page = top;
                if (had)
                    TO1_LOG("radar box: HUD page changed - re-resolving t2rRbx widgets\n");
            }
            if (!g.page) g.page = top;
            if (g.resolved || now - g.lastCheck < 1000) return;
            g.lastCheck = now;
            if (!g.firstFrame) g.firstFrame = engine::frame::Count() ? engine::frame::Count() : 1;

            const bool mayRoot = engine::frame::Count() - g.firstFrame >= 300 ||
                engine::flag::Present("t2r-radar-box-race-old.txt");
            for (int pass = 0; pass < (mayRoot ? 2 : 1); ++pass) {
                if (!LookupAll(pass == 0)) continue;
                g.resolved = true;
                g.onPage = pass == 0;
                for (int i = 0; i < core::kIcons; ++i) g.vis[i] = -1;
                TO1_LOG("radar box: t2rRbx0-%d found in %s - supply boxes use the lootbox icon\n",
                    core::kIcons - 1,
                    g.onPage ? "the currency-hud page" : "hud-root (old pak fallback)");
                break;
            }
            if (!g.resolved) return;

            if (g.onPage) {
                bool ok = true;
                for (int i = 0; i < core::kIcons && ok; ++i)
                    ok = Lookup(true, 'U', i, g.up[i]) &&
                         Lookup(true, 'D', i, g.down[i]);
                g.floor = ok;
                if (!ok)
                    for (auto& w : g.up) w = 0;
                TO1_LOG("box floor: %s\n", g.floor
                    ? "t2rRbxU / t2rRbxD found - other-floor boxes use the grey crate + arrow"
                    : "t2rRbxU / t2rRbxD missing - radar icons stay plain");

                ok = true;
                for (int c = 0; c < 2 && ok; ++c)
                    for (int k = 0; k < core::kArcSteps && ok; ++k) {
                        char name[24];
                        std::snprintf(name, sizeof name, "t2rRbx%c%d",
                                      c ? 'G' : 'A', k);
                        g.arc[c][k] = engine::widget::Page(name);
                        ok = g.arc[c][k] != 0;
                    }
                g.arcs = ok;
                if (!ok)
                    for (auto& a : g.arc) a.fill(0);
                TO1_LOG("box arcs: %s\n", g.arcs
                    ? "t2rRbxA / t2rRbxG found - out-of-range boxes use rim arcs"
                    : "t2rRbxA / t2rRbxG missing - pinned crates stay");
            }
        }

        void HideCrate(int i)
        {
            if (g.vis[i] < 0) return;
            const uint64_t w = g.vis[i] == 0 ? g.crate[i]
                             : g.vis[i] == 1 ? g.up[i] : g.down[i];
            if (w) engine::widget::Visible(w, false);
            g.vis[i] = -1;
        }

        void HideArcs()
        {
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < core::kArcSteps; ++k)
                    if (g.av[c][k]) {
                        if (g.arc[c][k]) engine::widget::Visible(g.arc[c][k], false);
                        g.av[c][k] = false;
                    }
        }

        void HideAll()
        {
            for (int i = 0; i < core::kIcons; ++i) HideCrate(i);
            HideArcs();
        }

    } // namespace

    void BeginFrame(uint64_t now)
    {
        gN = 0;
        if (Disabled()) {
            if (gActive) HideAll();
            gActive = false;
            return;
        }
        Resolve(now);
        gActive = g.resolved;
    }

    bool FeedBlip(float x, float y, bool pinned, core::Floor floor)
    {
        if (!gActive || gN >= core::kIcons) return false;
        Blip& b = gBlips[gN++];
        b.x = x;
        b.y = y;
        b.pinned = pinned;
        b.floor = floor;
        return true;
    }

    void EndFrame(uint64_t now)
    {
        (void)now;
        if (!gActive) return;

        bool wantArc[2][core::kArcSteps]{};
        for (int i = 0; i < gN; ++i) {
            Blip& b = gBlips[i];
            const Floor state = core::IconState(b.floor, g.floor);
            if (b.pinned && g.arcs) {
                const int k = core::ArcIndex(b.x, b.y);
                wantArc[state == Floor::Same ? 0 : 1][k] = true;
                continue;   // a pinned box shows an arc, not a crate
            }
            core::ClampIcon(b.x, b.y, g.onPage);
            const int slot = core::StateSlot(state);
            uint64_t w = slot == 0 ? g.crate[i]
                       : slot == 1 ? g.up[i] : g.down[i];
            if (!w) continue;
            engine::widget::Pivot(w, core::PivotX(b.x, g.onPage),
                                  core::PivotY(b.y));
            engine::widget::Alpha(w, core::IconAlpha(1.0f, 1.0f, g.onPage, false));
            if (g.vis[i] != slot) {
                HideCrate(i);
                engine::widget::Visible(w, true);
                g.vis[i] = slot;
            }
        }
        for (int i = gN; i < core::kIcons; ++i) HideCrate(i);

        const float al = core::IconAlpha(1.0f, 1.0f, g.onPage, false);
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < core::kArcSteps; ++k) {
                if (!g.arcs) break;
                const bool want = wantArc[c][k];
                if (want != g.av[c][k]) {
                    engine::widget::Visible(g.arc[c][k], want);
                    g.av[c][k] = want;
                }
                if (want) engine::widget::Alpha(g.arc[c][k], al);
            }
    }

    bool Active()
    {
        return gActive;
    }

    void Reset()
    {
        HideAll();
        for (Blip& b : gBlips) b = Blip{};
        gN = 0;
        gActive = false;
        g.page = 0;
        g.firstFrame = 0;
        g.resolved = false;
        g.floor = g.arcs = g.onPage = false;
        g.crate.fill(0);
        g.up.fill(0);
        g.down.fill(0);
        for (auto& a : g.arc) a.fill(0);
        for (int& v : g.vis) v = -1;
        std::memset(g.av, 0, sizeof g.av);
    }

} // namespace net::ui::radar_box
