#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Pure core of tools/ext/kicknote.c (plus the build-201 packet-loss branch from
// origin/port-201). No game headers, no Windows: shared by the native page owner
// and the standalone harness in mod/tests/kick_note_core_test.cpp.
//
// The daemon's kick text arrives already localised (EN / PL). It is parsed into a
// canonical EN message and a title id; engine::locale::Tr then translates the
// canonical pieces to the active language, exactly as the ext's loc layer did.
namespace net::ui::kick_note::core {

    constexpr uint64_t kLockMs       = 800;
    constexpr uint64_t kFadeMs       = 250;
    constexpr uint64_t kRetryMs      = 1500;
    constexpr int      kMaxPushes    = 3;
    constexpr uint64_t kDedupeMs     = 120000;
    constexpr uint64_t kAutoFindMs   = 3000;
    constexpr int      kRefillFrames = 10;

    enum class State : int { None = 0, Due = 1, Pushed = 2, Up = 3 };
    enum class Title : int { Kicked, Ping, PacketLoss };

    struct Parsed {
        char   msg[400]{};
        Title  title = Title::Kicked;
    };

    inline bool Contains(const char* text, const char* needle) noexcept
    {
        return text && needle && std::strstr(text, needle) != nullptr;
    }

    // The ext's MsgIsMove: a team move keeps its toast, it is not a kick box.
    inline bool IsMove(const char* t) noexcept
    {
        return Contains(t, "TEAMS: ") || Contains(t, "DRU\xC5\xBBYNY: ");
    }

    inline int Digits(const char* s, int count, int* out) noexcept
    {
        int n = 0;
        for (const char* p = s; *p && n < count; ++p) {
            if (*p < '0' || *p > '9') continue;
            int v = 0;
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (*p - '0');
                ++p;
            }
            out[n++] = v;
            --p;
        }
        return n;
    }

    inline void Reason(const char* t, char* out, size_t cap) noexcept
    {
        out[0] = '\0';
        if (!t) return;
        const char* q = std::strstr(t, "Reason: \"");
        if (!q) q = std::strstr(t, "Pow\xC3\xB3" "d: \"");
        if (!q) return;
        q = std::strchr(q, '"');
        if (!q) return;
        ++q;
        const char* e = std::strrchr(q, '"');
        if (!e || e <= q) return;
        size_t k = 0;
        for (const char* p = q; *p && p != e && k + 1 < cap; ++p)
            if (*p != '<' && *p != '>') out[k++] = *p;
        out[k] = '\0';
    }

    inline Parsed Parse(const char* t) noexcept
    {
        Parsed r;
        if (!t) return r;
        char reason[200];
        Reason(t, reason, sizeof reason);

        // Packet-loss removals first: they have no admin Reason and no "ping".
        const char* ls = nullptr;
        if (Contains(t, "of its packets")) ls = std::strstr(t, "lost ");
        else if (Contains(t, "pakiet"))     ls = std::strstr(t, "gubi");
        if (ls) {
            int v[3]{};
            const int n = Digits(ls, 3, v);
            const bool nowOk = Contains(t, "search again now") || Contains(t, "od razu");
            r.title = Title::PacketLoss;
            if (n == 3 && nowOk)
                std::snprintf(r.msg, sizeof r.msg,
                    "YOUR CONNECTION LOST %d PERCENT OF ITS PACKETS FOR OVER %d SECONDS (LIMIT %d PERCENT). YOU CAN SEARCH AGAIN NOW.",
                    v[0], v[1], v[2]);
            else if (n == 3)
                std::snprintf(r.msg, sizeof r.msg,
                    "YOUR CONNECTION LOST %d PERCENT OF ITS PACKETS FOR OVER %d SECONDS (LIMIT %d PERCENT), THE SECOND TIME IN A SHORT WHILE.",
                    v[0], v[1], v[2]);
            else
                std::snprintf(r.msg, sizeof r.msg, "YOUR CONNECTION LOST TOO MANY PACKETS.");
            return r;
        }

        if (Contains(t, "ping")) {
            int v[3]{};
            const int n = Digits(std::strstr(t, "ping"), 3, v);
            r.title = Title::Ping;
            if (n == 3)
                std::snprintf(r.msg, sizeof r.msg,
                    "YOUR PING TO THE HOST WAS <font color=\"#ECE9E2\">%d MS</font> FOR OVER %d SECONDS. THE LIMIT IS %d MS.",
                    v[0], v[1], v[2]);
            else
                std::snprintf(r.msg, sizeof r.msg, "YOUR PING TO THE HOST WAS TOO HIGH.");
            return r;
        }

        r.title = Title::Kicked;
        int secs = 0;
        const char* a = std::strstr(t, "no input for ");
        if (a) secs = Digits(a + 13, 1, &secs) ? secs : 0;
        else if ((a = std::strstr(t, "przez ")) && Contains(t, "(AFK)"))
            secs = Digits(a + 6, 1, &secs) ? secs : 0;
        if (secs > 0)
            std::snprintf(r.msg, sizeof r.msg, "NO INPUT FOR %d SECONDS (AFK).", secs);
        else if (reason[0])
            std::snprintf(r.msg, sizeof r.msg,
                "AN ADMIN REMOVED YOU FROM THE MATCH. REASON: <font color=\"#ECE9E2\">%s</font>", reason);
        else
            std::snprintf(r.msg, sizeof r.msg, "AN ADMIN REMOVED YOU FROM THE MATCH.");
        return r;
    }

    inline bool Duplicate(const char* stored, const char* text, uint64_t setAt, uint64_t now) noexcept
    {
        return stored && text && setAt && now >= setAt
            && now - setAt < kDedupeMs && std::strcmp(stored, text) == 0;
    }

    // ext KnTick's push gate. Returns true when a new PushPage is due.
    inline bool ShouldPush(State st, uint64_t now, uint64_t pushedAt, int pushes) noexcept
    {
        if (st != State::Due && st != State::Pushed) return false;
        if (st == State::Pushed) {
            if (now <= pushedAt || now - pushedAt <= kRetryMs) return false;
            if (pushes >= kMaxPushes) return false;
        }
        return true;
    }

    inline bool DropPushes(State st, uint64_t now, uint64_t pushedAt, int pushes) noexcept
    {
        return st == State::Pushed && pushedAt && now > pushedAt
            && now - pushedAt > kRetryMs && pushes >= kMaxPushes;
    }

    inline float Ramp(uint64_t now, uint64_t t0, uint64_t ms) noexcept
    {
        if (!t0 || now <= t0) return 0.0f;
        const float f = static_cast<float>(now - t0) / static_cast<float>(ms);
        return f < 1.0f ? f : 1.0f;
    }

} // namespace net::ui::kick_note::core
