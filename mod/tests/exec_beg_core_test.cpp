// Standalone test for the pure exec_beg rules; no engine / game headers.
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src -I mod/src/net/audio
//       mod/tests/exec_beg_core_test.cpp -o /tmp/opencode/exec_beg_check
//   /tmp/opencode/exec_beg_check

#include "exec-beg-core.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

    using namespace net::audio::exec_beg::core;

    constexpr int RangeIndex(int fac, int female, int voice, int kind, int field)
    {
        return (((fac * 2 + female) * 4 + voice) * 8 + kind) * 2 + field;
    }

    struct Fixture {
        uint64_t lines[10] = { 100, 101, 102, 103, 104, 105, 106, 107, 108, 109 };
        int      dur[10]   = { 260, 700, 500, 900, 2400, 300, 800, 1200, 420, 600 };
        uint8_t  scream[10] = { 0, 1, 0, 0, 1, 0, 0, 0, 0, 0 };
        uint8_t  avail[10]  = { 1, 1, 1, 0, 1, 1, 1, 1, 1, 1 };
        int      range[2 * 2 * 4 * 8 * 2] = {};

        Fixture()
        {
            auto set = [this](int fac, int female, int voice, int kind,
                              int first, int count) {
                range[RangeIndex(fac, female, voice, kind, 0)] = first;
                range[RangeIndex(fac, female, voice, kind, 1)] = count;
            };
            set(0, 0, 0, 0, 0, 4);   // plea, one long shout and one unavailable
            set(0, 0, 0, 1, 4, 2);   // pistol grunts
            set(0, 0, 0, 2, 8, 2);   // rifle grunts
            set(0, 0, 0, 5, 6, 2);   // short screams
        }

        VoxTable Table() const noexcept
        {
            return { lines, range, dur, scream, avail, 10 };
        }
    };

    void TestConstantsAndHash()
    {
        assert(kPleaSnap == 0x334b0a906e6eb50eULL);
        assert(kLeadMs == 800 && kCheckMs == 140 && kMaxCoverMs == 8000);
        assert(kMusicDuck == 0.35f);
        assert(kMeleeMaxDur == 1100 && kFinisherMaxDur == 800);
        assert(kScreamSteps == 12);
        assert(ScreamSequence(0) == 5 && ScreamSequence(3) == 5
            && ScreamSequence(4) == 4 && ScreamSequence(11) == 7
            && ScreamSequence(12) == 7 && ScreamSequence(13) == 6);
        assert(Fallback(0, 0) == 0 && Fallback(1, 2) == 3
            && Fallback(4, 4) == -1);
        assert(PickHash(0x1234, 0, 0) == 1618596808u);
        assert(PickHash(0x1234, 1, 0) == 3515090187u);
        assert(PickHash(0x1234, 0, 100) == 3972564330u);
        assert(PickHash(7, 2, 107) == 1360385080u);
    }

    void TestPick()
    {
        const Fixture f;
        const VoxTable t = f.Table();

        // Kind 0: rec 1 is a long shout, rec 3 has no stream in the language.
        assert(Pick(t, 0x1234, 0, 0, 0, 0, -1, 0, false, 0) == 0);
        assert(Pick(t, 0x1234, 0, 0, 0, 0, -1, 0, true, 0) == 1);
        assert(Pick(t, 0x1234, 0, 0, 0, 0, 0, 0, false, 0) == 2);  // avoid advances
        assert(Pick(t, 0x1234, 0, 0, 0, 0, -1, 0, false, 300) == 0); // 500 ms dropped
        // Non-verbal hit grunts ignore the language table and the duration cap.
        assert(Pick(t, 0x1234, 0, 0, 0, 1, -1, 100, false, 100) == 5);
        assert(Pick(t, 0x1234, 0, 0, 0, 2, -1, 100, false, 0) == 9);
        // An empty kind falls through FB to the short scream range.
        assert(Pick(t, 0x1234, 0, 0, 0, 4, -1, 0, false, 0) == 6);

        // Finisher gasp: no scream, short takes first, stream required.
        assert(FinGaspPick(t, 0, 0, 0, 0x1234) == 2);
        assert(FinGaspPick(t, -1, 0, 0, 0x1234) == -1);

        // No private table: no record can be picked and no wheel is consumed.
        assert(Pick(VoxTable{}, 0x1234, -1, -1, -1, 0, -1, 0, false, 0) == -1);
    }

    State MakeGun(const VoxTable& t, bool lastHead = true, int limb = -1)
    {
        const uint16_t shots[3] = { 1000, 1600, 2300 };
        GunBegin in;
        in.ent = 0xAA;
        in.pair = 3;
        in.fac = 0;
        in.female = 0;
        in.voice = 0;
        in.seed = 0x1234;
        in.moveMs = 0.0;
        in.weaponClass = 2;      // rifle
        in.shots = shots;
        in.shotCount = 3;
        in.limb = limb;
        in.lastHead = lastHead;
        State s;
        assert(BeginGun(s, t, in, false, false, false));
        return s;
    }

    TickInput Input(uint64_t now, double ems, const VoxTable& t,
                    bool matchLive = true, bool musicLive = true)
    {
        TickInput in;
        in.nowMs = now;
        in.moveMs = ems;
        in.matchLive = matchLive;
        in.musicLive = musicLive;
        in.table = t;
        return in;
    }

    int Count(const Result& r, CueKind k)
    {
        int n = 0;
        for (int i = 0; i < r.count; ++i)
            if (r.cues[i].kind == k)
                ++n;
        return n;
    }

    const Cue* Find(const Result& r, CueKind k)
    {
        for (int i = 0; i < r.count; ++i)
            if (r.cues[i].kind == k)
                return &r.cues[i];
        return nullptr;
    }

    void TestGunTimeline()
    {
        const Fixture f;
        const VoxTable t = f.Table();
        State s = MakeGun(t);
        // lead = 260 * 0.8 = 208 -> the plea starts at 792 ms.
        assert(s.begAt == 792 && s.nshot == 3 && s.pkind == 2);
        assert(s.t0 == 0.0);

        Result r = Tick(s, Input(100, 790, t));
        assert(Count(r, CueKind::kSpeakRecord) == 0);
        assert(r.duckChanged && r.duckWant);            // beg - 100 = 692

        r = Tick(s, Input(200, 800, t));
        const Cue* plea = Find(r, CueKind::kSpeakRecord);
        assert(plea && plea->line == 100 && plea->voiceKind == 0);
        assert(s.checkAt == 340 && !s.checkEv);
        CommitSpeak(s, true);

        // Shot 1 cuts the plea and starts a rifle grunt (rec 9).
        r = Tick(s, Input(400, 1000, t));
        assert(Count(r, CueKind::kStop) == 1);
        const Cue* grunt = Find(r, CueKind::kSpeakRecord);
        assert(grunt && grunt->line == 109 && grunt->voiceKind == 2);
        CommitSpeak(s, true);

        // Shot 2: another cut and the other rifle grunt (rec 8).
        r = Tick(s, Input(500, 1600, t));
        assert(Count(r, CueKind::kStop) == 1);
        grunt = Find(r, CueKind::kSpeakRecord);
        assert(grunt && grunt->line == 108);
        CommitSpeak(s, true);

        // Death shot: cut only, plus the headshot death take.
        r = Tick(s, Input(600, 2300, t));
        assert(Count(r, CueKind::kStop) == 1);
        assert(Count(r, CueKind::kHeadshot) == 1);
        assert(Count(r, CueKind::kSpeakRecord) == 0);

        r = Tick(s, Input(700, 2650, t));
        assert(!r.duckChanged);
        r = Tick(s, Input(800, 2651, t));
        assert(r.duckChanged && !r.duckWant);
        assert(s.on);                                  // cover continues
    }

    void TestClampsAndStops()
    {
        const Fixture f;
        const VoxTable t = f.Table();

        // A long take is clamped to 800 ms and the plea never starts before
        // 80 ms into the move.
        const uint16_t shots[1] = { 200 };
        GunBegin in;
        in.ent = 0xBB;
        in.fac = 0; in.female = 0; in.voice = 0;
        in.seed = 0x1234;
        in.shots = shots; in.shotCount = 1;
        State s;
        assert(BeginGun(s, t, in, false, false, false));
        assert(s.begAt == 80);

        // Match exit and the 8 s cover both cut the voice.
        Result r = Tick(s, Input(100, 100, t, false));
        assert(r.cutWhy && strcmp(r.cutWhy, "match left the playing state") == 0);
        assert(!s.on);

        s = MakeGun(t, false);
        CommitSpeak(s, true);
        r = Tick(s, Input(200, 8001, t));
        assert(Count(r, CueKind::kStop) == 1 && !s.on);

        // A duck is restored when the state is cut.
        s = MakeGun(t, false);
        r = Tick(s, Input(300, 800, t));
        assert(r.duckChanged && r.duckWant);
        Cut(s, r, "aborted");
        assert(r.duckChanged && !r.duckWant && !s.on);

        // Without a live music cue the duck is forgotten, never applied.
        s = MakeGun(t, false);
        r = Tick(s, Input(400, 800, t, true, false));
        assert(!r.duckChanged && !s.ducked);
    }

    void TestCheckFallback()
    {
        const Fixture f;
        const VoxTable t = f.Table();
        State s = MakeGun(t, false);
        Result r = Tick(s, Input(100, 800, t, true, false));
        assert(Count(r, CueKind::kSpeakRecord) == 1);
        CommitSpeak(s, true);
        assert(CheckDue(s, 240) && !CheckDue(s, 239));

        r = Tick(s, Input(239, 800, t, true, false));
        assert(!s.events && Count(r, CueKind::kSpeakEvent) == 0);

        // An explicit "no" switches the session to effort events from now on.
        TickInput in = Input(240, 800, t, true, false);
        in.checkReply = 0;
        r = Tick(s, in);
        assert(s.events && Count(r, CueKind::kSpeakEvent) == 1);
        assert(Find(r, CueKind::kSpeakEvent)->voiceKind == 0);
        CommitSpeak(s, true);

        r = Tick(s, Input(600, 1000, t, true, false));
        assert(Count(r, CueKind::kSpeakEvent) == 1);
        assert(Count(r, CueKind::kSpeakRecord) == 0);
    }

    void TestMeleeAndScream()
    {
        const Fixture f;
        const VoxTable t = f.Table();

        MeleeBegin mb;
        mb.ent = 0xCC;
        mb.pair = 1;
        mb.seed = 0x1234;
        mb.moveMs = 0.0;
        State s;
        assert(BeginMelee(s, mb, false, false, false, true)); // no mapping: events live
        Result r;
        BeginMeleePlea(s, r, t, 100, 0.0, 500.0, kMeleePleaSalt);
        assert(Count(r, CueKind::kSpeakEvent) == 1);
        assert(!DuckWanted(s, 0.0));           // ext ducks only gun executions
        assert(BoostWanted(s, 0.0, false, true));
        assert(!BoostWanted(s, 0.0, true, true));
        assert(!BoostWanted(s, 0.0, false, false));
        CommitSpeak(s, true);
        r = Tick(s, Input(200, 600, t));
        assert(Count(r, CueKind::kSpeakRecord) == 0); // the EST_BEG gate stays shut
        Cut(s, r, "the blow cuts the plea");
        assert(Count(r, CueKind::kStop) == 1 && !s.on);

        // Dismember scream: the limb shot starts the chain; the death shot ends it.
        s = MakeGun(t, true, 0);
        r = Tick(s, Input(250, 800, t, true, false));
        CommitSpeak(s, true);
        r = Tick(s, Input(300, 1000, t, true, false));
        assert(Count(r, CueKind::kStop) == 1);
        assert(Count(r, CueKind::kSpeakRecord) == 1); // the scream takes the shot's place
        assert(s.scrOn && s.scrStep == 1 && s.scrN == 1);
        assert(s.scrNext > 2115.0 && s.scrNext < 2117.0); // 1000 + 1200*0.93
        r = Tick(s, Input(400, 2120, t, true, false));
        assert(s.scrStep == 2 && s.scrN == 2);
        r = Tick(s, Input(500, 2300, t, true, false));
        assert(!s.scrOn && Count(r, CueKind::kHeadshot) == 1);
    }

    void TestScheduledMeleePleas()
    {
        const Fixture f;
        const VoxTable t = f.Table();
        MeleeBegin mb;
        mb.ent = 0xDD;
        mb.pair = 0;
        mb.seed = 0x1234;
        mb.moveMs = 0.0;
        State s;
        assert(BeginMelee(s, mb, false, false, false, true)); // events: no private table
        Result r;
        assert(ScheduleMeleePlea(s, r, t, 0.0, 1200.0, 1500.0, kMeleePleaSalt));
        assert(s.nplea == 1 && s.plea[0].start == 1200.0 && s.plea[0].cut == 1500.0);
        assert(Count(r, CueKind::kSpeakEvent) == 0); // scheduled, not spoken yet
        assert(!CoversGun(s, 0.0));                 // the window starts when it speaks

        r = Tick(s, Input(100, 1199, t));
        assert(Count(r, CueKind::kSpeakEvent) == 0);
        r = Tick(s, Input(200, 1201, t));
        assert(Count(r, CueKind::kSpeakEvent) == 1 && s.plea[0].said);
        assert(CoversGun(s, 1100.0) && !CoversGun(s, 1600.0));
        CommitSpeak(s, true);
        r = Tick(s, Input(300, 1501, t));
        assert(Count(r, CueKind::kStop) == 1 && s.plea[0].stopped);

        // A second EST_BEG row follows the same clock and cut rule.
        assert(ScheduleMeleePlea(s, r, t, 1501.0, 2500.0, 2933.0, 7u + 13u));
        r = Tick(s, Input(400, 2501, t));
        assert(Count(r, CueKind::kSpeakEvent) == 1);
        CommitSpeak(s, true);
        r = Tick(s, Input(500, 2934, t));
        assert(Count(r, CueKind::kStop) == 1);

        // A late start drops the row without speaking it; no stale stop either.
        assert(ScheduleMeleePlea(s, r, t, 2934.0, 3300.0, 3500.0, 33u));
        r = Tick(s, Input(600, 3700, t));
        assert(Count(r, CueKind::kSpeakEvent) == 0 && Count(r, CueKind::kStop) == 0);
        assert(s.plea[2].said && s.plea[2].stopped);

        ClearMeleePleas(s);
        assert(s.nplea == 0 && s.plea[0].rec == -1);
    }

} // namespace

int main()
{
    TestConstantsAndHash();
    TestPick();
    TestGunTimeline();
    TestClampsAndStops();
    TestCheckFallback();
    TestMeleeAndScream();
    TestScheduledMeleePleas();
    std::puts("EXEC_BEG_CORE_TEST_OK");
    return 0;
}
