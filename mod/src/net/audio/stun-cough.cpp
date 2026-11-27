// ported from tools/ext/alert_sting.c (stun cough: sc_pick / sc_muffled /
// stuncough_tick). All rules live in stun-cough-core.hpp; the engine / audio / net
// seams stay here.

#include "stun-cough.hpp"

#include "stun-cough-pcm.hpp"

#include "boot.hpp"
#include "engine/object.hpp"
#include "engine/pcm.hpp"
#include "gameplay/stun-lock.hpp"
#include "log.hpp"
#include "net/game/player-status.hpp"
#include "net/protocol/net-protocol.hpp"   // kMaxNetPlayers

#include <windows.h>

#include <cstdint>
#include <vector>

namespace net::audio::stun_cough {

    namespace {

        namespace core = net::audio::stun_cough::core;
        namespace sink = net::audio::stun_cough::pcm;

        core::State g_state;
        std::vector<core::Sample> g_muffled;
        bool g_built = false;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }

        bool EnsureMuffled()
        {
            if constexpr (!sink::kHave)
                return false;                    // no private names exist in this build
            if (g_built)
                return true;
            core::BuildMuffled(g_muffled, sink::Blob(),
                               sink::Descriptors().data(), sink::kCount);
            g_built = !g_muffled.empty();
            if (!g_built)
                TO1_LOG("[STUNCOUGH] muffled bank not built (empty private wave table)\n");
            return g_built;
        }

        bool WaveOk(int wave) noexcept
        {
            return wave >= 0 && wave < sink::kCount;
        }

    } // namespace

    void NoteOwnStun(uint64_t startMs)
    {
        if (ExtOwns())
            return;
        core::NoteOwnStun(g_state, startMs);
    }

    void ClearOwnStun()
    {
        if (ExtOwns())
            return;
        core::ClearOwnStun(g_state);
    }

    void SetOwnFemale(bool female)
    {
        if (ExtOwns())
            return;
        core::SetOwnFemale(g_state, female);
    }

    void NotePeerStun(int slot, uint64_t stunAtMs, bool female, uint64_t handle,
                      const float wpos[3])
    {
        if (ExtOwns())
            return;
        core::NotePeerStun(g_state, slot, stunAtMs, female, handle, wpos);
    }

    void ClearPeer(int slot)
    {
        if (ExtOwns())
            return;
        core::ClearPeer(g_state, slot);
    }

    void Tick()
    {
        if (ExtOwns())
            return;
        // No private data: no voices, no cadence / RNG / freshness consumption and
        // no playback logs. The feed APIs above remain live.
        if constexpr (!sink::kHave)
            return;
        if (!EnsureMuffled())
            return;

        const uint64_t now = GetTickCount64();
        // If the lock module is producing, it is authoritative (the ext read
        // g_stun_until here). An explicit NoteOwnStun still works before it lands.
        const uint64_t until = gameplay::stun_lock::Until();
        if (until)
            core::NoteOwnStunUntil(g_state, until);

        const bool matchLive = net::player_status::MatchLive();
        // ext's !downed && !down_dying && !uc4_dying && !psync_active() && hp > 0.
        // The dying latch has no separate producer yet: LocalDownState()==2 covers it.
        const bool localOk = !net::player_status::LocalDowned()
                          && net::player_status::LocalDownState() != 2
                          && !net::player_status::LocalInPairedMove()
                          && net::player_status::LocalAlive();

        bool peerOk[core::kCoreMaxPeers] = {};
        for (int i = 0; i < kMaxNetPlayers && i < core::kCoreMaxPeers; ++i)
            peerOk[i] = net::player_status::PuppetAlive(i)
                     && !net::player_status::PuppetDowned(i);

        const core::Result r = core::Tick(g_state, now, matchLive, localOk,
                                          peerOk, kMaxNetPlayers,
                                          sink::Descriptors().data(), sink::kCount);
        if (r.own && WaveOk(r.ownWave)) {
            const core::WaveDesc& w = sink::Descriptors()[r.ownWave];
            engine::pcm::Voice v = engine::pcm::Play(g_muffled.data() + w.off, w.n,
                                                     0.9f, 0.9f, sink::Rate());
            TO1_LOG("[STUNCOUGH] ours (muffled) %s -> %s\n", sink::Name(r.ownWave),
                    engine::pcm::Live(v) ? "playing" : "mixer off / voices busy");
            (void)v;                               // log-only in debug builds
        }
        for (int i = 0; i < r.peerCount; ++i) {
            const core::PeerCue& cue = r.peers[i];
            if (!WaveOk(cue.wave))
                continue;
            const core::WaveDesc& w = sink::Descriptors()[cue.wave];
            float fromHandle[8] = {};
            const float* wpos = cue.hasPos ? cue.pos : nullptr;
            if (!wpos && cue.handle
                && engine::object::EntityWorld(static_cast<StringId64>(cue.handle), fromHandle))
                wpos = fromHandle;
            // ext set g_mix_rnear = 2.5 / g_mix_rfar = 45 around pcm_play_at;
            // PlayAtRange carries the same pair on the voice itself.
            engine::pcm::Voice v = engine::pcm::PlayAtRange(sink::Blob() + w.off, w.n,
                                                            1.4f, sink::Rate(), wpos,
                                                            2.5f, 45.0f);
            TO1_LOG("[STUNCOUGH] slot %d's puppet %s -> %s\n", cue.slot,
                    sink::Name(cue.wave),
                    engine::pcm::Live(v) ? "playing" : "mixer off / voices busy");
            (void)v;                               // log-only in debug builds
            (void)cue.fresh;
        }
    }

    void Reset()
    {
        core::Reset(g_state);
        // g_muffled / g_built are immutable private-data products; keep them.
    }

    bool Active() noexcept
    {
        if (ExtOwns())
            return false;
        const uint64_t now = GetTickCount64();
        if (g_state.ownUntil && now < g_state.ownUntil)
            return true;
        for (int i = 0; i < core::kCoreMaxPeers; ++i)
            if (g_state.peers[i].at && now >= g_state.peers[i].at
                && now - g_state.peers[i].at < core::kStunMs)
                return true;
        return false;
    }

} // namespace net::audio::stun_cough
