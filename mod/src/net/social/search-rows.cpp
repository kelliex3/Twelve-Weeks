// Native mirror of tools/ext/t2r-ext.c SrchNameWrite / g_srchTab / g_srchName.

#include "search-rows.hpp"

#include "engine/frame.hpp"
#include "engine/lobby.hpp"
#include "engine/widget.hpp"

#include <cstdio>
#include <cstring>

namespace net::social::search_rows {

    namespace {

        constexpr int    kRows    = 8;
        constexpr size_t kNameCap = 48;   // ext g_srchName[8][48]
        constexpr int    kPending = 128;  // ext namerim's pending-write ring size
        constexpr uint64_t kResolveFrames = 120;

        struct Pending { uint64_t w; char text[96]; };
        Pending  g_pending[kPending];
        unsigned g_pi = 0;

        uint64_t g_handle[kRows]{};
        uint64_t g_resolvedAt = 0;
        int      g_tab[kRows]{};
        char     g_name[kRows][kNameCap]{};

        bool Installed()
        {
            static bool installed = false;
            if (!installed) {
                installed = true;
                engine::widget::SetTextObserver(&Note, nullptr);
            }
            return true;
        }

        bool OnSearchPage()
        {
            return engine::widget::TopPage()
                == engine::widget::PageId("find-match-menu");
        }

        // ext SrchNameWrite: cut " [T1]" / " [T2]" and " (party ... not searching)"
        // off the shown name and remember the relative team in the tab byte.
        void Parse(int row, const char* text)
        {
            const char* mk = std::strstr(text, " [T");
            const char* ns = std::strstr(text, "not searching");
            int tab = 0;
            char out[kNameCap];
            if (mk && mk[3] >= '1' && mk[3] <= '2' && mk[4] == ']') {
                tab = mk[3] == '1' ? 1 : 2;
                std::snprintf(out, sizeof out, "%.*s%s",
                    static_cast<int>(mk - text), text, mk + 5);
            } else if (ns) {
                const char* pp = std::strstr(text, " (party");
                if (pp && pp < ns)
                    std::snprintf(out, sizeof out, "%.*s",
                        static_cast<int>(pp - text), text);
                else
                    std::snprintf(out, sizeof out, "%s", text);
                tab = 3;
            } else {
                std::snprintf(out, sizeof out, "%s", text);
            }
            out[kNameCap - 1] = 0;
            std::memcpy(g_name[row], out, sizeof g_name[row]);
            g_tab[row] = tab;
        }

        // ext's MATCH FOUND branch: row r shows the r-th player of the our-team-first
        // order; a placeholder ("PLAYER n") stays. The lobby is only read here.
        const char* MatchFoundName(int row, const char* text, char* buf, size_t cap)
        {
            if (row < 1 || engine::lobby::PartyStatus() < 3 || !engine::lobby::Ready())
                return text;
            int idx[engine::lobby::kRosterMax]{};
            int enemy[engine::lobby::kRosterMax]{};
            const int n = engine::lobby::RosterSorted(idx, enemy);
            if (n <= 0 || row - 1 >= n)
                return text;
            bool known = false;
            for (int i = 0; i < engine::lobby::RosterCount(); ++i) {
                char nm[49];
                if (engine::lobby::RosterName(i, nm, sizeof nm)
                    && !std::strncmp(nm, text, 0x30)) {
                    known = true;
                    break;
                }
            }
            if (!known)
                return text;
            char nm[49];
            if (!engine::lobby::RosterName(idx[row - 1], nm, sizeof nm) || !nm[0])
                return text;
            std::snprintf(buf, cap, "%s", nm);
            return buf;
        }

        void Store(int row, const char* text)
        {
            char buf[96];
            text = MatchFoundName(row, text, buf, sizeof buf);
            Parse(row, text);
        }

        // ext SrchNameWrite's handle lookup: re-resolve the 8 name widgets (the page
        // rebuilds rows) and replay the newest pending write for any handle that moved.
        // No SetText is called here, so it is safe from inside the text observer.
        void Resolve()
        {
            for (int r = 0; r < kRows; ++r) {
                char p[96];
                std::snprintf(p, sizeof p,
                    "anim-group.lobbyPanel.lobbyList.Player%d.nameLabel", r);
                g_handle[r] = engine::widget::Page(p);
            }
            g_resolvedAt = engine::frame::Count();
            for (unsigned k = 1; k <= kPending; ++k) {
                const Pending& p = g_pending[(g_pi - k) % kPending];
                if (!p.w)
                    continue;
                for (int r = 0; r < kRows; ++r)
                    if (g_handle[r] == p.w) {
                        Store(r, p.text);
                        break;
                    }
            }
        }

    } // namespace

    void Note(uint64_t widget, const char* text, void*)
    {
        if (!widget || !text)
            return;
        for (int r = 0; r < kRows; ++r)
            if (g_handle[r] == widget) {
                Store(r, text);
                return;
            }
        // A row's text can be written before the page's handles are known: keep the
        // newest write like the ext's settext hook does for rebuilt rows.
        Pending& p = g_pending[g_pi++ % kPending];
        p.w = widget;
        std::snprintf(p.text, sizeof p.text, "%s", text);
        // ext: re-resolve on a [T marker write, throttled to 30 frames (the search
        // page rebuilds rows while the markers are being written).
        if (!g_handle[0] || std::strstr(text, " [T")
            || engine::frame::Count() - g_resolvedAt >= kResolveFrames)
            Resolve();
    }

    void Tick()
    {
        Installed();
        if (!OnSearchPage()) {
            for (int r = 0; r < kRows; ++r) {
                g_handle[r] = 0;
                g_tab[r] = 0;
                g_name[r][0] = 0;
            }
            g_resolvedAt = 0;
            return;
        }

        const uint64_t frame = engine::frame::Count();
        if (!g_handle[0] || frame - g_resolvedAt >= kResolveFrames)
            Resolve();
    }

    void Reset()
    {
        for (int r = 0; r < kRows; ++r) {
            g_handle[r] = 0;
            g_tab[r] = 0;
            g_name[r][0] = 0;
        }
        std::memset(g_pending, 0, sizeof g_pending);
        g_pi = 0;
        g_resolvedAt = 0;
    }

    int Tab(int row)
    {
        return row >= 0 && row < kRows ? g_tab[row] : 0;
    }

    const char* Name(int row)
    {
        return row >= 0 && row < kRows ? g_name[row] : "";
    }

} // namespace net::social::search_rows
