#pragma once

// Guarded private data sink for the execution-begging takes (the ext's
// vox_beg.h: VXG_LINES / VXG_RANGE / VXG_DUR / VXG_SCREAM / VXG_AVAIL_<lang>).
// Those SIDs and tables are game-derived and do not ship here, so every read
// lives behind TO1_EXEC_BEG_DATA.
//
// Build with -DTO1_EXEC_BEG_DATA and supply a private
// net/audio/exec_beg_data.hpp declaring these UNQUALIFIED names. That header is
// included below from inside the already-open net::audio::exec_beg::data
// namespace, so it must NOT wrap them in another namespace:
//   extern const uint64_t kLines[];   // VXG_LINES (at least kCount entries)
//   extern const int      kRange[];   // VXG_RANGE, flattened row-major as
//                                     // [(((fac*2+female)*4+voice)*8+kind)*2+field],
//                                     // fac 0 Seraphite / 1 WLF, female 0/1,
//                                     // voice 0..3, kind 0 plea .. 7 panting,
//                                     // field 0 first line, 1 count
//   extern const int      kDur[];     // VXG_DUR, voiced length in ms
//   extern const uint8_t  kScream[];  // VXG_SCREAM, 1 for the long shouts
//   extern const uint8_t  kAvail[];   // VXG_AVAIL_<active language>, or nullptr
//                                     // when the language is unknown/unfiltered
//   inline constexpr int  kCount = ...; // VXG_N
//
// kAvail is the table for the language this client latched; a generator shim
// over vox_beg.h can pass its VXG_AVAIL_* pointer directly. A definition of any
// array in a .cpp is fine as long as the extern declaration is visible here.
//
// With the macro off, kHave is false and Table() is empty: the pure core picks
// no record, consumes no wheel value and leaves the live paired-melee feed on
// the engine effort-event fallback. The ext's sidecars are handled by the
// owner; this header reads no flags.

#include "exec-beg-core.hpp"

namespace net::audio::exec_beg::data {

#if defined(TO1_EXEC_BEG_DATA)
#  include "exec_beg_data.hpp"

    inline constexpr bool kHave = true;

    inline core::VoxTable Table() noexcept
    {
        return { kLines, kRange, kDur, kScream, kAvail, kCount };
    }

    inline uint64_t Line(int rec) noexcept
    {
        return rec >= 0 && rec < kCount ? kLines[rec] : 0;
    }

#else

    inline constexpr bool kHave = false;
    inline constexpr int kCount = 0;

    inline core::VoxTable Table() noexcept { return {}; }
    inline uint64_t Line(int) noexcept { return 0; }

#endif

} // namespace net::audio::exec_beg::data
