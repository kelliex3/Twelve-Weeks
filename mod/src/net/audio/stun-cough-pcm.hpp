#pragma once

// Guarded private PCM sink for the stun cough (tools/ext/alert_sting.c 201-270,
// stun_cough.h / stun_cough.pcm: 6 male + 6 female TLOU2 cough one-shots). The blob
// is game-derived and does not ship here, so every read of it lives in this header
// behind TO1_STUN_COUGH_PCM, exactly like alertsting-pcm.hpp / near-miss-pcm.hpp.
//
// Build with -DTO1_STUN_COUGH_PCM and supply a private
// net/audio/stun_cough_pcm_data.hpp declaring the following UNQUALIFIED names. The
// header is included below from inside the already-open `pcm` namespace, so it
// must NOT wrap them in another namespace:
//   extern const engine::pcm::Sample kBlob[];      // all waves, back to back
//   struct Wave { int off; int n; int female; const char* name; };
//   inline constexpr Wave kWaves[] = { ... };      // ext SC_WAVE
//   inline constexpr int kCount = ...;             // ext SC_N (12)
//   inline constexpr int kRate = ...;              // ext SC_RATE
// A shim over the generator's contiguous sc_blob / SC_WAVE / SC_N / SC_RATE works.
//
// The owner builds the muffled 2-D copy once from the wave descriptors exposed
// below; peer voices use the raw blob through engine::pcm::PlayAtRange, which
// carries its own 2.5 / 45 m range (the ext temporarily set g_mix_rnear / rfar
// around pcm_play_at instead). Without the macro no voice starts, no cadence stamp,
// no RNG value and no per-slot freshness marker is consumed. The owner is the native
// sole owner of this sub-feature when the ext is absent; the ext is yielded via
// boot::ExtLoaded().

#include "stun-cough-core.hpp"

#include "engine/pcm.hpp"

#include <array>

namespace net::audio::stun_cough::pcm {

#if defined(TO1_STUN_COUGH_PCM)
#  include "stun_cough_pcm_data.hpp"

    inline constexpr bool kHave = true;

    inline const engine::pcm::Sample* Blob() noexcept { return kBlob; }
    inline int Rate() noexcept { return kRate; }

    inline const char* Name(int wave) noexcept
    {
        return wave >= 0 && wave < kCount ? kWaves[wave].name : "-";
    }

    // The private header's Wave keeps the generated field names; the pure core
    // only needs off / n / female, so translate once into a constexpr table.
    inline const std::array<core::WaveDesc, kCount>& Descriptors() noexcept
    {
        static const std::array<core::WaveDesc, kCount> table = [] {
            std::array<core::WaveDesc, kCount> out{};
            for (int i = 0; i < kCount; ++i)
                out[i] = { kWaves[i].off, kWaves[i].n, kWaves[i].female != 0 };
            return out;
        }();
        return table;
    }

#else

    inline constexpr bool kHave = false;
    inline constexpr int kCount = 0;

    inline const engine::pcm::Sample* Blob() noexcept { return nullptr; }
    inline int Rate() noexcept { return 0; }
    inline const char* Name(int) noexcept { return "-"; }
    inline const std::array<core::WaveDesc, 0>& Descriptors() noexcept
    {
        static const std::array<core::WaveDesc, 0> empty{};
        return empty;
    }

#endif

} // namespace net::audio::stun_cough::pcm
