// Standalone harness for the pure melee timing cores (no game headers / no Windows):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/gameplay
//       mod/tests/melee_timing_core_test.cpp mod/src/gameplay/melee-tell.cpp
//       mod/src/gameplay/melee-duel.cpp mod/src/gameplay/melee-dodge.cpp
//       -o /tmp/melee_timing_core_test && /tmp/melee_timing_core_test

#include "melee-dodge.hpp"
#include "melee-duel.hpp"
#include "melee-tell.hpp"

#include <cassert>
#include <cstdio>

namespace {

    class ReplaySink final : public gameplay::melee_dodge::Sink {
    public:
        void SendSwing(uint8_t) override {}
        void SendPress(uint8_t, uint8_t, uint8_t, uint64_t) override {}
        void SendResult(uint8_t) override {}
        void Log(const char*) override {}
        void ReplayBlow(const gameplay::melee_dodge::HeldBlow& h) override
        {
            ++replays;
            last = h;
        }
        int replays = 0;
        gameplay::melee_dodge::HeldBlow last;
    };

    gameplay::melee_dodge::HeldBlow Blow(int slot)
    {
        gameplay::melee_dodge::HeldBlow b;
        b.slot = slot;
        b.me = 100;
        b.attacker = 200;
        b.weapon = 300;
        b.damage = 50.0f;
        return b;
    }

    void TestTell()
    {
        using namespace gameplay::melee_tell;
        Reset();
        assert(WinMs() == 440 && ReactMs() == 320);
        assert(Tc(0, 440) == 320);          // authored 440 is capped at the tell
        assert(Speed(0, 440) > 1.24f && Speed(0, 440) < 1.26f);
        NoteContact(0, 400);                // est = 400 * 1.25 = 500, EMA 0.5 with the 440 author
        SetTellMs(500);
        assert(Tc(0, 440) == 470);
        SetTellMs(900);                     // the ext's config range clamp
        assert(TellMs() == 500);
        SetEnabled(false);
        assert(WinMs() == 0 && ReactMs() == 0);
        assert(Tc(0, 440) == 440 && Speed(0, 440) == 0.0f);
        SetEnabled(true);
        SetMeleeV2(true);
        assert(WinMs() == 0 && ReactMs() == 0);
        Reset();
    }

    void TestGeometry()
    {
        using namespace gameplay::melee_dodge;
        Pose self;
        Pose peer;
        peer.pos[2] = 2.0f;
        peer.rot[1] = 1.0f;                 // yaw 180: forward -z, facing us
        peer.rot[3] = 0.0f;
        assert(GeometryOk(self, peer, 1000, 1200, false));
        assert(GeometryOk(self, peer, 1000, 1600, false));
        peer.rot[1] = 0.0f;
        peer.rot[3] = 1.0f;                 // forward +z: back to us
        assert(!GeometryOk(self, peer, 1000, 1200, false));
        peer.rot[1] = 0.8660254f;           // yaw 120: forward x,z = 0,-0.5
        peer.rot[3] = 0.5f;
        assert(!GeometryOk(self, peer, 1000, 1600, false)); // strict cos 0.766
        assert(GeometryOk(self, peer, 1000, 1600, true));   // loose cos 0.5
        peer.pos[2] = 5.0f;
        assert(!GeometryOk(self, peer, 1000, 1600, true));  // loose reach 4.4 m
        peer.pos[2] = 0.0f;
        assert(GeometryOk(self, peer, 0, 0, false));        // d < 0.05 m
        peer.pos[1] = 1.4f;
        assert(!GeometryOk(self, peer, 0, 0, false));       // y tolerance 1.3 m
    }

