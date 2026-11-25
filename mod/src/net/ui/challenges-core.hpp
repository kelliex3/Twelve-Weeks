#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Pure core of tools/ext/customize_challenges.c (build 191). The native owner
// owns the page push/pop, widget writes, sfx, file save and network change;
// this header owns the grid, focus and the first-visit tip state so it can be
// compiled on its own in mod/tests/challenges_core_test.cpp. It consumes the
// shared build-191 item tables in customize-items.hpp / customize-core.hpp.

#include "customize-core.hpp"

namespace net::ui::customize::challenges::core {

    // Reuse the build-191 profile / item tables without repeating their names.
    using namespace net::ui::customize::core;

    // Screen grid (6 x 3). The parent customize core has its own kCols/kRows
    // (3 x 4 cards); distinct names keep call sites unambiguous.
    constexpr int   kScreenCols = 6;
    constexpr int   kScreenRows = 3;
    constexpr int   kMaxItems  = 48;
    constexpr float kX0        = 148.0f;
    constexpr float kTileW     = 300.0f;
    constexpr float kTileH     = 275.0f;
    constexpr float kGap       = 24.0f;
    constexpr float kX1        = kX0 + kScreenCols * kTileW + (kScreenCols - 1) * kGap;
    constexpr float kRowY0     = 440.0f;
    constexpr float kRowPitch  = 440.0f;
    constexpr float kTileDY    = 46.0f;
    constexpr float kBarDY     = kTileH + 16.0f;
    constexpr float kSbX       = kX1 + 60.0f;
    constexpr float kDX0       = 2300.0f;
    constexpr float kDX1       = 3700.0f;
    constexpr float kDetailArtY = 400.0f;
    constexpr float kDetailArtH = 715.0f;
    constexpr float kDetailTitleY = kDetailArtY + kDetailArtH + 70.0f;
    constexpr float kTipX1     = 2870.0f;
    constexpr float kTipContY  = 1816.0f;
    constexpr uint64_t kEnterFadeMs = 220;
    constexpr uint64_t kEquipGateMs = 300;
    constexpr uint64_t kBackFrames  = 6;

    using Profile = core::Profile;

    inline int Sex(const Profile& p, int fac)
    {
        return fac >= 0 && fac < CZ_NFAC && p.sex[fac] < CZ_NSEX ? p.sex[fac] : 0;
    }

    // PlainKind() comes from the parent customize core (the same CZK_* set).
    inline bool StoryMaskOff(const cz_item_t* it, bool storyMasksShown)
    {
        // The ext's cz_story_mask(it) is the picture slug check in
        // customize-core.hpp ("-grp-t2r-gm-"), not a label / hid heuristic.
        return StoryMask(it) && !storyMasksShown;
    }

    // nr_prompts() draws declaration order right-to-left but nr_prompt_hover()
    // returns the declaration index: SWITCH=0, EQUIP=1, BACK=2 (build 191).
    enum { kPromptSwitch = 0, kPromptEquip = 1, kPromptBack = 2 };

    struct Grid {
        int  fac = 0;
        int  nrow = 0;
        int  rcat[kWearCats]{};                 // category per row
        int  rn[kWearCats]{};                   // item count per row
        int  rk[kWearCats][kMaxItems]{};        // list indexes (row order)
    };

    // Rows of a faction + sex: every category before SEX with at least one
    // real challenge item (OG / KELLIE / ACH are plain lines, not challenges).
    inline int RowsOf(int fac, int sex, const Profile& p, bool storyMasksShown,
                      Grid& out)
    {
        out.fac = fac;
        out.nrow = 0;
        for (int c = 0; c < kWearCats; ++c) {
            int n = 0;
            for (int k = 0; k < Count(fac, sex, c) && n < kMaxItems; ++k) {
                const cz_item_t* it = Item(fac, sex, c, k);
                if (it && it->uk && it->uk < CZ_NCHAL
                    && !PlainKind(CZ_CHAL[it->uk].kind)
                    && !StoryMaskOff(it, storyMasksShown))
                    out.rk[out.nrow][n++] = k;
            }
            if (n) {
                out.rcat[out.nrow] = c;
                out.rn[out.nrow] = n;
                ++out.nrow;
            }
        }
        (void)p;
        return out.nrow;
    }

    inline void Count(const Profile& p, int fac, bool both, bool storyMasksShown,
                      int& done, int& total)
    {
        done = total = 0;
        for (int s = 0; s < CZ_NSEX; ++s) {
            if (!both && s != Sex(p, fac)) continue;
            Grid g;
            const int nr = RowsOf(fac, s, p, storyMasksShown, g);
            for (int r = 0; r < nr; ++r)
                for (int i = 0; i < g.rn[r]; ++i) {
                    ++total;
                    if (Unlocked(p, Item(fac, s, g.rcat[r], g.rk[r][i])))
                        ++done;
                }
        }
    }

    // The page's focus / navigation state (CZC in customize_challenges.c).
    struct State {
        bool     up = false, quiet = false, closing = false;
        uint64_t enterMs = 0, key = 0, backFrame = 0;
        int      fac = 0;
        Grid     grid;
        int      fr = 0, fc = 0, row0 = 0, want = 0;
        int      off[kWearCats]{};
        int      hvR = -1, hvC = -1, hvTab = -1, hvQ = 0, hvE = 0, hvPb = -1;
    };

    inline void KeepVisible(State& s);

    inline void Enter(State& s, const Profile& p, int fac, bool storyMasksShown,
                      int openCat, uint64_t now)
    {
        s = State{};
        s.up = true;
        s.enterMs = now;
        s.fac = fac >= 0 && fac < CZ_NFAC ? fac : 0;
        RowsOf(s.fac, Sex(p, s.fac), p, storyMasksShown, s.grid);
        for (int r = 0; r < s.grid.nrow; ++r)
            if (s.grid.rcat[r] == openCat) s.fr = r;
        KeepVisible(s);
    }

