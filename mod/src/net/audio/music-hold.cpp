// ported from tools/ext/t2r-ext.c (MusicSid / MusicCurrent / MusicPlay and the g_ms* hold state)

#include "music-hold.hpp"

#include <cstddef>
#include <cstdio>

namespace net::audio::music_hold {

    namespace {
        Sink*    g_sink           = nullptr;
        uint64_t g_holdUntil      = 0;
        uint64_t g_resumeAt       = 0;
        uint64_t g_nowatchUntil   = 0;
        bool     g_dead           = false;

        template<typename... A>
        void Log(const char* fmt, A... args)
        {
            if (!g_sink)
                return;
            char buf[192];
            std::snprintf(buf, sizeof buf, fmt, args...);
            g_sink->Log(buf);
        }
    }

    uint64_t MusicSid(const char* name) noexcept
    {
        if (!name)
            return Fnv64("**");
        std::size_t n = 0;
        while (name[n])
            ++n;
        char b[192];
        if (n + 2 > sizeof b / sizeof b[0])
            return 0;
        b[0] = '*';
        for (size_t i = 0; i < n; ++i)
            b[i + 1] = name[i];
        b[n + 1] = '*';
        b[n + 2] = '\0';
        return Fnv64(b);
    }

    void SetSink(Sink* sink) noexcept { g_sink = sink; }
    Sink* SinkInstance() noexcept { return g_sink; }

    uint64_t Play(const char* name, const char* why)
    {
        const uint64_t sid = MusicSid(name);
        if (g_sink)
            (void)g_sink->PlayMusic(sid);
        Log("music: play %s [%s]", name ? name : "?", why ? why : "?");
        return sid;
    }

    uint64_t Current() noexcept
    {
        if (!g_sink)
            return kNoPlaylist;
        const uint64_t r = g_sink->CurrentMusic();
        return r == static_cast<uint64_t>(-1) ? kNoPlaylist : r;
    }

    void Hold(uint64_t until) noexcept
    {
        g_holdUntil = until;
        g_resumeAt  = until;
    }

    void HoldUntil(uint64_t until) noexcept { g_holdUntil = until; }
    void ResumeAt(uint64_t at) noexcept { g_resumeAt = at; }
    void NowatchUntil(uint64_t until) noexcept { g_nowatchUntil = until; }

    bool BlocksDown(uint64_t nowMs) noexcept { return nowMs < g_holdUntil; }
    bool BlocksWatch(uint64_t nowMs) noexcept { return g_dead || nowMs < g_nowatchUntil; }
    bool ResumeDue(uint64_t nowMs) noexcept { return g_resumeAt != 0 && nowMs >= g_resumeAt; }

    uint64_t TakeResume(uint64_t nowMs) noexcept
    {
        if (!ResumeDue(nowMs))
            return 0;
        const uint64_t at = g_resumeAt;
        g_resumeAt = 0;
        return at;
    }

    void SetDead(bool dead) noexcept { g_dead = dead; }
    bool Dead() noexcept { return g_dead; }

    uint64_t HoldUntil() noexcept { return g_holdUntil; }
    uint64_t ResumeAt() noexcept { return g_resumeAt; }
    uint64_t NowatchUntil() noexcept { return g_nowatchUntil; }

    void Reset()
    {
        g_holdUntil = g_resumeAt = g_nowatchUntil = 0;
        g_dead = false;
    }

} // namespace net::audio::music_hold
