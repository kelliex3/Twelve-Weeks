// Standalone harness for the pure killfeed core (no game headers / no Windows):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/kill_feed_core_test.cpp -o /tmp/kill_feed_core_test && /tmp/kill_feed_core_test
//
// Covers the ext event queue, the 0x94 down-before-kill ordering rule,
// down-to-kill conversion windows, and the row alpha / glide curves.

#include "kill-feed-core.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

    using namespace net::ui::killfeed::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    Event Make(const char* killer, const char* victim, bool down = false,
               uint8_t vslot = 0, uint8_t kslot = 1)
    {
        Event e;
        CopyName(e.killer, killer);
        CopyName(e.victim, victim);
        e.haveKiller = killer && killer[0];
        e.isDown = down;
        e.vslot = vslot;
        e.kslot = kslot;
        e.born = 1000;
        return e;
    }

    void NameTests()
    {
        char out[44];
        CopyName(out, "01234567890123456789012345678901234567890123456789");
        Check(std::strlen(out) == 43 && std::strcmp(out, "0123456789012345678901234567890123456789012") == 0,
              "CopyName truncates to 43 characters and terminates");
        CopyName(out, nullptr);
        Check(out[0] == '\0', "CopyName handles a null name");
    }

    void QueueTests()
    {
        Event q[kQueue];
        int n = 0;
        for (int i = 0; i < kQueue; ++i) {
            Event e = Make("killer", "victim");
            e.born = static_cast<uint64_t>(1000 + i);
            Push(q, n, e);
        }
        Check(n == kQueue && q[kQueue - 1].born == 1000 + kQueue - 1,
              "the queue keeps the newest events");
        Push(q, n, Make("new", "fresh"));
        Check(n == kQueue && q[kQueue - 1].born == 1000,
              "a full queue drops the oldest event");

        Event waiting = Make("a", "b", true);
        waiting.waitUntil = 1700;
        Check(Waiting(waiting, 1699) == true && Waiting(waiting, 1700) == false,
              "a waiting event stays queued only until its deadline");

        Event q2[4] = { waiting, Make("a", "b", false), Event{}, Event{} };
        q2[1].born = 2000;
        Check(Blocked(q2, 2, 1, 1699) == true,
              "a kill cannot overtake a same-victim down waiting for its weapon");
        Check(Blocked(q2, 2, 1, 1700) == false,
              "once the down deadline passes the kill is unblocked");
        q2[0].vslot = 9;
        Check(Blocked(q2, 2, 1, 1699) == false,
              "a down of another slot never blocks the kill");
    }

    void IconTests()
    {
        Check(PickIcon(false, false) == Icon::Skull && PickIcon(false, true) == Icon::Skull,
              "a death with no killer always shows the skull");
        Check(PickIcon(true, false) == Icon::Fist,
              "a kill with no known weapon shows fists");
        Check(PickIcon(true, true) == Icon::Weapon,
              "a known weapon shows the weapon icon");
    }

    void ConvertTests()
    {
        Check(CanConvert(true, 2000, 1000, true, true, 0) == false,
              "a down inside the recent-kill window cannot convert");
        Check(CanConvert(true, 3000, 1000, true, true, 0) == true,
              "a down after the window can convert");
        Check(CanConvert(false, 3000, 0, true, true, 0) == false,
              "a kill row is never a conversion source");
        Check(CanConvert(true, 3000, 1000, false, true, 0) == false,
              "a different attacker blocks the conversion");
        Check(CanConvert(true, 3000, 1000, true, false, 0) == false,
              "a different victim blocks the conversion");
        Check(CanConvert(true, 3000, 1000, true, true, 4000) == false,
              "a revive after the down blocks the conversion");
    }

    void VisualTests()
    {
        Check(Alpha(1000, 1000, false) == 0.0f, "a row starts transparent");
        Check(Alpha(1000 + kFadeInMs / 2, 1000, false) > 0.49f
              && Alpha(1000 + kFadeInMs / 2, 1000, false) < 0.51f,
              "the fade-in is linear");
        Check(Alpha(1000 + 2000, 1000, false) == 1.0f, "a live row is fully opaque");
        Check(Alpha(1000 + kLifeMs - kFadeOutMs / 2, 1000, false) > 0.49f
              && Alpha(1000 + kLifeMs - kFadeOutMs / 2, 1000, false) < 0.51f,
              "the fade-out is linear too");
        Check(Alpha(1000 + kLifeMs, 1000, false) == 0.0f && Alpha(99999, 1000, true) == 0.0f,
              "an expired row is invisible");

        Check(Glide(-1.0f, kTop, 16) == kTop, "a new row snaps to the target y");
        float y = Glide(kTop, kTop + kPitch, 16);
        Check(y > kTop && y < kTop + kPitch, "an existing row glides toward the target");
        Check(std::fabs(Glide(kTop + kPitch, kTop + kPitch, 16) - (kTop + kPitch)) < 1e-6f,
              "a settled row stays at the target");
    }

} // namespace

int main()
{
    NameTests();
    QueueTests();
    IconTests();
    ConvertTests();
    VisualTests();
    std::printf("%s\n", fails ? "kill-feed core: FAILED" : "kill-feed core: all tests passed");
    return fails ? 1 : 0;
}
