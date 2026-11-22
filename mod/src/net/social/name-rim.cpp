// Native port of main's tools/ext/namerim.c (nrim_tick / nrim_apply).
// See name-rim.hpp for the honest gates (no scoreboard note, no clan 0xE1 reader).

#include "name-rim.hpp"

#include <winsock2.h>
#include <windows.h>

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/lobby.hpp"
#include "engine/widget.hpp"
#include "rank-badge.hpp"
#include "search-rows.hpp"
#include "friends-state.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace net::name_rim {

    namespace {

        constexpr int   kRimN = 8;
        constexpr float kRimR = 1.0f;                 // ext rank_badge_widgets.RIM_R
        constexpr float kDx[kRimN] = { 1.0f, 0.7071f, 0.0f, -0.7071f,
                                      -1.0f, -0.7071f, 0.0f, 0.7071f };
        constexpr float kDy[kRimN] = { 0.0f, 0.7071f, 1.0f, 0.7071f,
                                      0.0f, -0.7071f, -1.0f, -0.7071f };

        int       g_ownRole = 0;
        uint64_t  g_next = 0;

        struct RimRow {
            uint64_t gen = 0;
            uint64_t nameW = 0;
            int      on = -1;
            char     txt[64]{};
            float    px = -1e9f, py = -1e9f, sx = -1e9f, sy = -1e9f;
        };
        RimRow g_rows[8];

        bool Off()
        {
            static int off = -1;
            static uint64_t chk = 0;
            if (off < 0 || GetTickCount64() - chk > 1000) {
                chk = GetTickCount64();
                off = engine::flag::Absent("t2r-no-namerim.txt") ? 0 : 1;
            }
            return off != 0;
        }

        // ext nrim_strip: the name without <font ...> / <scale> markup (the rim's
        // pink tint is its own colour).
        void Strip(char* out, size_t cap, const char* text)
        {
            size_t k = 0;
            for (const char* p = text ? text : ""; *p && k + 1 < cap; p++) {
                if (*p == '<') {
                    const char* e = std::strchr(p, '>');
                    if (e) { p = e; continue; }
                }
                out[k++] = *p;
            }
            out[k] = 0;
        }

        // ext nrim_apply: write / hide the 8 clones (once per new text or new page
        // instance), then follow the name's pivot and scale.
        void Apply(RimRow* s, uint64_t nameW, uint64_t gen,
            const char* rimFmt, const char* show)
        {
            char stripped[64];
            Strip(stripped, sizeof stripped, show);
            const bool on = stripped[0] != 0 && nameW != 0;
            if (s->gen != gen || s->nameW != nameW) {
                *s = RimRow{};
                s->gen = gen;
                s->nameW = nameW;
                s->on = -1;
            }
            if (on == (s->on != 0) && (!on || !std::strcmp(s->txt, stripped))) {
                if (!on)
                    return;
            } else {
                for (int i = 0; i < kRimN; i++) {
                    char p[160];
                    std::snprintf(p, sizeof p, rimFmt, i);
                    if (on)
                        engine::widget::SetText(engine::widget::Page(p), stripped);
                    engine::widget::Visible(engine::widget::Page(p), on);
                }
                s->on = on ? 1 : 0;
                std::snprintf(s->txt, sizeof s->txt, "%s", stripped);
                s->px = s->py = s->sx = s->sy = -1e9f;
                if (!on)
                    return;
            }
            float x = 0.0f, y = 0.0f, sx = 1.0f, sy = 1.0f;
            if (!engine::widget::PivotGet(nameW, &x, &y))
                return;
            engine::widget::ScaleGet(nameW, &sx, &sy);
            if (x == s->px && y == s->py && sx == s->sx && sy == s->sy)
                return;
            s->px = x; s->py = y; s->sx = sx; s->sy = sy;
            for (int i = 0; i < kRimN; i++) {
                char p[160];
                std::snprintf(p, sizeof p, rimFmt, i);
                const uint64_t w = engine::widget::Page(p);
                if (!w)
                    continue;
                engine::widget::Pivot(w, x + kDx[i] * kRimR, y + kDy[i] * kRimR);
                engine::widget::Scale(w, sx, sy);
            }
        }

        // The shown name of a lobby row: the search mirror first (SEARCHING), then
        // the read-only roster order (MATCH FOUND). Row 0 is always us.
        const char* RowName(int r, char* buf, size_t cap, bool* me)
        {
            *me = r == 0;
            const char* s = net::social::search_rows::Name(r);
            if (s && s[0]) {
                std::snprintf(buf, cap, "%s", s);
                return buf;
            }
            if (r > 0 && engine::lobby::Ready()) {
                int idx[engine::lobby::kRosterMax]{};
                int en[engine::lobby::kRosterMax]{};
                const int n = engine::lobby::RosterSorted(idx, en);
                if (n > 0 && r - 1 < n
                    && engine::lobby::RosterName(idx[r - 1], buf, cap) && buf[0])
                    return buf;
            }
            buf[0] = 0;
            return buf;
        }

    } // namespace

    void SetOwnRole(int role)
    {
        g_ownRole = role >= 0 && role <= social::kRankMaxRole ? role : 0;
    }

    void Tick(uint64_t now)
    {
        if (now < g_next)
            return;
        g_next = now + 150;

        if (engine::lobby::InMatch())
            return;                 // the menu rows are gone; a new page starts fresh

        const bool off = Off();
        const uint64_t gen = engine::widget::TopPage();
        for (int r = 0; r < 8; r++) {
            char name[48];
            bool me = false;
            const char* shown = RowName(r, name, sizeof name, &me);
            char np[96];
            std::snprintf(np, sizeof np,
                "anim-group.lobbyPanel.lobbyList.Player%d.nameLabel", r);
            const uint64_t nw = engine::widget::Page(np);
            int ro = 0;
            if (!me && shown && shown[0] && !social::RankPlaceholder(shown))
                ro = ::net::rank_badge::RoleOf(shown);
            else if (me)
                ro = g_ownRole;
            char fmt[96];
            std::snprintf(fmt, sizeof fmt,
                "anim-group.lobbyPanel.lobbyList.Player%d.nmRim%%d", r);
            Apply(&g_rows[r], nw, gen, fmt, !off && ro == 4 ? shown : nullptr);
        }
        // Scoreboard rims (ext's SS[k] / g_rkb_sbw / g_rkb_sbn) are intentionally
        // absent: the mod has no scoreboard settext note, so there is no name to
        // look up and no widget to follow -- writing nothing is the honest gate.
    }

    void Reset()
    {
        for (RimRow& r : g_rows)
            r = RimRow{};
        g_next = 0;
    }

} // namespace net::name_rim
