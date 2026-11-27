#pragma once

// Pure rules for the native downed-vox owner (ported from tools/ext/downed_vox.c,
// `dv_*` for groans / breaths and `hs_*` for headshot deaths). No engine, audio,
// net, filesystem or clock calls live here: the caller supplies the wall clock,
// the downed-body feed, the owner's is-speaking? answer and the private line
// table. The rules kept:
//   * one poll per kPollMs (50 ms) for the downed bodies; the headshot queue is
//     consumed every Tick (ext's hs_tick is outside dv_tick's poll gate);
//   * first active poll is silent: since = now, next = now + 2600 + rand(2000),
//     and a second gate of now - since >= 2600 before any line;
//   * lying <-> crawling shortens next to min(next, now + 1500 + rand(1500));
//   * a busy body retries in 700 ms, a body with no takes in 3000 ms;
//   * a spoken line holds stop authority for 3000 ms; a stop cue is emitted only
//     while spoke_until > now and the body has an entity;
//   * groan / breath cadences are 6000 + rand(5000) and 2600 + rand(1600);
//   * headshots use 8 recent-mark rows and 6 due-queue rows, dedupe by entity,
//     force upgrades a queued row without moving its due time, marks expire at
//     exactly 3000 ms, and every due row is consumed once (matchLive and
//     force || recent still gate the two emitted cues: stop then gargle).
//
// Body indices are fixed at 8 peer slots + the local body at index 8 (the mod's
// file has 8 net slots while the ext's T2R_MAX_PEERS is 4; the native owner only
// fills the first kMaxNetPlayers peer rows, the rest stay inactive). The line
// table is injected as core::LineTable: data::Table() is empty when
// TO1_DOWNED_VOX_DATA is off, in which case no cue, no RNG draw and no cadence
// stamp is consumed by the downed-body rules. Headshot gargles are engine voice
// names chosen here deterministically and do not depend on the private table.
// Tests: mod/tests/downed_vox_core_test.cpp.

#include <array>
#include <cstdint>

namespace net::audio::downed_vox::core {

    inline constexpr uint64_t kPollMs           = 50;
    inline constexpr uint64_t kFirstMs          = 2600;
    inline constexpr uint64_t kFirstJitterMs    = 2000;
    inline constexpr uint64_t kSecondGateMs     = 2600;
    inline constexpr uint64_t kCrawlShiftMs     = 1500;
    inline constexpr uint64_t kCrawlShiftJitter = 1500;
    inline constexpr uint64_t kBusyRetryMs      = 700;
    inline constexpr uint64_t kNoLineRetryMs    = 3000;
    inline constexpr uint64_t kSpeakHoldMs      = 3000;
    inline constexpr uint64_t kLieCadenceMs     = 6000;
    inline constexpr uint64_t kLieJitterMs      = 5000;
    inline constexpr uint64_t kCrawlCadenceMs   = 2600;
    inline constexpr uint64_t kCrawlJitterMs    = 1600;
    inline constexpr uint64_t kHeadshotRecentMs = 3000;

    inline constexpr int kPeerSlots  = 8;
    inline constexpr int kLocalBody  = kPeerSlots;      // 8
    inline constexpr int kBodies     = kPeerSlots + 1;   // 9
    inline constexpr int kMarkRows   = 8;
    inline constexpr int kQueueRows  = 6;
    inline constexpr int kFacCount   = 2;
    inline constexpr int kFemaleCount= 2;
    inline constexpr int kVoiceCount = 4;
    inline constexpr int kKindCount  = 2;               // 0 groan, 1 breath
    inline constexpr int kMaxCues    = kBodies + kQueueRows * 2;

    enum class CueKind : uint8_t {
        kSpeak,       // Speak(ent, line): line is a private VXD_LINES SID
        kStopSpeak,   // StopSpeak(ent): cut whatever this body is saying
        kGargle,      // Gargle(ent, index): index 0/1 names an engine voice event
    };

