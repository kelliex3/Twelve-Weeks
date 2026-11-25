// Standalone harness for the pure achievement core (daemon client-half + UI):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/achieve_core_test.cpp -o /tmp/achieve_core_test
//   /tmp/achieve_core_test
//
// The daemon still owns every unlock; these checks pin the counter/report,
// table-parse and card/page timing the native owner relies on.

#include "achieve-core.hpp"

#include <cmath>
#include <cstdio>

namespace {

    using namespace net::ui::achieve::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }
    bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

    void TableTests()
    {
        const char* txt =
            "ACH 2 3\n"
            "se_hammer|melee|100|40|50|0|Hammer Time|Hit 50 with a hammer\n"
            "pin_low|skill|200|1|1|1|Cliffhanger|Escape a pin at low health\n"
            "craft_100|stats|300|275|300|0|Crafter III|Craft 300 items\n"
            "LIVE 1\n";
        Table t;
        bool live = false;
        Check(ParseTable(txt, t, live) && t.n == 3 && live,
              "the 0xAA table parses all rows and the LIVE 1 trailer");
        Check(t.ok && t.rows[0].goal == 50 && t.rows[1].hidden == 1 &&
              std::strcmp(t.rows[2].id, "craft_100") == 0,
              "row fields keep their authored values");
        const Row* r = FindRow(t, "se_hammer");
        Check(r && r->t == 100 && r->prog == 40,
              "FindRow resolves a table id to its unlock time / progress");
        Check(!ParseTable("garbage", t, live), "a foreign reply is rejected");
    }

    void ReportTests()
    {
        int ach[kLabN]{};
        ach[64] = 5;
        ach[76] = 70000;                 // clamps to u16 on the wire
        uint16_t mis[64]{};
        mis[7] = 3;
        uint8_t buf[kPayloadCap];

        const int n = BuildReport(ach, mis, 64, 0x1234, buf, sizeof buf);
        Check(n == 5 + 3 * 3 && buf[4] == 3,
              "0xA9 carries a u32 seq, a row count, and one row per non-zero label");
        uint32_t seq = 0;
        std::memcpy(&seq, buf, 4);
        Check(seq == 0x1234, "the report sequence is caller-owned so retries repeat it");
        uint16_t c = 0;
        // rows are [u8 label][u16 count]; label 64 is the third row here.
        std::memcpy(&c, buf + 9, 2);
        Check(c == 5, "an achievement-only label reads from the ach tally");
        Check(CountNow(ach, mis, 64, 7) == 3 && CountNow(ach, mis, 64, 64) == 5 &&
              CountNow(ach, mis, 64, 76) == 65535,
              "labels 1..63 read the mission tally, 64..127 the achievement tally, clamped");

        const int ln = BuildLive(ach, mis, 64, buf, sizeof buf);
        Check(buf[0] == 3 && ln == 1 + 3 * 3,
              "0xAB has no sequence and carries the same three non-zero labels");
        Check(LiveSkip(21) && LiveSkip(33) && !LiveSkip(64),
              "match-report-only labels are excluded from the live request");
        Check(kLiveN == 44 && kLiveSEN == 9,
              "the 44 generated live rules and 9 Special Execution labels are mirrored");
    }

    void UiTests()
    {
        Table t;
        bool live = false;
        ParseTable("ACH 3 3\n"
                   "se_hammer|melee|100|40|50|0|H|d1\n"
                   "se_bat|melee|200|40|50|0|B|d2\n"
                   "pin_low|skill|300|1|1|1|P|d3\n", t, live);
        UiState ui;
        BuildGroups(ui, t);
        Check(ui.groups == 2 && ui.g[0].n == 2 && ui.g[1].n == 1,
              "the grid compacts non-empty group rows by their group key");
        Check(FocusedIndex(ui) == 0, "the first cell is focused on open");
        Check(MoveRow(ui, 1) && ui.fr == 1 && ui.fc == 0,
              "down keeps the absolute screen column");
        Check(MoveCol(ui, 1) == false, "a one-item group cannot move sideways");
        HoverSignature(ui);

        uint32_t newest = 0;
        Toast q[kToastQ];
        const int nq = QueueNew(t, 100, q, kToastQ, newest);
        Check(nq == 2 && newest == 300,
              "rows newer than the seen stamp queue once and newest advances past the cap");

        CardState c;
        EnqueueCard(c, q[0]);
        StartCard(c, 1000, 0);
        Check(Near(CardAlpha(1150, 1000), 0.5f) && Near(CardAlpha(2000, 1000), 1.0f),
              "the card fades in over 300 ms then holds");
        Check(CardSlide(1000, 1000) > 0.99f && CardSlide(1300, 1000) == 0.0f,
              "the slide is cubic-out and gone at the end of the fade-in");
        Check(!CardDone(1000 + kCardIn + kCardHold + kCardOut - 1, 1000) &&
              CardDone(1000 + kCardIn + kCardHold + kCardOut, 1000),
              "the card leaves after in + hold + out");
        Check(AdvanceCard(c, 1000 + kCardIn + kCardHold + kCardOut) && c.n == 0,
              "advancing pops the queue head");
    }

} // namespace

int main()
{
    TableTests();
    ReportTests();
    UiTests();
    if (fails) {
        std::printf("%d failure(s)\n", fails);
        return 1;
    }
    std::printf("achieve core: all checks passed\n");
    return 0;
}
