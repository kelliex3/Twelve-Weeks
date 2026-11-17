// ---------------------------------------------------------------- overtime (with only 1 person standing, the timer should not end the match)
// re/overtime-1028.md. HOW A MATCH ENDS ON TIME (winmm, re/winmm_text.asm): TickMatchAuthority FUN_180034050 runs every frame on every
// connected peer. It ticks the match timer (ctx+0x2b8c: +0 duration, +4 remaining, +8 running, +9 expired; FUN_18002c120 counts +4 down and
// at <= 0 clears running and sets expired). Host only (FUN_18005da50, not ended FUN_180062a50): event 6 (timer + team lives) every 1 s, then
// expired = FUN_18002c170 and the elimination checks FUN_180034340(1 / 0) (team lives <= 0 AND none of its players alive, held for the
// 0x18009f490 threshold). No expiry and no elimination -> return. Otherwise winner = the eliminated team's enemy, or on time: more team
// lives (Match::BlueLives 0x18004C120 vs RedLives 0x18004C150), equal = 2 (draw); event 8 + match end FUN_180062b00 (sets ctx+0x2ca4).
// So on time the lives rule decides even while a team's last player (0 lives left, 2013 "Sudden Death" / "Last Man Standing") is still
// fighting, and two teams both at 0 lives end in a DRAW.
// RULE (2013 Supply Raid ends only by elimination; once a team is out of lives the match is decided by fighting - Sudden Death /
// Last Man Standing, re/factions2013/Supply_Raid.wiki; the repo's 2013 refs give no time-expiry rule): when the clock runs out while
// either team has 0 lives left (it can still be eliminated: the fight decides), the round goes on in OVERTIME until a team is eliminated
// (winmm's own elimination end), with no time cap (UNTIL DEATH below); only an opt-out releases the clock, runs out and the normal time-out rule decides.
// With lives left on both sides the clock ends the match as before. Not in Interrogation (2013: no lives, no sudden death there).
// MECHANISM: every peer holds the timer's remaining time at OT_FLOOR while overtime runs (+4 held, +0 raised by the same amount once at
// the start; running / expired are never touched), so FUN_18002c120 never expires it and the host's authority keeps waiting for an elimination. The host's event 6
// carries the held value to the clients every second (their own copy is held too, so a host migration in overtime keeps it).
// HUD: g_ot_hud -> matchTimer reads OVERTIME (timer_tick_text, red, no tick) and the pause menu clock too. A hitch longer than OT_FLOOR
// in one frame would still expire the clock (the dt of that frame is larger than what we hold).
// Log: "overtime:". t2r-no-overtime.txt next to the exe = off (only the HOST's file decides the match; a client with it only sees the clock).
// UNTIL DEATH (overtime lasts until death, allowing a full comeback if needed; the reports of "overtime ends for no reason" were
// every one the old 60 s cap releasing the clock with both teams alive). Overtime
// has NO time cap and NO countdown: it ends only when a team is eliminated - winmm's own check FUN_180034340 (team lives <= 0, at least one
// player of the team in the match, none of them alive; a downed player still counts as alive, so a revive keeps the team in it), which picks
// the survivor as the winner, or when a team has no players left in the match at all (everyone of it quit: winmm's check needs >= 1 player
// of the team, so an emptied team would never be "eliminated" - ot_empty_team ends the match for the other team on every peer, as
// interrogation.c does). No time-out rule, no lives tie-break. The HUD reads OVERTIME without a clock. The held value is OT_FLOOR s, with the
// duration raised by the same amount at the start (elapsed = duration - remaining unchanged: the respawn-wave clock does not jump), so a hitch
// of up to OT_FLOOR s in one frame cannot expire the clock any more (it was 2 s). Released only by: t2r-no-overtime.txt (host), both teams
// having lives again (then nobody can be eliminated), or t2r-ot-fixed-cap.txt = the old 60 s cap (shown as a countdown).
#define OT_START 2.0f           // s on the clock when overtime starts (the buzzer, as before)
#define OT_FLOOR 30.0f          // s kept on the clock while overtime runs (< the daemon's JIP_MIN_LEFT 90 s: still never offered to joiners)
#define OT_MAX_S 60             // t2r-ot-fixed-cap.txt only: s of overtime, then the time-out rule (the old rule)
#define OT_EMPTY_MS 5000        // a team with no player in the match for this long (host migration hiccups are shorter) = it left: the other team wins
static struct {
    int state;
    uint64_t t0, last_log, empty_since;
    int al0, en0, said_no, had[2], empty_team, ended_empty;
} g_ot;                         // state 0 none, 1 holding, 2 released (opt-out / lives back)
static int OtFixedCap(void)
{
    static int on = -1;
    static uint64_t chk;
    uint64_t now = GetTickCount64();
    if (on < 0 || now - chk > 2000) {
        chk = now;
        FILE *f = ExeSideOpen("t2r-ot-fixed-cap.txt", "r");
        on = f != NULL;
        if (f)
            fclose(f);
    }
    return on;
}