    struct Cue {
        CueKind  kind  = CueKind::kSpeak;
        uint64_t ent   = 0;
        uint64_t line  = 0;   // kSpeak only
        int      index = -1;  // kGargle only (0 or 1)
    };

    struct Result {
        std::array<Cue, kMaxCues> cues{};
        int count = 0;
        void Add(Cue cue) noexcept
        {
            if (count < kMaxCues)
                cues[count++] = cue;
        }
    };

    // `fac`, `voice` are -1 when the native owner has no mapping; `female` may
    // still be supplied for logs but selects nothing without a valid fac/voice.
    struct BodyInput {
        uint64_t ent    = 0;
        bool     active = false;   // ext's ok gate for this downed body
        bool     crawl  = false;
        int      fac    = -1;
        int      voice  = -1;
        bool     female = false;
    };

    // The private generated table, injected so this header stays pure and the
    // rules can be unit-tested with a synthetic table. `range` is VXD_RANGE
    // indexed [fac][female][voice][kind][first, count]; `lines` is VXD_LINES.
    struct LineTable {
        const uint64_t* lines = nullptr;
        const int (*range)[2][4][3][2] = nullptr;
        int count = 0;
        bool Usable() const noexcept { return lines && range && count > 0; }
    };

    struct Body {
        uint64_t next        = 0;
        uint64_t since       = 0;
        uint64_t spoke_until = 0;
        uint64_t ent         = 0;
        bool     act         = false;
        bool     crawl       = false;
        int      last[2]     = { -1, -1 };
    };

    struct HeadMark { uint64_t ent = 0; uint64_t at = 0; };
    struct HeadQueue { uint64_t ent = 0; uint64_t due = 0; bool force = false; };

    struct State {
        std::array<Body, kBodies>     body{};
        std::array<HeadMark, kMarkRows>  mark{};
        std::array<HeadQueue, kQueueRows> queue{};
        uint64_t rng      = 0;
        uint64_t lastPoll = 0;
    };

    struct TickInput {
        uint64_t now = 0;
        bool matchLive = false;
        bool downedOff = false;    // t2r-no-downed-vox.txt
        bool headshotsOff = false; // t2r-no-headshot-vox.txt
        const BodyInput* bodies = nullptr;
        int bodyCount = 0;
        const bool* busy = nullptr;  // owner's is-speaking? answer, per body
        LineTable table{};           // empty => no groan/breath path at all
    };

    // ext t2r_rand: 64-bit xorshift32-family generator, seeded from `now`.
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

    struct LinePick {
        uint64_t line = 0;
        int      kind = 0;
        bool     flipped = false;   // requested kind was empty, used the other
    };

    // ext dv_sid's take choice. An out-of-range fac/voice or a kind with no
    // takes returns line == 0 WITHOUT touching RNG or last[]; the caller then
    // applies the 3000 ms no-line retry. Kind 2 is never indexed here (the ext
    // read one row past its table for headshots; that bug is not reproduced).
    inline LinePick PickLine(State& s, uint64_t now, const LineTable& table,
                             int bodyIndex, int fac, bool female, int voice,
                             int kind) noexcept
    {
        LinePick out;
        if (!table.Usable() || bodyIndex < 0 || bodyIndex >= kBodies
            || fac < 0 || fac >= kFacCount || voice < 0 || voice >= kVoiceCount
            || (kind != 0 && kind != 1))
            return out;
        out.kind = kind;
        const int fem = female ? 1 : 0;
        if (table.range[fac][fem][voice][out.kind][1] <= 0) {
            out.kind ^= 1;      // flip ONCE, like ext's `k ^= 1`
            out.flipped = true;
            if (table.range[fac][fem][voice][out.kind][1] <= 0)
                return out;    // no takes at all: no RNG, no state change
        }
        const int first = table.range[fac][fem][voice][out.kind][0];
        const int n     = table.range[fac][fem][voice][out.kind][1];
        if (first < 0 || first >= table.count || n <= 0 || n > table.count - first)
            return out;

        Body& b = s.body[bodyIndex];
        const uint32_t r = NextRand(s.rng, now);
        int t = static_cast<int>(r % static_cast<uint32_t>(n));
        if (n > 1 && t == b.last[out.kind])
            t = (t + 1) % n;            // never the same take twice in a row
        b.last[out.kind] = t;
        out.line = table.lines[first + t];
        return out;
    }

