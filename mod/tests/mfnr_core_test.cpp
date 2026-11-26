#include "mfnr-core.hpp"

#include <cstring>
#include <iostream>
#include <string>

using namespace net::ui::mfnr::core;

namespace {

    int failed = 0;

    void Check(bool ok, const char* what)
    {
        if (!ok) ++failed;
        std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    }

} // namespace

int main()
{
    std::cout << "mfnr core:\n";

    {
        Vote m;
        Reset(m);
        Check(!m.ok && m.my == -1 && m.w == -1, "a reset state has no vote");
        for (int i = 0; i < kPlayers; ++i)
            Check(m.pk[i] == -1 && m.po[i] == -1, "rank / option start unresolved");
    }

    {
        Vote m;
        Parse(m, "st=1 n=3 m0=4 m1=5 m2=6 v0=2 v1=0 v2=1 my=-1 w=-1 pc=2 p0=ELLIE p1=JOEL pv0=1 pv1=0 po0=0 po1=1 pk0=3 pw0=6");
        Check(m.ok && m.st == 1 && m.n == 3, "the live server's three options parse");
        Check(m.m[0] == 4 && m.m[1] == 5 && m.m[2] == 6, "option map ids parse");
        Check(m.v[0] == 2 && m.v[1] == 0 && m.v[2] == 1, "vote counts parse");
        Check(m.my == -1 && m.w == -1 && m.pc == 2, "my / winner / player count parse");
        Check(std::strcmp(m.p[0], "ELLIE") == 0 && std::strcmp(m.p[1], "JOEL") == 0,
              "roster names parse");
        Check(m.pv[0] == 1 && m.pv[1] == 0, "voted flags parse");
        Check(m.po[0] == 0 && m.po[1] == 1, "voted options parse");
        Check(m.pk[0] == 3 && m.pw[0] == 6, "rank / weeks parse");
    }

    {
        Vote m;
        Parse(m, "n=99 pc=99 p0=THE_LAST_OF_US m23=7 v23=2 m24=9 v24=9 pk8=9 po8=9");
        Check(m.n == kGrid && m.pc == kPlayers, "n clamps to the grid, pc to the roster");
        Vote neg;
        Parse(neg, "n=-2");
        Check(neg.n == 0, "a negative n clamps to zero");
        Check(std::strcmp(m.p[0], "THE LAST OF US") == 0, "underscores in names become spaces");
        Check(m.m[23] == 7 && m.v[23] == 2, "option 23 is the last accepted option");
        Check(m.my == -1, "unknown option indices are ignored");
        Check(m.pk[7] == -1 && m.po[7] == -1, "out-of-range roster rows are ignored");
    }

    {
        Vote m;
        Parse(m, "n=20 p7=ABCDEFGHIJKLMNOPQRSTUVWX");
        Check(std::strlen(m.p[7]) == 20 && m.p[7][19] == 'T', "a long name is copied to 20 chars");
    }

    {
        Vote m;
        Parse(m, "garbage");
        Check(m.ok && m.n == 0 && m.my == -1 && m.w == -1,
              "an empty reply is a valid 'no vote yet' state");
    }

    {
        Vote m;
        Parse(m, "st=1 n=3 m0=4 m1=5 m2=6 v0=1 v1=0 v2=0 my=0 w=-1 pc=1");
        Check(!ApplyLocalVote(m, 0, 4), "a repeated vote is not re-applied");
        Check(ApplyLocalVote(m, 1, 5), "a new vote applies at once");
        Check(m.my == 1 && m.v[0] == 0 && m.v[1] == 1, "the old vote is moved to the new tile");
        Check(!ApplyLocalVote(m, 1, 9), "a stale option / map pair is ignored");
        Vote closed = m;
        closed.st = 2;
        Check(!ApplyLocalVote(closed, 2, 6), "a decided vote is not optimistic");
    }

    {
        Vote m;
        Parse(m, "st=1 n=3 m0=4 v0=8 my=0 pc=1");
        Check(ApplyLocalVote(m, 1, 0), "a different option with the same map still applies");
        Check(m.v[1] == 1, "the optimistic increment stops at the roster size");
    }

    {
        Check(std::strcmp(MapName(0), "Forest") == 0, "map 0 is Forest");
        Check(std::strcmp(MapName(24), "Plaza (Part I)") == 0, "map 24 is the last built-in");
        Check(std::strcmp(MapName(25), "Forest") == 0, "out-of-range map ids fall back to map 0");
        Check(std::strcmp(MapImage(11), "tv-station") == 0, "map images use the ext's slugs");
        char path[160];
        MapImagePath(path, sizeof path, 11);
        Check(std::strcmp(path, "streaming/menus/to1/map-pvp-tv-station.png") == 0,
              "map images use the generated map image path");
        Check(IsPartOne(17) && IsPartOne(24) && !IsPartOne(11),
              "Part I maps are recognised by their image slug");
        Check(std::strcmp(ModeLabel(), "SUPPLY RAID") == 0, "the default mode label is kept");
    }

    {
        Check(StageOf(0) == 0 && StageOf(99) == 0, "no flow below code 100");
        Check(StageOf(100) == 0 && StageOf(129) == 0, "100..129 is not a flow stage");
        Check(StageOf(130) == 2 && StageOf(159) == 2, "130..159 is the booster stage");
        Check(StageOf(160) == 1 && StageOf(199) == 1, "160..199 is the map vote stage");
        Check(StageOf(200) == 3 && StageOf(250) == 3, "200+ is the loading stage");
        Check(FlowActive(100) && !FlowActive(99), "flow activity pins at code 100");
        Check(CountdownFor(173, 1) == 13 && CountdownFor(142, 2) == 12,
              "the countdown is the code above its stage base");
        Check(CountdownFor(160, 1) == 0 && CountdownFor(200, 3) == 0,
              "the base code has no countdown");
    }

    {
        // The ext match horn: the raw code must have reached a stage before
        // its fall to <=1 asks for LOADING, and only phase 2 / state 2 counts.
        Check(HornWant(true, 1, 2, 2) && HornWant(true, 0, 2, 2),
              "the horn fires once the stage countdown reaches <=1");
        Check(!HornWant(true, 2, 2, 2) && !HornWant(true, 130, 2, 2),
              "a still-running countdown does not fire the horn");
        Check(!HornWant(true, 1, 1, 2) && !HornWant(true, 1, 2, 3)
              && !HornWant(true, 1, 2, 0),
              "the horn needs phase 2 and match state 2");
        Check(!HornWant(false, 1, 2, 2) && !HornWant(false, 0, 2, 2),
              "a flow that was never seen cannot fire the horn");
        Check(!HornWant(true, 200, 2, 2),
              "stage 3 is a direct take, not a horn countdown");
        Check(!HornCanceled(2, 2) && HornCanceled(1, 2)
              && HornCanceled(2, 3) && HornCanceled(2, 0),
              "only leaving phase 2 / state 2 cancels the latch");
    }

    {
        Vote m;
        Parse(m, "st=2 n=3 m0=4 m1=5 m2=6 v0=0 v1=3 v2=0 my=1 w=1 pc=3");
        Check(Decided(m), "a known winning map decides the vote");
        Vote pack = m;
        pack.w = 25;
        Check(!Decided(pack), "an unported pack map cannot decide locally");
        Vote open = m;
        open.st = 1;
        Check(!Decided(open), "an open vote is not decided");
    }

    {
        Check(PromptKey(false, false, false) == 0, "no prompt bits with keyboard / tiles / closed vote");
        Check(PromptKey(true, false, false) == 1, "pad is bit 1");
        Check(PromptKey(false, true, false) == 2, "grid is bit 2");
        Check(PromptKey(false, false, true) == 4, "can-vote is bit 4");
        Vote m;
        Reset(m);
        Check(PromptKeyOf(m, false) == 0, "no vote state has no prompt");
        Parse(m, "st=1 n=3 pc=1");
        Check(PromptKeyOf(m, false) == 4, "the live three-tile vote enables VOTE");
        m.n = 20;
        Check(PromptKeyOf(m, true) == 7, "the all-maps grid enables pad + grid + VOTE");
        Check(PromptHit(7, 1) && !PromptHit(7, 0) && !PromptHit(3, 1),
              "the VOTE prompt needs the can-vote bit and hover 1");
    }

    {
        Vote m;
        Parse(m, "st=1 n=20 pc=0");
        Check(GridMode(m), "more than three options use the all-maps grid");
        m.n = 3;
        Check(!GridMode(m) && HaveVote(m), "three options use the map tiles");
        Vote empty;
        Check(!HaveVote(empty), "no server state has no tiles");
    }

    {
        int tabs[kPlayers] = {2, 0, 1, 0, 0, 0, 0, 0};
        int ord[kPlayers]{};
        const int n = RosterOrder(tabs, 3, ord);
        Check(n == 3 && ord[0] == 2 && ord[1] == 1 && ord[2] == 0,
              "the roster sorts our team, unknown, then enemy");
        Vote empty;
        Check(VoteOf(empty, -1, false) == -1, "an invalid slot has no vote");
    }

    {
        Vote m;
        Parse(m, "pc=5 pv0=1 pv1=0 pv2=1 pv3=1 pv4=0");
        char t[32];
        VotedText(t, sizeof t, m);
        Check(std::strcmp(t, "3 | 5 VOTED") == 0, "the section counts voted / players");
    }

    {
        CellBar c = CellOf(0, 3);
        char t[64];
        CountText(t, sizeof t, c);
        Check(c.grey && std::strcmp(t, "<font color=\"#8C8C8C\">0 | 3</font>") == 0,
              "an empty tile's count is grey");
        c = CellOf(1, 3);
        CountText(t, sizeof t, c);
        Check(!c.grey && std::strcmp(t, "1 | 3") == 0, "a voted tile's count is plain");
        Check(c.pc == 3 && c.frac > 0.33f && c.frac < 0.34f,
              "the bar's denominator is the player count");
        c = CellOf(4, 2);
        Check(c.pc == 4 && c.frac == 1.0f, "the denominator grows to the highest vote count");
        c = CellOf(0, 0);
        Check(c.pc == 1 && c.frac == 0.0f, "a zero denominator is clamped");
    }

    {
        Vote m;
        Parse(m, "st=2 n=3 m0=4 m1=5 m2=6 w=5 my=1 pc=1");
        Check(CellFocused(m, 1, -1) && !CellFocused(m, 0, 0),
              "a decided vote focuses the winning tile");
        Check(CellMine(m, 1) && !CellMine(m, 0), "the local vote chip uses `my`");
        Check(CellVisible(m, 2) && !CellVisible(m, 3) && !CellVisible(m, -1),
              "only option 0..n-1 is drawn");
        Check(!CellVisible(m, kGrid), "the all-maps grid has exactly 20 cells");
        m.st = 1;
        Check(CellFocused(m, 2, 2) && !CellFocused(m, 1, 2),
              "an open vote focuses the hovered tile");
    }

    {
        Check(IsMe("ELLIE", "ellie") && IsMe("[TAG] ELLIE", "Ellie") && IsMe("S ELLIE", "Ellie"),
              "own name matches case-insensitively, with a tag or separator");
        Check(!IsMe("JOEL", "Ellie") && !IsMe("XELLIE", "Ellie") && !IsMe("", "Ellie"),
              "a different name or extra characters are another player");
    }

    {
        Vote m;
        Parse(m, "st=1 n=3 m0=4 m1=5 my=1 pc=2 p0=ELLIE p1=JOEL po0=0 po1=1");
        Check(VoteOf(m, 1, false) == 1, "a roster row shows its own voted option");
        Check(VoteOf(m, 0, true) == 1, "our row uses the optimistic vote");
        Check(RosterMapVisible(m, 1, false, true), "our row's map is always shown");
        Check(!RosterMapVisible(m, 0, false, false), "another player's row needs a voted flag");
        char t[128];
        VotedMapText(t, sizeof t, m, 1, m.pv[1] != 0, true);
        Check(std::strcmp(t, "<font color=\"#D9D86A\">Seraphite Camp</font>") == 0,
              "our row's voted map is highlighted");
        VotedMapText(t, sizeof t, m, 0, false, false);
        Check(t[0] == 0, "an unvoted row has no map text");
    }

    {
        int week = 0, day = 0;
        WeekOf(1, week, day);
        Check(week == 1 && day == 2, "day 1 is week 1, day 2 of the week");
        WeekOf(7, week, day);
        Check(week == 2 && day == 1, "day 7 starts week 2");
        WeekOf(83, week, day);
        Check(week == 12 && day == 7, "day 83 is the last day of the last week");
        WeekOf(999, week, day);
        Check(week == 12 && day == 7, "later days clamp to the last week");
        Check(SurvivorTotal(2, 1, 3) == 6, "the survivor total sums all three states");
        Check(SurvivorFrac(2, 6) > 0.33f && SurvivorFrac(0, 0) == 0.0f,
              "survivor bars are fractions with a safe zero total");
    }

    {
        int total = 0;
        Check(TimerMax(total, 15) == 15 && total == 15, "the timer starts at the countdown");
        Check(TimerMax(total, 4) == 4 && total == 15, "a later tick cannot shrink the bar");
        Check(TimerMax(total, 20) == 20 && total == 20, "a larger countdown grows the bar");
        Check(TimerFrac(total, 5) > 0.249f && TimerFrac(total, 5) < 0.251f,
              "the bar is the fraction of the longest countdown");
        Check(TimerFrac(0, 3) == 0.0f, "an unknown total is an empty bar");
    }

    {
        // voted=0, pending=false, open=true, busy=false, my=1 (server disagrees),
        // voted map 4 still at option 0, state version 2 > send version 1.
        Check(NeedResend(0, false, true, false, 1, 4, 4, 3, 2, 1, 0),
              "a stale state re-sends the same map option");
        Check(!NeedResend(-1, false, true, false, 1, 4, 4, 3, 2, 1, 0),
              "no click means no resend");
        Check(!NeedResend(0, false, false, false, 1, 4, 4, 3, 2, 1, 0),
              "a decided vote means no resend");
        Check(!NeedResend(0, false, true, true, 1, 4, 4, 3, 2, 1, 0),
              "an in-flight request means no resend");
        Check(!NeedResend(0, true, true, false, 1, 4, 4, 3, 2, 1, 0),
              "a newer click means no stale resend");
        Check(!NeedResend(0, false, true, false, 0, 4, 4, 3, 2, 1, 0),
              "the server already has the vote");
        Check(!NeedResend(0, false, true, false, 1, 4, 4, 0, 2, 1, 0),
              "the option must still be in this vote");
        Check(!NeedResend(0, false, true, false, 1, -1, 4, 3, 2, 1, 0),
              "an unported pack map is not re-sent locally");
        Check(!NeedResend(0, false, true, false, 1, 5, 4, 3, 2, 1, 0),
              "a different map is not re-sent");
        Check(!NeedResend(0, false, true, false, 1, 4, 4, 3, 1, 1, 0),
              "an unchanged state is not re-sent");
        Check(!NeedResend(0, false, true, false, 1, 4, 4, 3, 2, 1, kMaxResend),
              "resends are capped at four");
    }

    // ---- stage 2: 0xE8 / 0xEE / 0xE9 meta and the booster list --------------
    {
        Meta m;
        Reset(m);
        Check(!m.ok && m.lmd == -1, "no meta reply leaves a zero state and lmd = -1");
        ParseMeta(m, "");
        Check(m.ok && !m.bc[1] && m.bp == 0, "an empty meta reply is still published");
    }

    {
        Meta m;
        ParseMeta(m, "v=1 bc1=2 bc3=1 bp=3 ba=0 bn=4 mi=1 mp=5 ml=1 mr1=2 mg1=6 mg2=9 mg3=12 mev=1 mf=8 p1=7 lmd=2 junk=9");
        Check(m.ok && m.bc[1] == 2 && m.bc[3] == 1 && m.bp == 3 && m.ba == 0
              && m.bn == 4, "booster counts / pick / survivors parse");
        Check(m.mi == 1 && m.mp == 5 && m.ml == 1 && m.mr[1] == 2
              && m.mg[0] == 6 && m.mg[1] == 9 && m.mg[2] == 12 && m.mev == 1,
              "mission progress / server goals parse");
        Check(m.mf == 8 && m.p[0] == 7 && m.lmd == 2, "flow seconds / offer / last day parse");

        Meta clamped;
        ParseMeta(clamped, "mf=999 bc8=4 mr64=7 p1=99");
        Check(clamped.mf == 0 && clamped.bc[7] == 0 && clamped.mr[63] == 0
              && clamped.p[0] == 0, "out-of-range meta values are ignored or clamped");

        int ids[8]{};
        const int n = BoosterList(m, ids);
        Check(n == 3 && ids[0] == 0 && ids[1] == 1 && ids[2] == 3,
              "the list is Nothing then owned boosters in id order");
        Check(BoosterSlotId(ids, n, 2) == 3 && BoosterSlotOf(ids, n, 3) == 2,
              "focus is a slot index, not a booster id");

        char t[160];
        BoosterConfirmText(t, sizeof t, m, 3, n);
        Check(std::strcmp(t, "CHOSEN: Cheaper Armor") == 0,
              "the focused / picked booster shows CHOSEN");
        BoosterConfirmText(t, sizeof t, m, 1, n);
        Check(std::strcmp(t, "CONFIRM") == 0, "another owned booster shows CONFIRM");
        Meta none;
        Reset(none);
        ParseMeta(none, "bn=4");
        BoosterConfirmText(t, sizeof t, none, 1, 1);
        Check(std::strcmp(t, "NO BOOSTERS - 4 MORE SURVIVORS FOR THE NEXT ONE") == 0,
              "an empty list tells how many survivors until the next booster");
        BoosterConfirmText(t, sizeof t, none, 0, 1);
        Check(std::strcmp(t, "CHOSEN: NOTHING") == 0,
              "focusing Nothing with no pick says CHOSEN: NOTHING");
        none.ok = 0;
        BoosterConfirmText(t, sizeof t, none, 0, 1);
        Check(std::strcmp(t, "CONFIRM") == 0, "no meta reply shows a plain CONFIRM");

        BoosterTitleText(t, sizeof t, 4);
        Check(std::strcmp(t,
              "Choose One-Use Booster - <font color=\"#e04848\">00:04</font>") == 0,
              "the booster title colours the seconds");
        BoosterIconPath(t, sizeof t, 7);
        Check(std::strcmp(t, "streaming/t2r/icons/booster-parts-multiplier.png") == 0,
              "booster icons use the pinned slugs");
        BoosterCountText(t, sizeof t, 2);
        Check(std::strcmp(t, "x2") == 0, "owned counts use the xN format");
    }

    // ---- stage 3: loading missions, events, tips, release fade -------------
    {
        Meta m;
        ParseMeta(m, "mi=1 mp=5 ml=1 mr1=2 mg1=6 mg2=9 mg3=12 mev=1");
        char t[160];
        MissionLine(t, sizeof t, m, 1);
        Check(std::strcmp(t, "5/6 Down Enemies") == 0,
              "mission progress is the next unmet tier, not the top goal");
        m.mp = 12;
        MissionLine(t, sizeof t, m, 1);
        Check(std::strcmp(t, "12/12 Down Enemies - done") == 0,
              "a completed mission shows the top goal reached");
        MissionDayLine(t, sizeof t, m);
        Check(std::strcmp(t, "Day 3/3") == 0, "the mission day uses 4 - ml");
        MissionTierLine(t, sizeof t, m, 1, 0);
        Check(std::strcmp(t, "6 Downs") == 0, "tier goals use the mission unit");

        Check(EventKindAt(15) == 2 && EventKindAt(10) == 1 && EventKindAt(7) == 0,
              "day 15 is an attack, day 10 growth, day 7 no event");
        Check(EventWinStart(16) == 15 && EventNextStart(12) == 15,
              "event windows / next event resolve from the calendar");
        Check(EventLossPct(15, 1) == 40 && EventLossPct(35, 1) == 60,
              "early attacks use a lower loss table");
        Check(std::strcmp(EventGrowthPct(2), "+10%") == 0,
              "growth tiers pay +5 / +7.5 / +10 percent");
        NextEventLine(t, sizeof t, 13, 15);
        Check(std::strcmp(t, "Next event in 2 days") == 0,
              "the no-mission line is a short relative day count");
        NextEventLine(t, sizeof t, -1, -1);
        Check(std::strcmp(t, "No more events") == 0, "no future event is reported");

        Check(kTipCount == 29 && TipIndex(0) == 0 && TipIndex(29000) == 29 % kTipCount,
              "tips rotate over the fixed 29-entry table");
        Check(TipNext(kTipCount - 1) == 0, "tip rotation wraps");
        TipPicPath(t, sizeof t, 28, 1);
        Check(std::strcmp(t, "streaming/t2r/clan/tip-29-2.png") == 0,
              "the CLAN tip rotates its three pictures");
        TipCatText(t, sizeof t, 0);
        Check(std::strcmp(t, "WEAPONS:") == 0, "tip categories carry a colon");
        SpinText(t, sizeof t, 800);
        Check(std::strcmp(t, "LOADING..") == 0, "loading dots use a 400 ms step");
        Check(ReleaseAlpha(0) == 1.0f && ReleaseAlpha(300) == 1.0f
              && ReleaseAlpha(500) == 0.5f && ReleaseAlpha(1000) == 0.0f,
              "release holds black, then fades the group out");
    }

    std::cout << "mfnr core: " << (failed ? "FAILED" : "all passed") << '\n';
    return failed ? 1 : 0;
}
