// ---------------------------------------------------------------- DEV TEST BOT (compiled out under T2R_RELEASE; included after pairtest.c in t2r-ext.c)
//
// Lets us test MP melee / dodge / hit reactions / dodge counter / escape between the local A and B with no human. t2r-bot.txt next to the exe
// is a COMMAND PROGRAM (one command per line, '#' comments), re-read whenever its modification time changes (polled every 500 ms) - a new/changed file restarts the
// program at line 1. Every command and its result is logged with the prefix "bot:" so tools/win/ab_melee_scenarios.py can assert outcomes:
//     bot: program loaded (N lines, file mtime ...)            bot: #3 melee BEGIN ...    bot: #3 melee OK|FAIL <why> tms N    bot: DONE tms N   bot: MARK <name> tms N
// NO SendInput / keyboard / mouse. Input goes through the ENGINE-LEVEL VIRTUAL PAD (padinject.c, re/pad-inject-1026.md): presses are written into the Pad that
// Joypad::Update turns into the joypad's held / pressed / released masks, so the engine's C++ (melee start, dodge, locomotion) sees a real button edge and the left
// stick is a real stick - approach / retreat WALK with the game's own locomotion (walk / run animations) steered by a closed loop (a probe at the first use learns the
// camera heading and the stick handedness from how the body moves; afterwards the heading error is corrected from the observed movement). Fallbacks, each logged:
// when the pad path is not installed / the bit of a command is unknown the press goes through the old native wrapper (joypad-command-active? record +8 / +0x10,
// path "wrapper"); when the stick makes no progress approach / retreat fall back to set-object-locator steps and one pairtest teleport. Every press line carries
// "path pad|wrapper". The bot also turns on the engine's "Joypad w/o Focus" debug flag so an unfocused A/B window takes the injected input.
//
// Commands (all names/args case sensitive):
//   target <slot|nearest>        the opponent for face/approach/dodge/...; default nearest live enemy (not a teammate, not downed)
//   face [slot|nearest] [ms]     turn the body toward the opponent (rotation re-applied every frame for ms, default 300)
//   approach <m> [walk|run]      walk (2.0 m/s) / run (4.5 m/s, default) toward the opponent until the flat distance <= m (10 s timeout = FAIL)
//   retreat <m> [run]            move away from the opponent until the flat distance >= m (10 s timeout)
//   melee [n] [gap_ms]           press melee n times (default 1), gap_ms between press starts (default 900); logs whether each press started a swing (+N ms)
//   waitswing [timeout_ms]       wait until the opponent announces a swing (MSG_DODGE_SWING), default 10000
//   dodge [dir] [at_ms]          wait for the opponent's NEXT swing announce, press evade at announce + at_ms (default 120; at_ms 'now' = press at once); dir left|right|back|any is
//                                logged only (the engine/ext picks the dodge move from the swing class; no stick is drivable). OK = the dodge engine took it (mode/cooldown)
//   counter                      after a successful dodge: wait for the dodge counter to be armed + the melee lock to end, press melee; OK = dcounter START
//   escape                       as the countered attacker: wait for the escape window (dc_esc_open) and press evade; OK = dcounter ESCAPE
//   hold <command> <ms>          engine-level press of any joypad command (aim, fire, sprint, crouch-press, prone-toggle, reload, interact, evade, jump, melee-attack ...) held ms, NON-blocking
//   stick <lx> <ly> <rx> <ry> <ms>  both virtual sticks for ms (camera turn = rx, walk / strafe = lx ly), blocking
//   wait <ms>                    pause
//   loop <n>                     (last line) run the whole program n more times
//   weapon <fists|pipe|bat|machete|knife|id>   give + draw (fists = holster instantly)
//   teleport <x y z yaw_deg>     teleport-player (same call as t2r-tp.txt 'at')
//   hp <v>                       set our health;   god <0|1>  refill health to its maximum every frame (no deaths)
//   mark <name>                  log 'bot: MARK name tms N' (sync point for the checker)
//   duel-human <aggressor|dodger|average> [seed N] [ms N]   HUMAN FIGHT MODEL: a long-running command (default 120000 ms) that fights the nearest enemy like a human would:
//                                walks / strafes with the pad stick, presses melee / evade / E / interact through the pad path only, with press gaps, dodge reaction (from the swing
//                                announce this client saw = g_mdg.sw[].ms), swing start distance, mash bursts and dud dodges drawn from tables MEASURED from real match logs
//                                (humanfight.h, written by tools/win/duel_mine.py; tables with n < 12 are marked TUNING there). Personality (aggressor = masher, dodger = patient,
//                                average = the measured population) only squeezes which part of each measured distribution it draws from and sets the TUNING probabilities (hbot_pers).
//                                Seeded (same seed = same decisions given the same game events); one log line "bot: dh ..." per decision; BEGIN / END lines carry personality + seed
//                                for tools/win/duel_score.py. Run it on BOTH A and B; the fake network is t2r-netsim.txt (netsmooth.c).
//   script <name> [attacker|defender] [x y z yaw]    expands to a named scenario program (see bot_script): swing_vs_idle, swing_vs_dodger, dodge_counter,
//                                counter_escape, wall_dodge (defender teleports to x y z yaw first), prone_pull (ps45: the defender lies prone, the attacker pulls it out)
//   pullvar <sp|a|b|c|cycle|file>  ps45 prone: the pull-out variant this client plays as the attacker (overrides t2r-prone-variant.txt; 'file' = the file again)
//   pullpos <feet|left|right|head> [m]  ps45 prone: teleport next to the nearest prone enemy - that side of its body axis, m away (default 1.0), facing it
//   proneidle <ms>               ps45 prone: stay prone for ms (re-prones every 1 s when free: not in a pair / a pull-out / down), blocking
#ifndef T2R_RELEASE
#define BOT_MAXLINE 160
#include "humanfight.h"         // measured human-fight distributions (generated: tools/win/duel_mine.py)
typedef struct {
    char t[BOT_MAXLINE];
} BotLine;
static struct {
    BotLine ln[BOT_MAXLINE];
    int n, cur, started, loops_left, loops_total, tslot, god, waited_log,
        installed, done_logged;
    uint64_t t0, step_ms, cmd_ms, poll_ms, mtime_lo, last_qpc;
    unsigned mtime_hi;
    int ph;
    uint64_t a0, a1;
    int i0, i1;
    float f0, f1;               // per-command scratch
    int last_path, pad_cal, pad_swap, pad_handed;
    uint64_t pad_t;
    float pad_c, pad_p0[2], pad_hA, pv[2];
    uint64_t pv_t, walk_t0, walk_ck;
    float walk_d0;              // pad steering (bot_pad_walk)
    float maxhp;
    volatile uint64_t melee_until, evade_until;
    volatile unsigned melee_hits, evade_hits;
    volatile uint64_t gen_sid, gen_until;
    volatile unsigned gen_hits; // 'hold' fallback: any joypad command through the script native (sprint / evade never learned on the pad path)
} g_bot;
static fn_dccall g_botGen;
static uint64_t(*g_bot_typed) (uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t);
static int BotEvadeNow(void)
{
    return g_bot.evade_until > GetTickCount64();
}                               // wrapper path only: the pad path sets no flag (the engine reads the real press)

static int BotWant(uint64_t sid)
{
    static uint64_t S_MELEE, S_EV, S_EV2, S_EV3;
    if (!S_MELEE) {
        S_MELEE = FNV64("melee-attack");
        S_EV = FNV64("evade");
        S_EV2 = FNV64("evade-start");
        S_EV3 = FNV64("evade-press");
    }
    uint64_t now = GetTickCount64();
    if (g_bot.melee_until > now && sid == S_MELEE) {
        g_bot.melee_hits++;
        return 1;
    }
    if (g_bot.evade_until > now
        && (sid == S_EV || sid == S_EV2 || sid == S_EV3)) {
        g_bot.evade_hits++;
        return 1;
    }
    if (g_bot.gen_until > now && sid == g_bot.gen_sid) {
        g_bot.gen_hits++;
        return 1;
    }
    return 0;
}

// the generic handler returns the pointer to its ret block: the original runs first, only the answer's low byte is flipped (an earlier crash at exe+161359c was a void thunk)
typedef uint64_t *(*bot_gen_t)(uint64_t * ret, int argc, uint64_t * argv,
                               uint64_t sec);
static uint64_t *BotThunkGen(uint64_t *ret, int argc, uint64_t *argv,
                               uint64_t sec)
{
    uint64_t *r = ((bot_gen_t) g_botGen) (ret, argc, argv, sec);
    if (ret && argc >= 1 && argv && !IsBadReadPtr(argv, 8)
        && BotWant(argv[0])) {
        ret[0] = (ret[0] & ~0xffULL) | 1;
        if (!r)
            r = ret;
    }
    return r;
}

static uint64_t BotThunkTyped(uint64_t a, uint64_t b, uint64_t c,
                                uint64_t d, uint64_t e, uint64_t f,
                                uint64_t g, uint64_t h, uint64_t j)
{
    uint64_t r = g_bot_typed(a, b, c, d, e, f, g, h, j);
    return BotWant(a) ? (r & ~0xffULL) | 1 : r;
}

static void BotInstall(void)
{
    if (g_bot.installed)
        return;
    g_bot.installed = 1;
    uint8_t *rec =
        (uint8_t *) ((fn_lookup)
                     T2R_SYM(kFnNativeLookup)) (FNV64
                                                ("joypad-command-active?"));
    if (!rec) {
        Xlog("bot: no joypad-command-active? record - presses cannot be delivered");
        return;
    }
    fn_dccall gen = *(fn_dccall *) (rec + 8);
    void *typed = *(void **) (rec + 0x10);
    DWORD old;
    int n = 0;
    if (gen && IsCode((void *) gen) && !IsBadWritePtr(rec + 8, 8)) {
        g_botGen = gen;
        *(bot_gen_t volatile *) (rec + 8) = BotThunkGen;
        n |= 1;
    }
    if (typed && IsCode(typed)
        && VirtualProtect(rec + 0x10, 8, PAGE_READWRITE, &old)) {
        g_bot_typed =
            (uint64_t(*)
             (uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
              uint64_t, uint64_t, uint64_t)) typed;
        *(void *volatile *) (rec + 0x10) = (void *) BotThunkTyped;
        n |= 2;
    }
    Xlog("bot: press injection hook on joypad-command-active? (generic %s, typed %s)", n & 1 ? "yes" : "no", n & 2 ? "yes" : "no");
}

// ---- helpers
static int BotSelf(void)
{
    return (int) *(uint32_t *) T2R_SYM(kMySlot);
}

static int BotSlotOk(int s)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    if (s < 0 || s >= T2R_MAX_PEERS || s == BotSelf())
        return 0;
    uint8_t *pe = ((fn_peer) T2rPeer) ((uint32_t) s);
    return pe && pe[0x4c] && !pe[0x4d] && *(uint64_t *) (pe + 0x60)
        && !g_peerDowned[s] && !g_puppetDeadHere[s];
}

static int BotLocOf(int s, float *o8)
{
    return BotSlotOk(s) && PtPuppet((uint32_t) s, o8);
}

// the opponent slot: g_bot.tslot >= 0 fixed, else the nearest live enemy
static int BotOpp(float *loc8)
{
    float me[16];
    const float *l = Uc4MyLoc(me);
    if (g_bot.tslot >= 0)
        return BotLocOf(g_bot.tslot, loc8) ? g_bot.tslot : -1;
    int best = -1;
    float bd = 1e30f;
    for (int s = 0; s < T2R_MAX_PEERS; s++) {
        float p[8];
        if (IsTeammate((uint32_t) s) || !BotLocOf(s, p))
            continue;
        float d =
            l ? (p[0] - l[0]) * (p[0] - l[0]) + (p[2] - l[2]) * (p[2] -
                                                                 l[2]) :
            0.0f;
        if (d < bd) {
            bd = d;
            best = s;
            memcpy(loc8, p, 32);
        }
    }
    return best;
}

static int BotReady(void)
{
    return MatchSubstate() == 6 && !g_downed && !g_downDying
        && LocalHp() > 0.0f;
}

static void BotSetPose(const float *l, float x, float z, float yaw)
{
    float l8[8] =
        { x, l[1], z, 1.0f, 0.0f, __builtin_sinf(yaw * 0.5f), 0.0f,
  __builtin_cosf(yaw * 0.5f) };
    static PsyncBf bf;
    PsyncBfFromLoc(&bf, l8);
    SetObjLoc(SID_PLAYER, &bf);
}

static int BotPress(int melee, int ms)
{                               // 1 = pad path (engine-level press edge), 2 = the native wrapper fallback
    PadinjInstall();
    PadinjNofocus(1);
    if (melee)
        g_botMeleeMs = GetTickCount64();
    g_padinjBotPress = 1;
    int pr = PadinjPress(melee ? "melee-attack" : "evade", ms);
    g_padinjBotPress = 0;
    if (pr) {
        g_bot.last_path = 1;
        if (melee)
            g_bot.melee_hits = 0;
        else
            g_bot.evade_hits = 0;
        return 1;
    }
    BotInstall();
    uint64_t u = GetTickCount64() + (uint64_t) ms;
    if (melee) {
        g_bot.melee_hits = 0;
        g_bot.melee_until = u;
    } else {
        g_bot.evade_hits = 0;
        g_bot.evade_until = u;
    }
    g_bot.last_path = 2;
    return 2;
}