    inline void CutBody(State& s, int bodyIndex, uint64_t now, Result& out) noexcept
    {
        if (bodyIndex < 0 || bodyIndex >= kBodies)
            return;
        Body& b = s.body[bodyIndex];
        if (b.spoke_until > now && b.ent)
            out.Add({ CueKind::kStopSpeak, b.ent, 0, -1 });
        b = Body{};
        b.last[0] = b.last[1] = -1;
    }

    inline bool DueForSpeak(const State& s, int bodyIndex, uint64_t now) noexcept
    {
        if (bodyIndex < 0 || bodyIndex >= kBodies)
            return false;
        const Body& b = s.body[bodyIndex];
        return b.act && now >= b.next && now >= b.since
            && now - b.since >= kSecondGateMs;
    }

    inline void AdvanceBody(State& s, uint64_t now, int bodyIndex,
                            const BodyInput& in, bool busy, const LineTable& table,
                            Result& out) noexcept
    {
        if (!in.active) {
            if (s.body[bodyIndex].act || s.body[bodyIndex].spoke_until)
                CutBody(s, bodyIndex, now, out);
            return;
        }
        Body& b = s.body[bodyIndex];
        b.ent = in.ent;
        if (!b.act) {
            b.act = true;
            b.since = now;
            b.crawl = in.crawl;
            b.last[0] = b.last[1] = -1;
            // No private data: no RNG draw and no cadence stamp at all.
            if (table.Usable())
                b.next = now + kFirstMs + NextRand(s.rng, now) % kFirstJitterMs;
            return;
        }
        if (!table.Usable())
            return;

        if (in.crawl != b.crawl) {
            b.crawl = in.crawl;
            const uint64_t t = now + kCrawlShiftMs
                             + NextRand(s.rng, now) % kCrawlShiftJitter;
            if (t < b.next)
                b.next = t;      // ext: only shorten, never push the cue out
        }
        if (now < b.next || now - b.since < kSecondGateMs)
            return;
        if (busy) {
            b.next = now + kBusyRetryMs;
            return;
        }
        const LinePick p = PickLine(s, now, table, bodyIndex, in.fac, in.female,
                                    in.voice, b.crawl ? 1 : 0);
        if (!p.line) {
            b.next = now + kNoLineRetryMs;
            return;
        }
        out.Add({ CueKind::kSpeak, in.ent, p.line, -1 });
        b.spoke_until = now + kSpeakHoldMs;
        b.next = now + (b.crawl
                          ? kCrawlCadenceMs + NextRand(s.rng, now) % kCrawlJitterMs
                          : kLieCadenceMs + NextRand(s.rng, now) % kLieJitterMs);
    }

    // ---- headshot deaths (independent of the private groan/breath table) ----

    inline void MarkHeadshot(State& s, uint64_t ent, uint64_t now) noexcept
    {
        if (!ent || ent == ~0ULL)
            return;
        int slot = -1;
        for (int i = 0; i < kMarkRows; ++i) {
            if (s.mark[i].ent == ent) { slot = i; break; }
            if (slot < 0 && !s.mark[i].ent) slot = i;
        }
        if (slot < 0)
            slot = static_cast<int>(now & 7ULL);   // ext's overwrite-on-full
        s.mark[slot].ent = ent;
        s.mark[slot].at = now;
    }

    inline void UnmarkHeadshot(State& s, uint64_t ent) noexcept
    {
        for (int i = 0; i < kMarkRows; ++i) {
            if (s.mark[i].ent == ent) {
                s.mark[i].ent = 0;
                s.mark[i].at = 0;
            }
        }
    }

