#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Pure rules of build-191 tools/ext/namerim.c: the lead-developer pink name
// outline on the MENU name widgets. The native owner (name-rim.cpp) supplies
// the widget / role / page seams; nothing here touches the engine.

namespace net::social::name_rim::core {

    inline constexpr int    kRows = 8;
    inline constexpr int    kRimN = 8;
    inline constexpr float  kRimR = 1.0f;      // rank_badge_widgets.RIM_R
    inline constexpr uint64_t kTickMs = 150;
    inline constexpr int    kResolveFrames = 30;
    inline constexpr int    kLeadDevRole = 4;  // tools/roles.py

    inline constexpr float kDx[kRimN] = { 1.0f, 0.7071f, 0.0f, -0.7071f,
                                         -1.0f, -0.7071f, 0.0f, 0.7071f };
    inline constexpr float kDy[kRimN] = { 0.0f, 0.7071f, 1.0f, 0.7071f,
                                          0.0f, -0.7071f, -1.0f, -0.7071f };

    // nrim_strip: the name without <font ...> / <scale> markup (the rim's tint
    // is its own colour, so the name's markup must not be copied into it).
    inline void Strip(char* out, size_t cap, const char* text) noexcept
    {
        if (!out || cap == 0) return;
        size_t k = 0;
        for (const char* p = text ? text : ""; *p && k + 1 < cap; ++p) {
            if (*p == '<') {
                const char* e = std::strchr(p, '>');
                if (e) { p = e; continue; }
            }
            out[k++] = *p;
        }
        out[k] = 0;
    }

    inline bool LobbyNamePath(char* out, size_t cap, int row) noexcept
    {
        if (!out || row < 0 || row >= kRows) return false;
        return std::snprintf(out, cap,
            "anim-group.lobbyPanel.lobbyList.Player%d.nameLabel", row)
            < static_cast<int>(cap);
    }

    inline bool LobbyRimPath(char* out, size_t cap, int row) noexcept
    {
        if (!out || row < 0 || row >= kRows) return false;
        return std::snprintf(out, cap,
            "anim-group.lobbyPanel.lobbyList.Player%d.nmRim%%d", row)
            < static_cast<int>(cap);
    }

    inline bool ScoreboardRimPath(char* out, size_t cap, int slot) noexcept
    {
        if (!out || slot < 0 || slot >= kRows) return false;
        return std::snprintf(out, cap,
            "anim-group.scoreboard.team%d_row%d_nmRim%%d", slot / 4, slot % 4)
            < static_cast<int>(cap);
    }

    inline char AsciiLower(char c) noexcept
    {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    inline bool EqualsNoCase(const char* a, const char* b) noexcept
    {
        if (!a || !b) return false;
        while (*a && *b) {
            if (AsciiLower(*a) != AsciiLower(*b)) return false;
            ++a; ++b;
        }
        return *a == 0 && *b == 0;
    }

    // mfnr_is_me: exactly our name, or a "[TAG] name" / "WLF name" prefix.
    inline bool IsMe(const char* rowName, const char* local) noexcept
    {
        if (!rowName || !rowName[0] || !local || !local[0]) return false;
        const size_t a = std::strlen(rowName), b = std::strlen(local);
        if (a == b) return EqualsNoCase(rowName, local);
        if (a <= b + 1) return false;
        const char sep = rowName[a - b - 1];
        return (sep == ' ' || sep == ']') && EqualsNoCase(rowName + (a - b), local);
    }

    // ext rkb_placeholder / namerim's scoreboard gate. The mod's shared
    // friends-state placeholder covers the common cases; these are the exact
    // extra strings namerim checks on its own.
    inline bool IsPlaceholder(const char* name) noexcept
    {
        if (!name || !name[0]) return true;
        if (!std::strcmp(name, "-")) return true;
        if (!std::strncmp(name, "PLAYER", 6)) return true;
        if (std::strstr(name, "OPEN SEAT") || std::strstr(name, "WOLNE MIEJSCE"))
            return true;
        return !std::strcmp(name, "SEARCHING");
    }

    inline bool IsScoreboardPlaceholder(const char* name) noexcept
    {
        return !name || !name[0] || !std::strncmp(name, "PLAYER ", 7)
            || !std::strcmp(name, "-");
    }

    // ext namerim.c:128 / :138. Row 0 is us (own role from the clan state);
    // every other row uses the shared 0xBF table. Placeholders have no role.
    inline int LobbyRole(bool me, const char* shown, int ownRole,
                         int otherRole) noexcept
    {
        if (!shown || !shown[0] || IsPlaceholder(shown)) return 0;
        return me ? ownRole : otherRole;
    }

    inline int ScoreboardRole(const char* name, const char* local,
                              int ownRole, int otherRole) noexcept
    {
        if (IsScoreboardPlaceholder(name)) return 0;
        return IsMe(name, local) ? ownRole : otherRole;
    }

    [[nodiscard]] inline bool ShouldShow(bool off, int role,
                                         const char* stripped) noexcept
    {
        if (off || role != kLeadDevRole) return false;
        return stripped && stripped[0] != 0;
    }

    [[nodiscard]] inline bool ShouldTick(uint64_t now, uint64_t next) noexcept
    {
        return now >= next;
    }

    [[nodiscard]] inline bool ShouldResolve(uint64_t frame,
                                            uint64_t lastResolve) noexcept
    {
        return frame > lastResolve
            ? frame - lastResolve > static_cast<uint64_t>(kResolveFrames)
            : true;
    }

    // A new page generation or a rebuilt name widget starts with fresh,
    // hidden rims; the owner then rewrites all eight.
    struct RimState {
        uint64_t gen = 0;
        uint64_t nameW = 0;
        int      on = -1;
        char     txt[64]{};
        float    px = -1e9f, py = -1e9f, sx = -1e9f, sy = -1e9f;
    };

    inline void ResetState(RimState& s, uint64_t gen, uint64_t nameW) noexcept
    {
        s = RimState{};
        s.gen = gen;
        s.nameW = nameW;
        s.on = -1;
    }

    [[nodiscard]] inline bool NeedRewrite(const RimState& s, bool on,
                                          const char* stripped) noexcept
    {
        if (s.on < 0) return true;
        if (on == (s.on != 0) && (!on || !std::strcmp(s.txt, stripped ? stripped : "")))
            return false;
        return true;
    }

    // The offsets follow the name's own pivot and scale.
    inline void Place(float px, float py, float sx, float sy, int i,
                      float& outX, float& outY) noexcept
    {
        outX = px + kDx[i] * kRimR;
        outY = py + kDy[i] * kRimR;
        (void)sx;
        (void)sy;
    }

} // namespace net::social::name_rim::core
