// Standalone harness for the pure female effort-SID rules; no engine, DC or
// generated-data headers.
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src
//       mod/tests/vox_effort_core_test.cpp -o /tmp/opencode/vox_effort_check
//   /tmp/opencode/vox_effort_check

#include "net/audio/vox-effort-core.hpp"

#include <cstdio>

namespace {

    using namespace net::audio::effort::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    View Fixture(uint64_t (*lines)[kVoiceCount], uint16_t (*range)[2])
    {
        return { lines, 3, range };
    }

    void LookTests()
    {
        Check(VoiceOfLook(0, 6) == 0 && VoiceOfLook(0, 7) == 2
              && VoiceOfLook(1, 6) == 1 && VoiceOfLook(1, 7) == 3,
              "the ext's VX_LOOK_VOICE letters choose the female voice");
        Check(VoiceOfLook(2, 0) == -1 && VoiceOfLook(0, 8) == -1,
              "an unknown faction / look has no voice");
        Check(LookIndex("scar-female-11") == 6
              && LookIndex("militia-female-12-backpack") == 7
              && LookIndex("scar-winter-f09") == 7,
              "both authored and winter female rows resolve by name");
        Check(FemaleLook("scar-female-11")
              && !FemaleLook("scar-male-13") && !FemaleLook(nullptr),
              "effort takes only answer on the two female look rows");
        Check(LookIndex("cz-female-11-composed") == 6
              && LookIndex("cz-female-09-composed") == 7,
              "composed outfits keep the authored female voice letter");
    }

    void WheelTests()
    {
        State s;
        Reset(s);
        Check(s.last[0][0] == -1 && s.last2[1][3] == -1,
              "reset clears the no-repeat wheel");
        Check(Pick(s, 0, 0, 0, 2) == 0
              && Pick(s, 0, 0, 0, 2) == 1
              && Pick(s, 0, 0, 0, 2) == 0,
              "a two-line pool alternates on repeated rolls");
        Reset(s);
        Check(Pick(s, 1, 2, 0, 3) == 0
              && Pick(s, 1, 2, 0, 3) == 1
              && Pick(s, 1, 2, 0, 3) == 2
              && Pick(s, 1, 2, 0, 3) == 0,
              "a three-line pool avoids both of the last two takes");
        Check(Pick(s, 2, 0, 0, 3) == -1 && Pick(s, 0, 0, 0, 0) == -1,
              "an invalid faction / empty pool never picks");
    }

    void SidTests()
    {
        uint64_t lines[3][kVoiceCount] = {
            { 0x1111ULL, 0x2111ULL, 0x3111ULL, 0x4111ULL },
            { 0x1222ULL, 0x2222ULL, 0x3222ULL, 0x4222ULL },
            { 0x1333ULL, 0x2333ULL, 0x3333ULL, 0x4333ULL }
        };
        uint16_t range[kFactionCount * kKindCount][2] = {};
        const int give = static_cast<int>(Kind::kGive);
        const int pain = static_cast<int>(Kind::kPain);
        range[0 * kKindCount + give][0] = 0;
        range[0 * kKindCount + give][1] = 3;
        range[1 * kKindCount + pain][0] = 2;
        range[1 * kKindCount + pain][1] = 1;
        const View v = Fixture(lines, range);

        State s;
        Reset(s);
        Check(Sid(v, s, 0, Kind::kGive, 1, 0) == 0x2111ULL
              && Sid(v, s, 0, Kind::kGive, 1, 0) == 0x2222ULL
              && Sid(v, s, 0, Kind::kGive, 1, 0) == 0x2333ULL
              && Sid(v, s, 0, Kind::kGive, 1, 0) == 0x2111ULL,
              "Sid resolves the picked row for the chosen voice and never repeats");
        Check(Sid(v, s, 1, Kind::kPain, 3, 0) == 0x4333ULL,
              "the WLF pain range points at its authored row");
        Check(Sid(v, s, 0, Kind::kPain, 0, 0) == 0
              && Sid(v, s, 0, Kind::kGive, 4, 0) == 0
              && Sid(v, s, 2, Kind::kGive, 0, 0) == 0,
              "an unfilled range / voice / faction returns no line");

        const View empty;
        Check(Sid(empty, s, 0, Kind::kGive, 0, 0) == 0,
              "a build without generated data returns no line");
    }

} // namespace

int main()
{
    LookTests();
    WheelTests();
    SidTests();
    if (fails) {
        std::printf("%d effort core checks failed\n", fails);
        return 1;
    }
    std::puts("all effort core checks passed");
    return 0;
}