    inline bool RecentHeadshot(const State& s, uint64_t ent, uint64_t now) noexcept
    {
        if (!ent || ent == ~0ULL)
            return false;
        for (int i = 0; i < kMarkRows; ++i) {
            if (s.mark[i].ent == ent)
                return now >= s.mark[i].at
                    && now - s.mark[i].at < kHeadshotRecentMs;  // exactly 3000 expires
        }
        return false;
    }

    inline void ArmHeadshot(State& s, uint64_t ent, bool force, int delayMs,
                            uint64_t now) noexcept
    {
        if (!ent || ent == ~0ULL)
            return;
        for (int i = 0; i < kQueueRows; ++i) {
            if (s.queue[i].ent == ent) {
                if (force)
                    s.queue[i].force = true;   // due time stays as first armed
                return;
            }
        }
        for (int i = 0; i < kQueueRows; ++i) {
            if (!s.queue[i].ent) {
                s.queue[i].ent = ent;
                s.queue[i].due = now + static_cast<uint64_t>(delayMs > 0 ? delayMs : 0);
                s.queue[i].force = force;
                return;
            }
        }
        // ext drops a seventh distinct entity (no queue overwrite).
    }

    // ext hs_tick: due rows are consumed every frame, before the match / recent
    // gates. The gargle index is a stable ent/due hash: ext tried gargle[0] and
    // only fell back to gargle[1] when `say` itself was unresolved; with the
    // native AUDIO::PLAY_VOICE_LINE sink there is no result to branch on, so at
    // most one of the two engine voice names is emitted here.
    inline void TickHeadshots(State& s, uint64_t now, bool matchLive, bool off,
                              Result& out) noexcept
    {
        for (int i = 0; i < kQueueRows; ++i) {
            HeadQueue& q = s.queue[i];
            if (!q.ent || now < q.due)
                continue;
            const uint64_t ent = q.ent;
            const uint64_t due = q.due;
            const bool force = q.force;
            q = HeadQueue{};
            if (!matchLive || off || !ent || ent == ~0ULL)
                continue;
            if (!force && !RecentHeadshot(s, ent, now))
                continue;
            out.Add({ CueKind::kStopSpeak, ent, 0, -1 });
            const int gargle = static_cast<int>((ent ^ (ent >> 32) ^ due) & 1ULL);
            out.Add({ CueKind::kGargle, ent, 0, gargle });
        }
    }

    inline Result Tick(State& s, const TickInput& in) noexcept
    {
        Result out;
        // ext dv_tick's 50 ms gate covers the body poll AND the "left match /
        // switched off" cut pass; the headshot queue runs every frame.
        if (in.now >= s.lastPoll && in.now - s.lastPoll >= kPollMs) {
            s.lastPoll = in.now;
            if (!in.matchLive || in.downedOff) {
                for (int i = 0; i < kBodies; ++i)
                    if (s.body[i].act || s.body[i].spoke_until)
                        CutBody(s, i, in.now, out);
            } else {
                const int n = in.bodyCount < kBodies ? in.bodyCount : kBodies;
                for (int i = 0; i < n; ++i) {
                    const BodyInput empty{};
                    const BodyInput& b = in.bodies ? in.bodies[i] : empty;
                    const bool busy = in.busy ? in.busy[i] : false;
                    AdvanceBody(s, in.now, i, b, busy, in.table, out);
                }
            }
        }
        TickHeadshots(s, in.now, in.matchLive, in.headshotsOff, out);
        return out;
    }

    inline bool Active(const State& s, uint64_t now) noexcept
    {
        for (int i = 0; i < kBodies; ++i)
            if (s.body[i].act || s.body[i].spoke_until > now)
                return true;
        for (int i = 0; i < kMarkRows; ++i)
            if (s.mark[i].ent)
                return true;
        for (int i = 0; i < kQueueRows; ++i)
            if (s.queue[i].ent)
                return true;
        return false;
    }

    inline void Reset(State& s) noexcept { s = State{}; }

} // namespace net::audio::downed_vox::core
