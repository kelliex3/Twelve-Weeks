// Standalone harness for the pure CLAN cores (no game headers / no Windows):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic
//       -I mod/src/net/online -I mod/src/net/social
//       mod/tests/clan_core_test.cpp -o /tmp/clan_core_test && /tmp/clan_core_test
//
// Covers the port of tools/ext/t2r-ext.c (ClanT / ClanParse / EvWanted /
// PmReady / AwardHook) and tools/ext/clan13.h (Cl13Parse / Cards / Thread /
// WeekRows / DayParas / Clip / Wrap). The runtime wire modules are tested by
// syntax checks; only the pure data cores are exercised here.

#include "clan-state.hpp"
#include "clan-page-core.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    float Est(const char* t, float pt)
    {
        float w = 0.0f;
        for (const unsigned char* q = reinterpret_cast<const unsigned char*>(t); *q; ++q)
            if ((*q & 0xC0) != 0x80)
                w += (*q == ' ' ? 0.4f : 0.7f) * pt;
        return w;
    }

    // ---- clan-state pure core --------------------------------------------------

    void StateTests()
    {
        net::clan::State c;
        Check(net::clan::ParseState("fac=1 st=1 day=3 h=12 s=40 need=60 lday=2 levk=2 "
                                    "lmh=771 ev=1 evn=2 ag1=4 rk=3 dt=1 ro=2", c),
              "0xE1 state parses");
        Check(c.ok && c.fac == 1 && c.st == 1 && c.day == 3 && c.lday == 2
              && c.levk == 2 && c.lmh == 771 && c.ev == 1 && c.evn == 2
              && c.ag[0] == 4 && c.rk == 3 && c.dt == 1 && c.ro == 2,
              "0xE1 fields map to the ext ClanT layout (incl. build-201 ro)");

        net::clan::State bad;
        Check(net::clan::ParseState("nonsense", bad) && bad.ok && bad.fac == 0,
              "unrecognised keys are skipped (ext behaviour)");
        Check(!net::clan::ParseState(nullptr, bad), "null 0xE1 text refused");
        Check(bad.ro == 0, "an older daemon without ro leaves the plain role");

        // EvWanted: a played result beats an announcement; a missing day holds (-1).
        net::clan::EventBook ev;
        Check(net::clan::EventWanted(c, ev) == 1, "announcement due on st=1 + ev");
        ev.annDay = c.day;
        Check(net::clan::EventWanted(c, ev) == 0, "announcement already seen");
        ev.outDay = 3;                      // result due for day 3, state says day 2
        Check(net::clan::EventWanted(c, ev) == -1, "day not in yet -> -1");
        c.lday = 3;
        Check(net::clan::EventWanted(c, ev) == 2, "event result due (levk=2)");
        c.levk = 0;
        Check(net::clan::EventWanted(c, ev) == 0, "quiet day -> nothing due");

        // PmReady: the server booked our day, with a fresh match tag.
        Check(net::clan::PmReady(c, 3, 0) == true, "post-match ready with new tag");
        Check(net::clan::PmReady(c, 2, 0) == false, "wrong day");
        c.lmh = 771;
        Check(net::clan::PmReady(c, 3, 771) == false, "same match tag -> not ready");
        Check(net::clan::PmReady(c, 3, 0) == true, "changed tag -> ready");

        // AwardHook tally: uppercase, fold the 6th distinct row into the 5th.
        net::clan::PmRow rows[net::clan::kPmRows];
        int n = 0;
        for (int i = 0; i < 6; ++i) {
            char l[8];
            std::snprintf(l, sizeof l, "lab%d", i);
            net::clan::NoteAwardRow(rows, n, l, 10 + i);
        }
        net::clan::NoteAwardRow(rows, n, "headshot", 7);
        Check(n == net::clan::kPmRows, "award tally caps at 5 rows");
        Check(std::strcmp(rows[0].label, "LAB0") == 0 && rows[0].count == 1
              && rows[0].parts == 10 && rows[4].count == 3 && rows[4].parts == 36,
              "award labels uppercased, overflow folded into the last row");
        net::clan::NoteAwardRow(rows, n, nullptr, 5);
        Check(n == net::clan::kPmRows, "null award label ignored");
    }

    // ---- clan-page-core pure core ---------------------------------------------

    void PageParseTests()
    {
        using namespace net::clan::page;
        Data d;
        char sec1[] =
            "H\t2\t3\tW\t50\tSTEADY\t12\t3\n"
            "P\tAnn\tLEAD\th\tALIVE\texpr\tpx1\tBOND\tsister\ttag\tlately\tic\n";
        // The H row is a header, not a data row (the ext's Cl13Parse returns
        // only the counted P/J/Q/D/G/... rows, not the H line).
        Check(Parse(d, 1, sec1) == 1 && d.ok && d.week == 2 && d.dow == 3
              && d.fk == 'W' && d.morale == 50 && d.alive == 12 && d.np == 1,
              "section 1 header + roster parsed");
        Check(std::strcmp(d.p[0].name, "Ann") == 0 && d.p[0].st == 'h'
              && std::strcmp(d.p[0].pid, "px1") == 0, "roster fields");

        char sec2[] =
            "H\t2\t3\tW\t50\tSTEADY\t12\t3\n"
            "J\t9\tc\tT3\tb3\tgood\ta1\t3\t9\t2\t\t\t0\n"
            "J\t8\tc\tOTHER\tbo\t\t\t0\t8\t2\t\t\t0\n"
            "J\t7\tc\tT2\tb2\t\ta1\t2\t7\t2\t\t\t0\n"
            "J\t6\tc\tT1\tb1\t\ta1\t1\t6\t2\t\t\t0\n";
        Check(Parse(d, 2, sec2) == 4 && d.nj == 4, "hand made journal parsed");
        int cards[C13_NJ], thread[8];
        const int nc = Cards(d, cards, C13_NJ);
        Check(nc == 2 && cards[0] == 0 && cards[1] == 1,
              "cards: one per arc, newest step first");
        const int tn = Thread(d, 0, thread, 4);
        Check(tn == 3 && d.j[thread[0]].step == 1 && d.j[thread[2]].step == 3,
              "thread oldest -> newest");
        const int tn2 = Thread(d, 0, thread, 2);
        Check(tn2 == 2 && d.j[thread[0]].step == 2 && d.j[thread[1]].step == 3,
              "thread capped to the newest 2 steps");
        Check(Thread(d, 1, thread, 4) == 1 && thread[0] == 1,
              "a row without an arc is its own card");

        char garbage[] = "zzz";
        Check(Parse(d, 2, garbage) == -1, "garbage refused");
        Check(Parse(d, 6, const_cast<char*>("H")) == -1, "short header refused");

        char st5[] =
            "H\t2\t3\tW\t50\tSTEADY\t12\t3\n"
            "X\t3\tHill\t1\t2\n"
            "Q\tAnn\t0\t1\t5\t3\t2\t2\t1\t1\t320\t0\t104\n"
            "Q\tBob\t1\t0\t-1\t-1\t-1\t-1\t-1\t-1\t-1\t1\n";
        Check(Parse(d, 5, st5) == 3 && d.xn == 2 && d.xday == 3
              && d.xwin == 1 && std::strcmp(d.xmap, "Hill") == 0 && d.have[5],
              "match stats parsed");
        Check(std::strcmp(d.x[0].name, "Ann") == 0 && d.x[0].me
              && d.x[0].kills == 5 && d.x[0].deaths == 2 && d.x[0].parts == 320
              && d.x[1].team == 1 && d.x[1].kills == -1 && d.x[1].left,
              "match stats fields");
        Check(d.x[0].head == 104 && d.x[1].head == 0,
              "head id read (older daemon rows have none)");

        char sec6[] =
            "H\t3\t1\tW\t60\tSTEADY\t11\t2\n"
            "K\t1\t84\n"
            "D\t0\t1\t1\t101\tFOUNDING\tcalm\tHill\t1\t4\t4\t3\t3\t0\t0\t0\n"
            "D\t1\t2\t1\t102\tDAY ONE\tcalm\tHill\t0\t2\t4\t2\t3\t1\t1\t1\n"
            "G\t1\tTHE FIRST DAY\twe made it through the first night\n";
        Check(Parse(d, 6, sec6) == 4 && d.nd == 2 && d.ng == 1
              && d.jweek == 1 && d.jtop == 84, "section 6 journal page parsed");
        int rows[C13_ND], paras[C13_NG];
        Check(WeekRows(d, 1, rows, C13_ND) == 2 && rows[0] == 0 && rows[1] == 1,
              "journal week rows in file order");
        Check(DayParas(d, 1, paras, C13_NG) == 1 && paras[0] == 0,
              "a day's paragraphs grouped");
        Check(std::strcmp(d.d[1].title, "DAY ONE") == 0 && d.d[1].fresh,
              "journal day fields");

        char empty[] = "";
        Check(Parse(d, 5, empty) == 0 && d.have[5] && d.xn == 0,
              "empty section 5 reply (older daemon)");

        // Clip / Wrap: pure text, the caller supplies Est (the ext uses the
        // engine's text measurement).
        char out[128];
        Clip(out, sizeof out, "one two three four five six seven eight", 8.0f * 0.7f,
             1.0f, 2, Est);
        Check(std::strstr(out, "one") == out && std::strstr(out, "...") != nullptr,
              "clip ends the last line with '...'");
        Wrap(out, sizeof out, "one two\n\nthree four five", 12.0f * 0.7f, 1.0f, 4, Est);
        Check(std::strstr(out, "one two\n\nthree four") != nullptr,
              "wrap keeps explicit paragraph breaks");
    }

} // namespace

int main()
{
    StateTests();
    PageParseTests();
    std::printf("%s\n", fails ? "CLAN_CORE_TEST_FAIL" : "CLAN_CORE_TEST_OK");
    return fails ? 1 : 0;
}