// players in the match per team (the server's roster in the lobby object; another slot only while winmm still has its net player)
static void OtTeamCounts(int n[2])
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    n[0] = n[1] = 0;
    uint8_t *o = LobbyObj();
    if (!o)
        return;
    uint32_t self = *(volatile uint32_t *) T2R_SYM(kMySlot);
    int cnt = o[0x1a] > 8 ? 8 : o[0x1a];
    for (int i = 0; i < cnt; i++) {
        const uint8_t *e = o + 0x1d + i * 0x32;
        uint32_t s = e[0];
        if (s >= T2R_MAX_PEERS)
            continue;
        uint8_t *pr = s == self ? NULL : ((fn_peer) T2rPeer) (s);
        if (s == self || (pr && pr[4]))
            n[e[0x31] & 1]++;
    }
}

static int OtOff(void)
{
    static int off = -1;
    static uint64_t chk;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - chk > 2000) {
        chk = now;
        FILE *f = ExeSideOpen("t2r-no-overtime.txt", "r");
        int o = f != NULL;
        if (f)
            fclose(f);
        if (o != off)
            Xlog("overtime: %s", o ? "off (t2r-no-overtime.txt)" : "on");
        off = o;
    }
    return off;
}

static const char *OtRole(void)
{
    uint8_t r = *(volatile uint8_t *) T2R_SYM(kRoleByte);
    return r == 1 ? "host" : r == 2 ? "client" : "none";
}

static void OtReset(void)
{
    memset(&g_ot, 0, sizeof g_ot);
    g_otHud = 0;
    g_otLeft = -1;
}

