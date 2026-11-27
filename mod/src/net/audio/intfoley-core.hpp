#pragma once

// Pure interrogation-foley rules (tools/ext/intfoley.c): which 2013 pairs own
// the extra body foley, the shaking window and its irregular accelerating gaps,
// the "never over a generated struggle row / victim line" guards, and the
// intvox voice clash yield. The cue recipes themselves are the pure
// explus-core.hpp recipes, so no rules are duplicated here. No engine, PCM,
// network, sidecar-file or private-data read happens here: the native owner in
// intfoley.cpp supplies the pair name, move clock, sidecar answers and the
// explus sink. Tests: mod/tests/intfoley_core_test.cpp.
//
// Honest gap: no native intvox timeline / shift producer exists, so the owner
// passes an inert LineFeed and the clash / duck functions return false until
// such a producer is added.

#include "explus-core.hpp"

#include <cstdint>
#include <cstring>

namespace net::audio::intfoley::core {

    inline constexpr double kFistStartMs = 2400.0;
    inline constexpr double kShivStartMs = 700.0;
    inline constexpr double kKillLeadMs = 300.0;
    inline constexpr double kLateMs = 300.0;
    inline constexpr double kStruggleGuardMs = 300.0;
    inline constexpr int    kMaxCues = 16;

    enum class PairKind : uint8_t { kNone = 0, kFist = 1, kShiv = 2 };

    inline PairKind Pair(const char* name) noexcept
    {
        if (!name)
            return PairKind::kNone;
        if (!std::strncmp(name, "fist-interrogate", 16))
            return PairKind::kFist;
        if (!std::strncmp(name, "shiv-interrogate", 16))
            return PairKind::kShiv;
        return PairKind::kNone;
    }

    inline bool Gate(int kind, const char* pairName, bool off) noexcept
    {
        return kind == 1 && Pair(pairName) != PairKind::kNone && off;
    }

    // The intvox feed. With no producer the owner leaves on=false and every
    // query yields false, exactly like a move with no voice line.
    struct LineStep { double at = 0.0; };
    struct LineFeed {
        bool     on = false;
        double   t0 = 0.0;
        double   shift = 0.0;
        double   busy[2] = {0.0, 0.0};
        double   vline[2] = {0.0, 0.0};
        int      step = 0, count = 0;
        LineStep steps[8];
    };

    inline bool VoxClash(const LineFeed& line, double moveT0, double ems,
                         int kind, const char* pairName) noexcept
    {
        if (kind != 1 || Pair(pairName) == PairKind::kNone
            || !line.on || line.t0 != moveT0)
            return false;
        const double t = ems + line.shift;
        for (int i = 0; i < 2; ++i)
            if (line.busy[i] > 0.0 && t <= line.busy[i] + 100.0)
                return true;
        for (int i = line.step; i < line.count && i < 8; ++i) {
            const double d = t - line.steps[i].at;
            if (d > -400.0 && d < 400.0)
                return true;
        }
        return false;
    }

    inline bool VicLineAt(const LineFeed& line, double moveT0, double ems) noexcept
    {
        if (!line.on || line.t0 != moveT0 || line.vline[1] <= 0.0)
            return false;
        const double t = ems + line.shift;
        return t >= line.vline[0] - 50.0 && t <= line.vline[1] + 50.0;
    }

    inline bool VicLine(const LineFeed& line, double moveT0, double ems,
                        int kind, const char* pairName) noexcept
    {
        return kind == 1 && Pair(pairName) != PairKind::kNone
            && VicLineAt(line, moveT0, ems);
    }

    inline bool StruggleNear(const explus::core::DataView& d, int pair,
                             double ms) noexcept
    {
        for (int i = 0; i < d.rowCount; ++i) {
            const explus::core::Row& r = d.rows[i];
            if (r.kind != 1 || r.pair != static_cast<uint8_t>(pair))
                continue;
            if (r.layer != static_cast<uint8_t>(explus::core::Layer::kStrugTorso)
                && r.layer != static_cast<uint8_t>(explus::core::Layer::kStrugHead))
                continue;
            const double delta = static_cast<double>(r.ms) - ms;
            if (delta > -kStruggleGuardMs && delta < kStruggleGuardMs)
                return true;
        }
        return false;
    }

    struct State {
        double   t0 = 0.0;
        bool     on = false;
        bool     finished = false;
        int      k = 0;
        double   next = 0.0;
        uint32_t seed = 0;
        bool     fist = false;
        double   a = 0.0, b = 0.0;
        uint64_t att = 0, vic = 0;
    };

    inline void Reset(State& s) noexcept { s = State{}; }

    inline bool Begin(State& s, int kind, const char* pairName, double t0,
                      double killMs, uint64_t att, uint64_t vic) noexcept
    {
        Reset(s);
        const PairKind p = kind == 1 ? Pair(pairName) : PairKind::kNone;
        if (p == PairKind::kNone)
            return false;
        s.t0 = t0;
        s.on = true;
        s.fist = p == PairKind::kFist;
        s.att = att;
        s.vic = vic;
        s.a = s.fist ? kFistStartMs : kShivStartMs;
        s.b = killMs - kKillLeadMs;
        s.next = s.a;
        s.seed = static_cast<uint32_t>(t0 * 0.001) * 2654435761u
               ^ static_cast<uint32_t>(t0) * 0x9e3779b1u;
        return s.b > s.a;
    }

    struct Cue {
        int    k = 0;             // explus::core::IntFoleyRecipe(k, fist)
        bool   fist = false;
        double atMs = 0.0;
        uint64_t att = 0, vic = 0;
    };

    struct Result {
        Cue cues[kMaxCues];
        int  count = 0;
    };

    inline void AddCue(Result& r, const Cue& c) noexcept
    {
        if (r.count < kMaxCues)
            r.cues[r.count++] = c;
    }

    // Emit the due shaking cues. `ems` is the exec+ move clock; `off` is the
    // int-foley / exec+ sidecar answer supplied by the owner.
    inline int Tick(State& s, double ems, const explus::core::DataView& d,
                    int pair, const LineFeed& line, bool off,
                    Result& out) noexcept
    {
        if (!s.on || off || s.finished)
            return 0;
        int emitted = 0;
        while (s.next < s.b && s.next <= ems) {
            const double at = s.next;
            s.seed = s.seed * 1664525u + 1013904223u;
            const double gap = 560.0 + static_cast<double>((s.seed >> 16) % 240u);
            s.next += gap * (1.0 - 0.25 * (at - s.a) / (s.b - s.a + 1.0));
            if (ems - at > kLateMs || s.next >= s.b) {
                ++s.k;
                if (s.next >= s.b)
                    s.finished = true;
                continue;
            }
            const int k = s.k++;
            if (StruggleNear(d, pair, at) || VicLineAt(line, s.t0, at))
                continue;
            Cue c;
            c.k = k;
            c.fist = s.fist;
            c.atMs = at;
            c.att = s.att;
            c.vic = s.vic;
            AddCue(out, c);
            ++emitted;
        }
        return emitted;
    }

} // namespace net::audio::intfoley::core