    void TestWindowOverrides()
    {
        using namespace gameplay;
        melee_dodge::Reset();
        melee_dodge::NoteSwing(1, 1000, 1);
        uint16_t win = 0;
        assert(melee_dodge::Window(1100, &win) == 1 && win == 440);
        assert(melee_dodge::WindowEnd() == 1440); // swing + win; react would end at 1420

        melee_dodge::WindowOverrides ov;
        ov.exchange = [](int, uint64_t*) -> int { return 2; }; // defender's window closed
        melee_dodge::SetWindowOverrides(ov);
        assert(melee_dodge::Window(1100, nullptr) == -1);
        ov.exchange = [](int, uint64_t* end) -> int { *end = 1250; return 1; };
        melee_dodge::SetWindowOverrides(ov);
        assert(melee_dodge::Window(1100, nullptr) == 1);
        assert(melee_dodge::WindowEnd() == 1250);

        melee_duel::Reset();
        melee_duel::SetEnabled(true);
        melee_duel::SetSlot(1);
        melee_duel::NoteSwing(1, 1000, 1250);
        ov.exchange = nullptr;
        ov.duel = [](int slot, uint64_t now, uint64_t sw, uint64_t* end) -> int {
            return melee_duel::WindowEnd(slot, now, sw, end);
        };
        ov.geometry = [](int slot, uint64_t now, bool, float*) -> bool {
            // only slot 1 is in front and within reach here
            return slot == 1 && now < 2000;
        };
        melee_dodge::SetWindowOverrides(ov);
        assert(melee_dodge::Window(1100, nullptr) == 1);
        assert(melee_dodge::WindowEnd() == 1250);
        assert(melee_dodge::Window(1300, nullptr) == -1); // duel contact closed it
        melee_dodge::SetWindowOverrides(melee_dodge::WindowOverrides{});
        melee_duel::Reset();
    }

    void TestHold()
    {
        using namespace gameplay;
        ReplaySink sink;
        melee_dodge::Reset();
        melee_dodge::SetSink(&sink);
        melee_dodge::NoteSwing(1, 1000, 1);
        assert(melee_dodge::Window(1100, nullptr) == 1);

        const melee_dodge::HeldBlow b = Blow(1);
        assert(melee_dodge::HoldBlow(b, 1100, 40.0f, 40.0f));
        assert(melee_dodge::HeldActive());
        melee_dodge::HeldTick(1120, true, false);
        assert(sink.replays == 0);          // still inside the 80 ms grace
        assert(melee_dodge::Press(1120, melee_dodge::kFront, 7));
        melee_dodge::HeldTick(1125, true, false);
        assert(sink.replays == 1);          // the press covers the held blow
        assert(sink.last.weapon == 300 && sink.last.graceMs == 80);
        assert(!melee_dodge::HeldActive());

        melee_dodge::Reset();
        melee_dodge::NoteSwing(2, 1000, 1);
        assert(melee_dodge::Window(1100, nullptr) == 2);
        assert(melee_dodge::HoldBlow(Blow(2), 1100, 400.0f, 999.0f));
        melee_dodge::HeldTick(1180, true, false);
        assert(sink.replays == 1);          // clamp: round(40 + 150) -> 90 ms
        melee_dodge::HeldTick(1200, true, false);
        assert(sink.replays == 2);          // expired: replay the blow unmodified
        melee_dodge::Reset();
        melee_dodge::SetSink(nullptr);
    }

    void TestDuelFair()
    {
        using namespace gameplay::melee_duel;
        assert(VicExitMs(0.5f) == 500);     // pair hand-off before the 580 ms reaction exit
        assert(VicExitMs(2.0f) == 580);
        assert(AttLead(40.0f) == 20);       // 54 - 80 = -26, floored at 20
        assert(VicLead(40.0f) == 110);      // 20 + 80 + 10
        assert(FairGateMs(120.0f, 500, 460, 330, 0) == 350);
        assert(FairGateMs(400.0f, 500, 460, 330, 0) == 390); // base + MDU_FAIR_MAX_MS
        assert(FairGateMs(400.0f, 500, 460, 330, 0, false, true) == 900);
        assert(FairGateMs(40.0f, 500, 460, 330, 400) == 400); // never shortens recovery
        assert(NextBaseMs(1) == 330 && NextBaseMs(2) == 500 && NextBaseMs(2, true) == 910);
        Reset();
    }

} // namespace

int main()
{
    TestTell();
    TestGeometry();
    TestWindowOverrides();
    TestHold();
    TestDuelFair();
    std::puts("MELEE_TIMING_CORE_TEST_OK");
    return 0;
}
