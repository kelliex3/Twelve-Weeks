// Standalone test for the pure interrogation-voice rules; no engine, game,
// audio or network headers. The BEG_INT table is a small synthetic fixture.
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src
//       mod/tests/intvox_core_test.cpp -o /tmp/opencode/intvox_check
//   /tmp/opencode/intvox_check

#include "net/audio/intvox-core.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

    using namespace net::audio::intvox::core;

    struct Fixture {
        uint64_t lines[6] = { 1, 2, 3, 4, 5, 6 };
        uint16_t range[2 * 2 * 4 * 7 * 2] = {};
        uint16_t dur[6] = { 800, 1000, 900, 700, 950, 850 };
        uint8_t  flag[6] = { 0, 0, 0, 1, 0, 0 };
        uint32_t src[6] = { 10, 11, 12, 13, 14, 15 };
        uint32_t txt[6] = { 100, 101, 102, 103, 104, 105 };
        uint8_t  avail[6] = { 1, 1, 1, 1, 1, 1 };
        VoxTable t;

        Fixture()
        {
            Put(0, 0, 0, 8, 0, 2);
            Put(0, 0, 0, 9, 2, 2);  Put(0, 0, 0, 10, 4, 1);
            Put(0, 0, 0, 11, 5, 1); Put(0, 0, 0, 13, 0, 2);
            Put(0, 0, 0, 14, 2, 2);
            t = { lines, range, dur, flag, src, txt, avail, 6 };
        }

        void Put(int fac, int fem, int voice, int kind, int first, int count)
        {
            const int i = ((((fac * 2 + fem) * 4 + voice) * 7 + (kind - 8)) * 2);
            range[i] = static_cast<uint16_t>(first);
            range[i + 1] = static_cast<uint16_t>(count);
        }
    };

    BeginIn FistIn(uint32_t uid = 7)
    {
        BeginIn in;
        in.t0 = 1000.0;
        in.killMs = 5000.0;
        in.uid = uid;
        in.pair = 44;
        in.attSlot = 1;
        in.vicSlot = 2;
        in.fist = true;
        in.ok[0] = in.ok[1] = true;
        in.fac[0] = in.fac[1] = 0;
        in.female[0] = in.female[1] = 0;
        in.voice[0] = in.voice[1] = 0;
        return in;
    }

    int FindCue(const Result& r, CueKind kind, int who = -1)
    {
        for (int i = 0; i < r.count; ++i)
            if (r.cues[i].kind == kind && (who < 0 || r.cues[i].who == who))
                return i;
        return -1;
    }

    void TestPickHash()
    {
        const uint32_t a = PickHash(1234, 9, 9);
        assert(a == PickHash(1234, 9, 9));
        assert(a != PickHash(1235, 9, 9));
        assert(a != PickHash(1234, 10, 9));
    }

    void TestPickHistoryCapAndFlag()
    {
        Fixture f;
        State s;
        s.ok[0] = s.ok[1] = true;
        s.fac[0] = s.fac[1] = 0;
        s.female[0] = s.female[1] = 0;
        s.voice[0] = s.voice[1] = 0;
        s.seed = 0x12345678u;

        const int r9 = Pick(s, f.t, 1, 9, 9);
        assert(r9 >= 2 && r9 < 4);
        assert(HistHas(s, 0, f.t.Txt(r9)));
        // The same beat never repeats while the other take is available.
        const int again = Pick(s, f.t, 1, 9, 31);
        assert(again >= 2 && again < 4);
        assert(again != r9);

        // The duration cap drops the long take, and unpick / repick put the
        // history and used-source marks back exactly once.
        const int capped = PickFit(s, f.t, 0, 8, 8, 850);
        assert(capped == 0);
        Unpick(s, f.t, capped);
        assert(!HistHas(s, HistSlot(8), f.t.Txt(capped)));
        Repick(s, f.t, capped);

        // "Fuck you!" is picked once per interrogation.
        s.fuck = false;
        const int fu = Pick(s, f.t, 1, 9, 5);
        if (fu == 3) {
            assert(s.fuck);
            assert(Pick(s, f.t, 1, 9, 6) != 3);
        } else {
            s.fuck = true;
            assert(Pick(s, f.t, 1, 9, 5) != 3);
        }
        assert(Pick(s, f.t, 1, 16, 1) == -1);
    }

    void TestTimelineAndLineFeed()
    {
        Fixture f;
        State s;
        Begin(s, f.t, FistIn());
        assert(s.on == 1 && s.nstep >= 7 && s.nstep <= kMaxSteps);
        for (int i = 1; i < s.nstep; ++i)
            assert(s.st[i - 1].at <= s.st[i].at);
        assert(s.st[0].at == 450.0 && s.st[0].kind == 8);
        bool sawImpact = false, sawFinal = false, sawKillEffort = false;
        for (int i = 0; i < s.nstep; ++i) {
            sawImpact = sawImpact || (s.st[i].kind == kSlamHit && s.st[i].rec == -1);
            sawFinal = sawFinal || (s.st[i].kind == kSlamHit && s.st[i].rec == -2);
            sawKillEffort = sawKillEffort ||
                (s.st[i].kind == kSlamTake && s.st[i].at > 5000.0);
        }
        assert(sawImpact && sawFinal && sawKillEffort);

        Result r;
        TickIn tick;
        tick.ems = 0.0;
        tick.t0 = s.t0;
        tick.now = 1000;
        Tick(s, r, f.t, tick);
        assert(r.count == 0);
        assert(r.line.on && r.line.t0 == s.t0 && r.line.count > 0);
        assert(r.quietUntilSet && r.quietUntil == 5000);

        tick.ems = 500.0;
        Tick(s, r, f.t, tick);
        assert(FindCue(r, CueKind::kSayRecord, 0) >= 0 || FindCue(r, CueKind::kSayEffort, 0) >= 0);
        assert(s.step > 0);
        assert(r.line.busy[0] > 500.0);
    }

    void TestEffortFallbackAndCheck()
    {
        Fixture f;
        BeginIn in = FistIn(9);
        in.ok[0] = in.ok[1] = false;       // no voice set: every beat is an effort
        State s;
        Begin(s, f.t, in);
        Result r;
        TickIn tick;
        tick.ems = 500.0;
        tick.t0 = s.t0;
        tick.now = 1000;
        Tick(s, r, f.t, tick);
        assert(FindCue(r, CueKind::kSayEffort, 0) >= 0);
        assert(FindCue(r, CueKind::kSayRecord) < 0);

        // A refused record retries once at the SP effort priority, then a
        // first demand latches this interrogation over to effort events; a
        // late line re-speaks as an effort without latching.
        State c;
        c.on = 1;
        c.ok[0] = true;
        c.fac[0] = 0;
        c.female[0] = 0;
        c.voice[0] = 0;
        Result cr;
        Speak(c, cr, f.t, 0, 8, 0, 450.0, 1000);
        assert(FindCue(cr, CueKind::kSayRecord, 0) >= 0);
        assert(CheckDue(c, 1140));
        TickIn check;
        check.ems = 600.0;
        check.t0 = c.t0;
        check.now = 1140;
        check.checkDue = true;
        Result retry;
        Tick(c, retry, f.t, check);
        assert(FindCue(retry, CueKind::kSayRecord, 0) >= 0);
        int ri = FindCue(retry, CueKind::kSayRecord, 0);
        assert(retry.cues[ri].priority == 26);
        c.checkAt = 1140;
        Result late;
        Tick(c, late, f.t, check);
        assert(c.refused);
        assert(FindCue(late, CueKind::kSayEffort, 0) >= 0);
    }

    void TestCutAndShiv()
    {
        Fixture f;
        State s;
        Begin(s, f.t, FistIn());
        s.spoke[1] = true;
        s.ent[1] = 1234;
        Result r;
        Cut(s, r, "test cut");
        assert(!s.on && s.on != 1);
        assert(FindCue(r, CueKind::kStopVictim, 1) >= 0);
        assert(!r.line.on && r.lineClear && r.lookClear && r.end);

        BeginIn shiv = FistIn(11);
        shiv.fist = false;
        shiv.killMs = 2250.0;
        State ss;
        Begin(ss, f.t, shiv);
        assert(ss.nstep == 2);
        assert(ss.st[0].at == 350.0 && ss.st[0].kind == 13);
        assert(ss.st[1].at >= 700.0 && ss.st[1].kind == 14);

        Fixture empty;
        for (int i = 0; i < 6; ++i)
            empty.avail[i] = 0;
        State none;
        Begin(none, empty.t, shiv);
        Result fallback;
        TickIn tick;
        tick.ems = 400.0;
        tick.t0 = none.t0;
        tick.now = 1000;
        Tick(none, fallback, empty.t, tick);
        assert(FindCue(fallback, CueKind::kSayEffort, 0) >= 0);
    }

    void TestIntwarp()
    {
        assert(std::fabs(WarpExtraMs() - 1632.257) < 0.01);
        assert(ClipToRealMs(0.0) == 0.0);
        assert(std::fabs(ClipToRealMs(kClipMs) - (kClipMs + WarpExtraMs())) < 0.001);
        assert(ClipToRealMs(1000.0) > 1000.0);
        assert(ClipToRealMs(4000.0) > ClipToRealMs(2000.0));
    }

} // namespace

int main()
{
    TestPickHash();
    TestPickHistoryCapAndFlag();
    TestTimelineAndLineFeed();
    TestEffortFallbackAndCheck();
    TestCutAndShiv();
    TestIntwarp();
    std::puts("intvox core: ok");
    return 0;
}
