#pragma once

// Native sole owner of tools/ext/explus.c: the layered execution foley timeline
// on the paired-move clock, plus the shared recipes used by contextual
// takedowns, standing finishers, downed melee and interrogation foley. All row
// / mixer / gain / voice / recipe rules live in explus-core.hpp; the engine
// audio, speech, sidecar, private-data and paired-move seams stay here.
//
// Who calls it: xcam-bridge.cpp reports once per paired move through
// OnPairedStart(); boot::FrameTick() calls Tick() every frame;
// net-replication.cpp calls Reset() on a level / session reset. The recipe
// feeds are inert until a native producer exists (ctxdown / finishers / downed
// melee are not ported). intfoley.cpp supplies the interrogation voice gates.
// No new network message.
//
// Honest gaps: the generated tables and PCM blobs are absent unless the private
// TO1_EXPLUS_DATA / TO1_FOOTSTEPS_DATA builds are supplied, so no layer plays in
// this tree. The peer effective Agility / slowdown feeds now come from
// net::match::skill_sync and gameplay::slowdown, ground / vertical speeds from
// net::audio::footsteps, and female `say` takes from net::audio::effort; the
// ctxdown / finisher / gun-exec record producers are still absent, as is a
// direct engine voice-count API beyond read-only engine::pcm::ActiveVoices().

#include "explus-core.hpp"

#include <cstdint>

namespace net::audio::explus {

    struct PairFeed {
        int      kind = 1;          // 0 gun, 1 melee (ext g_ex.kind 1 / 2)
        int      pair = -1;
        const char* name = nullptr;
        double   t0 = 0.0;
        uint32_t uid = 0;
        uint64_t attEnt = 0, vicEnt = 0;
        int      attMat = 0, vicMat = 0;
    };

    // Paired-move timeline. Both kinds are accepted; the gun producer is a feed
    // API because no native gun-execution pair table is exposed yet.
    void OnPairedStart(const PairFeed& feed);
    bool BeginGun(const PairFeed& feed);

    // Shared recipes. `att` is SID("player") for the finisher feed.
    void ContextAct(int act, int style, uint64_t att, uint64_t vic);
    void FinisherLabel(int label, uint64_t att);
    void DownedCue(int kind, int cls, uint64_t att, uint64_t vic);

    // Shared recipe used by interrogation foley. intfoley.cpp feeds it once per
    // shaking cue; no engine / network message is involved.
    void IntFoleyCue(int k, bool fist, uint64_t att, uint64_t vic);

    // Voice gates fed by intfoley; inert until intvox has a native producer.
    void SetShivActive(bool on) noexcept;
    void SetVicLine(bool on) noexcept;
    void SetVoxClash(bool on) noexcept;
    void SetVictimDying(bool on) noexcept;

    void ShiftClock(double fromT0, double deltaMs);
    void Tick();
    void Cut(const char* why);
    void Reset();
    bool Active() noexcept;

} // namespace net::audio::explus
