// ported from tools/ext/exec_music.c

#include "exec-music.hpp"

#include "music-hold.hpp"

#include <cstdio>

namespace net::audio::exec_music {

    namespace {
        Sink*    g_sink       = nullptr;
        uint64_t g_pendingAt  = 0;
        uint64_t g_t0         = 0;
        int      g_pick       = 0;
        double   g_seek       = 0.0;
        bool     g_pendingCut = false;
        bool     g_playing    = false;
        bool     g_fading     = false;

        template<typename... A>
        void Log(const char* fmt, A... args)
        {
            if (!g_sink)
                return;
            char buf[224];
            std::snprintf(buf, sizeof buf, fmt, args...);
            g_sink->Log(buf);
        }

        void Play(uint64_t nowMs)
        {
            if (g_pendingCut && g_sink)
                g_sink->MusicCut();
            // The hold runs from the cue's first frame, through its cap and the resume.
            music_hold::Hold(nowMs + kCapMs + kResumeMs);
            g_playing = true;
            g_fading  = false;
            g_t0      = nowMs;
            if (g_sink)
                g_sink->StartCue(g_pick, g_seek, kLevel);
            Log("exec music: victim cue %d starts (seek %.2f s)", g_pick, g_seek);
        }
    }

    void SetSink(Sink* sink) noexcept { g_sink = sink; }
    Sink* SinkInstance() noexcept { return g_sink; }

    void Start(uint64_t nowMs, float killS, int pick)
    {
        if (pick < 0 || pick >= kCues)
            pick = 0;
        const double land = static_cast<double>(killS) + kDeathDelayMs / 1000.0;
        const double lead = land - kHit[pick];
        g_pick       = pick;
        g_pendingAt  = nowMs + (lead > 0.0 ? static_cast<uint64_t>(lead * 1000.0) : 0ULL);
        g_seek       = lead < 0.0 ? -lead : 0.0;
        // Decided now: the hold set below would block the cue's own cut at play time.
        g_pendingCut = !music_hold::BlocksDown(nowMs);
        music_hold::Hold(g_pendingAt + kCapMs + kResumeMs);
        Log("exec music: cue %d hit %.2f s on the death screen: starts in %.2f s (kill %.2f s)%s",
            pick, kHit[pick], lead > 0.0 ? lead : 0.0, static_cast<double>(killS),
            lead < 0.0 ? " - joined mid-cue" : "");
    }

    void Tick(uint64_t nowMs, bool inPlayState)
    {
        if (g_pendingAt && nowMs >= g_pendingAt && inPlayState) {
            g_pendingAt = 0;
            Play(nowMs);
        }
        if (!g_playing)
            return;
        if (!g_sink || !g_sink->CueLive()) {
            g_playing = false;
            music_hold::Hold(nowMs + kResumeMs);
            return;
        }
        if (nowMs - g_t0 >= kCapMs)
            Fade(nowMs, "cap reached");
        else if (!inPlayState)
            Fade(nowMs, "match left the playing state");
    }

    void Fade(uint64_t nowMs, const char* why)
    {
        g_pendingAt = 0;
        if (!g_playing || g_fading)
            return;
        music_hold::Hold(nowMs + kFadeMs + kResumeMs);
        g_fading = true;
        if (g_sink)
            g_sink->CutCue(static_cast<int>(kFadeMs));
        Log("exec music: %s - fade-out %llu ms in", why ? why : "?",
            static_cast<unsigned long long>(nowMs - g_t0));
    }

    bool     Pending(uint64_t nowMs) noexcept { return g_pendingAt != 0 && nowMs >= g_pendingAt; }
    bool     Playing() noexcept { return g_playing; }
    bool     Fading() noexcept { return g_fading; }
    uint64_t PendingAt() noexcept { return g_pendingAt; }
    uint64_t StartedAt() noexcept { return g_t0; }

    void Reset()
    {
        g_pendingAt = g_t0 = 0;
        g_pick = 0;
        g_seek = 0.0;
        g_pendingCut = g_playing = g_fading = false;
    }

} // namespace net::audio::exec_music
