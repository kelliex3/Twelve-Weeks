// Standalone test for the pure execsp rules; no engine / game headers.
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src -I mod/src/net/audio
//       mod/tests/exec_sp_core_test.cpp -o /tmp/opencode/exec_sp_check
//   /tmp/opencode/exec_sp_check

#include "exec-sp-core.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

    using namespace net::audio::exec_sp::core;

    int Count(const Result& r, CueKind k)
    {
        int n = 0;
        for (int i = 0; i < r.count; ++i)
            if (r.cues[i].kind == k)
                ++n;
        return n;
    }

    void TestRows()
    {
        assert(kRowCount > 60 && kRows[0].pair && !strcmp(kRows[0].pair, "pistol-execute-left"));
        assert(HasPleas("fist-execute-enter-front")
            && HasPleas("fist-stomp-execute-left")
            && !HasPleas("pipe-execute")
            && !HasPleas("no-such-pair"));
        assert(IsUnarmedName("fist-execute-enter-front")
            && IsUnarmedName("fist-stomp-execute-left")
            && IsUnarmedName("fist-execute-neckbreak-fastest-right")
            && !IsUnarmedName("pipe-execute")
            && !IsUnarmedName("machete-execute-right"));
        assert(NeedBluntWeapon("pipe-execute") && NeedBluntWeapon("pipe-execute-left")
            && !NeedBluntWeapon("pipe-bat"));
        assert(OwnsVictimVoice("fist-execute-enter-front", 1)
            && OwnsVictimVoice("fist-stomp-execute-right", 1)
            && OwnsVictimVoice("pipe-execute", 1)
            && !OwnsVictimVoice("pistol-execute-left", 1)
            && !OwnsVictimVoice("fist-execute-enter-front", 0));
    }

    void TestPleaSchedules()
    {
        PleaRow rows[kMaxRowsPerPair];
        const int n = Pleas("fist-execute-enter-front", rows);
        assert(n == 2);
        assert(rows[0].earliestMs == 720 && rows[0].cutMs == 1300 && rows[0].salt == 7);
        assert(rows[1].earliestMs == 2150 && rows[1].cutMs == 2933 && rows[1].salt == 20);

        const int m = Pleas("fist-stomp-execute-left", rows);
        assert(m == 1 && rows[0].earliestMs == 1450 && rows[0].cutMs == 2233
            && rows[0].salt == 7);
        assert(Pleas("execution-revolver-left", rows) == 0);
    }

    void TestTimeline()
    {
        State s;
        assert(Begin(s, 1, 3, "fist-execute-enter-front", 100.0, false, true, 9));
        assert(s.on && s.lo >= 0 && s.hi - s.lo == 11 && s.t0 == 100.0 && s.uid == 9);
        assert(s.kind == 1 && s.pair == 3 && s.ownVic && Matches(s, 1, 3, 9));

        TickInput in;
        in.attEnt = 0xAA;
        in.vicEnt = 0xBB;

        in.moveMs = 360.0;     // +260 ms into the move: the grabbed yelp
        Result r = Tick(s, in);
        assert(r.count == 1 && r.cues[0].kind == CueKind::kVox
            && r.cues[0].ent == 0xBB && r.cues[0].voice == static_cast<int>(Vox::kGrabbed));

        in.moveMs = 1231.0;    // wind-up effort and the right-wrist swing
        r = Tick(s, in);
        assert(Count(r, CueKind::kVox) == 1 && Count(r, CueKind::kSfx) == 1);
        for (int i = 0; i < r.count; ++i) {
            if (r.cues[i].kind == CueKind::kSfx) {
                assert(!strcmp(r.cues[i].name, "sfx-gear-swish-cotton"));
                assert(!strcmp(r.cues[i].joint, "r_wrist") && r.cues[i].ent == 0xAA);
            }
            else {
                assert(r.cues[i].voice == static_cast<int>(Vox::kGiveMed));
                assert(r.cues[i].ent == 0xAA);
            }
        }

        // EST_BEG rows are owned by exec_beg, never emitted twice here.
        in.moveMs = 1401.0;
        r = Tick(s, in);
        assert(r.count == 0);

        in.moveMs = 2741.0;    // finisher whoosh, one frame before the effort
        r = Tick(s, in);
        assert(Count(r, CueKind::kSfx) == 1 && Count(r, CueKind::kVox) == 0);
        assert(!strcmp(r.cues[0].name, "sfx-unarmed-finisher-whoosh"));

        in.moveMs = 2801.0;
        r = Tick(s, in);
        assert(Count(r, CueKind::kVox) == 1
            && r.cues[0].voice == static_cast<int>(Vox::kGiveHvy));

        // A late join never replays the rows it missed.
        State late;
        assert(Begin(late, 1, 0, "pipe-execute", 0.0, false, true, 0));
        in.moveMs = 3000.0;
        r = Tick(late, in);
        assert(r.count == 0 && late.skipped > 0 && late.played == 0);
    }

    void TestContacts()
    {
        Contact c;
        assert(ContactFor(ContactLabel::kHitFistFace, "fist-execute-enter-front", 1300.0, c));
        assert(!strcmp(c.ev, "sfx-unarmed-hit-face-skin") && !strcmp(c.joint, "heada"));
        assert(ContactFor(ContactLabel::kHitFistFaceFinisher, "fist-execute-enter-front", 2933.0, c));
        assert(!strcmp(c.ev, "sfx-unarmed-finisher-face-skin"));
        assert(ContactFor(ContactLabel::kHitFistFaceFinisher, "fist-execute-enter-front", 1833.0, c));
        assert(!strcmp(c.ev, "sfx-unarmed-hit-face-skin"));
        assert(ContactFor(ContactLabel::kGutKick, "fist-stomp-execute-left", 633.0, c));
        assert(!strcmp(c.ev, "sfx-unarmed-hit-gut") && !strcmp(c.joint, "pelvis"));
        assert(!ContactFor(ContactLabel::kGutKick, "fist-execute-enter-front", 633.0, c));
        assert(ContactFor(ContactLabel::kHitBootHead, "fist-stomp-execute-left", 2233.0, c));
        assert(!strcmp(c.ev, "sfx-unarmed-hit-kick-face-skin"));
        assert(ContactFor(ContactLabel::kBreakNeck, "fist-execute-neckbreak-fast-left", 1500.0, c));
        assert(!strcmp(c.ev, "sfx-unarmed-finisher-neckbreak-combat") && !strcmp(c.joint, "headb"));
        assert(!ContactFor(ContactLabel::kBreakNeck, "pipe-execute", 1500.0, c));
    }

    void TestGates()
    {
        State s;
        assert(!Begin(s, 1, 0, "pipe-execute", 0.0, false, false, 0));
        assert(Begin(s, 1, 0, "pipe-execute", 0.0, false, true, 0));
        assert(!Begin(s, 1, 0, "fist-execute-enter-front", 0.0, true, true, 0));
        assert(!Begin(s, 1, 0, "no-such-pair", 0.0, false, true, 0));
        // Gun rows are reachable through the (currently unproduced) feed.
        assert(Begin(s, 0, 0, "pistol-execute-left", 0.0, false, true, 0));
        TickInput in;
        in.moveMs = 1000.0;
        in.vicEnt = 0xBB;
        Result r = Tick(s, in);
        assert(Count(r, CueKind::kVox) == 1);
        in.gunVoiceCovered = true;
        State s2;
        assert(Begin(s2, 0, 0, "pistol-execute-left", 0.0, false, true, 0));
        r = Tick(s2, in);
        assert(Count(r, CueKind::kVox) == 0);

        // The intwarp feed shifts only the matching clock.
        const double t0 = s2.t0;
        Shift(s2, t0 - 1.0, 300.0);
        assert(s2.t0 == t0);
        Shift(s2, t0, 300.0);
        assert(s2.t0 == t0 + 300.0);
        Cut(s2);
        assert(!s2.on && !Matches(s2, 0, 0, 0));
    }

} // namespace

int main()
{
    TestRows();
    TestPleaSchedules();
    TestTimeline();
    TestContacts();
    TestGates();
    std::puts("EXEC_SP_CORE_TEST_OK");
    return 0;
}