static const char *BotPathName(void)
{
    return g_bot.last_path == 1 ? "pad" : "wrapper";
}

static unsigned BotReads(int melee)
{
    return g_bot.last_path ==
        1 ? PadinjReadsTrue(melee ? "melee-attack" : "evade") : melee ?
        g_bot.melee_hits : g_bot.evade_hits;
}

// ---- stick steering (the left stick through the virtual pad). Camera-relative movement: stick angle a (0 = up) moves the body along world heading c + handed * a.
// Heading convention here: atan2(dx, dz) of the flat world vector (the yaw the bot's pose code uses). c and handed are learned by a two-leg probe the first time:
// leg A (stick up) gives c, leg B (stick right) the sign of the turn = handed; if a leg moves the body < 0.2 m the other pad pair is tried (swap), then the pad
// walk is given up (cal 9) and the caller steps / teleports. Afterwards c is nudged by the observed heading error while moving.
static float BotWrap(float a)
{
    while (a > 3.14159265f)
        a -= 6.2831853f;
    while (a < -3.14159265f)
        a += 6.2831853f;
    return a;
}

// 0 = steering this frame, 1 = unusable (use the stepping fallback)
static int g_pwLearn = 1;      // duel-human clears it inside 3.5 m (dodges / swings / reactions move the body there, not the stick)
static int BotPadWalk(const float *l, float hd, float mag)
{
    uint64_t now = GetTickCount64();
    if (!PadinjReady())
        return 1;
    // The bots sometimes walked off after a hit (in one run a probe read -163 deg, in another it failed twice ->
    // "pad stick unusable - standing and pressing only" for the whole run): the two-leg probe learns the stick -> world mapping ONCE, but the melee / duel camera
    // turns all the time, so the learned heading goes stale after the first engage and a probe taken next to the enemy is garbage. The stick is camera relative:
    // stick up = the camera's flat forward, stick right = the screen right, measured (-fz, fx) (sfx spatial: camera right sign -1 on both clients). So every frame
    // a = -(target heading - camera heading), stick (sin a, cos a). t2r-bot-pad-probe.txt = the old probe + learned heading.
    {
        static int old = -1;
        static uint64_t oc;
        if (old < 0 || now - oc > 3000) {
            oc = now;
            old =
                GetFileAttributesA("t2r-bot-pad-probe.txt") !=
                INVALID_FILE_ATTRIBUTES;
        }
        float cp[3], cd[3];
        if (!old && CamPosDir(cp, cd)
            && cd[0] * cd[0] + cd[2] * cd[2] > 0.01f) {
            PadinjNofocus(1);
            // The bots spawned and walked backwards away from each other (a single learned offset drifted -176..+166 deg - the
            // mapping is not "camera heading + constant"). Model: walked heading h = c + k * a + b (c camera heading, a stick angle, k = +-1 handedness, b offset).
            // Every running sample gives b_k = h - c - k a for both k; the k whose b_k stays constant (largest circular resultant) wins; stick a = k (hd - c - b).
            // After a respawn (A 21719: "heading err -121 deg ... -> k -1"): every run so far converged to k +1, b 180 deg; noisy samples next to the
            // enemy flipped it back to k -1 and the bot walked sideways / away for seconds. Start at k +1, b pi; once 20 samples agree (R >= 0.7) the mapping is frozen.
            static float sc[2] = { -1.0f, -1.0f }, ss[2] =
                { 0.0f, 0.0f }, pv[2], pa, pc;
            static uint64_t pt;
            static int nl, kk = 1, ns, frozen;
            float c = __builtin_atan2f(cd[0], cd[2]);
            float k = kk ? 1.0f : -1.0f, b =
                __builtin_atan2f(ss[kk], sc[kk]);
            float a = BotWrap(k * (hd - c - b));
            PadinjStick(__builtin_sinf(a) * mag, __builtin_cosf(a) * mag,
                         0, 0, 150, 0);
            if (g_bot.pad_cal != 5) {
                g_bot.pad_cal = 5;
                pt = 0;
                Xlog("bot: pad walk steers from the live camera heading (no probe), k %+.0f offset %.0f deg tms %llu", k, b * 57.2958f, PT_MS);
            }
            if (!pt || now - pt > 400) {
                pv[0] = l[0];
                pv[1] = l[2];
                pt = now;
                pa = a;
                pc = c;
            } else if (now - pt >= 150) {
                float dx = l[0] - pv[0], dz = l[2] - pv[1], len =
                    __builtin_sqrtf(dx * dx + dz * dz);
                if (!frozen && g_pwLearn && mag >= 0.5f && len / ((float) (now - pt) * 0.001f) > 1.5f && __builtin_fabsf(BotWrap(a - pa)) < 0.35f && __builtin_fabsf(BotWrap(c - pc)) < 0.35f) {     // a steady stick + camera over the sample
                    float h = __builtin_atan2f(dx, dz), R[2];
                    for (int q = 0; q < 2; q++) {
                        float bk =
                            BotWrap(h - c - (q ? 1.0f : -1.0f) * a);
                        sc[q] = sc[q] * 0.85f + __builtin_cosf(bk) * 0.15f;
                        ss[q] = ss[q] * 0.85f + __builtin_sinf(bk) * 0.15f;
                        R[q] =
                            __builtin_sqrtf(sc[q] * sc[q] + ss[q] * ss[q]);
                    }
                    ns++;
                    int nk =
                        ns >= 6 ? (R[1] > R[0] + 0.15f ? 1 : R[0] >
                                   R[1] + 0.15f ? 0 : kk) : kk;
                    if (nk != kk || (nl++ < 60 && (nl % 3) == 1))
                        Xlog("bot: pad walk sample %d: heading err %.0f deg | k-1 offset %.0f R %.2f, k+1 offset %.0f R %.2f -> k %+d tms %llu", ns, BotWrap(h - hd) * 57.2958f, __builtin_atan2f(ss[0], sc[0]) * 57.2958f, R[0], __builtin_atan2f(ss[1], sc[1]) * 57.2958f, R[1], nk ? 1 : -1, PT_MS);
                    kk = nk;
                    if (ns >= 20 && R[kk] >= 0.7f
                        && R[kk] > R[1 - kk] + 0.15f
                        &&
                        GetFileAttributesA("t2r-bot-pad-learn-always.txt")
                        == INVALID_FILE_ATTRIBUTES) {
                        frozen = 1;
                        Xlog("bot: pad walk mapping frozen: k %+d offset %.0f deg (R %.2f vs %.2f) tms %llu", kk ? 1 : -1, __builtin_atan2f(ss[kk], sc[kk]) * 57.2958f, R[kk], R[1 - kk], PT_MS);
                    }
                }
                pv[0] = l[0];
                pv[1] = l[2];
                pt = now;
                pa = a;
                pc = c;
            }
            return 0;
        }
    }
    if (g_bot.pad_cal == 9)
        return 1;
    PadinjNofocus(1);
    if (g_bot.pad_cal == 0) {
        g_bot.pad_p0[0] = l[0];
        g_bot.pad_p0[1] = l[2];
        g_bot.pad_t = now;
        g_bot.pad_cal = 1;
        Xlog("bot: pad walk probe A (stick up, %s pad pair) tms %llu",
             g_bot.pad_swap ? "swapped" : "normal", PT_MS);
    }
    if (g_bot.pad_cal == 1 || g_bot.pad_cal == 3) {
        int leg_b = g_bot.pad_cal == 3;
        PadinjStick(leg_b ? 1.0f : 0.0f, leg_b ? 0.0f : 1.0f, 0, 0, 150,
                     g_bot.pad_swap);
        if (now - g_bot.pad_t < 500)
            return 0;
        float dx = l[0] - g_bot.pad_p0[0], dz =
            l[2] - g_bot.pad_p0[1], len =
            __builtin_sqrtf(dx * dx + dz * dz), h =
            __builtin_atan2f(dx, dz);
        PadinjStickOff();
        if (len < 0.2f) {
            if (!g_bot.pad_swap) {
                g_bot.pad_swap = 1;
                g_bot.pad_cal = 0;
                Xlog("bot: pad walk probe %c moved %.2f m only - trying the other pad pair tms %llu", leg_b ? 'B' : 'A', len, PT_MS);
                return 0;
            }
            g_bot.pad_cal = 9;
            Xlog("bot: pad walk probe %c moved %.2f m with both pad pairs - the stick does not move the body, falling back to stepping tms %llu", leg_b ? 'B' : 'A', len, PT_MS);
            return 1;
        }
        if (!leg_b) {
            g_bot.pad_hA = h;
            g_bot.pad_c = h;
            g_bot.pad_cal = 2;
            g_bot.pad_t = now;
            Xlog("bot: pad walk probe A: %.2f m along heading %.0f deg (camera heading) tms %llu", len, h * 57.2958f, PT_MS);
            return 0;
        }
        float dl = BotWrap(h - g_bot.pad_hA);
        if (__builtin_fabsf(dl) < 0.35f) {
            g_bot.pad_cal = 9;
            Xlog("bot: pad walk probe B turned only %.0f deg - handedness unknown, falling back to stepping tms %llu", dl * 57.2958f, PT_MS);
            return 1;
        }
        g_bot.pad_handed = dl > 0 ? 1 : -1;
        g_bot.pad_cal = 4;
        g_bot.pv_t = 0;
        g_bot.walk_t0 = 0;
        Xlog("bot: pad walk calibrated: camera heading %.0f deg, stick right turns the heading %s (leg B %.0f deg), pad pair %s tms %llu", g_bot.pad_c * 57.2958f, g_bot.pad_handed > 0 ? "+" : "-", dl * 57.2958f, g_bot.pad_swap ? "swapped" : "normal", PT_MS);
    }
    if (g_bot.pad_cal == 2) {   // the body stops between the legs
        PadinjStick(0, 0, 0, 0, 150, g_bot.pad_swap);
        if (now - g_bot.pad_t < 450)
            return 0;
        g_bot.pad_p0[0] = l[0];
        g_bot.pad_p0[1] = l[2];
        g_bot.pad_t = now;
        g_bot.pad_cal = 3;
        return 0;
    }
    // steer
    float a = (float) g_bot.pad_handed * BotWrap(hd - g_bot.pad_c);
    PadinjStick(__builtin_sinf(a) * mag, __builtin_cosf(a) * mag, 0, 0,
                 150, g_bot.pad_swap);
    if (!g_bot.walk_t0)
        g_bot.walk_t0 = now;
    if (!g_bot.pv_t) {
        g_bot.pv[0] = l[0];
        g_bot.pv[1] = l[2];
        g_bot.pv_t = now;
    } else if (now - g_bot.pv_t >= 120) {       // heading error from the observed movement
        float dx = l[0] - g_bot.pv[0], dz = l[2] - g_bot.pv[1], len =
            __builtin_sqrtf(dx * dx + dz * dz), sp =
            len / ((float) (now - g_bot.pv_t) * 0.001f);
        if (sp > 0.8f)
            g_bot.pad_c +=
                0.3f * BotWrap(__builtin_atan2f(dx, dz) -
                                (g_bot.pad_c +
                                 (float) g_bot.pad_handed * a));
        g_bot.pv[0] = l[0];
        g_bot.pv[1] = l[2];
        g_bot.pv_t = now;
    }
    return 0;
}

static const char *BotWeaponId(const char *w)
{
    if (!strcmp(w, "pipe"))
        return "lead-pipe";
    if (!strcmp(w, "bat"))
        return "baseball-bat";
    // A male life never keeps the knife (t2r-ext.c male_knife_strip removes it 1 frame later - the bot fought unarmed); a male gets the hatchet (one-handed blade)
    if (!strcmp(w, "knife"))
        return OwnFemale()? "knife-survival-mel-player" : "hatchet";
    return w;
}

// ---- named scenarios
static void BotAdd(const char *fmt, ...)
{
    if (g_bot.n >= BOT_MAXLINE)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_bot.ln[g_bot.n++].t, BOT_MAXLINE, fmt, ap);
    va_end(ap);
}

