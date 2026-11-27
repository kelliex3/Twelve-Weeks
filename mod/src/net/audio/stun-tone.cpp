// ported from tools/ext/alert_sting.c (stun tone: stuntone_build / stuntone_burst /
// stuntone_stunned / stuntone_tick). All rules live in stun-tone-core.hpp; the
// engine / audio / net seams stay here.

#include "stun-tone.hpp"

#include "stun-tone-pcm.hpp"

#include "boot.hpp"
#include "engine/object.hpp"
#include "engine/pcm.hpp"
#include "gameplay/stun-lock.hpp"
#include "log.hpp"
#include "net/game/player-status.hpp"

#include <windows.h>

#include <cstdint>
#include <vector>

namespace net::audio::stun_tone {

    namespace {

        namespace core = net::audio::stun_tone::core;
        namespace sink = net::audio::stun_tone::pcm;

        core::State g_state;
        engine::pcm::Voice g_left, g_right;
        std::vector<core::Sample> g_bedL, g_bedR;
        bool g_built = false;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }

        bool Live() noexcept
        {
            return engine::pcm::Live(g_left) && engine::pcm::Live(g_right);
        }

        bool EnsureBed()
        {
            if constexpr (!sink::kHave)
                return false;                    // no private names exist in this build
            if (g_built)
                return true;
            core::BuildBed(g_bedL, g_bedR,
                           sink::kImpact, sink::kImpactFrames,
                           sink::kRing, sink::kRingFrames,
                           sink::kImpactRate);
            g_built = !g_bedL.empty();
            if (!g_built)
                TO1_LOG("[STUNTONE] bed not built (empty private wave table)\n");
            return g_built;
        }

        bool StartVoice()
        {
            if constexpr (!sink::kHave)
                return false;
            if (!EnsureBed())
                return false;
            // PlayStereoRaw takes an explicit gain, unlike alertsting's Play: apply
            // the ext's 0.8 * sfx_user_volume() ourselves.
            const float gain = 0.8f * engine::pcm::SfxUserVolume();
            return sink::Play(g_bedL, g_bedR, static_cast<int>(g_bedL.size()),
                              gain, sink::kImpactRate, g_left, g_right);
        }

        void CutBoth(int ms) noexcept
        {
            engine::pcm::Cut(g_left, ms);
            engine::pcm::Cut(g_right, ms);
            engine::pcm::Invalidate(g_left);
            engine::pcm::Invalidate(g_right);
        }

    } // namespace

    void Burst(int slot, const float at[3], float radius)
    {
        (void)slot;                                // log-only in debug builds
        if (ExtOwns() || !at)
            return;
        // ext: match_substate()==6, !g_downed, !g_down_dying, !g_uc4_dying_until,
        // local_hp > 0. The mod's dying latch is LocalDownState()==2; the separate
        // uc4 dying timer has no producer yet, so it is covered by the down state.
        if (!net::player_status::MatchLive()
            || net::player_status::LocalDowned()
            || net::player_status::LocalDownState() == 2
            || !net::player_status::LocalAlive())
            return;

        float w[8] = {};
        if (!engine::object::LocalPlayerWorld(w))
            return;
        const float local[3] = { w[0], w[1], w[2] };
        const float d = core::BurstDistance(at, local);
        (void)d;                                   // log-only in debug builds
        if (!core::BurstInRange(at, local, radius))
            return;

        const uint64_t now = GetTickCount64();
        if (core::VoiceUsable(g_state, now, Live()))
            return;                              // the ext never restarts within 1500 ms

        // No private data: no voice, no t0 / confirmation state, no playback log.
        if constexpr (!sink::kHave)
            return;

        if (!StartVoice()) {
            TO1_LOG("[STUNTONE] slot %d burst (%.2f m, radius %.1f) - "
                    "mixer off / voices busy\n", slot, static_cast<double>(d),
                    static_cast<double>(radius));
            return;
        }
        core::Start(g_state, now, false);
        TO1_LOG("[STUNTONE] slot %d burst (%.2f m, radius %.1f) -> playing "
                "(stun not judged yet)\n", slot, static_cast<double>(d),
                static_cast<double>(radius));
    }

    void Stunned()
    {
        if (ExtOwns())
            return;
        const uint64_t now = GetTickCount64();
        if (core::CanConfirm(g_state, now, Live())) {
            core::Confirm(g_state);              // no new voice (ext confirmed return)
            TO1_LOG("[STUNTONE] confirmed %llu ms after the burst\n",
                    static_cast<unsigned long long>(now - g_state.t0));
            return;
        }

        if constexpr (!sink::kHave)
            return;

        if (!StartVoice()) {
            TO1_LOG("[STUNTONE] no burst seen locally - mixer off / voices busy\n");
            return;
        }
        core::Start(g_state, now, true);
        TO1_LOG("[STUNTONE] no burst seen locally -> playing (confirmed)\n");
    }

    void Tick()
    {
        if (ExtOwns() || !g_state.started)
            return;
        const uint64_t now = GetTickCount64();
        // Ext stun_fx_live covers the lock or the Launcher stun fx. Only the lock
        // is ported in the mod (gameplay/stun-lock.*), so it is the whole answer.
        const bool stunFx = gameplay::stun_lock::Active(now);
        const core::Fade fade = core::Tick(g_state, now, Live(),
                                           net::player_status::MatchLive(), stunFx);
        switch (fade) {
        case core::Fade::kNotStunned:
            CutBoth(core::kFadeNotStunnedMs);
            TO1_LOG("[STUNTONE] faded - the burst did not stun us\n");
            break;
        case core::Fade::kNotLive:
            CutBoth(core::kFadeNotLiveMs);
            break;
        case core::Fade::kStunOver:
            CutBoth(core::kFadeStunOverMs);
            TO1_LOG("[STUNTONE] faded - the stun is over\n");
            break;
        case core::Fade::kInvalidate:
            engine::pcm::Invalidate(g_left);
            engine::pcm::Invalidate(g_right);
            break;
        case core::Fade::kNone:
            break;
        }
    }

    void Reset()
    {
        core::Reset(g_state);
        engine::pcm::Invalidate(g_left);
        engine::pcm::Invalidate(g_right);
        // g_bedL / g_bedR and g_built are immutable private-data products; keep them.
    }

    bool Active() noexcept
    {
        return !ExtOwns() && g_state.started && Live();
    }

    uint64_t StartedAt() noexcept
    {
        return g_state.started ? g_state.t0 : 0;
    }

} // namespace net::audio::stun_tone
