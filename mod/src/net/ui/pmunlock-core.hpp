// Pure rules of the post-match UNLOCKS stage (tools/ext/pmunlock.c). No engine,
// Windows or widget dependency: the native owner in postmatch-screens.cpp
// supplies the profile / achievement snapshot and does all widget writes.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace net::ui::pmunlock::core {

    inline constexpr int kRows = 4;
    inline constexpr int kMaxG = 24;
    inline constexpr int kMaxA = 8;         // ext ACH_TOAST_Q
    inline constexpr int kFac = 2;          // CZ_NFAC
    inline constexpr int kSex = 2;          // CZ_NSEX
    inline constexpr int kWear = 7;         // CZ_NWEAR
    inline constexpr int kMaxIt = 48;       // CZC_MAXIT

    inline constexpr float kY0 = 530.0f;
    inline constexpr float kDy = 250.0f;
    inline constexpr float kTx = 280.0f;
    inline constexpr float kSecY = 470.0f;
    inline constexpr uint64_t kStagMs = 140;
    inline constexpr uint64_t kFadeMs = 260;
    inline constexpr uint64_t kRebuildMs = 250;

    // PMN_LX0 / PMN_LX1 / PMN_RX0 / PMN_RX1 (t2r-ext.c) and the 36 pt section.
    inline constexpr float kLx0 = 148.0f, kLx1 = 1900.0f;
    inline constexpr float kRx0 = 2100.0f, kRx1 = 3700.0f;
    inline constexpr float kTitlePt = 36.0f;
    inline constexpr float kRightPt = 33.0f;

    struct Achievement {
        char id[20]{};
        char title[48]{};
        char desc[112]{};
    };

    struct Gear {
        uint8_t f = 0, s = 0, c = 0, k = 0;
    };

    // The ext's g_czu.was: a flat copy of the CUSTOMIZE unlock grid at match
    // start. Cell order is [fac][sex][wear][item].
    struct Snapshot {
        std::array<uint8_t, kFac * kSex * kWear * kMaxIt> was{};
        bool have = false;
    };

    struct Lists {
        int na = 0;
        int ng = 0;
        int ngt = 0;
        std::array<Achievement, kMaxA> a{};
        std::array<Gear, kMaxG> g{};
        uint64_t builtAt = 0;
        int facFirst = 0;
    };

    using Grid = std::array<std::array<std::array<std::array<bool, kMaxIt>, kWear>, kSex>, kFac>;

    [[nodiscard]] inline size_t Cell(int f, int s, int c, int k) noexcept
    {
        if (f < 0 || f >= kFac || s < 0 || s >= kSex || c < 0 || c >= kWear
            || k < 0 || k >= kMaxIt) return 0;
        return (static_cast<size_t>(f) * kSex + s) * kWear * kMaxIt
             + static_cast<size_t>(c) * kMaxIt + k;
    }

    // pmu_match_start: remember what was unlocked before the match.
    inline void TakeSnapshot(const Grid& now, Snapshot& out) noexcept
    {
        out = Snapshot{};
        for (int f = 0; f < kFac; ++f)
            for (int s = 0; s < kSex; ++s)
                for (int c = 0; c < kWear; ++c)
                    for (int k = 0; k < kMaxIt; ++k)
                        out.was[Cell(f, s, c, k)] = now[f][s][c][k] ? 1 : 0;
        out.have = true;
    }

    // pmu_build: achievements come from the daemon's post-match reply; gear is
    // the union of both factions' worn sex, faction-first order. `eligible`
    // carries the item-table rules the owner reads (uk != 0, tier <= 1).
    inline void Build(const Snapshot& snap, const Grid& now, const Grid& eligible,
                      const Grid& storyOff, const int sexOf[kFac],
                      const Achievement* ach, int na,
                      int facFirst, uint64_t nowMs, Lists& out) noexcept
    {
        out.builtAt = nowMs;
        out.facFirst = facFirst;
        out.na = 0;
        if (ach) {
            for (int i = 0; i < na && out.na < kMaxA; ++i)
                out.a[out.na++] = ach[i];
        }
        out.ng = out.ngt = 0;
        if (!snap.have) return;
        for (int fi = 0; fi < kFac; ++fi) {
            const int f = fi == 0 ? facFirst : 1 - facFirst;
            const int s = sexOf[f] >= 0 && sexOf[f] < kSex ? sexOf[f] : 0;
            for (int c = 0; c < kWear; ++c)
                for (int k = 0; k < kMaxIt; ++k) {
                    const size_t i = Cell(f, s, c, k);
                    if (!eligible[f][s][c][k] || storyOff[f][s][c][k]
                        || snap.was[i] || !now[f][s][c][k]) continue;
                    ++out.ngt;
                    if (out.ng < kMaxG) {
                        out.g[out.ng++] = Gear{
                            static_cast<uint8_t>(f), static_cast<uint8_t>(s),
                            static_cast<uint8_t>(c), static_cast<uint8_t>(k) };
                    }
                }
        }
    }

    // clan faction 1 WLF / 2 Seraphites -> CUSTOMIZE 0 / 1.
    [[nodiscard]] inline int FacFirst(int clanFac) noexcept
    {
        return clanFac == 2 ? 1 : 0;
    }

    [[nodiscard]] inline bool NeedRebuild(uint64_t now, const Lists& l, int facFirst) noexcept
    {
        return (now > l.builtAt && now - l.builtAt > kRebuildMs)
            || l.facFirst != facFirst;
    }

    // pmu_frame: rows arrive one after another, each fading in over PMU_FADE.
    [[nodiscard]] inline float RowAlpha(uint64_t el, int row) noexcept
    {
        const uint64_t start = static_cast<uint64_t>(row) * kStagMs;
        if (el <= start) return 0.0f;
        const uint64_t d = el - start;
        return d >= kFadeMs ? 1.0f : static_cast<float>(d) / static_cast<float>(kFadeMs);
    }

    [[nodiscard]] inline uint64_t AnimMs() noexcept
    {
        return static_cast<uint64_t>(kRows - 1) * kStagMs + kFadeMs;
    }

    // Text under a full / partly full column (the ext's two foot lines).
    inline void MoreText(int n, char* b, size_t cap) noexcept
    {
        if (!b || !cap) return;
        if (n > kRows)
            std::snprintf(b, cap, "+%d MORE IN CLAN | ACHIEVEMENTS", n - kRows);
        else
            b[0] = '\0';
    }

    inline void GearFootText(int n, char* b, size_t cap) noexcept
    {
        if (!b || !cap) return;
        if (n > kRows)
            std::snprintf(b, cap, "+%d MORE  |  EQUIP THEM IN CUSTOMIZE", n - kRows);
        else
            std::snprintf(b, cap, "%s", "EQUIP THEM IN CUSTOMIZE");
    }

    [[nodiscard]] inline float GearFootY(int shown) noexcept
    {
        const int n = std::clamp(shown, 0, kRows);
        return kY0 + static_cast<float>(n) * kDy + 20.0f + 30.0f;
    }

} // namespace net::ui::pmunlock::core