static void BotScript(const char *name, const char *role, const float *tp,
                       int has_tp)
{
    int att = strcmp(role, "defender") != 0;
    if (!att && has_tp)
        BotAdd("teleport %.3f %.3f %.3f %.1f", tp[0], tp[1], tp[2],
                tp[3]);
    BotAdd("god 1");
    if (!strcmp(name, "swing_vs_idle")) {
        if (att) {
            BotAdd("face nearest");
            BotAdd("approach 1.4");
            BotAdd("melee 2 900");
            BotAdd("wait 3000");
        } else {
            BotAdd("face nearest 1000");
            BotAdd("wait 7000");
        }
    } else if (!strcmp(name, "swing_vs_dodger")
               || !strcmp(name, "wall_dodge")) {
        if (att) {
            BotAdd("face nearest");
            BotAdd("approach 1.4");
            BotAdd("wait 1000");
            BotAdd("melee 1");
            BotAdd("wait 3500");
        } else {
            BotAdd("face nearest");
            BotAdd("dodge any 150");
            BotAdd("wait 2500");
        }
    } else if (!strcmp(name, "dodge_counter")) {
        if (att) {
            BotAdd("face nearest");
            BotAdd("approach 1.4");
            BotAdd("wait 1000");
            BotAdd("melee 1");
            BotAdd("wait 6000");
        } else {
            BotAdd("face nearest");
            BotAdd("dodge any 340");
            BotAdd("counter");
            BotAdd("wait 4000");
        }
    } else if (!strcmp(name, "counter_lowhp")) {        // the countered attacker at 15 hp: the counter must down it (Melee Down), never kill it
        if (att) {
            BotAdd("god 0");
            BotAdd("hp 15");
            BotAdd("face nearest");
            BotAdd("approach 1.4");
            BotAdd("wait 1000");
            BotAdd("melee 1");
            BotAdd("wait 6000");
        } else {
            BotAdd("face nearest");
            BotAdd("dodge any 340");
            BotAdd("counter");
            BotAdd("wait 4000");
        }
    } else if (!strcmp(name, "trade_dodge")) {  // the defender is mid its own swing (past contact) when the attacker swings - the dodge must still open
        if (att) {
            BotAdd("face nearest");
            BotAdd("approach 1.4");
            BotAdd("waitswing");
            BotAdd("wait 250");
            BotAdd("melee 1 50");
            BotAdd("wait 3500");
        } else {
            BotAdd("face nearest");
            BotAdd("wait 3000");
            BotAdd("melee 1 50");
            BotAdd("dodge any 150");
            BotAdd("wait 2500");
        }
    } else if (!strcmp(name, "counter_escape")) {
        if (att) {
            BotAdd("face nearest");
            BotAdd("approach 1.4");
            BotAdd("wait 1000");
            BotAdd("melee 1");
            BotAdd("escape");
            BotAdd("wait 4000");
        } else {
            BotAdd("face nearest");
            BotAdd("dodge any 340");
            BotAdd("counter");
            BotAdd("wait 5000");
        }
    } else if (!strcmp(name, "prone_pull")) {   // ps45 prone: three pull-outs from the feet / left / right (the variant = pullvar or t2r-prone-variant.txt on the attacker)
        if (att) {
            for (int k = 0; k < 3; k++) {
                BotAdd("pullpos %s 1.0",
                        k == 0 ? "feet" : k == 1 ? "left" : "right");
                BotAdd("wait 500");
                BotAdd("melee 1");
                BotAdd("wait 10000");
        }} else {
            BotAdd("god 0");
            BotAdd("proneidle 40000");
        }
    } else
        Xlog("bot: script '%s' unknown (swing_vs_idle swing_vs_dodger trade_dodge dodge_counter counter_escape counter_lowhp wall_dodge prone_pull)", name);
    BotAdd("mark %s_%s", name, att ? "attacker" : "defender");
}

// ---- duel-human: the human fight model. All timing below is DRAWN from humanfight.h unless marked TUNING (nothing measured behind it).
typedef struct {
    const char *name;
    float atk_lo, atk_hi, react_lo, react_hi, follow, mash, dud_pm, execp,
        retreat_p;
} HbotPers;
// TUNING rows (aggressor / dodger): the bands pick the fast / slow side of the measured distributions; the probabilities are guesses that span the human range.
// average = the measured population (follow = HF_DODGE_PRESS_ANY_RATE, mash = HF_MASH_SHARE, duds = HF_DUD_EVADE_PER_MIN); its execp / retreat_p are TUNING.
static const HbotPers HBOT_PERS[] = {
    { "aggressor", 0.00f, 0.50f, 0.45f, 1.00f, 0.45f, 0.85f, 9.0f, 0.95f,
     0.10f },
    { "dodger", 0.45f, 1.00f, 0.00f, 0.60f, 0.97f, 0.05f, 1.5f, 0.60f,
     0.45f },
    { "average", 0.00f, 1.00f, 0.00f, 1.00f, HF_DODGE_PRESS_ANY_RATE,
     HF_MASH_SHARE, HF_DUD_EVADE_PER_MIN, 0.80f, 0.30f },
};

static struct {
    int on, pi, nlog, strafe_dir, burst_left, mash_left, retreating,
        exec_slot, n_swing, n_dodge, n_dud, n_mash, n_e, n_exec, n_retreat,
        dodge_slot, pad_bad;
    uint32_t rng, seed, seen[T2R_MAX_PEERS];
    uint64_t t0, end_ms, atk_due, mash_at, mash_gap, dodge_at, burst_at,
        strafe_flip, ret_until, pin_due, qte_seen, e_at, exec_at,
        exec_done, early_at, exec_wait, far_since, exec_back;
    float atk_dist, want_d, hp_prev, ret_m;
} g_hbot;
static float g_botHome[4];
static int g_botHomeOk;       // The program's last 'teleport' spot (the far-respawn regroup point)
static float HbotU(void)
{
    g_hbot.rng ^= g_hbot.rng << 13;
    g_hbot.rng ^= g_hbot.rng >> 17;
    g_hbot.rng ^= g_hbot.rng << 5;
    return (float) (g_hbot.rng >> 8) / 16777216.0f;
}

static float HbotDr(const HfDist *d)
{
    return HfDraw(d, &g_hbot.rng);
}

static float HbotBand(const HfDist *d, float lo, float hi)
{
    return HfDrawBand(d, &g_hbot.rng, lo, hi);
}

#define HBLOG(...) do { if (g_hbot.nlog++ < 6000) Xlog(__VA_ARGS__); } while (0)
static void HbotSchedAttack(uint64_t now)
{
    const HbotPers *P = &HBOT_PERS[g_hbot.pi];
    float gap = HbotBand(&HF_OWN_SWING_GAP_MS, P->atk_lo, P->atk_hi);
    g_hbot.atk_due = now + (uint64_t) (gap < 150.0f ? 150.0f : gap);
    g_hbot.atk_dist = HbotDr(&HF_OWN_SWING_START_DIST_M);
    // The table's low tail (0.36-0.6 m, logged mid pair) is closer than two standing bodies get (0.65 m): the bot stood face to face
    // with its opponent for 45 s, attack overdue, never swinging. Floor it at 0.8 m (fist reach).
    if (g_hbot.atk_dist < 0.8f)
        g_hbot.atk_dist = 0.8f;
    g_hbot.want_d = HbotDr(&HF_FIGHT_PAIR_DIST_M);
}