    inline bool RowOk(const State& s, int r) { return r >= 0 && r < s.grid.nrow; }

    inline void KeepVisible(State& s)
    {
        if (!s.grid.nrow) { s.fr = s.fc = s.row0 = 0; return; }
        s.fr = std::max(0, std::min(s.grid.nrow - 1, s.fr));
        s.fc = std::max(0, std::min(s.grid.rn[s.fr] - 1, s.fc));
        s.row0 = std::max(0, std::min(s.grid.nrow - kScreenRows, std::min(s.fr, s.row0)));
        if (s.fr < s.row0) s.row0 = s.fr;
        if (s.fr >= s.row0 + kScreenRows) s.row0 = s.fr - kScreenRows + 1;
        int& o = s.off[s.fr];
        if (s.fc < o) o = s.fc;
        if (s.fc >= o + kScreenCols) o = s.fc - kScreenCols + 1;
    }

    // Up / down: the next row, the same screen column (or its last item).
    inline void MoveRow(State& s, int d)
    {
        const int r = s.fr + d;
        if (!RowOk(s, r)) return;
        s.fr = r;
        s.fc = std::min(s.off[r] + s.want, s.grid.rn[r] - 1);
    }

    // Q / E / bumpers: wrap around the two factions and rebuild the grid.
    inline bool SetFac(State& s, const Profile& p, int f, bool storyMasksShown)
    {
        f = (f % CZ_NFAC + CZ_NFAC) % CZ_NFAC;
        if (f == s.fac) return false;
        s.fac = f;
        RowsOf(f, Sex(p, f), p, storyMasksShown, s.grid);
        s.fr = s.fc = s.row0 = s.want = 0;
        std::fill(s.off, s.off + kWearCats, 0);
        KeepVisible(s);
        return true;
    }

    struct EquipResult {
        bool     changed = false;
        bool     denied = false;
        const char* denyText = nullptr;
        int      cat = -1, item = -1;
    };

    // Validate a challenge item before the owner writes the profile and saves.
    inline EquipResult CanEquip(const State& s, const Profile& p, int fac,
                                int cat, int item)
    {
        EquipResult r;
        if (!RowOk(s, s.fr)) return r;
        const cz_item_t* it = Item(fac, Sex(p, fac), cat, item);
        if (!it || !Unlocked(p, it)) { r.denied = true; return r; }
        if (it->tier > 1) { r.denied = true; r.denyText = "NOT IN MULTIPLAYER YET"; return r; }
        if (p.it[fac][Sex(p, fac)][cat] == item) return r;
        r.changed = true;
        r.cat = cat;
        r.item = item;
        return r;
    }

    inline void TileXY(int r, int c, float& x, float& y)
    {
        x = kX0 + c * (kTileW + kGap);
        y = kRowY0 + r * kRowPitch + kTileDY;
    }

    inline void ScanHover(State& s, float cx, float cy, bool padMode, bool cursorValid)
    {
        s.hvR = s.hvC = s.hvTab = s.hvPb = -1;
        s.hvQ = s.hvE = 0;
        if (padMode || !cursorValid) return;
        for (int r = 0; r < kScreenRows; ++r) {
            const int ri = s.row0 + r;
            if (!RowOk(s, ri)) break;
            for (int c = 0; c < kScreenCols; ++c) {
                float x, y;
                TileXY(r, c, x, y);
                if (cx >= x && cx < x + kTileW && cy >= y && cy < y + kTileH
                    && s.off[ri] + c < s.grid.rn[ri]) {
                    s.hvR = ri;
                    s.hvC = s.off[ri] + c;
                }
            }
        }
    }

    inline bool ClickHit(const State& s)
    {
        if (!s.up || s.quiet) return false;
        return s.hvR >= 0 || s.hvTab >= 0 || s.hvQ || s.hvE || s.hvPb >= 0;
    }

    inline void ScrollRows(State& s, int notches)
    {
        if (!notches || s.grid.nrow <= kScreenRows) return;
        const int max = s.grid.nrow - kScreenRows;
        s.row0 = std::max(0, std::min(max, s.row0 - notches));
        if (s.fr < s.row0) MoveRow(s, s.row0 - s.fr);
        if (s.fr >= s.row0 + kScreenRows) MoveRow(s, s.row0 + kScreenRows - 1 - s.fr);
        KeepVisible(s);
    }

    inline void ScrollCols(State& s, int row, int notches)
    {
        if (!notches || !RowOk(s, row)) return;
        int& o = s.off[row];
        const int max = std::max(0, s.grid.rn[row] - kScreenCols);
        o = std::max(0, std::min(max, o - notches));
    }

    // The fade-in quantises to 24 steps of the anim-group's alpha.
    inline int FadeStep(const State& s, uint64_t now)
    {
        if (now < s.enterMs) return 0;
        const float f = std::min(1.0f, static_cast<float>(now - s.enterMs)
                                       / static_cast<float>(kEnterFadeMs));
        return static_cast<int>(f * 24.0f + 0.5f);
    }

    // ---- first-visit tip on the CUSTOMIZE page --------------------------------

    struct Tip {
        bool     up = false;
        uint64_t at = 0;
    };

    inline bool TipWanted(const Profile& p)
    {
        return (p.flags & kFlagChTip) == 0;
    }

    inline void TipShow(Tip& t, uint64_t now) { t.up = true; t.at = now; }
    inline void TipDone(Tip& t, Profile& p)
    {
        t.up = false;
        p.flags |= kFlagChTip;
    }

} // namespace net::ui::customize::challenges::core
