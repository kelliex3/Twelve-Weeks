#pragma once

// Guarded private-data sink for execution-foley-plus (tools/ext/explus.c: the
// generated execplus.h / execplus_tl.h and execplus.pcm). The layer timings,
// wave offsets / rates, generated names and PCM blob are game-derived and do
// not ship here, so every read lives behind TO1_EXPLUS_DATA, like
// exec-music-pcm.hpp / downed-vox-data.hpp.
//
// Build with -DTO1_EXPLUS_DATA and supply a private
// net/audio/explus_data.hpp declaring these UNQUALIFIED names. The include
// below is inside the `net::audio::explus::data` namespace, so the shim must
// NOT wrap them in another namespace:
//   extern const Layer kLayer[];        // ext EP_LAYER: { nv, mats, first, src }
//   extern const Wave  kWave[];         // ext EP_WAVE:  { off, n, rate }
//   inline constexpr int kWaveCount = ...;
//   extern const Row   kTimeline[];     // ext EXPLUS_TL: { kind, pair, layer,
//                                       //   who, joint, gain, ms }
//   inline constexpr int kTimelineCount = ...;
//   extern const char* const kGunNames[];     // ext EXPLUS_GUN_NAMES
//   inline constexpr int kGunNameCount = ...;
//   extern const char* const kMeleeNames[];   // ext EXPLUS_MELEE_NAMES
//   inline constexpr int kMeleeNameCount = ...;
//   extern const engine::pcm::Sample kBlob[], kBlobEnd[];
// The footsteps tier/wave blob comes from footsteps-data.hpp under its own
// guard; both guards are required before any layer can play.
//
// Without the macros the sink is inert: Data() is empty, Have() is false and
// core::TablesOk() refuses to arm a timeline. The pure rules and the owner API
// stay live; no invented data is played.

#include "engine/pcm.hpp"
#include "explus-core.hpp"
#include "footsteps-data.hpp"

#include <cstddef>
#include <cstdint>

namespace net::audio::explus::data {

    struct Layer { uint32_t nv = 0, mats = 0, first = 0; };
    struct Wave  { uint32_t off = 0, n = 0, rate = 0; };
    struct Row   { uint8_t kind = 0, pair = 0, layer = 0, who = 0, joint = 0, gain = 0; uint16_t ms = 0; };

#if defined(TO1_EXPLUS_DATA) && defined(TO1_FOOTSTEPS_DATA)
#  include "explus_data.hpp"

    inline constexpr bool Have = true;

    namespace detail {

        struct Converted {
            core::LayerSizing sizing[core::kLayerCount];
            core::WaveDesc    waves[kWaveCount];
            core::Row         rows[kTimelineCount];
            core::WaveDesc    footWaves[footsteps::data::kTierWaveCount];
            core::FootTier    footTiers[footsteps::data::kTierCount];

            Converted() noexcept
            {
                for (int l = 0; l < core::kLayerCount; ++l) {
                    sizing[l].nv = static_cast<uint8_t>(kLayer[l].nv);
                    sizing[l].mats = static_cast<uint8_t>(kLayer[l].mats);
                    sizing[l].first = static_cast<uint16_t>(kLayer[l].first);
                }
                for (int w = 0; w < kWaveCount; ++w) {
                    waves[w].off = kWave[w].off;
                    waves[w].n = kWave[w].n;
                    waves[w].rate = kWave[w].rate;
                }
                for (int r = 0; r < kTimelineCount; ++r) {
                    rows[r].kind = kTimeline[r].kind;
                    rows[r].pair = kTimeline[r].pair;
                    rows[r].layer = kTimeline[r].layer;
                    rows[r].who = kTimeline[r].who;
                    rows[r].joint = kTimeline[r].joint;
                    rows[r].gain = kTimeline[r].gain;
                    rows[r].ms = kTimeline[r].ms;
                }
                const footsteps::data::View fs = footsteps::data::Data();
                for (int w = 0; w < footsteps::data::kTierWaveCount; ++w) {
                    footWaves[w].off = fs.waves[w].off;
                    footWaves[w].n = fs.waves[w].n;
                    footWaves[w].rate = fs.waves[w].rate;
                }
                for (int t = 0; t < footsteps::data::kTierCount; ++t) {
                    footTiers[t].first = fs.tiers[t].first;
                    footTiers[t].count = fs.tiers[t].count;
                }
            }
        };

        inline const Converted& Table() noexcept
        {
            static const Converted c;
            return c;
        }

    } // namespace detail

    inline core::DataView Data() noexcept
    {
        const detail::Converted& t = detail::Table();
        const footsteps::data::View fs = footsteps::data::Data();
        core::DataView d;
        d.sizing = t.sizing;
        d.layerCount = core::kLayerCount;
        d.waves = t.waves;
        d.waveCount = kWaveCount;
        d.footWaves = t.footWaves;
        d.rows = t.rows;
        d.rowCount = kTimelineCount;
        d.gunNames = kGunNames;
        d.gunCount = kGunNameCount;
        d.meleeNames = kMeleeNames;
        d.meleeCount = kMeleeNameCount;
        d.footTiers = t.footTiers;
        d.footTierCount = footsteps::data::kTierCount;
        d.footWaveCount = footsteps::data::kTierWaveCount;
        d.footRate = fs.rate;
        d.pcmSamples = static_cast<uint32_t>(kBlobEnd - kBlob);
        d.footPcmSamples = fs.samples;
        return d;
    }

    inline const engine::pcm::Sample* Frames(int wave, bool foot) noexcept
    {
        if (foot)
            return footsteps::data::Frames(wave);
        if (wave < 0 || wave >= kWaveCount || !kBlob)
            return nullptr;
        return kBlob + static_cast<std::size_t>(kWave[wave].off);
    }

#else

    inline constexpr bool Have = false;

    inline core::DataView Data() noexcept { return core::DataView{}; }
    inline const engine::pcm::Sample* Frames(int, bool) noexcept { return nullptr; }

#endif

} // namespace net::audio::explus::data