// returns 0 = running, 1 = done
static int HbotStep(const char *args, char *why, size_t wn, uint64_t el,
                     uint64_t now)
{
    (void) el;
    if (!g_hbot.on) {
        char pn[16] = "average";
        unsigned seed = 1;
        int ms = 120000;
        char b[BOT_MAXLINE];
        snprintf(b, sizeof b, "%s", args);
        char *tok = strtok(b, " ");
        int k = 0;
        while (tok) {
            if (k == 0)
                snprintf(pn, sizeof pn, "%s", tok);
            else if (!strcmp(tok, "seed")) {
                tok = strtok(NULL, " ");
                if (tok)
                    seed = (unsigned) strtoul(tok, NULL, 10);
            } else if (!strcmp(tok, "ms")) {
                tok = strtok(NULL, " ");
                if (tok)
                    ms = atoi(tok);
            }
            k++;
            tok = strtok(NULL, " ");
        }
        int pi = 2;
        for (int i = 0; i < 3; i++)
            if (!strcmp(pn, HBOT_PERS[i].name))
                pi = i;
        memset(&g_hbot, 0, sizeof g_hbot);
        g_hbot.on = 1;
        g_hbot.pi = pi;
        g_hbot.seed = seed;
        g_hbot.rng = seed * 2654435761u + 0x9e3779b9u;
        if (!g_hbot.rng)
            g_hbot.rng = 1;
        g_hbot.t0 = now;
        g_hbot.end_ms = (uint64_t) (ms > 1000 ? ms : 1000);
        g_hbot.strafe_dir = 1;
        g_hbot.exec_slot = -1;
        g_hbot.hp_prev = LocalHp();
        for (int q = 0; q < T2R_MAX_PEERS; q++)
            g_hbot.seen[q] = g_mdg.sw[q].seq;
        Xlog("bot: dh BEGIN %s seed %u ms %d (tables: own swing gap n=%d, dodge reaction n=%d, swing start dist n=%d; follow %.2f mash %.2f duds/min %.1f; TUNING: atk band %.2f-%.2f, react band %.2f-%.2f) tms %llu", HBOT_PERS[pi].name, seed, ms, HF_OWN_SWING_GAP_MS.n, HF_DODGE_REACT_MS.n, HF_OWN_SWING_START_DIST_M.n, HBOT_PERS[pi].follow, HBOT_PERS[pi].mash, HBOT_PERS[pi].dud_pm, HBOT_PERS[pi].atk_lo, HBOT_PERS[pi].atk_hi, HBOT_PERS[pi].react_lo, HBOT_PERS[pi].react_hi, PT_MS);
    }
    const HbotPers *P = &HBOT_PERS[g_hbot.pi];
    if (now - g_hbot.t0 >= g_hbot.end_ms && !(PsyncActive() && now - g_hbot.t0 < g_hbot.end_ms + 10000)) {     // Never end mid paired move (the next teleport moved A 3.95 m inside a wall takedown)
        PadinjStickOff();
        Xlog("bot: dh END %s seed %u: %d swings pressed, %d dodges pressed, %d dud evades, %d mash presses, %d E presses, %d execution walks, %d retreats tms %llu", P->name, g_hbot.seed, g_hbot.n_swing, g_hbot.n_dodge, g_hbot.n_dud, g_hbot.n_mash, g_hbot.n_e, g_hbot.n_exec, g_hbot.n_retreat, PT_MS);
        snprintf(why, wn,
                 "%s seed %u: %d swings, %d dodges, %d duds, %d mash, %d E",
                 P->name, g_hbot.seed, g_hbot.n_swing, g_hbot.n_dodge,
                 g_hbot.n_dud, g_hbot.n_mash, g_hbot.n_e);
        g_hbot.on = 0;
        return 1;
    }
    float me[16];
    const float *l = Uc4MyLoc(me);
    {                           // Both bots stood 0.6-2 m apart for 57 s with no press and no log line - a 3 s heartbeat of the bot's state
        static uint64_t hb;
        if (now - hb > 3000) {
            hb = now;
            float o2[8];
            int s2 = BotOpp(o2);
            float d2 = s2 >= 0
                && l ? __builtin_sqrtf((o2[0] - l[0]) * (o2[0] - l[0]) +
                                       (o2[2] - l[2]) * (o2[2] -
                                                         l[2])) : -1.0f;
            HBLOG
                ("bot: dh state - ready %d loc %d opp %d at %.2f m, atk due in %lld ms at <= %.2f m, dodge_at %d, exec slot %d wait %d back %d, retreating %d, pad_bad %d, psync %d, duel %d, qte %d tms %llu",
                 BotReady(), l != NULL, s2, (double) d2,
                 (long long) (g_hbot.
                              atk_due ? (int64_t) (g_hbot.atk_due -
                                                   now) : -1),
                 (double) g_hbot.atk_dist, g_hbot.dodge_at != 0,
                 g_hbot.exec_slot, g_hbot.exec_wait != 0,
                 g_hbot.exec_back != 0, g_hbot.retreating, g_hbot.pad_bad,
                 PsyncActive(), (int) g_mdu.on, MpnQteOpen(), PT_MS);
        }
    }
    if (!BotReady() || !l) {
        PadinjStickOff();
        g_hbot.atk_due = 0;
        g_hbot.dodge_at = 0;
        g_hbot.hp_prev = LocalHp();
        return 0;
    }
    PadinjInstall();
    PadinjNofocus(1);
    // ---- the pinned victim: E at the Escape prompt (a human reaction after the PRESS prompt shows), sometimes an early mash press first (it burns the QTE: the anti-mash rule)
    if (MpnQteOpen() && g_mpn.vq_open != g_hbot.qte_seen) {
        g_hbot.qte_seen = g_mpn.vq_open;
        float react = HbotDr(&HF_DODGE_REACT_MS) * 0.5f;       // the prompt is the cue: TUNING 0.5 x the swing reaction
        g_hbot.e_at = MPN_RING_AT() + (uint64_t) react;
        g_hbot.early_at = HbotU() < HBOT_PERS[g_hbot.pi].mash ? g_mpn.vq_open + 40 + (uint64_t) (HbotU() * 160.0f) : 0;       // TUNING: early mash press 40-200 ms into the window
        HBLOG
            ("bot: dh pin QTE %d open: E planned at the ring +%.0f ms%s tms %llu",
             g_mpn.vq_id, react,
             g_hbot.early_at ? " (and an early mash press first)" : "",
             PT_MS);
    }
    if (g_hbot.early_at && MpnQteOpen() && now >= g_hbot.early_at) {
        g_botEMs = now;
        g_hbot.early_at = 0;
        g_hbot.n_e++;
        HBLOG("bot: dh E mash press (early) tms %llu", PT_MS);
    }
    if (g_hbot.e_at && now >= g_hbot.e_at) {
        g_hbot.e_at = 0;
        if (MpnQteOpen() && HbotU() >= 0.15f) {
            g_botEMs = now;
            g_hbot.n_e++;
            HBLOG("bot: dh E press (pin escape) tms %llu", PT_MS);
        }                       // TUNING: 15 % of prompts missed outright
        else
            HBLOG("bot: dh E press skipped (missed the prompt) tms %llu",
                  PT_MS);
    }
    // ---- the pinning attacker: the next press = the next blow, at the pin cadence
    if (g_mpn.on && g_mpn.state == 1) {
        if (!g_hbot.pin_due)
            g_hbot.pin_due = now + (uint64_t) HbotDr(&HF_PIN_BLOW_GAP_MS);
        if (now >= g_hbot.pin_due) {
            BotPress(1, 100);
            g_hbot.pin_due = 0;
            g_hbot.n_swing++;
            HBLOG("bot: dh pin blow press tms %llu", PT_MS);
        }
    } else
        g_hbot.pin_due = 0;
    float o[8];
    int s = BotOpp(o);
    // ---- a downed enemy near us: walk to it and press E (the execution) after a human delay (TUNING 250-900 ms); one roll per downed enemy
    {
        int ds = -1;
        float dd = 12.0f, dp[8];
        for (int q = 0; q < T2R_MAX_PEERS; q++) {
            if (q == BotSelf() || IsTeammate((uint32_t) q)
                || !g_peerDowned[q] || !PtPuppet((uint32_t) q, dp))
                continue;
            float d2 =
                __builtin_sqrtf((dp[0] - l[0]) * (dp[0] - l[0]) +
                                (dp[2] - l[2]) * (dp[2] - l[2]));
            if (d2 < dd) {
                dd = d2;
                ds = q;
                memcpy(o, dp, 32);
            }
        }
        if (ds >= 0 && now - g_hbot.exec_done > 4000
            && (g_hbot.exec_slot == ds || HbotU() < P->execp * 0.02f)) {
            if (g_hbot.exec_slot != ds) {
                g_hbot.exec_slot = ds;
                g_hbot.exec_at = 0;
                g_hbot.n_exec++;
                HBLOG
                    ("bot: dh downed enemy slot %d at %.1f m - going for the execution tms %llu",
                     ds, dd, PT_MS);
            }
            if (dd > 1.25f) {
                if (BotPadWalk
                    (l, __builtin_atan2f(o[0] - l[0], o[2] - l[2]), 1.0f))
                    PadinjStickOff();
                return 0;
            }
            // An early run made 11 E presses and 0 executions - the prompt (front half plane, LOS, not busy) was up only once. E only while the Execute prompt shows
            // for this slot (a human presses at the prompt); without it a slow step toward the body turns us onto it, and after 2 s one line says why.
            int prompt = g_execPromptSlot == (uint32_t) ds
                && g_t60 - g_execPromptT60 <= 6;
            if (!prompt) {
                if (!g_hbot.exec_wait)
                    g_hbot.exec_wait = now;
                // (a downed bot bled out at 0.65 m, "no Execute prompt after 2 s"): standing on / past the body puts it behind us (2013 front half plane),
                // a 0.15 stick step never turned us round. Too close without the prompt: step back to 1.4 m, then walk in facing it (a human's re-approach).
                if (dd < 0.9f && !g_hbot.exec_back) {
                    g_hbot.exec_back = now;
                    HBLOG
                        ("bot: dh no Execute prompt at %.2f m - stepping back to face the body tms %llu",
                         dd, PT_MS);
                }
                if (g_hbot.exec_back
                    && (dd > 1.4f || now - g_hbot.exec_back > 1500))
                    g_hbot.exec_back = 0;
                if (g_hbot.exec_back) {
                    if (BotPadWalk
                        (l, __builtin_atan2f(l[0] - o[0], l[2] - o[2]),
                         0.5f))
                        PadinjStickOff();
                } else
                    if (BotPadWalk
                        (l, __builtin_atan2f(o[0] - l[0], o[2] - l[2]),
                         dd > 0.6f ? 0.35f : 0.25f))
                    PadinjStickOff();
                if (now - g_hbot.exec_wait > 4500) {
                    HBLOG
                        ("bot: dh no Execute prompt on slot %d after 4.5 s (%.2f m, downed %d, busy %d, dead here %d) - giving up tms %llu",
                         ds, dd, (int) g_peerDowned[ds],
                         PsyncBusySlot((uint32_t) ds),
                         (int) g_puppetDeadHere[ds], PT_MS);
                    g_hbot.exec_wait = 0;
                    g_hbot.exec_done = now;
                    g_hbot.exec_slot = -1;
                    PadinjStickOff();
                }
                return 0;
            }
            g_hbot.exec_wait = 0;
            PadinjStickOff();
            if (!g_hbot.exec_at)
                g_hbot.exec_at =
                    now + 250 + (uint64_t) (HbotU() * 650.0f);
            if (now >= g_hbot.exec_at) {
                PadinjPress("interact", 140);
                g_ptPressF = g_frame + 1;     /* (0 of 5 downs executed - the pad "interact" press never made an E edge for execute_enemy_tick) */
                g_hbot.n_e++;
                g_hbot.exec_at = 0;
                g_hbot.exec_done = now;
                g_hbot.exec_slot = -1;
                HBLOG("bot: dh E press (execution) on slot %d tms %llu",
                      ds, PT_MS);
            }
            return 0;
        }
        if (ds < 0)
            g_hbot.exec_slot = -1;
    }
    if (s < 0) {
        PadinjStickOff();
        g_hbot.far_since = 0;
        return 0;
    }
    float dx = o[0] - l[0], dz = o[2] - l[2], d =
        __builtin_sqrtf(dx * dx + dz * dz), hd = __builtin_atan2f(dx, dz);
    // A fight that stays broken (> 6 m apart for 5 s: stuck on geometry, a knock-back
    // over a ledge) is restarted - one teleport to 2.5 m in front of the enemy, facing it. Never inside a pair / while either body is down.
    // A respawn far away (> 15 m) - each bot goes back to its OWN program spot (known clear ground). The old teleport next to the enemy put the
    // aggressor inside geometry at the enemy's respawn point (A's screen dark for 34 s, no fight in the whole duel).
    if (d > 15.0f && g_botHomeOk && !PsyncActive() && !g_peerDowned[s]) {
        if (!g_hbot.far_since)
            g_hbot.far_since = now;
        else if (now - g_hbot.far_since > 2000u) {
            PtTp(g_botHome, g_botHome[3] * 0.0174533f);
            g_testTpFrame = g_frame;
            g_hbot.far_since = 0;
            g_hbot.atk_due = 0;
            HBLOG
                ("bot: dh fight broken (%.1f m apart: a respawn far away) - back to our own program spot %.2f %.2f %.2f tms %llu",
                 d, g_botHome[0], g_botHome[1], g_botHome[2], PT_MS);
        }
        return 0;
    }
    if (d > 6.0f && g_hbot.pi == 0 && !PsyncActive() && !g_peerDowned[s]) {   // only the aggressor re-engages (both teleporting at once crossed them)
        if (!g_hbot.far_since)
            g_hbot.far_since = now;
        else if (now - g_hbot.far_since > (d > 15.0f ? 2000u : 5000u)) {        // > 15 m = a respawn far away: re-engage quickly
            float k = (d - 2.5f) / d, t3[3] =
                { l[0] + dx * k, o[1], l[2] + dz * k };
            PtTp(t3, hd);
            g_hbot.far_since = 0;
            g_hbot.atk_due = 0;
            HBLOG
                ("bot: dh fight broken (%.1f m apart for 5 s) - teleported to 2.5 m from slot %d tms %llu",
                 d, s, PT_MS);
            return 0;
        }
    } else
        g_hbot.far_since = 0;
    // ---- defence: a new swing announced on this client -> maybe a dodge press at announce + the measured reaction
    for (int q = 0; q < T2R_MAX_PEERS; q++) {
        if (IsTeammate((uint32_t) q) || g_mdg.sw[q].seq == g_hbot.seen[q])
            continue;
        g_hbot.seen[q] = g_mdg.sw[q].seq;
        if (g_hbot.dodge_at)
            continue;
        if (HbotU() < P->follow) {
            float react =
                HbotBand(&HF_DODGE_REACT_MS, P->react_lo, P->react_hi);
            g_hbot.dodge_at = g_mdg.sw[q].ms + (uint64_t) react;
            g_hbot.dodge_slot = q;
            HBLOG
                ("bot: dh swing of slot %d seen (%.2f m): dodge planned +%.0f ms tms %llu",
                 q, d, react, PT_MS);
        } else
            HBLOG
                ("bot: dh swing of slot %d seen (%.2f m): no dodge (takes it) tms %llu",
                 q, d, PT_MS);
    }
    if (g_hbot.dodge_at && now > g_hbot.dodge_at + 1500) {
        HBLOG("bot: dh dodge plan dropped (stale by %lld ms) tms %llu",
              (long long) (now - g_hbot.dodge_at), PT_MS);
        g_hbot.dodge_at = 0;
    }                           // planned before a death / pause: never press it seconds later
    if (g_hbot.dodge_at && now >= g_hbot.dodge_at) {
        BotPress(0, 110);
        g_hbot.n_dodge++;
        HBLOG
            ("bot: dh dodge press %lld ms after the swing of slot %d (window open %d, cooldown left %lld ms, path %s) tms %llu",
             (long long) (now - g_mdg.sw[g_hbot.dodge_slot].ms),
             g_hbot.dodge_slot, g_mdg.window_open,
             (long long) (g_mdg.cooldown_until >
                          now ? g_mdg.cooldown_until - now : 0),
             BotPathName(), PT_MS);
        g_hbot.dodge_at = 0;
    }
    // ---- dud evade presses (nothing to dodge): singles + bursts, only inside a fight (< 6 m). The burst share and size are measured; P->dud_pm scales the personality.
    // OFF by default (double dodges in random places) - a test bot dodges only at a swing it saw; t2r-bot-duds.txt = the human dud model.
    static int duds = -1;
    static uint64_t duds_ck;
    if (duds < 0 || now - duds_ck > 3000) {
        duds_ck = now;
        duds =
            GetFileAttributesA("t2r-bot-duds.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (duds && d < 6.0f) {
        if (g_hbot.burst_left > 0) {
            if (now >= g_hbot.burst_at) {
                BotPress(0, 90);
                g_hbot.n_dud++;
                g_hbot.burst_left--;
                g_hbot.burst_at =
                    now + (uint64_t) HbotBand(&HF_EVADE_GAP_MS, 0.0f,
                                               0.30f);
                HBLOG("bot: dh dud evade (burst, %d left) tms %llu",
                      g_hbot.burst_left, PT_MS);
            }
        } else if (HbotU() < P->dud_pm / 60.0f * 0.016f) {
            if (HbotU() < HF_EVADE_BURST_SHARE) {
                g_hbot.burst_left = (int) HbotDr(&HF_EVADE_BURST_SIZE);
                if (g_hbot.burst_left < 3)
                    g_hbot.burst_left = 3;
                g_hbot.burst_at = now;
                HBLOG("bot: dh dud evade burst of %d starts tms %llu",
                      g_hbot.burst_left, PT_MS);
            } else {
                BotPress(0, 90);
                g_hbot.n_dud++;
                HBLOG("bot: dh dud evade (single) tms %llu", PT_MS);
            }
        }
    }
    // ---- offence: swing when due and inside the drawn start distance (a start distance above reach = a sprint punch, the stick keeps pushing); mash presses ride the pace lock
    if (!g_hbot.atk_due)
        HbotSchedAttack(now);
    // (or 1.5 s overdue inside 1.5 m: whatever keeps the pair apart, an overdue swing is never held back for good)
    if (now >= g_hbot.atk_due
        && (d <= g_hbot.atk_dist
            || (now >= g_hbot.atk_due + 1500 && d <= 1.5f))) {
        BotPress(1, 90);
        g_hbot.n_swing++;
        HBLOG
            ("bot: dh melee press at %.2f m (drawn start %.2f m, %s) tms %llu",
             d, g_hbot.atk_dist, g_msAtk ? "during our swing" : "idle",
             PT_MS);
        g_hbot.mash_left = 0;
        if (HbotU() < P->mash) {
            g_hbot.mash_left = 1 + (int) (HbotU() * 3.0f);
            g_hbot.mash_at =
                now + (uint64_t) HbotDr(&HF_MASH_INTO_LOCK_MS);
        }
        HbotSchedAttack(now);
    }
    if (g_hbot.mash_left && g_hbot.mash_at && !MpnQteOpen()
        && now >= g_hbot.mash_at) {
        BotPress(1, 70);
        g_hbot.n_mash++;
        g_hbot.mash_left--;
        g_hbot.mash_at = g_hbot.mash_left ? now + 150 + (uint64_t) (HbotU() * 300.0f) : 0;     // TUNING: the next mash press 150-450 ms later
        HBLOG("bot: dh mash melee press (%d left) tms %llu",
              g_hbot.mash_left, PT_MS);
    }
    // ---- reaction to a hit: sometimes step away by the measured post-hit distance change (only a positive change = retreat)
    float hp = LocalHp();
    if (hp < g_hbot.hp_prev - 0.5f && !g_hbot.retreating
        && HbotU() < P->retreat_p) {
        g_hbot.ret_m = HbotDr(&HF_AFTER_HIT_DIST_CHANGE_M);
        if (g_hbot.ret_m > 0.3f) {
            g_hbot.retreating = 1;
            g_hbot.ret_until =
                now + (uint64_t) (g_hbot.ret_m / 3.5f * 1000.0f);
            g_hbot.n_retreat++;
            HBLOG("bot: dh hit (hp %.0f -> %.0f): retreat %.1f m tms %llu",
                  g_hbot.hp_prev, hp, g_hbot.ret_m, PT_MS);
        }
    }
    g_hbot.hp_prev = hp;
    if (g_hbot.retreating && now >= g_hbot.ret_until)
        g_hbot.retreating = 0;
    // ---- movement: far = run at it; fighting range = hold the drawn pair distance and circle (TUNING: the direction flips every 0.8-2.5 s); retreat after a hit
    if (g_hbot.pad_bad)
        return 0;
    float mag = 1.0f, hh = hd;
    if (g_hbot.retreating)
        hh = __builtin_atan2f(-dx, -dz);
    else if (d > 3.0f
             || (now + 700 >= g_hbot.atk_due && d > g_hbot.atk_dist))
        mag = d > 5.0f ? 1.0f : 0.7f;
    else if (d < g_hbot.want_d - 0.3f) {
        hh = __builtin_atan2f(-dx, -dz);
        mag = 0.4f;
    } else {
        if (now >= g_hbot.strafe_flip) {
            g_hbot.strafe_dir = -g_hbot.strafe_dir;
            g_hbot.strafe_flip =
                now + 800 + (uint64_t) (HbotU() * 1700.0f);
        }
        hh = hd + (float) g_hbot.strafe_dir * 1.5708f;
        mag = 0.6f;
    }
    g_pwLearn = d > 3.5f && !g_hbot.retreating;
    int pw_r = BotPadWalk(l, hh, mag);
    g_pwLearn = 1;
    if (pw_r) {
        g_hbot.pad_bad = 1;
        PadinjStickOff();
        HBLOG
            ("bot: dh pad stick unusable - standing and pressing only tms %llu",
             PT_MS);
    }
    return 0;
}

// ---- program load
static void BotLoad(void)
{
    FILE *f = ExeSideOpen("t2r-bot.txt", "r");
    if (!f) {
        if (g_bot.n || g_bot.started) {
            Xlog("bot: t2r-bot.txt gone - bot idle");
            g_bot.n = 0;
            g_bot.started = 0;
            g_bot.god = 0;
            g_bot.melee_until = g_bot.evade_until = 0;
            PadinjStickOff();
            PadinjNofocus(0);
        }
        g_bot.mtime_lo = g_bot.mtime_hi = 0;
        return;
    }
    HANDLE h = (HANDLE) _get_osfhandle(_fileno(f));
    FILETIME ft = { 0 };
    if (h != INVALID_HANDLE_VALUE)
        GetFileTime(h, NULL, NULL, &ft);
    if (ft.dwLowDateTime == g_bot.mtime_lo
        && ft.dwHighDateTime == g_bot.mtime_hi && g_bot.started) {
        fclose(f);
        return;
    }
    g_bot.mtime_lo = ft.dwLowDateTime;
    g_bot.mtime_hi = ft.dwHighDateTime;
    g_bot.n = 0;
    g_bot.cur = 0;
    g_bot.ph = 0;
    g_bot.loops_left = g_bot.loops_total = 0;
    g_bot.tslot = -1;
    g_bot.done_logged = 0;
    g_bot.god = 0;
    g_bot.melee_until = g_bot.evade_until = 0;
    char b[BOT_MAXLINE + 8];
    while (fgets(b, sizeof b, f) && g_bot.n < BOT_MAXLINE) {
        char *p = b;
        while (*p == ' ' || *p == '\t')
            p++;
        size_t L = strlen(p);
        while (L
               && (p[L - 1] == '\r' || p[L - 1] == '\n' || p[L - 1] == ' '
                   || p[L - 1] == '\t'))
            p[--L] = 0;
        if (!*p || *p == '#')
            continue;
        if (!strncmp(p, "script ", 7)) {
            char nm[40] = "", role[16] = "attacker";
            float tp[4] = { 0 };
            int k =
                sscanf(p + 7, "%39s %15s %f %f %f %f", nm, role, &tp[0],
                       &tp[1], &tp[2], &tp[3]);
            if (k >= 1)
                BotScript(nm, role, tp, k >= 6);
            continue;
        }
        snprintf(g_bot.ln[g_bot.n++].t, BOT_MAXLINE, "%s", p);
    }
    fclose(f);
    if (g_bot.n && !strncmp(g_bot.ln[g_bot.n - 1].t, "loop ", 5)) {
        g_bot.loops_total = g_bot.loops_left =
            atoi(g_bot.ln[g_bot.n - 1].t + 5);
        g_bot.n--;
    }
    g_bot.started = 1;
    g_bot.step_ms = 0;
    Xlog("bot: program loaded (%d lines%s, self slot %d) tms %llu",
         g_bot.n, g_bot.loops_total ? ", looped" : "", BotSelf(), PT_MS);
    for (int i = 0; i < g_bot.n; i++)
        Xlog("bot:   %02d %s", i + 1, g_bot.ln[i].t);
}

static void BotNext(void)
{
    g_bot.cur++;
    g_bot.ph = 0;
    g_bot.cmd_ms = 0;
    PadinjStickOff();
}

// one command step: 0 running, 1 OK, -1 FAIL (the FAIL reason is logged by the caller)
static int BotStep(char *cmd, const char *args, char *why, size_t wn)
{
    uint64_t now = GetTickCount64();
    uint64_t el = now - g_bot.cmd_ms;
    float me[16];
    const float *l = Uc4MyLoc(me);
    float o[8];
    if (!strcmp(cmd, "wait")) {
        return el >= (uint64_t) atoi(args) ? 1 : 0;
    }
    if (!strcmp(cmd, "mark")) {
        Xlog("bot: MARK %s tms %llu", args, PT_MS);
        return 1;
    }
    if (!strcmp(cmd, "target")) {
        g_bot.tslot = !strcmp(args, "nearest") || !*args ? -1 : atoi(args);
        return 1;
    }
    if (!strcmp(cmd, "god")) {
        g_bot.god = atoi(args);
        return 1;
    }
    if (!strcmp(cmd, "hp")) {
        if (!BotReady())
            return el > 15000 ? (snprintf(why, wn, "not in a live match"),
                                 -1) : 0;
        float v = (float) atof(args);
        uint64_t a[2] = { SID_PLAYER, 0 };
        memcpy(&a[1], &v, 4);
        DcCall(0xcac2cb5a30fccf12ULL, 2, a);
        snprintf(why, wn, "hp set to %.0f", v);
        return 1;
    }
    if (!strcmp(cmd, "gxabort")) {      // "gxabort [ms]": wait for an execution we play (either half), abort it ms (default 1500) after it started, as when the executioner is downed
        if (!g_gx.until || !g_gx.kill_at) {
            g_bot.ph = 0;
            return el >
                30000 ? (snprintf(why, wn, "no execution within 30 s"),
                         -1) : 0;
        }
        float at_s = (*args ? atoi(args) : 1500) / 1000.0f;     // (timed from the move's start: kill_at - kill_s; a press before this command takes 1.5 s)
        if (at_s < g_gx.kill_s
            && g_t60 + S60(g_gx.kill_s - at_s) < g_gx.kill_at)
            return 0;
        GxAbort(g_gx.role == 1 ? "we were downed" : "bot gxabort", 1);
        snprintf(why, wn, "execution aborted (role %d)", g_gx.role);
        return 1;
    }
    if (!strcmp(cmd, "pullvar")) {      // ps45 prone: the pull-out variant (melee_pullout.c g_pul_var_force)
        char w[16] = "";
        sscanf(args, "%15s", w);
        int v =
            !strcmp(w, "sp") ? 0 : !strcmp(w, "a") ? 1 : !strcmp(w,
                                                                 "b") ? 2 :
            !strcmp(w, "c") ? 3 : !strcmp(w, "cycle") ? -1 : !strcmp(w,
                                                                     "file")
            ? -2 : -9;
        if (v == -9) {
            snprintf(why, wn, "need sp|a|b|c|cycle|file");
            return -1;
        }
        g_pulVarForce = v;
        snprintf(why, wn, "pull-out variant %s", w);
        return 1;
    }
    if (!strcmp(cmd, "pullpos")) {      // ps45 prone: next to the nearest prone enemy, on one side of its body axis (pul_bearing: 0 past its head, +90 its left, 180 its feet)
        if (!BotReady())
            return el > 15000 ? (snprintf(why, wn, "not in a live match"),
                                 -1) : 0;
        if ((PsyncActive() || PulAttActive()) && el < 12000)
            return 0;
        char side[16] = "feet";
        float m = 1.0f;
        sscanf(args, "%15s %f", side, &m);
        if (!(m > 0.3f && m < 3.0f))
            m = 1.0f;
        float want =
            !strcmp(side, "left") ? 90.0f : !strcmp(side,
                                                    "right") ? -90.0f :
            !strcmp(side, "head") ? 0.0f : 180.0f;
        float mb[16];
        const float *me = PlayerLocator(mb);
        if (!me || !PsLocOk(me))
            return el > 5000 ? (snprintf(why, wn, "no own locator"),
                                -1) : 0;
        int best = -1;
        float bd = 1e9f, bl[8];
        uint64_t bh = 0;
        for (int s = 0; s < T2R_MAX_PEERS; s++) {
            if (!PulSlotOk(s))
                continue;
            uint64_t h = PuppetHandle((uint32_t) s);
            float lb[16], hd[3];
            const float *vl = h ? PuppetLocator(h, lb) : NULL;
            if (!vl || !PsLocOk(vl) || !GxJointPos(h, "headb", hd)
                || hd[1] - vl[1] >= PUL_PRONE_HEAD)
                continue;
            float d =
                (me[0] - vl[0]) * (me[0] - vl[0]) + (me[2] -
                                                     vl[2]) * (me[2] -
                                                               vl[2]);
            if (d < bd) {
                bd = d;
                best = s;
                bh = h;
                memcpy(bl, vl, sizeof bl);
            }
        }
        if (best < 0)
            return el > 8000 ? (snprintf(why, wn, "no prone enemy"),
                                -1) : 0;
        float bp[8], be = 1e9f;
        for (int k = 0; k < 72; k++) {  // the world direction whose bearing round the body answers `want` (pul_bearing's own sense, no sign assumed)
            float a = (float) k * 5.0f * 0.0174533f, p[8];
            memcpy(p, bl, sizeof p);
            p[0] = bl[0] + __builtin_sinf(a) * m;
            p[2] = bl[2] + __builtin_cosf(a) * m;
            float e =
                __builtin_fabsf(PulWrap(PulBearing(bh, bl, p) - want));
            if (e < be) {
                be = e;
                memcpy(bp, p, sizeof bp);
            }
        }
        float yaw = __builtin_atan2f(bl[0] - bp[0], bl[2] - bp[2]), t[4] =
            { bp[0], bl[1] + 0.05f, bp[2], yaw * 57.29578f };
        PtTp(t, yaw);
        g_testTpFrame = g_frame;
        snprintf(why, wn,
                 "at slot %d's %s, %.2f m (bearing error %.0f deg) %.2f %.2f %.2f yaw %.0f",
                 best, side, (double) m, (double) be, (double) t[0],
                 (double) t[1], (double) t[2], (double) t[3]);
        return 1;
    }
    if (!strcmp(cmd, "proneidle")) {    // ps45 prone: stay prone (the victim of the pull-out scenario)
        static uint64_t last;
        int ms = *args ? atoi(args) : 30000;
        if (el >= (uint64_t) ms) {
            snprintf(why, wn, "prone idle over");
            return 1;
        }
        uint64_t now = GetTickCount64();
        if (BotReady() && !PsyncActive() && !g_pulv.on
            && now - last >= 1000) {
            last = now;
            uint64_t none = 0, pr =
                DcCall(FNV64("player-is-prone?"), 0, &none);
            if (!(pr != ~0ULL && (pr & 0xff))) {
                DcCall(FNV64("player-force-prone"), 0, &none);
                Xlog("bot: proneidle - prone again");
            }
        }
        return 0;
    }
    if (!strcmp(cmd, "prone")) {        // "prone": force our player prone (player-force-prone; pull-out tests)
        if (!BotReady())
            return el > 15000 ? (snprintf(why, wn, "not in a live match"),
                                 -1) : 0;
        uint64_t none = 0;
        DcCall(FNV64("player-force-prone"), 0, &none);
        uint64_t pr = DcCall(FNV64("player-is-prone?"), 0, &none);
        if (!(pr & 0xff) && el < 2000)
            return 0;
        snprintf(why, wn, "prone %s", (pr & 0xff) ? "yes" : "no");
        return (pr & 0xff) ? 1 : -1;
    }
    if (!strcmp(cmd, "clip")) { // "clip <n>": loaded rounds of the weapon in hand (set-player-weapon-clip-ammo)
        if (!BotReady())
            return el > 15000 ? (snprintf(why, wn, "not in a live match"),
                                 -1) : 0;
        int n = atoi(args);
        int ok = AmmoSet(n);
        snprintf(why, wn, "clip %d -> %s, now %d", n,
                 ok ? "set" : "failed", AmmoClip());
        return ok ? 1 : -1;
    }
    if (!strcmp(cmd, "give")) { // "give <item> [count]": any item by name through the give path (ammo for a weapon test: give ammo-rifle 8)
        if (!BotReady())
            return el > 15000 ? (snprintf(why, wn, "not in a live match"),
                                 -1) : 0;
        char w[64];
        int n = 1;
        if (sscanf(args, "%63s %d", w, &n) < 1) {
            snprintf(why, wn, "no item");
            return -1;
        }
        uint64_t ret[64] = { 0 }, give[4] =
            { FNV64(w), 0, 0, (uint64_t) (n > 0 ? n : 1) };
        if (g_give_orig)
            g_give_orig(ret, 4, give, 0);
        snprintf(why, wn, "%s x%d given", w, n);
        return 1;
    }
    if (!strcmp(cmd, "weapon")) {
        if (!BotReady())
            return el > 15000 ? (snprintf(why, wn, "not in a live match"),
                                 -1) : 0;
        char w[64];
        if (sscanf(args, "%63s", w) != 1) {
            snprintf(why, wn, "no weapon");
            return -1;
        }
        int fists = !strcmp(w, "fists");
        const char *id = fists ? "fists" : BotWeaponId(w);
        uint64_t sid = FNV64(id);
        // (melee matrix: after 'weapon machete' every later 'weapon crowbar / baseball-bat / two-by-four' drew the new one, but the engine's
        //  melee kept swinging the machete (its set, its damage) - giving never takes the old melee weapon away, a pickup in game swaps it.
        //  Every other melee weapon is taken first, so the case gets the weapon it names.)
        static const char *const MEL[] =
            { "crowbar", "pipe-wrench", "hammer", "hatchet", "tomahawk",
"machete", "baseball-bat", "two-by-four", "lead-pipe",
            "knife-survival-mel-player", "golf-club", "sickle"
        };
        int took = 0;
        for (size_t i = 0; i < sizeof MEL / sizeof *MEL; i++) {
            uint64_t m = FNV64(MEL[i]);
            if (m == sid)
                continue;
            uint64_t has = DcCall(FNV64("has-weapon?"), 1, &m);
            if (has != ~0ULL && (has & 0xff)) {
                DcCall(FNV64("remove-player-weapon"), 1, &m);
                took++;
            }
        }
        // ('weapon fists' only holstered - a crowbar given before stayed the melee weapon, the 'knife' cases swung the crowbar)
        if (fists) {
            uint64_t z[2] = { 0, 0 };
            uint64_t r =
                DcCall(FNV64("holster-player-weapon-instantly"), 2, z);
            snprintf(why, wn,
                     "fists (holster -> %llx), %d melee weapon(s) taken",
                     (unsigned long long) r, took);
            return 1;
        }
        uint64_t ret[64] = { 0 }, give[4] = { sid, 0, 0, 1 };
        if (g_give_orig)
            g_give_orig(ret, 4, give, 0);
        uint64_t r =
            DcCall(FNV64("player-instant-draw-weapon-by-name"), 1, &sid);
        snprintf(why, wn,
                 "%s given + drawn (-> %llx), held now %llx, %d other melee weapon(s) taken",
                 id, (unsigned long long) r,
                 (unsigned long long) CurrentWeapon(), took);
        return 1;
    }
    if (!strcmp(cmd, "teleport")) {
        float t[4];
        if (sscanf(args, "%f %f %f %f", &t[0], &t[1], &t[2], &t[3]) != 4) {
            snprintf(why, wn, "need x y z yaw");
            return -1;
        }
        if (!BotReady())
            return el > 15000 ? (snprintf(why, wn, "not in a live match"),
                                 -1) : 0;
        if ((PsyncActive() || MduSwinging()) && el < 10000)
            return 0;           // Wait for a paired move / our duel swing (ps22: the warp ran across the teleport = the 2.54 m SNAP) to end (a teleport inside one = the pair in a different spot)
        PtTp(t, t[3] * 0.0174533f);
        g_testTpFrame = g_frame;
        g_botHome[0] = t[0];
        g_botHome[1] = t[1];
        g_botHome[2] = t[2];
        g_botHome[3] = t[3];
        g_botHomeOk = 1;
        snprintf(why, wn, "teleported to %.2f %.2f %.2f yaw %.0f", t[0],
                 t[1], t[2], t[3]);
        return 1;
    }
    if (!strcmp(cmd, "animlen")) {      // dev probe: "animlen <clip>" = is the clip loaded for our body (get-animation-duration, 0 = no)
        char c[128];
        if (sscanf(args, "%127s", c) != 1) {
            snprintf(why, wn, "need a clip name");
            return -1;
        }
        snprintf(why, wn, "animlen %s = %.3f s", c,
                 (double) AnimLen(SID_PLAYER, c));
        return 1;
    }
    if (!strcmp(cmd, "anim")) { // dev probe: "anim <clip>" = play the clip on our body (0.15 s blend)
        char c[128];
        if (sscanf(args, "%127s", c) != 1) {
            snprintf(why, wn, "need a clip name");
            return -1;
        }
        if (!BotReady())
            return el > 15000 ? (snprintf(why, wn, "not in a live match"),
                                 -1) : 0;
        uint64_t r = MhrAnimate(SID_PLAYER, Fnv64Rt(c), 0.0f, 0.15f);
        snprintf(why, wn, "anim %s (%.3f s) -> %llx", c,
                 (double) AnimLen(SID_PLAYER, c), (unsigned long long) r);
        return 1;
    }
    if (!strcmp(cmd, "duel-human"))
        return HbotStep(args, why, wn, el, now);       // (its own readiness handling: a long command that survives deaths / respawns)
    if (!BotReady()) {
        if (el > 20000) {
            snprintf(why, wn, "not in a live match / downed for 20 s");
            return -1;
        }
        return 0;
    }
    if (!strcmp(cmd, "face")) {
        char who[16] = "";
        int ms = 300;
        sscanf(args, "%15s %d", who, &ms);
        if (who[0] && strcmp(who, "nearest"))
            g_bot.tslot = atoi(who);
        else if (!strcmp(who, "nearest"))
            g_bot.tslot = -1;
        int s = BotOpp(o);
        if (s < 0 || !l)
            return el >
                10000 ? (snprintf(why, wn, "no live enemy to face"),
                         -1) : 0;
        float yaw = __builtin_atan2f(o[0] - l[0], o[2] - l[2]);
        BotSetPose(l, l[0], l[2], yaw);
        if (el < (uint64_t) ms)
            return 0;
        snprintf(why, wn, "faced slot %d (yaw %.0f deg, flat dist %.2f m)",
                 s, yaw * 57.2958f,
                 __builtin_sqrtf((o[0] - l[0]) * (o[0] - l[0]) +
                                 (o[2] - l[2]) * (o[2] - l[2])));
        return 1;
    }
    if (!strcmp(cmd, "approach") || !strcmp(cmd, "retreat")) {
        int ap = cmd[0] == 'a';
        float m = 1.4f;
        char mode[8] = "run";
        sscanf(args, "%f %7s", &m, mode);
        int s = BotOpp(o);
        if (s < 0 || !l)
            return el > 10000 ? (snprintf(why, wn, "no live enemy"),
                                 -1) : 0;
        float dx = o[0] - l[0], dz = o[2] - l[2], d =
            __builtin_sqrtf(dx * dx + dz * dz);
        if (el < 60) {
            g_bot.pv_t = 0;
            g_bot.walk_t0 = 0;
            g_bot.walk_ck = 0;
            if (g_bot.pad_cal >= 1 && g_bot.pad_cal <= 3)
                g_bot.pad_cal = 0;
        }
        if (ap ? d <= m + (g_bot.pad_cal == 4 ? 0.12f : 0.05f) : d >=
            m - (g_bot.pad_cal == 4 ? 0.12f : 0.05f)) {
            PadinjStickOff();
            snprintf(why, wn,
                     "reached flat distance %.2f m to slot %d in %llu ms (%s)",
                     d, s, (unsigned long long) el,
                     g_bot.pad_cal ==
                     4 ? "walked with the pad stick" : "stepped");
            return 1;
        }
        if (el >
            (g_bot.pad_cal == 4 || g_bot.pad_cal == 0 ? 14000u : 10000u)) {
            PadinjStickOff();
            snprintf(why, wn,
                     "timeout, still %.2f m from slot %d (wanted %s %.2f)",
                     d, s, ap ? "<=" : ">=", m);
            return -1;
        }
        if (ap && d > m + 8.0f && g_bot.pad_cal != 1 && g_bot.pad_cal != 2 && g_bot.pad_cal != 3) {     // far (A/B: 100 m, 10 s timeout): teleport to 5 m short, then walk the rest with the stick
            float k = (d - m - 5.0f) / d, t3[3] =
                { l[0] + dx * k, o[1], l[2] + dz * k };
            PtTp(t3, __builtin_atan2f(dx, dz));
            Xlog("bot: approach teleported %.1f m closer (far), walking the last 5 m tms %llu", d - m - 5.0f, PT_MS);
            return 0;
        }
        if (g_bot.pad_cal != 9) {       // the engine-level left stick (padinject.c): real locomotion, the camera heading / handedness learned by a two-leg probe
            float hd =
                ap ? __builtin_atan2f(dx, dz) : __builtin_atan2f(-dx, -dz),
                mag = !strcmp(mode, "walk") ? 0.5f : 1.0f, rem =
                ap ? d - m : m - d;
            if (rem < 0.9f) {
                float k = rem / 0.9f;
                if (k < 0.45f)
                    k = 0.45f;
                if (k < mag)
                    mag = k;
            }
            if (g_bot.pad_cal == 4) {   // progress watchdog: a calibrated stick that gains < 0.3 m in 2.5 s is given up for the rest of the program
                if (!g_bot.walk_ck) {
                    g_bot.walk_ck = now;
                    g_bot.walk_d0 = d;
                } else if (now - g_bot.walk_ck >= 2500) {
                    float prog =
                        ap ? g_bot.walk_d0 - d : d - g_bot.walk_d0;
                    if (prog < 0.3f) {
                        g_bot.pad_cal = 9;
                        PadinjStickOff();
                        Xlog("bot: pad walk made no progress in 2.5 s (%.2f m -> %.2f m) - falling back to stepping / teleport tms %llu", g_bot.walk_d0, d, PT_MS);
                        return 0;
                    }
                    g_bot.walk_ck = now;
                    g_bot.walk_d0 = d;
                }
            }
            if (BotPadWalk(l, hd, mag) == 0)
                return 0;
        }
        if (ap && el > 600 && d > m + 0.5f) {   // set-object-locator steps are undone by the locomotion controller on a live player (A/B: 37 m, no progress): one pairtest teleport to m in front of the target, facing it
            float k = (d - m) / d, t3[3] =
                { l[0] + dx * k, o[1], l[2] + dz * k };
            PtTp(t3, __builtin_atan2f(dx, dz));
            Xlog("bot: approach teleported %.2f m (stepping made no progress) tms %llu", d - m, PT_MS);
            return 0;
        }
        LARGE_INTEGER q, fq;
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&fq);
        double dt =
            g_bot.last_qpc ? (double) (q.QuadPart -
                                       (long long) g_bot.last_qpc) /
            (double) fq.QuadPart : 0.016;
        g_bot.last_qpc = (uint64_t) q.QuadPart;
        if (dt > 0.05)
            dt = 0.05;
        if (dt < 0.001)
            dt = 0.001;
        float sp = !strcmp(mode, "walk") ? 2.0f : 4.5f, st =
            sp * (float) dt, want = ap ? d - m : m - d;
        if (st > want)
            st = want;
        if (st > 0.1f)
            st = 0.1f;
        float k = (ap ? st : -st) / (d > 0.001f ? d : 1.0f);
        BotSetPose(l, l[0] + dx * k, l[2] + dz * k,
                     __builtin_atan2f(dx, dz));
        return 0;
    }
    if (!strcmp(cmd, "stand")) {
        uint64_t none = 0;
        DcCall(FNV64("player-force-stand"), 0, &none);
        snprintf(why, wn, "player-force-stand");
        return 1;
    }
    if (!strcmp(cmd, "towall")) {       // "towall [gap_m]": put us with our back to the nearest flat wall (16 directions x 3 heights up to 1.8 m + knee rays 0.35 m to the sides: ctxdown's wall test (a plane above chest height), 25 m), gap_m off it (default 0.45), facing away - a pin / wall takedown spot
        static uint64_t j[16][5];
        static float org[3];
        float gap = 0.45f;
        sscanf(args, "%f", &gap);
        float lb[16];
        const float *me = PlayerLocator(lb);
        if (g_bot.ph == 0) {
            if (!me) {
                snprintf(why, wn, "no locator");
                return -1;
            }
            org[0] = me[0];
            org[1] = me[1];
            org[2] = me[2];
            for (int i = 0; i < 16; i++) {
                float a = (float) i * 0.39269908f, sx =
                    __builtin_sinf(a), sz = __builtin_cosf(a);
                for (int k = 0; k < 5; k++) {
                    float so =
                        k == 3 ? 0.35f : k == 4 ? -0.35f : 0.0f, f[3] =
                        { org[0] + so * sz,
                     org[1] + (k == 2 ? 1.8f : k == 1 ? 1.2f : 0.45f),
                     org[2] - so * sx }, t[3] =
 { f[0] + 25.0f * sx, f[1], f[2] + 25.0f * sz };
                    j[i][k] = WorldRayKick(f, t);
                }
            }
            g_bot.ph = 1;
            return 0;
        }
        int best = -1;
        float bd = 99.0f;
        for (int i = 0; i < 16; i++) {
            float t0 = RayT(j[i][0]), t1 = RayT(j[i][1]), t2 =
                RayT(j[i][2]), t3 = RayT(j[i][3]), t4 = RayT(j[i][4]);
            if (t0 >= 0.0f && t0 < 1.0f && t1 >= 0.0f && t1 < 1.0f
                && t2 >= 0.0f && t2 < 1.0f
                && __builtin_fabsf(t0 - t1) * 25.0f < 0.25f
                && __builtin_fabsf(t2 - t1) * 25.0f < 0.25f && t3 >= 0.0f
                && t3 < 1.0f && t4 >= 0.0f && t4 < 1.0f
                && __builtin_fabsf(t3 - t0) * 25.0f < 0.3f
                && __builtin_fabsf(t4 - t0) * 25.0f < 0.3f) {
                float d = 25.0f * (t0 < t1 ? t0 : t1);
                if (d > gap + 0.05f && d < bd) {
                    bd = d;
                    best = i;
                }
            }
        }
        if (best < 0) {         // nothing flat here: hop next to the nearest chest-height hit and look again (3 hops)
            static int hops, hcur = -1;
            static uint64_t hat;
            if (hcur != (int) g_bot.cur || now - hat > 5000)
                hops = 0;
            hcur = (int) g_bot.cur;
            hat = now;
            int nb = -1;
            float nd = 99.0f;
            for (int i = 0; i < 16; i++) {
                float t1 = RayT(j[i][1]);
                if (t1 >= 0.0f && t1 < 1.0f && 25.0f * t1 > 2.0f
                    && 25.0f * t1 < nd) {
                    nd = 25.0f * t1;
                    nb = i;
                }
            }
            if (nb < 0 || hops++ >= 3) {
                hops = 0;
                snprintf(why, wn, "no flat wall within 25 m (after hops)");
                return -1;
            }
            float a = (float) nb * 0.39269908f, sx =
                __builtin_sinf(a), sz = __builtin_cosf(a), pos[3] =
                { org[0] + sx * (nd - 1.5f), org[1],
org[2] + sz * (nd - 1.5f) };
            PtTp(pos, __builtin_atan2f(sx, sz));
            Xlog("bot: towall: no flat wall here - hop %d to %.1f m toward the nearest hit (%d deg) tms %llu", hops, (double) nd, nb * 225 / 10, PT_MS);
            g_bot.ph = 0;
            return 0;
        }
        float a = (float) best * 0.39269908f, sx = __builtin_sinf(a), sz =
            __builtin_cosf(a), pos[3] =
            { org[0] + sx * (bd - gap), org[1], org[2] + sz * (bd - gap) };
        PtTp(pos, __builtin_atan2f(-sx, -sz));
        snprintf(why, wn,
                 "wall %.2f m away at %d deg - placed %.2f m off it at %.2f %.2f %.2f, facing away",
                 (double) bd, best * 225 / 10, (double) gap,
                 (double) pos[0], (double) pos[1], (double) pos[2]);
        return 1;
    }
    if (!strcmp(cmd, "unpause")) {      // the pause menu opens when an unfocused A/B window is moved / loses focus - pop it (menu2-pop-page), no key input
        if (PageOnStack(0x27fd8e283ee18ea2ULL /* pause-menu */ )) {
            if (g_bot.ph == 0) {
                PopPage();
                g_bot.ph = 1;
                g_bot.cmd_ms = now;
                Xlog("bot: unpause: pause menu popped tms %llu", PT_MS);
            }
            return now - g_bot.cmd_ms >
                3000 ? (snprintf(why, wn, "pause menu still on the stack"),
                        -1) : 0;
        }
        snprintf(why, wn, "%s",
                 g_bot.ph ? "pause menu closed" : "no pause menu");
        return 1;
    }
    if (!strcmp(cmd, "hold")) { // melee matrix: "hold <command> <ms>" = engine-level press of any joypad command held ms, NON-blocking (the next line runs at once:
        // hold aim 1500 + stick = aim and turn; hold sprint 1200 + stick 0 1 = sprint)
        char c[48] = "";
        int ms = 150;
        sscanf(args, "%47s %d", c, &ms);
        PadinjInstall();
        PadinjNofocus(1);
        g_padinjBotPress = 1;
        int r = PadinjPress(c, ms);
        g_padinjBotPress = 0;
        if (!strcmp(c, "melee-attack"))
            g_botMeleeMs = GetTickCount64();
        if (!r) {               // not learned on the pad path (the engine never read it through the hooked joypad reader): answer the script native joypad-command-active? for it
            if (!strcmp(c, "evade") || !strcmp(c, "dodge")) {
                BotPress(0, ms);
                snprintf(why, wn,
                         "%s held %d ms (wrapper: the dodge path)", c, ms);
                return 1;
            }
            BotInstall();
            g_bot.gen_sid = Fnv64Rt(c);
            g_bot.gen_hits = 0;
            g_bot.gen_until = GetTickCount64() + (uint64_t) ms;
            snprintf(why, wn,
                     "%s held %d ms (wrapper: joypad-command-active? answers yes - only script-side readers see it)",
                     c, ms);
            return 1;
        }
        snprintf(why, wn, "%s held %d ms (pad)", c, ms);
        return 1;
    }
    if (!strcmp(cmd, "stick")) {        // "stick <lx> <ly> <rx> <ry> <ms>" = both sticks (-1..1, ly > 0 forward, rx > 0 look right) for ms (blocking for ms): camera turns,
        // walk / strafe in any direction without the approach steering
        float lx = 0, ly = 0, rx = 0, ry = 0;
        int ms = 300;
        sscanf(args, "%f %f %f %f %d", &lx, &ly, &rx, &ry, &ms);
        if (g_bot.ph == 0) {
            PadinjInstall();
            PadinjNofocus(1);
            g_bot.ph = 1;
        }
        PadinjStick(lx, ly, rx, ry, 60, g_bot.pad_swap);
        if (el < (uint64_t) ms)
            return 0;
        PadinjStickOff();
        snprintf(why, wn, "sticks L %.2f %.2f R %.2f %.2f for %d ms", lx,
                 ly, rx, ry, ms);
        return 1;
    }
    if (!strcmp(cmd, "press")) {        // "press <command> [ms]": one engine-level press of any joypad command, crouch / prone state logged before and 1.5 s after
        char c[48] = "";
        int ms = 150;
        sscanf(args, "%47s %d", c, &ms);
        uint64_t none = 0;
        if (g_bot.ph == 0) {
            PadinjInstall();
            PadinjNofocus(1);
            uint64_t cr = DcCall(FNV64("player-is-crouched?"), 0, &none);
            g_padinjBotPress = 1;
            int r = PadinjPress(c, ms);
            g_padinjBotPress = 0;
            g_bot.ph = 1;
            g_bot.cmd_ms = now;
            Xlog("bot: press %s %d ms (%s), crouched before %d tms %llu",
                 c, ms, r ? "pad" : "NOT injected", cr != ~0ULL
                 && (cr & 0xff), PT_MS);
            return r ? 0 : (snprintf(why, wn, "%s not injected", c), -1);
        }
        if (el < 1500)
            return 0;
        uint64_t cr = DcCall(FNV64("player-is-crouched?"), 0, &none);
        snprintf(why, wn, "crouched after %d", cr != ~0ULL && (cr & 0xff));
        return 1;
    }
    if (!strcmp(cmd, "box")) {  // "box [dist]": stand dist m (default 1.0) from the nearest closed + ready supply box facing it, then press interact (box reach test, supplybox.c "boxreach:" lines)
        typedef void *(*fn_proc)(uint64_t);
        static float bp[3];
        float dist = 1.0f;
        sscanf(args, "%f", &dist);
        uint8_t *beg = *(uint8_t **) T2R_SYM(kPeerListBegin), *end =
            *(uint8_t **) T2R_SYM(kPeerListEnd);
        if (!beg || end <= beg || (size_t) (end - beg) > 0x48 * 64
            || IsBadReadPtr(beg, (size_t) (end - beg)) || !l) {
            snprintf(why, wn, "no supply box on this map");
            return -1;
        }
        if (g_bot.ph == 0) {    // records 0x48: +0 handle, +0x10 ready time (game time, state +0x2c90), +0x14 open flag (the engine opens the nearest = #0 after a sort)
            uint8_t *gs =
                ((uint8_t * (*)(void)) T2R_SYM(kFnNetPlayersCtx)) ();
            float gt = gs
                && !IsBadReadPtr(gs + 0x2c90,
                                 4) ? *(float *) (gs + 0x2c90) : 0.0f, bd =
                1e9f;
            int nb = 0, nready = 0;
            for (uint8_t * r = beg; r + 0x48 <= end; r += 0x48, nb++) {
                if (r[0x14] || gt < *(float *) (r + 0x10))
                    continue;
                uint64_t h = *(uint64_t *) r;
                uint8_t *proc =
                    h ? ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h) :
                    NULL;
                float p3[3];
                if (!proc || !ProcPos(proc, p3))
                    continue;
                nready++;
                float ex = p3[0] - l[0], ez = p3[2] - l[2], e =
                    ex * ex + ez * ez;
                if (e < bd) {
                    bd = e;
                    memcpy(bp, p3, sizeof bp);
                }
            }
            if (!nready)
                return el >
                    30000
                    ? (snprintf
                       (why, wn, "no closed + ready box of %d for 30 s",
                        nb), -1) : 0;
            float dx = bp[0] - l[0], dz = bp[2] - l[2], d =
                __builtin_sqrtf(dx * dx + dz * dz);
            if (d < 0.05f) {
                dx = 0.0f;
                dz = 1.0f;
                d = 1.0f;
            }
            float t3[3] =
                { bp[0] - dx / d * dist, bp[1], bp[2] - dz / d * dist };
            PtTp(t3, __builtin_atan2f(dx, dz));
            g_bot.ph = 1;
            g_bot.cmd_ms = now;
            Xlog("bot: box - %d of %d boxes closed + ready, nearest %.1f m away tms %llu", nready, nb, __builtin_sqrtf(bd), PT_MS);
            return 0;
        }
        float dx = bp[0] - l[0], dz = bp[2] - l[2], d =
            __builtin_sqrtf(dx * dx + dz * dz);
        if (g_bot.ph == 1) {
            if (el < 800)
                return 0;
            PadinjInstall();
            PadinjNofocus(1);
            int r = PadinjPress("interact", 150);
            g_bot.ph = 2;
            g_bot.cmd_ms = now;
            Xlog("bot: box press (%s) at %.2f m, box dy %.2f tms %llu",
                 r ? "pad" : "NOT injected", d, bp[1] - l[1], PT_MS);
            return r ? 0
                : (snprintf
                   (why, wn,
                    "interact press not injected (pad bit unknown)"), -1);
        }
        if (el < 2500)
            return 0;
        int open = 0;
        for (uint8_t * r = beg; r + 0x48 <= end; r += 0x48) {
            uint64_t h = *(uint64_t *) r;
            uint8_t *proc =
                h ? ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h) : NULL;
            float p3[3];
            if (proc && ProcPos(proc, p3)
                && __builtin_fabsf(p3[0] - bp[0]) < 0.05f
                && __builtin_fabsf(p3[2] - bp[2]) < 0.05f)
                open = r[0x14] != 0;
        }
        snprintf(why, wn, "box %s after the press",
                 open ? "OPEN" : "still closed");
        return open ? 1 : -1;
    }
    if (!strcmp(cmd, "melee")) {
        int n = 1, gap = 900;
        sscanf(args, "%d %d", &n, &gap);
        if (n < 1)
            n = 1;
        if (g_bot.ph == 0) {
            g_bot.i0 = 0;
            g_bot.i1 = 0;
            g_bot.ph = 1;
            g_bot.a0 = 0;
        }
        if (g_bot.ph == 1) {    // press i0
            if (g_bot.i0 >= n) {
                snprintf(why, wn, "%d press(es), %d started a swing", n,
                         g_bot.i1);
                return g_bot.i1 > 0 ? 1 : -1;
            }
            g_bot.a0 = now;
            g_bot.a1 = g_msAtk ? 0 : 1;
            g_bot.f0 = 0;
            BotPress(1, 110);
            g_bot.ph = 2;
            Xlog("bot: melee press %d/%d injected (path %s) tms %llu",
                 g_bot.i0 + 1, n, BotPathName(), PT_MS);
            return 0;
        }
        // ph 2: watch for the swing start, then wait out the gap
        int started = g_msAtk && (g_bot.f0 == 0.0f);
        if (started) {
            g_bot.f0 = (float) (now - g_bot.a0);
            g_bot.i1++;
            Xlog("bot: melee press %d: swing started %.0f ms after the press (path %s, engine reads %u) tms %llu", g_bot.i0 + 1, g_bot.f0, BotPathName(), BotReads(1), PT_MS);
        }
        if (now - g_bot.a0 >= (uint64_t) gap) {
            if (g_bot.f0 == 0.0f)
                Xlog("bot: melee press %d: NO swing within %d ms (path %s, engine reads %u; mpace lock, dodge lock or a busy state?) tms %llu", g_bot.i0 + 1, gap, BotPathName(), BotReads(1), PT_MS);
            g_bot.i0++;
            g_bot.ph = 1;
        }
        return 0;
    }
    if (!strcmp(cmd, "waitswing")) {
        int to = 10000;
        sscanf(args, "%d", &to);
        int s = BotOpp(o);
        if (s < 0)
            s = g_bot.tslot;
        if (g_bot.ph == 0) {
            g_bot.a0 = g_mdg.seq_n;
            g_bot.ph = 1;
        }
        for (int q = 0; q < T2R_MAX_PEERS; q++)
            if (g_mdg.sw[q].seq > (uint32_t) g_bot.a0
                && !IsTeammate((uint32_t) q)) {
                snprintf(why, wn,
                         "slot %d announced a swing (%llu ms ago)", q,
                         (unsigned long long) (now - g_mdg.sw[q].ms));
                return 1;
            }
        return el >
            (uint64_t) to
            ? (snprintf(why, wn, "no swing announced in %d ms", to),
               -1) : 0;
    }
    if (!strcmp(cmd, "dodge")) {
        char dir[12] = "any", at[12] = "120";
        sscanf(args, "%11s %11s", dir, at);
        if (g_bot.ph == 0) {
            g_bot.a0 = g_mdg.seq_n;
            g_bot.i0 = -1;
            g_bot.ph = strcmp(at, "now") ? 1 : 2;
            if (g_bot.ph == 1)
                Xlog("bot: dodge dir %s (the engine picks the move from the swing class) waits for the next swing announce, press at +%s ms", dir, at);
        }
        if (g_bot.ph == 1) {
            for (int q = 0; q < T2R_MAX_PEERS; q++)
                if (g_mdg.sw[q].seq > (uint32_t) g_bot.a0
                    && !IsTeammate((uint32_t) q)) {
                    g_bot.i0 = q;
                    g_bot.a1 = g_mdg.sw[q].ms + (uint64_t) atoi(at);
                    g_bot.ph = 3;
                    Xlog("bot: dodge: slot %d swing announced %llu ms ago - evade press due in %lld ms tms %llu", q, (unsigned long long) (now - g_mdg.sw[q].ms), (long long) (g_bot.a1 - now), PT_MS);
                    break;
                }
            return el >
                10000
                ? (snprintf(why, wn, "no enemy swing announced in 10 s"),
                   -1) : 0;
        }
        if (g_bot.ph == 3 && now < g_bot.a1)
            return 0;
        if (g_bot.ph == 3 || g_bot.ph == 2) {
            g_bot.a1 = now;
            BotPress(0, 120);
            g_bot.ph = 4;
            Xlog("bot: dodge: evade press injected (path %s, window open %d, cooldown left %lld ms) tms %llu", BotPathName(), g_mdg.window_open, (long long) (g_mdg.cooldown_until > now ? g_mdg.cooldown_until - now : 0), PT_MS);
            return 0;
        }
        if (g_bot.ph == 4) {
            if (g_mdg.mode != 0 || g_mdg.cooldown_until > now) {
                snprintf(why, wn,
                         "the dodge engine took the press: mode %d, i-frames %s, cooldown %lld ms left, path %s, engine reads %u",
                         g_mdg.mode, MdgIframes(now) ? "on" : "off",
                         (long long) (g_mdg.cooldown_until >
                                      now ? g_mdg.cooldown_until -
                                      now : 0), BotPathName(),
                         BotReads(0));
                return 1;
            }
            return now - g_bot.a1 >
                700
                ? (snprintf
                   (why, wn,
                    "press NOT taken (no window open? window_open=%d path %s engine reads %u)",
                    g_mdg.window_open, BotPathName(), BotReads(0)),
                   -1) : 0;
        }
        return -1;
    }
    if (!strcmp(cmd, "pullout")) {      // (melee_pullout.c): as the attacker behind a prone enemy: one melee press (starts the pull-out), then waits for the whole sequence; ok = a pull-out ran to its outcome
        if (g_bot.ph == 0) {
            g_bot.i0 = g_pul.n_started;
            g_bot.a0 = 0;
            BotPress(1, 110);
            g_bot.ph = 1;
            g_bot.a1 = now;
            Xlog("bot: pullout: melee press injected (path %s) tms %llu",
                 BotPathName(), PT_MS);
            return 0;
        }
        if (g_pul.n_started > g_bot.i0 && g_pul.st == PUL_IDLE) {
            snprintf(why, wn,
                     "pull-out ran: countered %d, downed %d, refused %d",
                     g_pul.n_ok, g_pul.n_fail, g_pul.n_refused);
            return 1;
        }
        return now - g_bot.a1 >
            15000
            ? (snprintf
               (why, wn,
                "no pull-out (started %d, state %d; see the pullout: lines)",
                g_pul.n_started, g_pul.st), -1) : 0;
    }
    if (!strcmp(cmd, "pulesc")) {       // as the pulled victim: press E at_ms (default 100) after the counter window opens on us (a negative value = early); ok = the counter was valid
        int at = 100;
        sscanf(args, "%d", &at);
        if (g_bot.ph == 0) {
            g_bot.ph = 1;
        }
        if (g_bot.ph == 1) {
            if (g_pulv.on && !g_pulv.out && g_pulv.grap != 1
                && g_ps.role == 2 && g_ps.uid == g_pulv.uid
                && (double) (PsyncMs() - g_ps.t0) >=
                g_pulv.end_s * 1000.0 - PUL_WIN_LEAD_MS + at) {
                g_botEMs = now;
                g_bot.ph = 2;
                g_bot.a1 = now;
                Xlog("bot: pulesc: E injected %d ms into the window tms %llu", at, PT_MS);
            }
            return el >
                30000 ? (snprintf(why, wn, "no pull-out on us in 30 s"),
                         -1) : 0;
        }
        if (g_pulv.on && !g_pulv.decided && now - g_bot.a1 < 2500)
            return 0;
        snprintf(why, wn, "counter %s",
                 g_pulv.ok ? "VALID" : "failed / none");
        return g_pulv.ok ? 1 : -1;
    }
    if (!strcmp(cmd, "qte")) {  // as a pinned victim, press E (escape) at_ms (default 150) after the next pin QTE opens on us; ok = the press was valid
        int at = 150;
        sscanf(args, "%d", &at);
        if (g_bot.ph == 0) {
            g_bot.a0 = (int64_t) g_mpn.vq_open;
            g_bot.ph = 1;
        }
        if (g_bot.ph == 1) {
            if (g_mpn.vq_open && (int64_t) g_mpn.vq_open != g_bot.a0) {
                g_bot.a1 = g_mpn.vq_open + (uint64_t) at;
                g_bot.ph = 3;
                Xlog("bot: qte: pin QTE id %d open - E in %d ms tms %llu",
                     g_mpn.vq_id, at, PT_MS);
            }
            return el > 30000 ? (snprintf(why, wn, "no pin QTE in 30 s"),
                                 -1) : 0;
        }
        if (g_bot.ph == 3) {
            if (now < g_bot.a1)
                return 0;
            g_botEMs = now;
            g_bot.ph = 4;
            g_bot.a1 = now;
            Xlog("bot: qte: E injected tms %llu", PT_MS);
            return 0;
        }
        if (g_bot.ph == 4) {
            if (g_mpn.vq_ok) {
                snprintf(why, wn, "QTE press valid - escape (id %d)",
                         g_mpn.vq_id);
                return 1;
            }
            return now - g_bot.a1 >
                700
                ? (snprintf
                   (why, wn, "QTE press not taken (burn %d, open %d)",
                    g_mpn.vq_burn, MpnQteOpen()), -1) : 0;
        }
        return -1;
    }
    if (!strcmp(cmd, "counter")) {
        if (g_bot.ph == 0) {
            g_bot.a0 = 0;
            g_bot.i0 = g_dcm.n_started;
            g_bot.ph = 1;
        }
        if (g_bot.ph == 1) {
            if (g_dcm.arm_until > now && now >= g_mdg.melee_ok_at) {
                BotPress(1, 110);
                g_bot.a0 = now;
                g_bot.ph = 2;
                Xlog("bot: counter: armed on slot %u, melee press injected (path %s, %lld ms of window left) tms %llu", g_dcm.arm_slot, BotPathName(), (long long) (g_dcm.arm_until - now), PT_MS);
                return 0;
            }
            return el >
                4000
                ? (snprintf
                   (why, wn,
                    "the dodge counter was never armed in 4 s (no successful dodge?)"),
                   -1) : 0;
        }
        if (g_dcm.n_started > g_bot.i0) {
            snprintf(why, wn,
                     "dodge counter STARTED %llu ms after the press",
                     (unsigned long long) (now - g_bot.a0));
            return 1;
        }
        if (g_dcm.n_plain || g_dcm.n_refused) {
        }
        return now - g_bot.a0 >
            1200
            ? (snprintf
               (why, wn, "no counter started (plain %d, refused %d)",
                g_dcm.n_plain, g_dcm.n_refused), -1) : 0;
    }
    if (!strcmp(cmd, "escape")) {
        if (g_bot.ph == 0) {
            g_bot.i0 = g_dcm.n_escaped;
            g_bot.ph = 1;
        }
        if (g_bot.ph == 1) {
            if (DcEscOpen(now)) {
                BotPress(0, 120);
                g_bot.a0 = now;
                g_bot.ph = 2;
                Xlog("bot: escape: window open, evade press injected (path %s) tms %llu", BotPathName(), PT_MS);
                return 0;
            }
            return el >
                6000
                ? (snprintf
                   (why, wn,
                    "the escape window never opened in 6 s (not countered?)"),
                   -1) : 0;
        }
        if (g_dcm.n_escaped > g_bot.i0) {
            snprintf(why, wn,
                     "ESCAPED the counter %llu ms after the press",
                     (unsigned long long) (now - g_bot.a0));
            return 1;
        }
        return now - g_bot.a0 >
            800
            ? (snprintf(why, wn, "press taken but no escape registered"),
               -1) : 0;
    }
    snprintf(why, wn, "unknown command");
    return -1;
}

