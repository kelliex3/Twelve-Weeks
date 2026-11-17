// clan13_test.c - the pure core of the in-game CLAN page (tools/ext/clan13.h) against the daemon's real op 0xB7 text.
//   gcc -std=gnu2x -O2 -o clan13_test clan13_test.c && clan13_test <sec1.txt> <sec2.txt> <sec3.txt> [<sec6.txt>]
// (the three files are tools/clan_ingame.build() output for a seeded clan: tools/clan_demo_seed.py)
#include "../clan13.h"
static int fails;
#define CHECK(c, ...) do { int ok_ = !!(c); printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
static char *Slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    static char buf[4][32768];
    static int k;
    char *b = buf[k++ & 3];
    size_t n = fread(b, 1, 32767, f);
    b[n] = 0;
    fclose(f);
    return b;
}

static float Est(const char *t, float pt)
{
    float w = 0;
    for (const unsigned char *q = (const unsigned char *)t; *q; q++)
        if ((*q & 0xC0) != 0x80)
            w += (*q == ' ' ? 0.4f : 0.7f) * pt;
    return w;
}

int main(int argc, char **argv)
{
    static Cl13T d;
    if (argc < 4) {
        puts("usage: clan13_test sec1 sec2 sec3");
        return 2;
    }
    char *t1 = Slurp(argv[1]), *t2 = Slurp(argv[2]), *t3 = Slurp(argv[3]);
    CHECK(t1 && t2 && t3, "files read");
    int n1 = Cl13Parse(&d, 1, t1), n2 = Cl13Parse(&d, 2, t2), n3 =
        Cl13Parse(&d, 3, t3);
    CHECK(n1 > 5 && d.np == n1, "roster: %d members", d.np);
    CHECK(n2 > 5 && d.nj == n2, "journal: %d rows", d.nj);
    CHECK(n3 >= 10 && d.ns == 5
          && d.nm >= 3, "raids: %d supply rows, %d matches, %d loot rows",
          d.ns, d.nm, d.nt);
    CHECK(d.ok && d.week >= 1 && d.dow >= 1 && d.dow <= 7
          && (d.fk == 'W' || d.fk == 'S') && d.morale >= 0
          && d.morale <= 100,
          "header: week %d day %d %c morale %d '%s' alive %d worst %d",
          d.week, d.dow, d.fk, d.morale, d.label, d.alive, d.worst);
    CHECK(d.p[0].name[0] && d.p[0].role[0] && strchr("husdk", d.p[0].st)
          && d.p[0].pid[0], "first member: %s / %s / %c / %s", d.p[0].name,
          d.p[0].role, d.p[0].st, d.p[0].pid);
    int arcs = 0;
    for (int i = 0; i < d.nj; i++)
        if (d.j[i].arc[0])
            arcs++;
    int idx[C13_NJ], th[8];
    int nc = Cl13Cards(&d, idx, C13_NJ);
    CHECK(nc <= d.nj
          && nc > 0, "cards: %d of %d rows (%d are story steps)", nc, d.nj,
          arcs);
    for (int i = 0; i < nc; i++)
        for (int k = i + 1; k < nc; k++)
            if (d.j[idx[i]].arc[0]
                && !strcmp(d.j[idx[i]].arc, d.j[idx[k]].arc)) {
                CHECK(0, "arc %s listed twice", d.j[idx[i]].arc);
            }
    for (int i = 0; i < nc; i++) {
        int n = Cl13Thread(&d, idx[i], th, 4);
        int ok = n >= 1 && n <= 4;
        for (int k = 1; k < n; k++)
            ok &= d.j[th[k - 1]].step <= d.j[th[k]].step;
        if (d.j[idx[i]].arc[0])
            ok &= th[n - 1] == idx[i]
                || d.j[th[n - 1]].step >= d.j[idx[i]].step;
        if (!ok)
            CHECK(0, "thread of card %d (arc %s): %d steps out of order",
                  i, d.j[idx[i]].arc, n);
    }
    CHECK(1, "threads in order");
    // a hand made story: rows newest first, steps 3 2 1 and another story in between
    static Cl13T h;
    memset(&h, 0, sizeof h);
    char txt[] =
        "H\t2\t3\tW\t50\tSTEADY\t12\t3\nJ\t9\tc\tT3\tb3\tgood\ta1\t3\t9\t2\t\t\t0\nJ\t8\tc\tOTHER\tbo\t\t\t0\t8\t2\t\t\t0\nJ\t7\tc\tT2\tb2\t\ta1\t2\t7\t2\t\t\t0\nJ\t6\tc\tT1\tb1\t\ta1\t1\t6\t2\t\t\t0\n";
    CHECK(Cl13Parse(&h, 2, txt) == 4
          && h.nj == 4, "hand made journal parsed");
    int hc[8], ht[8];
    int hn = Cl13Cards(&h, hc, 8);
    CHECK(hn == 2 && hc[0] == 0
          && hc[1] == 1,
          "two cards: the story (newest step) and the single row");
    int tn = Cl13Thread(&h, 0, ht, 4);
    CHECK(tn == 3 && h.j[ht[0]].step == 1
          && h.j[ht[2]].step == 3, "thread oldest -> newest (%d steps)",
          tn);
    tn = Cl13Thread(&h, 0, ht, 2);
    CHECK(tn == 2 && h.j[ht[0]].step == 2
          && h.j[ht[1]].step == 3, "thread capped to the newest 2 steps");
    CHECK(Cl13Parse(&h, 2, (char[]) { "zzz" }) == -1,
          "garbage: not a reply");
    CHECK(Cl13Parse(&h, 6, (char[]) { "H" }) == -1,
          "bad section refused");
    char st5[] =
        "H\t2\t3\tW\t50\tSTEADY\t12\t3\nX\t3\tHill\t1\t2\nQ\tAnn\t0\t1\t5\t3\t2\t2\t1\t1\t320\t0\t104\nQ\tBob\t1\t0\t-1\t-1\t-1\t-1\t-1\t-1\t-1\t1\n";
    CHECK(Cl13Parse(&h, 5, st5) == 3 && h.xn == 2 && h.xday == 3
          && h.xwin == 1 && !strcmp(h.xmap, "Hill")
          && h.have[5], "match stats parsed");
    CHECK(!strcmp(h.x[0].name, "Ann") && h.x[0].me && h.x[0].kills == 5
          && h.x[0].deaths == 2 && h.x[0].parts == 320 && h.x[1].team == 1
          && h.x[1].kills == -1 && h.x[1].left, "match stats fields");
    CHECK(h.x[0].head == 104
          && h.x[1].head == 0,
          "head id read (an older daemon's 12-field row has none)");
    // MATCH STATS awards: US = team 0 (Ann me), THE ENEMY = team 1. 6 players: kills, downedBy, deaths, revives, execs, heads, parts
    {
        static Cl13XT xs[6];
        memset(xs, 0, sizeof xs);
#define XP(i, nm_, tm, ki, de, rv, ex, hs, pa) do { snprintf(xs[i].name, sizeof xs[i].name, "%s", nm_); xs[i].team = tm; xs[i].kills = ki; xs[i].downs = -1; xs[i].deaths = de; xs[i].revives = rv; xs[i].execs = ex; xs[i].heads = hs; xs[i].parts = pa; } while (0)
        XP(0, "Ann", 0, 6, 2, 1, 0, 1, 900);
        XP(1, "Cy", 0, 2, 5, 4, 0, 0, 700);
        XP(2, "Di", 0, 3, 3, 0, 3, 2, 500);
        XP(3, "Bob", 1, 5, 4, 0, 1, 0, 800);
        XP(4, "Ed", 1, 4, 1, 0, 0, 5, 400);
        XP(5, "Fay", 1, 0, 0, 0, 0, 0, 0);
        Cl13AwT aw[6];
        Cl13Awards(xs, 6, aw);
        CHECK(aw[2].stat == 1
              && aw[2].lead, "most executions: Di (%d)", aw[2].stat);
        CHECK(aw[1].stat == 2
              && aw[1].lead, "most revives: Cy (%d)", aw[1].stat);
        CHECK(aw[4].stat == 3
              && aw[4].lead, "most headshots: Ed (%d)", aw[4].stat);
        CHECK(aw[1].stat != 5
              && aw[3].stat != 5,
              "deaths award goes to the unique top (Cy has most deaths but took revives first)");
        CHECK(aw[0].stat == 0
              && aw[0].lead, "most downs: Ann (%d)", aw[0].stat);
        CHECK(aw[3].stat >= 0 && !aw[3].lead
              || aw[3].stat == 4,
              "Bob gets a relative stat or the parts lead (%d lead %d)",
              aw[3].stat, aw[3].lead);
        CHECK(aw[5].stat == -1, "no stats at all = no award");
        CHECK(!strcmp(Cl13AwWord(1), "EXECUTIONS")
              && !strcmp(Cl13AwRole(1, 1), "EXECUTIONER")
              && !strcmp(Cl13AwRole(-1, 0), "SURVIVOR"), "words");
        CHECK(!strcmp(Cl13AwExpr(0, 1), "angry")
              && !strcmp(Cl13AwExpr(2, 1), "perk")
              && !strcmp(Cl13AwExpr(5, 1), "dead")
              && !strcmp(Cl13AwExpr(0, 0), "neutral"), "faces");
        int idx[6], m0 = Cl13Order(xs, 6, 0, 0, idx);
        CHECK(m0 == 3 && idx[0] == 0 && idx[1] == 1
              && idx[2] == 2, "US sorted by parts: %d %d %d", idx[0],
              idx[1], idx[2]);
        int m1 = Cl13Order(xs, 6, 0, 1, idx);
        CHECK(m1 == 3 && idx[0] == 3
              && idx[2] == 5, "ENEMY sorted by parts");
        m0 = Cl13Order(xs, 6, 1, 0, idx);
        CHECK(m0 == 3
              && idx[0] == 3,
              "our team = team 1 when the asker stands there");
        int tie = 0;
        static Cl13XT ys[2];
        memset(ys, 0, sizeof ys);
        ys[0].kills = 3;
        ys[1].kills = 3;
        ys[0].parts = 20;
        ys[1].parts = 20;
        Cl13AwT ay[2];
        Cl13Awards(ys, 2, ay);
        tie = ay[0].stat == 0 && !ay[0].lead && ay[1].stat == 0
            && !ay[1].lead;
        CHECK(tie,
              "a tie leads nothing: both take their best relative stat");
    }
    CHECK(Cl13Parse(&h, 5, (char[]) { "" }) == 0 && h.xn == 0
          && h.xday == 0
          && h.have[5], "empty match stats = an older daemon");
    static Cl13T e;
    CHECK(Cl13Parse(&e, 1, (char[]) { "" }) == 0 && !e.ok
          && e.have[1], "empty reply = no clan");
    // the clipper
    char out[300];
    Cl13Clip(out, sizeof out,
              "one two three four five six seven eight nine ten", 1000.0f,
              10.0f, 3, Est);
    CHECK(!strcmp(out, "one two three four five six seven eight nine ten"),
          "short text unchanged: %s", out);
    const char *long_t =
        "It took a whole evening, one lost piece and an argument about the rules. Nobody remembers when the common room was last this loud. Nobody could settle on the Convention Center count";
    Cl13Clip(out, sizeof out, long_t, 300.0f, 10.0f, 2, Est);
    CHECK(strlen(out) > 20 && strlen(out) < strlen(long_t)
          && !strcmp(out + strlen(out) - 3, "..."),
          "long text cut with dots: '%s'", out);
    CHECK(Est(out, 10.0f) <= 2 * 300.0f + 40.0f,
          "cut text fits two lines (%.0f of 600)", Est(out, 10.0f));
    Cl13Clip(out, 16, long_t, 300.0f, 10.0f, 2, Est);
    CHECK(strlen(out) < 16, "tiny buffer never overruns ('%s')", out);
    // the journal's week pages (section 6; argv[4] = tools/clan_ingame.build(section 6) of a seeded clan)
    if (argc > 4) {
        char *t6 = Slurp(argv[4]);
        static Cl13T w;
        int n6 = t6 ? Cl13Parse(&w, 6, t6) : -1;
        CHECK(n6 > 3 && w.have[6] && w.nd > 0 && w.jtop >= 1
              && w.jweek >= 1
              && w.jweek <= w.jtop,
              "week pages: %d rows, %d entries, %d paragraphs, week %d of %d",
              n6, w.nd, w.ng, w.jweek, w.jtop);
        int ok = 1, idx[C13_ND], tot = 0;
        for (int i = 1; i < w.nd; i++)
            ok &= w.d[i].day > w.d[i - 1].day;
        CHECK(ok, "entries are oldest first, one per clan day");
        for (int wk = 1; wk <= w.jtop; wk++) {
            int n = Cl13WeekRows(&w, wk, idx, C13_ND);
            tot += n;
            ok &= n <= 8 && (wk == w.jtop || n >= 1);
        }
        CHECK(ok
              && tot == w.nd,
              "every entry is on exactly one week page, at most 8 rows a page (%d of %d)",
              tot, w.nd);
        int n = Cl13WeekRows(&w, w.jweek, idx, C13_ND), withtext =
            0, pi[C13_NG];
        for (int i = 0; i < n; i++) {
            int np = Cl13DayParas(&w, w.d[idx[i]].day, pi, C13_NG);
            if (np)
                withtext++;
            for (int k = 0; k < np; k++)
                ok &= w.g[pi[k]].text[0] != 0;
        }
        CHECK(ok
              && withtext >= 1,
              "the shown week carries its paragraphs (%d of %d days with text)",
              withtext, n);
        for (int i = 0; i < w.nd; i++)
            ok &= w.d[i].title[0] != 0
                && w.d[i].week == (w.d[i].day >
                                   0 ? (w.d[i].day - 1) / 7 + 1 : 1);
        CHECK(ok, "every entry has a headline and its week");
        CHECK(Cl13Parse(&w, 6, (char[]) { "" }) == 0 && w.nd == 0
              && w.have[6],
              "empty week pages = an older daemon (the clan itself stays)");
    }
    // the wrapper of the journal's reading pane
    {
        char wr[600];
        int n =
            Cl13Wrap(wr, sizeof wr,
                      "one two three four five six seven eight nine ten",
                      100.0f, 10.0f, 9, Est);
        int brk = 0;
        for (char *q = wr; *q; q++)
            if (*q == '\n')
                brk++;
        CHECK(n == brk + 1
              && n >= 3, "wrap: %d lines, %d line breaks", n, brk);
        int fits = 1;
        char tmp[600];
        snprintf(tmp, sizeof tmp, "%s", wr);
        for (char *ln = strtok(tmp, "\n"); ln; ln = strtok(NULL, "\n"))
            fits &= Est(ln, 10.0f) <= 100.0f;
        CHECK(fits, "wrap: every line fits the box");
        n = Cl13Wrap(wr, sizeof wr, long_t, 300.0f, 10.0f, 3, Est);
        CHECK(n == 3
              && !strcmp(wr + strlen(wr) - 3, "..."),
              "wrap: cut at 3 lines with dots: '%s'", wr);
        n = Cl13Wrap(wr, sizeof wr, "first part\n\nsecond part", 1000.0f,
                      10.0f, 9, Est);
        CHECK(n == 3
              && !strcmp(wr, "first part\n\nsecond part"),
              "wrap: a blank line stays a blank line (%d lines)", n);
        n = Cl13Wrap(wr, 12, long_t, 300.0f, 10.0f, 5, Est);
        CHECK(strlen(wr) < 12, "wrap: tiny buffer never overruns ('%s')",
              wr);
        CHECK(Cl13Wrap(wr, sizeof wr, "", 300.0f, 10.0f, 5, Est) == 0
              && !wr[0], "wrap: empty text = no lines");
    }
    Cl13SeenT sn;
    CHECK(Cl13SeenParse("215 7", &sn) && sn.jid == 215
          && sn.day == 7, "seen file parsed");
    printf(fails ? "\n%d failed\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