static void FeatureOvertime(void)
{
    uint8_t *lo = LobbyObj();
    if (!lo || IsBadReadPtr(lo + 0x2b8c, 0x2ca8 - 0x2b8c)) {
        if (g_ot.state)
            OtReset();
        return;
    }
    uint64_t now = GetTickCount64();
    if (MatchSubstate() != 6 || lo[0x2ca4]) {
        if (g_ot.state) {
            if (lo[0x2ca4])
                Xlog("overtime: END after %.1f s - the match ended, winner team %d (2 = draw), team lives %d / %d (%s)", (now - g_ot.t0) / 1000.0, *(int *) (lo + 0x2ca0), ((int (*)(void *)) T2R_SYM(kFnTeamLives)) (lo), ((int (*)(void *)) T2R_SYM(kFnTeamLivesEnabled)) (lo), OtRole());
            else
                Xlog("overtime: END after %.1f s - left the match (state %u)", (now - g_ot.t0) / 1000.0, MatchSubstate());
        }
        if (g_ot.state || g_ot.said_no)
            OtReset();
        return;
    }
    float *tm = (float *) (lo + 0x2b8c);        // [0] duration, [1] remaining
    int run = lo[0x2b94];
    // DEV (the A/B test bench lost every batch to the 15 min clock - the match ended to the menu and autofind does not requeue):
    // t2r-dev-match-minutes.txt next to the exe holding N = the running match's clock is set to N minutes once per match (duration and remaining
    // raised by the same amount: elapsed unchanged, the respawn-wave clock does not jump). Every peer applies it to its own copy; the host's event 6
    // carries it. Absent (every real install) = unchanged.
    {
        static int dev_min = -1;
        static uint64_t dev_chk;
        static float dev_done;
        if (dev_min < 0 || now - dev_chk > 3000) {
            dev_chk = now;
            FILE *f = ExeSideOpen("t2r-dev-match-minutes.txt", "r");
            int v = 0;
            if (f) {
                if (fscanf(f, "%d", &v) != 1)
                    v = 0;
                fclose(f);
            }
            dev_min = v > 0 && v <= 600 ? v : 0;
        }
        if (dev_min && run && tm[0] > 0.0f && g_ot.state == 0
            && dev_done != tm[0]) {
            float want = (float) dev_min * 60.0f;
            if (tm[0] < want) {
                float add = want - tm[0];
                tm[0] += add;
                tm[1] += add;
                Xlog("overtime: DEV t2r-dev-match-minutes.txt - the match clock is %d min now (+%.0f s, %.0f s left)", dev_min, add, tm[1]);
            }
            dev_done = tm[0];
        }
    }
    int al = ((int (*)(void *)) T2R_SYM(kFnTeamLives)) (lo), en =
        ((int (*)(void *)) T2R_SYM(kFnTeamLivesEnabled)) (lo);
    int sd = al <= 0 || en <= 0;        // a team is out of lives: Sudden Death / Last Man Standing, only elimination can end it
    if (g_ot.state == 0) {
        if (!run || tm[0] <= 0.0f || tm[1] > OT_START)
            return;
        if (!sd || R13IntMode() || OtOff()) {
            if (!g_ot.said_no) {
                g_ot.said_no = 1;
                Xlog("overtime: not started - clock %.1f s, team lives %d / %d%s%s: the time-out rule decides", tm[1], al, en, R13IntMode()? ", Interrogation" : "", OtOff()? ", t2r-no-overtime.txt" : "");
            }
            return;
        }
        g_ot.state = 1;
        g_ot.t0 = g_ot.last_log = now;
        g_ot.al0 = al;
        g_ot.en0 = en;
        g_otHud = 1;
        g_otLeft = OtFixedCap()? OT_MAX_S : -1;
        if (run && tm[1] < OT_FLOOR) {
            float d = OT_FLOOR - tm[1];
            tm[0] += d;
            tm[1] += d;
        }                       // margin against a long frame; elapsed kept
        {
            int n[2];
            OtTeamCounts(n);
            g_ot.had[0] = n[0] > 0;
            g_ot.had[1] = n[1] > 0;
            Xlog("overtime: START - clock %.2f s of %.0f s, team lives %d (ours) / %d (theirs), players team0 %d team1 %d: a team is out of lives, the round " "goes on until a team is eliminated (no time cap%s) (%s)", tm[1], tm[0], al, en, n[0], n[1], OtFixedCap()? "; t2r-ot-fixed-cap.txt: the old 60 s cap" : "", OtRole());
        }
    }
    if (g_ot.state != 1) {
        if (g_otLeft > 0)
            g_otLeft = 0;
        return;
    }                           // released: the clock runs out by itself, OVERTIME stays on the HUD until the end
    const char *why = NULL;
    if (OtFixedCap() && now - g_ot.t0 >= OT_MAX_S * 1000ULL)
        why = "t2r-ot-fixed-cap.txt: 60 s cap reached";
    else if (!sd)
        why = "both teams have lives again (no team can be eliminated)";
    else if (OtOff())
        why = "t2r-no-overtime.txt";
    if (why) {
        g_ot.state = 2;
        if (g_otLeft >= 0)
            g_otLeft = 0;
        if (run && tm[1] > 2.0f)
            tm[1] = 2.0f;
        Xlog("overtime: RELEASED after %.1f s (%s) - the clock runs out from %.1f s and the time-out rule decides, team lives %d / %d (%s)", (now - g_ot.t0) / 1000.0, why, tm[1], al, en, OtRole());
        return;
    }
    if (run && tm[1] < OT_FLOOR)
        tm[1] = OT_FLOOR;
    if (OtFixedCap()) {
        uint64_t e = now - g_ot.t0;
        g_otLeft =
            e >=
            OT_MAX_S *
            1000ULL ? 0 : (int) ((OT_MAX_S * 1000ULL - e + 999) / 1000);
    } else
        g_otLeft = -1;
    {                           // a team that left the match entirely (both had players at the start): winmm's elimination needs >= 1 player of the team, so end it here
        int n[2];
        OtTeamCounts(n);
        int et = (g_ot.had[0] && g_ot.had[1] && !TestKeepEmptyTeam())? (n[0] == 0 && n[1] > 0 ? 0 : n[1] == 0 && n[0] > 0 ? 1 : -1) : -1;    // (TEST: t2r-test-keep-empty-team.txt)
        if (et < 0)
            g_ot.empty_since = 0;
        else if (!g_ot.empty_since || g_ot.empty_team != et) {
            g_ot.empty_since = now;
            g_ot.empty_team = et;
            Xlog("overtime: team %d has no player in the match any more - ends in %d ms unless one is back", et, OT_EMPTY_MS);
        } else if (!g_ot.ended_empty
                   && now - g_ot.empty_since >= OT_EMPTY_MS) {
            g_ot.ended_empty = 1;
            Xlog("overtime: team %d left the match (no player for %d ms) - it is eliminated, team %d wins (%s)", et, OT_EMPTY_MS, et ^ 1, OtRole());
            ((fn_msg)
             T2R_SYM(kFnShowMessage)) ("The other team left the match",
                                       5.0f, 1, MSG_TEXT_ID);
            ((void (*)(int)) T2R_SYM(kFnMatchEnd)) (et ^ 1);    // winmm's match end (the host's copy sends 0x12), as interrogation.c on every peer
            return;
        }
    }
    if (now - g_ot.last_log >= 10000) {
        g_ot.last_log = now;
        static int n;
        if (LOGCAP(n, 60))
            Xlog("overtime: %.0f s, team lives %d / %d, clock held at %.1f s, running %d expired %d (%s)", (now - g_ot.t0) / 1000.0, al, en, tm[1], run, lo[0x2b95], OtRole());
    }
}
