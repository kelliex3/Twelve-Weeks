// Native owner of tools/ext/pmsfx.c. Cue / one-shot / count-duration rules live
// in pmsfx-core.hpp; the game-derived waves live behind pmsfx-data.hpp. The
// page edges come from the native post-match screen, so no new wire message and
// no ext call site is touched.

#include "pmsfx.hpp"

#include "pmsfx-data.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "engine/pcm.hpp"
#include "log.hpp"

#include <cstdint>

namespace net::audio::pmsfx {

    namespace {

        namespace core = net::audio::pmsfx::core;
        namespace data = net::audio::pmsfx::data;

        constexpr char kOff[] = "t2r-no-postmatch-sfx.txt";

        core::State         g_state;
        engine::pcm::Voice  g_voice[static_cast<int>(core::Kind::kCountKinds)];
        engine::pcm::Voice  g_count;
        bool                g_gapped = false;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }
        bool Off()             { return engine::flag::Present(kOff, 120); }

        void Gap(const char* what)
        {
            if (g_gapped)
                return;
            g_gapped = true;
            TO1_LOG("[PMSFX] %s (native gap)\n", what);
        }

        const char* Name(core::Kind kind) noexcept
        {
            switch (kind) {
            case core::Kind::kEnter: return "enter";
            case core::Kind::kCount: return "count";
            case core::Kind::kEnd:   return "end";
            case core::Kind::kStamp: return "stamp";
            case core::Kind::kPage:  return "page";
            default:                 return "?";
            }
        }

        void Play(core::Kind kind)
        {
            if (Off() || !data::kHave) {
                Gap("postmatch waves not built (TO1_PMSFX_PCM off)");
                return;
            }
            const int i = static_cast<int>(kind);
            if (i < 0 || i >= static_cast<int>(core::Kind::kCountKinds))
                return;
            if (!data::Play(kind, g_voice[i])) {
                Gap("mixer refused a post-match cue");
                return;
            }
            TO1_LOG("[PMSFX] cue %d (%s) -> playing\n", i, Name(kind));
        }

        void StartCount(int ms)
        {
            if (Off() || !data::kHave) {
                Gap("postmatch waves not built (TO1_PMSFX_PCM off)");
                return;
            }
            engine::pcm::Cut(g_count, 1);      // ext pms_count_cut(1) before a new loop
            if (!data::CountStart(ms, g_count)) {
                Gap("count loop source / buffer not ready");
                return;
            }
            TO1_LOG("[PMSFX] count loop %d ms -> playing\n", ms);
        }

        void Handle(const core::Result& r)
        {
            for (int i = 0; i < r.count; ++i) {
                const core::Cue& cue = r.cues[i];
                switch (cue.kind) {
                case core::CueKind::kPlay:
                    Play(static_cast<core::Kind>(cue.cue));
                    break;
                case core::CueKind::kCountStart:
                    StartCount(cue.ms);
                    break;
                case core::CueKind::kCountCut:
                    engine::pcm::Cut(g_count, cue.ms > 0 ? cue.ms : 1);
                    TO1_LOG("[PMSFX] count cut (%d ms)\n", cue.ms);
                    break;
                default:
                    break;
                }
            }
        }

    } // namespace

    void OnEnter(uint64_t now)
    {
        if (ExtOwns())
            return;
        core::Result r;
        core::Enter(g_state, r);
        Handle(r);
        (void)now;
    }

    void OnSkip(uint64_t)
    {
        if (ExtOwns())
            return;
        core::Result r;
        core::Skip(g_state, r);
        Handle(r);
    }

    void OnTick(uint64_t, bool ready, int64_t elapsedMs, int countDurMs)
    {
        if (ExtOwns())
            return;
        core::Result r;
        core::Tick(g_state, r, ready, elapsedMs, countDurMs);
        Handle(r);
    }

    void OnFinish(uint64_t)
    {
        if (ExtOwns())
            return;
        core::Result r;
        core::Finish(g_state, r);
        Handle(r);
    }

    void OnPage(uint64_t)
    {
        if (ExtOwns())
            return;
        core::Result r;
        core::Page(r);
        Handle(r);
    }

    void Reset()
    {
        core::Reset(g_state);
        engine::pcm::Cut(g_count, 0);
        for (engine::pcm::Voice& v : g_voice)
            engine::pcm::Cut(v, 0);
        g_gapped = false;
    }

    bool Active() noexcept
    {
        return !ExtOwns() && g_state.armed;
    }

} // namespace net::audio::pmsfx
