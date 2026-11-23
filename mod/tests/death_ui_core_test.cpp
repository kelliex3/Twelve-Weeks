// Standalone harness for the pure death-screen core (no game headers / no Windows):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/death_ui_core_test.cpp -o /tmp/death_ui_core_test && /tmp/death_ui_core_test
//
// Covers the port of tools/ext/death_ui.c's timeline / tip / credit tables and the
// victim-POV state machine. The engine-facing writes (widgets, PCM, pause hook) are
// checked by syntax checks; only the pure core is exercised here.

#include "death-ui.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

    using namespace net::ui::death_ui::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }
    bool Near(float a, float b, float eps = 0.002f) { return std::fabs(a - b) <= eps; }

    // ---- RampMs / Track -------------------------------------------------------------
    void TestRamp()
    {
        Check(RampMs(0, 83, 153) == 0.0f, "RampMs before from -> 0");
        Check(RampMs(83, 83, 153) == 0.0f, "RampMs at from -> 0");
        Check(RampMs(153, 83, 153) == 1.0f, "RampMs at to -> 1");
        Check(RampMs(999, 83, 153) == 1.0f, "RampMs after to -> 1");
        Check(Near(RampMs(118, 83, 153), 0.5f), "RampMs midpoint -> 0.5 (smoothstep)");

        Track t;
        Set(t, 0.3f, 0);
        Check(Near(Val(t, 500), 0.3f), "Track Set holds value");

        Go(t, 1.0f, 100, 100, 1000);
        Check(Near(Val(t, 1000), 0.3f), "Track Go ramps from current value");
        Check(Near(Val(t, 1050), 0.3f), "Track Go respects delay");
        Check(Near(Val(t, 1100), 0.3f), "Track Go at delay end still from");
        Check(Near(Val(t, 1150), 0.65f), "Track Go halfway");
        Check(Near(Val(t, 1200), 1.0f), "Track Go reached target");

        Go(t, 1.0f, 999, 999, 2000); // same target -> no-op
        Check(t.d0 == 100 && t.t0 == 1000, "Track Go same target is a no-op");
    }

    // ---- tables ---------------------------------------------------------------------
    void TestTables()
    {
        Check(kTipCount == 33, "ext DU_TIPS has 33 rows");
        Check(sizeof(kTipMf) == 33, "ext DU_TIP_MF has 33 rows");

        Check(std::strcmp(TipText(0),
            "Enemies are deadly accurate when you're charging towards them.") == 0,
            "SP tip text passes through");
        Check(std::strcmp(TipText(1),
            "The Molotov is great for flushing the enemy out of cover.") == 0,
            "NULL SP tip resolves to MF_TIPS row");
        Check(std::strcmp(TipText(11),
            "The bow is silent. Pick your arrows back up after the fight.") == 0,
            "silent-bow resolves to MF_TIPS[3]");
        Check(TipText(-1)[0] == '\0' && TipText(kTipCount)[0] == '\0',
            "TipText out of range -> empty");

        char b[128];
        TipImage(0, b, sizeof b);
        Check(std::strcmp(b, "streaming/large/death-tips/easy-target.png") == 0,
            "TipImage builds the SP streaming path");
        TipImage(32, b, sizeof b);
        Check(std::strcmp(b, "streaming/large/death-tips/buddy-rescue.png") == 0,
            "TipImage last row");
        char small[12];
        TipImage(0, small, sizeof small);
        Check(std::strlen(small) == sizeof(small) - 1 && small[sizeof(small) - 1] == '\0',
            "TipImage truncates safely into a small buffer");

        const WeaponInfo shiv = Weapon("shiv");
        Check(shiv.found && std::strcmp(shiv.name, "SHIV") == 0, "weapon shiv lookup");
        Check(shiv.icon && std::strcmp(shiv.icon, "hud/weapons/consumables/shiv.png") == 0,
            "weapon shiv icon");

        // A wrong string with the right SID still resolves via the FNV hash arm,
        // which is the native glue's fallback when the name lookup disagrees.
        const WeaponInfo bow = Weapon("wrong-name", Fnv64("bow-ellie"));
        Check(bow.found && std::strcmp(bow.name, "BOW") == 0, "weapon lookup by SID");

        const WeaponInfo fists = Weapon("unarmed");
        Check(fists.found && std::strcmp(fists.name, "FISTS") == 0, "fists alias");
        Check(std::strcmp(fists.icon, "streaming/t2r/icons/fist.png") == 0,
            "fists icon");

        const WeaponInfo unknown = Weapon("totally-new-weapon");
        Check(!unknown.found && std::strcmp(unknown.name, "FISTS") == 0,
            "unknown weapon falls back to FISTS, reported unfound");
        Check(Weapon("").name == nullptr && !Weapon(nullptr).found,
            "empty weapon credit -> no info");
    }

    // Drive a normal death from a fresh state to the point where the black is down.
    static uint64_t OpenDeath(State& s, Outputs& out, uint64_t t0)
    {
        Reset(s, t0);
        Inputs alive{}; alive.matchLive = true; alive.alive = true;
        Step(s, alive, out, t0 + 16);            // arm: we saw a live frame
        Inputs die{}; die.matchLive = true; die.dead = true; die.deadEdge = true;
        const uint64_t edge = t0 + 1000;
        Step(s, die, out, edge);
        return edge;
    }

    void TestLifecycle()
    {
        State s;
        Outputs out;

        // No match: never active, no exit at phase 0.
        Reset(s, 0);
        Inputs in{}; in.alive = true;
        Step(s, in, out, 16);
        Check(!out.active && !out.exit, "no match -> idle, no exit");

        const uint64_t edge = OpenDeath(s, out, 1000);
        Check(s.phase == 1, "death edge enters dead phase");
        Check(out.active && out.hideDeathBanner, "death edge is active + hides banner");
        Check(out.setTip && out.tipIndex == 0, "death edge rotates to first tip");
        Check(Near(out.black, 0.0f), "black still clear on the kill frame");
        Check(s.delayMs == int(kDelayMs) && s.cutMs == int(kCutMs),
            "default delay / cut match ext constants");

        // Land: delay 83 + cut 70 = 153 ms.
        Inputs dead{}; dead.matchLive = true; dead.dead = true;
        Step(s, dead, out, edge + kLand(kDelayMs, kCutMs));
        Check(Near(out.black, 1.0f), "black full at land");
        Check(out.stingDue, "sting decision fires at land");
        Check(out.fillCard, "card filled at land");
        Check(out.setCount && out.seconds == -1 && !out.noLives,
            "no respawn -> no countdown number");

        // Countdown from the mod's respawn timer.
        Inputs resp{}; resp.matchLive = true; resp.dead = true;
        resp.respawnRunning = true; resp.respawnRemaining = 2.4f;
        Step(s, resp, out, edge + 200);
        Check(out.seconds == 3, "countdown uses ceil(remaining)");
        Inputs nolives = resp; nolives.respawnRunning = false; nolives.outOfLives = true;
        Step(s, nolives, out, edge + 216);
        Check(out.noLives && out.seconds == -1,
            "out of lives -> 'no reinforcements' label");

        // Final blackout near the end of a live respawn countdown.
        Inputs fin = resp; fin.respawnRemaining = 0.5f;
        Step(s, fin, out, edge + 232);
        Check(s.final, "rem <= 1.0 starts the final blackout");
        Check(Near(out.black, 1.0f), "final darkens the screen again");

        // Respawn: alive -> fade out over black, then finish and clear records.
        Inputs alive{}; alive.matchLive = true; alive.alive = true;
        Step(s, alive, out, edge + 400);
        Check(s.phase == 2 && out.active, "alive after death -> respawning phase");
        Step(s, alive, out, edge + 400 + kRespawnHoldMs + kRespawnMs + 100);
        Check(out.exit && out.resetRecords, "respawn fade completes -> exit + reset records");
        Check(!out.active && s.phase == 0, "phase back to idle after respawn");

        // A second death rotates the tip.
        const uint64_t edge2 = OpenDeath(s, out, 5000);
        Check(out.setTip && out.tipIndex == 1, "second death rotates to next tip");
        (void)edge2;
    }

    void TestMatchOverAndPause()
    {
        State s;
        Outputs out;
        const uint64_t edge = OpenDeath(s, out, 1000);

        // Match end supersedes the death screen: whole UI fades, then exits.
        Inputs over{}; over.matchLive = true; over.dead = true; over.matchOver = true;
        Step(s, over, out, edge + 200);
        Check(s.phase == 3 && out.active, "match over -> end phase, still visible");
        Step(s, over, out, edge + 200 + kEndMs + 100);
        Check(out.exit && !out.active, "match-over fade completes -> exit");

        // Pause menu: the pause factor scales every track to 0.
        const uint64_t edge2 = OpenDeath(s, out, 10000);
        Inputs dead{}; dead.matchLive = true; dead.dead = true;
        Step(s, dead, out, edge2 + kLand(kDelayMs, kCutMs));
        Check(Near(out.black, 1.0f), "black down before pause");
        Inputs paused = dead; paused.pauseMenu = true;
        Step(s, paused, out, edge2 + kLand(kDelayMs, kCutMs) + 200);
        Check(Near(s.pf, 0.0f), "pause factor decays to 0");
        Check(Near(out.black, 0.0f), "paused black track is scaled to 0");
        Inputs unpause = dead; unpause.pauseMenu = false;
        Step(s, unpause, out, edge2 + kLand(kDelayMs, kCutMs) + 400);
        Check(Near(s.pf, 1.0f), "pause factor restores to 1");
    }

    void TestSpectate()
    {
        State s;
        Outputs out;
        const uint64_t edge = OpenDeath(s, out, 1000);
        Inputs dead{}; dead.matchLive = true; dead.dead = true;
        Step(s, dead, out, edge + kLand(kDelayMs, kCutMs));
        Check(Near(out.black, 1.0f), "black down before spectate");

        Inputs spec = dead; spec.spectateTarget = 3;
        Step(s, spec, out, edge + 200);
        Check(out.spectating && out.showSpecBar && !out.showSwitch,
            "spectate presentation is on, switch prompt stays gated");
        Check(Near(out.black, 1.0f), "black holds through the spectate lift delay");

        const uint64_t specT = edge + 200;
        Step(s, spec, out, specT + kSpecLift + kSpecLiftMs);
        Check(Near(out.black, 0.0f), "black lifted after spectate delay");

        Inputs back = dead; back.spectateTarget = -1;
        Step(s, back, out, specT + kSpecLift + kSpecLiftMs + 200);
        Check(!out.spectating && !out.showSpecBar, "spectate ends with the owner's target");
    }

    // Arm + death edge with the caller's inputs, returning the chosen delay/cut/ui.
    static void DeathTiming(bool soft, int leadMs, int& delay, int& cut, int& ui)
    {
        State s;
        Outputs out;
        Reset(s, 0);
        Inputs alive{}; alive.matchLive = true; alive.alive = true;
        Step(s, alive, out, 16);
        Inputs die{}; die.matchLive = true; die.dead = true; die.deadEdge = true;
        die.softMode = soft; die.leadMs = leadMs;
        Step(s, die, out, 1000);
        delay = s.delayMs; cut = s.cutMs; ui = s.uiMs;
    }

    void TestSoftMode()
    {
        int d = 0, c = 0, u = 0;
        DeathTiming(true, 2000, d, c, u);
        Check(d == 2000 && c == int(kSoftCutMs) && u == int(kSoftUiMs),
            "soft mode uses the tuned lead / cut / ui values");

        DeathTiming(true, 9000, d, c, u);
        Check(d == 3000, "lead is clamped to 3000 ms");

        DeathTiming(false, 9000, d, c, u);
        Check(d == int(kDelayMs) && c == int(kCutMs) && u == int(kUiMs),
            "base death ignores the (unset) soft producer lead");
    }

    // ---- BaseWeaponId: "-supp" clone maps to its base gun ------------------
    void TestBaseWeaponId()
    {
        Check(BaseWeaponId(Fnv64("rifle-vepr-supp")) == Fnv64("rifle-vepr"),
            "rifle-vepr-supp -> rifle-vepr");
        Check(BaseWeaponId(Fnv64("rifle-remington-bolt-supp")) == Fnv64("rifle-remington-bolt"),
            "rifle-remington-bolt-supp -> rifle-remington-bolt");
        Check(BaseWeaponId(Fnv64("pistol-beretta-supp")) == Fnv64("pistol-beretta"),
            "pistol-beretta-supp -> pistol-beretta");
        Check(BaseWeaponId(Fnv64("pistol-glock-supp")) == Fnv64("pistol-glock"),
            "pistol-glock-supp -> pistol-glock");
        Check(BaseWeaponId(Fnv64("rifle-mpx5-supp")) == Fnv64("rifle-mpx5"),
            "rifle-mpx5-supp -> rifle-mpx5");
        Check(BaseWeaponId(Fnv64("rifle-m14-semi-supp")) == Fnv64("rifle-m14-semi"),
            "rifle-m14-semi-supp -> rifle-m14-semi");
        Check(BaseWeaponId(Fnv64("pistol-cz75-enforcer-supp")) == Fnv64("pistol-cz75-enforcer"),
            "pistol-cz75-enforcer-supp -> pistol-cz75-enforcer");
        Check(BaseWeaponId(Fnv64("shotgun-tactical-supp")) == Fnv64("shotgun-tactical"),
            "shotgun-tactical-supp -> shotgun-tactical");
        Check(BaseWeaponId(Fnv64("rifle-burst-supp")) == Fnv64("rifle-burst"),
            "rifle-burst-supp -> rifle-burst");
        Check(BaseWeaponId(Fnv64("rifle-frontier-supp")) == Fnv64("rifle-frontier"),
            "rifle-frontier-supp -> rifle-frontier");

        // Non-cloned ids pass through unchanged.
        Check(BaseWeaponId(Fnv64("shiv")) == Fnv64("shiv"), "shiv passes through");
        Check(BaseWeaponId(Fnv64("bow-ellie")) == Fnv64("bow-ellie"), "bow-ellie passes through");
        Check(BaseWeaponId(0) == 0, "zero -> zero");
    }

    // ---- ResolveTiming: soft, shiv, paired, execution, instant --------------
    static void TimingOf(bool soft, int lead, bool forceInstant, bool execution,
                         bool shivVictim, int shivStingMs, int pairedLeftMs,
                         bool pairHoldOld, bool execEndNew,
                         int& delay, int& cut, int& ui, bool& softOut)
    {
        Timing t = ResolveTiming(soft, lead, forceInstant, execution,
            shivVictim, shivStingMs, pairedLeftMs, pairHoldOld, execEndNew);
        delay = t.delayMs; cut = t.cutMs; ui = t.uiMs; softOut = t.soft;
    }

    void TestResolveTiming()
    {
        int d = 0, c = 0, u = 0; bool s = false;

        // Base: instant kill, 83/70/180.
        TimingOf(false, 0, false, false, false, 0, -1, false, true, d, c, u, s);
        Check(d == int(kDelayMs) && c == int(kCutMs) && u == int(kUiMs) && !s,
            "base timing 83/70/180");

        // Soft mode: lead wins, soft cut/ui.
        TimingOf(true, 2000, false, false, false, 0, -1, false, true, d, c, u, s);
        Check(d == 2000 && c == int(kSoftCutMs) && u == int(kSoftUiMs) && s,
            "soft mode lead/cut/ui");

        // Soft mode clamped to 3000.
        TimingOf(true, 9000, false, false, false, 0, -1, false, true, d, c, u, s);
        Check(d == 3000 && s, "soft lead clamped to 3000");

        // Force instant: soft lead ignored.
        TimingOf(true, 2000, true, false, false, 0, -1, false, true, d, c, u, s);
        Check(d == int(kDelayMs) && !s, "force instant overrides soft");

        // Execution: instant regardless of soft lead.
        TimingOf(true, 2000, false, true, false, 0, -1, false, true, d, c, u, s);
        Check(d == int(kDelayMs) && !s, "execution is instant");

        // Shiv victim: sting + 0.5s, clamped 1..2500.
        TimingOf(false, 0, false, false, true, 600, -1, false, true, d, c, u, s);
        Check(d == 1100 && c == int(kCutMs) && u == int(kUiMs) && !s,
            "shiv victim: sting 600 + 500 = 1100");

        TimingOf(false, 0, false, false, true, 2000, -1, false, true, d, c, u, s);
        Check(d == 2500, "shiv victim clamped to 2500 max");

        TimingOf(false, 0, false, false, true, 100, -1, false, true, d, c, u, s);
        Check(d == 1000, "shiv victim clamped to 1000 min");

        // Paired victim: hold at most 1200, 350ms margin.
        TimingOf(false, 0, false, false, false, 0, 2000, false, true, d, c, u, s);
        Check(d <= 1200 && d > int(kDelayMs), "paired victim holds beyond base delay");

        // Paired victim with ps45 old (no margin): cap = left - cut.
        TimingOf(false, 0, false, false, false, 0, 2000, true, true, d, c, u, s);
        Check(d == int(kDelayMs) || d == 2000 - int(kCutMs),
            "paired hold old: cap without margin");

        // Force instant beats shiv and paired.
        TimingOf(true, 2000, true, false, true, 1000, 2000, false, true, d, c, u, s);
        Check(d == int(kDelayMs) && !s, "force instant beats shiv + paired");
    }

    // ---- Edge kinds: execution is instant, others use base ------------------
    void TestEdgeKinds()
    {
        // Edge kind is stored but ResolveTiming's `execution` flag is what matters.
        // A non-execution edge with soft lead uses soft timing.
        Timing t1 = ResolveTiming(true, 1500, false, false, false, 0, -1, false, true);
        Check(t1.delayMs == 1500 && t1.soft, "non-execution edge uses soft lead");

        // An execution edge is instant even with soft lead.
        Timing t2 = ResolveTiming(true, 1500, false, true, false, 0, -1, false, true);
        Check(t2.delayMs == int(kDelayMs) && !t2.soft, "execution edge is instant");
    }

    // ---- Spectate switch: showSwitch gated by candidates --------------------
    void TestSpectateSwitch()
    {
        State s;
        Outputs out;
        const uint64_t edge = OpenDeath(s, out, 1000);
        Inputs dead{}; dead.matchLive = true; dead.dead = true;
        Step(s, dead, out, edge + kLand(kDelayMs, kCutMs));

        // One candidate: switch prompt off.
        Inputs one = dead; one.spectateTarget = 1; one.spectateCandidates = 1;
        Step(s, one, out, edge + 200);
        Check(out.showSpecBar && !out.showSwitch, "one candidate -> bar on, switch off");

        // Two+ candidates: switch prompt on (not in final phase).
        Inputs two = dead; two.spectateTarget = 1; two.spectateCandidates = 2;
        Step(s, two, out, edge + 400);
        Check(out.showSpecBar && out.showSwitch, "two candidates -> bar + switch on");

        // In final phase: switch off even with candidates.
        Inputs fin = two; fin.respawnRunning = true; fin.respawnRemaining = 0.5f;
        Step(s, fin, out, edge + 500);
        Check(s.final && !out.showSwitch, "final phase -> switch off");
    }

    // ---- Spectate edge outputs: on / off / switch ---------------------------
    void TestSpectateEdgeOutputs()
    {
        State s;
        Outputs out;
        const uint64_t edge = OpenDeath(s, out, 1000);
        Inputs dead{}; dead.matchLive = true; dead.dead = true;
        Step(s, dead, out, edge + kLand(kDelayMs, kCutMs));

        // Spectate starts -> spectateOn.
        Inputs spec = dead; spec.spectateTarget = 3;
        Step(s, spec, out, edge + 200);
        Check(out.spectateOn && !out.spectateOff, "spectate start -> spectateOn");

        // Continue spectating -> neither edge.
        Step(s, spec, out, edge + 400);
        Check(!out.spectateOn && !out.spectateOff, "steady spectate -> no edges");

        // Spectate ends -> spectateOff.
        Inputs back = dead; back.spectateTarget = -1;
        Step(s, back, out, edge + 600);
        Check(!out.spectateOn && out.spectateOff, "spectate end -> spectateOff");

        // Switch prompt with candidates + switch edge -> spectateSwitchDir.
        // Must be >500ms into spectate for the edge to be accepted.
        Inputs sw = dead; sw.spectateTarget = 3; sw.spectateCandidates = 2;
        Step(s, sw, out, edge + 1000);  // enter spectate
        sw.spectateSwitch = +1;
        Step(s, sw, out, edge + 1600);  // 600ms after spectate start
        Check(out.showSwitch && out.spectateSwitchDir == +1,
            "switch edge -> spectateSwitchDir +1");
    }

} // namespace

int main()
{
    TestRamp();
    TestTables();
    TestLifecycle();
    TestMatchOverAndPause();
    TestSpectate();
    TestSoftMode();
    TestBaseWeaponId();
    TestResolveTiming();
    TestEdgeKinds();
    TestSpectateSwitch();
    TestSpectateEdgeOutputs();

    if (fails) {
        std::printf("DEATH_UI_CORE_TEST_FAIL %d\n", fails);
        return 1;
    }
    std::printf("DEATH_UI_CORE_TEST_OK\n");
    return 0;
}
