#pragma once

// Pure rules for the native port of tools/ext/bomb_roll.c (the LIVE function bodies, not
// the stale numbers in that file's top comment): a thrown bomb's floor / side impact and
// its post-impact rolling grains. No engine, audio, net, window or clock headers live
// here -- the owner supplies the clock, QPC delta, listener and PCM result. The only
// state is the ext's 12-entry tracker ring plus its RNG / last-pick-per-group caches.
// Tests: mod/tests/bomb_roll_core_test.cpp.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace net::audio::bomb_roll::core {

    // ---- ext constants (from the br_* code, not the BR_N / comment prose) ----------
    inline constexpr int      kRing        = 12;      // ext BR_N / g_br[12]
    inline constexpr float    kMaster      = 0.55f;   // ext BR_MASTER
    inline constexpr uint64_t kStaleMs     = 2500;    // g_br[j] free after 2500 ms
    inline constexpr double   kDtMin       = 0.003;   // ext: dt must be > 0.003 s
    inline constexpr double   kDtMax       = 0.2;     // ... and < 0.2 s
    inline constexpr float    kMaxStepSq   = 16.0f;   // a > 4 m frame step is a re-place
    inline constexpr int      kSkipFrames  = 2;       // ext br_steer / stall / jump

    inline constexpr float kFloorVy        = -1.0f;   // vp.y < -1 (was falling)
    inline constexpr float kFloorDvy       = 1.0f;   // dvy > 1 (reversed / stopped)
    inline constexpr float kSideDvh        = 2.8f;   // sideways velocity change
    inline constexpr float kSideHp         = 2.5f;   // with a real horizontal speed before
    inline constexpr float kSideRatioSq    = 0.16f;  // dvh^2 > 0.16 * hp^2
    inline constexpr float kSideStrength   = 0.9f;   // strength = dvh * 0.9
    inline constexpr float kMinStrength    = 0.9f;   // below this is not an impact
    inline constexpr float kMidStrength    = 1.5f;   // soft < 1.5 <= mid < 4 <= hard
    inline constexpr float kHardStrength   = 4.0f;
    inline constexpr uint64_t kImpactCdMs  = 80;     // between impacts
    inline constexpr float kImpactGain     = 0.7f;   // every impact wave's extra factor

    inline constexpr float kRollHsMin      = 0.45f;  // sideways speed window
    inline constexpr float kRollHsMax      = 7.0f;
    inline constexpr float kRollVyMax      = 0.9f;   // |vy| < 0.9 (on the ground)
    inline constexpr float kRollStepM      = 0.30f;  // one grain per 0.30 m of travel
    inline constexpr uint64_t kRollCdMs    = 220;    // between grains
    inline constexpr uint64_t kRollAfterImpactMs = 120;
    inline constexpr float kRollGainBase   = 0.05f;  // 0.05 + 0.25 * min(hs/4, 1)
    inline constexpr float kRollGainSpan   = 0.25f;
    inline constexpr float kRollGainScale  = 0.9f;
    inline constexpr float kRollRfar       = 8.0f;

    inline constexpr int   kGroupCount     = 10;     // WaveGroup values 0..9
    inline constexpr int   kLastGroups     = 12;     // ext g_br_last[12]

    enum class Kind {
        PipeBomb  = 1,   // nail / pipe bomb
        StunSmoke = 2,   // stun or smoke bomb
        Molotov   = 3,
    };

    // Ext br_play's group indices, in that exact order. 10 and 11 of g_br_last stay
    // unused so the cache keeps the ext's 12-slot layout.
    enum class WaveGroup {
        SbHard   = 0,
        SbSoft   = 1,
        NbHard   = 2,
        NbMid    = 3,
        NbSoft   = 4,
        NbRattle = 5,
        MoHit    = 6,
        SbRoll   = 7,
        NbRoll   = 8,
        MoRoll   = 9,
    };

    // ---- pure helpers ------------------------------------------------------------

    // ext's shared xorshift32 (t2r_rand), seeded from the clock on first use. The
    // 64-bit shifts and the uint32_t(x >> 33) truncation are deliberate.
    inline uint32_t NextRand(uint64_t& state, uint64_t nowMs) noexcept
    {
        uint64_t x = state;
        if (!x)
            x = (nowMs * 0x9E3779B97F4A7C15ULL) | 1ULL;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state = x;
        return static_cast<uint32_t>(x >> 33);
    }

    // ext br_gain: 0.12 + 0.88 * (min(str / 9, 1))^0.7.
    inline float Gain(float str) noexcept
    {
        const float x = str / 9.0f;
        return 0.12f + 0.88f * std::pow(x > 1.0f ? 1.0f : x, 0.7f);
    }

    // ext br_play: a small can is full within 1 or 2 m, then rolls off to rfar.
    inline float Rnear(float rfar) noexcept { return rfar <= 10.0f ? 1.0f : 2.0f; }

    // ext br_play's player_locator check: drop a wave only when the bomb is strictly
    // outside (rfar + 3) m of the listener. No listener (nullptr) means no drop,
    // exactly like a failed player_locator in the ext.
    inline bool OutOfEarshot(const float pos[3], const float listener[3],
                            float rfar) noexcept
    {
        if (!pos || !listener)
            return false;
        const float dx = pos[0] - listener[0];
        const float dy = pos[1] - listener[1];
        const float dz = pos[2] - listener[2];
        const float sum = dx + dy + dz;
        if (!std::isfinite(sum))
            return false;
        const float limit = rfar + 3.0f;
        return dx * dx + dy * dy + dz * dz > limit * limit;
    }

    // ---- state ------------------------------------------------------------------

    // One ext g_br[i] record. `q` / QPC is deliberately NOT here: the owner keeps
    // the clock and hands Feed() a ready dt (GetTickCount64 for the cooldowns, QPC
    // for the velocity). `slot < 0` is our own throw, as in the ext.
    struct Tracker {
        uint64_t h       = 0;
        uint64_t seenMs  = 0;
        uint64_t impMs   = 0;
        uint64_t rollMs  = 0;
        int      kind    = 0;
        int      slot    = -1;
        int      armed   = 0;   // ext: set on the first impact, never cleared by a roll
        int      skip    = 0;   // ext: two judged frames after a steer / stall / jump
        int      nimp    = 0;
        float    last[3] = { 0.0f, 0.0f, 0.0f };
        float    v[3]    = { 0.0f, 0.0f, 0.0f };
        float    rollD   = 0.0f;
    };

    // The private PCM header supplies the real per-group wave counts; the owner
    // copies them into `waveCount` once (default 1 keeps Feed() valid with no data).
    // `baseRate` is the private header's kRate; PitchRate() applies ext's +-6 %.
    struct State {
        std::array<Tracker, kRing>    slots{};
        std::array<int, kLastGroups>  last{};       // ext g_br_last: no same wave twice
        std::array<int, kGroupCount>  waveCount{};
        int                           baseRate = 0;
        uint64_t                      rng = 0;

        State() noexcept { waveCount.fill(1); }
    };

    inline void SetWaveCount(State& s, WaveGroup group, int count) noexcept
    {
        const int i = static_cast<int>(group);
        if (i >= 0 && i < kGroupCount)
            s.waveCount[static_cast<std::size_t>(i)] = count > 0 ? count : 1;
    }

    // One br_play: wave without an immediate repeat for this group, then the pitch
    // RNG. That order (wave, then rate) is the ext's and is observable.
    inline int PickWave(State& s, WaveGroup group, int count, uint64_t nowMs) noexcept
    {
        const int gi = static_cast<int>(group);
        if (count <= 0 || gi < 0 || gi >= kLastGroups)
            return -1;
        int k = static_cast<int>(NextRand(s.rng, nowMs)
                                 % static_cast<uint32_t>(count));
        if (count > 1 && k == s.last[static_cast<std::size_t>(gi)])
            k = (k + 1) % count;
        s.last[static_cast<std::size_t>(gi)] = k;
        return k;
    }

    // ext: BR_RATE * (0.94 + 0.12 * (rand & 1023) / 1023), truncated to int.
    inline int PitchRate(State& s, int baseRate, uint64_t nowMs) noexcept
    {
        const float jitter = 0.94f
            + 0.12f * static_cast<float>(NextRand(s.rng, nowMs) & 1023u) / 1023.0f;
        return static_cast<int>(static_cast<float>(baseRate) * jitter);
    }

    struct Cue {
        WaveGroup group   = WaveGroup::SbHard;
        int       wave    = -1;
        int       rate    = 0;
        float     gain    = 0.0f;      // pre-master, pre-Volume
        float     rfar    = 0.0f;
        float     pos[3]  = { 0.0f, 0.0f, 0.0f };
        bool      primary = true;      // false = the ext's secondary layer / rattle
    };

    struct FrameResult {
        enum class Type { None, Impact, Roll };

        Type type    = Type::None;
        Cue  cues[2] = {};
        int  n       = 0;
        float strength = 0.0f;         // the impact strength, 0 for a roll / none
    };

    namespace detail {

        inline void AddCue(FrameResult& out, State& s, WaveGroup group, float gain,
                           float rfar, const float pos[3], uint64_t nowMs,
                           bool primary) noexcept
        {
            if (out.n >= 2)
                return;
            Cue& c = out.cues[out.n++];
            c.group = group;
            const int gi = static_cast<int>(group);
            c.wave = PickWave(s, group,
                              gi >= 0 && gi < kGroupCount
                                  ? s.waveCount[static_cast<std::size_t>(gi)] : 0,
                              nowMs);
            c.rate = PitchRate(s, s.baseRate, nowMs);
            c.gain = gain;
            c.rfar = rfar;
            for (int j = 0; j < 3; ++j)
                c.pos[j] = pos[j];
            c.primary = primary;
        }

        inline void ImpactCues(FrameResult& out, State& s, int kind, float str,
                               const float pos[3], uint64_t nowMs) noexcept
        {
            const float g = Gain(str) * kImpactGain;
            const bool hard = str >= kHardStrength;
            const bool mid = !hard && str >= kMidStrength;
            const Kind k = static_cast<Kind>(kind);

            if (k == Kind::StunSmoke) {
                if (hard) {
                    AddCue(out, s, WaveGroup::SbHard, g * 1.3f, 24.0f,
                           pos, nowMs, true);
                } else {
                    AddCue(out, s, WaveGroup::SbSoft, g * (mid ? 1.5f : 1.1f),
                           mid ? 16.0f : 10.0f, pos, nowMs, true);
                    if (mid)
                        AddCue(out, s, WaveGroup::SbHard, g * 0.35f, 16.0f,
                               pos, nowMs, false);
                }
            } else if (k == Kind::PipeBomb) {
                if (hard) {
                    AddCue(out, s, WaveGroup::NbHard, g, 24.0f, pos, nowMs, true);
                    AddCue(out, s, WaveGroup::NbRattle, g * 0.45f, 16.0f,
                           pos, nowMs, false);
                } else if (mid) {
                    AddCue(out, s, WaveGroup::NbMid, g * 1.1f, 16.0f,
                           pos, nowMs, true);
                    AddCue(out, s, WaveGroup::NbRattle, g * 0.3f, 11.0f,
                           pos, nowMs, false);
                } else {
                    AddCue(out, s, WaveGroup::NbSoft, g * 1.3f, 16.0f,
                           pos, nowMs, true);
                    AddCue(out, s, WaveGroup::NbRattle, g * 0.2f, 8.0f,
                           pos, nowMs, false);
                }
            } else if (k == Kind::Molotov) {
                AddCue(out, s, WaveGroup::MoHit, g * 1.2f,
                       hard ? 24.0f : mid ? 16.0f : 10.0f, pos, nowMs, true);
            }
        }

        inline void RollCue(FrameResult& out, State& s, int kind, float hs,
                            const float pos[3], uint64_t nowMs) noexcept
        {
            const float f = hs > 4.0f ? 1.0f : hs / 4.0f;
            const float g = (kRollGainBase + kRollGainSpan * f) * kRollGainScale;
            const WaveGroup group = kind == static_cast<int>(Kind::StunSmoke)
                ? WaveGroup::SbRoll
                : kind == static_cast<int>(Kind::PipeBomb)
                    ? WaveGroup::NbRoll : WaveGroup::MoRoll;
            AddCue(out, s, group, g, kRollRfar, pos, nowMs, true);
        }

    } // namespace detail

    // ext br_steer: the trajectory stream set this copy's velocity; the next two
    // judged frames are ours, not a contact.
    inline void Steer(State& s, uint64_t h) noexcept
    {
        for (Tracker& t : s.slots)
            if (t.h == h) {
                t.skip = kSkipFrames;
                return;
            }
    }

    // Forget a handle immediately (an unwatch / invalid locator), rather than
    // waiting for the 2500 ms stale eviction used by Feed().
    inline void Forget(State& s, uint64_t h) noexcept
    {
        for (Tracker& t : s.slots)
            if (t.h == h)
                t = Tracker{};
    }

    // One ext br_feed frame. `nowMs` is GetTickCount64 and `dt` is the QPC delta in
    // seconds, both supplied by the owner. First sight only seeds the record; a
    // stalled / jumping frame sets skip=2 and zeroes the stored velocity; a skipped
    // frame stores velocity but judges no contact. The stored velocity is updated
    // at the end of every judged frame, exactly like ext's final memcpy(vp, v, 12).
    inline FrameResult Feed(State& s, uint64_t h, int kind, int slot,
                            const float pos[3], uint64_t nowMs, double dt) noexcept
    {
        FrameResult out;
        if (!h || !pos || (kind != static_cast<int>(Kind::PipeBomb)
                        && kind != static_cast<int>(Kind::StunSmoke)
                        && kind != static_cast<int>(Kind::Molotov))
            || !std::isfinite(pos[0] + pos[1] + pos[2]))
            return out;

        int hit = -1, free = -1, old = 0;
        for (int i = 0; i < kRing; ++i) {
            Tracker& t = s.slots[static_cast<std::size_t>(i)];
            if (t.h == h) {
                hit = i;                       // exact handle wins, stale or not
                break;
            }
            if (!t.h || nowMs - t.seenMs > kStaleMs) {
                if (free < 0)
                    free = i;
            }
            if (t.seenMs < s.slots[static_cast<std::size_t>(old)].seenMs)
                old = i;
        }

        if (hit < 0) {
            const int i = free >= 0 ? free : old;
            Tracker& t = s.slots[static_cast<std::size_t>(i)];
            t = Tracker{};
            t.h = h;
            t.kind = kind;
            t.slot = slot;
            t.seenMs = nowMs;
            for (int j = 0; j < 3; ++j)
                t.last[j] = pos[j];
            return out;                        // ext: first frame only remembered
        }

        Tracker& t = s.slots[static_cast<std::size_t>(hit)];
        const float d[3] = { pos[0] - t.last[0], pos[1] - t.last[1],
                             pos[2] - t.last[2] };
        t.seenMs = nowMs;
        for (int j = 0; j < 3; ++j)
            t.last[j] = pos[j];

        const float stepSq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (!(dt > kDtMin && dt < kDtMax) || stepSq > kMaxStepSq) {
            t.skip = kSkipFrames;
            for (int j = 0; j < 3; ++j)
                t.v[j] = 0.0f;
            return out;
        }

        const float v[3] = { static_cast<float>(static_cast<double>(d[0]) / dt),
                             static_cast<float>(static_cast<double>(d[1]) / dt),
                             static_cast<float>(static_cast<double>(d[2]) / dt) };
        if (t.skip > 0) {
            --t.skip;
            for (int j = 0; j < 3; ++j)
                t.v[j] = v[j];
            return out;
        }

        const float hs = std::sqrt(v[0] * v[0] + v[2] * v[2]);
        const float dvy = v[1] - t.v[1];
        const float dvx = v[0] - t.v[0];
        const float dvz = v[2] - t.v[2];
        const float hp = std::sqrt(t.v[0] * t.v[0] + t.v[2] * t.v[2]);
        const float dvh = std::sqrt(dvx * dvx + dvz * dvz);

        float str = 0.0f;
        if (t.v[1] < kFloorVy && dvy > kFloorDvy)
            str = dvy;                         // the fall reversed or stopped: floor
        if (dvh > kSideDvh && hp > kSideHp
            && dvh * dvh > kSideRatioSq * hp * hp) {
            const float s2 = dvh * kSideStrength;
            if (s2 > str)
                str = s2;                      // wall / edge / body: sideways change
        }

        if (str >= kMinStrength
            && (t.impMs == 0 || nowMs - t.impMs >= kImpactCdMs)) {
            t.impMs = nowMs;
            t.armed = 1;
            ++t.nimp;
            t.rollD = 0.0f;
            out.type = FrameResult::Type::Impact;
            out.strength = str;
            detail::ImpactCues(out, s, t.kind, str, pos, nowMs);
        } else if (t.armed && hs >= kRollHsMin && hs <= kRollHsMax
                   && std::fabs(v[1]) < kRollVyMax) {
            t.rollD += hs * static_cast<float>(dt);
            if (t.rollD >= kRollStepM
                && (t.rollMs == 0 || nowMs - t.rollMs >= kRollCdMs)
                && t.impMs != 0 && nowMs - t.impMs >= kRollAfterImpactMs) {
                t.rollD = 0.0f;
                t.rollMs = nowMs;
                out.type = FrameResult::Type::Roll;
                detail::RollCue(out, s, t.kind, hs, pos, nowMs);
            }
        }

        for (int j = 0; j < 3; ++j)
            t.v[j] = v[j];
        return out;
    }

    // Reset the tracker ring / RNG / last-pick cache. The data configuration
    // (waveCount, baseRate) is kept so an owner does not have to re-seed it.
    inline void Reset(State& s) noexcept
    {
        const auto counts = s.waveCount;
        const int rate = s.baseRate;
        s = State{};
        s.waveCount = counts;
        s.baseRate = rate;
    }

} // namespace net::audio::bomb_roll::core
