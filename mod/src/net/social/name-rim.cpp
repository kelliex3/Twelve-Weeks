// Native port of build-191 tools/ext/namerim.c (nrim_tick / nrim_apply).
// Lobby rows come from the read-only search / lobby mirrors; scoreboard rows
// from net-gui2's read-only name mirror. See name-rim.hpp for the honest gates.

#include "name-rim.hpp"
#include "name-rim-core.hpp"

#include <windows.h>

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp
#include "loader/boot.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/lobby.hpp"
#include "engine/widget.hpp"
#include "net/online/clan-state.hpp"
#include "net/online/net-session.hpp"
#include "net/ui/net-gui2.hpp"
#include "rank-badge.hpp"
#include "search-rows.hpp"
#include "friends-state.hpp"

#include <cstdio>
#include <cstring>

namespace net::name_rim {

    namespace {

        // The pure rules keep their own nested namespace; the native owner stays
        // at net::name_rim like its boot/reset call sites.
        namespace core = ::net::social::name_rim::core;

        using core::RimState;

        RimState g_lobby[core::kRows];
        RimState g_score[core::kRows];
        uint64_t g_next = 0;
        int      g_roleOverride = 0;    // test / producer seam; clan state wins at 0

        bool Off()
        {
            static int off = -1;
            static uint64_t chk = 0;
            if (off < 0 || ::GetTickCount64() - chk > 1000) {
                chk = ::GetTickCount64();
                off = engine::flag::Absent("t2r-no-namerim.txt") ? 0 : 1;
            }
            return off != 0;
        }

        // ext nrim_apply: write / hide the eight rim clones once per new text or
        // new page instance, then follow the name's pivot and scale.
        void Apply(RimState* s, uint64_t nameW, uint64_t gen,
                   const char* rimFmt, const char* show)
        {
            char stripped[64];
            core::Strip(stripped, sizeof stripped, show);
            const bool on = stripped[0] != 0 && nameW != 0;
            if (s->gen != gen || s->nameW != nameW)
                core::ResetState(*s, gen, nameW);
            if (core::NeedRewrite(*s, on, stripped)) {
                for (int i = 0; i < core::kRimN; ++i) {
                    char p[160];
                    std::snprintf(p, sizeof p, rimFmt, i);
                    const uint64_t w = engine::widget::Page(p);
                    if (!w) continue;
                    if (on) engine::widget::SetText(w, stripped);
                    engine::widget::Visible(w, on);
                }
                s->on = on ? 1 : 0;
                std::snprintf(s->txt, sizeof s->txt, "%s", stripped);
                s->px = s->py = s->sx = s->sy = -1e9f;
                if (!on) return;
            }
            float x = 0.0f, y = 0.0f, sx = 1.0f, sy = 1.0f;
            if (!engine::widget::PivotGet(nameW, &x, &y)) return;
            if (!engine::widget::ScaleGet(nameW, &sx, &sy)) { sx = 1.0f; sy = 1.0f; }
            if (x == s->px && y == s->py && sx == s->sx && sy == s->sy) return;
            s->px = x; s->py = y; s->sx = sx; s->sy = sy;
            for (int i = 0; i < core::kRimN; ++i) {
                char p[160];
                std::snprintf(p, sizeof p, rimFmt, i);
                const uint64_t w = engine::widget::Page(p);
                if (!w) continue;
                float ox = 0.0f, oy = 0.0f;
                core::Place(x, y, sx, sy, i, ox, oy);
                engine::widget::Pivot(w, ox, oy);
                engine::widget::Scale(w, sx, sy);
            }
        }

        // The shown name of a lobby row: the search mirror first (SEARCHING),
        // then the read-only roster order (MATCH FOUND). Row 0 is always us.
        const char* RowName(int r, char* buf, size_t cap)
        {
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
        g_roleOverride = role >= 0 && role <= social::kRankMaxRole ? role : 0;
    }

    void Tick(uint64_t now)
    {
        if (boot::ExtLoaded() || !core::ShouldTick(now, g_next)) return;
        g_next = now + core::kTickMs;

        const bool off = Off();
        const uint64_t gen = engine::widget::TopPage();
        const int ownRole = g_roleOverride ? g_roleOverride : ::net::clan::OwnRole();
        const char* local = ::net::session::LocalName();

        // Lobby rows (MP menu / find match). In a match the page is gone; a new
        // page instance starts over (new handles, Apply's gen check).
        if (!engine::lobby::InMatch()) {
            for (int r = 0; r < core::kRows; ++r) {
                char name[48];
                const char* shown = RowName(r, name, sizeof name);
                const bool me = r == 0;
                char np[96], fmt[96];
                core::LobbyNamePath(np, sizeof np, r);
                core::LobbyRimPath(fmt, sizeof fmt, r);
                const uint64_t nw = engine::widget::Page(np);
                int ro = 0;
                if (me) ro = ownRole;
                else if (shown && shown[0] && !social::RankPlaceholder(shown))
                    ro = ::net::rank_badge::RoleOf(shown);
                const bool show = core::ShouldShow(off, ro, shown);
                Apply(&g_lobby[r], nw, gen, fmt, show ? shown : nullptr);
            }
        }

        // Scoreboards (ESC page + end of match): names mirrored read-only by
        // net-gui2. Slot k is team(k/4) row(k%4), the ext's SS[k] layout.
        if (::net::gui2::ScoreboardNamesUp()) {
            for (int k = 0; k < core::kRows; ++k) {
                const char* nm = ::net::gui2::ScoreboardRowName(k);
                if (!nm || !nm[0]) continue;
                char fmt[96];
                core::ScoreboardRimPath(fmt, sizeof fmt, k);
                int ro = 0;
                if (!core::IsScoreboardPlaceholder(nm)) {
                    if (core::IsMe(nm, local)) ro = ownRole;
                    else ro = ::net::rank_badge::RoleOf(nm);
                }
                const bool show = core::ShouldShow(off, ro, nm);
                char np[96];
                std::snprintf(np, sizeof np,
                              "anim-group.scoreboard.team%d_row%d_name", k / 4, k % 4);
                Apply(&g_score[k], engine::widget::Page(np), gen, fmt,
                      show ? nm : nullptr);
            }
        }
    }

    void Reset()
    {
        for (RimState& r : g_lobby) r = RimState{};
        for (RimState& r : g_score) r = RimState{};
        g_next = 0;
    }

} // namespace net::name_rim
