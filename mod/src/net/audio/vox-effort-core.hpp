#pragma once

// Pure rules for the ext's female effort-SID feed (t2r-ext.c fem_effort_sid /
// vx_voice_of_body, tools/features/data/vox_lines_female.json). The owner in
// vox-effort.cpp supplies the bodies, their shown look / composed voice, the
// faction, the RNG and the generated line table; no engine, DC or file read
// happens here. Tests: mod/tests/vox_effort_core_test.cpp.
//
// The generated vox_lines_female.h is rebuilt into the guarded
// vox-effort-data.hpp seam from the in-tree JSON, so the table is not a hard
// build dependency. Without it the owner returns 0 and the callers keep the
// engine's default gender take.

#include <cstdint>
#include <cstring>

namespace net::audio::effort::core {

    inline constexpr int kFactionCount = 2;   // 0 Seraphite, 1 WLF
    inline constexpr int kKindCount = 4;      // ext VXE_NKINDS
    inline constexpr int kVoiceCount = 4;     // a..d

    // ext VXE_* from vox_lines.h, in the order of EFFORT_KINDS in vox_lines.py.
    enum class Kind : int { kGive = 0, kPain, kDeath, kDeathKnife };

    // ext FACTION_LOOKS[2][FL_N] + FACTION_LOOKS_WINTER (rows 6 / 7 female).
    inline constexpr const char* const kFactionLooks[kFactionCount][8] = {
        { "scar-male-13", "scar-male-01", "scar-male-12", "scar-male-06",
          "scar-male-09", "scar-male-02", "scar-female-11", "scar-female-09" },
        { "militia-male-11-backpack", "militia-male-08-backpack",
          "militia-male-12-backpack", "militia-male-05-backpack",
          "militia-male-13-backpack", "militia-male-14-backpack",
          "militia-female-13-backpack", "militia-female-12-backpack" }
    };
    inline constexpr const char* const kWinterLooks[kFactionCount][8] = {
        { "scar-winter-13", "scar-winter-01", "scar-winter-12", "scar-winter-06",
          "scar-winter-09", "scar-winter-02", "scar-winter-f11", "scar-winter-f09" },
        { "wlf-winter-01", "wlf-winter-02", "wlf-winter-03", "wlf-winter-05",
          "wlf-winter-13", "wlf-winter-14", "wlf-winter-f13", "wlf-winter-f12" }
    };

    // ext vx_voice_of_look: VX_LOOK_VOICE = {"abcdcaac", "abcdbdbd"}.
    inline constexpr char kLookVoice[kFactionCount][8] = {
        { 'a', 'b', 'c', 'd', 'c', 'a', 'a', 'c' },
        { 'a', 'b', 'c', 'd', 'b', 'd', 'b', 'd' }
    };

    inline int VoiceOfLook(int fac, int look) noexcept
    {
        if ((fac != 0 && fac != 1) || look < 0 || look >= 8)
            return -1;
        return kLookVoice[fac][look] - 'a';
    }

    // The native look string is the authored pool name (or a composed-body
    // name carrying the same suffix). Only rows 6 / 7 answer a female effort.
    inline int LookIndex(const char* look) noexcept
    {
        if (!look || !*look)
            return -1;
        for (int f = 0; f < kFactionCount; ++f)
            for (int i = 0; i < 8; ++i)
                if (!std::strcmp(look, kFactionLooks[f][i])
                    || !std::strcmp(look, kWinterLooks[f][i]))
                    return i;
        // Composed outfits keep the sex in the look name; keep the same two
        // voice letters the authored female rows use.
        if (std::strstr(look, "female-11") || std::strstr(look, "female-13")
            || std::strstr(look, "-f11") || std::strstr(look, "-f13"))
            return 6;
        if (std::strstr(look, "female-09") || std::strstr(look, "female-12")
            || std::strstr(look, "-f09") || std::strstr(look, "-f12"))
            return 7;
        return -1;
    }

    inline bool FemaleLook(const char* look) noexcept
    {
        const int i = LookIndex(look);
        return i >= 6 && i < 8;
    }

    // Generated VXE_LINES / VXE_RANGE in a layout-independent view.
    struct View {
        const uint64_t (*lines)[kVoiceCount] = nullptr;
        int count = 0;
        const uint16_t (*range)[2] = nullptr;   // [fac * 4 + kind][first, count]
    };

    // fem_effort_sid's no-repeat wheel: never one of the last two takes of the
    // same faction / kind, and swap a lone repeat when only two takes exist.
    struct State {
        int16_t last[kFactionCount][kKindCount] = {
            { -1, -1, -1, -1 }, { -1, -1, -1, -1 }
        };
        int16_t last2[kFactionCount][kKindCount] = {
            { -1, -1, -1, -1 }, { -1, -1, -1, -1 }
        };
    };

    inline void Reset(State& s) noexcept
    {
        s = State{};
    }

    inline int Pick(State& s, int fac, int kind, uint32_t roll,
                    int n) noexcept
    {
        if (fac < 0 || fac >= kFactionCount || kind < 0 || kind >= kKindCount
            || n <= 0)
            return -1;
        int i = static_cast<int>(roll % static_cast<uint32_t>(n));
        for (int t = 0; t < n && n > 2
             && (i == s.last[fac][kind] || i == s.last2[fac][kind]); ++t)
            i = (i + 1) % n;
        if (n == 2 && i == s.last[fac][kind])
            i ^= 1;
        s.last2[fac][kind] = s.last[fac][kind];
        s.last[fac][kind] = static_cast<int16_t>(i);
        return i;
    }

    inline uint64_t Sid(const View& v, State& s, int fac, Kind kind,
                        int voice, uint32_t roll) noexcept
    {
        const int k = static_cast<int>(kind);
        if (!v.lines || !v.range || v.count <= 0 || voice < 0
            || voice >= kVoiceCount || fac < 0 || fac >= kFactionCount
            || k < 0 || k >= kKindCount)
            return 0;
        const uint16_t first = v.range[fac * kKindCount + k][0];
        const uint16_t n = v.range[fac * kKindCount + k][1];
        if (n <= 0 || first >= v.count || first + n > v.count)
            return 0;
        const int i = Pick(s, fac, k, roll, n);
        if (i < 0)
            return 0;
        return v.lines[first + i][voice];
    }

} // namespace net::audio::effort::core
