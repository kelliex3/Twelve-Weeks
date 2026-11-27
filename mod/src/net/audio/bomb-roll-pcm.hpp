#pragma once

// Guarded private PCM sink for the thrown-bomb impact / roll waves. The game-derived
// bomb_roll.pcm / bomb_roll.h are not in this repo, so every read of them lives here
// behind TO1_BOMB_ROLL_PCM, exactly like near-miss-pcm.hpp / stun-cough-pcm.hpp.
//
// Build with -DTO1_BOMB_ROLL_PCM and supply a private
// net/audio/bomb_roll_pcm_data.hpp declaring the following UNQUALIFIED names. It is
// included below from inside the already-open `pcm` namespace, so it must NOT wrap
// them in another namespace:
//   extern const engine::pcm::Sample kBlob[];             // all waves, back to back
//   struct Wave { int off; int n; };                      // ext br_wave
//   inline constexpr int kRate = ...;                     // ext BR_RATE
//   inline constexpr Wave kSbHard[]   = { ... };          // ext SB_HARD_W
//   inline constexpr Wave kSbSoft[]   = { ... };          // SB_SOFT_W
//   inline constexpr Wave kNbHard[]   = { ... };          // NB_HARD_W
//   inline constexpr Wave kNbMid[]    = { ... };          // NB_MID_W
//   inline constexpr Wave kNbSoft[]   = { ... };          // NB_SOFT_W
//   inline constexpr Wave kNbRattle[] = { ... };          // NB_RATTLE_W
//   inline constexpr Wave kMoHit[]    = { ... };          // MO_HIT_W
//   inline constexpr Wave kSbRoll[]   = { ... };          // SB_ROLL_W
//   inline constexpr Wave kNbRoll[]   = { ... };          // NB_ROLL_W
//   inline constexpr Wave kMoRoll[]   = { ... };          // MO_ROLL_W
// Group wave counts are derived here with std::size; no private count constants
// are needed. A shim over the generator's contiguous br_blob / BR_* arrays works.
//
// The owner feeds the pure core and calls Play(cue) from Tick(). Play() applies
// ext's BR_MASTER and the read-only t2r-bomb-roll-vol.txt value (0..2, re-read at
// most every 2 s through engine::exe_side) at the voice start; core::Rnear maps
// rfar 24/16/10/11/8 to the 2 m / 1 m near radius. Without the macro kHave is
// false, kRate is 0, Volume() is 1, Play() returns false and starts no voice and
// consumes no RNG of its own.

#include "bomb-roll-core.hpp"

#include "engine/exe_side.hpp"
#include "engine/pcm.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <iterator>

namespace net::audio::bomb_roll::pcm {

#if defined(TO1_BOMB_ROLL_PCM)
#  include "bomb_roll_pcm_data.hpp"

    inline constexpr bool kHave = true;

    // ext br_vol: t2r-bomb-roll-vol.txt, default 1, clamped to 0..2, 2 s cadence.
    inline float Volume() noexcept
    {
        static float    v = 1.0f;
        static uint64_t at = 0;
        const uint64_t now = GetTickCount64();
        if (now - at > 2000) {
            at = now;
            v = 1.0f;
            if (FILE* f = engine::exe_side::Open("t2r-bomb-roll-vol.txt", "r")) {
                float x = 0.0f;
                if (std::fscanf(f, "%f", &x) == 1)
                    v = x < 0.0f ? 0.0f : (x > 2.0f ? 2.0f : x);
                std::fclose(f);
            }
        }
        return v;
    }

    inline int Rate() noexcept { return kRate; }

    template<class W>
    struct GroupView {
        const W* waves;
        int      count;
    };

    template<class W, std::size_t N>
    inline constexpr GroupView<W> View(const W (&a)[N]) noexcept
    {
        return { a, static_cast<int>(N) };
    }

    inline GroupView<Wave> Group(core::WaveGroup group) noexcept
    {
        switch (group) {
        case core::WaveGroup::SbHard:   return View(kSbHard);
        case core::WaveGroup::SbSoft:   return View(kSbSoft);
        case core::WaveGroup::NbHard:   return View(kNbHard);
        case core::WaveGroup::NbMid:    return View(kNbMid);
        case core::WaveGroup::NbSoft:   return View(kNbSoft);
        case core::WaveGroup::NbRattle: return View(kNbRattle);
        case core::WaveGroup::MoHit:    return View(kMoHit);
        case core::WaveGroup::SbRoll:   return View(kSbRoll);
        case core::WaveGroup::NbRoll:   return View(kNbRoll);
        case core::WaveGroup::MoRoll:   return View(kMoRoll);
        }
        return { nullptr, 0 };
    }

    inline int WaveCount(core::WaveGroup group) noexcept { return Group(group).count; }

    template<class W>
    inline bool Play(const W& w, float gain, int rate,
                     const float pos[3], float rfar) noexcept
    {
        if (!pos || w.off < 0 || w.n <= 0 || rate <= 0)
            return false;
        const engine::pcm::Voice v = engine::pcm::PlayAtRange(
            kBlob + w.off, w.n, gain * core::kMaster * Volume(), rate, pos,
            core::Rnear(rfar), rfar);
        return engine::pcm::Live(v);
    }

    inline bool Play(const core::Cue& cue) noexcept
    {
        const GroupView<Wave> g = Group(cue.group);
        if (cue.wave < 0 || cue.wave >= g.count || !g.waves)
            return false;
        return Play(g.waves[cue.wave], cue.gain, cue.rate, cue.pos, cue.rfar);
    }

#else

    inline constexpr bool kHave = false;
    inline constexpr int  kRate = 0;

    inline float Volume() noexcept { return 1.0f; }
    inline int Rate() noexcept { return kRate; }

    // Keep a core-produced cue's wave index in range even with no private table:
    // the sink is inert, but the pure rules stay observable to tests / logs.
    inline int WaveCount(core::WaveGroup) noexcept { return 1; }

    inline bool Play(const core::Cue&) noexcept { return false; }

    template<class W>
    inline bool Play(const W&, float, int, const float*, float) noexcept
    {
        return false;
    }

#endif

} // namespace net::audio::bomb_roll::pcm
