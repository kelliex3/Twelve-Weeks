// Standalone harness for build-191 tools/ext/namerim.c rules:
//   g++ -std=c++20 -Wall -Wextra -Werror -pedantic -I mod/src/net/social
//       mod/tests/name_rim_core_test.cpp -o /tmp/opencode/name_rim_core_test
//   /tmp/opencode/name_rim_core_test

#include "name-rim-core.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

    using namespace net::social::name_rim::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

} // namespace

int main()
{
    std::printf("name rim core:\n");

    Check(kRows == 8 && kRimN == 8 && kRimR == 1.0f,
          "eight rims at one canvas unit around every name");
    Check(kLeadDevRole == 4 && kTickMs == 150 && kResolveFrames == 30,
          "role 4 lead dev, 150 ms poll, 30-frame resolve");

    // The offsets are the eight compass points, in clockwise order from east.
    Check(kDx[0] == 1.0f && kDy[0] == 0.0f && kDx[2] == 0.0f && kDy[2] == 1.0f,
          "east / south rim offsets");
    Check(kDx[4] == -1.0f && kDy[6] == -1.0f, "west / north rim offsets");
    Check(std::fabs(kDx[1] - 0.7071f) < 0.0001f
          && std::fabs(kDy[1] - 0.7071f) < 0.0001f,
          "diagonal offsets use 0.7071");

    char s[64];
    Strip(s, sizeof s, "<font color=\"#ff537d\">[T2R] Ellie</font>");
    Check(std::strcmp(s, "[T2R] Ellie") == 0, "markup is stripped from rim text");
    Strip(s, sizeof s, "<scale>AB<font color=\"x\">C</font>");
    Check(std::strcmp(s, "ABC") == 0, "nested tags are stripped");
    char small[6];
    Strip(small, sizeof small, "ABCDEFGH");
    Check(std::strcmp(small, "ABCDE") == 0, "strip respects the output cap");

    char p[128];
    Check(LobbyNamePath(p, sizeof p, 0)
          && std::strcmp(p, "anim-group.lobbyPanel.lobbyList.Player0.nameLabel") == 0,
          "lobby name path");
    Check(LobbyRimPath(p, sizeof p, 7)
          && std::strcmp(p, "anim-group.lobbyPanel.lobbyList.Player7.nmRim%d") == 0,
          "lobby rim format keeps the %%d placeholder");
    Check(ScoreboardRimPath(p, sizeof p, 0)
          && std::strcmp(p, "anim-group.scoreboard.team0_row0_nmRim%d") == 0,
          "scoreboard slot 0 is team 0 row 0");
    Check(ScoreboardRimPath(p, sizeof p, 7)
          && std::strcmp(p, "anim-group.scoreboard.team1_row3_nmRim%d") == 0,
          "scoreboard slot 7 is team 1 row 3");
    Check(!LobbyRimPath(p, sizeof p, 8) && !ScoreboardRimPath(p, sizeof p, -1),
          "out-of-range rows are refused");

    Check(IsMe("Ellie", "ellie") && IsMe("[T2R] Ellie", "Ellie")
          && IsMe("WLF]Ellie", "ellie"), "own name matches exactly or after a tag");
    Check(!IsMe("xEllie", "ellie") && !IsMe("Ellie2", "ellie"),
          "no separator / extra characters mean another player");
    Check(IsPlaceholder("") && IsPlaceholder("-") && IsPlaceholder("PLAYER 3")
          && IsPlaceholder("OPEN SEAT") && IsPlaceholder("WOLNE MIEJSCE")
          && IsPlaceholder("SEARCHING"), "lobby placeholders never get a rim");
    Check(!IsPlaceholder("[T2R] Ellie"), "a real name is not a placeholder");
    Check(IsScoreboardPlaceholder("PLAYER 2") && IsScoreboardPlaceholder("-")
          && IsScoreboardPlaceholder(""),
          "scoreboard placeholders are the ext's three cases");
    Check(!IsScoreboardPlaceholder("Ellie"), "a real scoreboard name can get a rim");

    Check(LobbyRole(true, "Ellie", 4, 0) == 4,
          "our own lobby row uses the 0xE1 role");
    Check(LobbyRole(false, "Ellie", 4, 4) == 4,
          "another row uses the 0xBF table role");
    Check(LobbyRole(true, "PLAYER 1", 4, 4) == 0,
          "a placeholder row has no role even when it is ours");
    Check(ScoreboardRole("[T2R] Ellie", "Ellie", 4, 0) == 4,
          "our scoreboard row matches a tagged local name");
    Check(ScoreboardRole("Other", "Ellie", 4, 4) == 4,
          "another scoreboard row uses the table role");
    Check(ScoreboardRole("PLAYER 2", "Ellie", 4, 4) == 0,
          "an unfilled scoreboard row has no rim");

    Check(!ShouldShow(false, 4, ""), "an empty name never shows the rim");
    Check(ShouldShow(false, 4, "Ellie") && !ShouldShow(true, 4, "Ellie")
          && !ShouldShow(false, 0, "Ellie"), "opt-out and non-lead roles hide the rim");

    Check(ShouldTick(300, 300) && ShouldTick(451, 300) && !ShouldTick(299, 300),
          "the poll repeats once the next tick is due");
    Check(!ShouldResolve(30, 1) && ShouldResolve(32, 1),
          "the ext resolves a rebuilt row after more than 30 frames");

    RimState st;
    ResetState(st, 7, 42);
    Check(st.gen == 7 && st.nameW == 42 && st.on == -1,
          "a new page / name handle starts with hidden rims");
    Check(NeedRewrite(st, false, ""), "a fresh row is always written once");
    st.on = 0;
    std::strcpy(st.txt, "");
    Check(!NeedRewrite(st, false, "") && NeedRewrite(st, true, "Ellie"),
          "hidden rows are not rewritten until a name appears");
    st.on = 1;
    std::strcpy(st.txt, "Ellie");
    Check(!NeedRewrite(st, true, "Ellie") && NeedRewrite(st, true, "Abby"),
          "a changed name rewrites the rim text");

    float ox = 0.0f, oy = 0.0f;
    Place(100.0f, 200.0f, 2.0f, 3.0f, 0, ox, oy);
    Check(ox == 101.0f && oy == 200.0f,
          "rim offsets are added to the name pivot, not scaled by it");

    if (fails) std::printf("name rim core: %d FAILED\n", fails);
    else       std::printf("name rim core: all passed\n");
    return fails ? 1 : 0;
}