static void BotTick(void)
{
    uint64_t now = GetTickCount64();
    if (now - g_bot.poll_ms >= 500) {
        g_bot.poll_ms = now;
        BotLoad();
    }
    if (!g_bot.started)
        return;
    PadinjInstall();           // the engine-level virtual pad (idempotent); the heartbeat / learn lines are written from padinj_tick
    PadinjTick();
    if (g_bot.god && BotReady()) {
        float hp = LocalHp();
        if (hp > g_bot.maxhp)
            g_bot.maxhp = hp;
        if (g_ptMaxhp > g_bot.maxhp)
            g_bot.maxhp = g_ptMaxhp;
        // not inside a paired move: the hp jump aborted the dodge counter on the partner (A/B: "puppet gone")
        if (hp > 0.0f && hp < g_bot.maxhp - 0.5f && g_bot.maxhp > 1.0f
            && !g_ps.role) {
            uint64_t a[2] = { SID_PLAYER, 0 };
            memcpy(&a[1], &g_bot.maxhp, 4);
            DcCall(0xcac2cb5a30fccf12ULL, 2, a);
        }
    }
    if (g_bot.cur >= g_bot.n) {
        if (g_bot.loops_left > 0) {
            g_bot.loops_left--;
            g_bot.cur = 0;
            g_bot.ph = 0;
            g_bot.cmd_ms = 0;
            Xlog("bot: LOOP %d/%d tms %llu",
                 g_bot.loops_total - g_bot.loops_left + 1,
                 g_bot.loops_total + 1, PT_MS);
        } else if (!g_bot.done_logged) {
            g_bot.done_logged = 1;
            Xlog("bot: DONE tms %llu", PT_MS);
        }
        return;
    }
    if (!g_bot.cmd_ms) {
        g_bot.cmd_ms = now ? now : 1;
        g_bot.ph = 0;
        g_bot.last_qpc = 0;
        Xlog("bot: #%d %s BEGIN tms %llu", g_bot.cur + 1,
             g_bot.ln[g_bot.cur].t, PT_MS);
    }
    char line[BOT_MAXLINE], cmd[24] = "", why[240] = "";
    snprintf(line, sizeof line, "%s", g_bot.ln[g_bot.cur].t);
    char *sp = strchr(line, ' ');
    if (sp) {
        *sp = 0;
        sp++;
    } else
        sp = line + strlen(line);
    snprintf(cmd, sizeof cmd, "%s", line);
    int r = BotStep(cmd, sp, why, sizeof why);
    if (r == 0)
        return;
    Xlog("bot: #%d %s %s%s%s tms %llu", g_bot.cur + 1,
         g_bot.ln[g_bot.cur].t, r > 0 ? "OK" : "FAIL", why[0] ? " " : "",
         why, PT_MS);
    BotNext();
}
#endif
