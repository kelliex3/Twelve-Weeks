// Standalone harness for the pure downed-vox core (no game / engine headers):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src -I mod/src/net/audio
//       mod/tests/downed_vox_core_test.cpp -o /tmp/downed_vox && /tmp/downed_vox

#include "downed-vox-core.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

    namespace core = net::audio::downed_vox::core;

    static_assert(core::kLocalBody == 8 && core::kBodies == 9);
    static_assert(core::kMarkRows == 8 && core::kQueueRows == 6);

    // Synthetic VXD_LINES / VXD_RANGE. Nothing private is needed to test the
    // rules: fac 0 voice 0 = two groans + one breath, voice 1 = no groan / one
    // breath (the ext's flip case), voice 2 = no takes at all.
    const uint64_t kLines[] = { 100, 101, 102, 103, 104, 105 };
    int kRange[2][2][4][3][2] = {};

    core::LineTable Table()
    {
        return { kLines, kRange, static_cast<int>(sizeof kLines / sizeof kLines[0]) };
    }

    void InitRange()
    {
        kRange[0][0][0][0][0] = 0; kRange[0][0][0][0][1] = 2;   // groans 100, 101
        kRange[0][0][0][1][0] = 2; kRange[0][0][0][1][1] = 1;   // breath 102
        kRange[0][0][1][0][0] = 0; kRange[0][0][1][0][1] = 0;   // flip -> breath
        kRange[0][0][1][1][0] = 3; kRange[0][0][1][1][1] = 1;   // breath 103
        kRange[0][0][2][0][1] = 0; kRange[0][0][2][1][1] = 0;   // no takes
    }

    core::TickInput Input(uint64_t now, const core::BodyInput* bodies, int count,
                          const core::LineTable& table, const bool* busy = nullptr)
    {
        core::TickInput in;
        in.now = now;
        in.matchLive = true;
        in.bodies = bodies;
        in.bodyCount = count;
        in.busy = busy;
        in.table = table;
        return in;
    }

    core::BodyInput Body(uint64_t ent, bool active = true, bool crawl = false,
                         int fac = 0, int voice = 0)
    {
        core::BodyInput b;
        b.ent = ent;
        b.active = active;
        b.crawl = crawl;
        b.fac = fac;
        b.voice = voice;
        return b;
    }

    // ---- RNG and line selection -----------------------------------------------

    void TestRngAndPick()
    {
        uint64_t seed = 0;
        const uint32_t first = core::NextRand(seed, 1000);
        assert(first != 0);
        assert(seed != 0);
        // Same clock and seed give the same draw; a fresh state seeds from now.
        uint64_t a = 0, b = 0;
        assert(core::NextRand(a, 777) == core::NextRand(b, 777));
        assert(a == b);

        core::State s;
        uint64_t prev = 0;
        for (int i = 0; i < 12; ++i) {
            const core::LinePick p = core::PickLine(s, 1000 + i, Table(), 0,
                                                    0, false, 0, 0);
            assert(p.line == 100 || p.line == 101);
            assert(p.line != prev);       // never the same take twice in a row
            prev = p.line;
        }
        assert(s.body[0].last[0] >= 0 && s.body[0].last[0] < 2);

        // Requested groan has no takes: flip once to breath (no kind 2 bug).
        core::State flip;
        const core::LinePick f = core::PickLine(flip, 2000, Table(), 0,
                                                0, false, 1, 0);
        assert(f.flipped && f.kind == 1 && f.line == 103);

        // No takes at all: no line, no RNG consumed, no last[] change.
        core::State none;
        none.rng = 12345;
        const uint64_t before = none.rng;
        const core::LinePick n = core::PickLine(none, 2000, Table(), 0,
                                                0, false, 2, 0);
        assert(n.line == 0 && none.rng == before);
        assert(none.body[0].last[0] == -1 && none.body[0].last[1] == -1);

        // Unknown fac / voice are never indexed into the private range table.
        assert(core::PickLine(none, 2000, Table(), 0, -1, false, 0, 0).line == 0);
        assert(core::PickLine(none, 2000, Table(), 0, 0, false, -1, 0).line == 0);
        assert(core::PickLine(none, 2000, Table(), 0, 0, false, 0, 2).line == 0);
    }

    // ---- first-active silence, busy, no-line, cadence, cut, poll ---------------

    void TestDownedCadence()
    {
        core::BodyInput b = Body(7);
        core::State s;

        core::Result r = core::Tick(s, Input(1000, &b, 1, Table()));
        assert(r.count == 0);
        assert(s.body[0].act && s.body[0].since == 1000 && s.body[0].crawl == false);
        uint64_t seed = 0;   // core seeds its zero state from `now`
        const uint32_t firstRand = core::NextRand(seed, 1000);
        assert(s.rng == seed);
        assert(s.body[0].next == 1000 + core::kFirstMs + firstRand % core::kFirstJitterMs);
        assert(s.body[0].next >= 1000 + core::kFirstMs);
        assert(s.body[0].next < 1000 + core::kFirstMs + core::kFirstJitterMs);

        const uint64_t due = s.body[0].next;
        assert(core::Tick(s, Input(due - core::kPollMs, &b, 1, Table())).count == 0);
        uint64_t rngCopy = s.rng;
        const uint32_t pickRand = core::NextRand(rngCopy, due);
        const uint32_t cadRand = core::NextRand(rngCopy, due);
        r = core::Tick(s, Input(due, &b, 1, Table()));
        assert(r.count == 1);
        assert(r.cues[0].kind == core::CueKind::kSpeak);
        assert(r.cues[0].ent == 7);
        assert(r.cues[0].line == 100 + pickRand % 2);
        assert(s.body[0].spoke_until == due + core::kSpeakHoldMs);
        assert(s.body[0].next == due + core::kLieCadenceMs + cadRand % core::kLieJitterMs);

        // A stop cue only while spoke_until > now and ent != 0, on the cut.
        core::BodyInput off = Body(7, false);
        r = core::Tick(s, Input(due + core::kSpeakHoldMs - 1, &off, 1, Table()));
        assert(r.count == 1 && r.cues[0].kind == core::CueKind::kStopSpeak
               && r.cues[0].ent == 7);
        assert(core::Tick(s, Input(due + core::kSpeakHoldMs, &off, 1, Table())).count == 0);
        assert(!s.body[0].act && s.body[0].ent == 0 && s.body[0].next == 0);
        assert(s.body[0].last[0] == -1 && s.body[0].last[1] == -1);

        core::State noEnt;
        noEnt.body[0].spoke_until = 5000;
        noEnt.body[0].ent = 0;
        noEnt.body[0].act = true;
        assert(core::Tick(noEnt, Input(4000, &off, 1, Table())).count == 0);
    }

    void TestCrawlBusyNoLineAndSecondGate()
    {
        // Crawl transition shortens next, never lengthens it.
        core::BodyInput b = Body(7);
        core::State s;
        core::Tick(s, Input(1000, &b, 1, Table()));
        const uint64_t oldNext = s.body[0].next;
        uint64_t rngCopy = s.rng;
        const uint32_t shift = core::NextRand(rngCopy, 1200);
        b.crawl = true;
        assert(core::Tick(s, Input(1200, &b, 1, Table())).count == 0);
        assert(s.body[0].crawl);
        assert(s.body[0].next == 1200 + core::kCrawlShiftMs + shift % core::kCrawlShiftJitter);
        assert(s.body[0].next < oldNext);

        // Busy check: 700 ms retry, no cue, no line selection.
        core::State busyState;
        core::BodyInput bb = Body(9);
        core::Tick(busyState, Input(1000, &bb, 1, Table()));
        busyState.body[0].next = 1000;
        bool yes[core::kBodies] = {};
        yes[0] = true;
        core::Result r = core::Tick(busyState, Input(3600, &bb, 1, Table(), yes));
        assert(r.count == 0);
        assert(busyState.body[0].next == 3600 + core::kBusyRetryMs);

        // No takes (unknown fac): 3000 ms retry and no RNG draw.
        core::State noLine;
        core::BodyInput unknown = Body(11, true, false, -1, -1);
        core::Tick(noLine, Input(1000, &unknown, 1, Table()));
        const uint64_t before = noLine.rng;
        noLine.body[0].next = 1000;
        r = core::Tick(noLine, Input(3600, &unknown, 1, Table()));
        assert(r.count == 0);
        assert(noLine.body[0].next == 3600 + core::kNoLineRetryMs);
        assert(noLine.rng == before);

        // The 2600 ms second gate holds even if next is forced early.
        core::State gate;
        core::BodyInput g = Body(13);
        core::Tick(gate, Input(1000, &g, 1, Table()));
        gate.body[0].next = 1000;
        assert(core::Tick(gate, Input(3550, &g, 1, Table())).count == 0);
        assert(core::Tick(gate, Input(3600, &g, 1, Table())).count == 1);

        // Crawling cadence is 2600 + rand(1600) and picks the breath take.
        core::State crawling;
        core::BodyInput cr = Body(15, true, true);
        core::Tick(crawling, Input(1000, &cr, 1, Table()));
        crawling.body[0].next = 1000;
        uint64_t crawlRng = crawling.rng;
        (void)core::NextRand(crawlRng, 3600);   // the single-take pick
        const uint32_t crawlCad = core::NextRand(crawlRng, 3600);
        r = core::Tick(crawling, Input(3600, &cr, 1, Table()));
        assert(r.count == 1 && r.cues[0].line == 102);
        assert(crawling.body[0].next == 3600 + core::kCrawlCadenceMs
               + crawlCad % core::kCrawlJitterMs);

        // 50 ms poll gate: the second sample inside the window is ignored.
        core::State poll;
        core::BodyInput one = Body(21);
        core::Tick(poll, Input(1000, &one, 1, Table()));
        one.ent = 22;
        core::Tick(poll, Input(1049, &one, 1, Table()));
        assert(poll.body[0].ent == 21);
        core::Tick(poll, Input(1050, &one, 1, Table()));
        assert(poll.body[0].ent == 22);
    }

    void TestNoData()
    {
        core::State s;
        s.rng = 12345;
        core::LineTable empty{};
        core::BodyInput b = Body(31);
        core::Result r = core::Tick(s, Input(1000, &b, 1, empty));
        assert(r.count == 0);
        assert(s.body[0].act && s.body[0].since == 1000);
        assert(s.body[0].next == 0);          // no first-silence RNG / cadence stamp
        assert(s.rng == 12345);
        b.crawl = true;
        r = core::Tick(s, Input(4000, &b, 1, empty));
        assert(r.count == 0 && s.rng == 12345 && s.body[0].next == 0);
        assert(s.body[0].spoke_until == 0);
    }

    void TestLocalBodySlot()
    {
        // The native owner fills peer rows 0..kMaxNetPlayers and the local body
        // at index 8; unused core rows stay inactive.
        core::BodyInput all[core::kBodies]{};
        all[core::kLocalBody] = Body(55);
        core::State s;
        const core::Result r = core::Tick(s, Input(1000, all, core::kBodies, Table()));
        assert(r.count == 0);
        assert(s.body[core::kLocalBody].act && s.body[core::kLocalBody].ent == 55);
        assert(!s.body[core::kLocalBody - 1].act);
    }

    // ---- headshot path ---------------------------------------------------------

    void TestHeadshots()
    {
        core::State s;

        // Mark expiry: recent until exactly 3000 ms, then gone.
        core::MarkHeadshot(s, 42, 1000);
        assert(core::RecentHeadshot(s, 42, 3999));
        assert(!core::RecentHeadshot(s, 42, 4000));
        core::MarkHeadshot(s, 42, 5000);
        assert(core::RecentHeadshot(s, 42, 5000));
        core::UnmarkHeadshot(s, 42);
        assert(!core::RecentHeadshot(s, 42, 5001));
        core::MarkHeadshot(s, 0, 1000);
        core::MarkHeadshot(s, ~0ULL, 1000);
        assert(!core::Active(s, 1000));

        // Marks dedupe and the 8th-row overwrite.
        core::State marks;
        core::MarkHeadshot(marks, 7, 100);
        core::MarkHeadshot(marks, 7, 600);
        int rows = 0;
        for (int i = 0; i < core::kMarkRows; ++i)
            if (marks.mark[i].ent) { ++rows; assert(marks.mark[i].at == 600); }
        assert(rows == 1);
        for (uint64_t i = 1; i <= core::kMarkRows; ++i)
            core::MarkHeadshot(marks, 100 + i, 1000);
        rows = 0;
        for (int i = 0; i < core::kMarkRows; ++i)
            if (marks.mark[i].ent) ++rows;
        assert(rows == core::kMarkRows);

        // Queue dedupe: force upgrades but does not move due.
        core::State q;
        core::ArmHeadshot(q, 77, false, 500, 1000);
        assert(q.queue[0].ent == 77 && q.queue[0].due == 1500 && !q.queue[0].force);
        core::ArmHeadshot(q, 77, true, 9000, 1100);
        assert(q.queue[0].due == 1500 && q.queue[0].force);
        core::ArmHeadshot(q, 0, true, 0, 1000);
        core::ArmHeadshot(q, ~0ULL, true, 0, 1000);

        // The headshot cues are emitted with NO private table at all.
        assert(core::Tick(q, Input(1499, nullptr, 0, core::LineTable{})).count == 0);
        core::Result r = core::Tick(q, Input(1500, nullptr, 0, core::LineTable{}));
        assert(r.count == 2);
        assert(r.cues[0].kind == core::CueKind::kStopSpeak && r.cues[0].ent == 77);
        assert(r.cues[1].kind == core::CueKind::kGargle && r.cues[1].ent == 77);
        assert(r.cues[1].index == 0 || r.cues[1].index == 1);
        assert(q.queue[0].ent == 0);                       // consumed once
        assert(core::Tick(q, Input(1501, nullptr, 0, core::LineTable{})).count == 0);

        // Without force, a fresh mark is required; marks expire at 3000 ms.
        core::State recent;
        core::MarkHeadshot(recent, 88, 1000);
        core::ArmHeadshot(recent, 88, false, 100, 1000);
        assert(core::Tick(recent, Input(1100, nullptr, 0, core::LineTable{})).count == 2);
        core::State stale;
        core::MarkHeadshot(stale, 88, 0);
        core::ArmHeadshot(stale, 88, false, 2000, 1000);
        assert(core::Tick(stale, Input(3000, nullptr, 0, core::LineTable{})).count == 0);

        // Not live / switched off: the row is still consumed, no cue is emitted.
        core::State gated;
        core::ArmHeadshot(gated, 99, true, 0, 1000);
        core::TickInput off = Input(1000, nullptr, 0, core::LineTable{});
        off.matchLive = false;
        assert(core::Tick(gated, off).count == 0 && gated.queue[0].ent == 0);
        core::ArmHeadshot(gated, 99, true, 0, 1000);
        off.now = 1000;
        off.matchLive = true;
        off.headshotsOff = true;
        assert(core::Tick(gated, off).count == 0 && gated.queue[0].ent == 0);

        // Six queue rows; a seventh distinct entity is dropped.
        core::State full;
        for (uint64_t i = 0; i < core::kQueueRows; ++i)
            core::ArmHeadshot(full, 200 + i, true, 0, 1000);
        core::ArmHeadshot(full, 999, true, 0, 1000);
        int queued = 0;
        for (int i = 0; i < core::kQueueRows; ++i)
            if (full.queue[i].ent) ++queued;
        assert(queued == core::kQueueRows);
        assert(core::Active(full, 1000));
        core::Reset(full);
        assert(!core::Active(full, 1000));
        for (int i = 0; i < core::kQueueRows; ++i) assert(full.queue[i].ent == 0);
        for (int i = 0; i < core::kMarkRows; ++i) assert(full.mark[i].ent == 0);
        assert(full.rng == 0 && full.lastPoll == 0 && !full.body[0].act);
    }

} // namespace

int main()
{
    InitRange();
    TestRngAndPick();
    TestDownedCadence();
    TestCrawlBusyNoLineAndSecondGate();
    TestNoData();
    TestLocalBodySlot();
    TestHeadshots();
    std::printf("DOWNED_VOX_CORE_TEST_OK\n");
    return 0;
}
