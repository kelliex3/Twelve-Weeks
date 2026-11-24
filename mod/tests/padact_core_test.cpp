// Standalone harness for the pure pad-activity core (no game headers / no Windows):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/engine
//       mod/tests/padact_core_test.cpp -o /tmp/padact_core_test && /tmp/padact_core_test
//
// Port of tools/ext/tests/padact_test.c for padact.h (build 199): activity on any
// (joypad, Pad) pair, not only the command-reader's learned joypad, and Steam Input
// command-answer flips. The engine-facing hooks are checked separately; only the
// pure core is exercised here.

#include "pad-act.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

    using namespace engine::pad::activity;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    const float kRest[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    void TestNote()
    {
        Sources t{};
        char jpA = 0, jpB = 0, padA = 0, padB = 0, padB2 = 0;   // addresses only
        const void* a = &jpA;
        const void* b = &jpB;
        uint64_t ms = 1000;

        // A new source is a baseline only: a held button at match start is no new input.
        Check(!Note(t, a, &padA, 0x40, kRest, ms), "first call of a source must not count");
        Check(!Note(t, a, &padA, 0x40, kRest, ms += 16), "same mask must not count");
        Check(t.activityMs == 0 && t.activityCount == 0, "no activity yet");

        // A joypad that is NOT the learned one (there is none in pad mode).
        Check(!Note(t, b, &padB, 0, kRest, ms += 16), "jpB baseline");
        Check(Note(t, b, &padB, 0x200, kRest, ms += 16),
              "button press on a second joypad must count");
        Check(t.activityMs == ms && t.activityCount == 1, "act_ms / act_n after jpB press");
        Check(Note(t, b, &padB, 0, kRest, ms += 16), "release counts too");

        // Sticks: past 0.3 counts on any joypad, rest noise and NaN do not.
        const float lx[4] = { 0.31f, 0.0f, 0.0f, 0.0f };
        const float noise[4] = { 0.12f, -0.2f, 0.29f, -0.29f };
        const float rs[4] = { 0.0f, 0.0f, 0.0f, -0.9f };
        const float nan4[4] = { NAN, NAN, NAN, NAN };
        Check(Note(t, a, &padA, 0x40, lx, ms += 16), "left stick past 0.3");
        Check(Note(t, b, &padB, 0, rs, ms += 16), "right stick on the second joypad");
        Check(!Note(t, a, &padA, 0x40, noise, ms += 16), "stick noise under 0.3");
        Check(!Note(t, a, &padA, 0x40, nan4, ms += 16), "NaN sticks");

        // Two Pads feeding one joypad on alternate frames (different masks): not activity.
        Sources u{};
        int acts = 0;
        for (int f = 0; f < 600; ++f)
            acts += Note(u, a, (f & 1) ? static_cast<const void*>(&padB2)
                                       : static_cast<const void*>(&padA),
                         (f & 1) ? 0x80u : 0u, kRest,
                         1000 + static_cast<uint64_t>(f) * 16u) ? 1 : 0;
        Check(acts == 0, "alternating pads on one joypad gave no activity");

        // Idle: 90 s of unchanged input on every source is no activity.
        Sources v{};
        acts = 0;
        for (int f = 0; f < 90 * 60; ++f) {
            const uint64_t frame = static_cast<uint64_t>(f);
            acts += Note(v, a, &padA, 0x40, noise, frame * 16u + 1u) ? 1 : 0;
            acts += Note(v, b, &padB, 0, kRest, frame * 16u + 2u) ? 1 : 0;
        }
        Check(acts == 0 && v.activityMs == 0, "idle pads gave no activity");

        // A full table reuses the oldest slot; recent ones keep their baseline.
        Sources w{};
        char many[kSources + 2]{};
        for (int i = 0; i < kSources + 2; ++i)
            Check(!Note(w, a, &many[i], 1, kRest, 5), "new source is baseline");
        Check(!Note(w, a, &many[kSources + 1], 1, kRest, 6), "newest source kept baseline");
        Check(Note(w, a, &many[kSources + 1], 3, kRest, 7), "newest source change counts");
        Check(w.activityMs == 7, "act_ms after table reuse");

        // A timestamp of 0 still marks activity (activityMs 0 means none).
        Sources z{};
        Note(z, a, &padA, 0, kRest, 0);
        Check(Note(z, a, &padA, 1, kRest, 0) && z.activityMs != 0, "act at t=0");
    }

    void TestAnswer()
    {
        char jpA = 0, jpB = 0, jpC = 0;   // addresses only
        AnswerSlot a[2]{};

        Check(!Answer(a, &jpA, 1), "first answer is a baseline");
        Check(!Answer(a, &jpA, 1), "same answer");
        Check(Answer(a, &jpA, 0), "release = input");
        Check(Answer(a, &jpA, 5), "press (any non-zero answer) = input");

        // Two joypads read alternately with different steady answers: no input.
        Check(!Answer(a, &jpB, 0), "jpB baseline");
        int acts = 0;
        for (int f = 0; f < 1000; ++f) {
            acts += Answer(a, &jpA, 1) ? 1 : 0;
            acts += Answer(a, &jpB, 0) ? 1 : 0;
        }
        Check(acts == 0, "alternating joypads with steady answers gave no flips");

        // A third joypad is not tracked; NULL never flips.
        acts = 0;
        for (int f = 0; f < 10; ++f)
            acts += Answer(a, &jpC, f & 1) ? 1 : 0;
        Check(acts == 0, "third joypad gave no flips");
        Check(!Answer(a, nullptr, 1), "NULL joypad");

        // Idle 90 s on the command path: no flips.
        AnswerSlot b[2]{};
        acts = 0;
        for (int f = 0; f < 90 * 60; ++f)
            acts += Answer(b, &jpA, 0) ? 1 : 0;
        Check(acts == 0, "idle command answers gave no flips");
    }

} // namespace

int main()
{
    TestNote();
    TestAnswer();
    std::printf("%s (%d failures)\n", fails ? "FAILED" : "padact_core_test: all passed", fails);
    return fails != 0;
}
