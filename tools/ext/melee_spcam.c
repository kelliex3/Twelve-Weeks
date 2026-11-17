// melee_spcam.c - SP's melee camera framing on the free-swing melee camera (melee sequences should feel different from regular
// gunplay, using the melee cameras). Included after meleefov.c in t2r-ext.c. Notes: re/melee-cam-sp-1027.md.
// SP, measured (re/sp-melee-1027/report.md, session2/report.md, sp-melee-tables.json; moves where "melee camera on" goes 0 -> 1):
//   - field of view 70 -> 75 at the press (min 72.4 in the swings of session 1, 70.3 in session 2): 75 / 70 in tangent space = x1.096;
//   - camera position relative to the player: from about (+0.55, +1.46..1.58, 1.5..1.95 behind) to about (-0.24..+0.83, +1.61, 0.86..1.29 behind):
//     closer by about 0.6 m and re-centred so both fighters are in frame side by side;
//   - per contact a camera shake: rms 10-18 mm, peak 52-87 mm of camera-offset jitter.
// What this does (render camera only, the game's camera / aim / shots are never touched; the same copy-the-camera hook as the hit kicks, gsh_filter):
//   pull-in   the camera moves along its line to us to SP's 1.22 m (SPM_CAM_BACK; never pushed out, never closer than 1.1 m to our root) and
//             to SP's 1.61 m above our root (SPM_CAM_UP; measured - t2r-ext.c; was a fixed 0.45 m pull at the engine camera's height);
//   re-centre the camera moves SPM_CENTRE (0.5) of the way sideways toward the midpoint of the two fighters (capped 0.35 m), so the enemy is not
//             parked at the screen edge;
//   fov       meleefov.c eases the held fov up by x SPM_FOV (SP's 75 / 70) while the weight is up;
//   shake     a positional hit shake (13 Hz, 0.35 s, peak 0.045 m x the hit class: fist 0.8, weapon 1.1, victim 1.87, capped 0.085 m) on every
//             mfk_add (attacker blow / victim jolt / whiff 0.3x), on top of the existing rotational kick;
//   blend     weight eases in over ~0.2 s (tau 70 ms) and out over ~0.25 s (tau 80 ms), SP blends in 150-200 ms.
// Fair to both: the same rules on both screens - the framing is on while OUR melee attack / melee camera runs (+0.45 s) or a melee blow hit us or
// landed from us (1.2 s), toward the nearest standing enemy within 4.5 m (an enemy already in melee reach: nothing is shown that the player could not
// see), no more than the pull-in and 0.35 m sideways, no rotation: neither player's view of the other is ever blocked (the camera only comes toward
// its own player, the midpoint stays between the two).
// Off in: any exec / ctxdown / intro / down / free cameras (they own the camera), paired moves, downed / dying, stun, no enemy in reach.
// Opt-out t2r-no-sp-melee-cam.txt next to the exe. Log "spmelee cam:".
// ======================================================================================================================================================
// SP melee camera spec, measured (SP per-frame logs melee-prev.log / melee-cur.log of the melee-only No Return run, player rows, 60 fps: 8883
// frames with is-current-camera-melee = 1 ("cmel"), 43 melee episodes; MP side: "camrec:" logs
// of two recordings). med (p10 .. p90). lat < 0 = right. "line" = the horizontal line player -> nearest enemy.
//   WHEN     cmel is 1 only while a melee move of the PLAYER runs (swing, its -hit follow-through, dodge, pair) and 1 frame after; between combos it is 0.
//            Hit by an NPC while it is 0: the melee camera does NOT come on (fov 70.5, camera world yaw 0.10 deg/frame med, p90 0.59).
//   RIG      target-relative, in the LINE frame: lens 0.80 m beside the line (0.76 .. 1.01 on the kept side) and 0.90 m behind the player (0.66 .. 1.15)
//            = 42-45 deg off straight-behind, 1.26 m from the root horizontally (1.10 .. 1.40; 1.27 / 1.24 / 1.27 for a pair 0.6-0.9 / 0.9-1.5 / 1.5-2.5 m
//            apart: no dependence). SIDE: the one the camera was on before the melee camera came on (15 of 15 episodes), 34 episodes right / 9 left, 0 side
//            changes in 39 of 43 episodes. In the BODY frame (camrec): lat -0.59 (-0.91 .. right side), back 1.00 (0.57 .. 1.23), cyaw +18.9 (.. +33.8); the
//            body faces +9 deg off the line (-6 .. +26), so the body-frame numbers swing with the swing clips while the lens holds still in the world.
//   HEIGHT   above the root: wind-up 1.67 (1.50 .. 1.79), follow-through 1.70 (1.57 .. 1.79), hit 1.68, dodge 1.52, no move 1.55 (cmel 0: 1.55). In: 1.52 ->
//            1.57 (20 frames) -> 1.63 (45); out: 1.69 -> 1.60 (10 frames) -> 1.52 (20).
//   AIM      between the fighters: the forward axis passes 0.23 m to the enemy's side of the player's root at 1.36 m height (1.27 .. 1.43) and 0.25 m to the
//            player's side of the enemy's root at 1.10 m; camera yaw = line direction +- 29 deg (25.6 .. 40.3); pitch -16.3 deg (-19.2 .. -9.1; wind-up
//            -15.2, follow-through -17.6; in: -8.6 -> -9.1 (20 f) -> -13.0 (45 f), out: -16.3 -> -10.7 in 15 frames); roll 0.0 (-1.3 .. +0.4).
//   SCREEN   (x, y in half-widths / half-heights from the centre, + = right / up, fov 75): player root -0.18, -1.82 (below the frame), player head -0.24,
//            +0.35; enemy root +0.13, -0.95; enemy head +0.16, +0.49. Pair 0.6-0.9 m: enemy head +0.12; 1.5-2.5 m: +0.29 (the rig does not move, the enemy does).
//   FOV      75.0 while cmel is 1 (70.1 otherwise): 71.8 -> 72.8 (5 f) -> 74.3 (10) -> 75.0 (15); back to 70.1 in 30-45 frames.
//   RATES    camera world yaw per frame med 0.23 deg, p90 1.0, p99 2.2, max 4.2; over 30 frames med 5, p90 18, p99 52-60 deg. On the 107 frames where the
//            BODY turned > 5 deg (med 8.2) the camera turned 0.5 deg (p90 1.8, max 2.9): the camera does not follow body turns. Distance at the start:
//            1.68 -> 1.45 (10 f) -> 1.32 (20) -> 1.28 (30). No cut.
//   MP (earlier build, the attacker): lat -0.53 (-0.84 .. -0.04, 10 of 112 frames LEFT), back 1.07, up 1.62 flat, dist 1.19, pitch -18.3, cyaw -0.3 (-14.5 ..
//            +19.2), axis 0.45 m off the own root at 1.27 m, own root at x -0.37 / head -0.53, +0.59 (SP -0.24, +0.35), fov 75.7 for the whole duel. B (never
//            swung): lat +0.83, back 0.80 (0.24 ..), cyaw -70.8, dist 1.15, fov 75.1. rec15 A: 495 of 869 frames on the LEFT.
// Additions, measured (same logs + the FXN native lines of the SP log):
//   HIT SHAKE   one additive camera animation per blow, camera-animation-enable(.., camera-shake-melee-hit-b, .., scale, ..): scale 1.5 fists, 1.0 blade /
//            blunt, 0.5 on a finisher's blows, 1.75 when the player is hit. Per unit scale: roll +1.10 deg at +4 frames then -1.74 at +10, pitch +0.23 then
//            -1.31 (down) at +10, both back at 0 by +20 frames; no fov, no yaw; the lens dips 19-36 mm at +6 frames (curve + table: melee_feel.c MFK32).
//   BETWEEN HITS the melee camera stays on through a chain: it goes off 333 ms after a light blow's contact (= the cancel point; 1.0 s after a 4th hit /
//            ender, n = 27) unless the next move has started (contact -> next move med 333 ms, p90 661, n = 60), and it holds fov 75.0 flat. Through the clip's
//            six blows: line pose -0.66 .. -1.03 beside / 0.83 .. 1.04 behind, distance 1.18 .. 1.36 m, height 1.64 .. 1.84 m, pitch -15.8 .. -20.9 deg, fov
//            75.0 on every frame. LEAVING (7 clean exits): fov 75.0 at +3 frames, 74.9 (+6), 74.7 (+9), 74.1 (+12), 73.1 (+15), 71.3 (+20), 70.6 (+25), 70.2
//            (+30) = 4 frames of nothing, then a smoothstep over ~24; height 1.80 to +9 frames, 1.75 (+12), 1.70 (+15), 1.62 (+20), 1.57 (+25), 1.54 (+30),
//            1.51 (+36); pitch -18.3 -> -17.0 (+15) -> -12 (+36). ENTERING (14): fov 70.1 -> 70.8 (+4) -> 72.7 (+8) -> 74.3 (+12) -> 75.0 (+20).
//   FINISHER (34 in the two logs: 17 free-standing, 7 disarm, 7 wall, 2 low cover, 1 ledge) an AUTHORED camera blended in from the melee camera without a
//            cut (roll > 1 deg in 32 of 34, up to 22 deg), is-current-camera-melee 0 from its first frame. Free-standing / disarm medians: fov 75.0 -> 74.8
//            (+10 f) -> 74.4 (+15) -> 74.1 (+20) -> 73.8 (+30) -> 73.7; height 1.72 -> 1.63 (+10) -> 1.50 (+15) -> 1.35-1.50 (+20) -> 1.40-1.53; pitch -16.5
//            -> -13 (+10) -> -10 (+15) -> -8 .. -12; distance to the root 1.24 -> 1.11 (+30) -> 1.0-1.2, to the pair's chest midpoint 1.78 -> 1.53 (+30)
//            [disarm 1.70 -> 1.33 -> 1.16 (+45)]; camera world yaw over the move med 34 deg (p90 92). Wall / low cover: fov 61.9-63.8 (already there in a wall
//            combo). The half-wall head smash of the clip (90.48 s): fov 75.0 -> 73.7 (+7 f) -> 69.6 (+13) -> 65.7 (+19) -> 63.5 (+25) -> 63.0 (+31), then 66-67
//            through the move; distance 1.29 -> 1.17 (+13) -> 1.05 (+19) -> 0.98 (+25); height 1.81 -> 1.51 -> 1.37 -> 1.34; pitch -18.0 -> -12.5 -> -6.4
//            -> -3.4; roll to -2.7 (later +15); own head at x -0.56 / y +0.82 of the half-frame at +25 f, the enemy's at +0.10 / +0.52. OUT: fov back at 70.1
//            10-26 frames after the last frame (the head smash: 67.3 -> 70.0 over its last 20 frames), distance settled 24-34 frames after it.
//   PLAYER AS VICTIM of a paired move (struggle / grab; n small): the lens 0.5-0.9 m from the root (med 0.86), 1.53 m up, fov 75, the hit shake at 1.75.
// What this changes for it (each with its opt-out, meleefov.c PS25_*): the duel framing is SP's RIG in the line frame (spm25_rig: the lens's world azimuth
// round our pivot is its own state - the kept side, SPM25_AZ off the line, SP's turn rates - so neither a body turn nor an engine-camera swing moves it;
// no re-centre in a duel), the look-at aims SPM25_AIM of the way from us to the partner (in angle) at SP's turn rate without the engine-heading gate, the
// height / fov follow our own melee activity, a fighter who has not swung keeps his camera (PS25_VICTIM), and the release blends on a kept target.
// The engine's own melee camera under force-melee-camera-target stays the BASE (its raw pose in the line frame is logged: "spmelee cam: ps25 rig ... engine
// base") - whether it alone already gives SP's rig in MP is NOT measured yet (its heading does not: the look-at's engine-heading gate left us 21.7 deg off
// the view centre with the partner at +2.8 in a recording, 'cam2: duel framing shown').
// ======================================================================================================================================================
#define SPM25_AZ 0.7505f        // rad = 43 deg off straight-behind the line (SP: atan(0.80 / 0.90) .. atan(0.88 / 0.88))
#define SPM25_SIDE_DEAD 0.15f   // m [TUNING ps25]: a lens within this of the line (or right of it) takes the RIGHT shoulder (SP 34 of 43), clearly left keeps left
#define SPM25_ORB_OMEGA 4.0f    // rad/s [TUNING ps25]: the rig azimuth's spring (SP start: 12 deg in 20 frames, 17 in 45)
#define SPM25_ORB_RATE 2.0f     // rad/s = 1.9 deg per 60 fps frame (SP: p99 52-60 deg over 30 frames)
#define SPM25_LOOK_OMEGA 8.0f   // rad/s [TUNING ps25]: the look-at spring inside the rig
#define SPM25_LOOK_RATE 2.3f    // rad/s = 2.2 deg per frame (SP p99 per frame; max 4.2 = both at their caps)
#define SPM25_DIST 1.26f        // m: the lens's horizontal distance from our root (SP 1.26, 1.10 .. 1.40; no dependence on the pair's distance)
#define SPM25_AIM 0.58f         // of the angle from us to the partner (SP screen x: player root -0.18, enemy root +0.13)
#define SPM25_UP_MOVE 1.69f     // m above our root while a melee move of ours runs / a blow just landed on us (SP 1.67 wind-up, 1.70 follow-through, 1.68 hit)
#define SPM25_UP_IDLE 1.55f     // otherwise (SP no move 1.55, dodge 1.52)
#define SPM25_UP_TAU_IN 0.40f   // s (SP: 1.52 -> 1.63 in 45 frames)
#define SPM25_UP_TAU_OUT 0.20f  // s (SP: 1.69 -> 1.52 in 20 frames)
#define SPM25_ACT_MS 450        // ms [TUNING ps25]: our melee move counts as running this long after its last tick (bridges contact -> the next swing of a combo, 333-367 ms on uc2 A; SP chains them inside one melee camera)
#define SPM25_ACT_FOLLOW_MS 1200        // ms: the engine's own attack state extends OUR duel swing by at most this (its -hit follow-through; SP's -hit clips 1.0-1.3 s)
#define SPM25_RIG_KEEP_MS 6000  // ms [TUNING ps25]: the rig lets go of a fighter who has not swung for this long (back to his own camera)
#define SPM25_FOV_TAU_OUT 0.25f // s (SP: 75 -> 70.1 in 30-45 frames)
#define SPM25_OUT_TAU 0.25f     // s: release of the framing + look-at (95 % in 0.75 s; SP 0.5-1.0 s)
#define SPM25_DIR_KEEP 0.60f    // m [TUNING ps25]: an engine lens closer than this to our pivot keeps the last lens direction (was 0.30)
// main thread (spm_tick) -> render thread: up = the lens height above our root, rc = the re-centre's weight (0 in a duel); render -> main: the rig's log values
static struct {
    volatile float up, rc, side, w, eb_r, eb_b, sh_r, sh_b, dx, dz;
    volatile int dok;
} g_s25 = {.up = SPM25_UP_IDLE,.rc = 1.0f };

// (g_s25_lock, t2r-ext.c next to cam2_orbit's declaration: spm_tick says whether the engine's duel camera lock may be taken - not for a fighter who has not swung)
static int MduSwinging(void);
static int MduInPair(void);
static int MduVicPairOn(void);       // melee_duel.c
static int SpmInExchange(void);       // melee_exchange.c: we are in an exchange (its synced hit pairs keep the framing: no pulse in / out per pair)
static int SpmOff(void)
{
    static int v;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (!at || now - at >= 2000) {
        at = now ? now : 1;
        FILE *f = ExeSideOpen("t2r-no-sp-melee-cam.txt", "r");
        int o = f != NULL;
        if (f)
            fclose(f);
        if (o != v)
            Xlog("spmelee cam: %s",
                 o ? "OFF (t2r-no-sp-melee-cam.txt)" : "on");
        v = o;
    }
    return v;
}

// a contact (mfk_add): the positional shake
static void SpmHit(float amp)
{
    if (!(g_spm.w > 0.15f))
        return;                 // only inside the melee framing (no shake on a stray hit at range)
    g_spm.kamp = amp > 1.9f ? 1.9f : amp;
    MemoryBarrier();
    g_spm.kt0 = GshNow();
    g_gshAny = 1;
}

static void SpmEvent(void)
{
    g_spm_ev_ms = GetTickCount64();
}

static float g_spmEy;          // the opponent's root height (spm_tick -> g_spm.ey)
// ---- v5-cam2 (melee_camcont.c cam2_*; the camera is one continuous camera with blends in between, as in SP):
// one duel camera from engage until after the duel tail. Here: the framing stays on through the duel's own synced pairs (no fade-out / back-in per blow),
// it frames the PARTNER (not the nearest enemy), the look-at follows its target on a critically damped spring (starts and stops without a velocity
// step), and on a VICTIM engage (the other person also gets a duel camera upon entering one, even as a victim, so it takes their
// focus on them) the look-at comes in at once and turns faster (g_cam2_boost_until), with an ORBIT around our head (cam2_orbit) when the attacker is off
// to the side / behind - an in-place turn cannot frame someone standing beside the camera. Opt-out t2r-duel-cam-v5.txt = the v5-cam camera (cam2_tick).
static volatile int g_cam2On = 1;
static volatile int g_ps23Piv = 1;     // ps23 (spm_tick, block comment at spm_slew): the framing pivots follow the roots; 0 = t2r-ps23-cam-pivot-old.txt
static volatile int g_cam2BoostOld;   // main thread (cam2_tick): t2r-cam-boost-old.txt - the old 600 deg/s boost caps
static volatile uint64_t g_cam2BoostUntil;    // main thread (cam2_tick): a victim engage - fast look-at + orbit until framed
static volatile float g_cam2Orb;       // render thread: the orbit applied this frame (rad), for the logs
// ---- the pinned victim's camera (getting pinned adjusts the camera automatically to a set position, as in SP, that shows the attacks clearly; the engine camera used to sit inside our own back / the wall for the whole pin). While a pin
// pair has us on a surface the render camera moves to a fixed side view of both fighters: 1.9 m out from their midpoint, square to the attacker -> victim
// line, a little toward the attacker (away from the surface), 1.35 m above our root, looking at the midpoint at chest height. The side is the one the game
// camera was on when the pin started; a blocked side (geometry ray) swaps, both blocked = the camera comes in to 90 % of the free distance. Blend in 0.25 s,
// out 0.35 s. Opt-out t2r-no-pin-cam.txt. Log "pincam:".
#define SPC_OUT 1.9f
#define SPC_BACK 2.3f           // behind the attacker
#define SPC_LAT 0.75f           // over its right shoulder (the game camera's side)
#define SPC_UP 1.6f
static uint64_t RayKick2(const float *a, const float *b);
static void SpmFwd(const float *q, float *f);
static void SpmRotx(const float *q, float a, float *o);
static float RayT(uint64_t job);
static int Spc5V4(void);
static void SpcTick5(float dt);
static void SpcApply5(float *pos, float *q);   // the v5 pin camera (below)
static void SpcTick(float dt)
{
    static int side, was;
    static uint64_t job[2];
    static float tt[2] = { 1, 1 }, ax, az, cmx, cmz, outs = SPC_OUT;
    static int sm;
    if (!Spc5V4()) {
        if (!g_pcam.v5) {
            g_pcam.w = 0.0f;
            g_pcam.v5 = 1;
            was = 0;
        }
        SpcTick5(dt);
        return;
    }                           // v5 (t2r-pincam-v4.txt = this one)
    if (g_pcam.v5) {
        g_pcam.w = 0.0f;
        g_pcam.ho = 0;
        g_pcam.v5 = 0;
    }
    int on = MpnPinnedNow() && MduPartnerSlot() >= 0 && !g_downed
        && MatchSubstate() == 6
        && GetFileAttributesA("t2r-no-pin-cam.txt") ==
        INVALID_FILE_ATTRIBUTES;
    float pb[16], ab[16];
    const float *pl = on ? PlayerLocator(pb) : NULL;
    uint64_t h = on ? PuppetHandle((uint32_t) MduPartnerSlot()) : 0;
    const float *al = h ? PuppetLocator(h, ab) : NULL;
    if (!pl || !al || !PsLocOk(pl) || !PsLocOk(al))
        on = 0;
    // (at 60 fps: the camera alternated frame by frame between the free spot and inside the container - a ray that had no answer
    //  yet counted as "clear". Now a hit holds at once and the clear distance only comes back at 1.5 m/s; answers outside 0..1 change nothing.)
    for (int k = 0; k < 2; k++)
        if (job[k]) {
            float t = RayT(job[k]);
            job[k] = 0;
            if (t >= 0.0f && t < 1.0f)
                tt[k] =
                    t < tt[k] ? t : tt[k] + fminf(t - tt[k],
                                                  dt * 1.5f / SPC_OUT);
            else if (t >= 1.0f)
                tt[k] = fminf(1.0f, tt[k] + dt * 1.5f / SPC_OUT);
        }
    if (on) {
        float ux = al[0] - pl[0], uz = al[2] - pl[2], ul =
            __builtin_sqrtf(ux * ux + uz * uz);
        if (ul < 0.2f) {
            ux = 0;
            uz = 1;
        } else {
            ux /= ul;
            uz /= ul;
        }
        float mx = 0.5f * (pl[0] + al[0]), mz =
            0.5f * (pl[2] + al[2]), ly = pl[1] + 1.05f;
        if (!was) {             // the side the game camera is on now
            float sx = g_pcam.px - mx, sz = g_pcam.pz - mz;
            side = (sx * -uz + sz * ux) >= 0.0f ? 1 : -1;
            sm = 0;
            ax = ux;
            az = uz;
            cmx = mx;
            cmz = mz;
            tt[0] = tt[1] = 1.0f;
            g_pcam.sgn = 0;
            Xlog("pincam: on - the attacker's over-the-shoulder view, attacker %.2f m", (double) ul);
        }
        {
            float k = 1.0f - __builtin_expf(-dt / 0.25f);
            ax += (ux - ax) * k;
            az += (uz - az) * k;
            cmx += (mx - cmx) * k;
            cmz += (mz - cmz) * k;
            float l = __builtin_sqrtf(ax * ax + az * az);
            if (l > 0.1f) {
                ax /= l;
                az /= l;
            }
        }
        float L[3] = { cmx, ly, cmz };
        // The victim camera was obnoxious; the attacker camera mirrored (to show from the right side instead of the left) is used: the attacker's over-the-shoulder view, mirrored - behind the attacker (u = us -> attacker), over its other shoulder, at its
        // camera height, looking at us. One ray from the attacker's head to that spot: geometry behind the attacker pulls the camera in (hit at once,
        // the free distance back at 1.5 m/s, as before). The side-view smoothing (ax / az / cmx / cmz) stays.
        (void) side;
        (void) L;
        // (the camera sat inside the attacker's head - the ray started there and hit its own body. Now the attacker's own camera
        //  pose: behind it, over its RIGHT shoulder like the game camera; the ray starts beside that shoulder, outside its capsule.)
        float rgx = az, rgz = -ax;      // the attacker's right (it faces -a; -az / ax put it over the left shoulder)
        // the camera moves slightly right/left depending on availability: the right shoulder (k 0) unless its line is blocked and
        // the left one (k 1) is clear; a 0.4 s hold before each swap, the lateral offset eases across (no cut)
        float A0[3] =
            { cmx + ax * 0.5f * ul, al[1], cmz + az * 0.5f * ul };
        static float latk = 1.0f;
        static int lside = 1, lhold;
        for (int k = 0; k < 2; k++) {
            float sg = k ? -1.0f : 1.0f, Hk[3] =
                { A0[0] + rgx * SPC_LAT * sg, al[1] + SPC_UP,
A0[2] + rgz * SPC_LAT * sg }, Ck[3] =
{ Hk[0] + ax * SPC_BACK, al[1] + SPC_UP, Hk[2] + az * SPC_BACK };
            job[k] = RayKick2(Hk, Ck);
        }
        if (!was) {
            lside = 1;
            latk = 1.0f;
            lhold = 0;
        }
        int want = lside;
        if (lside == 1 && tt[0] < 0.7f && tt[1] > tt[0] + 0.25f)
            want = -1;
        else if (lside == -1 && (tt[0] >= 0.95f || tt[0] > tt[1] + 0.25f))
            want = 1;
        if (want != lside) {
            if (++lhold > 24) {
                lside = want;
                lhold = 0;
                Xlog("pincam: camera to the %s shoulder (rays right %.2f left %.2f)", lside > 0 ? "right" : "left", (double) tt[0], (double) tt[1]);
            }
        } else
            lhold = 0;
        latk +=
            ((float) lside - latk) * (1.0f - __builtin_expf(-dt / 0.25f));
        int kk = lside > 0 ? 0 : 1;
        float tsel = tt[kk];
        // (the swap eased the offset through 0 = right behind the attacker's head - B's view went into its hair. Mid-swap the camera rises
        //  and backs off, over the attacker's head.)
        float mid = 1.0f - __builtin_fabsf(latk);
        if (mid < 0.0f)
            mid = 0.0f;
        float H[3] =
            { A0[0] + rgx * SPC_LAT * latk, al[1] + SPC_UP + 0.6f * mid,
       A0[2] + rgz * SPC_LAT * latk };
        float C[3] =
            { H[0] + ax * (SPC_BACK + 0.6f * mid), H[1],
       H[2] + az * (SPC_BACK + 0.6f * mid) };
        float out = tsel < 1.0f ? fmaxf(0.15f, tsel * 0.85f) : 1.0f;
        if (!was)
            outs = out;
        else
            outs =
                out <
                outs ? out : outs + (out - outs) * (1.0f -
                                                    __builtin_expf(-dt /
                                                                   0.3f));
        out = outs;
        g_pcam.cx = H[0] + (C[0] - H[0]) * out;
        g_pcam.cy = C[1];
        g_pcam.cz = H[2] + (C[2] - H[2]) * out;
        g_pcam.lx = pl[0] + ax * 0.2f;
        g_pcam.ly = pl[1] + 1.25f;
        g_pcam.lz = pl[2] + az * 0.2f;
    } else if (was)
        Xlog("pincam: off");
    was = on;
    float tgt = on ? 1.0f : 0.0f;
    g_pcam.w +=
        (tgt - g_pcam.w) * (1.0f -
                            __builtin_expf(-dt /
                                           (tgt >
                                            g_pcam.w ? 0.08f : 0.12f)));
    if (g_pcam.w < 0.002f)
        g_pcam.w = 0.0f;
    if (on || g_pcam.w > 0.0f)
        g_gshAny = 1;
}

// render thread: pos (3) + q (x y z w) of the render camera, moved / turned toward the pin camera by its weight
static void SpcApply(float *pos, float *q)
{
    // (at 60 fps: the old relative turn guessed the camera's forward axis from the CURRENT game camera every frame; pinned against a wall the
    //  game camera faced away and the guess flipped - B's view swung to the sky / into the attacker. The axis sign is taken once, on the pin camera's
    //  first frame (the game camera still looks at the fighters), and the orientation is built from the look direction: yaw, then pitch, no roll.)
    if (g_pcam.v5) {
        SpcApply5(pos, q);
        return;
    }
    float w = g_pcam.w;
    if (!(w > 0.001f))
        return;
    float s = w * w * (3.0f - 2.0f * w);
    float f[3];
    SpmFwd(q, f);
    if (!g_pcam.sgn)
        g_pcam.sgn =
            ((g_pcam.lx - pos[0]) * f[0] + (g_pcam.lz - pos[2]) * f[2]) <
            0.0f ? -1 : 1;
    pos[0] += (g_pcam.cx - pos[0]) * s;
    pos[1] += (g_pcam.cy - pos[1]) * s;
    pos[2] += (g_pcam.cz - pos[2]) * s;
    float sg = (float) g_pcam.sgn, tx = sg * (g_pcam.lx - pos[0]), ty =
        sg * (g_pcam.ly - pos[1]), tz = sg * (g_pcam.lz - pos[2]);
    float hd = __builtin_sqrtf(tx * tx + tz * tz);
    if (!(hd > 0.2f))
        return;
    float th = __builtin_atan2f(tx, tz), ph = __builtin_atan2f(ty, hd);
    float t[4] =
        { 0.0f, __builtin_sinf(th * 0.5f), 0.0f,
   __builtin_cosf(th * 0.5f) }, o[4], c[3];
    SpmRotx(t, ph, o);
    SpmFwd(o, c);
    if (__builtin_fabsf
        (c[1] - __builtin_sinf(ph) * (ty >= 0.0f ? 1.0f : 1.0f)) >
        __builtin_fabsf(c[1] + __builtin_sinf(ph)))
        SpmRotx(t, -ph, o);
    float d = o[0] * q[0] + o[1] * q[1] + o[2] * q[2] + o[3] * q[3];
    if (d < 0.0f)
        for (int i = 0; i < 4; i++)
            o[i] = -o[i];
    float r[4], l = 0.0f;
    for (int i = 0; i < 4; i++) {
        r[i] = q[i] + (o[i] - q[i]) * s;
        l += r[i] * r[i];
    }
    l = __builtin_sqrtf(l);
    if (l > 0.5f)
        for (int i = 0; i < 4; i++)
            q[i] = r[i] / l;
}

// ---- the v5 pin camera (v5 = the duel rules unchanged, visually like SP; remote-player clips a4371fab / 851ea9ca / ce052301 and the later log lpslog4). What was wrong with the camera above (spc_tick / spc_apply, kept behind t2r-pincam-v4.txt):
//   - its shoulder rays were ray_kick2 rays from bare points (t2r-ext.c world_ray_kick: "rays from points hit at t 0.06 everywhere"): every pin in lpslog4
//     answered right 0.10-0.12 / left 0.38-0.43, at three different walls - the lens sat at 15-36 % of its 2.3 m arm, 0.35-0.8 m behind the attacker's
//     shoulder, its back / backpack filling the frame;
//   - a ray without a hit answers -1 (ctxdown: "chest rays 0 hit (-1.00 ...)"), which the old code took as "no answer": a pull-in never came back in that pin;
//   - its blend moved the lens on a straight line from the game camera (the victim's own, behind it at the wall) to the pin spot on the far side: through both bodies;
//   - the victim's camera let go 700 ms after a pin pair (mpn_pinned_now): between two blows (hand-off 0.7-1.5 s, the next press up to 1.8 s later) it left and came back;
//   - only the victim had one: the pinner kept the raw game camera (the duel framing is off while psync plays the pin pairs);
//   - the escape / takedown camera (xcam) blended from the RAW game camera (icam_filter): a one-frame cut from the pin camera to the game camera, then 450 ms from there.
// v5, the same framing on both screens - SP's player-pins-NPC camera (SP clip, 47.5-51 s: behind the attacker over one shoulder, ~2 m off it,
// the attacker in one third of the frame, the victim and the wall in the middle):
//   spot    PC5_BACK behind the attacker along the pair line, PC5_LAT beside it, PC5_UP above its root, looking at 62 % of the way to the victim at chest height;
//   rays    locator rays (world_ray_kick_m, mask 0x1b07 = cars / props / bodies too, the attacker's own body ignored), one shoulder per frame, each read the next
//           frame: the arm (beside its head -> the lens + PC5_MARGIN), beside the lens (0.55 m out) and the line of sight (lens -> 85 % of the way to the look
//           point). No hit (-1) = clear; an engine error keeps the last answer;
//   side    the shoulder the game camera is on at the start; the other one when its score (arm + 0.35 beside + 0.6 sight) beats ours by 0.35 for 0.4 s (0.2 s in
//           the first 0.4 s); mid-swap the lens rises and backs off (never through the attacker's head);
//   pull-in a hit pulls the arm in with tau 60 ms (no one-frame pop), the free length comes back at 1.2 m/s; never inside a body: nearer than PC5_MIN_A /
//           PC5_MIN_V to the attacker's / victim's axis the lens rises instead (up to 0.9 m);
//   blend   (pincam2) no weight blend: a speed / acceleration limited follower on the render thread (spc_apply5) moves the shown camera from the
//           incoming one to the pin spot and back, orbiting the pair midpoint (radius >= PC5_ORBIT_MIN while the arc is long), <= PC5_STEP_DEG / PC5_STEP_M
//           per frame; a takedown / escape / down camera (xcam) blends from the framed view (g_pcam.ho) while the pin camera stays frozen under it.
// Mechanics untouched (render camera only). Opt-out t2r-pincam-v4.txt (the camera above), t2r-no-pin-cam.txt = no pin camera at all. Log "pincam5:".
// SP 1:1 (SP run PLAYER-FRAME numbers, melee-a/b.log P rows, tools/sp_pin_cam.py; the earlier numbers were WORLD-axis offsets read as player
// frame): exchange (melee camera on, 3433 frames) 0.75 right / 0.93 behind / 1.61 up, pitch -16; wall pin (402 frames) 0.75 right / 0.74 behind / 1.64 up,
// pitch -15: the pin starts from the exchange spot and only comes in 0.19 m - it does NOT swing wider. (Was 1.60 back / 0.90 beside / 1.60 up, closing 0.42 and widening x1.15; t2r-pincam-sp-old.txt only stops the close-in.)
#define PC5_BACK 0.93f
#define PC5_LAT 0.75f
#define PC5_UP 1.64f
#define PC5_PIV 0.50f           // the arm ray starts this far beside the attacker's head (outside its capsule and backpack)
#define PC5_MARGIN 0.30f        // the lens stays this far off a surface the arm ray hits
#define PC5_MIN_A 0.85f
#define PC5_MIN_V 1.05f
#define PC5_ORBIT_MIN 1.35f
#define PC5_IN_S 0.60f          // (log only since pincam2: the follower sets the real time)
// the render follower (spc_apply5, pincam2): SP never steps more than 5.6 deg / 0.10 m per frame (p99 1.6 deg)
#define PC5_VMAX 6.0f           // (the swing onto the pin side within the enter, ~0.5 s, as SP - was the slow drift)          // m/s (0.075 m per 48 fps frame)
#define PC5_AMAX 14.0f          // m/s^2
#define PC5_WMAX 6.0f           // rad/s = 183 deg/s (3.8 deg per 48 fps frame)
#define PC5_WACC 20.0f          // rad/s^2
#define PC5_OMEGA 6.0f          // 1/s: the tail of every approach (v <= omega x distance)
#define PC5_STEP_DEG 7.0f
#define PC5_STEP_M 0.14f
static int MpnCamRole(int *slot);     // melee_duel_pin.c
static struct {
    int on, role, slot, side, init, hold, swaps, ho, kside, n, arm_blk,
        lat_blk, los_blk, both_blk, blow0;
    uint64_t t_on, t_out, job[2][3], ign;
    float tight, latm, rt[2][3], alen[2], sax, say, saz, fx, fz, sd, latk,
        frac, raise, ho_w, min_a, min_v, min_frac, max_raise;
    const char *why;
    int v34, v34w, hi, pk_side, pk_ok;  // ps34: the spot is the SIDE two-shot (else over the attacker's shoulder); deciding (1 = the primary kind, 2 = the other); lens raised; the primary's best side
} g_pc5 = {.kside = -1,.tight = 1.0f,.latm = 1.0f };

// ======================================================================================================================================================
// ps34 THE VICTIM's PIN CAMERA and THE VICTIM's FINISHER CAMERA - one owner for both (this pin camera: spc_tick5 -> s34_apply), t2r-ext.c g_s34_tb.
// Goal: a cinematic, non-distracting view for the victim of a pin (the camera used to end up inside the player): somewhere over one of the attacker's
// shoulders, pointed at the victim. For a finisher done on the victim: a cut to a camera from the side that shows the victim, like the death screens in
// SP, that eases back to the normal downed camera.
// Before (uc3 B, the pinned side of a hand-played pin; rec13 B clip 89.5-90.4 / 96.4-96.6 frames: the opponent's face, then a torso, fill the frame):
//   23369 'pincam5: ON as the pinned victim ... over the attacker's left shoulder ..., follower in / out <= 7.0 deg / 0.14 m per frame', camrec 23376 'lat
//   +0.77 back +1.11 ... cyaw -28.5' -> 23406 'lat +1.33 back -0.96 ... cyaw -76.1' -> 23421 'lat +0.28 back -1.69 ... cyaw -130.9' -> 23451 'lat -1.29 back
//   -0.91 ... cyaw +136.6': the speed-limited follower swung the view ~180 deg round the pair THROUGH the first escape prompt (QTE open 23400, closed
//   23431) and never arrived - 'in -1.00 s (-1 = not reached)' on 4 of 6 victim pins of uc3 / rec13 / rec21, 1.35 s and 2.97 s on the other two, one with
//   'closest 0.23 m to the attacker ... 13 frames under 0.5 m'; the target itself moved (close-in over 0.5 s, shoulder swaps, the per-blow fov 75 -> 66
//   -> 64 -> 62). Then at the finisher 23454 'xcam victim: ctxdown:wall-throat-punch role 2: 2013 AUTHORED camera (... apReference-camera1 of the defender
//   clip ...)' = the attacker-side authored shot blended in over 450 ms (the earlier rule: the victim shows the attacker's authored shot), camrec
//   23466-23691 'cyaw +132 -> +97 -> +146 -> -163 -> -33': another 190 deg tour, then its 1 s hand-back orbit into the downed camera.
// SP reference (an SP choke struggle, clip 16-35-27 35.9-42.6 s = melee-cur.log 753.25-759.88 s; general numbers only - it is a choke, and the other
//   struggle of the run was bugged): an authored camera, lens 0.93-1.10 m from the enemy, 0.78-1.06 m from the pair's midpoint, 1.38 m above the player's
//   root (1.06-1.13 late), fov 73.7, both heads in the upper half (player x -0.04 .. -0.28 / y +0.53 .. +0.73, enemy x +0.12 .. +0.33 / y +0.41 .. +0.57),
//   in over 28-56 frames (distance 1.30 -> 0.81 at +28 f -> 0.64 at +56 f, fov 75 -> 74.2 -> 73.7), camera world yaw 20-30 deg over 5 s. No SP camera
//   exists for a player PINNED on a wall, and none for the player as a finisher's victim (SP's NPC side).
// The earlier down camera (downcam.c: SP's own player-death clip cameras, a hard cut on EVERY down incl. shot downs, 4-6 candidate cameras with
//   24-31 mm lenses ahead of / beside the body, 1 s blend back) was cut the same day in favour of a camera shake and a wobble down smoothly synced to the
//   torso (kept behind t2r-sp-down-cam.txt): it fired on every gunshot down in gunplay, its
//   tracks belong to SP's fall clips, not to our falls, and the shot changed per down. This one is for a finisher / takedown victim only (the player has
//   no control anyway), one fixed geometric shot.
// DESIGN, both on the same machinery (target on the main thread, rays through the engine's locator rays, a TIMED blend on the render thread):
//   PIN      over the attacker's shoulder, pointed at the victim:      wall |V|          lens S34_OS_BACK behind the attacker along the smoothed A -> V line,
//            (top view; A = attacker, V = victim = us, L = lens)            | |          S34_OS_LAT beside it (the shoulder is chosen ONCE, by rays, and
//                                                                           A            kept), PC5_UP + S34_OS_UP above its root; aim = the victim's root +
//                                                                         .              S34_OS_LOOK. Attacker and line are smoothed with tau 0.35 / 0.45 s
//                                                                       L                (pin-blow motion does not shake the frame); fov = the melee 75, no
//            per-blow zoom; SP's blow shake at the victim's scale on top (mfk_rot, re-applied over the shot). If both shoulders are blocked: the side
//            two-shot below at the pin's distance; if that is blocked too: the better shoulder, pulled in by its ray and raised S34_HI.
//   FINISHER side shot, the victim as the subject:                     V  .  .  .  L     lens beside the pair on the clear side, S34_VF_ANG toward the
//            (cd_cam_tracks role 2 -> s34_vic_take)                    A                 attacker's end off square, S34_VF_D0 + S34_VF_DK x pair distance out
//                                                                                        (slow push-in of S34_VF_PUSH over 3 s), S34_VF_UP above the
//            attacker's root; aim = our own chest joint, smoothed (the shot tilts down with the fall). In over S34_VF_IN from the start of the pair (the
//            victim has no control from that frame; the kill beat is not known for every pair and a second camera change inside the move is the thing to
//            avoid), held to the pair's end THROUGH our down, then S34_VF_OUT of ease into whatever the game shows - the downed camera with its wobble, or
//            the gameplay camera. No authored / enemy-side camera. A finisher that starts inside a pin keeps the pin's shot instead (one shot).
//   BLEND    s34_apply: from the camera shown at that moment to the target in orbit coordinates round the pair's midpoint (angle the short way, radius kept
//            >= PC5_ORBIT_MIN mid-way on a long arc: round the bodies, never through), smootherstep over the in time; then the target, low-passed (tau 80
//            ms); out the same way onto the live incoming camera. View axis +Z (ps25b). Nothing moves until the side is decided (<= 150 ms x 2).
//   CHECK    s34_check: once a second our head's and the attacker's head's screen position, the lens's distance to both body axes, the lens -> our head
//            ray; one warning per shot when a head is out of frame, the lens is within 0.35 m of a body axis or that ray is blocked for > 0.3 s.
// Stand-downs: the speed-limited follower (spc_apply5) and the over-shoulder target's close-in / swaps / per-blow fov for a victim; the victim's authored
// takedown camera (ctxdown.c cd_cam_tracks role 2). Unchanged: the pinner's screen (role 2), the escape pair's own SP camera (the hand-over freezes this
// shot under its blend-in), executions on a downed player and the 2013 / gun executions (their own xcam_start calls).
// t2r-ps34-cam-pinvic-old.txt = the old victim pin camera; t2r-ps34-cam-vicfin-old.txt = the authored victim camera for finishers. Log "pincam5: ps34".
// ======================================================================================================================================================
#define S34_OS_LAT 0.85f        // m beside the attacker [TUNING ps34] (was 0.75: the line lens -> victim's head passes the attacker's head axis at 0.26-0.39 m for a pair 0.5-1.0 m apart)
#define S34_OS_BACK 0.95f       // m behind the attacker (SP exchange 0.93; the SP choke's lens-enemy 0.93-1.10)
#define S34_OS_UP 0.06f         // m on top of PC5_UP (1.70 m: over the shoulder line) [TUNING ps34]
#define S34_OS_LOOK 1.35f       // m above the victim's root: the upper chest / face [TUNING ps34]
#define S34_HI 0.20f            // m: the raise of the last-resort shot [TUNING ps34]
#define S34_PIN_D0 0.85f        // m: the fallback two-shot's distance from the pair's midpoint = D0 + DK x pair distance (SP choke 0.78-1.06 at a pair 0.3-0.4 m apart)
#define S34_PIN_DK 0.45f        // [TUNING ps34]
#define S34_PIN_UP 1.40f        // m (SP choke 1.38)
#define S34_PIN_LOOK 1.32f      // m [TUNING ps34]
#define S34_VF_D0 1.25f         // m [TUNING ps34]: the finisher side shot (the whole falling body in frame)
#define S34_VF_DK 0.40f
#define S34_VF_UP 1.15f         // m: low-mid [TUNING ps34] (ps35: was 1.05)
#define S34_VF_ANG 0.20f        // rad off square toward the attacker's end [TUNING ps34]
#define S34_VF_PUSH 0.12f       // of the distance, over 3 s [TUNING ps34]
#define S34_PIV 0.40f           // m: the side spot's ray starts this far beside the pair's midpoint
#define S34_MIN_BODY 0.60f      // m: nearer to a body axis the lens rises (SP choke: lens 0.64-0.86 m from the player's root)
#define S34_ARM_MIN 0.75f       // of the arm: a spot whose ray is shorter is blocked [TUNING ps34]
#define S34_PIN_IN 0.50f        // s (SP choke: 28 frames to the bulk of the move, 56 to settle)
#define S34_PIN_OUT 0.50f
#define S34_VF_IN 0.30f         // s [TUNING ps34] ("a cut is acceptable": a fast ease can not land inside anything)
#define S34_VF_OUT 0.90f        // s: the ease into the downed camera [TUNING ps34]
static volatile int g_ps34Pin = 1, g_ps34_vf = 1;      // spc_tick5 re-reads the two opt-out files every 2 s
static volatile int g_ps37 = 1; // ps37 (spm_tick): the duel camera's kept state is dropped on an own-body teleport; 0 = t2r-ps37-cam-reset-old.txt
static volatile LONG g_s37Gen; // ps37: bumped by spm_tick when our root moved > 3 m in one tick (teleport / respawn) - every stage with state restarts
// ======================================================================================================================================================
// ps39 (the 30 fps review of the ps38 bot run: rec38a.mp4, ablogs-rec38a A / B, sheets rev38). One switch per item, a file next to the exe, re-read
// every 2 s (ps39_cfg); the evidence stands at each change.
//   PS39_WALL  t2r-ps39-cam-wall-old.txt   C1: the pin cameras (pinner + victim) stay on the open side of the pin's wall plane; a blocked arm of the
//                                          pinner's camera goes behind the attacker, not along the arm to its head.
//   PS39_VIC   t2r-ps39-cam-viccut-old.txt C2 / C3 / C4 / C8: the victim's shots - ONE cut in (finisher side shot always; pin shot when the arc is long or the
//                                          straight path is blocked), the finisher lens fitted once and held, the blend's way round kept, the way out round
//                                          our own body looking at it, a finisher on us takes the view from a takedown camera that still runs.
//   PS39_MF    t2r-ps39-cam-mf-rig.txt     C5: (file present = the ps35 rig push) every finisher with an authored track plays it on the attacker's screen;
//                                          the frame checks count a partner hidden behind our own body as not in frame.
//   PS39_SWAP  t2r-ps39-cam-swap-old.txt   C6: a duel lens pulled in under S39_SWAP_BELOW by a surface swaps to the free shoulder side, else rises.
//   PS39_LATCH t2r-ps39-cam-latch-old.txt  C7: a rig whose lens stands on the partner's side of us takes its spot on one cut; faster away from the partner's axis.
enum { PS39_WALL, PS39_VIC, PS39_MF, PS39_SWAP, PS39_LATCH, PS39_N };
static volatile int g_ps39[PS39_N] = { 1, 1, 1, 1, 1 };

#define S39_WALL_OFF 0.35f      // m [TUNING ps39]: the pin camera's target lens stays this far in front of the pin's wall plane
#define S39_WALL_REN 0.25f      // m [TUNING ps39]: the shown lens (render thread) on its way in / in the shot
#define S39_CUT_ARC 1.2217f     // rad = 70 deg [TUNING ps39]: a pin shot further round the pair than this from the shown camera comes in on a cut
#define S39_SWAP_BELOW 0.90f    // m: a collision pull-in below ~0.9 m
#define S39_FAR_RAD 1.7453f     // rad = 100 deg [TUNING ps39]: the rig's lens this far off its spot stands on the partner's side of us
static struct {
    volatile float nx, nz, px, pz;
    volatile int ok;
    int told;
} g_s39w;                       // the pin's wall plane: a point on it + the normal toward the room (main -> render)
static struct {
    float E[3];
    int e_ok;
    uint64_t pjob;
    float arc;
    int vf_wall, xstop;
} g_s39;                        // the finisher shot's held lens; the pin shot's path ray; a wall takedown is on us; a takedown camera to end at the side shot's cut
static volatile int g_s39Hold; // the victim's shot is being decided: the shown camera holds its pose (no fade to the game camera before the cut)
static volatile float g_s39B[3];
static volatile int g_s39Bok;  // our own chest (main -> render): the centre + look point of the shot's way out
static volatile LONG g_s39_flip, g_s39_cuts;    // main -> render: swap the rig's side; render -> main: rig cuts taken (logged by spm_tick)
static volatile float g_s39Rz; // main -> render: the duel lens's extra height while both sides are blocked
static volatile int g_s39Yt;   // main -> render: the look-at is wanted (in a duel, latched) - no rig cut while the framing releases
static int S39PinWall(int role, float *pt, float *n); // melee_camcont.c (after melee_duel_pin.c): 2 = we pin, 1 = we are pinned, 3 = the running pair's own wall frame
static void Ps39Cfg(uint64_t now)
{
    static uint64_t at;
    static int first = 1;
    static const char *const F[PS39_N] =
        { "t2r-ps39-cam-wall-old.txt", "t2r-ps39-cam-viccut-old.txt",
"t2r-ps39-cam-mf-rig.txt", "t2r-ps39-cam-swap-old.txt", "t2r-ps39-cam-latch-old.txt" };
    if (!first && now - at < 2000)
        return;
    at = now;
    for (int i = 0; i < PS39_N; i++) {
        int v = GetFileAttributesA(F[i]) == INVALID_FILE_ATTRIBUTES;
        if (v != g_ps39[i] || first)
            Xlog("spmelee cam: ps39 %s %s", F[i],
                 v ? "absent - the ps39 behaviour is ON" :
                 "present - the OLD (ps38) behaviour");
        g_ps39[i] = v;
    }
    first = 0;
}

// render thread: the lens kept in front of the pin's wall plane (only near the plane's anchor: the wall is not endless); 1 = moved
static int S39WallClamp(float *p)
{
    if (!g_s39w.ok || !g_ps39[PS39_WALL])
        return 0;
    float ox = p[0] - g_s39w.px, oz = p[2] - g_s39w.pz, d =
        ox * g_s39w.nx + oz * g_s39w.nz;
    if (!(d < S39_WALL_REN) || !(ox * ox + oz * oz < 16.0f))
        return 0;
    p[0] += g_s39w.nx * (S39_WALL_REN - d);
    p[2] += g_s39w.nz * (S39_WALL_REN - d);
    return 1;
}

// main thread: is `head` (the other body's) hidden from the lens `cp` by OUR body or geometry? The other body is ignored by the ray; the answer is last tick's.
typedef struct {
    uint64_t job;
    int blk;
} S39OccT;
static int S39Occ(S39OccT *o, const float *cp, const float *head,
                   uint64_t other_h)
{
    if (o->job) {
        g_rayErr = 0;
        float t = RayT(o->job);
        o->job = 0;
        if (!g_rayErr)
            o->blk = t >= 0.0f && t < 0.90f;
    }
    float dx = head[0] - cp[0], dy = head[1] - cp[1], dz =
        head[2] - cp[2], d = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
    if (d > 0.4f && d < 8.0f && other_h)
        o->job = WorldRayKickM(cp, head, other_h, 0x1b07);
    else
        o->blk = 0;
    return o->blk;
}

static struct {
    int vf, vf_slot, nlog;
    uint64_t vf_until, keep, vf_t0;     // the finisher shot: on, the attacker's slot, its end; a pin finisher's hold; its start
    volatile float tin, tout;   // the blend times of the running shot (render thread)
    float lk[3];
    int lk_ok;                  // our own chest, smoothed (the finisher shot's aim)
    uint64_t hr_job, chk_at, bad_at, hr_bad_at;
    int hr_blk, warned, n_dec0;
    float hr_len;
    float vfD, ahy;
    int was_vf;
    uint64_t dec_ms;            // ps35: the finisher shot's distance (fitted to both bodies, smoothed), the attacker's head height; the shot was a finisher shot; when it was decided
} g_s34 = {.tin = S34_PIN_IN,.tout = S34_PIN_OUT };

// the side spot (k 0 = the attacker's right): P = the ray's pivot beside the pair's midpoint, C = the lens
static void S34SideSpot(int k, float mid, float *P, float *C)
{
    float sg = k ? -1.0f : 1.0f, rx = -g_pc5.fz, rz = g_pc5.fx;
    float mx = g_pc5.sax + g_pc5.fx * 0.5f * g_pc5.sd, mz =
        g_pc5.saz + g_pc5.fz * 0.5f * g_pc5.sd, D, up, ang;
    if (g_s34.vf) {
        float x = (float) (GetTickCount64() - g_s34.vf_t0) / 3000.0f;
        x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
        D = S34_VF_D0 + S34_VF_DK * g_pc5.sd;
        D = D < 1.30f ? 1.30f : D > 1.90f ? 1.90f : D;
        D *= 1.0f - S34_VF_PUSH * x * x * (3.0f - 2.0f * x);
        if (g_s34.vfD > 1.0f)
            D = g_s34.vfD;      // ps35: the distance that fits BOTH bodies (spc_tick5), not the fixed 1.3-1.9 m with a push-in
        up = S34_VF_UP;
        ang = S34_VF_ANG;
    } else {
        D = S34_PIN_D0 + S34_PIN_DK * g_pc5.sd;
        D = D < 0.95f ? 0.95f : D > 1.35f ? 1.35f : D;
        up = S34_PIN_UP;
        ang = 0.30f;
    }
    float cs = __builtin_cosf(ang), sn = __builtin_sinf(ang);
    P[0] = mx + rx * sg * S34_PIV;
    P[1] = g_pc5.say + up - 0.05f;
    P[2] = mz + rz * sg * S34_PIV;
    C[0] = mx + rx * sg * D * cs - g_pc5.fx * D * sn;
    C[1] = g_pc5.say + up + 0.55f * mid;
    C[2] = mz + rz * sg * D * cs - g_pc5.fz * D * sn;
}

// v5-spcam (SP run 1031 wall pin, re/sp-melee-run-1031.md "Camera during the wall pin"): SP's camera offset goes (-0.32,1.62,-1.85) -> (+1.04,1.55,-0.77) over the
// enter - it swings to the side and CLOSES IN, horizontal 1.88 -> 1.29 m (x0.69), 1.55-1.62 m up throughout. The v5 spot sat at a fixed 1.6 back / 0.9 beside
// (1.84 m); now the back distance closes by PC5_CLOSE (0.42) and the side widens x1.15 (0.9 -> 1.04) over PC5_CLOSE_S (1.2 s, smoothstep) from the pin's start,
// ~1.41 m horizontal at the end. The rays, the body-clearance raise and the follower (spc_apply5) work on the moving spot unchanged. Opt-out t2r-pincam-sp-old.txt.
// The matching fov tightening (75 -> 66 -> 64 -> 62 per strike) is meleefov.c's pin block (t2r-pin-fov-old.txt). Logs "pincam5: SP close-in", "pinfov:".
#define PC5_CLOSE 0.20f         // 0.93 -> 0.74 m behind (SP wall pin median)
#define PC5_CLOSE_S 0.5f        // (the 1.2 s close-in shifted very slowly across the shoulder; SP closes in over the enter, impact +509 ms)
static int Spc5SpOld(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (v < 0 || now - at >= 2000) {
        at = now ? now : 1;
        int o =
            GetFileAttributesA("t2r-pincam-sp-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
        if (o != v)
            Xlog("pincam5: SP close-in %s",
                 o ? "OFF (t2r-pincam-sp-old.txt)" :
                 "on (1.6 -> 0.93 m back, 0.9 -> 1.04 m beside over 1.2 s)");
        v = o;
    }
    return v;
}

static int Spc5FovState(int *blows)
{                               // meleefov.c: the pin camera is engaged and shown (not handed over to a takedown camera)
    int on = g_pc5.on && g_pc5.role && g_pcam.want && !g_pc5.ho
        && !g_xcamOn;
    *blows = on && !g_s34Tb ? g_mfkBlows - g_pc5.blow0 : 0;   // (ps34: the victim's shot holds the 75-equivalent - no per-blow zoom steps)
    return on;
}

static int Spc5V4(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (v < 0 || now - at >= 2000) {
        at = now ? now : 1;
        int o =
            GetFileAttributesA("t2r-pincam-v4.txt") !=
            INVALID_FILE_ATTRIBUTES;
        if (o != v)
            Xlog("pincam5: %s",
                 o ? "OFF - the 10-28 pin camera (t2r-pincam-v4.txt)" :
                 "on (v5 pin camera, both screens; t2r-pincam-v4.txt = the 10-28 one)");
        v = o;
    }
    return v;
}

static void Pc5Drain(void)
{
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 3; i++)
            if (g_pc5.job[k][i]) {
                RayT(g_pc5.job[k][i]);
                g_pc5.job[k][i] = 0;
            }
    g_pc5.kside = -1;
}

// side k (0 = the attacker's right, 1 = its left): the arm ray's pivot beside its head, and the lens spot at lateral offset lat (signed, along its right)
static void Pc5Spot(int k, float lat, float mid, float *P, float *C)
{
    if (g_pc5.v34) {
        S34SideSpot(k, mid, P, C);
        return;
    }                           // ps34: the side two-shot (the victim's finisher shot / the pin's fallback)
    float sg = k ? -1.0f : 1.0f, rx = -g_pc5.fz, rz = g_pc5.fx, b = (g_s34Tb ? S34_OS_BACK : PC5_BACK * g_pc5.tight) + 0.5f * mid;     // the right of a body facing f = (-fz, fx) (the A/B sign)
    float up = PC5_UP + (g_s34Tb ? S34_OS_UP : 0.0f);  // (ps34: the victim's over-the-shoulder shot sits a little higher and wider, via latm)
    P[0] = g_pc5.sax + rx * sg * PC5_PIV;
    P[1] = g_pc5.say + up - 0.1f;
    P[2] = g_pc5.saz + rz * sg * PC5_PIV;
    C[0] = g_pc5.sax - g_pc5.fx * b + rx * lat;
    C[1] = g_pc5.say + up + 0.55f * mid;
    C[2] = g_pc5.saz - g_pc5.fz * b + rz * lat;
}

static void Pc5Kick(int k, const float *L)
{
    float sg = k ? -1.0f : 1.0f, P[3], C[3];
    Pc5Spot(k, sg * PC5_LAT * g_pc5.latm, 0.0f, P, C);
    float d[3] = { C[0] - P[0], C[1] - P[1], C[2] - P[2] }, l =
        __builtin_sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(l > 0.2f))
        return;
    float E[3] =
        { C[0] + d[0] / l * PC5_MARGIN, C[1] + d[1] / l * PC5_MARGIN,
   C[2] + d[2] / l * PC5_MARGIN };
    float B[3] =
        { C[0] - g_pc5.fz * sg * 0.55f, C[1],
   C[2] + g_pc5.fx * sg * 0.55f };
    float sf = g_pc5.v34 ? 0.60f : 0.85f;       // (ps34: the side spot looks AT the bodies - its sight ray stops short of them)
    float S[3] =
        { C[0] + (L[0] - C[0]) * sf, C[1] + (L[1] - C[1]) * sf,
   C[2] + (L[2] - C[2]) * sf };
    g_pc5.alen[k] = l;
    g_pc5.job[k][0] = WorldRayKickM(P, E, g_pc5.ign, 0x1b07);
    g_pc5.job[k][1] = WorldRayKickM(C, B, g_pc5.ign, 0x1b07);
    g_pc5.job[k][2] = WorldRayKickM(C, S, g_pc5.ign, 0x1b07);
}

static float Pc5Read(uint64_t *job, int *got)
{
    *got = 0;
    if (!*job)
        return 1.0f;
    g_rayErr = 0;
    float t = RayT(*job);
    *job = 0;
    if (g_rayErr)
        return 1.0f;
    *got = 1;
    return t >= 0.0f && t < 1.0f ? t : 1.0f;
}

static void Pc5Take(int k, float dt2)
{
    int g;
    float t = Pc5Read(&g_pc5.job[k][0], &g);
    if (g) {
        float l = g_pc5.alen[k] > 0.2f ? g_pc5.alen[k] : PC5_BACK, u =
            t >= 1.0f ? 1.0f : (t * (l + PC5_MARGIN) - PC5_MARGIN) / l;
        u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
        g_pc5.rt[k][0] =
            u < g_pc5.rt[k][0] ? u : fminf(u,
                                           g_pc5.rt[k][0] +
                                           dt2 * 1.2f / l);
    }
    for (int i = 1; i < 3; i++) {
        t = Pc5Read(&g_pc5.job[k][i], &g);
        if (!g)
            continue;
        float c = t >= 1.0f ? 1.0f : 0.0f;
        g_pc5.rt[k][i] +=
            (c - g_pc5.rt[k][i]) * (1.0f -
                                    __builtin_expf(-dt2 /
                                                   (c <
                                                    g_pc5.
                                                    rt[k][i] ? 0.06f :
                                                    0.25f)));
    }
}

static void Spc5Done(const char *why)
{
    Pc5Drain();
    Xlog("pincam5: SP close-in ended at back x%.2f / side x%.2f, %d blow(s) in the pin, lowest pin fov %.1f", (double) g_pc5.tight, (double) g_pc5.latm, g_mfkBlows - g_pc5.blow0, (double) g_pfMin);
    Xlog("pincam5: DONE as %s (slot %d) after %llu ms, let go: %s | target: %d frames, shoulder swaps %d, closest %.2f m to the attacker / %.2f m to the victim (horizontal, " "floors %.2f / %.2f), lifted up to %.2f m, arm ray blocked %d frames (pulled in to %.0f %%), beside the lens %d, line of sight %d, both shoulders blocked %d | " "render output, %d frames: closest %.2f m to the attacker / %.2f m to the victim, %d frames under 0.5 m, max turn %.1f deg/frame, max move %.3f m/frame (%s; limits %.1f deg / %.2f m), in %.2f s, out %.2f s (-1 = not reached), %d frame(s) held to the limit with the moving pair midpoint", g_pc5.role == 2 ? "the pinner" : !g_s34Tb ? "the pinned victim" : g_s34.was_vf ? "the VICTIM OF A TAKEDOWN (ps34 side shot)" : "the pinned victim (ps34 over-the-shoulder shot)", g_pc5.slot, (unsigned long long) (GetTickCount64() - g_pc5.t_on), why ? why : "-", g_pc5.n, g_pc5.swaps, (double) g_pc5.min_a, (double) g_pc5.min_v, (double) PC5_MIN_A, (double) PC5_MIN_V, (double) g_pc5.max_raise, g_pc5.arm_blk, (double) (g_pc5.min_frac * 100.0f), g_pc5.lat_blk, g_pc5.los_blk, g_pc5.both_blk, g_pcam.st_n, (double) g_pcam.st_min_a, (double) g_pcam.st_min_v, g_pcam.st_near, (double) g_pcam.st_max_deg, (double) g_pcam.st_max_mv, g_pcam.st_mv_w < 0.5f ? "on the way in" : g_pcam.st_mv_w < 1.5f ? "holding" : "on the way out", (double) PC5_STEP_DEG, (double) PC5_STEP_M, (double) g_pcam.st_in_s, (double) g_pcam.st_out_s, g_pcam.st_wcap);
    g_pc5.on = 0;
    g_pc5.why = NULL;
}

// ======================================================================================================================================================
// ps40 (the 30 fps review of the ps39 bot run rec39a; files next to the exe, re-read every 2 s by ps40_cfg; evidence at each change):
//   PS40_OVER   t2r-ps40-cam-over-old.txt    E1: a duel lens pulled in by a surface goes UP as it comes in (over our head, never into it); the other
//                                            side's ray no longer reads a wall right behind us as our own capsule.
//   PS40_OUT    t2r-ps40-cam-out-old.txt     E2: the victim shot's way out is ray-tested while the shot holds - blocked = ONE cut onto the downed camera.
//   PS40_SIDE   t2r-ps40-cam-side-old.txt    E3: the fallback rig of a wall / cover takedown stands side-on, the occlusion ray picks the side; an authored
//                                            takedown camera that hides the victim behind our body for 0.6 s trips the live guard.
//   PS40_ENGAGE t2r-ps40-cam-engage-old.txt  E6: the rig engages when THEIR swing registers on us (not only when a blow lands), unless the camera is being steered.
//   PS40_LAG    t2r-ps40-cam-lag-old.txt     E8: the rig follows a steadily turning line without the spring's lag (the lens stood straight behind us).
//   PS40_EXEC   t2r-ps40-cam-exec-old.txt    E5: the continuity stages pass the 2013 execution cameras through.
//   (t2r-ext.c: t2r-ps40-cam-downfade-old.txt - E2: while we lie downed only our own body is kept from the engine's near-camera hide.)
enum { PS40_OVER, PS40_OUT, PS40_SIDE, PS40_ENGAGE, PS40_LAG, PS40_EXEC,
        PS40_N };
static volatile int g_ps40[PS40_N] = { 1, 1, 1, 1, 1, 1 };

// ps41 step 0 (rec40a, world-camera measurement of the 27 open-fight cuts of the frame review): the duel rig is the lens's ONE owner while it is
// fully in (look-at weight and framing weight > 0.985). Measured: A 45.77 (11637 engage latch + 11638 'camcont: OUR FILTER position jump 0.21 m - absorbed'
// + 11639 'cam2: engine camera jump in duel framing 0.34 m - blended over 450 ms' = three owners on one engage), A 50.63 (11927 cam2 engine-jump blend),
// A 66.60 / 66.73 (12893 camcont absorb 0.18 m, paid back over 0.5 s = the second step), B 142.07-142.95 (19963 / 19984 / 19985 camcont absorbs). The
// other listed cuts carry no later-stage line: the rig's own azimuth walk on a line that a body step turned (world lens <= 0.14 m a frame).
// With the rig fully in: (1) its radius is its own (rate-limited, S41_R_*) instead of the incoming engine lens's distance - an engine pull-in / jump under
// the rig no longer reaches the screen; (2) ccg_filter absorbs nothing new (a running pay-back still ends), cam2_filter starts no engine-jump blend and
// rejects no flip. The lens guard (clg_filter) keeps shortening along the head -> lens line. t2r-ps41-cam-owner-old.txt = the ps40 chain.
#define S41_R_OUT 1.2f          // m/s the rig's radius grows [TUNING ps41]
#define S41_R_IN 3.0f           // m/s it shrinks [TUNING ps41]
static volatile int g_ps41o = 1;
#define S41_OUT_ARC 1.745f      // rad = 100 deg [TUNING ps41]: the victim shot's way out round our chest above which it lands on one cut (G4)
static float g_s41Oarc;
static struct {
    volatile LONG n;
    volatile float mx;
} g_s41r;                       // ps41 G1: the execution camera roll clamp (melee_camcont.c s41_roll)
static struct {
    volatile float R;
    volatile int rok;
    volatile double t;
    volatile LONG n_own, n_r, n_ccg, n_cam2;
} g_s41;
static double GshNow(void);
static int S41RigOwner(void)
{
    return g_ps41o && g_s41.rok && GshNow() - g_s41.t < 0.05;
}                               // render thread

static volatile int g_ps41 = 1; // ps41 (rec40a review; t2r-ps41-cam-old.txt = ps40): the rise over our head capped (G7), the rig cut only at engage (G5)
#define S40_AZ_CTX 1.3963f      // rad = 80 deg off straight-behind [TUNING ps40]: the wall / cover takedown's fallback rig (SP's wall takedown shots are side-on)
static volatile float g_s40Pq; // main -> render: extra pitch down (rad) of a lens raised over our head
static volatile uint64_t g_s40Boost;   // main -> render: until this tick the rig walks to its spot fast (the engage ease, ~0.35 s)
static volatile float g_s40In[3];
static volatile int g_s40InOk;        // render -> main: the camera under the victim's shot (what its way out lands on)
static struct {
    uint64_t j1, j2;
    int blk;
} g_s40o;                       // the way out's two path rays (shot -> mid-arc -> incoming camera)
static int S40Threat(void);    // melee_camcont.c (after melee_duel.c): the duel partner's swing is registered on us and not over
static int S40ExecCam(void)
{
    return g_ps40[PS40_EXEC] && g_xcamOn
        && strncmp(g_xcamName, "ctxdown", 7) != 0;
}                               // a 2013 execution camera runs

// ps42 (rec41a review, the ATTACKER's screen). Opt-outs, re-read every 2 s (file present = the ps41 behaviour):
//   PS42_MF    t2r-ps42-cam-mf-authored.txt   H1: a duel finisher (mf-) on the attacker's screen keeps the duel rig, swung SIDE-ON (S40_AZ_CTX, a two-shot
//              of both fighters) - not the clip's authored track (rec41a A 11136-11200 'exec cam: +6 .. +69 ... yaw 33 -> 142': the mf-ellie-2h-blunt
//              track swings round to the FRONT of our own fighter, clip A 53.73 / 90.57; B 18413-18637 mf-machete-slash-2 'camrec lat -0.75 back +0.47
//              dist 0.88': 0.9 m over our own shoulder, our face / backpack over the frame, 'both heads in frame on 61 %').
//   PS42_CTX   t2r-ps42-cam-ctx-authored.txt  H1: the same for a contextual wall / cover / weapon takedown started out of a duel (A 8890 wall-throat-punch:
//              own cap and arms fill the frame 15.90-16.00; B 19534 wpn-blade-wall-neck-chop: our backpack over the victim 153.9-155.2). Not the pin
//              escape pairs (pe-). The authored track still plays when the rig is not up on the victim.
//   PS42_HOLD  t2r-ps42-cam-relhold-old.txt   H1: the rig push does not end on the attacker's release frame - it holds S42_HOLD_MS more (the victim's fall
//              stays in the shot; B 19584 'hand-back from the release ... at tick 54 of 737', clip B 156.0-157.6 the fall off-screen), ends at once when
//              our body has moved S42_HOLD_M from the release spot.
//   PS42_ENG   t2r-ps42-cam-engage-old.txt    H4: melee_spcam.c spm25_rig - the engage cut (see there).
//   PS42_EXEC  t2r-ps42-cam-exec-old.txt      H2: t2r-ext.c - the execution camera (see there).
//   PS42_PIN   t2r-ps42-cam-pin-old.txt       H3: (see its use).
#define S42_HOLD_MS 1200        // ms [TUNING ps42]
#define S42_HOLD_M 0.45f        // m [TUNING ps42]
#define S42_DIST_SIDE 1.35f     // m [TUNING ps42]: the side-on two-shot's distance from our root (the push-in distances 1.09 / 0.98 are SP's over-the-shoulder values)
#define S42_ENG_RAD 1.0472f     // rad = 60 deg: an engage further off the rig's spot than this is ONE cut on the engage frame
enum { PS42_MF, PS42_CTX, PS42_HOLD, PS42_ENG, PS42_EXEC, PS42_PIN,
        PS42_N };
static volatile int g_ps42[PS42_N] = { 1, 1, 1, 1, 1, 1 };

static volatile int g_s42Side; // main -> render: the running rig push is the side-on two-shot
static volatile LONG g_s42_fcuts, g_s42_ecuts;  // render -> main: side-swap cuts of the side-on shot / engage cuts (logged by ps42_cfg)
static void Ps42Cfg(uint64_t now)
{
    static uint64_t at;
    static int first = 1;
    static LONG lf, le, lw;
    {
        LONG c = g_s42Wlim;
        if (c != lw && now - at >= 1900) {
            Xlog("xcam: ps42 - the blend-in weight of an exec / takedown camera was held to %.1f /s on %ld frame(s) so far (+%ld; a weight that jumped inside an eased blend-in is eased, not shown as a cut; last camera %s, frame %llu)", (double) S42_WRATE, (long) c, (long) (c - lw), g_xcamName, (unsigned long long) g_frame);
            lw = c;
        }
    }
    static const char *const F[PS42_N] =
        { "t2r-ps42-cam-mf-authored.txt", "t2r-ps42-cam-ctx-authored.txt",
"t2r-ps42-cam-relhold-old.txt", "t2r-ps42-cam-engage-old.txt", "t2r-ps42-cam-exec-old.txt",
"t2r-ps42-cam-pin-old.txt" };
    {
        LONG a = g_s42_fcuts, b = g_s42_ecuts;
        if (a != lf) {
            lf = a;
            Xlog("spmelee cam: ps42 side-on finisher shot - the victim was hidden from the kept side: ONE cut to the other side (%ld so far, frame %llu)", (long) a, (unsigned long long) g_frame);
        }
        if (b != le) {
            le = b;
            Xlog("spmelee cam: ps42 engage cut - the lens stood more than %.0f deg off the rig's spot (or on the partner's side of our body) when the duel engaged: the rig's spot on ONE cut (%ld so far, frame %llu)", (double) (S42_ENG_RAD * 57.29578f), (long) b, (unsigned long long) g_frame);
        }
    }
    if (!first && now - at < 2000)
        return;
    at = now;
    for (int i = 0; i < PS42_N; i++) {
        int v = GetFileAttributesA(F[i]) == INVALID_FILE_ATTRIBUTES;
        if (v != g_ps42[i] || first)
            Xlog("spmelee cam: ps42 %s %s", F[i],
                 v ? "absent - the ps42 behaviour is ON" :
                 "present - the OLD (ps41) behaviour");
        g_ps42[i] = v;
    }
    g_ps42x = g_ps42[PS42_EXEC];
    first = 0;
}

static void Ps40Cfg(uint64_t now)
{
    static uint64_t at;
    static int first = 1;
    static const char *const F[PS40_N] =
        { "t2r-ps40-cam-over-old.txt", "t2r-ps40-cam-out-old.txt",
"t2r-ps40-cam-side-old.txt", "t2r-ps40-cam-engage-old.txt", "t2r-ps40-cam-lag-old.txt",
"t2r-ps40-cam-exec-old.txt" };
    if (!first && now - at < 2000)
        return;
    at = now;
    for (int i = 0; i < PS40_N; i++) {
        int v = GetFileAttributesA(F[i]) == INVALID_FILE_ATTRIBUTES;
        if (v != g_ps40[i] || first)
            Xlog("spmelee cam: ps40 %s %s", F[i],
                 v ? "absent - the ps40 behaviour is ON" :
                 "present - the OLD (ps39) behaviour");
        g_ps40[i] = v;
    }
    {
        int v41 =
            GetFileAttributesA("t2r-ps41-cam-old.txt") ==
            INVALID_FILE_ATTRIBUTES;
        if (v41 != g_ps41 || first)
            Xlog("spmelee cam: ps41 t2r-ps41-cam-old.txt %s",
                 v41 ?
                 "absent - rise over the head capped at 0.30 m, rig cut only within 0.5 s of the rig's start: ON"
                 : "present - the ps40 behaviour");
        g_ps41 = v41;
    }
    {
        int v =
            GetFileAttributesA("t2r-ps41-cam-owner-old.txt") ==
            INVALID_FILE_ATTRIBUTES;
        if (v != g_ps41o || first)
            Xlog("spmelee cam: ps41 t2r-ps41-cam-owner-old.txt %s",
                 v ?
                 "absent - the duel rig is the lens's single owner while fully in (own radius; no continuity absorb, no cam2 engine-jump blend / flip reject): ON"
                 : "present - the ps40 stage chain");
        g_ps41o = v;
    }
    {
        static LONG lo;
        static uint64_t lt;
        LONG n = g_s41.n_own;
        if (n != lo && now - lt > 10000) {
            lt = now;
            lo = n;
            Xlog("spmelee cam: ps41 single owner - the rig owned the lens on %ld frame(s) so far (radius held against the incoming lens on %ld; camcont absorb skipped on %ld, cam2 engine-jump blend / flip reject skipped on %ld; frame %llu)", (long) n, (long) g_s41.n_r, (long) g_s41.n_ccg, (long) g_s41.n_cam2, (unsigned long long) g_frame);
        }
    }
    first = 0;
}

// ps34, main thread (ctxdown.c cd_cam_tracks, role 2 = a takedown / finisher plays ON us): 1 = this camera keeps / takes the victim's view (no xcam_start)
// ======================================================================================================================================================
// ps43 (rec42a review) - ONE pair-frame two-shot, planned once per move from where the pair WILL stand, shared by the victim's finisher side shot
// (M1), the attacker's finisher / wall / cover takedown shot (M2 / M3) and the attacker's execution shot (M5).
//   rec42a B 18.43-19.53 (wall-throat-punch on us): 11485 'ps34 shot decided after 78 ms ... LEFT side', 'lens is fixed at 103.57 0.16 284.27'; 11507 'psync
//   place ... att slot 1 ... world 103.75 -0.96 283.88' = 78 ms in the pair still stood where the duel left it (attacker 105.11 283.64 at 11460); the wall
//   takedown then places the attacker 1.4 m off the wall straight behind the victim - 0.43 m from the fixed lens: coat / strap / backpack for 22 frames,
//   11552 the ps41 late cut. The side was chosen from the roots of the first ticks; the clip's own placement was never asked.
//   rec42a A 57.07-58.1 / 93.63-95.07, B 134.74-137.08 / 183.18-186.37 (open finishers on the rig): '80 deg off straight-behind' is measured from OUR root
//   and facing - a body that turns 150 deg in the clip takes the lens in front of itself; the rig's springs do not arrive inside a 1 s move.
// The plan (main thread): S43_NS placements of both halves over the finisher clip (paired.c psync_pair_end on the running pair's ap + its last segment's
// clips; the live roots when the clips cannot be read), the pair's mean midpoint M and line f (attacker -> victim); S43_NC lens spots on a circle round M
// (distance fitted to the bodies' travel), one geometry ray each (kicked, read next tick); a spot is dropped when its ray is blocked, when it stands under
// S43_WALL in front of a wall takedown's plane, or when it comes within S43_MIN_AX of a body's axis at any sample; the rest are scored: side-on to the
// pair's line, the other body's capsule (S43_CAP, backpack included) not between the lens and the victim (attacker's screen: nor the victim between the
// lens and our head), the victim's front toward the lens, (attacker's screen) near the shown lens. No clear spot: the nearest blocked one pulled in and
// raised; none at all: the old camera. Logs "paircam:". Opt-outs (re-read every 2 s; file present = the ps42 behaviour):
//   PS43_VIC   t2r-ps43-cam-vic-old.txt     the victim's finisher side shot takes the planned lens (spc_tick5)
//   PS43_ATT   t2r-ps43-cam-att-old.txt     the attacker's finisher / contextual takedown: the planned two-shot as the chain's last stage (s43_filter)
//   PS43_EXEC  t2r-ps43-cam-exec-old.txt    the attacker's execution: the same shot instead of the 2013 authored track
//   PS43_CLIP  t2r-ps43-cam-noclip.txt      the plan uses the live roots only (no clip sampling)
//   PS43_SWAP  t2r-ps43-cam-swap-old.txt    M4 (rec42a B 150.467 engage cut, 150.900 side swap; A 2.30-2.43 two cuts in 0.13 s; B 88.633): the duel rig
//              takes no side swap within S43_SWAP_MS of an engage cut - one cut per duel start
#define S43_SWAP_MS 1500        // ms
enum { PS43_VIC, PS43_ATT, PS43_EXEC, PS43_CLIP, PS43_SWAP, PS43_N };
static volatile int g_ps43[PS43_N] = { 1, 1, 1, 1, 1 };

static volatile uint64_t g_s43Ecut;    // render -> main: GetTickCount64 of the rig's last engage cut (spm25_rig)
static int S43SwapWait(uint64_t now)
{                               // spm_tick, where the rig's side swap is decided: 1 = not now
    uint64_t c = g_s43Ecut;
    static uint64_t told;
    static int nl;
    if (!g_ps43[PS43_SWAP] || !c || now - c >= S43_SWAP_MS)
        return 0;
    if (told != c) {
        told = c;
        if (LOGCAP(nl, 120))
            Xlog("paircam: ps43 - a rig side swap is asked %llu ms after the engage cut: withheld until %d ms after it (one cut per duel start; t2r-ps43-cam-swap-old.txt = the swap at once; frame %llu)", (unsigned long long) (now - c), S43_SWAP_MS, (unsigned long long) g_frame);
    }
    return 1;
}

static void Ps43Cfg(uint64_t now)
{
    static uint64_t at;
    static int first = 1;
    static const char *const F[PS43_N] =
        { "t2r-ps43-cam-vic-old.txt", "t2r-ps43-cam-att-old.txt",
"t2r-ps43-cam-exec-old.txt", "t2r-ps43-cam-noclip.txt", "t2r-ps43-cam-swap-old.txt" };
    if (!first && now - at < 2000)
        return;
    at = now;
    for (int i = 0; i < PS43_N; i++) {
        int v = GetFileAttributesA(F[i]) == INVALID_FILE_ATTRIBUTES;
        if (v != g_ps43[i] || first)
            Xlog("paircam: ps43 %s %s", F[i],
                 v ? "absent - the ps43 behaviour is ON" :
                 "present - the OLD (ps42) behaviour");
        g_ps43[i] = v;
    }
    first = 0;
}

// ======================================================================================================================================================
// ps44 (rec43a review) - the edge cases left by ps43. Opt-outs, re-read every 2 s (file present = the ps43 behaviour). Logs "paircam: ps44".
//   PS44_TP    t2r-ps44-cam-tp-old.txt     R1 (A 9345 / 20070 'the two-shot ends after 516 / 265 ms (our body was teleported ...): a cut onto the camera under
//              it', one line after 'ps42 side-on finisher shot - ... ONE cut to the other side'): s43_att_tick read g_s37_gen as 'teleport' - that counter
//              is also bumped by spm25_rig's own cuts (the ps42 side swap, the engage cut). The two-shot now ends on a teleport only when our root moved
//              > 3 m in a tick (s43q_track's watch) or the pair itself ended.
//   PS44_UNDER t2r-ps44-cam-under-old.txt  R3: a rig push that stands under a requested two-shot does nothing - no side-on swing, no side-swap cut, no
//              push-in (g_s32.p held at 0): the ordinary duel rig runs underneath, unseen. The ps42 side-on push comes back when the plan finds no spot.
//   PS44_EXIT  t2r-ps44-cam-exit-old.txt   R4 (A 10023-10088: two-shot ease-out onto the execution camera's blend-out, then the behind hold, then the
//              engine camera = a 250 deg swing past our own face): every two-shot hands DIRECTLY to the behind-the-body lens (s41b) - one ease of
//              S44X_LEN along the short arc round our head, or ONE cut when that arc is over S44X_ARC, would pass in front of our body or within 0.6 m of a
//              body. The behind lens then follows its own release rules (not before its way in is over). Nothing else is shown in between.
//   PS44_EXEC  t2r-ps44-cam-exec-old.txt   R2 (B 20019 / A 14134 '1 live-root sample(s)' = the execution's plan knew only the first tick's roots; the clip
//              then walks the attacker round: own back over the victim): an execution's plan samples its clips' own placement over S44_TX; and any
//              two-shot whose victim's head + chest are hidden behind our own body for S44_HID_MS re-plans ONCE from the live roots (the spots within
//              50 deg of the shown one are out) and takes the new spot on one cut.
//   PS44_AIM   t2r-ps44-cam-aim-old.txt    R9 (B 19724 'the victim's 66 % ... FRAMING FAULT': the aim followed the ROOTS' midpoint, the victim's body fell
//              away from its root): the two-shot looks at the midpoint of our chest and the victim's chest / pelvis (live joints).
#define S44X_LEN 0.50f          // s [TUNING ps44] (ps43's S43A_OUT)
#define S44X_ARC 2.0944f        // rad = 120 deg
#define S44_HID_MS 300          // ms
#define S44_HID_R 0.30f         // m [TUNING ps44]: our torso's radius for 'the victim is behind our own body' (ps41's S41V_CAP_R)
//   PS44_HEAD  t2r-ps44-cam-head-old.txt   R7 (B 11306 'the lens cut in to 1.49 m', 11338 'ps40 over the head - ... allowed 0.74 m ... rises 0.10 m', 11365
//              'LENS WITHIN 0.45 m OF OUR HEAD ... lens-head 0.36 m'; clip B 11.97-12.27 inside our own cap): every floor was measured from the ROOT + a fixed
//              height, the live head leans back toward the lens in hit reactions. Hard floor: the duel lens is never closer than S44H_MIN to our live head
//              joint (s44h_filter, after the lens guard: the lens goes up and over the head, re-aimed at the point it looked at); when the kept side is
//              pulled in by a surface, the lens is within the floor and the other side's ray is clear, the side swap is ONE cut (spm25_rig, g_s39_flip 2).
#define S44H_MIN 0.75f          // m
//   PS44_VOUT  t2r-ps44-cam-vout-old.txt   R5b (B 16047 'the finisher side shot's way out to the downed camera is BLOCKED ...: it lands on ONE CUT', 16048 'LENS
//              STEP ... lens-head 0.36 m'; B 11659 / 11660 'lens-head 0.48 m'; clip B 90.70-90.97 into our own beanie, 17.07-17.6 the attacker's face / coat,
//              196.33 the back of our cap): the cut / ease landed on the downed camera's RAW spot while it was still arriving. The side shot is now held
//              past the pair's end until the camera under it has settled (its lens >= S44O_HEAD from our live head, our chest within S44O_FRAME of its
//              axis, not within 0.40 m of the attacker's torso, moved < 5 cm a tick for 3 ticks); not settled within S44O_PB the held shot pulls back
//              (S44O_BACK) and looks at our own chest until it does (at most S44O_MAX). Then the ordinary way out (one cut or the ease).
#define S44O_HEAD 0.80f         // m
#define S44O_FRAME 0.5236f      // rad = 30 deg off the under-camera's axis [TUNING ps44]
#define S44O_PB 1500            // ms
#define S44O_MAX 4000           // ms [TUNING ps44]: the hold never runs longer
#define S44O_BACK 0.60f         // m [TUNING ps44]
//   PS44_ENG   t2r-ps44-cam-eng-old.txt    R6 (A 14820 'ps42 engage latch ... the rig latches now', 'ps25 frame check ... partner's head x -1.08 ... OUT OF
//              FRAME', camrec 14823-14838 'lat +1.52 back +0.63' -> '+1.04 / +0.70' = the rig walked ~40 deg round in 0.25 s with no 'ps39 rig cut' /
//              'ps43 engage cut destination' line, while B at the same start (16915-16917) cut once: the swinger's own screen (A here) starts the rig on a
//              lens under S42_ENG_RAD off its spot and EASES it there - the opponent out of frame until the hit, the lens stepping round): at the engage the
//              rig takes its planned spot on ONE cut when the incoming lens stands more than S44E_RAD off it or the partner's head is out of frame.
#define S44E_RAD 0.6981f        // rad = 40 deg [TUNING ps44] (ps42: 60 deg)
//   PS44_FOV   t2r-ps44-cam-fov-old.txt    R10 (the victim's finisher side shot was too zoomed out; the target is cinematic: narrow fov, both
//              characters filling most of the screen): while that shot is shown the fov is fitted (spc_tick5, every tick from the shown lens) so the span from
//              the attacker's head + S44F_HEAD down to the lower of our chest / the attacker's pelvis - S44F_FOOT fills S44F_FILL of the frame height and the
//              pair + 0.55 m each side fits 85 % of its width, clamped to S44F_VMIN..S44F_VMAX vertical; meleefov.c's pin fov eases to it (tau 0.35 s) and back
//              out with the shot (no pop). The lens is not moved (its distance fit / occlusion tests are unchanged).
//   PS44_ATTFIN t2r-ps44-cam-attfin-old.txt R11 (the victim finisher camera must not apply to the attacker: it should stay looking
//              like a finishing blow on an NPC in SP): a plain duel finisher (ctxdown:mf-*) on the ATTACKER's screen gets no ps42 side-on
//              swing and no ps43 pair-frame two-shot - SP's player finisher: the clip's own authored track (ps39 path), else the behind-the-shoulder rig with
//              SP's finisher push. Contextual takedowns (wall / cover) keep the two-shot; the victim keeps its side shot.
#define S44F_FILL 0.78f         // share of the frame height [TUNING ps44] (most of the screen, 70-80 %)
#define S44F_HEAD 0.22f         // m head room over the attacker's head [TUNING ps44]
#define S44F_FOOT 0.25f         // m under the lower body point (the contact stays in) [TUNING ps44]
#define S44F_VMIN 33.0f         // deg vertical [TUNING ps44]
#define S44F_VMAX 42.0f         // deg vertical [TUNING ps44]
//   PS44_ATTEXEC t2r-ps44-cam-attexec-old.txt R12 (rec43a 01:12-01:16 pipe-execute: the victim got the 2013 victim camera, the attacker a camera
//              behind him; B 14963 'camera choice ... -> camera 5', 14964 'paircam: ps43 pipe-execute on the attacker's screen - the pair-frame two-shot ...
//              held until the execution camera starts its way out', 15197 'ends after 3875 ms', 15202 'behindcam: ps41 - hand-back from pipe-execute'): the
//              ps43 two-shot sat over the chosen 2013 track for the whole execution, then the behind hold. An execution on the attacker's screen whose
//              2013 choice kept an authored track plays that track (s43_att_tick waits for the choice; the two-shot only for the procedural fallback) and its
//              hand-back gets no behind-the-body hold (s41b_tick); the track's own blend-out hands back.
enum { PS44_TP, PS44_UNDER, PS44_EXIT, PS44_EXEC, PS44_AIM, PS44_HEAD,
        PS44_VOUT, PS44_ENG, PS44_FOV, PS44_ATTFIN, PS44_ATTEXEC, PS44_N };
static volatile int g_ps44[PS44_N] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

static struct {
    volatile float want, fill, vdeg;
    volatile int ok;
} g_s44f;                       // main: the victim side shot's cinematic fov (hfov deg, meleefov.c's units)
static float S44fWant(void)
{
    return g_ps44[PS44_FOV] && g_s44f.ok && g_s34.vf
        && g_s44f.want > 20.0f ? g_s44f.want : 0.0f;
}

static volatile LONG g_s44eCuts;
static volatile float g_s44eDeg;
static volatile int g_s44eOof; // render -> main (ps44_cfg logs): engage cuts that ps42's 60 deg rule would have eased
static volatile float g_s44oQ[4];      // render -> main: the rotation of the camera under the victim's shot (s34_apply, with g_s40_in)
static struct {
    uint64_t vt0, t0, tpb;
    int st, ok_n;
    float lp[3];
} g_s44o;                       // st 0 idle, 1 held (waiting for the downed camera), 2 held + pulled back, 3 done
static struct {
    volatile float H[3];
    volatile double t;
    volatile int ok;
    volatile LONG n;
} g_s44h;                       // main -> render: our live head joint in a duel; render -> main: frames floored
static volatile int g_s44Under;        // main: the running rig push stands under a requested two-shot
static uint64_t g_s44Pt0;      // main: the push's ramp restarts here when the two-shot was not obtained
static struct {
    volatile int req, off;
    uint64_t oh;
    char name[64];
} g_s44x;                       // the two-shot's exit: req 1 = s43_att_tick asks s41b_tick for the behind-the-body lens (-1 = refused); off = taken, s43_filter stands down at once
static float g_s44Ban[2];
static int g_s44BanOn;        // a re-plan: the direction (from the pair's midpoint) of the spot that failed
//   R13 t2r-ps44-cam-vicmove-old.txt (the victim's finisher side shot must not be a cut - like SP's finisher camera it swings to the side): the
//              side shot comes in by ONE smooth move along the arc round the pair (s34_apply's timed orbit, smootherstep, S44M_T0..S44M_T1 by the arc's size),
//              its path ray-tested in S44M_NR legs (blocked, or within 0.6 m of a body = ONE cut, logged); and it starts BEFORE the contact once the blow can no
//              longer fail on our screen: the partner's weapon finisher windup whose dodge window on us has closed (melee_camcont.c s44m_vic_commit) - not
//              confirmed by the pair's start within S44M_WAIT ms of the contact = the shot eases out again. Fists (punch rule: no finisher row in the
//              SWING) start at the pair's start (the contact) as before, with the move. The cinematic fov (R10) starts with the move. File present = ps43 (cut).
#define S44M_T0 0.35f           // s [TUNING ps44]: the move for a short arc
#define S44M_T1 0.50f           // s [TUNING ps44]: for a half circle
#define S44M_NR 6               // legs of the arc's path test
#define S44M_WAIT 450           // ms after the expected contact [TUNING ps44]
static int g_ps44m = 1;
//   R5a late / R5c t2r-ps44-cam-vrep-old.txt (B 11426 'pincam5: ps41 - the fixed side shot lost us for > 0.3 s (the lens -> our head ray is BLOCKED ...): ONE cut
//              to the attacker's LEFT side' = the old side flip, whose spot put the attacker's back over our torso until ~15.0; B 131.2-131.67 a foreground crate
//              hides our fall while the fixed lens holds): during the hold the lens -> our live CHEST ray is tested every tick too (stopping S44V_SHORT short;
//              blocked for S44_HID_MS = lost, like the head ray / the attacker's torso), and the one late correction RE-PLANS from the live roots with the
//              failed spot's direction banned (the s43 plan, as R2's re-plan on the attacker's screen) and cuts once to that spot. No other spot = the old
//              side flip when that side is clear, else the shot stays.
#define S44V_SHORT 0.45f        // m short of the chest [TUNING ps44] (our own arms / the attacker's hands at the contact)
static int g_ps44vr = 1;
static struct {
    int st, cblk, ncb;
    uint64_t t0, t_req, cjob, cblk_at;
    float E0[3];
} g_s44vr;                      // st 0 idle, 1 re-planning, 2 re-planned (this shot: t0 = g_pc5.t_on)
static int g_ps44x14 = 1;       // R14 t2r-ps44-cam-attkill-old.txt: (file present = R11 only: duel finishers mf-*) - the note in s32_fin_begin
static struct {
    int early, to_c, cut, nleg;
    uint64_t t0, tc, last_tc, job[S44M_NR];
    float arc, dur;
    char trig[96], why[96];
} g_s44m;
static int S44mVicCommit(int *slot, int *fin, int *to_c);     // melee_camcont.c
// ps45 (rec44a review, the camera got worse with ps44). Opt-outs re-read every 2 s (file present = the ps44 behaviour); proof lines "pincam5: ps45" /
// "paircam: ps45" / "xcam: ps45" (t2r-ext.c, the note at S45X_*: X1 / X2, the authored execution / takedown cameras).
//   PS45_UNDER t2r-ps45-cam-under-old.txt  U1 (A 12804 / B 13943 'ps44 vout ... lets go after a hold of 47 ms past the pair's end: the downed camera has
//              settled (the camera under it 306.94 / 303.51 m from our live head ... moved 0.00 m in a tick)' = the world origin: s34_apply ran on the
//              render camera's second view too and wrote ITS position as the camera under the shot. The settle hold let go at once and the ps40 / ps41
//              way-out test measured its arc toward the origin: B 13943 'its arc: 101 deg ... ONE CUT' (clip B 88.97 cut), A no ray at all (r1 > 8 m) =
//              the 900 ms ease went round our chest through the attacker (clip A 68.60-68.70 lens inside his backpack ~90 %). The camera under the shot
//              is taken only within S45U_R of our body.
//   PS45_EARLY t2r-ps45-cam-early-old.txt  U2 (B 12011 'the early side shot was NOT confirmed ... it eases out' -> 'ps40 - ... way out ... BLOCKED ... or
//              longer than 100 deg (its arc: 107 deg) ... ONE CUT', clip B 54.567 cut): an unconfirmed early side shot goes back the way it came (its arc
//              in was ray-tested) - the 900 ms ease, never the ps40 cut.
//   PS45_ARC   t2r-ps45-cam-arc-old.txt    U3 (A 12727 'side shot comes in by ONE CUT (fallback) ... arc 162 deg ... leg 6 of 6 ... BLOCKED by geometry',
//              B 13829 'arc 140 deg ... leg 3 of 6 ... BLOCKED', clips A 67.067 / B 87.100 cuts): a blocked arc tries two other paths before the cut -
//              the other way round the pair (arcs up to S45M_MAXARC) and the planned arc raised S45M_UP mid-way; the first clear one is flown.
//   PS45_EASE  t2r-ps45-cam-ease-old.txt   U4 (B 18701-18723 camrec 'cyaw +2.1 -> -98' in 22 frames, 419 ms smootherstep = up to 25 deg / 3 frames, clip B
//              169.20-169.33 '20-30 deg per frame'; B 14830-14850 the same): the way in runs S45M_T0..S45M_T1 by the arc on a smoothstep (peak 1.5x the
//              mean speed, smootherstep 1.875x).
//   PS45_FOV   t2r-ps45-cam-fov-old.txt    U5 (A 12787 'frame check - WARNING: the ATTACKER's head out of frame ... (x +0.94 y +1.12 ...)' at 'ps44 fov ... 33.0
//              deg vertical ... at the narrow clamp', clip A 67.53-68.43 / B 88.53-88.97 attacker cut at the frame edge): the cinematic fov is also fitted
//              from the AIMED axis - both heads and our chest inside S45F_EDGE of the half-frame - widened up to S45F_VMAX.
#define PS45_UNDER 0
#define PS45_EARLY 1
#define PS45_ARC 2
#define PS45_EASE 3
#define PS45_FOV 4
//   PS45_PEXEC t2r-ps45-cam-postexec-old.txt  P1 (rec44a B 10867 'ps44 attexec fist-execute-enter-front ... no behind-the-body hold after it', 11109 'xcam:
//              hand-back ... 0.75 s', camrec 11109-11154 'back -1.54 .. -1.30 cyaw -160 .. -153', clip B 39.80-41.57: the lens in front of our own fighter,
//              the corpse out of frame): R12 was about the execution itself (play the 2013 authored track - kept). After that track ends, the hand-back of
//              an authored attacker execution takes the ps41 / ps42 behind-the-body lens again (s41b_tick: engine lens > 60 deg round from behind our body),
//              eased round our head over S45P_MIN..S45P_MAX (smootherstep, no cut), then released onto the engine camera by the ps41 rules. File present =
//              ps44 (the plain hand-back). Proof 'behindcam: ps45 postexec'.
//   PS45_SIDE  t2r-ps45-cam-sideease-old.txt  S1 (rec44a A 9743 / 9744 'ps44 head floor ... ONE cut to the LEFT side', clip A 15.200; ps42 'side-on finisher
//              shot - the victim was hidden ... ONE cut'): the rig's side swaps that were ONE cut are a fast eased swing of S45S_T0..S45S_T1 round our pivot
//              (smoothstep). Its path - the short way round, then the other way (up to S45M_MAXARC) - is ray-tested in S45S_NR legs at the lens height and
//              kept S45S_BODY from the partner's root (s45s_flip kicks, s45s_tick reads one tick later); every path blocked = the old ONE cut (logged).
//              During the swing ccg_filter / cam2_filter pass it through (s45s_swing) - a timed owner, like the victim's shot. Proof 'spmelee cam: ps45 sideease'.
#define PS45_PEXEC 5
#define PS45_SIDE 6
#define PS45_N 7
#define S45P_MIN 0.60f          /* s [TUNING ps45]: the post-execution way to the behind-the-body lens */
#define S45P_MAX 0.80f          /* s [TUNING ps45] */
#define S45S_T0 0.30f           /* s [TUNING ps45]: a side swing over a short arc */
#define S45S_T1 0.40f           /* s [TUNING ps45]: over a half circle or more */
#define S45S_NR 6               /* legs of a side swing's path test */
#define S45S_BODY 0.60f         /* m: the closest a swing's path may pass the partner's root [TUNING ps45] */
static volatile int g_s45sDsg; // main -> render: the decided way round of the next side swing (+1 / -1 = the sign of the azimuth step)
static volatile float g_s45sT = S45S_T0;       // main -> render: its time (s)
static volatile double g_s45sUntil;    // render: gsh_now() until the swing runs (ccg / cam2 pass it through - s45s_swing)
static volatile LONG g_s45sN;  // render -> main: swings flown
static struct {
    int st;
    LONG kind;
    uint64_t t0, job[2][S45S_NR];
    float da[2], nb[2], R0;
    int ok[2];
    char why[96];
} g_s45s;                       // main: st 1 = rays out
#define S45U_R 25.0f            /* m: a camera under the shot farther from our body is not it */
#define S45M_UP 0.90f           /* m the raised arc climbs mid-way [TUNING ps45] */
#define S45M_MAXARC 3.50f       /* rad (200 deg): the longest way round tried [TUNING ps45] */
#define S45M_T0 0.42f           /* s [TUNING ps45] */
#define S45M_T1 0.50f           /* s [TUNING ps45] */
#define S45F_EDGE 0.80f         /* share of the half-frame [TUNING ps45] */
#define S45F_VMAX 55.0f         /* deg vertical [TUNING ps45] */
static volatile int g_ps45[PS45_N] = { 1, 1, 1, 1, 1, 1, 1 };

static volatile LONG g_s45uRej;        // render: camera-under copies rejected (U1)
static uint64_t g_s45eT0;      // U2: the vf_t0 of an unconfirmed early side shot on its way out
static volatile int g_s45m_var, g_s45m_mv;
static volatile uint64_t g_s45mMvAt;  // (ps45 pre-mortem: _at = when the way in was decided; a flag older than 1.5 s is stale - its shot never started)
static int S45mMvTake(void)
{
    if (!g_s45m_mv)
        return 0;
    g_s45m_mv = 0;
    return GetTickCount64() - g_s45mMvAt < 1500;
}

//        // U3 / U4: main -> render, the decided way in (0 planned, 1 the other way round, 2 raised) / the in-move is a ps44 vicmove
static struct {
    uint64_t job[2][S44M_NR];
    float nb[3], da[3];
    int ok[3];
} g_s45m;
static volatile LONG g_s45fN;
static volatile float g_s45f_v, g_s45f_v0;      // U5: fits that widened the fov, the last one (deg) and the ps44 fit under it
static int S45UnderOk(const float *pos)
{                               // render thread (U1)
    if (!g_ps45[PS45_UNDER])
        return 1;
    if (!(__builtin_fabsf(pos[0]) < 1e6f)
        || !(__builtin_fabsf(pos[1]) < 1e6f)
        || !(__builtin_fabsf(pos[2]) < 1e6f)) {
        InterlockedIncrement(&g_s45uRej);
        return 0;
    }
    float ux = g_pcam.ux, uz = g_pcam.uz, dx = pos[0] - ux, dz =
        pos[2] - uz;
    if (ux == 0.0f && uz == 0.0f)
        return 1;
    if (dx * dx + dz * dz > S45U_R * S45U_R) {
        InterlockedIncrement(&g_s45uRej);
        return 0;
    }
    return 1;
}

static void Ps45Cfg(uint64_t now)
{
    static uint64_t at;
    static int first = 1;
    static const char *const F[PS45_N] =
        { "t2r-ps45-cam-under-old.txt", "t2r-ps45-cam-early-old.txt",
"t2r-ps45-cam-arc-old.txt", "t2r-ps45-cam-ease-old.txt", "t2r-ps45-cam-fov-old.txt",
        "t2r-ps45-cam-postexec-old.txt", "t2r-ps45-cam-sideease-old.txt"
    };
    if (!first && now - at < 2000)
        return;
    at = now;
    for (int i = 0; i < PS45_N; i++) {
        int v = GetFileAttributesA(F[i]) == INVALID_FILE_ATTRIBUTES;
        // ps46 (clip 17-52-05: the victim camera was far too wide; logs: U5 widened the side shot from
        // the ps44 33-42 deg to 50-55 deg vertical on 273 / 682 fits): U5 is OFF by default now, t2r-ps46-cam-fov-on.txt = the ps45 widening.
        if (i == PS45_FOV) {
            v = GetFileAttributesA("t2r-ps46-cam-fov-on.txt") !=
                INVALID_FILE_ATTRIBUTES;
            if (v != g_ps45[i] || first)
                Xlog("pincam5: ps46 victim side shot fov fit (U5) %s",
                     v ? "ON (t2r-ps46-cam-fov-on.txt)" :
                     "OFF - the ps44 cinematic 33-42 deg");
            g_ps45[i] = v;
            continue;
        }
        if (v != g_ps45[i] || first)
            Xlog("pincam5: ps45 %s %s", F[i],
                 v ? "absent - the ps45 behaviour is ON" :
                 "present - the OLD (ps44) behaviour");
        g_ps45[i] = v;
    }
    {
        int v =
            GetFileAttributesA("t2r-ps45-cam-xrate-old.txt") ==
            INVALID_FILE_ATTRIBUTES, p =
            GetFileAttributesA("t2r-ps45-cam-xpush-old.txt") ==
            INVALID_FILE_ATTRIBUTES;
        if (v != g_ps45x || first)
            Xlog("xcam: ps45 t2r-ps45-cam-xrate-old.txt %s",
                 v ?
                 "absent - an authored execution / takedown camera follows its track at most 260 deg/s of view turn, 3.5 m/s of lens travel (whip pans + hitches eased): ON"
                 :
                 "present - the OLD (ps44) behaviour: the track played as is");
        if (p != g_ps45p || first)
            Xlog("xcam: ps45 t2r-ps45-cam-xpush-old.txt %s",
                 p ?
                 "absent - an authored execution / takedown lens is pushed out of both bodies' capsules along its look line: ON"
                 : "present - the OLD (ps44) behaviour: no push");
        g_ps45x = v;
        g_ps45p = p;
    }
    {                           // ps45 camera worker 2 (t2r-ext.c, the note above S45H_FADE / at S45A_*): A1 attacker track clearance, H2 hand-back floor, D3 downed lens release
        static const char *const F2[4] =
            { "t2r-ps45-cam-attclear-old.txt",
"t2r-ps45-cam-hbfloor-old.txt", "t2r-ps45-cam-downrel-old.txt",
"t2r-ps45-cam-prone-old.txt" };
        static const char *const D2[4] =
            {
"an attacker's TLOU2 finisher / takedown track keeps 1.25 m from our body (stepped back along its look line, side step when the victim hides behind us; proof 'xcam: ps45 A1')",
            "the attacker hand-back's 1.40 m follow floor fades in from the cine end (no first-frame step)",
            "the downed lens guard lets go of a wall pull-in at 4 m/s once its ray is clear",
            "the prone pull-out / grapple attacker camera = a close over-the-shoulder two-shot on the victim's upper body (proof 'pullout cam: ps45')"
        };
        volatile int *G2[4] = { &g_ps45a, &g_ps45h, &g_ps45d, &g_ps45pr };
        for (int i = 0; i < 4; i++) {
            int v = GetFileAttributesA(F2[i]) == INVALID_FILE_ATTRIBUTES;
            if (v != *G2[i] || first)
                Xlog("xcam: ps45 %s %s", F2[i],
                     v ? "absent - ON" :
                     "present - the OLD (ps44) behaviour");
            if (first && v)
                Xlog("xcam: ps45   (%s)", D2[i]);
            *G2[i] = v;
        }
        static LONG lh, ld;
        LONG h = g_s45h_n, d = g_s45d_n;
        if (h != lh) {
            Xlog("xcam: ps45 H2 - %ld attacker hand-back(s) started under the 1.40 m follow floor so far (+%ld; the last %.2f m inside it): the floor faded in over the first %.0f %% of the way back instead of a first-frame step (frame %llu)", (long) h, (long) (h - lh), (double) g_s45hStep, (double) (S45H_FADE * 100.0f), (unsigned long long) g_frame);
            lh = h;
        }
        if (d != ld) {
            Xlog("xcam: ps45 D3 - the downed lens guard let go of a wall pull-in at %.1f m/s (its ray clear) on %ld frame(s) so far (+%ld; was 1.5 m/s; frame %llu)", (double) S45D_REL, (long) d, (long) (d - ld), (unsigned long long) g_frame);
            ld = d;
        }
    }
    {
        static LONG ls;
        LONG n = g_s45sN;
        if (n != ls) {
            Xlog("spmelee cam: ps45 sideease - %ld rig side swing(s) flown so far (+%ld; eased, no cut; t2r-ps45-cam-sideease-old.txt = the ONE cut; frame %llu)", (long) n, (long) (n - ls), (unsigned long long) g_frame);
            ls = n;
        }
    }
    {
        static LONG lr;
        LONG r = g_s45uRej;
        if (r != lr) {
            Xlog("pincam5: ps45 under - %ld render camera cop%s rejected as the camera under the victim's shot so far (+%ld; farther than %.0f m from our body / non-finite - the second view's camera at the world origin); the ps44 settle hold and the ps40 way-out test read the real one (frame %llu)", (long) r, r == 1 ? "y" : "ies", (long) (r - lr), (double) S45U_R, (unsigned long long) g_frame);
            lr = r;
        }
    }
    {
        static LONG ln;
        LONG n = g_s45fN;
        if (n != ln) {
            Xlog("paircam: ps45 fov - the victim's side shot fov widened from the aimed axis on %ld fit(s) so far (+%ld; last %.1f deg vertical, the ps44 fit %.1f: both heads + our chest inside %.0f %% of the half-frame; frame %llu)", (long) n, (long) (n - ln), (double) g_s45f_v, (double) g_s45f_v0, (double) (S45F_EDGE * 100.0f), (unsigned long long) g_frame);
            ln = n;
        }
    }
    first = 0;
}

// U5: the vertical fov (deg) that keeps both heads and our chest inside S45F_EDGE of the half-frame round the axis E -> L (0 = no answer)
static float S45fFit(const float *E, const float *L,
                      const float *const *pt, int n)
{
    float f[3] = { L[0] - E[0], L[1] - E[1], L[2] - E[2] }, fl =
        __builtin_sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(fl > 0.2f))
        return 0.0f;
    for (int i = 0; i < 3; i++)
        f[i] /= fl;
    float r[3] = { f[2], 0.0f, -f[0] }, rl =
        __builtin_sqrtf(r[0] * r[0] + r[2] * r[2]);
    if (!(rl > 0.05f))
        return 0.0f;
    r[0] /= rl;
    r[2] /= rl;
    float u[3] =
        { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2],
   r[0] * f[1] - r[1] * f[0] }, need = 0.0f;
    for (int k = 0; k < n; k++) {
        const float *p = pt[k];
        if (!p || !XvFin3(p))
            continue;
        float v[3] = { p[0] - E[0], p[1] - E[1], p[2] - E[2] }, dep =
            v[0] * f[0] + v[1] * f[1] + v[2] * f[2];
        if (dep < 0.4f)
            continue;
        float x =
            __builtin_fabsf(v[0] * r[0] + v[1] * r[1] + v[2] * r[2]) / dep,
            y =
            __builtin_fabsf(v[0] * u[0] + v[1] * u[1] + v[2] * u[2]) / dep;
        float t = y / S45F_EDGE, th = x / S45F_EDGE * 9.0f / 16.0f;
        if (t > need)
            need = t;
        if (th > need)
            need = th;
    }
    return need > 0.0f ? 2.0f * __builtin_atanf(need) * 57.29578f : 0.0f;
}

static void Ps44Cfg(uint64_t now)
{
    static uint64_t at;
    static int first = 1;
    static const char *const F[PS44_N] =
        { "t2r-ps44-cam-tp-old.txt", "t2r-ps44-cam-under-old.txt",
"t2r-ps44-cam-exit-old.txt", "t2r-ps44-cam-exec-old.txt", "t2r-ps44-cam-aim-old.txt",
"t2r-ps44-cam-head-old.txt", "t2r-ps44-cam-vout-old.txt", "t2r-ps44-cam-eng-old.txt",
"t2r-ps44-cam-fov-old.txt", "t2r-ps44-cam-attfin-old.txt", "t2r-ps44-cam-attexec-old.txt" };
    if (!first && now - at < 2000)
        return;
    at = now;
    for (int i = 0; i < PS44_N; i++) {
        int v = GetFileAttributesA(F[i]) == INVALID_FILE_ATTRIBUTES;
        if (v != g_ps44[i] || first)
            Xlog("paircam: ps44 %s %s", F[i],
                 v ? "absent - the ps44 behaviour is ON" :
                 "present - the OLD (ps43) behaviour");
        g_ps44[i] = v;
    }
    {
        int v = GetFileAttributesA("t2r-ps44-cam-vicmove-old.txt") == INVALID_FILE_ATTRIBUTES;  // R13 (the note at S44M_*)
        if (v != g_ps44m || first)
            Xlog("pincam5: ps44 t2r-ps44-cam-vicmove-old.txt %s",
                 v ?
                 "absent - the victim's finisher side shot MOVES in along the arc round the pair (no cut), started once the blow can no longer fail on our screen: ON"
                 :
                 "present - the OLD (ps43) behaviour: one cut in at the pair's start");
        g_ps44m = v;
    }
    {
        int v = GetFileAttributesA("t2r-ps44-cam-vrep-old.txt") == INVALID_FILE_ATTRIBUTES;     // R5a late / R5c (the note at S44V_*)
        if (v != g_ps44vr || first)
            Xlog("pincam5: ps44 t2r-ps44-cam-vrep-old.txt %s",
                 v ?
                 "absent - the victim's side shot also tests lens -> our chest every tick; its one late correction re-plans (the next best planned spot): ON"
                 :
                 "present - the OLD (ps41) late side flip, head ray only");
        g_ps44vr = v;
    }
    {
        int v = GetFileAttributesA("t2r-ps44-cam-attkill-old.txt") == INVALID_FILE_ATTRIBUTES;  // R14 (s32_fin_begin's note); t2r-ps44-cam-attfin-old.txt switches R11 + R14 off together
        if (v != g_ps44x14 || first)
            Xlog("paircam: ps44 t2r-ps44-cam-attkill-old.txt %s",
                 v ?
                 "absent - EVERY paired down / kill on the attacker's screen plays SP's camera (authored track, else the rig push): ON"
                 : "present - only the duel finishers (R11)");
        g_ps44x14 = v;
    }
    {
        int v = GetFileAttributesA("t2r-ps44-cam-td-old.txt") == INVALID_FILE_ATTRIBUTES;       // R8b (t2r-ext.c, the note at g_ps44x)
        if (v != g_ps44x || first)
            Xlog("xcam victim: ps44 t2r-ps44-cam-td-old.txt %s",
                 v ?
                 "absent - a steep authored execution shot keeps its full roll outside the 84 deg hold, the hold's heading restarts at every entry (no 180 deg picture turn at the hold's edge): ON"
                 : "present - the OLD (ps43) behaviour");
        g_ps44x = v;
        static LONG lr, le;
        LONG r = g_s44x_roll, e = g_s44x_re;
        {
            static LONG lc;
            LONG c = g_s44eCuts;
            if (c != lc) {
                lc = c;
                Xlog("spmelee cam: ps44 engage - the rig's spot taken on ONE cut at the engage (the incoming lens %.0f deg off it%s; ps42 eased anything under 60 deg; cut #%ld; t2r-ps44-cam-eng-old.txt = ps42; frame %llu)", (double) g_s44eDeg, g_s44eOof ? ", the partner's head out of frame" : "", (long) c, (unsigned long long) g_frame);
            }
        }
        {
            static LONG ln;
            static uint64_t lt;
            LONG n = g_s44h.n;
            if (n != ln && now - lt >= 5000) {
                lt = now;
                Xlog("spmelee cam: ps44 head floor - the duel lens was held %.2f m from our LIVE head joint (up and over it, re-aimed) on %ld frame(s) so far (+%ld; t2r-ps44-cam-head-old.txt = off; frame %llu)", (double) S44H_MIN, (long) n, (long) (n - ln), (unsigned long long) g_frame);
                ln = n;
            }
        }
        if (r != lr || e != le) {
            Xlog("xcam victim: ps44 - %s: an authored roll over 80 deg was kept on %ld steep frame(s) so far (+%ld; ps43 dropped it = the picture upside down against the straight-down hold), the hold's heading restarted on %ld entr%s (+%ld; frame %llu)", g_xcamName, (long) r, (long) (r - lr), (long) e, e == 1 ? "y" : "ies", (long) (e - le), (unsigned long long) g_frame);
            lr = r;
            le = e;
        }
    }
    first = 0;
}

// the two-shot was not obtained (no spot / the plan timed out): the rig push under it becomes the ps42 side-on push again, ramping in from now
static void S44UnderOff(uint64_t now)
{
    if (!g_s44Under)
        return;
    g_s44Under = 0;
    if (g_s32.on) {
        g_s42Side = 1;
        g_s44Pt0 = now;
        static int nl;
        if (LOGCAP(nl, 80))
            Xlog("paircam: ps44 - no two-shot for this move: the rig push under it runs again as the ps42 side-on push, ramping in from now (frame %llu)", (unsigned long long) g_frame);
    }
}

static void XvBody(uint64_t h, const float *loc, float *pel, float *chest,
                    float *head);
// ps44 R5b (the note at S44O_*): main thread, spc_tick5 at the victim's finisher shot's end. 1 = the shot is held this tick
static int S44oHold(uint64_t now, int slot)
{
    if (!g_ps44[PS44_VOUT])
        return 0;
    if (g_s44o.vt0 != g_s34.vf_t0) {
        g_s44o.vt0 = g_s34.vf_t0;
        g_s44o.st = 0;
    }
    if (g_s44o.st == 3)
        return 0;
    if (g_s44o.st == 0) {
        g_s44o.st = 1;
        g_s44o.t0 = now;
        g_s44o.ok_n = 0;
        g_s44o.lp[0] = 1e9f;
    }
    float pb[16], ab[16];
    const float *me = PlayerLocator(pb);
    uint64_t h = slot >= 0 ? PuppetHandle((uint32_t) slot) : 0;
    const float *ot = h ? PuppetLocator(h, ab) : NULL;
    const char *res = NULL;
    float dh = -1.0f, ang = -1.0f, dat = -1.0f, mv = -1.0f;
    if (!me || !PsLocOk(me) || !g_s40InOk)
        res = "our body / the camera under the shot is not readable";
    else {
        float p0[3], c0[3], h0[3], G[3] =
            { g_s40In[0], g_s40In[1], g_s40In[2] }, q[4] =
            { g_s44oQ[0], g_s44oQ[1], g_s44oQ[2], g_s44oQ[3] }, f[3];
        XvBody(SID_PLAYER, me, p0, c0, h0);
        SpmFwd(q, f);
        dh = __builtin_sqrtf((G[0] - h0[0]) * (G[0] - h0[0]) +
                             (G[1] - h0[1]) * (G[1] - h0[1]) + (G[2] -
                                                                h0[2]) *
                             (G[2] - h0[2]));
        float v[3] = { c0[0] - G[0], c0[1] - G[1], c0[2] - G[2] }, vl =
            __builtin_sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), fl =
            __builtin_sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        ang = vl > 0.05f
            && fl >
            0.5f ?
            __builtin_acosf(fminf
                            (1.0f,
                             fmaxf(-1.0f,
                                   (v[0] * f[0] + v[1] * f[1] +
                                    v[2] * f[2]) / (vl * fl)))) : 9.0f;
        if (ot && PsLocOk(ot)) {
            float p1[3], c1[3], h1[3];
            XvBody(h, ot, p1, c1, h1);
            dat = S41vSegd(G, G, p1, h1);
        }
        mv = g_s44o.lp[0] <
            1e8f ? __builtin_sqrtf((G[0] - g_s44o.lp[0]) *
                                   (G[0] - g_s44o.lp[0]) + (G[1] -
                                                            g_s44o.lp[1]) *
                                   (G[1] - g_s44o.lp[1]) + (G[2] -
                                                            g_s44o.lp[2]) *
                                   (G[2] - g_s44o.lp[2])) : 9.0f;
        g_s44o.lp[0] = G[0];
        g_s44o.lp[1] = G[1];
        g_s44o.lp[2] = G[2];
        int ok = dh >= S44O_HEAD && ang <= S44O_FRAME && (dat < 0.0f
                                                          || dat >= 0.40f)
            && mv < 0.05f;
        g_s44o.ok_n = ok ? g_s44o.ok_n + 1 : 0;
        if (g_s44o.ok_n >= 3)
            res = "the downed camera has settled";
    }
    if (!res && now - g_s44o.t0 > S44O_MAX)
        res = "the hold's time-out";
    static int nl;
    if (res) {
        g_s44o.st = 3;
        if (LOGCAP(nl, 200))
            Xlog("paircam: ps44 vout - the victim's side shot lets go after a hold of %llu ms past the pair's end: %s (the camera under it %.2f m from our live head, our chest %.0f deg off its axis, %.2f m from the attacker's torso, moved %.2f m in a tick%s; t2r-ps44-cam-vout-old.txt = no hold; frame %llu)", (unsigned long long) (now - g_s44o.t0), res, (double) dh, (double) (ang * 57.29578f), (double) dat, (double) mv, g_s44o.tpb ? "; it was pulled back" : "", (unsigned long long) g_frame);
        g_s44o.tpb = 0;
        return 0;
    }
    if (g_s44o.st == 1 && now - g_s44o.t0 > S44O_PB) {
        g_s44o.st = 2;
        g_s44o.tpb = now;
        if (LOGCAP(nl, 200))
            Xlog("paircam: ps44 vout - the downed camera has not settled %d ms after the pair's end (%.2f m from our live head, our chest %.0f deg off its axis, %.2f m from the attacker's torso): the held side shot pulls back %.2f m and looks at our own body (frame %llu)", S44O_PB, (double) dh, (double) (ang * 57.29578f), (double) dat, (double) S44O_BACK, (unsigned long long) g_frame);
    } else if (g_s44o.st == 1 && now - g_s44o.t0 < 20 && LOGCAP(nl, 200))
        Xlog("paircam: ps44 vout - the pair is over: the victim's side shot holds until the downed camera has settled (>= %.2f m from our live head, our body in its frame, not inside the attacker; frame %llu)", (double) S44O_HEAD, (unsigned long long) g_frame);
    return 1;
}

// 1 = seen from the lens C, the other body's head AND chest both stand behind the body 'own' (its pelvis -> head line, live joints)
static int S44Hidden(const float *C, uint64_t own_h, const float *own,
                      uint64_t oth_h, const float *oth)
{
    float p0[3], c0[3], h0[3], p1[3], c1[3], h1[3];
    XvBody(own_h, own, p0, c0, h0);
    XvBody(oth_h, oth, p1, c1, h1);
    if (!XvFin3(p0) || !XvFin3(h0)
        || __builtin_fabsf(h0[1] - own[1]) > 2.2f) {
        p0[0] = h0[0] = own[0];
        p0[2] = h0[2] = own[2];
        p0[1] = own[1] + 0.90f;
        h0[1] = own[1] + 1.55f;
    }
    if (!XvFin3(c1) || !XvFin3(h1)
        || __builtin_fabsf(h1[1] - oth[1]) > 2.2f) {
        c1[0] = h1[0] = oth[0];
        c1[2] = h1[2] = oth[2];
        c1[1] = oth[1] + 1.10f;
        h1[1] = oth[1] + 1.45f;
    }
    float *T[2] = { h1, c1 };
    for (int i = 0; i < 2; i++) {       // (the line stops 0.35 m short of its target: a victim held against our body is not 'behind' it)
        float dx = C[0] - T[i][0], dy = C[1] - T[i][1], dz =
            C[2] - T[i][2], l =
            __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
        if (!(l > 0.5f))
            return 0;
        float e[3] =
            { T[i][0] + dx / l * 0.35f, T[i][1] + dy / l * 0.35f,
       T[i][2] + dz / l * 0.35f };
        if (!(S41vSegd(C, e, p0, h0) < S44_HID_R))
            return 0;
    }
    return 1;
}

// ps43 Q1 (rec42a review) - the engage cut's DESTINATION comes from the two fighters' live roots, never from the incoming engine lens.
//   A 132.67-132.90 (16153 a 48 m respawn teleport, the engine camera still flying in; 16160 'ps42 engage cut'; camdiag 16159 'gsh_filter ... move 2.51 m,
//   lens_guard ... move 2.08 m', 'lens-head 0.39 m', camrec 16161-16173 'dist 0.41 .. 0.55'): spm25_rig's cut set the azimuth only - the radius stayed the
//   incoming lens's (2.5 m, the fly-in), and the lens guard's pull-in built up during the fly-in (2.1 m, released at 1 m/s) held the lens 0.35-0.5 m from
//   our head for 14 frames: jacket and backpack over the frame. A 2.30 (8374 'ps39 rig cut', camdiag 'lens-head 1.80 m', 'shown +1.23 / 1.32'): radius 1.80 m
//   from the engine lens. B 73.30-73.67 (14761 'ps37 reset' + 'ps42 engage latch ... on the tick of a teleport', 'partner's head x +9.99', camrec 14763-14772
//   'dist 0.59 .. 0.78 ... cyaw +142 .. +150', no cut line): the engage fell on the teleport tick, the rig started on the stale lens and judged it 'near
//   its spot'. B 173.33-173.80 (20715 rig cut, 20741 'shown +0.96 / 0.82' = the lens IS on our side of the line; camrec 'back -1.58 .. -1.15': our BODY
//   faced away from the partner until 20742) is the body's facing, not this.
// Now: when the rig latches (spm_tick: the look-at target goes 0 -> 1) the main thread plans the cut's spot from the raw roots - SP's spot (SPM25_AZ off
// straight-behind the line us -> partner, SP's distance) on the nearer shoulder side (right when the lens is stale) - and kicks three geometry rays from
// our head: that spot, the other shoulder's, and one straight behind raised S43Q_UP. One tick later (two when our root was teleported on the latch tick:
// the plan waits for the tick after it) the answers pick: the kept side (arm shortened to the clear length, never under S43Q_RMIN), else the other
// shoulder, else the raised spot, else the longest of the three at S43Q_RMIN (the lens guard has the last word). The rig does not start until the plan is
// there (the look-at weight is held at 0 for that tick). spm25_rig then cuts as before (lens > 60 deg off / on the partner's side) and ALWAYS when our
// root was teleported within S43Q_TP_MS (the engine lens is stale by definition) - to the planned azimuth, radius and height, which ease to the chain's
// own over S43Q_HOLD. One cut per engage: no second rig cut for S43Q_GAP. The lens guard (melee_camcont.c clg_filter) drops a pull-in left over from
// the old lens line on the cut. Log "spmelee cam: ps43 engage cut destination". t2r-ps43-cam-engdest-old.txt (re-read every 2 s) = the ps42 cut.
#define S43Q_RMIN 0.90f         // m: the cut's radius from our axis is never under this
#define S43Q_PAX 0.60f          // m: nor the lens closer than this to the partner's axis
#define S43Q_UP 0.40f           // m: the raised fallback spot
#define S43Q_GAP 1.5            // s: no second rig cut after an engage cut
#define S43Q_TP_MS 1000         // ms [TUNING ps43]: the engine lens counts as stale this long after an own-body teleport (rec42a A 16153-16162: its fly-in ran ~10 frames after 48 m; cam2 blends such a jump over 1200 ms)
#define S43Q_HOLD 0.60f         // s [TUNING ps43]: the cut's radius / height ease to the chain's own over this (the framing weight's spring is in by then)
#define S43Q_WHOLD 1.5          // s [TUNING ps43]: after a cut to the raised fallback spot (both shoulders blocked) the rig does not walk to a shoulder for this long
#define S43Q_FRESH_MS 400       // ms [TUNING ps43]: a plan older than this is not used
static volatile int g_ps43q = 1;
static struct {
    volatile int st;            // main: 0 none, 3 to kick, 1 rays kicked, 2 planned
    volatile float az, R, up, side;
    volatile int fb, tp;
    volatile uint64_t ms;       // the plan (main -> render)
    float beside, behind, pax, mid, al[3];
    int rerr;                   // its numbers for the proof line
    uint64_t job[3];
    float jl[3], caz[3], P[3], E[2], f[2], d, R0;
    float cside;
    uint64_t tp_ms, tick_ms, plan_ms;
    int yt_was;
    float tx, tz;
    int t_ok;
    volatile LONG cuts, clg;
    volatile float c_rl, c_e;
    volatile int c_fb, c_tp;    // render -> main: cuts taken, lens-guard pull-ins dropped; the incoming radius / angle off the spot (not used for the spot), what the cut used
} g_s43e;
static struct {
    double t, wh;
    float R0, up, R, k, tin;
} g_s43qh;                      // render thread (spm25_rig): the cut's radius / height hold (ps46: tin > 0 = eased in over tin s first)
static void S46Drain(void);    // ps46 E1 (below)
static void S43qDrain(void)
{
    for (int i = 0; i < 3; i++)
        if (g_s43e.job[i]) {
            RayT(g_s43e.job[i]);
            g_s43e.job[i] = 0;
        }
    S46Drain();
}

// ps46 E1 (rec45a: the camera cut too abruptly when initiating a melee duel; ablogs-rec45a: A 15952 / 17821 / 23100 / 23951,
//   B 16284 / 17832 / 19717 / 20812 / 22811 / 23767 / 25046 / 25355 / 25939 'ps43 engage cut destination' = 13 engages taken on ONE cut; 9 of them
//   'our root was teleported just before: ... the cut is taken whatever its angle' with the incoming lens 1.4-1.8 m from our pivot (A 15952 4.12 m = a
//   real 49.7 m respawn), and 6 of those 9 (A 17821, B 17832 / 19717 / 22811 / 23767 / 25939) had no camcont 'filter reset (respawn / teleport)' on
//   their frame: 'ps37 reset - our root moved 5.7 m in one tick' compared our root against the sample of the PREVIOUS melee window). Now:
//   - the engage takes the ps43 planned spot on a swift EASE, not the cut: the lens swings round our pivot from the shown lens to the spot over
//     S46_T0..S46_T1 (smoothstep, the line's own turn followed, as the ps45 side swing), the spot's radius / height eased in over the same time and
//     then handed to the chain over S43Q_HOLD as before; the look-at heading turns with the swing; no stage restart (g_s37_gen untouched); ccg / cam2
//     pass it through (s45s_swing). Its path - the short way round, then the other way (up to S45M_MAXARC) - is ray-tested in S45S_NR legs and kept
//     S45S_BODY off the partner's root (s46_kick when the plan is made, s46_read one tick later; the rig waits that tick at look-at weight 0).
//     Every path blocked / the shown lens off our pivot (stale, flying in) / a real own-body teleport within S43Q_TP_MS / the lens moved off the
//     tested path = the ps43 ONE cut (logged with the reason).
//   - the ps37 watch counts a move as a teleport only between two samples at most S46_TP_FRESH apart (else the sample is re-taken; s43q_track's
//     every-tick watch still flags a real teleport for the ps43 rule).
//   t2r-ps46-cam-engage-old.txt (re-read every 2 s) = ps45. Proof 'spmelee cam: ps46 engage'.
#define S46_T0 0.35f            /* s [TUNING ps46]: the engage ease over a short arc */
#define S46_T1 0.50f            /* s [TUNING ps46]: over a half circle or more */
#define S46_RMIN 0.50f          /* m [TUNING ps46]: a shown / incoming lens closer to our pivot is not one to ease from */
#define S46_RMAX 4.00f          /* m [TUNING ps46]: farther = stale / flying in (rec45a A 15952: 4.12 m on a 49.7 m respawn) */
#define S46_DY 1.50f            /* m [TUNING ps46]: the shown lens this far above / below the lens height = stale */
#define S46_DRIFT 0.35f         /* rad = 20 deg [TUNING ps46]: the rig's lens this far off the tested path's start = cut */
#define S46_TP_FRESH 250        /* ms [TUNING ps46]: the ps37 watch's two samples at most this far apart */
static volatile int g_ps46 = 1;
static uint64_t g_spmPh;       // (tentative: defined with the framing below)
static struct {
    int st;
    uint64_t job[2][S45S_NR];
    float da[2], nb[2], R0, dy, a0;
    int ok[2], bl[2];           // main: st 1 = path rays out
    volatile int pick, why;
    volatile float pda, pT, pnb;
    volatile uint64_t ms;       // main -> render: the answer for the plan of g_s43e.ms (pick -1: why)
    volatile LONG n, nc;
    volatile int cwhy;
    volatile float cdeg, cT, cR0, cR1, cnb;     // render -> main: eases / cuts at an engage, the last one's numbers
    LONG tps;
    float tpd;
    uint64_t tpg;               // main: stale ps37 samples not taken as a teleport
} g_s46 = {.pick = -1,.why = -1,.cwhy = -1 };

static volatile double g_s46Until;     // render: gsh_now() until the engage ease runs (ccg / cam2 pass it through - s45s_swing)
static const char *const S46_WHY[] =
    { "-", "the shown camera is not readable",
"the shown lens is not near our pivot (stale / flying in)", "every path is blocked by geometry",
    "every path passes too near the partner",
        "no fresh plan for this engage",
        "our root was really teleported just before (the engine lens is stale)",
    "the incoming lens is not near our pivot",
        "the lens moved off the tested path's start"
};

static void S46Drain(void)
{
    for (int v = 0; v < 2; v++)
        for (int i = 0; i < S45S_NR; i++)
            if (g_s46.job[v][i]) {
                RayT(g_s46.job[v][i]);
                g_s46.job[v][i] = 0;
            }
    g_s46.st = 0;
}

static void S46Arc(int v, const float *cp, float px, float pz, float a0,
                    float da, float R0, float R1, float dup, float ox,
                    float oz)
{                               // (s45s_arc with a height step)
    float Q0[3] = { cp[0], cp[1], cp[2] }, nb = 99.0f;
    for (int i = 1; i <= 2 * S45S_NR; i++) {
        float u = (float) i / (float) (2 * S45S_NR), a = a0 + da * u, r =
            R0 + (R1 - R0) * u, qx = px + __builtin_sinf(a) * r, qz =
            pz + __builtin_cosf(a) * r;
        float d =
            __builtin_sqrtf((qx - ox) * (qx - ox) + (qz - oz) * (qz - oz));
        if (d < nb)
            nb = d;
    }
    g_s46.da[v] = da;
    g_s46.nb[v] = nb;
    g_s46.ok[v] = nb >= S45S_BODY;
    g_s46.bl[v] = -1;
    for (int i = 0; i < S45S_NR; i++)
        g_s46.job[v][i] = 0;
    if (!g_s46.ok[v])
        return;
    for (int i = 1; i <= S45S_NR; i++) {
        float u = (float) i / (float) S45S_NR, a = a0 + da * u, r =
            R0 + (R1 - R0) * u;
        float Q1[3] =
            { px + __builtin_sinf(a) * r, cp[1] + dup * u,
      pz + __builtin_cosf(a) * r };
        g_s46.job[v][i - 1] = WorldRayKickM(Q0, Q1, g_spmPh, 0x1b07);
        memcpy(Q0, Q1, sizeof Q0);
    }
}

// main thread, s43q_tick when the plan is made (g_s43e.az / R / up / P / E set): 1 = the path rays are out (the answer comes next tick), 0 = answered now
static int S46Kick(uint64_t ms)
{
    float cp[3], cf[3], px = g_s43e.P[0], pz = g_s43e.P[2], ox =
        g_s43e.E[0], oz = g_s43e.E[1];
    S46Drain();
    g_s46.pick = -1;
    g_s46.ms = ms;
    g_s46.ok[0] = g_s46.ok[1] = 0;
    g_s46.bl[0] = g_s46.bl[1] = -1;
    g_s46.da[0] = g_s46.da[1] = 0.0f;
    if (!RcamGet(cp, cf) || !XvFin3(cp)) {
        g_s46.why = 1;
        return 0;
    }
    float R0 =
        __builtin_sqrtf((cp[0] - px) * (cp[0] - px) +
                        (cp[2] - pz) * (cp[2] - pz)), dy =
        cp[1] - (g_s43e.P[1] + SPM_CAM_UP);
    g_s46.R0 = R0;
    g_s46.dy = dy;
    if (!(R0 >= S46_RMIN && R0 <= S46_RMAX)
        || !(__builtin_fabsf(dy) < S46_DY)) {
        g_s46.why = 2;
        return 0;
    }
    float a0 = __builtin_atan2f(cp[0] - px, cp[2] - pz), R1 =
        g_s43e.R, dup = g_s43e.P[1] + SPM_CAM_UP + g_s43e.up - cp[1];
    if (!(dup == dup) || __builtin_fabsf(dup) > S46_DY)
        dup = 0.0f;
    float da0 = __builtin_remainderf(g_s43e.az - a0, 6.2831853f), da1 =
        da0 > 0.0f ? da0 - 6.2831853f : da0 + 6.2831853f;
    if (!(da0 == da0)) {
        g_s46.why = 2;
        return 0;
    }
    g_s46.a0 = a0;
    S46Arc(0, cp, px, pz, a0, da0, R0, R1, dup, ox, oz);
    if (__builtin_fabsf(da1) <= S45M_MAXARC)
        S46Arc(1, cp, px, pz, a0, da1, R0, R1, dup, ox, oz);
    else {
        g_s46.da[1] = da1;
        g_s46.nb[1] = 0.0f;
        g_s46.ok[1] = 0;
        g_s46.bl[1] = -1;
    }
    if (!g_s46.ok[0] && !g_s46.ok[1]) {
        g_s46.why = 4;
        return 0;
    }
    g_s46.st = 1;
    return 1;
}

static void S46Read(void)
{                               // main thread, one tick after s46_kick: the way round the engage ease takes (g_s46.pick), or why it cannot
    int pick = -1;
    for (int v = 0; v < 2; v++)
        for (int i = 0; i < S45S_NR; i++) {
            uint64_t *j = &g_s46.job[v][i];
            if (*j) {
                g_rayErr = 0;
                float t = RayT(*j);
                *j = 0;
                if (g_s46.bl[v] < 0
                    && (g_rayErr || (t >= 0.0f && t < 0.97f)))
                    g_s46.bl[v] = i;
            }
        }
    g_rayErr = 0;
    g_s46.st = 0;
    for (int v = 0; v < 2 && pick < 0; v++)
        if (g_s46.ok[v] && g_s46.bl[v] < 0)
            pick = v;
    if (pick >= 0) {
        float da = g_s46.da[pick], x = __builtin_fabsf(da) / 3.14159265f;
        g_s46.pda = da;
        g_s46.pT = S46_T0 + (S46_T1 - S46_T0) * (x > 1.0f ? 1.0f : x);
        g_s46.pnb = g_s46.nb[pick];
        g_s46.why = 0;
    } else
        g_s46.why = 3;
    MemoryBarrier();
    g_s46.pick = pick;
}

// ps46 V1 (clip 17-54-12 right half, B = the victim of A's mf-switchblade-quick-left-ice-pick: B's camera swings down into the ground / grass
//   ~7.37-7.43 s; B.log 114553-114586): the lens owner is pincam5 ps34 (s34_apply), the ps44 vicmove way in, 'arc 123 deg round the pair, 475 ms'.
//   The lens orbits the pair, but its rotation was slerp(start view, look from the moving lens, s): the start view keeps pointing where it pointed
//   from the START position, so half way round the view sits ~half the arc off the pair - vistrace 114572 lens 104.76 0.50 287.85 dir 0.96 -0.24 -0.16
//   looks 28 deg off the pair's midpoint 106.85 288.55 (61 deg of the arc done), clip 7.37-7.43 s grass / empty ground, the attacker at the frame edge,
//   with the start view's -17 deg pitch while the lens drops 0.47 m. Now the way in turns the start view WITH the arc (yaw by the angle done round
//   the pair) before the slerp onto the look: the pair keeps its place in the frame through the swing. t2r-ps46-cam-vicarc-old.txt (re-read every
//   2 s) = the ps45 slerp. Proof 'pincam5: ps46 V1'.
static volatile int g_ps46v = 1;
static volatile LONG g_s46vN;
static volatile float g_s46vDeg;       // render -> main: ways in that turned with their arc, the last one's arc
static void Ps46Cfg(uint64_t now)
{                               // main thread (spm_tick): the opt-out + the proof lines of the render thread's eases / cuts
    static uint64_t at;
    static int first = 1, nl;
    static LONG ln, lc;
    {
        static uint64_t at2;
        static int f2 = 1, nl2;
        static LONG lr, lf, lv; // ps46 R1 (t2r-ext.c, the note at g_ps46r) / V1 (the note at g_ps46v)
        if (f2 || now - at2 >= 2000) {
            at2 = now;
            int v =
                GetFileAttributesA("t2r-ps46-cam-attrel-old.txt") ==
                INVALID_FILE_ATTRIBUTES;
            if (v != g_ps46r || f2)
                Xlog("xcam: ps46 R1 t2r-ps46-cam-attrel-old.txt %s",
                     v ?
                     "absent - a contextual takedown's attacker hand-back keeps the authored track playing under it (no frozen release frame): ON"
                     : "present - the OLD (ps45) frozen cine end");
            g_ps46r = v;
            v = GetFileAttributesA("t2r-ps46-cam-vicarc-old.txt") ==
                INVALID_FILE_ATTRIBUTES;
            if (v != g_ps46v || f2)
                Xlog("pincam5: ps46 V1 t2r-ps46-cam-vicarc-old.txt %s",
                     v ?
                     "absent - the victim side shot's way in turns its view with the arc round the pair: ON"
                     :
                     "present - the OLD (ps45) slerp from the start view");
            g_ps46v = v;
            f2 = 0;
        }
        LONG r = g_s46r_n, f = g_s46r_f, vn = g_s46vN;
        if (r != lr && f != lf) {
            lr = r;
            lf = f;
            if (LOGCAP(nl2, 200))
                Xlog("xcam: ps46 R1 - %ld attacker hand-back(s) with the authored track playing on, %ld frame(s) of it so far (frame %llu)", (long) r, (long) f, (unsigned long long) g_frame);
        }
        if (vn != lv) {
            lv = vn;
            if (LOGCAP(nl2, 200))
                Xlog("pincam5: ps46 V1 - the victim side shot's way in turned its view with the arc (%.0f deg round the pair; %ld so far; frame %llu)", (double) g_s46vDeg, (long) vn, (unsigned long long) g_frame);
        }
    }
    if (first || now - at >= 2000) {
        at = now;
        int v =
            GetFileAttributesA("t2r-ps46-cam-engage-old.txt") ==
            INVALID_FILE_ATTRIBUTES;
        if (v != g_ps46 || first) {
            if (v)
                Xlog("spmelee cam: ps46 t2r-ps46-cam-engage-old.txt absent - a duel engage EASES the lens onto the rig's spot (%.0f-%.0f ms smoothstep round our pivot, ray-tested path; blocked / stale lens / real teleport = the ONE cut), the ps37 teleport watch needs two samples <= %d ms apart: ON", (double) (S46_T0 * 1000.0f), (double) (S46_T1 * 1000.0f), S46_TP_FRESH);
            else
                Xlog("spmelee cam: ps46 t2r-ps46-cam-engage-old.txt present - the OLD (ps45) engage: ONE cut, the ps37 watch on any two samples");
        }
        g_ps46 = v;
        first = 0;
    }
    LONG n = g_s46.n, c = g_s46.nc;
    if (n != ln) {
        ln = n;
        if (LOGCAP(nl, 200))
            Xlog("spmelee cam: ps46 engage - EASED onto the rig's spot, no cut: %.0f deg round our pivot over %.0f ms (smoothstep), radius %.2f -> %.2f m, path clear (%d legs, the partner's root %.2f m off at the closest; ccg / cam2 pass it through; eases %ld / cuts %ld so far; frame %llu)", (double) g_s46.cdeg, (double) (g_s46.cT * 1000.0f), (double) g_s46.cR0, (double) g_s46.cR1, S45S_NR, (double) g_s46.cnb, (long) n, (long) c, (unsigned long long) g_frame);
    }
    if (c != lc) {
        lc = c;
        int w = g_s46.cwhy;
        if (LOGCAP(nl, 200))
            Xlog("spmelee cam: ps46 engage - ONE CUT kept (fallback): %s (the short way %.0f deg: %s, the other way %.0f deg: %s; shown lens %.2f m from our pivot, %+.2f m off the lens height; eases %ld / cuts %ld so far; frame %llu)", w >= 0 && w <= 8 ? S46_WHY[w] : "?", (double) (__builtin_fabsf(g_s46.da[0]) * 57.29578f), g_s46.ok[0] ? (g_s46.bl[0] >= 0 ? "geometry on its path" : "clear") : "passes near the partner / not tested", (double) (__builtin_fabsf(g_s46.da[1]) * 57.29578f), __builtin_fabsf(g_s46.da[1]) > S45M_MAXARC ? "longer than 200 deg" : g_s46.ok[1] ? (g_s46.bl[1] >= 0 ? "geometry on its path" : "clear") : "passes near the partner / not tested", (double) g_s46.R0, (double) g_s46.dy, (long) n, (long) c, (unsigned long long) g_frame);
    }
}

// main thread, the top of every spm_tick: the opt-out, our root's own teleport watch (spm_tick's ps37 watch only runs inside the melee window: rec42a A
// 16153 was not seen by it), the proof line of a cut taken on the render thread
static void S43qTrack(uint64_t now)
{
    static uint64_t at;
    static int first = 1;
    static LONG lc;
    static int nl;
    if (first || now - at >= 2000) {
        at = now;
        int v =
            GetFileAttributesA("t2r-ps43-cam-engdest-old.txt") ==
            INVALID_FILE_ATTRIBUTES;
        if (v != g_ps43q || first)
            Xlog("spmelee cam: ps43 t2r-ps43-cam-engdest-old.txt %s",
                 v ?
                 "absent - the engage cut's spot is planned from the two live roots (radius / partner axis / midpoint / geometry ray; always cut after a teleport; one cut per engage): ON"
                 :
                 "present - the OLD (ps42) engage cut (azimuth only, the incoming lens's radius)");
        g_ps43q = v;
        first = 0;
    }
    {
        float b[16];
        const float *l = PlayerLocator(b);
        if (l && __builtin_isfinite(l[0]) && __builtin_isfinite(l[2])) {
            float dx = l[0] - g_s43e.tx, dz = l[2] - g_s43e.tz;
            if (g_s43e.t_ok && dx * dx + dz * dz > 9.0f)
                g_s43e.tp_ms = now;
            g_s43e.tx = l[0];
            g_s43e.tz = l[2];
            g_s43e.t_ok = 1;
        } else
            g_s43e.t_ok = 0;
    }
    if ((g_s43e.st == 1 || g_s43e.st == 3 || g_s43e.st == 4)
        && now - g_s43e.tick_ms > 150) {
        S43qDrain();
        g_s43e.st = 0;
    }                           // spm_tick left before the plan's turn (framing off; ps46: 4 = the engage ease's path rays out)
    {                           // ps43 Q2 (t2r-ext.c, the note at g_ps43x): the victim's execution camera - opt-out, the render thread's flag, the proof lines
        static uint64_t at2;
        static int first2 = 1, nl2, nl3;
        static LONG lc2, lt2;
        static uint64_t tt2;
        if (first2 || now - at2 >= 2000) {
            at2 = now;
            int v =
                GetFileAttributesA("t2r-ps43-cam-vicexec-old.txt") ==
                INVALID_FILE_ATTRIBUTES;
            if (v != g_ps43x || first2)
                Xlog("xcam victim: ps43 t2r-ps43-cam-vicexec-old.txt %s",
                     v ?
                     "absent - an execution on our screen as the victim: camera choice in ~3 ticks (thinned rays), its first shown frame is ONE cut, a straight-down shot keeps the authored heading + roll: ON"
                     : "present - the OLD (ps42) behaviour");
            g_ps43x = v;
            first2 = 0;
        }
        g_s43x_vic = g_ps43x && g_xcamOn && g_xcam_role == 2
            && S40ExecCam();
        LONG c2 = g_s43x_cuts;
        if (c2 != lc2) {
            lc2 = c2;
            if (LOGCAP(nl2, 120))
                Xlog("xcam victim: ps43 - execution camera %s taken on ONE cut on its first shown frame (the victim's screen: no 450 ms blend-in from the downed camera; cut #%ld, frame %llu)", g_xcamName, (long) c2, (unsigned long long) g_frame);
        }
        LONG t2 = g_s43x_td;
        if (t2 != lt2 && now - tt2 > 2000) {
            tt2 = now;
            if (LOGCAP(nl3, 120))
                Xlog("xcam victim: ps43 - %s looks straight down: the hold's heading is the authored heading + roll (slewed at <= %.0f deg/s, roll taken out) on %ld tick(s) so far (+%ld; frame %llu)", g_xcamName, (double) (S43X_TD_RATE * 57.29578f), (long) t2, (long) (t2 - lt2), (unsigned long long) g_frame);
            lt2 = t2;
        }
    }
    LONG c = g_s43e.cuts;
    if (c != lc) {
        lc = c;
        int fb = g_s43e.c_fb;
        if (LOGCAP(nl, 200)) {
            if (fb < 0)
                Xlog("spmelee cam: ps43 engage cut destination - NO PLAN was ready (the two roots closer than 0.3 m / no ray): the rig's own azimuth at SP's distance, floored at %.2f m (the incoming lens stood %.2f m from our pivot, %.0f deg off the spot - not used; cut %ld, frame %llu)", (double) S43Q_RMIN, (double) g_s43e.c_rl, (double) (g_s43e.c_e * 57.29578f), (long) c, (unsigned long long) g_frame);
            else
                Xlog("spmelee cam: ps43 engage cut destination - from the two live roots (%.2f m apart): %.2f m beside (%s) / %.2f m behind the line, radius %.2f m from our axis (floor %.2f), %.2f m from the partner's axis (min %.2f), %.2f m on our side of the midpoint, %+.2f m up; fallback %d = %s; clear arms kept %.2f / other %.2f / raised %.2f m%s; %s; the incoming lens stood %.2f m from our pivot, %.0f deg off the spot - not used; lens-guard pull-ins dropped on cuts so far %ld (cut %ld, frame %llu)", (double) g_s43e.d, (double) __builtin_fabsf(g_s43e.beside), g_s43e.side >= 0.0f ? "RIGHT" : "LEFT", (double) g_s43e.behind, (double) g_s43e.R, (double) S43Q_RMIN, (double) g_s43e.pax, (double) S43Q_PAX, (double) g_s43e.mid, (double) g_s43e.up, fb, fb == 0 ? "SP's spot on the kept / nearer shoulder" : fb == 1 ? "the kept shoulder, arm shortened to the clear length" : fb == 2 ? "the OTHER shoulder (the kept one cannot keep the floor)" : fb == 3 ? "straight behind, RAISED (neither shoulder keeps the floor)" : "NO spot keeps the floor: the longest arm at the floor, the lens guard decides", (double) g_s43e.al[0], (double) g_s43e.al[1], (double) g_s43e.al[2], g_s43e.rerr ? " (a ray gave no answer: taken as clear)" : "", g_s43e.c_tp ? "our root was teleported just before: the engine lens is stale, the cut is taken whatever its angle" : "the lens stood > 60 deg off the spot / on the partner's side", (double) g_s43e.c_rl, (double) (g_s43e.c_e * 57.29578f), (long) g_s43e.clg, (long) c, (unsigned long long) g_frame);
        }
    }
}

static void S43qCand(int i, float az, float R, float up)
{                               // one candidate's ray: our head (0.3 m out) -> its spot + the margin
    float sx = __builtin_sinf(az), sz = __builtin_cosf(az), y =
        g_s43e.P[1] + SPM_CAM_UP;
    float H[3] =
        { g_s43e.P[0] + sx * 0.3f, y, g_s43e.P[2] + sz * 0.3f }, S[3] =
        { g_s43e.P[0] + sx * (R + SPM_CAM_MARGIN), y + up,
   g_s43e.P[2] + sz * (R + SPM_CAM_MARGIN) };
    g_s43e.caz[i] = az;
    g_s43e.jl[i] = R + SPM_CAM_MARGIN - 0.3f;
    g_s43e.job[i] = WorldRayKickM(H, S, 0, 0x1b07);
}

// main thread, spm_tick where the look-at target is set: yt = the rig is latched on a partner. 1 = the plan is not there yet: the rig waits (look-at weight 0)
static int S43qTick(uint64_t now, int yt, int have, float px, float py,
                     float pz, float ex, float ez, int tp37)
{
    int was = g_s43e.yt_was && now - g_s43e.tick_ms < 150;
    g_s43e.yt_was = yt;
    g_s43e.tick_ms = now;
    if (tp37)
        g_s43e.tp_ms = now;
    if (!g_ps43q || !yt || !have) {
        if (g_s43e.st == 1 || g_s43e.st == 3 || g_s43e.st == 4) {
            S43qDrain();
            g_s43e.st = 0;
        }
        return 0;
    }
    if (!was && g_s43e.st != 1 && g_s43e.st != 3 && g_s43e.st != 4
        && g_spm.yw < 0.02f)
        g_s43e.st = 3;          // the rig latches from nothing: plan its cut
    if (g_s43e.st == 3) {
        if (g_s43e.tp_ms == now)
            return 1;           // our root was teleported on THIS tick: the plan is made on the next one, from fresh positions
        float fx = ex - px, fz = ez - pz, d =
            __builtin_sqrtf(fx * fx + fz * fz);
        if (!(d == d) || !(d > 0.3f) || !(d < 12.0f)) {
            g_s43e.st = 0;
            return 0;
        }
        fx /= d;
        fz /= d;
        int stale = g_s43e.tp_ms && now - g_s43e.tp_ms < S43Q_TP_MS;
        float side = 1.0f, cp[3], cf[3];
        if (stale) {
            if (g_s43e.plan_ms && now - g_s43e.plan_ms < 5000)
                side = g_s43e.cside;
        }                       // (a stale lens has no side: the last plan's, else SP's usual right)
        else if (RcamGet(cp, cf))
            side = ((cp[0] - px) * -fz + (cp[2] - pz) * fx) >= -SPM25_SIDE_DEAD ? 1.0f : -1.0f; // the nearer shoulder (the rig's own rule) - the lens's SIDE only
        float R = g_spm.bt > 0.5f && g_spm.bt < 2.0f
            && !(g_s32.p > 0.0f) ? g_spm.bt : SPM25_DIST;
        if (R < S43Q_RMIN)
            R = S43Q_RMIN;
        g_s43e.P[0] = px;
        g_s43e.P[1] = py;
        g_s43e.P[2] = pz;
        g_s43e.E[0] = ex;
        g_s43e.E[1] = ez;
        g_s43e.f[0] = fx;
        g_s43e.f[1] = fz;
        g_s43e.d = d;
        g_s43e.R0 = R;
        g_s43e.cside = side;
        float ab = __builtin_atan2f(-fx, -fz);
        S43qCand(0, ab + side * SPM25_AZ, R, 0.0f);
        S43qCand(1, ab - side * SPM25_AZ, R, 0.0f);
        S43qCand(2, ab, R, S43Q_UP);
        g_s43e.st = 1;
        return 1;
    }
    if (g_s43e.st == 1) {
        float al[3];
        int rerr = 0;
        for (int i = 0; i < 3; i++) {
            al[i] = g_s43e.R0;
            if (!g_s43e.job[i]) {
                rerr = 1;
                continue;
            }
            g_rayErr = 0;
            float t = RayT(g_s43e.job[i]);
            g_s43e.job[i] = 0;
            if (g_rayErr) {
                rerr = 1;
                continue;
            }
            if (t >= 0.0f && t < 1.0f) {
                float hd = 0.3f + t * g_s43e.jl[i], hx =
                    g_s43e.P[0] + __builtin_sinf(g_s43e.caz[i]) * hd -
                    g_s43e.E[0], hz =
                    g_s43e.P[2] + __builtin_cosf(g_s43e.caz[i]) * hd -
                    g_s43e.E[1];
                if (hx * hx + hz * hz < 0.45f * 0.45f)
                    continue;   // (the partner's body is not a wall: spm_tick's head-to-lens ray, v5-cam4)
                float a = hd - SPM_CAM_MARGIN;
                if (a < al[i])
                    al[i] = a;
            }
        }
        g_rayErr = 0;
        int fb, c;
        float R = g_s43e.R0, up = 0.0f, side = g_s43e.cside;
        if (al[0] >= S43Q_RMIN) {
            c = 0;
            fb = al[0] < R - 0.01f;
            R = al[0] < R ? al[0] : R;
        } else if (al[1] >= S43Q_RMIN) {
            c = 1;
            fb = 2;
            side = -side;
            R = al[1] < R ? al[1] : R;
        } else if (al[2] >= S43Q_RMIN) {
            c = 2;
            fb = 3;
            up = S43Q_UP;
            R = al[2] < R ? al[2] : R;
        } else {
            c = al[0] >= al[1]
                && al[0] >= al[2] ? 0 : al[1] >= al[2] ? 1 : 2;
            fb = 4;
            if (c == 1)
                side = -side;
            if (c == 2)
                up = S43Q_UP;
            R = S43Q_RMIN;
        }
        float az = g_s43e.caz[c], lx = __builtin_sinf(az) * R, lz =
            __builtin_cosf(az) * R, fx = g_s43e.f[0], fz = g_s43e.f[1];
        float qx = g_s43e.P[0] + lx - g_s43e.E[0], qz =
            g_s43e.P[2] + lz - g_s43e.E[1], pax =
            __builtin_sqrtf(qx * qx + qz * qz), behind =
            -(lx * fx + lz * fz);
        if (pax < S43Q_PAX || behind + 0.5f * g_s43e.d <= 0.0f) {       // (cannot happen for a spot behind us; kept as the rule's own check: straight behind at the floor)
            az = __builtin_atan2f(-fx, -fz);
            lx = __builtin_sinf(az) * R;
            lz = __builtin_cosf(az) * R;
            behind = R;
            pax = R + g_s43e.d;
            fb = 4;
        }
        g_s43e.beside = lx * -fz + lz * fx;
        g_s43e.behind = behind;
        g_s43e.pax = pax;
        g_s43e.mid = behind + 0.5f * g_s43e.d;
        g_s43e.rerr = rerr;
        for (int i = 0; i < 3; i++)
            g_s43e.al[i] = al[i];
        g_s43e.az = az;
        g_s43e.R = R;
        g_s43e.up = up;
        g_s43e.side = side;
        g_s43e.fb = fb;
        g_s43e.cside = side;
        g_s43e.tp = g_s43e.tp_ms && now - g_s43e.tp_ms < S43Q_TP_MS;
        g_s43e.plan_ms = now;
        if (g_ps46) {
            if (S46Kick(now)) {
                g_s43e.st = 4;
                return 1;
            }
        }                       // ps46 E1: the engage ease's path first (one more tick)
        else {
            g_s46.pick = -1;
            g_s46.ms = 0;
        }
        g_s43e.ms = now;
        MemoryBarrier();
        g_s43e.st = 2;
        return 0;
    }
    if (g_s43e.st == 4) {       // ps46 E1: the path answers; the plan goes out with it
        S46Read();
        g_s46.ms = now;
        g_s43e.ms = now;
        MemoryBarrier();
        g_s43e.st = 2;
        return 0;
    }
    return 0;
}

#define S43_NS 5
#define S43_NC 12
#define S43_MIN_AX 0.75f        // m [TUNING ps43]: never closer than 0.6 m to either body's axis, + 0.15 for the travel between two samples
#define S43_CAP 0.45f           // m: a body's capsule with its backpack
#define S43_WALL 0.50f          // m: the lens in front of a wall takedown's plane
#define S43_PIV 0.70f           // m [TUNING ps43]: a spot's ray starts this far from M (outside the two bodies: the ray ignores only one of them)
static const char *PulPairName(uint64_t move);        // ctxdown.c
static struct {
    int st, view, clip, ns, best, pass, wok;
    uint64_t t0;                // st: 0 none, 1 rays kicked, 2 planned, -1 no spot; view: 1 the victim's screen, 2 the attacker's; t0: the owner's start (its identity)
    float A[S43_NS][3], V[S43_NS][3], Vf[S43_NS][2], M[3], f[2], D, up,
        wp[2], wn[2];
    float C[S43_NC][3], reach[S43_NC], E[3];
    uint64_t job[S43_NC];
    int nlog;
} g_s43;
// ps44 R5a (B 11396 'the victim's finisher side shot takes the PLANNED lens 99.82 0.19 289.05'; clip B 12.67: a pillar over the right 35 % of the frame,
// both fighters small and dark): a spot's one ray ran from near the pair's midpoint OUT to the spot at lens height - a pillar beside that line, inside the
// frame, was never tested. Now each spot also casts lens -> the victim's head and chest and lens -> the attacker's head (the middle sample's placement,
// each stopping 0.35 m short of its target; a stop = the subject hidden) and two rays S44P_GRAZE either side of the view axis (a stop within S44P_NEAR =
// geometry in the near frame); each costs the spot's score (S44P_SUBJ / S44P_GRZ). t2r-ps44-cam-plan-old.txt = the ps43 single ray.
#define S44P_GRAZE 0.2618f      // rad = 15 deg
#define S44P_NEAR 1.60f         // m [TUNING ps44]
#define S44P_SUBJ 0.90f         // score per hidden subject [TUNING ps44]
#define S44P_GRZ 0.60f          // score per near graze [TUNING ps44]
static uint64_t g_s44pJob[S43_NC][5];
static float g_s44pLen[S43_NC][5];
static int g_s44pHit[S43_NC][5], g_ps44p = 1;
static void S44pDrain(void)
{
    for (int i = 0; i < S43_NC; i++)
        for (int k = 0; k < 5; k++)
            if (g_s44pJob[i][k]) {
                RayT(g_s44pJob[i][k]);
                g_s44pJob[i][k] = 0;
            }
}

static void S43Reset(void)
{
    for (int i = 0; i < S43_NC; i++)
        if (g_s43.job[i]) {
            RayT(g_s43.job[i]);
            g_s43.job[i] = 0;
        }
    S44pDrain();
    g_s43.st = 0;
}

static void S43Samples(int view, const float *me, const float *ot,
                        int use_clip)
{
    const float *Al = view == 2 ? me : ot, *Vl = view == 2 ? ot : me;
    float lmx = 0.5f * (Al[0] + Vl[0]), lmz = 0.5f * (Al[2] + Vl[2]);
    int n = 0;
    g_s43.clip = 0;
    if (use_clip && g_ps43[PS43_CLIP] && PsyncOwn()
        && !PulPairName(g_ps.move)) {
        int ls = g_ps.nseg > 1
            && g_ps.nseg <= PS_MAXSEG ? g_ps.nseg - 1 : -1;
        uint64_t sa = ls >= 0 ? g_ps.seg_att[ls] : g_ps.att_sid, Sv =
            ls >= 0 ? g_ps.seg_vic[ls] : g_ps.vic_sid;
        float sp = ls >= 0 ? g_ps.seg_spd[ls] : g_ps.speed;
        if (!(sp > 0.2f && sp < 5.0f))
            sp = 1.0f;
        float da = sa ? PsyncDur(SID_PLAYER, sa) : 0.0f, dv =
            Sv ? PsyncDur(SID_PLAYER, Sv) : 0.0f;
        static const float T1[S43_NS] = { 0.15f, 0.50f, 0.90f, 1.40f, 2.00f };  // s into the finisher clip [TUNING ps43]
        static const float T2[S43_NS] = { 0.40f, 1.00f, 1.70f, 2.50f, 3.30f };  // S44_TX: s into an execution's clips [TUNING ps44] (use_clip 2)
        const float *T = use_clip == 2 ? T2 : T1;
        for (int j = 0; j < S43_NS && da > 0.2f && dv > 0.2f; j++) {
            float pa = T[j] * sp / da, pv =
                T[j] * sp / dv, o1[2][8], o2[2][8];
            pa = pa > 1.0f ? 1.0f : pa;
            pv = pv > 1.0f ? 1.0f : pv;
            if (!(PsyncPairEnd(g_ps.ap, sa, Sv, pa, 0, o1) & 1)
                || !(PsyncPairEnd(g_ps.ap, sa, Sv, pv, 0, o2) & 2))
                continue;
            const float *a = o1[0], *v = o2[1];
            if (!XvFin3(a) || !XvFin3(v))
                continue;
            if ((a[0] - lmx) * (a[0] - lmx) + (a[2] - lmz) * (a[2] - lmz) >
                9.0f
                || (v[0] - lmx) * (v[0] - lmx) + (v[2] - lmz) * (v[2] -
                                                                 lmz) >
                9.0f)
                continue;       // (not this pair's frame)
            memcpy(g_s43.A[n], a, 12);
            memcpy(g_s43.V[n], v, 12);
            g_s43.Vf[n][0] = 2.0f * (v[4] * v[6] + v[7] * v[5]);
            g_s43.Vf[n][1] = 1.0f - 2.0f * (v[4] * v[4] + v[5] * v[5]);
            n++;
        }
        if (n >= 2)
            g_s43.clip = 1;
        else
            n = 0;
    }
    if (!n) {
        memcpy(g_s43.A[0], Al, 12);
        memcpy(g_s43.V[0], Vl, 12);
        g_s43.Vf[0][0] = 2.0f * (Vl[4] * Vl[6] + Vl[7] * Vl[5]);
        g_s43.Vf[0][1] = 1.0f - 2.0f * (Vl[4] * Vl[4] + Vl[5] * Vl[5]);
        n = 1;
    }
    g_s43.ns = n;
}

// the samples, the pair frame, the spots and their rays (ign: the other body's handle - the rays test geometry)
static void S43Build(int view, const float *me, const float *ot,
                      uint64_t ign, int wok, const float *wp,
                      const float *wn, float dmin, float dmax, float dpre,
                      float up, int use_clip)
{
    int keep_view = g_s43.view;
    uint64_t keep_t0 = g_s43.t0;
    S43Reset();
    g_s43.view = keep_view;
    g_s43.t0 = keep_t0;
    S43Samples(view, me, ot, use_clip);
    if (!g_s43.clip && wok && wp && wn) {
        // a wall takedown whose clips cannot be read: the roots of the first ticks are where the DUEL left the pair, not where the takedown puts it. Two
        // stand-in placements on the wall frame [TUNING ps43, from rec42a B 11481-11552 wall-throat-punch: the victim 0.68 -> 0.24 m off the anchor, the
        // attacker 1.39 -> 0.59 m, both within 0.3 m of the anchor along the wall]
        static const float SV[2] = { 0.65f, 0.30f }, SA[2] =
            { 1.40f, 0.75f };
        float y = 0.5f * (me[1] + ot[1]);
        for (int j = 0; j < 2; j++) {
            g_s43.V[j][0] = wp[0] + wn[0] * SV[j];
            g_s43.V[j][1] = y;
            g_s43.V[j][2] = wp[1] + wn[1] * SV[j];
            g_s43.A[j][0] = wp[0] + wn[0] * SA[j];
            g_s43.A[j][1] = y;
            g_s43.A[j][2] = wp[1] + wn[1] * SA[j];
            g_s43.Vf[j][0] = wn[0];
            g_s43.Vf[j][1] = wn[1];
        }
        g_s43.ns = 2;
        if (LOGCAP(g_s43.nlog, 300))
            Xlog("paircam: ps43 plan - the wall takedown's clips are not readable here: the 'live-root' samples of the next line are 2 STAND-IN placements on the wall frame (victim 0.65 / 0.30 m, attacker 1.40 / 0.75 m off the plane at %.2f %.2f; frame %llu)", (double) wp[0], (double) wp[1], (unsigned long long) g_frame);
    }
    int n = g_s43.ns;
    float mx = 0.0f, my = 0.0f, mz = 0.0f, fx = 0.0f, fz = 0.0f;
    for (int j = 0; j < n; j++) {
        mx += 0.5f * (g_s43.A[j][0] + g_s43.V[j][0]);
        my += 0.5f * (g_s43.A[j][1] + g_s43.V[j][1]);
        mz += 0.5f * (g_s43.A[j][2] + g_s43.V[j][2]);
        fx += g_s43.V[j][0] - g_s43.A[j][0];
        fz += g_s43.V[j][2] - g_s43.A[j][2];
    }
    mx /= (float) n;
    my /= (float) n;
    mz /= (float) n;
    float fl = __builtin_sqrtf(fx * fx + fz * fz);
    if (!(fl > 0.2f * (float) n)) {
        const float *Al = view == 2 ? me : ot, *Vl = view == 2 ? ot : me;
        fx = Vl[0] - Al[0];
        fz = Vl[2] - Al[2];
        fl = __builtin_sqrtf(fx * fx + fz * fz);
    }
    if (!(fl > 0.05f)) {
        fx = wok ? -wn[0] : 0.0f;
        fz = wok ? -wn[1] : 1.0f;
        fl = __builtin_sqrtf(fx * fx + fz * fz);
        if (!(fl > 0.05f)) {
            fx = 0.0f;
            fz = 1.0f;
            fl = 1.0f;
        }
    }
    fx /= fl;
    fz /= fl;
    float ext = 0.0f;
    for (int j = 0; j < n; j++) {
        float da =
            __builtin_sqrtf((g_s43.A[j][0] - mx) * (g_s43.A[j][0] - mx) +
                            (g_s43.A[j][2] - mz) * (g_s43.A[j][2] - mz)),
            dv =
            __builtin_sqrtf((g_s43.V[j][0] - mx) * (g_s43.V[j][0] - mx) +
                            (g_s43.V[j][2] - mz) * (g_s43.V[j][2] - mz));
        if (da > ext)
            ext = da;
        if (dv > ext)
            ext = dv;
    }
    float D = (ext + 0.55f) / 0.61f;    // both bodies + 0.55 m inside 80 % of the 75 fov's half width
    if (dpre > D)
        D = dpre;
    D = D < dmin ? dmin : D > dmax ? dmax : D;
    g_s43.M[0] = mx;
    g_s43.M[1] = my;
    g_s43.M[2] = mz;
    g_s43.f[0] = fx;
    g_s43.f[1] = fz;
    g_s43.D = D;
    g_s43.up = up;
    g_s43.wok = wok && wp && wn;
    if (g_s43.wok) {
        g_s43.wp[0] = wp[0];
        g_s43.wp[1] = wp[1];
        g_s43.wn[0] = wn[0];
        g_s43.wn[1] = wn[1];
    }
    float az0 = __builtin_atan2f(fx, fz);
    for (int i = 0; i < S43_NC; i++) {
        float az = az0 + (float) i * (6.2831853f / (float) S43_NC), sx =
            __builtin_sinf(az), sz = __builtin_cosf(az);
        g_s43.C[i][0] = mx + sx * D;
        g_s43.C[i][1] = my + up;
        g_s43.C[i][2] = mz + sz * D;
        float P[3] =
            { mx + sx * S43_PIV, my + up - 0.05f, mz + sz * S43_PIV },
            Q[3] =
            { mx + sx * (D + 0.25f), my + up, mz + sz * (D + 0.25f) };
        g_s43.reach[i] = D + 0.25f;
        g_s43.job[i] = WorldRayKickM(P, Q, ign, 0x1b07);
    }
    {
        static uint64_t at;
        uint64_t t9 = GetTickCount64(); // ps44 R5a (the note at S44P_*)
        if (!at || t9 - at >= 2000) {
            at = t9;
            int v =
                GetFileAttributesA("t2r-ps44-cam-plan-old.txt") ==
                INVALID_FILE_ATTRIBUTES;
            if (v != g_ps44p)
                Xlog("paircam: ps44 t2r-ps44-cam-plan-old.txt %s",
                     v ?
                     "absent - each two-shot spot also tests lens -> both heads + the victim's chest and the near frame either side of the view: ON"
                     : "present - the ps43 single ray");
            g_ps44p = v;
        }
    }
    S44pDrain();
    memset(g_s44pHit, 0, sizeof g_s44pHit);
    if (g_ps44p && n > 0) {
        const float *A = g_s43.A[n / 2], *V = g_s43.V[n / 2];
        float T[3][3] =
            { { V[0], V[1] + 1.45f, V[2] }, { V[0], V[1] + 1.10f, V[2] },
            { A[0], A[1] + 1.55f, A[2] } };
        if (view == 1 && me) {  // (our own body on our screen: its live head / chest when the sample stands where we are)
            float p9[3], c9[3], h9[3];
            XvBody(SID_PLAYER, me, p9, c9, h9);
            if ((V[0] - me[0]) * (V[0] - me[0]) +
                (V[2] - me[2]) * (V[2] - me[2]) < 0.25f && XvFin3(h9)
                && XvFin3(c9)) {
                memcpy(T[0], h9, 12);
                memcpy(T[1], c9, 12);
            }
        }
        for (int i = 0; i < S43_NC; i++) {
            const float *C = g_s43.C[i];
            for (int k = 0; k < 3; k++) {
                float dx = T[k][0] - C[0], dy = T[k][1] - C[1], dz =
                    T[k][2] - C[2], l =
                    __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
                if (!(l > 0.6f))
                    continue;
                float E[3] =
                    { C[0] + dx / l * (l - 0.35f),
               C[1] + dy / l * (l - 0.35f), C[2] + dz / l * (l - 0.35f) };
                g_s44pLen[i][k] = l - 0.35f;
                g_s44pJob[i][k] = WorldRayKickM(C, E, ign, 0x1b07);
            }
            float vx = mx - C[0], vz = mz - C[2], va =
                __builtin_atan2f(vx, vz);
            for (int k = 3; k < 5; k++) {
                float a = va + (k == 3 ? S44P_GRAZE : -S44P_GRAZE), E[3] =
                    { C[0] + __builtin_sinf(a) * S44P_NEAR, C[1] - 0.10f,
C[2] + __builtin_cosf(a) * S44P_NEAR };
                g_s44pLen[i][k] = S44P_NEAR;
                g_s44pJob[i][k] = WorldRayKickM(C, E, ign, 0x1b07);
            }
        }
    }
    g_s43.st = 1;
}

// the rays' answers + the choice; cur = the shown lens (the attacker's screen: the nearer spot wins a tie) or NULL
static int S43Score(const float *cur)
{
    float D = g_s43.D, mx = g_s43.M[0], mz = g_s43.M[2];
    int n = g_s43.ns, view = g_s43.view;
    for (int i = 0; i < S43_NC; i++) {
        if (!g_s43.job[i])
            continue;
        g_rayErr = 0;
        float t = RayT(g_s43.job[i]);
        g_s43.job[i] = 0;
        if (!g_rayErr && t >= 0.0f && t < 1.0f)
            g_s43.reach[i] = S43_PIV + t * (D + 0.25f - S43_PIV);
        for (int k = 0; k < 5; k++)
            if (g_s44pJob[i][k]) {     // ps44 R5a
                g_rayErr = 0;
                float t5 = RayT(g_s44pJob[i][k]);
                g_s44pJob[i][k] = 0;
                g_s44pHit[i][k] = !g_rayErr && t5 >= 0.0f && t5 < 0.97f;
            }
    }
    int n44s = 0, n44g = 0;
    int best = -1, bpass = 0, nwall = 0, nray = 0, nax = 0;
    float bs = -1e9f, bE[3] = { 0, 0, 0 }, bso = 0.0f, boc = 0.0f, bfr =
        0.0f, bmin = 0.0f;
    for (int pass = 0; pass < 2 && best < 0; pass++) {
        nwall = nray = nax = 0;
        for (int i = 0; i < S43_NC; i++) {
            float C[3] = { g_s43.C[i][0], g_s43.C[i][1], g_s43.C[i][2] };
            if (pass == 0) {
                if (g_s43.reach[i] < D + 0.15f) {
                    nray++;
                    continue;
                }
            } else {            // every spot blocked: this one pulled in front of what blocks it, raised
                if (g_s43.reach[i] < 1.50f) {
                    nray++;
                    continue;
                }
                float dd = g_s43.reach[i] - 0.25f;
                if (dd > D)
                    dd = D;
                C[0] = mx + (C[0] - mx) * dd / D;
                C[2] = mz + (C[2] - mz) * dd / D;
                C[1] += 0.35f;
            }
            if (g_s43.wok
                && (C[0] - g_s43.wp[0]) * g_s43.wn[0] + (C[2] -
                                                         g_s43.wp[1]) *
                g_s43.wn[1] < S43_WALL) {
                nwall++;
                continue;
            }
            if (g_s44BanOn) { // ps44: a re-plan - not the spot that failed nor its neighbours (within 50 deg of it round the midpoint)
                float bx = C[0] - mx, bz = C[2] - mz, bl =
                    __builtin_sqrtf(bx * bx + bz * bz);
                if (bl > 0.2f
                    && (bx * g_s44Ban[0] + bz * g_s44Ban[1]) / bl >
                    0.643f) {
                    nax++;
                    continue;
                }
            }
            float mind = 99.0f, occ = 0.0f, so = 0.0f, frn = 0.0f;
            for (int j = 0; j < n; j++) {
                const float *A = g_s43.A[j], *V = g_s43.V[j];
                float dax = C[0] - A[0], daz = C[2] - A[2], dvx =
                    C[0] - V[0], dvz = C[2] - V[2], da =
                    __builtin_sqrtf(dax * dax + daz * daz), dv =
                    __builtin_sqrtf(dvx * dvx + dvz * dvz);
                if (da < mind)
                    mind = da;
                if (dv < mind)
                    mind = dv;
                float vh[3] = { V[0], V[1] + 1.45f, V[2] }, vc[3] =
                    { V[0], V[1] + 1.10f, V[2] }, v0[3] =
                    { V[0], V[1] + 0.15f, V[2] };
                float a0[3] = { A[0], A[1] + 0.15f, A[2] }, a1[3] =
                    { A[0], A[1] + 1.62f, A[2] }, ah[3] =
                    { A[0], A[1] + 1.55f, A[2] };
                float hv =
                    0.5f * (float) (S41vSegd(C, vh, a0, a1) <
                                    S43_CAP) + 0.5f * (float) (S41vSegd(C,
                                                                         vc,
                                                                         a0,
                                                                         a1)
                                                               < S43_CAP);
                if (view == 2) {
                    float ha =
                        (float) (S41vSegd(C, ah, v0, vh) <
                                 S43_CAP - 0.05f);
                    if (ha > hv)
                        hv = ha;
                }
                occ += hv;
                float px = V[0] - A[0], pz = V[2] - A[2], pl =
                    __builtin_sqrtf(px * px + pz * pz), lx =
                    C[0] - 0.5f * (A[0] + V[0]), lz =
                    C[2] - 0.5f * (A[2] + V[2]), ll =
                    __builtin_sqrtf(lx * lx + lz * lz);
                so += pl > 0.25f
                    && ll >
                    0.2f ? __builtin_fabsf(px * lz -
                                           pz * lx) / (pl * ll) : 0.7f;
                float vfl =
                    __builtin_sqrtf(g_s43.Vf[j][0] * g_s43.Vf[j][0] +
                                    g_s43.Vf[j][1] * g_s43.Vf[j][1]);
                if (vfl > 0.2f && dv > 0.2f)
                    frn +=
                        (g_s43.Vf[j][0] * dvx +
                         g_s43.Vf[j][1] * dvz) / (vfl * dv);
            }
            if (mind < S43_MIN_AX) {
                nax++;
                continue;
            }
            occ /= (float) n;
            so /= (float) n;
            frn /= (float) n;
            float cont = 0.0f;
            if (cur) {
                float cx = cur[0] - mx, cz = cur[2] - mz, cl =
                    __builtin_sqrtf(cx * cx + cz * cz), ex =
                    C[0] - mx, ez = C[2] - mz, el =
                    __builtin_sqrtf(ex * ex + ez * ez);
                if (cl > 0.2f && el > 0.2f)
                    cont = (cx * ex + cz * ez) / (cl * el);
            }
            float sc = 1.2f * so - 2.5f * occ + (view == 1 ? 0.35f : 0.20f) * frn + 0.30f * cont;       // [TUNING ps43]
            {
                int hs = g_s44pHit[i][0] + g_s44pHit[i][1] + g_s44pHit[i][2], hg = g_s44pHit[i][3] + g_s44pHit[i][4];      // ps44 R5a
                sc -= S44P_SUBJ * (float) hs + S44P_GRZ * (float) hg;
                if (hs)
                    n44s++;
                if (hg)
                    n44g++;
            }
            if (sc > bs) {
                bs = sc;
                best = i;
                bpass = pass;
                memcpy(bE, C, sizeof bE);
                bso = so;
                boc = occ;
                bfr = frn;
                bmin = mind;
            }
        }
    }
    g_s43.best = best;
    g_s43.pass = bpass;
    if (g_ps44p && (n44s || n44g) && LOGCAP(g_s43.nlog, 300))
        Xlog("paircam: ps44 plan (%s) - %d spot(s) with a subject hidden from the lens (the victim's head / chest, the attacker's head), %d with geometry within %.1f m of the frame's middle (+-%.0f deg); the chosen spot %d: %d hidden, %d near (frame %llu)", view == 1 ? "the victim's screen" : "the attacker's screen", n44s, n44g, (double) S44P_NEAR, (double) (S44P_GRAZE * 57.29578f), best, best >= 0 ? g_s44pHit[best][0] + g_s44pHit[best][1] + g_s44pHit[best][2] : -1, best >= 0 ? g_s44pHit[best][3] + g_s44pHit[best][4] : -1, (unsigned long long) g_frame);
    if (best < 0) {
        g_s43.st = -1;
        if (LOGCAP(g_s43.nlog, 300))
            Xlog("paircam: ps43 plan (%s) - NO spot: of %d round the pair %d blocked by geometry, %d behind / at the wall plane, %d within %.2f m of a body's axis (%d %s sample(s), distance %.2f m): the old camera (frame %llu)", view == 1 ? "the victim's screen" : "the attacker's screen", S43_NC, nray, nwall, nax, (double) S43_MIN_AX, n, g_s43.clip ? "clip" : "live-root", (double) D, (unsigned long long) g_frame);
        return 0;
    }
    memcpy(g_s43.E, bE, sizeof bE);
    g_s43.st = 2;
    if (LOGCAP(g_s43.nlog, 300)) {
        float wd =
            g_s43.wok ? (bE[0] - g_s43.wp[0]) * g_s43.wn[0] + (bE[2] -
                                                               g_s43.
                                                               wp[1]) *
            g_s43.wn[1] : -1.0f;
        Xlog("paircam: ps43 plan (%s) - %d %s sample(s) of the pair (the %s), midpoint %.2f %.2f, line %+.2f %+.2f; lens %.2f %.2f %.2f = spot %d of %d, %.0f deg round from the line attacker -> victim, %.2f m from the midpoint%s, %.2f m up; nearest to a body's axis %.2f m over the move, side-on %.2f, covered %.0f %% of the samples, the victim's front %+.2f, score %.2f; dropped: %d blocked, %d wall, %d too near a body; wall plane %.2f m (-1 = none) (frame %llu)", view == 1 ? "the victim's screen" : "the attacker's screen", n, g_s43.clip ? "clip" : "live-root", g_s43.clip ? "finisher clip's own placement, 0.15-2.0 s in" : "roots as they stand now", (double) mx, (double) mz, (double) g_s43.f[0], (double) g_s43.f[1], (double) bE[0], (double) bE[1], (double) bE[2], best, S43_NC, (double) ((float) best * 360.0f / (float) S43_NC), (double) __builtin_sqrtf((bE[0] - mx) * (bE[0] - mx) + (bE[2] - mz) * (bE[2] - mz)), bpass ? " (PULLED IN + raised: every spot was blocked)" : "", (double) (bE[1] - g_s43.M[1]), (double) bmin, (double) bso, (double) (boc * 100.0f), (double) bfr, (double) bs, nray, nwall, nax, (double) wd, (unsigned long long) g_frame);
    }
    return 1;
}

// spc_tick5, before the side decision of the victim's finisher side shot: 1 = the decision waits a tick for the plan. sc = the two sides' scores (cleared when the plan holds)
static int S43VicStep(uint64_t now, uint64_t t_on, const float *me,
                        const float *ot, uint64_t h, int wok,
                        const float *wp, const float *wn, float rfx,
                        float rfz, float vfD, float *sc);
static void S43VicLens(uint64_t t_on, float *E);
// the attacker's request (s32_fin_begin): the takedown's two-shot is planned and shown by s43_att_tick / s43_filter
static struct {
    uint64_t h, t;
    float dur;
    char name[64];
} g_s43q;
static void S43AttReq(uint64_t other_h, float dur_s, const char *name)
{
    if (!g_ps43[PS43_ATT] || !other_h)
        return;
    g_s43q.h = other_h;
    g_s43q.t = GetTickCount64();
    g_s43q.dur = dur_s;
    snprintf(g_s43q.name, sizeof g_s43q.name, "%s", name ? name : "?");
}

static int S34VicTake(uint64_t other_h, float dur_s, const char *name)
{
    if (!other_h || !(dur_s > 0.3f) || MatchSubstate() != 6 || g_fcam_on
        || g_icam_on)
        return 0;
    if (Spc5V4()
        || GetFileAttributesA("t2r-no-pin-cam.txt") !=
        INVALID_FILE_ATTRIBUTES || !g_icam_tramp)
        return 0;               // (this camera does not run: the old path)
    if (g_s44m.early == 1 && g_s34.vf && g_s34.vf_t0 == g_s44m.t0 && g_s34.vf_slot >= 0 && PuppetHandle((uint32_t) g_s34.vf_slot) == other_h) {        // ps44 R13: the early shot is confirmed
        uint64_t n13 = GetTickCount64();
        g_s34.vf_until = n13 + (uint64_t) (dur_s * 1000.0f);
        g_s44m.early = 2;
        if (LOGCAP(g_s34.nlog, 200))
            Xlog("pincam5: ps44 vicmove - %s's pair started on us %lld ms %s the expected contact, %llu ms after the side shot began: the shot runs on for the pair's %.0f ms (no new camera; frame %llu)", name ? name : "?", (long long) ((int64_t) n13 - (int64_t) g_s44m.tc), n13 >= g_s44m.tc ? "after" : "before", (unsigned long long) (n13 - g_s44m.t0), (double) (dur_s * 1000.0f), (unsigned long long) g_frame);
        return 1;
    }
    uint64_t now = GetTickCount64();
    // ps39 C2 (rec38a A 10507 'xcam victim: blend-out w 0.04 ... (ctxdown:pe-fist-wall-abby' = the pinner's escape camera still on its way out, then
    // 10512 'ctxdown: on us by slot 0 ...', 'xcam victim: ctxdown:mf-fist-nose-break-kill role 2: 2013 AUTHORED camera', 10534-10542 'camcont: OUR FILTER
    // rotation jump ... absorbed ... residual 64.4 deg / 2.05 m'; clip A 41.0-41.9: both fighters out of frame, then a face at point-blank): a takedown
    // camera that still ran made this function stand down ('g_xcam_on'), so the finisher on us took the old authored attacker-side camera, mid-clip,
    // under the continuity filter. A finisher on us now ends that camera and takes the side shot (the victim of a finisher gets the side shot).
    if (g_xcamOn) {
        int pinfree = !g_pc5.on || g_pc5.ho || !g_pcam.want;
        if (!g_ps39[PS39_VIC] || !g_ps34_vf || !pinfree)
            return 0;
        if (LOGCAP(g_s34.nlog, 200))
            Xlog("pincam5: ps39 - a takedown camera (%s, weight %.2f) still runs when %s starts on us: it is ended on the frame the victim's side shot cuts in (one cut; frame %llu)", g_xcamName, (double) g_xcam_w, name ? name : "?", (unsigned long long) g_frame);
        g_s39.xstop = 1;        // (spc_tick5 ends it when the shot's side is decided: that camera's view, then the shot - not the game camera for four frames in between)
        if (g_pc5.on) {
            g_pc5.ho = 0;
            g_pcam.ho = 0;
            g_pcam.freeze = 0;
            g_pcam.w = 0.0f;
            g_pcam.want = 0;
            Spc5Done("a finisher on us: the side shot takes over");
        }
    }
    g_s39.vf_wall = name && strstr(name, "wall") != NULL;
    if (g_ps34Pin && g_s34Tb && g_pc5.on && g_pc5.role == 1 && !g_pc5.ho
        && !g_s34.vf && g_pc5.slot >= 0
        && PuppetHandle((uint32_t) g_pc5.slot) == other_h) {
        g_s34.keep = now + (uint64_t) (dur_s * 1000.0f);
        if (LOGCAP(g_s34.nlog, 200))
            Xlog("pincam5: ps34 pin finisher %s on us - the pin's over-the-shoulder shot is held for its %.0f ms (no authored victim camera; frame %llu)", name ? name : "?", (double) (dur_s * 1000.0f), (unsigned long long) g_frame);
        return 1;
    }
    if (!g_ps34_vf || g_pc5.on || g_pcam.w > 0.002f)
        return 0;               // (a pin camera of another kind runs: the old path)
    int sl = -1;
    for (uint32_t i = 0; i < T2R_MAX_PEERS; i++)
        if (PuppetHandle(i) == other_h) {
            sl = (int) i;
            break;
        }
    if (sl < 0)
        return 0;
    g_s34.vf_slot = sl;
    g_s34.vf_t0 = now;
    g_s34.vf_until = now + (uint64_t) (dur_s * 1000.0f);
    g_s34.lk_ok = 0;
    g_s34.vf = 1;
    g_s44m.early = 0;
    g_s44m.to_c = 0;
    g_s44m.tc = 0;
    snprintf(g_s44m.trig, sizeof g_s44m.trig, "the pair's start on us (= the contact)");        // ps44 R13 (s44m_early overwrites this for an early shot)
    if (LOGCAP(g_s34.nlog, 200))
        Xlog("pincam5: ps34 finisher %s on us (by slot %d) - the victim's side shot for %.0f ms, then eased into the downed / game camera over %.0f ms (no authored victim camera; frame %llu)", name ? name : "?", sl, (double) (dur_s * 1000.0f), (double) (S34_VF_OUT * 1000.0f), (unsigned long long) g_frame);
    return 1;
}

// ps41 V - the victim's finisher side shot is chosen by OCCLUSION and corrected once (rec40a A 177.9-179.6 s: 19538 'ps34 shot decided after 62 ms -
// side two-shot, the attacker's LEFT side, kept to the end' with both sides at 1.00 - the attacker stood between the fixed lens and us while the pair
// turned; B 16.13-18.3 s 12509 'ps34 frame check ... lens -> our head BLOCKED': us behind the attacker's back). The side rays only test geometry (the
// attacker's puppet is ignored). Added to each side's score: - S41V_PEN x the part of the lines lens -> our head / our chest that the ATTACKER's torso
// (capsule root -> head, S41V_CAP_R) crosses, + S41V_FRONT x how far that lens stands in front of OUR body (our face visible) = the tie-break. The
// pair's placement at later times of the clip is not sampled (no cheap psync API): it is evaluated on every tick before the decision and, with the lens
// fixed, on every tick after it - a shot that loses us (the lens -> our head ray blocked, or the capsule on both lines) for > 0.3 s takes ONE cut to the
// other side, once per shot, when that side is clear. t2r-ps41-cam-vis-old.txt = the ps40 choice (also the attacker's authored-camera test, t2r-ext.c).
#define S41V_CAP_R 0.30f        // m
#define S41V_PEN 1.0f           // [TUNING ps41]: a fully covered victim costs as much as a fully blocked arm ray
#define S41V_FRONT 0.12f        // [TUNING ps41]: +-0.12 = up to 0.24 between the sides, over s34_decide's 0.15 tie margin
static struct {
    float occ[2], fr[2];
    int flipped, told, cut_req;
    uint64_t hid_at;
} g_s41v;
static volatile LONG g_s41vCut;        // main -> render: the fixed lens was re-fitted on the other side - s34_apply takes it on one cut
static int g_s41vCnow;         // render thread: this frame is that cut
static float S41vOcc(const float *C, const float *me, const float *ot,
                      uint64_t h)
{                               // 0 / 0.5 / 1: the attacker's torso between the lens C and our head / chest
    float p3[3], c3[3], h3[3], q3[3], d3[3], a3[3];
    float vh[3] = { me[0], me[1] + 1.55f, me[2] }, vc[3] =
        { me[0], me[1] + 1.15f, me[2] }, a0[3] =
        { ot[0], ot[1] + 0.15f, ot[2] }, a1[3] =
        { ot[0], ot[1] + 1.62f, ot[2] };
    XvBody(SID_PLAYER, me, p3, c3, h3);
    if (XvFin3(h3) && __builtin_fabsf(h3[1] - me[1]) < 2.2f)
        memcpy(vh, h3, sizeof vh);
    if (XvFin3(c3) && __builtin_fabsf(c3[1] - me[1]) < 2.0f)
        memcpy(vc, c3, sizeof vc);
    XvBody(h, ot, q3, d3, a3);
    if (XvFin3(a3) && __builtin_fabsf(a3[1] - ot[1]) < 2.2f)
        memcpy(a1, a3, sizeof a1);
    return 0.5f * (float) (S41vSegd(C, vh, a0, a1) <
                           S41V_CAP_R) + 0.5f * (float) (S41vSegd(C, vc,
                                                                   a0,
                                                                   a1) <
                                                         S41V_CAP_R);
}

// ps34: the spot's kind and side are decided ONCE, from the rays of the first ticks (sc = the two sides' scores of the kind being tried)
// ps44 R13 (the note at S44M_*): the finisher side shot's way in - the arc s34_apply will fly (round the pair's midpoint from the shown lens to the spot,
// radius pushed out to PC5_ORBIT_MIN mid-way on a long arc), tested in S44M_NR legs (rays kicked here, read next tick by s44m_arc_read). 0 = ONE cut (logged)
static void S44mLog(uint64_t now)
{
    if (LOGCAP(g_s34.nlog, 200))
        Xlog("pincam5: ps44 vicmove - the finisher side shot comes in by %s: trigger %s, the move starts %d ms %s the contact; arc %.0f deg round the pair, %.0f ms%s%s (t2r-ps44-cam-vicmove-old.txt = the cut; frame %llu)", g_s44m.cut ? "ONE CUT (fallback)" : "a SMOOTH MOVE", g_s44m.trig, g_s44m.tc ? abs((int) ((int64_t) g_s44m.tc - (int64_t) now)) : 0, g_s44m.tc && now > g_s44m.tc ? "after" : "before", (double) (g_s44m.arc * 57.29578f), g_s44m.cut ? 0.0 : (double) (g_s44m.dur * 1000.0f), g_s44m.cut ? " - " : ", path clear", g_s44m.cut ? g_s44m.why : "", (unsigned long long) g_frame);
}

// ps45 U3 (the note at PS45_*): one more way in - the arc from the shown lens cp to T round (mx, mz), da signed, raise = the mid-way climb; legs kicked into
// g_s45m.job[slot]; returns the closest horizontal distance of its legs' ends to a body (< 0.60 = not used, its rays drained)
static volatile int g_s45mDsg; // main -> render: the decided way round (+1 / -1 = the sign of the arc s34_apply flies)
static float S45mKick(int slot, const float *cp, const float *T, float mx,
                       float mz, float a0, float da, float R0, float R1,
                       float raise, const float *me, const float *ot)
{
    float big = (__builtin_fabsf(da) - 0.25f) / 0.5f;
    big = big < 0.0f ? 0.0f : big > 1.0f ? 1.0f : big;
    float Q0[3] = { cp[0], cp[1], cp[2] }, nb = 99.0f;
    for (int i = 1; i <= S44M_NR; i++) {
        float u = (float) i / (float) S44M_NR, rl = R0 + (R1 - R0) * u, r =
            rl + __builtin_sinf(3.14159265f * u) * big * (rl <
                                                          PC5_ORBIT_MIN ?
                                                          PC5_ORBIT_MIN -
                                                          rl : 0.0f), a =
            a0 + da * u;
        float Q1[3] =
            { mx + __builtin_sinf(a) * r,
      cp[1] + (T[1] - cp[1]) * u + __builtin_sinf(3.14159265f * u) * raise,
      mz + __builtin_cosf(a) * r };
        if (i == S44M_NR)
            memcpy(Q1, T, sizeof Q1);
        if (me && PsLocOk(me)) {
            float d =
                __builtin_sqrtf((Q1[0] - me[0]) * (Q1[0] - me[0]) +
                                (Q1[2] - me[2]) * (Q1[2] - me[2]));
            if (d < nb)
                nb = d;
        }
        if (ot && PsLocOk(ot)) {
            float d =
                __builtin_sqrtf((Q1[0] - ot[0]) * (Q1[0] - ot[0]) +
                                (Q1[2] - ot[2]) * (Q1[2] - ot[2]));
            if (d < nb)
                nb = d;
        }
        g_s45m.job[slot][i - 1] =
            WorldRayKickM(Q0, Q1, g_pc5.ign, 0x1b07);
        memcpy(Q0, Q1, sizeof Q0);
    }
    if (nb < 0.60f)
        for (int i = 0; i < S44M_NR; i++)
            if (g_s45m.job[slot][i]) {
                RayT(g_s45m.job[slot][i]);
                g_s45m.job[slot][i] = 0;
            }
    return nb;
}

static int S44mArcKick(int best, uint64_t now)
{
    float cp[3], cf[3], T[3], P[3], C[3];
    g_s44m.cut = 1;
    g_s44m.arc = 0.0f;
    g_s44m.dur = 0.0f;
    g_s44m.nleg = 0;
    snprintf(g_s44m.why, sizeof g_s44m.why,
             "the shown camera is not readable");
    if (!RcamGet(cp, cf)) {
        S44mLog(now);
        return 0;
    }
    if (g_ps43[PS43_VIC] && g_s43.view == 1 && g_s43.st == 2
        && g_s43.t0 == g_pc5.t_on)
        memcpy(T, g_s43.E, sizeof T);
    else {
        Pc5Spot(best, (best ? -1.0f : 1.0f) * PC5_LAT * g_pc5.latm, 0.0f,
                 P, C);
        memcpy(T, C, sizeof T);
    }
    float mx = g_pc5.sax + g_pc5.fx * 0.5f * g_pc5.sd, mz =
        g_pc5.saz + g_pc5.fz * 0.5f * g_pc5.sd;
    float a0 = __builtin_atan2f(cp[0] - mx, cp[2] - mz), da =
        __builtin_remainderf(__builtin_atan2f(T[0] - mx, T[2] - mz) - a0,
                             6.2831853f);
    float R0 =
        __builtin_sqrtf((cp[0] - mx) * (cp[0] - mx) +
                        (cp[2] - mz) * (cp[2] - mz)), R1 =
        __builtin_sqrtf((T[0] - mx) * (T[0] - mx) +
                        (T[2] - mz) * (T[2] - mz));
    float big = (__builtin_fabsf(da) - 0.25f) / 0.5f;
    big = big < 0.0f ? 0.0f : big > 1.0f ? 1.0f : big;
    g_s44m.arc = __builtin_fabsf(da);
    float pb[16], ab[16];
    const float *me = PlayerLocator(pb);
    uint64_t h =
        g_s34.vf_slot >= 0 ? PuppetHandle((uint32_t) g_s34.vf_slot) : 0;
    const float *ot = h ? PuppetLocator(h, ab) : NULL;
    float Q0[3] = { cp[0], cp[1], cp[2] }, nb = 99.0f;
    for (int i = 1; i <= S44M_NR; i++) {
        float u = (float) i / (float) S44M_NR, rl = R0 + (R1 - R0) * u, r =
            rl + __builtin_sinf(3.14159265f * u) * big * (rl <
                                                          PC5_ORBIT_MIN ?
                                                          PC5_ORBIT_MIN -
                                                          rl : 0.0f), a =
            a0 + da * u;
        float Q1[3] =
            { mx + __builtin_sinf(a) * r, cp[1] + (T[1] - cp[1]) * u,
      mz + __builtin_cosf(a) * r };
        if (i == S44M_NR)
            memcpy(Q1, T, sizeof Q1);
        if (me && PsLocOk(me)) {
            float d =
                __builtin_sqrtf((Q1[0] - me[0]) * (Q1[0] - me[0]) +
                                (Q1[2] - me[2]) * (Q1[2] - me[2]));
            if (d < nb)
                nb = d;
        }
        if (ot && PsLocOk(ot)) {
            float d =
                __builtin_sqrtf((Q1[0] - ot[0]) * (Q1[0] - ot[0]) +
                                (Q1[2] - ot[2]) * (Q1[2] - ot[2]));
            if (d < nb)
                nb = d;
        }
        g_s44m.job[i - 1] = WorldRayKickM(Q0, Q1, g_pc5.ign, 0x1b07);
        if (g_s44m.job[i - 1])
            g_s44m.nleg++;
        memcpy(Q0, Q1, sizeof Q0);
    }
    g_s45m_var = 0;
    g_s45m_mv = 0;
    g_s45m.ok[0] = g_s45m.ok[1] = g_s45m.ok[2] = 0;
    if (g_ps45[PS45_ARC]) {     // ps45 U3 (the note at PS45_*): two more ways in, tested with the planned one (read next tick by s44m_arc_read)
        float da1 = da > 0.0f ? da - 6.2831853f : da + 6.2831853f;
        g_s45m.da[0] = da;
        g_s45m.nb[0] = nb;
        g_s45m.ok[0] = nb >= 0.60f;
        g_s45m.da[1] = da1;
        g_s45m.nb[1] =
            __builtin_fabsf(da1) <= S45M_MAXARC ? S45mKick(0, cp, T, mx,
                                                            mz, a0, da1,
                                                            R0, R1, 0.0f,
                                                            me, ot) : 0.0f;
        g_s45m.ok[1] = g_s45m.nb[1] >= 0.60f;
        g_s45m.da[2] = da;
        g_s45m.nb[2] =
            S45mKick(1, cp, T, mx, mz, a0, da, R0, R1, S45M_UP, me, ot);
        g_s45m.ok[2] = g_s45m.nb[2] >= 0.60f;
        if (!g_s45m.ok[0])
            for (int i = 0; i < S44M_NR; i++)
                if (g_s44m.job[i]) {
                    RayT(g_s44m.job[i]);
                    g_s44m.job[i] = 0;
                }
        if (g_s45m.ok[0] || g_s45m.ok[1] || g_s45m.ok[2]) {
            g_s44m.dur = S44M_T0;
            return 1;
        }
        snprintf(g_s44m.why, sizeof g_s44m.why,
                 "every way in passes near a body (planned %.2f m, the other way round %.2f m, raised %.2f m)",
                 (double) nb, (double) g_s45m.nb[1],
                 (double) g_s45m.nb[2]);
        S44mLog(now);
        return 0;
    }
    if (nb < 0.60f) {
        for (int i = 0; i < S44M_NR; i++)
            if (g_s44m.job[i]) {
                RayT(g_s44m.job[i]);
                g_s44m.job[i] = 0;
            }
        snprintf(g_s44m.why, sizeof g_s44m.why,
                 "the arc passes %.2f m from a body", (double) nb);
        S44mLog(now);
        return 0;
    }
    g_s44m.dur =
        S44M_T0 + (S44M_T1 - S44M_T0) * (g_s44m.arc / 3.14159265f >
                                         1.0f ? 1.0f : g_s44m.arc /
                                         3.14159265f);
    return 1;
}

static void S44mArcRead(uint64_t now)
{
    if (g_ps45[PS45_ARC] && (g_s45m.ok[0] || g_s45m.ok[1] || g_s45m.ok[2])) {   // ps45 U3 / U4 (the note at PS45_*): the first clear way in of the three
        int bl[3] = { -1, -1, -1 };
        for (int v = 0; v < 3; v++)
            for (int i = 0; i < S44M_NR; i++) {
                uint64_t *j =
                    v == 0 ? &g_s44m.job[i] : &g_s45m.job[v - 1][i];
                if (*j) {
                    g_rayErr = 0;
                    float t = RayT(*j);
                    *j = 0;
                    if (bl[v] < 0 && !g_rayErr && t >= 0.0f && t < 0.97f)
                        bl[v] = i;
                }
            }
        int pick = -1;
        for (int v = 0; v < 3 && pick < 0; v++)
            if (g_s45m.ok[v] && bl[v] < 0)
                pick = v;
        static const char *const NM[3] =
            { "the planned arc", "the other way round the pair",
"the planned arc raised" };
        if (pick >= 0) {
            float a01 = __builtin_fabsf(g_s45m.da[pick]) / 3.14159265f;
            a01 = a01 > 1.0f ? 1.0f : a01;
            g_s44m.arc = __builtin_fabsf(g_s45m.da[pick]);
            g_s44m.dur =
                g_ps45[PS45_EASE] ? S45M_T0 + (S45M_T1 -
                                               S45M_T0) * a01 : S44M_T0 +
                (S44M_T1 - S44M_T0) * a01;
            g_s45mDsg = g_s45m.da[pick] >= 0.0f ? 1 : -1;
            g_s45m_var = pick;
            g_s45mMvAt = GetTickCount64();
            MemoryBarrier();
            g_s45m_mv = 1;
            g_s34.tin = g_s44m.dur;
            g_s44m.cut = 0;
        } else {
            g_s34.tin = 0.0f;
            g_s44m.cut = 1;
            snprintf(g_s44m.why, sizeof g_s44m.why,
                     "every way in is BLOCKED by geometry (planned leg %d, the other way round leg %d, raised leg %d of %d; 0 = not tried)",
                     bl[0] + 1, bl[1] + 1, bl[2] + 1, S44M_NR);
        }
        if (LOGCAP(g_s34.nlog, 200))
            Xlog("pincam5: ps45 arc - the victim side shot's way in: planned %.0f deg %s, the other way round %.0f deg %s, raised %.2f m %s -> %s%s (t2r-ps45-cam-arc-old.txt = the ps44 cut on a blocked arc; frame %llu)", (double) (__builtin_fabsf(g_s45m.da[0]) * 57.29578f), !g_s45m.ok[0] ? "near a body" : bl[0] >= 0 ? "BLOCKED" : "clear", (double) (__builtin_fabsf(g_s45m.da[1]) * 57.29578f), !g_s45m.ok[1] ? "not used" : bl[1] >= 0 ? "BLOCKED" : "clear", (double) S45M_UP, !g_s45m.ok[2] ? "near a body" : bl[2] >= 0 ? "BLOCKED" : "clear", pick >= 0 ? NM[pick] : "ONE CUT", pick >= 0 && g_ps45[PS45_EASE] ? ", smoothstep" : "", (unsigned long long) g_frame);
        S44mLog(now);
        return;
    }
    int blk = -1;
    for (int i = 0; i < S44M_NR; i++)
        if (g_s44m.job[i]) {
            g_rayErr = 0;
            float t = RayT(g_s44m.job[i]);
            g_s44m.job[i] = 0;
            if (blk < 0 && !g_rayErr && t >= 0.0f && t < 0.97f)
                blk = i;
        }
    if (blk >= 0) {
        g_s34.tin = 0.0f;
        g_s44m.cut = 1;
        snprintf(g_s44m.why, sizeof g_s44m.why,
                 "leg %d of %d of the arc is BLOCKED by geometry", blk + 1,
                 S44M_NR);
    } else {
        g_s34.tin = g_s44m.dur;
        g_s44m.cut = 0;
    }
    S44mLog(now);
}

static void S34Decide(const float *sc, uint64_t now)
{
    if (!g_pc5.v34w)
        return;
    if (g_pc5.v34w == 4) {
        S44mArcRead(now);
        g_pc5.v34w = 0;
        g_s34.dec_ms = now;
        return;
    }                           // ps44 R13: the move's path was ray-tested last tick
    if (g_pc5.v34w == 3) {      // ps39: the straight path from the shown camera to the pin shot was ray-tested last tick - blocked = one cut
        float t = 1.0f;
        int got = 0;
        if (g_s39.pjob) {
            g_rayErr = 0;
            t = RayT(g_s39.pjob);
            g_s39.pjob = 0;
            got = !g_rayErr;
        }
        int blk = got && t >= 0.0f && t < 0.95f;
        if (blk)
            g_s34.tin = 0.0f;
        g_pc5.v34w = 0;
        g_s34.dec_ms = now;
        if (LOGCAP(g_s34.nlog, 200))
            Xlog("pincam5: ps39 - the pin shot comes in %s: %.0f deg round the pair from the shown camera, the straight path to it is %s (frame %llu)", blk ? "on ONE CUT" : "eased", (double) (g_s39.arc * 57.29578f), blk ? "BLOCKED (a body / geometry)" : "clear", (unsigned long long) g_frame);
        return;
    }
    if (g_pc5.n - g_s34.n_dec0 < 4
        && now - g_pc5.t_on < (g_pc5.v34w == 1 ? 150u : 300u))
        return;
    int best =
        sc[1 - g_pc5.side] >
        sc[g_pc5.side] + 0.15f ? 1 - g_pc5.side : g_pc5.side;
    int ok = g_pc5.rt[best][0] >= S34_ARM_MIN && g_pc5.rt[best][2] >= 0.5f;
    const char *kind =
        g_pc5.v34 ? "side two-shot" : "over the attacker's shoulder";
    if (ok || g_pc5.v34w == 2) {
        if (!ok && g_pc5.v34w == 2) {   // both kinds blocked: the primary kind's better side, pulled in by its ray, raised
            g_pc5.v34 = !g_pc5.v34;
            best = g_pc5.pk_side;
            g_pc5.hi = 1;
            Pc5Drain();
            for (int k = 0; k < 2; k++)
                for (int i = 0; i < 3; i++)
                    g_pc5.rt[k][i] = 1.0f;
            kind =
                g_pc5.
                v34 ?
                "side two-shot, raised (LAST RESORT: every spot was blocked)"
                :
                "over the attacker's shoulder, raised (LAST RESORT: every spot was blocked)";
        }
        g_pc5.side = best;
        g_pc5.latk = best ? -1.0f : 1.0f;
        g_pc5.v34w = 0;
        g_s34.dec_ms = now;
        // ps39 C4 / C8 (rec38a B, the victim's shots: 15001 'ps34 ON as the victim of a takedown ... (in 300 ms', camwrite 17372-17375 'displayed step yaw
        // -10.3 / -11.0 / -11.1 / -10.3', 21822-21829 'yaw -11.1 .. -22.6' = 150 deg in 8 frames with nobody in frame mid-way (clip 194.03-194.23 'four views
        // in six frames'); the pin shot 12508 'camdiag: LENS STEP > 0.5 m IN ONE FRAME ... shown step 1.47 m', clip 38.70-39.17): the timed ease from the
        // camera behind OUR back to a shot on the far side of the pair is a 100-180 deg swing in 0.3-0.5 s - at 30 fps three or four unrelated views, through
        // whatever stands on the arc. The wanted result is a cut to a camera from the side. The finisher side shot comes in on ONE cut; the pin shot too when it is
        // more than S39_CUT_ARC round the pair from the shown camera or the straight path to it is blocked (one ray, read next tick), else it eases as before.
        if (g_ps39[PS39_VIC]) {
            if (g_s34.vf) {
                if (g_ps44m && S44mArcKick(best, now))
                    g_pc5.v34w = 4;
                else
                    g_s34.tin = 0.0f;
            }                   // ps44 R13: a move along the arc (its path read next tick), else the cut
            else {
                float cp[3], cf[3], P[3], C[3];
                Pc5Spot(best,
                         (best ? -1.0f : 1.0f) * PC5_LAT * g_pc5.latm,
                         0.0f, P, C);
                if (RcamGet(cp, cf)) {
                    float mx = g_pc5.sax + g_pc5.fx * 0.5f * g_pc5.sd, mz =
                        g_pc5.saz + g_pc5.fz * 0.5f * g_pc5.sd;
                    g_s39.arc =
                        __builtin_remainderf(__builtin_atan2f
                                             (C[0] - mx,
                                              C[2] - mz) -
                                             __builtin_atan2f(cp[0] - mx,
                                                              cp[2] - mz),
                                             6.2831853f);
                    if (!(__builtin_fabsf(g_s39.arc) <= S39_CUT_ARC))
                        g_s34.tin = 0.0f;
                    else {
                        g_s39.pjob =
                            WorldRayKickM(cp, C, g_pc5.ign, 0x1b07);
                        if (g_s39.pjob)
                            g_pc5.v34w = 3;
                    }
                }
            }
            if (g_s34.tin == 0.0f && LOGCAP(g_s34.nlog, 200))
                Xlog("pincam5: ps39 - the %s comes in on ONE CUT (%s; frame %llu)", g_s34.vf ? "finisher side shot" : "pin shot", g_s34.vf ? "a cut to a camera from the side, then it holds" : "more than 70 deg round the pair from the shown camera", (unsigned long long) g_frame);
        }
        if (LOGCAP(g_s34.nlog, 200))
            Xlog("pincam5: ps34 shot decided after %llu ms - %s, the attacker's %s side, kept to the end (right: arm %.2f beside %.2f sight %.2f | left: arm %.2f beside %.2f sight %.2f; in %.0f ms; frame %llu)", (unsigned long long) (now - g_pc5.t_on), kind, best ? "LEFT" : "RIGHT", (double) g_pc5.rt[0][0], (double) g_pc5.rt[0][1], (double) g_pc5.rt[0][2], (double) g_pc5.rt[1][0], (double) g_pc5.rt[1][1], (double) g_pc5.rt[1][2], (double) (g_s34.tin * 1000.0f), (unsigned long long) g_frame);
        return;
    }
    // the primary kind is blocked on both sides: try the other kind
    if (LOGCAP(g_s34.nlog, 200))
        Xlog("pincam5: ps34 %s blocked on both sides (right: arm %.2f sight %.2f | left: arm %.2f sight %.2f) - trying the %s", kind, (double) g_pc5.rt[0][0], (double) g_pc5.rt[0][2], (double) g_pc5.rt[1][0], (double) g_pc5.rt[1][2], g_pc5.v34 ? "over-the-shoulder shot" : "side two-shot");
    g_pc5.pk_side = best;
    g_pc5.v34 = !g_pc5.v34;
    g_pc5.v34w = 2;
    g_s34.n_dec0 = g_pc5.n;
    g_pc5.frac = 1.0f;
    Pc5Drain();
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 3; i++)
            g_pc5.rt[k][i] = 1.0f;
}

// ps34 self-check, main thread, every tick of the victim's shot once it is in: heads on screen (the SHOWN camera), lens to the two body axes, the lens ->
// our head ray (our own body ignored: a hit is the attacker or geometry). One line a second + one warning per shot.
static void S34Check(uint64_t now, const float *me, const float *ot)
{
    float cp[3], cf[3];
    if (!RcamGet(cp, cf))
        return;
    float fh = __builtin_sqrtf(cf[0] * cf[0] + cf[2] * cf[2]);
    if (!(fh > 0.2f))
        return;
    float fv = g_mfHold
        && g_mf_held > 30.0f ? g_mf_held : g_mfBase >
        30.0f ? g_mfBase : 70.0f, th =
        __builtin_tanf(fv * 0.5f * 0.0174533f), tv = th * 9.0f / 16.0f;
    float rx = -cf[2] / fh, rz = cf[0] / fh, ux = -cf[1] * cf[0] / fh, uy =
        fh, uz = -cf[1] * cf[2] / fh;
    float hh[3] = { me[0], me[1] + 1.55f, me[2] }, c3[3], p3[3], h3[3];
    XvBody(SID_PLAYER, me, p3, c3, h3);
    if (XvFin3(h3) && __builtin_fabsf(h3[1] - me[1]) < 2.2f) {
        hh[0] = h3[0];
        hh[1] = h3[1];
        hh[2] = h3[2];
    }                           // our real head (we may be falling)
    float H[2][3] =
        { { hh[0], hh[1], hh[2] }, { ot[0], ot[1] + 1.55f, ot[2] } },
        sx[2] = { 9.99f, 9.99f }, sy[2] = { 9.99f, 9.99f };
    for (int i = 0; i < 2; i++) {
        float vx = H[i][0] - cp[0], vy = H[i][1] - cp[1], vz =
            H[i][2] - cp[2], dep = vx * cf[0] + vy * cf[1] + vz * cf[2];
        if (dep > 0.05f) {
            sx[i] = (vx * rx + vz * rz) / dep / th;
            sy[i] = (vx * ux + vy * uy + vz * uz) / dep / tv;
        }
    }
    float dme =
        __builtin_sqrtf((cp[0] - me[0]) * (cp[0] - me[0]) +
                        (cp[2] - me[2]) * (cp[2] - me[2])), dat =
        __builtin_sqrtf((cp[0] - ot[0]) * (cp[0] - ot[0]) +
                        (cp[2] - ot[2]) * (cp[2] - ot[2]));
    if (cp[1] > me[1] + 1.95f)
        dme = 9.0f;
    if (cp[1] > ot[1] + 1.95f)
        dat = 9.0f;
    // the lens -> our head ray: read the last one, kick the next
    if (g_s34.hr_job) {
        g_rayErr = 0;
        float t = RayT(g_s34.hr_job);
        g_s34.hr_job = 0;
        if (!g_rayErr)
            g_s34.hr_blk = t >= 0.0f && t < 0.92f;
    }
    {
        float d3[3] = { hh[0] - cp[0], hh[1] - cp[1], hh[2] - cp[2] };
        g_s34.hr_len =
            __builtin_sqrtf(d3[0] * d3[0] + d3[1] * d3[1] + d3[2] * d3[2]);
        if (g_s34.hr_len > 0.3f && g_s34.hr_len < 6.0f)
            g_s34.hr_job = WorldRayKickM(cp, hh, 0, 0x1b07);
    }
    if (g_s34.hr_blk) {
        if (!g_s34.hr_bad_at)
            g_s34.hr_bad_at = now;
    } else
        g_s34.hr_bad_at = 0;
    int in0 = sx[0] >= -1.0f && sx[0] <= 1.0f && sy[0] >= -1.0f
        && sy[0] <= 1.0f, in1 = sx[1] >= -1.0f && sx[1] <= 1.0f
        && sy[1] >= -1.0f && sy[1] <= 1.0f;
    int nearb = dme < 0.35f || dat < 0.35f, bad = !in0 || !in1 || nearb
        || (g_s34.hr_bad_at && now - g_s34.hr_bad_at > 300);
    if (!bad)
        g_s34.bad_at = 0;
    else {
        if (!g_s34.bad_at)
            g_s34.bad_at = now;
        if (!g_s34.warned && now - g_s34.bad_at > 300) {
            g_s34.warned = 1;
            if (LOGCAP(g_s34.nlog, 200))
                Xlog("pincam5: ps34 frame check - WARNING: %s%s%s%s for > 0.3 s (our head x %+.2f y %+.2f, the attacker's head x %+.2f y %+.2f, lens %.2f m from our axis / %.2f m from the attacker's; %s; frame %llu)", !in0 ? "OUR head out of frame; " : "", !in1 ? "the ATTACKER's head out of frame; " : "", nearb ? "the lens within 0.35 m of a body; " : "", g_s34.hr_bad_at && now - g_s34.hr_bad_at > 300 ? "the line lens -> our head is BLOCKED (the attacker's body or geometry)" : "", (double) sx[0], (double) sy[0], (double) sx[1], (double) sy[1], (double) dme, (double) dat, g_s34.vf ? "finisher side shot" : g_pc5.v34 ? "pin, side two-shot" : "pin, over the shoulder", (unsigned long long) g_frame);
        }
    }
    // the last resort for a covered face: one raise of the over-the-shoulder shot
    if (!g_pc5.v34 && !g_pc5.hi && g_s34.hr_bad_at
        && now - g_s34.hr_bad_at > 400) {
        g_pc5.hi = 1;
        if (LOGCAP(g_s34.nlog, 200))
            Xlog("pincam5: ps34 - the attacker covers our head from the lens for > 0.4 s: the shot rises %.2f m (once; frame %llu)", (double) S34_HI, (unsigned long long) g_frame);
    }
    if (now - g_s34.chk_at >= 1000) {
        g_s34.chk_at = now;
        if (LOGCAP(g_s34.nlog, 400))
            Xlog("pincam5: ps34 frame check - our head x %+.2f y %+.2f, the attacker's head x %+.2f y %+.2f, lens %.2f m from our axis / %.2f m from the attacker's, lens -> our head %s (%s; %s; fov %.1f; frame %llu)", (double) sx[0], (double) sy[0], (double) sx[1], (double) sy[1], (double) dme, (double) dat, g_s34.hr_blk ? "BLOCKED" : "clear", in0 && in1 ? "both in frame" : "OUT OF FRAME", g_s34.vf ? "finisher side shot" : g_pc5.v34 ? "pin, side two-shot" : "pin, over the shoulder", (double) fv, (unsigned long long) g_frame);
    }
}

// ps44 R13: the victim's side shot starts once the partner's finisher can no longer fail on our screen (main thread, spc_tick5's top)
static void S44mEarly(uint64_t now)
{
    static int nl;
    if (g_s44m.early == 1) {
        if (g_s34.vf && g_s34.vf_t0 == g_s44m.t0) {
            if (now > g_s44m.tc + S44M_WAIT) {  // the pair never came (a late dodge ruled by the attacker, the duel broken): the shot eases out again
                g_s34.vf_until = now;
                g_s44o.vt0 = g_s34.vf_t0;
                g_s44o.st = 3;
                g_s44m.early = 0;
                g_s45eT0 = g_s34.vf_t0;        // (ps45 U2: it eases out)
                if (LOGCAP(nl, 100))
                    Xlog("pincam5: ps44 vicmove - the early side shot was NOT confirmed: no finisher pair on us %d ms after the expected contact - it eases out (frame %llu)", S44M_WAIT, (unsigned long long) g_frame);
            }
            return;
        }
        g_s44m.early = 0;
    }
    if (!g_ps44m || g_s34.vf)
        return;
    int s, f, tc;
    if (!S44mVicCommit(&s, &f, &tc))
        return;
    uint64_t c = now + (uint64_t) (tc > 0 ? tc : 0), h =
        PuppetHandle((uint32_t) s);
    if (!h
        || (g_s44m.last_tc
            && (c >
                g_s44m.last_tc ? c - g_s44m.last_tc : g_s44m.last_tc - c) <
            400))
        return;                 // (one try per lethal swing)
    g_s44m.last_tc = c;
    char nm[80];
    snprintf(nm, sizeof nm, "ctxdown:%s", CD_PAIRS[f].name);
    if (!S34VicTake
        (h, (float) (tc > 0 ? tc : 0) / 1000.0f + CdChainTotal(f), nm)
        || !g_s34.vf || g_s34.vf_t0 < now)
        return;                 // (gates as at the pair's start; a pin's hold is not this shot)
    g_s44m.early = 1;
    g_s44m.t0 = g_s34.vf_t0;
    g_s44m.tc = c;
    g_s44m.to_c = tc;
    snprintf(g_s44m.trig, sizeof g_s44m.trig,
             "slot %d's weapon finisher windup %s, our dodge window closed",
             s, CD_PAIRS[f].name);
    if (LOGCAP(nl, 100))
        Xlog("pincam5: ps44 vicmove - %s: the blow can no longer fail on our screen (dodge window closed, no dodge clip of ours) - the side shot starts %d ms before the contact (frame %llu)", g_s44m.trig, tc, (unsigned long long) g_frame);
}

// ps44 R5c: the lens -> our chest ray of the held victim side shot (read the last, kick the next); 1 = blocked for S44_HID_MS
static int S44vChest(uint64_t now, const float *E, const float *me,
                      uint64_t h)
{
    if (!g_ps44vr)
        return 0;
    if (g_s44vr.t0 != g_pc5.t_on) {
        if (g_s44vr.cjob) {
            RayT(g_s44vr.cjob);
            g_s44vr.cjob = 0;
        }
        g_s44vr.t0 = g_pc5.t_on;
        g_s44vr.st = 0;
        g_s44vr.cblk = 0;
        g_s44vr.cblk_at = 0;
        g_s44vr.ncb = 0;
    }
    if (g_s44vr.cjob) {
        g_rayErr = 0;
        float t = RayT(g_s44vr.cjob);
        g_s44vr.cjob = 0;
        if (!g_rayErr)
            g_s44vr.cblk = t >= 0.0f && t < 0.97f;
    }
    float p3[3], c3[3], h3[3];
    XvBody(SID_PLAYER, me, p3, c3, h3);
    if (!XvFin3(c3) || __builtin_fabsf(c3[1] - me[1]) > 2.0f) {
        c3[0] = me[0];
        c3[1] = me[1] + 1.10f;
        c3[2] = me[2];
    }
    float dx = c3[0] - E[0], dy = c3[1] - E[1], dz = c3[2] - E[2], l =
        __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
    if (l > S44V_SHORT + 0.3f && l < 8.0f) {
        float T[3] =
            { E[0] + dx / l * (l - S44V_SHORT),
       E[1] + dy / l * (l - S44V_SHORT),
       E[2] + dz / l * (l - S44V_SHORT) };
        g_s44vr.cjob = WorldRayKickM(E, T, h, 0x1b07);
    }
    if (!g_s44vr.cblk) {
        g_s44vr.cblk_at = 0;
        return 0;
    }
    if (!g_s44vr.cblk_at) {
        g_s44vr.cblk_at = now;
        g_s44vr.ncb++;
    }
    return now - g_s44vr.cblk_at >= S44_HID_MS;
}

// ps44 R5a late: the one late correction re-plans (1 = started; the cut comes when the plan is scored, s44v_step)
static int S44vStart(uint64_t now, const float *me, const float *ot,
                      uint64_t h, int wok, const float *wp,
                      const float *wn, const char *why)
{
    if (!g_ps44vr || !g_ps43[PS43_VIC] || g_s44vr.st)
        return 0;
    float mx = 0.5f * (me[0] + ot[0]), mz = 0.5f * (me[2] + ot[2]), bx =
        g_s39.E[0] - mx, bz = g_s39.E[2] - mz, bl =
        __builtin_sqrtf(bx * bx + bz * bz);
    if (!(bl > 0.3f))
        return 0;
    g_s44Ban[0] = bx / bl;
    g_s44Ban[1] = bz / bl;
    g_s44BanOn = 1;
    S43Reset();
    g_s43.view = 1;
    g_s43.t0 = g_pc5.t_on;
    S43Build(1, me, ot, h, wok, wp, wn, 2.00f, 3.00f, g_s34.vfD,
              S34_VF_UP, 0);
    g_s44vr.st = 1;
    g_s44vr.t_req = now;
    memcpy(g_s44vr.E0, g_s39.E, sizeof g_s44vr.E0);
    if (LOGCAP(g_s34.nlog, 200))
        Xlog("pincam5: ps44 vrep - the fixed side shot lost us for > 0.3 s (%s): ONE re-plan from the live roots, the spots within 50 deg of the lens %.2f %.2f %.2f are out (once per shot; t2r-ps44-cam-vrep-old.txt = the ps41 side flip; frame %llu)", why, (double) g_s39.E[0], (double) g_s39.E[1], (double) g_s39.E[2], (unsigned long long) g_frame);
    return 1;
}

static void S44vStep(uint64_t now, float rfx, float rfz)
{
    if (g_s44vr.st != 1 || g_s44vr.t0 != g_pc5.t_on)
        return;
    if (g_s43.view == 1 && g_s43.st == 1 && g_s43.t0 == g_pc5.t_on)
        S43Score(NULL);
    if (g_s43.view == 1 && g_s43.st == 2 && g_s43.t0 == g_pc5.t_on) {
        memcpy(g_s39.E, g_s43.E, sizeof g_s39.E);
        g_s41v.cut_req = 1;
        g_s44BanOn = 0;
        g_s44vr.st = 2;
        g_pc5.side =
            ((g_s43.E[0] - g_s43.M[0]) * -rfz +
             (g_s43.E[2] - g_s43.M[2]) * rfx) >= 0.0f ? 0 : 1;
        g_pc5.latk = g_pc5.side ? -1.0f : 1.0f;
        g_s34.dec_ms = now;
        g_s34.hr_bad_at = 0;
        g_s34.bad_at = 0;
        g_s34.hr_blk = 0;
        g_s34.warned = 0;
        g_s44vr.cblk = 0;
        g_s44vr.cblk_at = 0;
        if (LOGCAP(g_s34.nlog, 200))
            Xlog("pincam5: ps44 vrep - re-planned: ONE cut to spot %d of %d, lens %.2f %.2f %.2f (%.2f m from the lost one; %llu ms into the shot; frame %llu)", g_s43.best, S43_NC, (double) g_s39.E[0], (double) g_s39.E[1], (double) g_s39.E[2], (double) __builtin_sqrtf((g_s39.E[0] - g_s44vr.E0[0]) * (g_s39.E[0] - g_s44vr.E0[0]) + (g_s39.E[2] - g_s44vr.E0[2]) * (g_s39.E[2] - g_s44vr.E0[2])), (unsigned long long) (now - g_pc5.t_on), (unsigned long long) g_frame);
    } else if (g_s43.st == -1 || now - g_s44vr.t_req > 400) {
        g_s44BanOn = 0;
        g_s44vr.st = 3;         // (3: failed - the late block takes the old side flip if that side is clear)
        if (LOGCAP(g_s34.nlog, 200))
            Xlog("pincam5: ps44 vrep - the re-plan found no other clear spot (%s): the ps41 side flip if its side is clear, else the shot stays (frame %llu)", g_s43.st == -1 ? "every spot dropped" : "not scored in 400 ms", (unsigned long long) g_frame);
    }
}

static void SpcTick5(float dt)
{
    uint64_t now = GetTickCount64();
    S44mEarly(now);            // ps44 R13
    int xon39 = g_xcamOn && !g_s39.xstop;      // ps39 C2: a takedown camera that the finisher side shot is about to replace does not stop this camera
    {
        static uint64_t at;
        static int first = 1;   // ps34: the two opt-out files
        if (first || now - at >= 2000) {
            at = now;
            int a =
                GetFileAttributesA("t2r-ps34-cam-pinvic-old.txt") ==
                INVALID_FILE_ATTRIBUTES, v =
                GetFileAttributesA("t2r-ps34-cam-vicfin-old.txt") ==
                INVALID_FILE_ATTRIBUTES;
            if (first || a != g_ps34Pin)
                Xlog("pincam5: ps34 victim pin camera %s",
                     a ?
                     "ON - over the attacker's shoulder at the victim, side decided once, timed ease in / out (t2r-ps34-cam-pinvic-old.txt = the follower camera)"
                     :
                     "OFF (t2r-ps34-cam-pinvic-old.txt): the old follower camera");
            if (first || v != g_ps34_vf)
                Xlog("pincam5: ps34 victim finisher camera %s",
                     v ?
                     "ON - a side shot of the victim through the takedown, eased into the downed camera (t2r-ps34-cam-vicfin-old.txt = the authored victim camera)"
                     :
                     "OFF (t2r-ps34-cam-vicfin-old.txt): the authored victim camera");
            g_ps34Pin = a;
            g_ps34_vf = v;
            first = 0;
        }
    }
    int slot = -1, role =
        GetFileAttributesA("t2r-no-pin-cam.txt") ==
        INVALID_FILE_ATTRIBUTES ? MpnCamRole(&slot) : 0;
    const char *why = role ? NULL : "the pin is over";
    int vf34 = 0;
    if (g_s34.vf) {             // ps49 (A [148087]-[148371]: the face smash on us aborted at 301 ms, not downed, the side shot held 4734 ms): a pair on us that ends > 600 ms
        // before the shot's planned end while we are up = the shot eases out now. t2r-vicfin-cam-ps49-old.txt = held to the planned end.
        static uint64_t t49;
        static int seen49;
        if (t49 != g_s34.vf_t0) {
            t49 = g_s34.vf_t0;
            seen49 = 0;
        }
        if (PsyncRole() == 2 && g_ps.ent_vic == SID_PLAYER)
            seen49 = 1;
        else if (seen49 && !g_downed && now + 600 < g_s34.vf_until
                 && GetFileAttributesA("t2r-vicfin-cam-ps49-old.txt") ==
                 INVALID_FILE_ATTRIBUTES) {
            Xlog("pincam5: ps49 - the pair on us ended %llu ms before the side shot's planned end and we are not down: the shot eases out now (frame %llu)", (unsigned long long) (g_s34.vf_until - now), (unsigned long long) g_frame);
            g_s34.vf_until = now;
            g_s44o.vt0 = g_s34.vf_t0;
            g_s44o.st = 3;
            g_s44m.early = 0;
            g_s45eT0 = g_s34.vf_t0;
            seen49 = 0;
        }
    }
    if (g_s34.vf) {             // ps34: the victim's finisher shot runs to the pair's end, through our down
        int ok34 = MatchSubstate() == 6 && !xon39 && !(role && !g_s34Tb);
        if (ok34
            && (now < g_s34.vf_until || S44oHold(now, g_s34.vf_slot))) {
            role = 1;
            slot = g_s34.vf_slot;
            why = NULL;
            vf34 = 1;
        }                       // (ps44 R5b: held past the pair's end until the downed camera settles)
        else {
            g_s34.vf = 0;
            g_s39.xstop = 0;
            why =
                now >=
                g_s34.
                vf_until ? "the takedown on us is over" :
                "the takedown's shot was taken over";
            if (role != 2)
                role = 0;
        }
    } else if (!role && g_pc5.on && g_pc5.role == 1 && g_s34Tb
               && now < g_s34.keep && !g_xcamOn) {
        role = 1;
        slot = g_pc5.slot;
        why = NULL;
    }                           // a pin finisher: the pin's shot is held
    if (role && ((g_downed && !vf34) || MatchSubstate() != 6)) {
        why = g_downed ? "we are down" : "not in the match";
        role = 0;
    }
    float pb[16], ab[16];
    const float *me = role ? PlayerLocator(pb) : NULL;
    uint64_t h = role ? PuppetHandle((uint32_t) slot) : 0;
    const float *ot = h ? PuppetLocator(h, ab) : NULL;
    if (role
        && (!me || !ot || !PsLocOk(me) || !PsLocOk(ot)
            || g_peerDowned[slot] || g_puppetDeadHere[slot])) {
        why = "the other body is gone / down";
        role = 0;
    }
    // a takedown / escape / down camera takes over: the pin camera freezes and fades with its blend-in, xcam blends from this view (icam_filter, g_pcam.ho)
    if (xon39 && (g_pc5.ho || (g_pc5.on && g_pcam.w > 0.002f))) {
        // (pincam2: the pin camera stays frozen - not fading toward the game camera, which moved the base under the takedown camera's own blend -
        //  until that camera covers it fully; then it lets go: nothing of it is on screen any more)
        if (!g_pc5.ho) {
            g_pc5.ho = 1;
            Xlog("pincam5: hand-over to the takedown / escape camera (the pin camera frozen under its blend-in)");
        }
        g_pcam.freeze = 1;
        g_pcam.ho = 1;
        if (g_xcam_w < 0.995f) {
            g_gshAny = 1;
            return;
        }
        g_pcam.w = 0.0f;
        g_pcam.ho = 0;
        g_pcam.freeze = 0;
        g_pc5.ho = 0;
        if (g_pc5.on)
            Spc5Done("handed over to the takedown / escape camera");
        return;
    }
    if (g_pc5.ho) {
        g_pc5.ho = 0;
        g_pcam.freeze = 0;
    }                           // that camera stopped before covering us: in / out from the frozen pose
    if (xon39 && role) {
        role = 0;
        why = "a takedown / escape camera runs";
    }
    if (!g_s34.vf)
        g_s39.xstop = 0;
    if (role && !g_pc5.on) {
        Pc5Drain();
        g_pc5.on = 1;
        g_pc5.role = role;
        g_pc5.slot = slot;
        g_pc5.t_on = now;
        g_pc5.init = 0;
        g_pc5.hold = 0;
        g_pc5.swaps = 0;
        g_pc5.why = NULL;
        g_pc5.n = g_pc5.arm_blk = g_pc5.lat_blk = g_pc5.los_blk =
            g_pc5.both_blk = 0;
        g_pc5.min_a = g_pc5.min_v = 99.0f;
        g_pc5.min_frac = 1.0f;
        g_pc5.max_raise = 0.0f;
        for (int k = 0; k < 2; k++)
            for (int i = 0; i < 3; i++)
                g_pc5.rt[k][i] = 1.0f;
        g_pc5.frac = 1.0f;
        g_pc5.raise = 0.0f;
        g_pc5.tight = 1.0f;
        g_pc5.latm = 1.0f;
        g_pc5.blow0 = g_mfkBlows;
        // ps34: the victim's shot (block comment at S34_*): the timed blend, the kind / side decided once
        g_s34Tb = role == 1 && (vf34 || (g_ps34Pin && MpnPinnedNow()));
        g_pc5.v34 = vf34;
        g_pc5.v34w = g_s34Tb;
        g_pc5.hi = 0;
        g_pc5.pk_side = 0;
        g_pc5.pk_ok = 0;
        g_s34.keep = 0;
        g_s34.n_dec0 = 0;
        g_s34.tin = vf34 ? S34_VF_IN : S34_PIN_IN;
        g_s34.tout = vf34 ? S34_VF_OUT : S34_PIN_OUT;
        g_s34.warned = 0;
        g_s34.bad_at = 0;
        g_s34.hr_bad_at = 0;
        g_s34.hr_blk = 0;
        g_s34.chk_at = now;
        g_s34.vfD = 0.0f;
        g_s34.ahy = 0.0f;
        g_s34.was_vf = vf34;
        g_s34.dec_ms = now;
        if (g_s34.hr_job) {
            RayT(g_s34.hr_job);
            g_s34.hr_job = 0;
        }
        g_s39.e_ok = 0;
        if (g_s39.pjob) {
            RayT(g_s39.pjob);
            g_s39.pjob = 0;
        }
        g_s39w.ok = 0;
        g_s39w.told = 0;
        g_s39Bok = 0;
        if (!vf34)
            g_s39.vf_wall = 0;  // ps39
        // v5-cam5 (A/B v5-int18, symptom: a pin teleported both cameras miles out of bounds; B 313728 'duel framing -> pin camera
        // (would step 51.7 deg / 539.99 m)', A 311230 548.36 m, 316806 520.42 m, 317314 508.44 m; every such pin's DONE line 'closest 9.00 m', 'the way out
        // did not reach the game camera within 4 s'; bodies never moved - bb: positions steady): spc_apply5 restarted its follower only when it saw w == 0,
        // but the render thread calls it only while w > 0 (gsh_filter: 'if (g_pcam.w > 0.001f) spc_apply'), so after the match's first pin its static
        // follower state (P / A / R / Y / Q) never reset - the next pin resumed from where the LAST pin's camera was (here a pin of the previous match on
        // another map, ~540 m away) and crawled back at 0.08 m/frame, never arriving; cam2 then blended the screen out to it over 1.2 s (10-12 m a frame).
        // A new pin from nothing now bumps gen, and the follower starts on the incoming camera again.
        if (!(g_pcam.w > 0.002f)) {
            g_pcam.sgn = 0;
            g_pcam.gen++;
        }
        g_pcam.st_reset = 1;
        g_pcam.out_done = 0;
        g_pcam.freeze = 0;
        g_pc5.t_out = 0;
    }
    if (!g_pc5.on) {
        g_pcam.ho = 0;
        g_pcam.want = 0;
        if (g_pcam.w > 0.0f)
            g_pcam.w = 0.0f;
        return;
    }
    if (role) {
        g_pc5.why = NULL;
        g_pc5.role = role;
        g_pc5.slot = slot;
        if (g_s34Tb && role == 2) {
            g_s34Tb = 0;
            g_pc5.v34 = 0;
            g_pc5.v34w = 0;
            g_pc5.hi = 0;
            g_pcam.gen++;
        }                       // ps34: we became the pinner while the victim's shot was still on its way out - the pinner's camera is the old one, restarted
        {
            float s = (Spc5SpOld()
                       || g_s34Tb) ? 0.0f : fminf(1.0f,
                                                   (float) (now -
                                                            g_pc5.t_on) /
                                                   (PC5_CLOSE_S *
                                                    1000.0f)), sm =
                s * s * (3.0f - 2.0f * s);
            g_pc5.tight = 1.0f - PC5_CLOSE * sm;
            g_pc5.latm = g_s34Tb ? S34_OS_LAT / PC5_LAT : 1.0f;
        }                       // (SP: no widening - 0.75 m beside through the whole pin; ps34: the victim's shot does not close in and sits S34_OS_LAT beside)
        const float *A = role == 2 ? me : ot, *V = role == 2 ? ot : me;
        g_pc5.ign = role == 1 ? h : 0;  // the attacker's body: the puppet on the victim's screen, us (SID_PLAYER, the default) on ours
        float dx = V[0] - A[0], dz = V[2] - A[2], d =
            __builtin_sqrtf(dx * dx + dz * dz);
        if (d > 0.2f) {
            dx /= d;
            dz /= d;
        } else {
            dx = g_pc5.init ? g_pc5.fx : 0.0f;
            dz = g_pc5.init ? g_pc5.fz : 1.0f;
            d = 0.2f;
        }
        // ps39 C1 (rec38a, the wall pin at clip 105.8-106.6: B 16524 'ps34 shot decided ... LEFT side', camrec 16533-16548 'lat -0.58 .. -1.42 back
        // +0.93 .. -2.01' = the victim's blend ran round the pair THROUGH the panel beside it (3 black frames, then a shot from behind the panel); A 14458
        // 'pincam5: DONE as the pinner ... closest 0.51 m to the attacker ... arm ray blocked 16 frames (pulled in to 3 %)' = the pinner's lens slid along
        // its arm to the pivot beside its own head, the panel over the picture): the pin's wall frame (the plan's anchor: on the surface, facing out of
        // it - ctxdown.c) gives a plane; the target lens stays S39_WALL_OFF in front of it here and the shown lens S39_WALL_REN on the render thread
        // (s34_apply / spc_apply5), every frame. The normal is taken toward the two bodies; a frame the bodies do not stand in front of is not used.
        float wp[2] = { 0.0f, 0.0f }, wn[2] = { 0.0f, 0.0f };
        int wok = 0;
        if (g_ps39[PS39_WALL] && (!g_s34.vf || g_s39.vf_wall)
            && S39PinWall(g_s34.vf ? 3 : role, wp, wn)) {
            float dA =
                (A[0] - wp[0]) * wn[0] + (A[2] - wp[1]) * wn[1], dV =
                (V[0] - wp[0]) * wn[0] + (V[2] - wp[1]) * wn[1];
            if (dA + dV < 0.0f) {
                wn[0] = -wn[0];
                wn[1] = -wn[1];
                dA = -dA;
                dV = -dV;
            }
            wok = dA + dV > 0.15f && dA > -0.3f && dA < 4.0f && dV < 4.0f && (V[0] - wp[0]) * (V[0] - wp[0]) + (V[2] - wp[1]) * (V[2] - wp[1]) < 2.25f; // (the anchor is the victim's spot on the surface: a frame further than 1.5 m from the victim is an old pin's)
            if (wok) {
                g_s39w.nx = wn[0];
                g_s39w.nz = wn[1];
                g_s39w.px = wp[0];
                g_s39w.pz = wp[1];
                MemoryBarrier();
                g_s39w.ok = 1;
            }
        }
        if (!wok && g_s39w.ok && g_ps39[PS39_WALL]) {
            wn[0] = g_s39w.nx;
            wn[1] = g_s39w.nz;
            wp[0] = g_s39w.px;
            wp[1] = g_s39w.pz;
            wok = 1;
        }                       // (the plane of this pin is kept between its pairs)
        // ps39 C8 (B 12493 'ps34 ON as the pinned victim (slot 1, the pair 0.20 m apart)', clip B 38.70-39.17): two roots 0.2 m apart give no line - the
        // shot's 'behind the attacker' pointed anywhere. Against a wall the line attacker -> victim is INTO the wall: the plane's normal, until the pair is 0.45 m apart.
        if (wok && g_ps39[PS39_VIC] && g_s34Tb && !g_s34.vf && d < 0.45f) {
            dx = -wn[0];
            dz = -wn[1];
            if (!g_pc5.init && LOGCAP(g_s34.nlog, 200))
                Xlog("pincam5: ps39 - the pair is %.2f m apart: the shot's line is the wall's normal (into the wall), not the line between the two roots (frame %llu)", (double) d, (unsigned long long) g_frame);
        }
        if (!g_pc5.init) {
            g_pc5.fx = dx;
            g_pc5.fz = dz;
            g_pc5.sax = A[0];
            g_pc5.say = A[1];
            g_pc5.saz = A[2];
            g_pc5.sd = d;
        } else {
            float k = 1.0f - __builtin_expf(-dt / (g_s34Tb ? 0.45f : 0.25f)), k2 = 1.0f - __builtin_expf(-dt / (g_s34Tb ? 0.35f : 0.15f));    // (ps34: the victim's shot rides a slower point - pin-blow motion does not shake it)
            g_pc5.fx += (dx - g_pc5.fx) * k;
            g_pc5.fz += (dz - g_pc5.fz) * k;
            float l =
                __builtin_sqrtf(g_pc5.fx * g_pc5.fx + g_pc5.fz * g_pc5.fz);
            if (l > 0.1f) {
                g_pc5.fx /= l;
                g_pc5.fz /= l;
            } else {
                g_pc5.fx = dx;
                g_pc5.fz = dz;
            }
            g_pc5.sax += (A[0] - g_pc5.sax) * k2;
            g_pc5.say += (A[1] - g_pc5.say) * k2;
            g_pc5.saz += (A[2] - g_pc5.saz) * k2;
            g_pc5.sd += (d - g_pc5.sd) * k2;
        }
        float rx = -g_pc5.fz, rz = g_pc5.fx;
        float L[3] =
            { g_pc5.sax + g_pc5.fx * 0.62f * g_pc5.sd, g_pc5.say + 1.2f,
       g_pc5.saz + g_pc5.fz * 0.62f * g_pc5.sd };
        if (g_s34Tb) {         // ps34 aim: the victim (over the shoulder), the pair's midpoint (the pin's side two-shot), our own chest joint (the finisher shot)
            float lk = g_pc5.v34 ? 0.5f : 1.0f, lh =
                g_pc5.v34 ? S34_PIN_LOOK : S34_OS_LOOK;
            L[0] = g_pc5.sax + g_pc5.fx * lk * g_pc5.sd;
            L[1] = g_pc5.say + lh;
            L[2] = g_pc5.saz + g_pc5.fz * lk * g_pc5.sd;
            if (g_s34.vf) {
                float p3[3], c3[3], h3[3];
                XvBody(SID_PLAYER, me, p3, c3, h3);
                if (XvFin3(c3) && __builtin_fabsf(c3[1] - me[1]) < 2.0f) {
                    float kc =
                        g_s34.lk_ok ? 1.0f -
                        __builtin_expf(-dt / 0.25f) : 1.0f;
                    for (int i = 0; i < 3; i++)
                        g_s34.lk[i] +=
                            (c3[i] -
                             (g_s34.lk_ok ? g_s34.lk[i] : 0.0f)) * kc -
                            (g_s34.lk_ok ? 0.0f : g_s34.lk[i]);
                    g_s34.lk_ok = 1;
                }
                if (g_s34.lk_ok) {
                    L[0] = g_s34.lk[0];
                    L[1] = g_s34.lk[1];
                    L[2] = g_s34.lk[2];
                }
                // ps35 (rec23 B frame checks of the finisher side shot: 9936 'our head x -0.22 y -0.14, the attacker's head x -0.60 y +1.87', 14263 'the
                // attacker's head x -1.01 y +0.95', 18555 'x +1.90 y +2.04', 11924 'our head x +1.48 y -2.36 ... lens 0.84 m from our axis'; one of five in
                // frame): the shot stood a fixed 1.3-1.9 m out, pushed IN, and aimed at our chest alone - as we fell the aim went down and the standing
                // attacker left the top / side of the frame. The distance is fitted to both bodies now: wide enough for the pair's length + 0.55 m each
                // side inside 80 % of the frame's width, and far enough that the attacker's head and our chest fit between y +0.70 and -0.55 (0.60 = the
                // tangent of that span at the 75 fov); never under 1.6 m, eased (tau 0.5 s) - it backs off as we go down instead of pushing in.
                {
                    float q3[3], d3[3], a3[3], ahy = ot[1] + 1.62f;
                    XvBody(h, ot, q3, d3, a3);
                    if (XvFin3(a3)
                        && __builtin_fabsf(a3[1] - ot[1]) < 2.2f)
                        ahy = a3[1];
                    g_s34.ahy = g_s34.ahy > 0.0f
                        || g_s34.ahy <
                        0.0f ? g_s34.ahy + (ahy - g_s34.ahy) * (1.0f -
                                                                __builtin_expf
                                                                (-dt /
                                                                 0.2f)) :
                        ahy;
                    float lowy = g_s34.lk_ok
                        && g_s34.lk[1] <
                        g_pc5.say + 1.0f ? g_s34.lk[1] : g_pc5.say + 1.0f;
                    float Dv = (g_s34.ahy - lowy + 0.30f) / 0.60f, Dh =
                        (g_pc5.sd + 1.10f) / 1.155f, D = Dv > Dh ? Dv : Dh;
                    D = D < 1.60f ? 1.60f : D > 3.00f ? 3.00f : D;
                    if (g_ps39[PS39_VIC]) {     // ps39 C4: fitted ONCE, before the rays and the cut, for where the move ENDS (us on the ground 0.25 m up, 0.8 m of fall travel) - the lens then holds
                        if (!(g_s34.vfD > 1.0f)) {
                            float De =
                                (g_s34.ahy - (g_pc5.say + 0.25f) +
                                 0.30f) / 0.60f, Dl =
                                (g_pc5.sd + 1.90f) / 1.155f;
                            D = De > Dl ? De : Dl;
                            g_s34.vfD =
                                D < 2.00f ? 2.00f : D > 3.00f ? 3.00f : D;
                        }
                    } else
                        g_s34.vfD =
                            g_s34.vfD >
                            1.0f ? g_s34.vfD + (D - g_s34.vfD) * (1.0f -
                                                                  __builtin_expf
                                                                  (-dt /
                                                                   0.5f)) :
                            D;
                }
            }
        }
        if (!g_pc5.init) {      // the shoulder the game camera is on now (the shortest way in)
            float sx = g_pcam.px - g_pc5.sax, sz = g_pcam.pz - g_pc5.saz;
            g_pc5.side = (sx * rx + sz * rz) >= 0.0f ? 0 : 1;
            g_pc5.latk = g_pc5.side ? -1.0f : 1.0f;
            g_pc5.init = 1;
            if (g_s34Tb)
                Xlog("pincam5: ps34 ON as the %s (slot %d, the pair %.2f m apart): %s, the side is decided from the first rays, then one continuous shot (in %.0f ms, out %.0f ms; frame %llu)", g_s34.vf ? "victim of a takedown" : "pinned victim", slot, (double) d, g_s34.vf ? "side shot of the victim" : "over the attacker's shoulder at the victim", (double) (g_s34.tin * 1000.0f), (double) (g_s34.tout * 1000.0f), (unsigned long long) g_frame);
            else
                Xlog("pincam5: ON as %s (slot %d, the pair %.2f m apart): over the attacker's %s shoulder (the game camera's side), %.2f m behind / %.2f m beside / %.2f m up, follower in / out <= %.1f deg / %.2f m per frame", role == 2 ? "the pinner" : "the pinned victim", slot, (double) d, g_pc5.side ? "left" : "right", (double) PC5_BACK, (double) PC5_LAT, (double) PC5_UP, (double) PC5_STEP_DEG, (double) PC5_STEP_M);
        }
        // rays: read the shoulder kicked last frame, kick the other one (one shoulder per frame, each answer read on the next frame)
        if (g_pc5.kside >= 0)
            Pc5Take(g_pc5.kside, 2.0f * dt);
        g_pc5.kside = g_pc5.kside == 0 ? 1 : 0;
        Pc5Kick(g_pc5.kside, L);
        float sc[2];
        for (int k = 0; k < 2; k++)
            sc[k] =
                g_pc5.rt[k][0] + 0.35f * g_pc5.rt[k][1] +
                0.6f * g_pc5.rt[k][2];
        if (g_ps41v && g_s34.vf && g_s34Tb && role == 1) {     // ps41 V: the attacker's torso between each side's lens and us; our front
            const float *q9 = me + 4;
            float f0 = 2.0f * (q9[0] * q9[2] + q9[3] * q9[1]), f2 =
                1.0f - 2.0f * (q9[0] * q9[0] + q9[1] * q9[1]), fl =
                __builtin_sqrtf(f0 * f0 + f2 * f2);
            for (int k = 0; k < 2; k++) {
                float P9[3], C9[3];
                S34SideSpot(k, 0.0f, P9, C9);
                float dx9 = C9[0] - me[0], dz9 = C9[2] - me[2], dl9 =
                    __builtin_sqrtf(dx9 * dx9 + dz9 * dz9);
                g_s41v.occ[k] = S41vOcc(C9, me, ot, h);
                g_s41v.fr[k] = fl > 0.2f
                    && dl9 >
                    0.2f ? (dx9 * f0 + dz9 * f2) / (fl * dl9) : 0.0f;
                sc[k] +=
                    S41V_FRONT * g_s41v.fr[k] - S41V_PEN * g_s41v.occ[k];
            }
        }
        int o = 1 - g_pc5.side;
        if (g_s34Tb) {         // ps34: decided once, never swapped
            int w41 = g_pc5.v34w;
            if (!
                (g_s34.vf && g_pc5.v34 && g_pc5.v34w == 1
                 && S43VicStep(now, g_pc5.t_on, me, ot, h, wok, wp, wn,
                                 g_pc5.fx, g_pc5.fz, g_s34.vfD, sc)))
                S34Decide(sc, now);    // ps43: the finisher side shot's spot is planned from the clip's placement first
            g_pc5.hold = 0;
            if (w41 && !g_pc5.v34w && g_ps41v && g_s34.vf) {
                g_s41v.flipped = 0;
                g_s41v.told = 0;
                g_s41v.hid_at = 0;
                g_s41v.cut_req = 0;
                if (LOGCAP(g_s34.nlog, 200))
                    Xlog("pincam5: ps41 side by occlusion - the attacker's torso (capsule %.2f m) crosses %.0f %% of the lines lens -> our head / chest from its RIGHT side, %.0f %% from its LEFT; our front toward the right lens %+.2f / the left %+.2f; scores right %.2f / left %.2f -> the %s side (the pair as placed in the first ticks; a fixed lens that loses us for > 0.3 s takes ONE cut to the other side; frame %llu)", (double) S41V_CAP_R, (double) (g_s41v.occ[0] * 100.0f), (double) (g_s41v.occ[1] * 100.0f), (double) g_s41v.fr[0], (double) g_s41v.fr[1], (double) sc[0], (double) sc[1], g_pc5.side ? "LEFT" : "RIGHT", (unsigned long long) g_frame);
            }
        }
        if (g_s34.vf && g_s34Tb && role == 1)
            S44vStep(now, g_pc5.fx, g_pc5.fz); // ps44 R5a late: a running re-plan
        if (g_ps41v && g_s34.vf && g_s34Tb && role == 1 && !g_pc5.v34w && g_s39.e_ok && now - g_s34.dec_ms > 150) {    // ps41 V: the one late correction
            float oc9 = S41vOcc(g_s39.E, me, ot, h);
            int cb9 = S44vChest(now, g_s39.E, me, h), hid9 = g_s34.hr_blk || oc9 >= 0.99f || cb9;      // (ps44 R5c: + the chest ray, 0.3 s)
            if (!hid9)
                g_s41v.hid_at = 0;
            else if (!g_s41v.hid_at)
                g_s41v.hid_at = now;
            if (g_s41v.hid_at && now - g_s41v.hid_at > 300
                && !g_s41v.flipped && g_s44vr.st != 3
                && S44vStart(now, me, ot, h, wok, wp, wn,
                              g_s34.
                              hr_blk ?
                              "the lens -> our head ray is BLOCKED" : cb9 ?
                              "the lens -> our chest ray is BLOCKED (geometry)"
                              :
                              "the attacker stands between the lens and us"))
            {
                g_s41v.flipped = 1;
                g_s41v.hid_at = 0;
            }
            if (g_s41v.hid_at && now - g_s41v.hid_at > 300
                && !g_s41v.flipped) {
                int o9 = 1 - g_pc5.side, ok9 = g_s41v.occ[o9] < 0.5f
                    && g_pc5.rt[o9][0] >= S34_ARM_MIN
                    && g_pc5.rt[o9][2] >= 0.5f;
                if (ok9) {
                    const char *wy9 =
                        g_s34.
                        hr_blk ? "the lens -> our head ray is BLOCKED" :
                        "the attacker stands between the lens and us";
                    g_s41v.flipped = 1;
                    g_s41v.hid_at = 0;
                    g_pc5.side = o9;
                    g_pc5.latk = o9 ? -1.0f : 1.0f;
                    g_pc5.frac = g_pc5.rt[o9][0];
                    g_s39.e_ok = 0;
                    g_s41v.cut_req = 1;
                    g_s34.dec_ms = now;
                    g_s34.hr_bad_at = 0;
                    g_s34.bad_at = 0;
                    g_s34.hr_blk = 0;
                    g_s34.warned = 0;
                    Xlog("pincam5: ps41 - the fixed side shot lost us for > 0.3 s (%s; the attacker's torso on %.0f %% of the lines to our head / chest): ONE cut to the attacker's %s side, once per shot - the lens is re-fitted there and the shot's in-time restarts (that side: torso %.0f %%, arm %.2f, sight %.2f; %llu ms into the shot; frame %llu)", wy9, (double) (oc9 * 100.0f), o9 ? "LEFT" : "RIGHT", (double) (g_s41v.occ[o9] * 100.0f), (double) g_pc5.rt[o9][0], (double) g_pc5.rt[o9][2], (unsigned long long) (now - g_pc5.t_on), (unsigned long long) g_frame);
                } else if (!g_s41v.told) {
                    g_s41v.told = 1;
                    Xlog("pincam5: ps41 - the fixed side shot lost us for > 0.3 s but the other side is no better (torso %.0f %%, arm %.2f, sight %.2f): the shot stays (frame %llu)", (double) (g_s41v.occ[o9] * 100.0f), (double) g_pc5.rt[o9][0], (double) g_pc5.rt[o9][2], (unsigned long long) g_frame);
                }
            }
        }
        if (g_s39.xstop && g_s34Tb && !g_pc5.v34w) {
            g_s39.xstop = 0;
            if (g_xcamOn)
                XcamHardStop
                    ("ps39: a finisher on us - the victim's side shot cuts in");
        } else if (sc[o] > sc[g_pc5.side] + 0.35f) {
            if (++g_pc5.hold > (now - g_pc5.t_on < 400 ? 12 : 24)) {
                g_pc5.side = o;
                g_pc5.hold = 0;
                g_pc5.swaps++;
                Xlog("pincam5: to the attacker's %s shoulder after %llu ms (right: arm %.2f beside %.2f sight %.2f | left: arm %.2f beside %.2f sight %.2f)", o ? "left" : "right", (unsigned long long) (now - g_pc5.t_on), (double) g_pc5.rt[0][0], (double) g_pc5.rt[0][1], (double) g_pc5.rt[0][2], (double) g_pc5.rt[1][0], (double) g_pc5.rt[1][1], (double) g_pc5.rt[1][2]);
            }
        } else
            g_pc5.hold = 0;
        if (g_s34Tb)
            g_pc5.latk = g_pc5.side ? -1.0f : 1.0f;
        else
            g_pc5.latk +=
                ((g_pc5.side ? -1.0f : 1.0f) - g_pc5.latk) * (1.0f -
                                                              __builtin_expf
                                                              (-dt /
                                                               0.3f));
        float mid = 1.0f - __builtin_fabsf(g_pc5.latk);
        mid = mid < 0.0f ? 0.0f : mid;
        float P[3], C[3];
        Pc5Spot(g_pc5.side, PC5_LAT * g_pc5.latm * g_pc5.latk, mid, P, C);
        float al =
            __builtin_sqrtf((C[0] - P[0]) * (C[0] - P[0]) +
                            (C[1] - P[1]) * (C[1] - P[1]) + (C[2] -
                                                             P[2]) *
                            (C[2] - P[2])), tg = g_pc5.rt[g_pc5.side][0];
        if (tg < g_pc5.frac)
            g_pc5.frac +=
                (tg - g_pc5.frac) * (1.0f - __builtin_expf(-dt / 0.06f));
        else
            g_pc5.frac =
                fminf(tg,
                      g_pc5.frac + dt * 1.2f / (al > 0.5f ? al : 0.5f));
        float E[3] =
            { P[0] + (C[0] - P[0]) * g_pc5.frac,
       P[1] + (C[1] - P[1]) * g_pc5.frac,
       P[2] + (C[2] - P[2]) * g_pc5.frac };
        // ps35 (rec23 A, the one victim pin: 15272 'ps34 shot decided ... RIGHT side (right: arm 1.00 beside 0.34 ... | left: arm 0.48 ... sight 0.37)',
        // 15356 'DONE ... closest 0.51 m to the attacker ..., lifted up to 0.39 m, arm ray blocked 52 frames (pulled in to 2 %)', 15312 'frame check -
        // WARNING: the ATTACKER's head out of frame (... the attacker's head x +9.99 y +9.99, lens 1.13 m from our axis / 9.00 m from the attacker's': the
        // attacker was valid - 9.00 is the check's 'lens above its head'): a wall came into the chosen shoulder's arm, the lens slid along the arm to its
        // pivot 0.5 m beside the attacker's HEAD and the body floor then lifted it over that head, looking down past the attacker at our legs. A blocked
        // arm now takes the victim's over-the-shoulder lens toward the spot straight behind the attacker and 0.30 m up (over its head at the victim),
        // 0.95 m off its axis the whole way - never toward the body.
        // (ps39 C1: the pinner's camera the same way - its lens went along the arm to 0.5 m beside its own head, A 14458 'pulled in to 3 %')
        if (((g_s34Tb && !g_pc5.v34) || (!g_s34Tb && g_ps39[PS39_WALL]))
            && g_pc5.frac < 1.0f) {
            float bk39 = g_s34Tb ? S34_OS_BACK : PC5_BACK * g_pc5.tight;
            float B0[3] =
                { g_pc5.sax - g_pc5.fx * bk39, C[1] + 0.30f,
          g_pc5.saz - g_pc5.fz * bk39 };
            for (int i = 0; i < 3; i++)
                E[i] = C[i] + (B0[i] - C[i]) * (1.0f - g_pc5.frac);
        }
        // never inside a body: too near an axis, the lens rises over the heads instead
        float da =
            __builtin_sqrtf((E[0] - g_pc5.sax) * (E[0] - g_pc5.sax) +
                            (E[2] - g_pc5.saz) * (E[2] - g_pc5.saz)), dv =
            __builtin_sqrtf((E[0] - V[0]) * (E[0] - V[0]) +
                            (E[2] - V[2]) * (E[2] - V[2]));
        float fla = g_s34Tb ? (g_s34.vf ? 1.20f : g_pc5.v34 ? S34_MIN_BODY : 0.80f) : PC5_MIN_A, flv = g_s34Tb ? (g_s34.vf ? 1.20f : S34_MIN_BODY) : PC5_MIN_V;       // (ps35: the finisher shot keeps 1.2 m off both axes - a blocked arm RAISES it)      // (ps34: the victim's shot stands 1.27 m from the attacker by construction; nearer = pulled in by a wall)
        float rs = da < fla ? (fla - da) * 1.4f : 0.0f;
        if (dv < flv && (flv - dv) * 1.4f > rs)
            rs = (flv - dv) * 1.4f;
        if (rs > 0.9f)
            rs = 0.9f;
        g_pc5.raise +=
            (rs - g_pc5.raise) * (1.0f -
                                  __builtin_expf(-dt /
                                                 (rs >
                                                  g_pc5.
                                                  raise ? 0.05f : 0.25f)));
        E[1] += g_pc5.raise + (g_pc5.hi ? S34_HI : 0.0f);
        if (wok) {              // ps39 C1: never behind / inside the pin's wall
            float dw = (E[0] - wp[0]) * wn[0] + (E[2] - wp[1]) * wn[1];
            if (dw < S39_WALL_OFF) {
                E[0] += wn[0] * (S39_WALL_OFF - dw);
                E[2] += wn[1] * (S39_WALL_OFF - dw);
                if (!g_s39w.told) {
                    g_s39w.told = 1;
                    static int nlw;
                    if (LOGCAP(nlw, 120))
                        Xlog("pincam5: ps39 wall plane - the %s lens target stood %.2f m %s the pin's wall plane: kept %.2f m in front of it, tested every frame (plane at %.2f %.2f, normal %.2f %.2f; frame %llu)", role == 2 ? "pinner's" : g_s34.vf ? "finisher side shot's" : "pinned victim's", (double) __builtin_fabsf(dw), dw < 0.0f ? "BEHIND" : "in front of", (double) S39_WALL_OFF, (double) wp[0], (double) wp[1], (double) wn[0], (double) wn[1], (unsigned long long) g_frame);
                }
            }
        }
        // ps39 C4: the finisher side shot's lens is where the decision put it, to the end ("hold still": the lens rode the smoothed attacker / the pair's
        // line / a re-fitted distance - B camrec 17379-17520 'dist 2.83 -> 2.64 -> 3.50' through the move); only the aim follows our body
        if (g_ps39[PS39_VIC] && g_s34.vf && !g_pc5.v34w) {
            if (!g_s39.e_ok) {
                S43VicLens(g_pc5.t_on, E);
                memcpy(g_s39.E, E, sizeof g_s39.E);
                g_s39.e_ok = 1;
                if (LOGCAP(g_s34.nlog, 200))
                    Xlog("pincam5: ps39 - the finisher side shot's lens is fixed at %.2f %.2f %.2f (%.2f m from the pair's midpoint, fitted once for the end of the move) and holds to the ease-out (frame %llu)", (double) E[0], (double) E[1], (double) E[2], (double) g_s34.vfD, (unsigned long long) g_frame);
            } else
                memcpy(E, g_s39.E, sizeof g_s39.E);
        }
        // ps35: the finisher shot's AIM, from both bodies and the lens it has: 65 % our chest / 35 % the pair's midpoint, the axis kept where the attacker's
        // head is not above y +0.70 and our chest not below y -0.55 (the middle of the two when both cannot hold)
        if (g_s34.vf && g_s34.lk_ok && g_s34.ahy != 0.0f) {
            float mxp = g_pc5.sax + g_pc5.fx * 0.5f * g_pc5.sd, mzp =
                g_pc5.saz + g_pc5.fz * 0.5f * g_pc5.sd;
            float ax = 0.65f * g_s34.lk[0] + 0.35f * mxp, az =
                0.65f * g_s34.lk[2] + 0.35f * mzp, ay =
                0.65f * g_s34.lk[1] + 0.35f * (g_pc5.say + 1.0f);
            float dh =
                __builtin_sqrtf((ax - E[0]) * (ax - E[0]) +
                                (az - E[2]) * (az - E[2]));
            float dA =
                __builtin_sqrtf((g_pc5.sax - E[0]) * (g_pc5.sax - E[0]) +
                                (g_pc5.saz - E[2]) * (g_pc5.saz - E[2])),
                dC =
                __builtin_sqrtf((g_s34.lk[0] - E[0]) * (g_s34.lk[0] -
                                                        E[0]) +
                                (g_s34.lk[2] - E[2]) * (g_s34.lk[2] -
                                                        E[2]));
            if (dh > 0.3f && dA > 0.3f && dC > 0.3f) {
                float fvh = g_mfHold
                    && g_mf_held > 30.0f ? g_mf_held : g_mfBase >
                    30.0f ? g_mfBase : 70.0f, tv =
                    __builtin_tanf(fvh * 0.5f * 0.0174533f) * 9.0f / 16.0f;
                float thA = __builtin_atan2f(g_s34.ahy - E[1], dA), thC =
                    __builtin_atan2f(g_s34.lk[1] - E[1], dC), a =
                    __builtin_atan2f(ay - E[1], dh);
                float lo = thA - __builtin_atanf(0.70f * tv), hi =
                    thC + __builtin_atanf(0.55f * tv);
                if (lo > hi)
                    a = 0.5f * (lo + hi);
                else
                    a = a < lo ? lo : a > hi ? hi : a;
                L[0] = ax;
                L[1] = E[1] + __builtin_tanf(a) * dh;
                L[2] = az;
                // ps37 (rec35a A 11117 'the attacker's head x +1.01 y +0.87, lens 1.83 m ...', 13554 'x +1.14 y +1.07, lens 3.00 m' - ps35's fitted
                // distance and vertical clamp were in; the aim's AZIMUTH was not clamped: aimed 65 % at our chest with us lying away from the attacker, the
                // attacker stood past the frame's side, which also lifts its y): the axis is kept where the attacker's head and our chest are both
                // inside x +-0.75 (the middle of the two when they do not both fit).
                {
                    float th =
                        __builtin_tanf(fvh * 0.5f * 0.0174533f), lim =
                        __builtin_atanf(0.75f * th);
                    float zA =
                        __builtin_atan2f(g_pc5.sax - E[0],
                                         g_pc5.saz - E[2]), zC =
                        __builtin_atan2f(g_s34.lk[0] - E[0],
                                         g_s34.lk[2] - E[2]), zL =
                        __builtin_atan2f(ax - E[0], az - E[2]);
                    float dAC = __builtin_remainderf(zA - zC, 6.2831853f), zlo = dAC > 0.0f ? dAC - lim : -lim, zhi = dAC > 0.0f ? lim : dAC + lim;     // (the axis's azimuth relative to our chest's)
                    float zr = __builtin_remainderf(zL - zC, 6.2831853f);
                    if (zlo > zhi)
                        zr = 0.5f * dAC;
                    else
                        zr = zr < zlo ? zlo : zr > zhi ? zhi : zr;
                    L[0] = E[0] + __builtin_sinf(zC + zr) * dh;
                    L[2] = E[2] + __builtin_cosf(zC + zr) * dh;
                }
            }
        }
        if (g_s34Tb && !g_pc5.v34w
            && now - g_s34.dec_ms > (uint64_t) (g_s34.tin * 1000.0f) + 150)
            S34Check(now, me, ot);     // ps34 self-check (the shown camera), once the shot is in (ps35: it ran through the blend-in)
        if (g_s34.vf && g_s44o.st == 2 && g_s44o.tpb && g_s44o.vt0 == g_s34.vf_t0 && g_s39Bok) {       // ps44 R5b: the held shot pulls back and looks at our own chest
            float u = (float) (now - g_s44o.tpb) / 500.0f;
            u = u > 1.0f ? 1.0f : u;
            u = u * u * (3.0f - 2.0f * u);
            float bx = E[0] - g_s39B[0], bz = E[2] - g_s39B[2], bl =
                __builtin_sqrtf(bx * bx + bz * bz);
            if (bl > 0.2f) {
                E[0] += bx / bl * S44O_BACK * u;
                E[2] += bz / bl * S44O_BACK * u;
                E[1] += 0.30f * u;
            }
            for (int i = 0; i < 3; i++)
                L[i] += (g_s39B[i] - L[i]) * u;
        }
        if (g_s34.vf && g_ps44[PS44_FOV] && !(g_ps44m && g_pc5.v34w)) { // ps44 R10 (R13: from the move's first frame, not while the shot is decided) (the note at S44F_*): the cinematic fov from the shown lens and both bodies
            float p9[3], c9[3], h9[3], hx = g_pc5.sax, hz =
                g_pc5.saz, top = g_s34.ahy + S44F_HEAD, low =
                g_s39Bok ? g_s39B[1] : me[1] + 0.9f;
            XvBody(h, ot, p9, c9, h9);
            if (XvFin3(p9) && __builtin_fabsf(p9[1] - ot[1]) < 2.2f
                && p9[1] < low)
                low = p9[1];
            low -= S44F_FOOT;
            float mx9 =
                0.5f * (hx + (g_s39Bok ? g_s39B[0] : me[0])), mz9 =
                0.5f * (hz + (g_s39Bok ? g_s39B[2] : me[2])), Dd =
                __builtin_sqrtf((E[0] - mx9) * (E[0] - mx9) +
                                (E[2] - mz9) * (E[2] - mz9));
            if (Dd > 0.8f && top > low + 0.3f) {
                float tvn = (top - low) / S44F_FILL * 0.5f / Dd, thn =
                    (g_pc5.sd + 1.10f) / 0.85f * 0.5f / Dd, tv =
                    thn * 9.0f / 16.0f > tvn ? thn * 9.0f / 16.0f : tvn;
                float v = 2.0f * __builtin_atanf(tv) * 57.29578f;
                v = v < S44F_VMIN ? S44F_VMIN : v >
                    S44F_VMAX ? S44F_VMAX : v;
                if (g_ps45[PS45_FOV]) { // ps45 U5 (the note at PS45_*): the fov that holds both heads, our chest and the attacker's pelvis round the AIMED axis
                    float up9[3], uc9[3], uh9[3], cb9[3] =
                        { g_s39B[0], g_s39B[1], g_s39B[2] }, ah9[3] =
                        { h9[0], h9[1] + 0.15f, h9[2] };
                    XvBody(SID_PLAYER, me, up9, uc9, uh9);
                    if (!
                        (XvFin3(h9)
                         && __builtin_fabsf(h9[1] - ot[1]) < 2.2f)) {
                        ah9[0] = hx;
                        ah9[1] = g_s34.ahy + 0.15f;
                        ah9[2] = hz;
                    }
                    const float *pts[4] =
                        { ah9, uh9, g_s39Bok ? cb9 : uc9, p9 };
                    float vf = S45fFit(E, L, pts, 4), v0 = v;
                    if (vf > v + 0.5f) {
                        v = vf > S45F_VMAX ? S45F_VMAX : vf;
                        if (v > v0 + 0.5f) {
                            g_s45f_v0 = v0;
                            g_s45f_v = v;
                            InterlockedIncrement(&g_s45fN);
                        }
                    }
                }
                float hf =
                    2.0f *
                    __builtin_atanf(__builtin_tanf(v * 0.5f * 0.0174533f) *
                                    16.0f / 9.0f) * 57.29578f;
                g_s44f.want = hf;
                g_s44f.vdeg = v;
                g_s44f.fill =
                    (top -
                     low) / (2.0f * __builtin_tanf(v * 0.5f * 0.0174533f) *
                             Dd);
                MemoryBarrier();
                g_s44f.ok = 1;
                static uint64_t lt;
                static int nl;
                if (now - lt > 700 && LOGCAP(nl, 200)) {
                    lt = now;
                    Xlog("paircam: ps44 fov - the victim's side shot at %.1f deg vertical (%.1f hfov; lens %.2f m from the pair, the span %.2f m = %.0f %% of the frame height%s; t2r-ps44-cam-fov-old.txt = the 75-equivalent; frame %llu)", (double) v, (double) hf, (double) Dd, (double) (top - low), (double) (g_s44f.fill * 100.0f), v >= S44F_VMAX ? ", at the wide clamp" : v <= S44F_VMIN ? ", at the narrow clamp" : "", (unsigned long long) g_frame);
                }
            }
        } else
            g_s44f.ok = 0;
        g_pcam.cx = E[0];
        g_pcam.cy = E[1];
        g_pcam.cz = E[2];
        g_pcam.lx = L[0];
        g_pcam.ly = L[1];
        g_pcam.lz = L[2];
        if (g_s41v.cut_req) {
            g_s41v.cut_req = 0;
            MemoryBarrier();
            InterlockedIncrement(&g_s41vCut);
        }                       // ps41 V: the re-fitted lens is published - the render thread cuts to it
        g_pcam.mx = g_pc5.sax + g_pc5.fx * 0.5f * g_pc5.sd;
        g_pcam.mz = g_pc5.saz + g_pc5.fz * 0.5f * g_pc5.sd;
        g_pcam.ux = me[0];
        g_pcam.uz = me[2];
        g_pcam.bax = g_pc5.sax;
        g_pcam.bay = g_pc5.say;
        g_pcam.baz = g_pc5.saz;
        g_pcam.bvx = V[0];
        g_pcam.bvy = V[1];
        g_pcam.bvz = V[2];
        g_pc5.n++;
        if (tg < 0.98f)
            g_pc5.arm_blk++;
        if (g_pc5.rt[g_pc5.side][1] < 0.5f)
            g_pc5.lat_blk++;
        if (g_pc5.rt[g_pc5.side][2] < 0.5f)
            g_pc5.los_blk++;
        if (g_pc5.rt[0][0] < 0.6f && g_pc5.rt[1][0] < 0.6f)
            g_pc5.both_blk++;
        if (da < g_pc5.min_a)
            g_pc5.min_a = da;
        if (dv < g_pc5.min_v)
            g_pc5.min_v = dv;
        if (g_pc5.frac < g_pc5.min_frac)
            g_pc5.min_frac = g_pc5.frac;
        if (g_pc5.raise > g_pc5.max_raise)
            g_pc5.max_raise = g_pc5.raise;
    } else if (!g_pc5.why) {
        g_pc5.why = why;
        Pc5Drain();
    }                           // the first reason it let go; the target stays frozen while it fades out
    // (pincam2: no weight ramp any more - the render thread's follower (spc_apply5) moves the shown camera in and out at SP speeds and reports
    //  when it is back on the incoming camera; w is 1 while the pin camera is engaged)
    g_pcam.want = role != 0 && !(g_s34Tb && g_pc5.v34w);
    g_pcam.w = 1.0f;            // (ps34: nothing moves until the kind / side is decided)
    g_s39Hold = g_ps39[PS39_VIC] && g_s34Tb && role && g_pc5.v34w;    // ps39: ... and the shown camera holds its pose until the cut (was: the duel framing faded to the game camera in those frames = one more view)
    if (g_s34Tb) {             // ps39 C3: our own chest, through the way out too (s34_apply: the way out goes round it and looks at it)
        float pb9[16];
        const float *m9 = role ? me : PlayerLocator(pb9);
        if (m9 && PsLocOk(m9)) {
            float p3[3], c3[3], h3[3];
            XvBody(SID_PLAYER, m9, p3, c3, h3);
            int cok = XvFin3(c3) && __builtin_fabsf(c3[1] - m9[1]) < 2.0f;
            g_s39B[0] = cok ? c3[0] : m9[0];
            g_s39B[1] = cok ? c3[1] : m9[1] + 0.5f;
            g_s39B[2] = cok ? c3[2] : m9[2];
            MemoryBarrier();
            g_s39Bok = 1;
        }
    }
    // ps40 E2 (rec39a B camrec 11577-11619 'lat +2.80 back +0.26' -> '-0.16 / +2.01' -> '-1.40 / +0.54' = the 900 ms way out swung 180 deg round
    // our body with a wall 0.84 m from it ('downlens: ... wall 0.84 m from the upper body'), clip B 17.47-17.93 nearly black; A 145.83-146.13 through a
    // pillar): while the finisher shot holds, two rays test the way it would go out - shot lens -> the arc's mid point -> the camera under the shot (our own
    // body ignored). Blocked when the shot lets go = it lands on the downed camera on ONE cut.
    if (g_ps40[PS40_OUT] && g_s34Tb && role && g_s34.vf && !g_pc5.v34w
        && g_s39Bok && g_s40InOk) {
        int b1 = 0, b2 = 0, got = 0;
        if (g_s40o.j1) {
            g_rayErr = 0;
            float t = RayT(g_s40o.j1);
            g_s40o.j1 = 0;
            if (!g_rayErr) {
                got = 1;
                b1 = t >= 0.0f && t < 0.97f;
            }
        }
        if (g_s40o.j2) {
            g_rayErr = 0;
            float t = RayT(g_s40o.j2);
            g_s40o.j2 = 0;
            if (!g_rayErr) {
                got = 1;
                b2 = t >= 0.0f && t < 0.97f;
            }
        }
        if (got)
            g_s40o.blk = b1 || b2;
        float E9[3] = { g_pcam.cx, g_pcam.cy, g_pcam.cz }, G9[3] =
            { g_s40In[0], g_s40In[1], g_s40In[2] }, bx =
            g_s39B[0], bz = g_s39B[2];
        float a0 = __builtin_atan2f(E9[0] - bx, E9[2] - bz), da =
            __builtin_remainderf(__builtin_atan2f(G9[0] - bx, G9[2] - bz) -
                                 a0, 6.2831853f);
        float r0 =
            __builtin_sqrtf((E9[0] - bx) * (E9[0] - bx) +
                            (E9[2] - bz) * (E9[2] - bz)), r1 =
            __builtin_sqrtf((G9[0] - bx) * (G9[0] - bx) +
                            (G9[2] - bz) * (G9[2] - bz));
        // ps41 G4 (rec40a B 20941 'DONE as the VICTIM OF A TAKEDOWN (ps34 side shot) ... after 6109 ms', clip B 157.80-158.30: the 900 ms way out swung
        // 180 deg through a crate lid; 0 'way out ... BLOCKED' lines - the two chords shot -> mid-arc -> landing run INSIDE a half-circle arc and at lens
        // height, a low prop on the arc itself is missed): a way out of more than S41_OUT_ARC round our chest is never eased - ONE cut, like a blocked one.
        g_s41_oarc = da;
        if (g_ps41x && da == da && __builtin_fabsf(da) > S41_OUT_ARC)
            g_s40o.blk = 2;
        float rm = 0.5f * (r0 + r1);
        if (__builtin_fabsf(da) > 0.75f && rm < PC5_ORBIT_MIN)
            rm = PC5_ORBIT_MIN;
        float M9[3] =
            { bx + __builtin_sinf(a0 + 0.5f * da) * rm,
      0.5f * (E9[1] + G9[1]), bz + __builtin_cosf(a0 + 0.5f * da) * rm };
        if (XvFin3(E9) && XvFin3(G9) && XvFin3(M9) && r1 < 8.0f) {
            g_s40o.j1 = WorldRayKickM(E9, M9, 0, 0x1b07);
            g_s40o.j2 = WorldRayKickM(M9, G9, 0, 0x1b07);
        }
    } else if (role || !g_s34Tb) {
        if (g_s40o.j1) {
            RayT(g_s40o.j1);
            g_s40o.j1 = 0;
        }
        if (g_s40o.j2) {
            RayT(g_s40o.j2);
            g_s40o.j2 = 0;
        }
        if (!(role && g_s34.vf))
            g_s40o.blk = 0;
    }
    if (!role && !g_pc5.t_out && g_ps45[PS45_EARLY] && g_s45eT0 && g_s45eT0 == g_s34.vf_t0 && g_s34Tb) {     // ps45 U2 (the note at PS45_*): the unconfirmed early shot never cuts out
        if (LOGCAP(g_s34.nlog, 200))
            Xlog("pincam5: ps45 early - the unconfirmed early side shot goes back the way it came: the %.0f ms ease out (its way in was ray-tested; the ps40 way-out test said %s; t2r-ps45-cam-early-old.txt = ps44; frame %llu)", (double) (g_s34.tout * 1000.0f), g_s40o.blk ? "BLOCKED / over 100 deg = a cut - not taken" : "clear", (unsigned long long) g_frame);
        g_s40o.blk = 0;
        g_s45eT0 = 0;
    }
    if (!role && !g_pc5.t_out && g_ps40[PS40_OUT] && g_s34Tb
        && g_s34.was_vf && g_s40o.blk) {
        g_s34.tout = 0.0f;
        g_s40o.blk = 0;
        if (LOGCAP(g_s34.nlog, 200))
            Xlog("pincam5: ps40 - the finisher side shot's way out to the downed camera is BLOCKED (a body / geometry on its arc) or longer than 100 deg (ps41; its arc: %.0f deg round our chest): it lands on ONE CUT instead of the %.0f ms ease (frame %llu)", (double) (g_s41Oarc * 57.29578f), (double) (S34_VF_OUT * 1000.0f), (unsigned long long) g_frame);
    }
    if (role)
        g_pc5.t_out = 0;
    else if (!g_pc5.t_out) {
        g_pc5.t_out = now;
        g_pcam.out_done = 0;
    }
    g_pcam.ho = 1;              // (a takedown camera starting later this frame blends from the pin camera's view on its first frame too)
    // v5-pcout (the camera lost both fighters from the frame; log 71229-71604: a pin cut after 1 blow, the duel went on
    // with swings + dodge windows while the speed-limited follower chased the moving duel framing for the whole 4 s): the way out gets 0.7 s, then cam2's
    // own 450 ms blend takes the rest. t2r-pincam-out-4s.txt = the old 4 s.
    uint64_t out_max =
        GetFileAttributesA("t2r-pincam-out-4s.txt") ==
        INVALID_FILE_ATTRIBUTES ? 700 : 4000;
    if (g_s34Tb)
        out_max = (uint64_t) (g_s34.tout * 1000.0f) + 250;      // ps34: the timed way out ends by itself
    if (!role && (g_pcam.out_done || now - g_pc5.t_out > out_max)) {
        if (!g_pcam.out_done)
            Xlog("pincam5: the way out did not reach the game camera within %llu ms - let go (cam2 blends the rest)", (unsigned long long) out_max);
        g_pcam.w = 0.0f;
        g_pcam.ho = 0;
        g_pcam.want = 0;
        Spc5Done(g_pc5.why ? g_pc5.why : why);
        return;
    }
    g_gshAny = 1;
}

// render thread: the v5 pin camera over pos (3) + q (x y z w).
// (A/B v5-int, ~200 ms rtt, B = the pinned victim: "max turn 17.9 deg/frame, max move 0.424 m/frame (at weight 0.44)" - the smoothstep weight
//  blend's middle: a 180 deg orbit of ~6 m in 0.6 s peaks at 1.5x the mean speed, at 48 fps 0.4 m / 18 deg a frame. cam2 then saw those steps as engine
//  jumps "in pin camera" and restarted its own 450 ms blend ~20 times per pin; SP never steps more than 5.6 deg / 0.10 m.)
// Now a FOLLOWER instead of a weight blend: the shown pin camera starts as the incoming camera and moves toward its target with a speed and an
// acceleration limit - position in orbit coordinates round the pair midpoint (arc, radius, height; at least PC5_ORBIT_MIN out while the arc is long, so
// never through the bodies), speed <= PC5_VMAX with an accel/decel ramp (v <= sqrt(2 a d)), orientation the same way (<= PC5_WMAX); never more than
// PC5_STEP_DEG / PC5_STEP_M in one frame whatever the frame time. Target: the pin spot looking at the pair (want), the live incoming camera on the way
// out (until it is within 0.04 m / 1 deg: out_done -> the main thread lets go, nothing left to cut), the last shown pose while a takedown / escape
// camera blends in from it (freeze). A big orbit simply takes longer (180 deg at 2 m ~ 2 s).
static float Pc5Qang(const float *a, const float *b)
{
    float d =
        __builtin_fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] +
                        a[3] * b[3]);
    d = d > 1.0f ? 1.0f : d;
    return 2.0f * __builtin_acosf(d);
}

static void Pc5Slerp(float *a, const float *b0, float t)
{                               // a = slerp(a, b, t), in place, shortest way
    float b[4] = { b0[0], b0[1], b0[2], b0[3] }, d =
        a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0f) {
        for (int i = 0; i < 4; i++)
            b[i] = -b[i];
        d = -d;
    }
    float wa, wb;
    if (d > 0.9995f) {
        wa = 1.0f - t;
        wb = t;
    } else {
        float th = __builtin_acosf(d), s = __builtin_sinf(th);
        wa = __builtin_sinf((1.0f - t) * th) / s;
        wb = __builtin_sinf(t * th) / s;
    }
    float l = 0.0f;
    for (int i = 0; i < 4; i++) {
        a[i] = wa * a[i] + wb * b[i];
        l += a[i] * a[i];
    }
    l = __builtin_sqrtf(l);
    if (l > 0.5f)
        for (int i = 0; i < 4; i++)
            a[i] /= l;
}

// the orientation that looks from p at the pin camera's look point (yaw, then pitch, no roll; the camera axis sign g_pcam.sgn), 0 = too close
static int Pc5Look(const float *p, const float *ref, float *o)
{
    float sg = (float) g_pcam.sgn, lx = sg * (g_pcam.lx - p[0]), ly =
        sg * (g_pcam.ly - p[1]), lz = sg * (g_pcam.lz - p[2]), hd =
        __builtin_sqrtf(lx * lx + lz * lz);
    if (!(hd > 0.2f))
        return 0;
    float th = __builtin_atan2f(lx, lz), ph = __builtin_atan2f(ly, hd);
    float t[4] =
        { 0.0f, __builtin_sinf(th * 0.5f), 0.0f,
   __builtin_cosf(th * 0.5f) }, c[3];
    SpmRotx(t, ph, o);
    SpmFwd(o, c);
    if (__builtin_fabsf(c[1] - __builtin_sinf(ph)) >
        __builtin_fabsf(c[1] + __builtin_sinf(ph)))
        SpmRotx(t, -ph, o);
    if (o[0] * ref[0] + o[1] * ref[1] + o[2] * ref[2] + o[3] * ref[3] <
        0.0f)
        for (int i = 0; i < 4; i++)
            o[i] = -o[i];
    return 1;
}

// ps34, render thread: the victim's shot as a TIMED blend (block comment at S34_*). phase 0 = the incoming camera (not started / done), 1 = in, 2 = the
// shot, 3 = out. The view axis is +Z of the quaternion (ps25b).
static int S34Look(const float *p, const float *ref, float *o)
{
    float lx = g_pcam.lx - p[0], ly = g_pcam.ly - p[1], lz =
        g_pcam.lz - p[2], hd = __builtin_sqrtf(lx * lx + lz * lz);
    if (!(hd > 0.2f) || !(ly == ly))
        return 0;
    float th = __builtin_atan2f(lx, lz), ph = __builtin_atan2f(ly, hd);
    float t[4] =
        { 0.0f, __builtin_sinf(th * 0.5f), 0.0f,
   __builtin_cosf(th * 0.5f) }, c[3];
    SpmRotx(t, ph, o);
    SpmFwd(o, c);
    if (__builtin_fabsf(c[1] - __builtin_sinf(ph)) >
        __builtin_fabsf(c[1] + __builtin_sinf(ph)))
        SpmRotx(t, -ph, o);
    if (o[0] * ref[0] + o[1] * ref[1] + o[2] * ref[2] + o[3] * ref[3] <
        0.0f)
        for (int i = 0; i < 4; i++)
            o[i] = -o[i];
    return 1;
}

static int S39LookAt(const float *p, const float *tg, const float *ref,
                       float *o)
{                               // ps39: s34_look at a given point
    float lx = tg[0] - p[0], ly = tg[1] - p[1], lz = tg[2] - p[2], hd =
        __builtin_sqrtf(lx * lx + lz * lz);
    if (!(hd > 0.2f) || !(ly == ly))
        return 0;
    float th = __builtin_atan2f(lx, lz), ph = __builtin_atan2f(ly, hd);
    float t[4] =
        { 0.0f, __builtin_sinf(th * 0.5f), 0.0f,
   __builtin_cosf(th * 0.5f) }, c[3];
    SpmRotx(t, ph, o);
    SpmFwd(o, c);
    if (__builtin_fabsf(c[1] - __builtin_sinf(ph)) >
        __builtin_fabsf(c[1] + __builtin_sinf(ph)))
        SpmRotx(t, -ph, o);
    if (o[0] * ref[0] + o[1] * ref[1] + o[2] * ref[2] + o[3] * ref[3] <
        0.0f)
        for (int i = 0; i < 4; i++)
            o[i] = -o[i];
    return 1;
}

static void S34Apply(float *pos, float *q)
{
    static int on, gen, ph, rich;
    static double lt, tp;
    static float A0, R0, Y0, Q0[4], P[3], Q[4];
    static int s45v, s45e;      // ps45 U3 / U4: the way in latched at the in-phase's start (variant; smoothstep)
    static float Dp, C3[2];
    static int dok, c3ok, hw;   // ps39: the arc's unwrapped angle; the way out's centre (our own body); the pose was held before the shot
    static float s46a;
    static int s46u;            // ps46 V1: the angle round the pair done so far on this phase; the way in turned with it
    double now = GshNow();
    int g = g_pcam.gen;
    float mx = g_pcam.mx, mz = g_pcam.mz, sw = 0.0f;
    if (S45UnderOk(pos)) {    // ps45 U1 (the note at PS45_*): never the second view's camera at the world origin
        g_s40In[0] = pos[0];
        g_s40In[1] = pos[1];
        g_s40In[2] = pos[2];
        g_s40InOk = 1;        // ps40 E2: the camera under the shot (spc_tick5's path rays)
        g_s44oQ[0] = q[0];
        g_s44oQ[1] = q[1];
        g_s44oQ[2] = q[2];
        g_s44oQ[3] = q[3];     // ps44 R5b: its rotation (spc_tick5's settle test)
    }
    if (g != gen) {
        gen = g;
        on = 0;
    }
    if (!on) {
        on = 1;
        ph = 0;
        rich = 0;
        lt = now;
        memcpy(P, pos, sizeof P);
        memcpy(Q, q, sizeof Q);
        dok = 0;
        c3ok = 0;
        hw = 0;
        g_pcam.out_done = 0;
        g_pcam.st_in_s = g_pcam.st_out_s = -1.0f;
        g_pcam.sgn = 1;
        return;
    }
    float dt = (float) (now - lt);
    if (dt < 0.0005f) {
        memcpy(pos, P, sizeof P);
        memcpy(q, Q, sizeof Q);
        return;
    }                           // the second copy of this frame's camera
    if (dt > 0.1f)
        dt = 0.1f;
    lt = now;
    float pp[3] = { P[0], P[1], P[2] }, pq[4] = { Q[0], Q[1], Q[2], Q[3] };
    if (!g_pcam.freeze) {
        int want = g_pcam.want;
        if ((want && (ph == 0 || ph == 3))
            || (!want && (ph == 1 || ph == 2))) {
            if (ph == 0 && !hw) {
                memcpy(P, pos, sizeof P);
                memcpy(Q, q, sizeof Q);
            }
            ph = want ? 1 : 3;
            tp = now;
            dok = 0;
            s46a = 0.0f;
            s46u = 0;
            s45e = ph == 1 && g_ps45[PS45_ARC] && S45mMvTake();
            s45v = s45e ? g_s45m_var : 0;       // ps45 U3 / U4
            // ps39 C3 (rec38a B camrec 22080-22131 'cyaw -118.8 -> -0.4 -> +80.1', clip 198.57-199.03: nobody in frame through the ease into the downed
            // camera): the way out orbited the pair's old midpoint while the view turned from the shot's direction straight to the downed camera's. It goes
            // round OUR OWN body now (the short way, the arc kept once started) and looks at our chest until the downed camera's view takes over.
            c3ok = ph == 3 && g_ps39[PS39_VIC] && g_s39Bok;
            if (c3ok) {
                C3[0] = g_s39B[0];
                C3[1] = g_s39B[2];
            }
            {
                float ox = c3ok ? C3[0] : mx, oz = c3ok ? C3[1] : mz;
                A0 = __builtin_atan2f(P[0] - ox, P[2] - oz);
                R0 = __builtin_sqrtf((P[0] - ox) * (P[0] - ox) +
                                     (P[2] - oz) * (P[2] - oz));
            } Y0 = P[1];
            memcpy(Q0, Q, sizeof Q0);
        }
        if (ph == 0) {
            hw = g_s39Hold;
            if (!hw) {
                memcpy(P, pos, sizeof P);
                memcpy(Q, q, sizeof Q);
            }
        }                       // (ps39: held while the shot is decided)
        else {
            float T[3] =
                { ph == 3 ? pos[0] : g_pcam.cx,
           ph == 3 ? pos[1] : g_pcam.cy, ph == 3 ? pos[2] : g_pcam.cz };
            float dur = ph == 3 ? g_s34.tout : g_s34.tin, x =
                dur > 0.02f ? (float) (now - tp) / dur : 1.0f;
            x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
            float s = ph == 2 ? 1.0f : ph == 1 && s45e && g_ps45[PS45_EASE] ? x * x * (3.0f - 2.0f * x) : x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f); // (ps45 U4: smoothstep on the vicmove way in)
            if (ph == 3 && c3ok) {
                mx = C3[0];
                mz = C3[1];
            }                   // ps39 C3: the way out's orbit centre
            float a1 = __builtin_atan2f(T[0] - mx, T[2] - mz), r1 =
                __builtin_sqrtf((T[0] - mx) * (T[0] - mx) +
                                (T[2] - mz) * (T[2] - mz));
            float N[3] = { T[0], T[1], T[2] };
            if (ph != 2) {
                float da = __builtin_remainderf(a1 - A0, 6.2831853f);
                // ps39 C8 (B 12508 'LENS STEP > 0.5 m IN ONE FRAME ... shown step 1.47 m' inside the pin shot's ease): the short way round was chosen anew
                // every frame - a target that crossed the point opposite the start flipped the arc to the other way round mid-blend. The arc's angle is
                // continuous from its first frame now.
                if (ph == 1 && s45e && !dok
                    && da * (float) g_s45mDsg < 0.0f)
                    da += g_s45mDsg > 0 ? 6.2831853f : -6.2831853f;    // ps45 U3: the decided way round
                if (g_ps39[PS39_VIC]) {
                    if (dok)
                        da = Dp + __builtin_remainderf(a1 - A0 - Dp,
                                                       6.2831853f);
                    Dp = da;
                    dok = 1;
                }
                float big = (__builtin_fabsf(da) - 0.25f) / 0.5f;
                big = big < 0.0f ? 0.0f : big > 1.0f ? 1.0f : big;
                float rl = R0 + (r1 - R0) * s, r =
                    rl + __builtin_sinf(3.14159265f * s) * big * (rl <
                                                                  PC5_ORBIT_MIN
                                                                  ?
                                                                  PC5_ORBIT_MIN
                                                                  -
                                                                  rl :
                                                                  0.0f),
                    a = A0 + da * s;
                s46a = da * s;
                if (r < 0.2f)
                    r = 0.2f;
                N[0] = mx + __builtin_sinf(a) * r;
                N[1] = Y0 + (T[1] - Y0) * s;
                N[2] = mz + __builtin_cosf(a) * r;
                if (ph == 1 && s45e && s45v == 2)
                    N[1] += __builtin_sinf(3.14159265f * s) * S45M_UP;  // ps45 U3: the raised way in
                memcpy(P, N, sizeof P);
            } else {
                static LONG cg41;
                g_s41vCnow = cg41 != g_s41vCut;
                cg41 = g_s41vCut;      // (ps41 V: the one late side correction is a cut, not a walk)
                float kf =
                    g_s41vCnow ? 1.0f : 1.0f -
                    __builtin_expf(-dt / 0.08f);
                for (int i = 0; i < 3; i++)
                    P[i] += (N[i] - P[i]) * kf;
            }
            if (ph != 3)
                S39WallClamp(P);      // ps39 C1: the shown lens in front of the pin's wall plane on the way in and in the shot
            float Tq[4];
            if (ph == 3)
                memcpy(Tq, q, sizeof Tq);
            else if (!S34Look(P, Q, Tq))
                memcpy(Tq, Q, sizeof Tq);
            if (ph == 2)
                Pc5Slerp(Q, Tq,
                          g_s41vCnow ? 1.0f : 1.0f -
                          __builtin_expf(-dt / 0.08f));
            else if (ph == 3 && c3ok) { // ps39 C3: onto our own chest in the first quarter, onto the downed camera's view over the second half
                float bt3[3] =
                    { g_s39B[0], g_s39B[1], g_s39B[2] }, Lq[4], e1 =
                    x / 0.25f, e2 = (x - 0.45f) / 0.55f;
                e1 = e1 > 1.0f ? 1.0f : e1;
                e1 = e1 * e1 * (3.0f - 2.0f * e1);
                e2 = e2 < 0.0f ? 0.0f : e2 > 1.0f ? 1.0f : e2;
                e2 = e2 * e2 * (3.0f - 2.0f * e2);
                memcpy(Q, Q0, sizeof Q);
                if (S39LookAt(P, bt3, Q0, Lq))
                    Pc5Slerp(Q, Lq, e1);
                else
                    e2 = s;
                Pc5Slerp(Q, Tq, e2);
            } else {
                memcpy(Q, Q0, sizeof Q);
                if (ph == 1 && g_ps46v && __builtin_fabsf(s46a) > 1e-4f) {      // ps46 V1 (the note at g_ps46v): the start view turned by the arc done (world yaw, the s34_look convention)
                    float cy = __builtin_cosf(s46a * 0.5f), sy =
                        __builtin_sinf(s46a * 0.5f), o[4] =
                        { cy * Q0[0] + sy * Q0[2], cy * Q0[1] + sy * Q0[3],
cy * Q0[2] - sy * Q0[0], cy * Q0[3] - sy * Q0[1] };
                    float l =
                        __builtin_sqrtf(o[0] * o[0] + o[1] * o[1] +
                                        o[2] * o[2] + o[3] * o[3]);
                    if (l > 0.5f && l == l) {
                        for (int i = 0; i < 4; i++)
                            Q[i] = o[i] / l;
                        s46u = 1;
                    }
                }
                Pc5Slerp(Q, Tq, s);
            }
            sw = ph == 2 ? 1.0f : ph == 1 ? s : 1.0f - s;
            if (x >= 1.0f) {
                if (ph == 1) {
                    ph = 2;
                    g_pcam.st_in_s = (float) (now - tp);
                    if (s46u) {
                        g_s46vDeg = __builtin_fabsf(s46a) * 57.29578f;
                        InterlockedIncrement(&g_s46vN);
                    }
                } else if (ph == 3) {
                    ph = 0;
                    sw = 0.0f;
                    memcpy(P, pos, sizeof P);
                    memcpy(Q, q, sizeof Q);
                    g_pcam.out_done = 1;
                    g_pcam.st_out_s = (float) (now - tp);
                }
            }
        }
        if (!(P[0] == P[0]) || !(P[1] == P[1]) || !(P[2] == P[2])
            || !(Q[0] == Q[0])) {
            memcpy(P, pp, sizeof P);
            memcpy(Q, pq, sizeof Q);
        }
    }
    memcpy(pos, P, sizeof P);
    memcpy(q, Q, sizeof Q);
    // SP's blow shake on top of the shot (gsh_filter's own kick went into the incoming camera, which the shot replaces)
    if (sw > 0.01f) {
        float rot[3] = { 0.0f, 0.0f, 0.0f };
        if (MfkRot(now, rot)) {
            float d[4] =
                { 0.5f * rot[0] * 0.0174532925f * sw,
           0.5f * rot[1] * 0.0174532925f * sw,
           0.5f * rot[2] * 0.0174532925f * sw, 1.0f };
            float o[4] =
                { q[3] * d[0] + q[0] * d[3] + q[1] * d[2] - q[2] * d[1],
           q[3] * d[1] - q[0] * d[2] + q[1] * d[3] + q[2] * d[0],
                q[3] * d[2] + q[0] * d[1] - q[1] * d[0] + q[2] * d[3],
                    q[3] * d[3] - q[0] * d[0] - q[1] * d[1] - q[2] * d[2]
            };
            float l =
                __builtin_sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] +
                                o[3] * o[3]);
            if (l > 0.5f)
                for (int i = 0; i < 4; i++)
                    q[i] = o[i] / l;
        }
    }
    // metrics for spc5_done's line
    if (g_pcam.st_reset) {
        g_pcam.st_reset = 0;
        rich = 0;
        g_pcam.st_min_a = g_pcam.st_min_v = 99.0f;
        g_pcam.st_max_deg = g_pcam.st_max_mv = 0.0f;
        g_pcam.st_mv_w = -1.0f;
        g_pcam.st_n = g_pcam.st_near = g_pcam.st_wcap = 0;
    }
    if (rich) {
        float deg = Pc5Qang(pq, Q) * 57.2957795f, mv =
            __builtin_sqrtf((P[0] - pp[0]) * (P[0] - pp[0]) +
                            (P[1] - pp[1]) * (P[1] - pp[1]) + (P[2] -
                                                               pp[2]) *
                            (P[2] - pp[2]));
        if (deg > g_pcam.st_max_deg)
            g_pcam.st_max_deg = deg;
        if (mv > g_pcam.st_max_mv) {
            g_pcam.st_max_mv = mv;
            g_pcam.st_mv_w = ph == 1 ? 0.0f : ph == 2 ? 1.0f : 2.0f;
        }
    }
    rich = 1;
    {
        float ea =
            __builtin_sqrtf((P[0] - g_pcam.bax) * (P[0] - g_pcam.bax) +
                            (P[2] - g_pcam.baz) * (P[2] - g_pcam.baz));
        float ev =
            __builtin_sqrtf((P[0] - g_pcam.bvx) * (P[0] - g_pcam.bvx) +
                            (P[2] - g_pcam.bvz) * (P[2] - g_pcam.bvz));
        if (P[1] > g_pcam.bay + 2.0f)
            ea = 9.0f;
        if (P[1] > g_pcam.bvy + 2.0f)
            ev = 9.0f;
        if (ph) {
            if (ea < g_pcam.st_min_a)
                g_pcam.st_min_a = ea;
            if (ev < g_pcam.st_min_v)
                g_pcam.st_min_v = ev;
            if (ea < 0.5f || ev < 0.5f)
                g_pcam.st_near++;
        }
        g_pcam.st_n++;
    }
}

static void SpcApply5(float *pos, float *q)
{
    static int on, odir, phase, rich, gen;
    static double lt, t0, tout;
    static float A, R, Y, v[3], wv, Q[4], P[3];
    if (g_s34Tb) {
        S34Apply(pos, q);
        return;
    }                           // ps34: the victim's shot
    if (!(g_pcam.w > 0.001f)) {
        on = 0;
        return;
    }                           // (never reached from gsh_filter, which calls us only while w > 0: the pin's gen restarts us below)
    double now = GshNow();
    float mx = g_pcam.mx, mz = g_pcam.mz;
    int g = g_pcam.gen;
    if (g != gen) {
        gen = g;
        on = 0;
    }                           // v5-cam5: a new pin - never resume the last pin's follower (it sat where that pin was, maybe another map)
    if (!on) {                  // first frame: the shown camera IS the incoming one
        float f[3];
        SpmFwd(q, f);
        if (!g_pcam.sgn)
            g_pcam.sgn =
                ((g_pcam.ux - pos[0]) * f[0] +
                 (g_pcam.uz - pos[2]) * f[2]) < 0.0f ? -1 : 1;
        A = __builtin_atan2f(pos[0] - mx, pos[2] - mz);
        R = __builtin_sqrtf((pos[0] - mx) * (pos[0] - mx) +
                            (pos[2] - mz) * (pos[2] - mz));
        Y = pos[1];
        v[0] = v[1] = v[2] = 0.0f;
        wv = 0.0f;
        memcpy(Q, q, sizeof Q);
        memcpy(P, pos, sizeof P);
        odir = 0;
        phase = 0;
        rich = 0;
        on = 1;
        lt = t0 = now;
        tout = 0;
        g_pcam.out_done = 0;
        g_pcam.st_in_s = g_pcam.st_out_s = -1.0f;
        return;
    }
    float dt = (float) (now - lt);
    if (dt < 0.0005f) {
        memcpy(pos, P, sizeof P);
        memcpy(q, Q, sizeof Q);
        return;
    }                           // the second copy of this frame's camera
    if (dt > 0.1f)
        dt = 0.1f;
    lt = now;
    int out = !g_pcam.want;
    if (out && !tout)
        tout = now;
    if (!out)
        tout = 0;
    float in_p[3] = { pos[0], pos[1], pos[2] }, in_q[4] =
        { q[0], q[1], q[2], q[3] };
    float pp[3] = { P[0], P[1], P[2] }, pq[4] = { Q[0], Q[1], Q[2], Q[3] };
    float dlast = 1e9f, alast = 1e9f;
    if (!g_pcam.freeze) {
        // position: orbit coordinates toward the target, speed + accel limited
        float T[3] =
            { out ? in_p[0] : g_pcam.cx, out ? in_p[1] : g_pcam.cy,
       out ? in_p[2] : g_pcam.cz };
        float a1 = __builtin_atan2f(T[0] - mx, T[2] - mz), r1 =
            __builtin_sqrtf((T[0] - mx) * (T[0] - mx) +
                            (T[2] - mz) * (T[2] - mz));
        float A0 = A, R0 = R, Y0 = Y;
        int n = (int) (dt * 120.0f) + 1;
        float h = dt / (float) n;
        for (int s = 0; s < n; s++) {
            float da = __builtin_remainderf(a1 - A, 6.2831853f);
            if (__builtin_fabsf(da) > 2.6f && odir) {
                if (odir > 0 && da < 0.0f)
                    da += 6.2831853f;
                else if (odir < 0 && da > 0.0f)
                    da -= 6.2831853f;
            }                   // (no flip of the way round near 180 deg)
            else
                odir = da >= 0.0f ? 1 : -1;
            float big = (__builtin_fabsf(da) - 0.25f) / 0.5f;
            big = big < 0.0f ? 0.0f : big > 1.0f ? 1.0f : big;
            float rt =
                r1 < PC5_ORBIT_MIN ? r1 + (PC5_ORBIT_MIN - r1) * big : r1,
                rr = R > 0.3f ? R : 0.3f;
            float e[3] = { da * rr, rt - R, T[1] - Y }, d =
                __builtin_sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
            float vd =
                d > 1e-4f ? fminf(PC5_VMAX,
                                  fminf(__builtin_sqrtf
                                        (2.0f * PC5_AMAX * d),
                                        PC5_OMEGA * d)) : 0.0f, dv[3],
                dvl = 0.0f;
            for (int i = 0; i < 3; i++) {
                dv[i] = (d > 1e-4f ? e[i] / d * vd : 0.0f) - v[i];
                dvl += dv[i] * dv[i];
            }
            dvl = __builtin_sqrtf(dvl);
            if (dvl > PC5_AMAX * h)
                for (int i = 0; i < 3; i++)
                    dv[i] *= PC5_AMAX * h / dvl;
            for (int i = 0; i < 3; i++)
                v[i] += dv[i];
            A += v[0] * h / rr;
            R += v[1] * h;
            Y += v[2] * h;
            if (R < 0.2f)
                R = 0.2f;
            dlast = d;
        }
        // never more than PC5_STEP_M in one frame (whatever the frame time)
        float ox = (A - A0) * (R0 > 0.3f ? R0 : 0.3f), st =
            __builtin_sqrtf(ox * ox + (R - R0) * (R - R0) +
                            (Y - Y0) * (Y - Y0));
        if (st > PC5_STEP_M) {
            float f = PC5_STEP_M / st;
            A = A0 + (A - A0) * f;
            R = R0 + (R - R0) * f;
            Y = Y0 + (Y - Y0) * f;
            for (int i = 0; i < 3; i++)
                v[i] *= f;
        }
        P[0] = mx + __builtin_sinf(A) * R;
        P[1] = Y;
        P[2] = mz + __builtin_cosf(A) * R;
        // v5-feel2 (pincam5 'on the way out' on the pinned victim's screen moved 0.12 m/frame, limit 0.08; B i6b / i5 'max move 0.123-0.124
        // m/frame'): the cap above limits the step in orbit coordinates only, but the orbit centre is the pair midpoint (g_pcam.mx / mz), which the main
        // thread moves with the pinned pair - the shown camera moved by the follower's 0.08 m PLUS the midpoint's own motion. The output's WORLD step is
        // capped now (the orbit coordinates re-derived from the capped spot, so the follower carries on from where the lens really is).
        {
            float wx = P[0] - pp[0], wy = P[1] - pp[1], wz =
                P[2] - pp[2], wl =
                __builtin_sqrtf(wx * wx + wy * wy + wz * wz);
            if (wl > PC5_STEP_M && pp[0] == pp[0]) {
                float f = PC5_STEP_M / wl;
                P[0] = pp[0] + wx * f;
                P[1] = pp[1] + wy * f;
                P[2] = pp[2] + wz * f;
                A = __builtin_atan2f(P[0] - mx, P[2] - mz);
                R = __builtin_sqrtf((P[0] - mx) * (P[0] - mx) +
                                    (P[2] - mz) * (P[2] - mz));
                Y = P[1];
                if (R < 0.2f)
                    R = 0.2f;
                for (int i = 0; i < 3; i++)
                    v[i] *= f;
                g_pcam.st_wcap++;
            }
        }
        if (!out && S39WallClamp(P)) {
            A = __builtin_atan2f(P[0] - mx, P[2] - mz);
            R = __builtin_sqrtf((P[0] - mx) * (P[0] - mx) +
                                (P[2] - mz) * (P[2] - mz));
            if (R < 0.2f)
                R = 0.2f;
        }                       // ps39 C1 (spc_tick5's note)
        // orientation: toward looking at the pair from where we are now (in), or toward the incoming camera's (out)
        float Tq[4];
        if (out || !Pc5Look(P, Q, Tq))
            memcpy(Tq, in_q, sizeof Tq);
        float th = Pc5Qang(Q, Tq), wd =
            th > 1e-4f ? fminf(PC5_WMAX,
                               fminf(__builtin_sqrtf(2.0f * PC5_WACC * th),
                                     PC5_OMEGA * th)) : 0.0f;
        float dw = wd - wv, mxw = PC5_WACC * dt;
        wv += dw > mxw ? mxw : dw < -mxw ? -mxw : dw;
        if (wv < 0.0f)
            wv = 0.0f;
        float step = wv * dt, cap = PC5_STEP_DEG * 0.0174533f;
        if (step > cap)
            step = cap;
        if (th > 1e-5f)
            Pc5Slerp(Q, Tq, step >= th ? 1.0f : step / th);
        alast = Pc5Qang(Q, Tq);
        if (!(P[0] == P[0]) || !(P[1] == P[1]) || !(P[2] == P[2])
            || !(Q[0] == Q[0])) {
            memcpy(P, pp, sizeof P);
            memcpy(Q, pq, sizeof Q);
        }
        if (!out && phase == 0 && dlast < 0.05f && alast < 0.0175f) {
            phase = 1;
            g_pcam.st_in_s = (float) (now - t0);
        }
        if (!out && phase == 2)
            phase = 1;          // (v5-feel2: back in after a let-go flicker - the metric's label said 'on the way out' for the rest of the pin)
        if (out) {
            phase = 2;
            float ex = P[0] - in_p[0], ey = P[1] - in_p[1], ez =
                P[2] - in_p[2];
            if (__builtin_sqrtf(ex * ex + ey * ey + ez * ez) < 0.04f
                && Pc5Qang(Q, in_q) < 0.0175f) {
                g_pcam.out_done = 1;
                g_pcam.st_out_s = (float) (now - tout);
            }
        }
    }
    memcpy(pos, P, sizeof P);
    memcpy(q, Q, sizeof Q);
    // metrics (spc5_done logs them): body clearance of the output, its largest turn / move per frame
    {
        if (g_pcam.st_reset) {
            g_pcam.st_reset = 0;
            rich = 0;
            g_pcam.st_min_a = g_pcam.st_min_v = 99.0f;
            g_pcam.st_max_deg = g_pcam.st_max_mv = 0.0f;
            g_pcam.st_mv_w = -1.0f;
            g_pcam.st_n = g_pcam.st_near = g_pcam.st_wcap = 0;
        }
        if (rich) {
            float deg = Pc5Qang(pq, Q) * 57.2957795f, mv =
                __builtin_sqrtf((P[0] - pp[0]) * (P[0] - pp[0]) +
                                (P[1] - pp[1]) * (P[1] - pp[1]) + (P[2] -
                                                                   pp[2]) *
                                (P[2] - pp[2]));
            if (deg > g_pcam.st_max_deg)
                g_pcam.st_max_deg = deg;
            if (mv > g_pcam.st_max_mv) {
                g_pcam.st_max_mv = mv;
                g_pcam.st_mv_w = (float) phase;
            }
        }
        rich = 1;
        float ea =
            __builtin_sqrtf((P[0] - g_pcam.bax) * (P[0] - g_pcam.bax) +
                            (P[2] - g_pcam.baz) * (P[2] - g_pcam.baz));
        float ev =
            __builtin_sqrtf((P[0] - g_pcam.bvx) * (P[0] - g_pcam.bvx) +
                            (P[2] - g_pcam.bvz) * (P[2] - g_pcam.bvz));
        if (P[1] > g_pcam.bay + 2.0f)
            ea = 9.0f;          // over the attacker's head: not inside it
        if (P[1] > g_pcam.bvy + 2.0f)
            ev = 9.0f;
        if (ea < g_pcam.st_min_a)
            g_pcam.st_min_a = ea;
        if (ev < g_pcam.st_min_v)
            g_pcam.st_min_v = ev;
        if (ea < 0.5f || ev < 0.5f)
            g_pcam.st_near++;
        g_pcam.st_n++;
    }
}

static volatile LONG g_spmCollapse;    // v5-cam4: render frames the engine lens sat within 0.3 m of our root and the framing kept its last direction (spm_pos)
static volatile float g_spmRal = -1.0f;        // v5-cam4: the newest head-to-lens ray answer - lens distance it allows before the margin cut (9 = clear, -1 = none)
static uint64_t g_spmPh, g_spm_rjob2;  // v5-cam4: the framed partner's puppet handle (spm_tick) + the head-to-lens ray cast again through it
static uint32_t g_spmRhit[3];
static float g_spm_rcx, g_spm_rcz;      // (+ the last ray's unit direction from our root)        // v5-cam4: head-to-lens ray hits in 30 s: [0] geometry, [1] the partner's body (taken through it), [2] partner-ignoring ray stopped on our own body (clear)
static uint64_t g_spmRjob;
static float g_spm_rlen, g_spm_rbm;     // v5-feel2 round 3: the head-to-lens ray (spm_tick)
// ps23 (rec13: A 'camdiag: frame 17407 LENS STEP > 0.5 m IN ONE FRAME ... shown step 0.62 m (world; our root 0.62 m, engine camera 0.11 m)' with
// 'dtl: SNAP slot 0 0.73 m in 1 frame ... (hitreact.attacker_glide)', the same at 17460 (shown 0.80 m, root 0.81 m, engine 0.21 m); A 20464 'SNAP ... 0.39 m
// (paired.tc_finish)' at the 'ctxdown: hand-off' -> 'cam2: engine camera jump in duel framing (0.5 deg / 0.43 m)'; own-body steps of 0.15-0.95 m in one frame
// on pair starts / pair travel / glides / hand-offs, 'dtl: SNAP slot N ... us' 12 times on A and about 30 on B): the framing kept the lens at SP's distance from the RAW root (spm_pos: 'want'
// from g_spm.px / pz), and the two stages behind it count a lens that moves with our root as allowed motion (cam2_filter works in root-relative offsets,
// ccg_filter's 'rdl') - so every one-frame step of our body inside a duel was shown 1:1 as a lens step, although the engine camera itself had eased it
// (0.11 m of the 0.62 m). The framing's pivot (ours) and its look-at / re-centre target (the partner's root, which pair placement and the contact carry
// step the same way) now FOLLOW the two roots: exactly up to SPM_PIV_MPS (6 m/s = 0.10 m per 60 fps frame, SP's p90 camera step 0.11 m), a faster step
// is eased in (tau SPM_PIV_TAU) and the pivot never trails by more than SPM_PIV_LAG (SP's largest camera-to-player offset change in one frame: 0.66 m). A
// move over 2 m (respawn / teleport) is taken at once. Same owner, no new stage. t2r-ps23-cam-pivot-old.txt next to the exe = the raw roots.
#define SPM_PIV_MPS 6.0f
#define SPM_PIV_TAU 0.10f
#define SPM_PIV_LAG 0.60f
static float SpmSlew(float *sx, float *sz, float tx, float tz, float dt,
                      int *ok)
{                               // returns how far the target was from the pivot before this step (0 on a reset)
    if (!*ok) {
        *sx = tx;
        *sz = tz;
        *ok = 1;
        return 0.0f;
    }
    float dx = tx - *sx, dz = tz - *sz, d =
        __builtin_sqrtf(dx * dx + dz * dz);
    if (!(d == d) || d > 2.0f) {
        *sx = tx;
        *sz = tz;
        return 0.0f;
    }
    if (d < 1e-4f) {
        *sx = tx;
        *sz = tz;
        return d;
    }
    float st =
        fmaxf(SPM_PIV_MPS * dt,
              d * (1.0f - __builtin_expf(-dt / SPM_PIV_TAU)));
    if (d - st > SPM_PIV_LAG)
        st = d - SPM_PIV_LAG;
    if (st >= d) {
        *sx = tx;
        *sz = tz;
        return d;
    }
    *sx += dx / d * st;
    *sz += dz / d * st;
    return d;
}

// ps32: the opt-out files of the ps32 camera pass (t2r-ext.c g_ps32), re-read every 2 s
static void Ps32Cfg(uint64_t now)
{
    static uint64_t at;
    static int first = 1;
    static const char *const F[PS32_N] =
        { "t2r-ps32-cam-shake-old.txt", "t2r-ps32-cam-trans-old.txt",
"t2r-ps32-cam-fin-old.txt" };
    if (!first && now - at < 2000)
        return;
    at = now;
    for (int i = 0; i < PS32_N; i++) {
        int v = GetFileAttributesA(F[i]) == INVALID_FILE_ATTRIBUTES;
        if (v != g_ps32[i] || first)
            Xlog("spmelee cam: ps32 %s %s", F[i],
                 v ? "absent - the ps32 behaviour is ON" :
                 "present - the OLD (ps31) behaviour");
        g_ps32[i] = v;
    }
    first = 0;
}

// ps32 FINISHER PUSH (the spec block's FINISHER lines; ctxdown.c cd_cam_tracks has the MP evidence): on the attacker's screen a duel finisher /
// contextual takedown that starts while the rig frames its victim keeps the rig - no authored takedown camera, no switch - and the rig pushes in with a
// weight p on SP's timing (S32_IN_DELAY, then a smoothstep over S32_IN_FREE / S32_IN_CTX): distance SPM25_DIST -> S32_DIST_*, lens height -> S32_UP_*,
// pitch SPM_PITCH_SP -> + S32_PITCH_*, fov -> meleefov.c S32_FOV_* (75 -> 73.7 free-standing / disarm, -> 63 wall / low cover). The victim stays the
// framed partner through its down / death, the framing's own gates (paired move, finisher guard) stand aside, the finisher's blows shake at SP's 0.5.
// After the move's end (or its abort) p eases out over S32_OUT while the ps25 release blends the framing out: back on the explore camera 0.5-0.75 s
// after the last beat (SP 0.4-0.6 s). Not taken (the old camera runs): the rig is not up on this victim (a takedown outside a duel, a fighter who has
// not swung, an aiming player), we are the victim, t2r-ps32-cam-fin-old.txt.
#define S32_IN_DELAY 0.07f      // s (SP: the first 4-5 frames move nothing)
#define S32_IN_FREE 0.50f       // s (SP free-standing / disarm: fov 90 % down by frame 26-36, height by +20, distance by +30)
#define S32_IN_CTX 0.42f        // s (SP half-wall head smash: fov 75 -> 63.5 in 25 frames, distance 1.29 -> 0.98 in 25)
#define S32_OUT 0.50f           // s (SP: distance settled 24-34 frames after the move, fov back 10-26 frames after)
#define S32_DIST_FREE 1.09f     // m (SP 1.22 -> 1.09 at +30 frames; 0.98 at +60 is the move's own travel)
#define S32_DIST_CTX 0.98f      // m (SP head smash 1.29 -> 0.98)
#define S32_UP_FREE 1.45f       // m above our root (SP 1.72 -> 1.35-1.50 at +20 frames, 1.40-1.53 after)
#define S32_UP_CTX 1.36f        // m (SP head smash 1.81 -> 1.34)
#define S32_PITCH_FREE 0.105f   // rad = +6.0 deg: -16.5 -> -10.5 (SP -10 at +15 frames, -8 .. -12 at +20)
#define S32_PITCH_CTX 0.166f    // rad = +9.5 deg: -16.5 -> -7 (SP head smash -3.4 at +25 frames, -5 .. -8 after) [TUNING ps32: one SP sample]
#define S32_FOV_OUT_DELAY 0.067f        // s: PS32_TRANS, the fov's way out of the melee value (SP: 75.0 for 4 frames, then a smoothstep over ~24)
#define S32_FOV_OUT 0.40f
#define S32_UP_OUT_DELAY 0.13f  // s: PS32_TRANS, the lens height's way out (SP: 1.80 for 9 frames, 1.54 at +30)
#define S32_UP_OUT 0.42f
// main thread, ctxdown.c cd_cam_tracks (attacker): 1 = the rig keeps this move's camera
// ps35 (ps34 had the camera positioned wrong; SP's camera positions are the reference: Abby's contextual takedown frames both of the
// characters when ours stood behind the back; uc5 A 16924 'ps32 finisher ctxdown:cover-face-smash - the rig keeps the camera for 3500 ms and pushes in',
// the lens behind the attacker's back, and after the attacker's release at 1.78 s inside the two bodies until the 3.5 s hand-off; SP clip 16-23-29 88.6-
// 92.6 s: an ANIMATED camera - round from behind Abby to a close side shot of both heads, then above the cover at the smash; the spec block's FINISHER line:
// roll > 1 deg in 32 of 34 SP finishers): a CONTEXTUAL takedown (wall / cover / car / weapon pair - every ctxdown row that is not a duel finisher 'mf-')
// plays its clip's own authored camera again, as before ps32: blended in from the frozen duel framing over 450 ms (spm_tick's hand-over), ended by
// ctxdown's 'attacker free after the final blow' with its hand-back - not held to the victim's hand-off. The rig push stays for the duel's free
// finishers (mf-), and takes a contextual one only when its authored camera is missing (cd_cam_tracks) or blocked on every candidate
// (xcam_choose_tick -> g_s32_fb_req -> spm_tick); either way it ENDS at the attacker's release now (s32_fin_tick). t2r-ps35-cam-ctx-rig.txt = the ps32 rig
// push for contextual takedowns too; t2r-ps32-cam-fin-old.txt = the authored camera for every finisher.
static struct {
    uint64_t ph, t0;
    float dur;
    char name[64];
} g_s35n;                       // the last attacker takedown handed to the authored camera (for the blocked-camera fallback)
static int S32FinStart(uint64_t other_h, float dur_s, const char *name);
static int S32FinForce(uint64_t other_h, float dur_s, const char *name,
                         const char *why)
{
    static int nl;
    int r = S32FinStart(other_h, dur_s, name);
    if (LOGCAP(nl, 80))
        Xlog("spmelee cam: ps35 takedown %s - %s: %s (frame %llu)",
             name ? name : "?", why ? why : "?",
             r ? "the duel rig keeps the camera and pushes in" :
             "the rig is not up either - the game camera / the caller's own fallback",
             (unsigned long long) g_frame);
    return r;
}

static int S32FinBegin(uint64_t other_h, float dur_s, const char *name)
{
    if (!g_ps32[PS32_FIN] || !other_h || !(dur_s > 0.3f))
        return 0;
    // ps39 C5 (rec38a A 16390-16440 / 19486-19535, clip A 120.13-120.70 and 171.63-172.40: through a weapon finisher on the rig our own back hides
    // the victim, the check said '100 % in frame' - frustum only): the duel's free finishers (mf-) have authored tracks like the contextual takedowns.
    // Every finisher with a track plays it now - eased in from the duel framing, out at the attacker's release, the ps38 live guard on it; the rig push
    // only without a track / with every shot blocked. t2r-ps39-cam-mf-rig.txt = the rig push for mf- finishers (ps35).
    int mf39 = name && !strncmp(name, "ctxdown:mf-", 11);
    // ps44 R11 (the note at S44F_*) + R14 (EVERY paired move that downs or kills - finishers, disarms (mf-*-disarm-* rows), the upgraded one-shots
    // (upg-*), contextual wall / cover / weapon / knife takedowns - plays the proper SP player camera on the attacker's screen): every ctxdown pair that comes
    // here (ctxdown.c cd_cam role 1: the contextual takedowns and cd_upg_go's mf- / upg- rows; not the pin escape 'pe-') = SP's camera - the clip's own
    // authored track when it has one, else SP's finisher rig push behind our shoulder. No ps42 side-on swing, no ps43 two-shot. (The pin finishers, the
    // prone pull-out and the dodge counters start their authored track through xcam_start directly - never this path; the 2013 executions: R12.)
    int att44 = g_ps44[PS44_ATTFIN] && name
        && !strncmp(name, "ctxdown:", 8)
        && strncmp(name, "ctxdown:pe-", 11) && (mf39 || g_ps44x14);
    if (att44) {
        static int nl44;
        int kd = -1, ntr = 0;
        for (size_t i = 0; i < CD_N; i++)
            if (!strcmp(CD_PAIRS[i].name, name + 8)) {
                kd = CD_PAIRS[i].kind;
                break;
            }
#ifdef XCAM_HAVE_CTX
        for (size_t i = 0;
             i < sizeof CTX_XCAM_TRACKS / sizeof CTX_XCAM_TRACKS[0]; i++)
            if (CTX_XCAM_TRACKS[i].move
                && !strcmp(CTX_XCAM_TRACKS[i].move, name)
                && CTX_XCAM_TRACKS[i].role == 1)
                ntr++;
#endif
        int trk = ntr && (!mf39 || g_ps39[PS39_MF])
            && GetFileAttributesA("t2r-ps35-cam-ctx-rig.txt") ==
            INVALID_FILE_ATTRIBUTES;
        if (LOGCAP(nl44, 200))
            Xlog("paircam: ps44 attfin %s kind %d - SP camera: %s (%d authored attacker track(s)%s; %s; no side-on swing, no pair-frame two-shot; t2r-ps44-cam-attfin-old.txt = ps43; frame %llu)", name, kd, trk ? "track" : "rig push", ntr, trk ? ", the rig push behind our shoulder if every shot is blocked" : "", mf39 ? (strstr(name, "disarm") ? "a duel disarm finisher" : "a duel finisher") : !strncmp(name, "ctxdown:upg-", 12) ? "an upgraded one-shot" : kd == 1 ? "a wall takedown" : kd == 2 ? "a cover takedown" : "a paired down / kill", (unsigned long long) g_frame);
    }
    // ps42 H1 (the note at ps42_cfg): the rig keeps the attacker's finisher / takedown as a side-on two-shot; the authored track only when the rig is not up
    if (!att44 && name && !strncmp(name, "ctxdown:", 8)
        && strncmp(name, "ctxdown:pe-", 11)
        && g_ps42[mf39 ? PS42_MF : PS42_CTX]) {
        static int nl42;
        snprintf(g_s35n.name, sizeof g_s35n.name, "%s", name);
        S43AttReq(other_h, dur_s, name);      // ps43: the pair-frame two-shot on top of whatever camera this takedown gets (s43_att_tick)
        if (S32FinStart(other_h, dur_s, name)) {
            if (LOGCAP(nl42, 120))
                Xlog("spmelee cam: ps42 takedown %s - %s on the attacker's screen: the duel rig keeps the lens and swings SIDE-ON (%.0f deg off straight-behind, %.2f m, both fighters in the shot; one side swap by cut if the victim is hidden); no authored track (%s = the authored track; frame %llu)", name, mf39 ? "a duel finisher" : "a contextual takedown", (double) (S40_AZ_CTX * 57.29578f), (double) S42_DIST_SIDE, mf39 ? "t2r-ps42-cam-mf-authored.txt" : "t2r-ps42-cam-ctx-authored.txt", (unsigned long long) g_frame);
            return 1;
        }
        if (LOGCAP(nl42, 120))
            Xlog("spmelee cam: ps42 takedown %s - the duel rig is not up on this victim: the clip's authored camera as in ps41 (frame %llu)", name, (unsigned long long) g_frame);
    }
    if (name && (!mf39 || g_ps39[PS39_MF])
        && GetFileAttributesA("t2r-ps35-cam-ctx-rig.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        static int nl;
        g_s35n.ph = other_h;
        g_s35n.t0 = GetTickCount64();
        g_s35n.dur = dur_s;
        snprintf(g_s35n.name, sizeof g_s35n.name, "%s", name);
        if (LOGCAP(nl, 80))
            Xlog("spmelee cam: %s takedown %s - %s: the clip's own authored camera (eased in from the duel framing, out at the attacker's release); the rig only if that camera is missing / blocked (frame %llu)", mf39 ? "ps39" : "ps35", name, mf39 ? "a duel finisher (every finisher with an authored track plays it)" : "a contextual takedown", (unsigned long long) g_frame);
        return 0;
    }
    return S32FinStart(other_h, dur_s, name);
}

static int S32FinStart(uint64_t other_h, float dur_s, const char *name)
{
    static int nl;
    if (!g_ps32[PS32_FIN] || !other_h || !(dur_s > 0.3f))
        return 0;
    const char *no =
        !g_ps25[PS25_RIG] ? "the ps25 rig is switched off" : !g_cam2On ?
        "the v5 duel camera is switched off" : (!g_spm.ok
                                                || !(g_spm.w >
                                                     0.5f)) ?
        "the duel framing is not up" : !(g_spm.yw >
                                         0.5f) ?
        "the rig is not latched (no swing of ours in this duel)" : g_spmPh
        !=
        other_h ? "the framing is on another body" : g_xcamOn ?
        "a takedown camera already runs" : NULL;
    if (no) {
        if (LOGCAP(nl, 80))
            Xlog("spmelee cam: ps32 finisher %s - not taken by the rig (%s): the authored takedown camera as before", name ? name : "?", no);
        return 0;
    }
    uint64_t now = GetTickCount64();
    g_s32.ph = other_h;
    g_s32.ctx = name && (strstr(name, "wall")
                         || strstr(name, "cover")) ? 1 : 0;
    g_s42Side = name && !strncmp(name, "ctxdown:", 8) && g_ps42[!strncmp(name, "ctxdown:mf-", 11) ? PS42_MF : PS42_CTX];       // ps42 H1: every takedown the rig holds is the side-on two-shot
    if (g_ps44[PS44_ATTFIN] && name && !strncmp(name, "ctxdown:", 8)
        && strncmp(name, "ctxdown:pe-", 11) && (g_ps44x14
                                                || !strncmp(name,
                                                            "ctxdown:mf-",
                                                            11)))
        g_s42Side = 0;         // ps44 R11 / R14: every paired down / kill = SP's push behind our shoulder
    g_s44Under = g_ps44[PS44_UNDER] && g_s42Side && g_ps43[PS43_ATT]
        && g_s43q.h == other_h && now - g_s43q.t < 300;
    g_s44Pt0 = 0;
    if (g_s44Under) {          // ps44 R3 (the note at ps44_cfg): the two-shot was requested for this move - the push under it does not move the lens
        g_s42Side = 0;
        static int nl44;
        if (LOGCAP(nl44, 120))
            Xlog("paircam: ps44 %s - the pair-frame two-shot is requested for this move: the rig push under it is held at 0 (no side-on swing, no side-swap cut, no push-in; the ordinary duel rig runs underneath, unseen; t2r-ps44-cam-under-old.txt = the ps42 side-on push under it; frame %llu)", name ? name : "?", (unsigned long long) g_frame);
    }
    g_s32.t0 = now;
    g_s32.until = now + (uint64_t) (dur_s * 1000.0f);
    g_s32.kn = 0;
    g_s32.kdone = 0;
    MemoryBarrier();
    g_s32.on = 1;
    if (LOGCAP(nl, 80))
        Xlog("spmelee cam: ps32 finisher %s - the rig keeps the camera for %.0f ms and pushes in (%s: distance %.2f -> %.2f m, lens %.2f m up, pitch %+.1f deg, fov to SP's %s over %.0f ms; frame %llu)", name ? name : "?", (double) (dur_s * 1000.0f), g_s32.ctx ? "wall / cover" : "free-standing / disarm", (double) SPM25_DIST, (double) (g_s32.ctx ? S32_DIST_CTX : S32_DIST_FREE), (double) (g_s32.ctx ? S32_UP_CTX : S32_UP_FREE), (double) ((SPM_PITCH_SP + (g_s32.ctx ? S32_PITCH_CTX : S32_PITCH_FREE)) * 57.29578f), g_s32.ctx ? "63" : "73.7", (double) (1000.0f * (S32_IN_DELAY + (g_s32.ctx ? S32_IN_CTX : S32_IN_FREE))), (unsigned long long) g_frame);
    return 1;
}

// main thread, ctxdown.c cd_cam_tracks right after cd_cam_kicks: the finisher's blow times (g_xcam_kicks, s on the track clock; the clock starts trk_off in)
static void S32FinKicks(float trk_off)
{
    int n = 0;
    for (int i = 0; i < g_xcamKicks.n && n < 12; i++) {
        float t = g_xcamKicks.t[i] - trk_off;
        if (t > 0.0f && t == t)
            g_s32.kt[n++] = t;
    }
    g_s32.kdone = 0;
    g_s32.kn = n;
    static int nl;
    if (LOGCAP(nl, 80))
        Xlog("spmelee cam: ps32 finisher - %d blow shake(s) at SP's scale %.2f, the first %.0f ms from now", n, (double) MFK32_FINISHER, n ? (double) (g_s32.kt[0] * 1000.0f) : -1.0);
}

// main thread, spm_tick: 1 = the finisher push is running; eases g_s32.p
// ps35 self-check of the ATTACKER's takedown camera (authored or the rig push), main thread, every tick it runs: both heads on screen in the SHOWN camera
// (rcam_get, the engine's fov) and the lens's distance to the nearer body axis; one summary line when the camera ends.
// ps38 LIVE GUARD (xc = a takedown / escape camera of the execution camera runs; rec37a A 14798 above, B 13875-13975 'xcam victim: ... arm 0.13 -> 0.00 ...
// framing' with camrec 'up 2.02-2.16 dist 0.40-0.79 pitch -56' = the pinner's escape camera collapsed onto our head, the picture black on both screens
// for ~3 s): once such a camera is in (weight > 0.6, not on its way out), 0.25 s with both heads out of frame or the lens within 0.30 m of a body axis
// stops it - it eases out through its own hand-back / blend-out onto the duel framing or the game camera - once per camera, logged.
// t2r-ps38-cam-guard-old.txt = no guard.
static void S35TkCheck(int act, uint64_t other_h, const char *name,
                         const char *kind, int xc)
{
    static int on, n, n0, n1, nb, nl;
    static float dmin;
    static char nm[64];
    static const char *kd = "";
    static uint64_t bad_at;
    static int tripped;
    static int nocc;
    static S39OccT oc;        // ps39 C5: ticks the victim's head stood in the frustum but behind our own body / geometry
    static uint64_t occ_at;     // ps40 E3: since when it is hidden
    if (!act) {
        if (on) {
            on = 0;
            if (n > 0 && LOGCAP(nl, 160))
                Xlog("spmelee cam: ps35 takedown camera check - %s (%s): both heads in frame on %d %% of %d ticks (ours %d %%, the victim's %d %% - hidden behind our own body / geometry on %d %%, counted as out, ps39), the lens came within %.2f m of a body axis%s", nm, kd, nb * 100 / n, n, n0 * 100 / n, n1 * 100 / n, nocc * 100 / n, (double) dmin, nb * 100 < n * 80 ? " - FRAMING FAULT (under 80 %)" : dmin < 0.35f ? " - LENS IN A BODY" : "");
        }
        return;
    }
    if (!on) {
        on = 1;
        n = n0 = n1 = nb = nocc = 0;
        dmin = 99.0f;
        snprintf(nm, sizeof nm, "%s", name && name[0] ? name : "?");
        kd = kind ? kind : "";
        bad_at = 0;
        tripped = 0;
    }
    float cp[3], cf[3], a[16], b[16];
    const float *me = PlayerLocator(a), *ot =
        other_h ? PuppetLocator(other_h, b) : NULL;
    if (!me || !ot || !XvFin3(me) || !XvFin3(ot) || !RcamGet(cp, cf))
        return;
    float fh = __builtin_sqrtf(cf[0] * cf[0] + cf[2] * cf[2]);
    if (!(fh > 0.2f))
        return;
    float fv = MfReadFov();
    if (!(fv > 30.0f))
        fv = 70.0f;
    float th = __builtin_tanf(fv * 0.5f * 0.0174533f), tv =
        th * 9.0f / 16.0f, rx = -cf[2] / fh, rz = cf[0] / fh, ux =
        -cf[1] * cf[0] / fh, uy = fh, uz = -cf[1] * cf[2] / fh;
    float H[2][3] =
        { { me[0], me[1] + 1.55f, me[2] }, { ot[0], ot[1] + 1.55f,
                                            ot[2] } }, p3[3], c3[3], h3[3];
    int in[2] = { 0, 0 };
    XvBody(SID_PLAYER, me, p3, c3, h3);
    if (XvFin3(h3) && __builtin_fabsf(h3[1] - me[1]) < 2.2f) {
        H[0][0] = h3[0];
        H[0][1] = h3[1];
        H[0][2] = h3[2];
    }
    XvBody(other_h, ot, p3, c3, h3);
    if (XvFin3(h3) && __builtin_fabsf(h3[1] - ot[1]) < 2.2f) {
        H[1][0] = h3[0];
        H[1][1] = h3[1];
        H[1][2] = h3[2];
    }
    for (int i = 0; i < 2; i++) {
        float vx = H[i][0] - cp[0], vy = H[i][1] - cp[1], vz =
            H[i][2] - cp[2], dep = vx * cf[0] + vy * cf[1] + vz * cf[2];
        if (dep > 0.05f) {
            float sx = (vx * rx + vz * rz) / dep / th, sy =
                (vx * ux + vy * uy + vz * uz) / dep / tv;
            in[i] = sx >= -1.0f && sx <= 1.0f && sy >= -1.0f && sy <= 1.0f;
        }
    }
    float d0 =
        cp[1] >
        me[1] +
        1.95f ? 9.0f : __builtin_sqrtf((cp[0] - me[0]) * (cp[0] - me[0]) +
                                       (cp[2] - me[2]) * (cp[2] - me[2])),
        d1 =
        cp[1] >
        ot[1] +
        1.95f ? 9.0f : __builtin_sqrtf((cp[0] - ot[0]) * (cp[0] - ot[0]) +
                                       (cp[2] - ot[2]) * (cp[2] - ot[2]));
    if (g_ps39[PS39_MF] && in[1] && S39Occ(&oc, cp, H[1], other_h)) {
        in[1] = 0;
        nocc++;
        if (!occ_at)
            occ_at = GetTickCount64();
    } else
        occ_at = 0;             // ps39 C5: hidden by our own body = not in frame
    n++;
    n0 += in[0];
    n1 += in[1];
    nb += in[0] && in[1];
    if (d0 < dmin)
        dmin = d0;
    if (d1 < dmin)
        dmin = d1;
    if (xc && !tripped && g_xcamOn && g_xcam_w > 0.6f && !g_xcam_out && !g_ehbOn) {   // ps38 live guard
        float g0 =
            cp[1] >
            me[1] +
            2.4f ? 9.0f : __builtin_sqrtf((cp[0] - me[0]) *
                                          (cp[0] - me[0]) + (cp[2] -
                                                             me[2]) *
                                          (cp[2] - me[2])), g1 =
            cp[1] >
            ot[1] +
            2.4f ? 9.0f : __builtin_sqrtf((cp[0] - ot[0]) *
                                          (cp[0] - ot[0]) + (cp[2] -
                                                             ot[2]) *
                                          (cp[2] - ot[2]));
        // ps40 E3 (rec39a A 13652 'camera for us (attacker): TLOU2's own authored camera of cover-face-smash (2 candidates, camera 1 first', clip A
        // 87.5-88.3: our own backpack over the frame): an authored camera under which the victim's head is hidden behind our own body for 0.6 s is as bad
        // as one that has both heads out of frame - the guard stops it (it eases onto the duel framing / the game camera).
        uint64_t nowm = GetTickCount64();
        int hid40 = g_ps40[PS40_SIDE] && occ_at && nowm - occ_at > 600;
        int bad = (!in[0] && !in[1]) || g0 < 0.30f || g1 < 0.30f || hid40;
        if (!bad)
            bad_at = 0;
        else if (!bad_at)
            bad_at = nowm;
        else if (nowm - bad_at > 250
                 && GetFileAttributesA("t2r-ps38-cam-guard-old.txt") ==
                 INVALID_FILE_ATTRIBUTES) {
            tripped = 1;
            static int nlg;
            if (LOGCAP(nlg, 80))
                Xlog("spmelee cam: ps38 live guard - %s (%s): %s for > 0.25 s (lens %.2f m from our axis / %.2f m from the other body's) - the camera is stopped and eases out onto the duel framing / game camera (frame %llu)", nm, kd, hid40 ? "the victim's head hidden behind our own body / geometry (ps40: 0.6 s)" : (!in[0] && !in[1]) ? "both heads out of frame" : "the lens within 0.30 m of a body axis", (double) g0, (double) g1, (unsigned long long) g_frame);
            XcamStop
                ("ps38 live guard: the lens in a body / both heads out of frame");
        }
    } else if (!xc)
        bad_at = 0;
}

static int S32FinTick(uint64_t now)
{
    static float p_from;
    static uint64_t off_ms;
    static int nl;
    int fin = 0;
    if (g_s32.on) {
        uint64_t el = now - g_s32.t0;
        // ps35: the push ends at the ATTACKER's release (ctxdown's 'attacker released at ... s': control is back), not at the victim's hand-off - uc5 A: the
        // lens sat inside the two bodies from the release at 1.78 s (f17029) to the hand-off at 3.5 s (f17132)
        static int relp;
        static uint64_t rel_t0;
        int rel_now = 0;
        static uint64_t rel42;
        static float rp42[2];   // ps42 H1 (PS42_HOLD): the release does not end the shot at once
        if (rel_t0 != g_s32.t0) {
            rel_t0 = g_s32.t0;
            relp = g_cdSfx.att_rel;
            rel42 = 0;
        } else {
            rel_now = g_cdSfx.att_rel && !relp;
            relp = g_cdSfx.att_rel;
        }
        if (g_ps42[PS42_HOLD] && g_s42Side && (rel_now || rel42)) {
            float lb42[16];
            const float *l42 = PlayerLocator(lb42);
            int lok42 = l42 && XvFin3(l42);
            if (rel_now && !rel42 && lok42) {
                rel42 = now;
                rp42[0] = l42[0];
                rp42[1] = l42[2];
                rel_now = 0;
                if (LOGCAP(nl, 160))
                    Xlog("spmelee cam: ps42 release hold - the attacker is released %llu ms into the takedown: the side-on shot holds up to %d ms more (the victim's fall), ends when our body moves %.2f m (frame %llu)", (unsigned long long) el, S42_HOLD_MS, (double) S42_HOLD_M, (unsigned long long) g_frame);
            } else if (rel42) {
                float mx = lok42 ? l42[0] - rp42[0] : 9.0f, mz =
                    lok42 ? l42[2] - rp42[1] : 9.0f;
                rel_now = now - rel42 > S42_HOLD_MS || mx * mx + mz * mz > S42_HOLD_M * S42_HOLD_M || MduSwinging();   // (a new swing of ours ends the hold too)
            }
        }
        const char *endw =
            now >=
            g_s32.
            until ? "the move is over" : rel_now ?
            "the attacker is released" : (el > 300 && !PsyncOwn()
                                          && !rel42) ?
            "the paired move ended early" : (g_downed
                                             || g_downDying) ?
            "we are down" : MatchSubstate() !=
            6 ? "left the match" : g_xcamOn ? "a takedown camera started"
            : !g_ps32[PS32_FIN] ? "switched off" : NULL;
        if (endw) {
            g_s32.on = 0;
            if (LOGCAP(nl, 160))
                Xlog("spmelee cam: ps32 finisher push ends after %llu ms (%s) at weight %.2f - eases out over %.0f ms, the framing releases (frame %llu)", (unsigned long long) el, endw, (double) g_s32.p, (double) (S32_OUT * 1000.0f), (unsigned long long) g_frame);
        } else {
            fin = 1;
            for (int i = 0; i < g_s32.kn && i < 12; i++)
                if (!(g_s32.kdone >> i & 1)
                    && (float) el >= g_s32.kt[i] * 1000.0f) {
                    g_s32.kdone |= 1 << i;
                    MfkAdd(MFK32_FINISHER, 1.0f, 1.0f, 0.0f);  // SP: camera-shake-melee-hit-b at 0.5 on a finisher's blows
                    if (LOGCAP(nl, 160))
                        Xlog("spmelee cam: ps32 finisher blow shake %d of %d at +%llu ms (scale %.2f)", i + 1, g_s32.kn, (unsigned long long) el, (double) MFK32_FINISHER);
                }
        }
    }
    float p = g_s32.p;
    if (fin) {
        float x =
            ((float) (now - (g_s44Pt0 > g_s32.t0 ? g_s44Pt0 : g_s32.t0))
             / 1000.0f -
             S32_IN_DELAY) / (g_s32.ctx ? S32_IN_CTX : S32_IN_FREE);
        x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
        float s = x * x * (3.0f - 2.0f * x);
        if (g_s44Under) {
            s = 0.0f;
            p = p > 0.02f ? p * 0.85f : 0.0f;
        }                       // ps44 R3: under a requested two-shot the push stays out (a leftover of the last push decays)
        if (s > p)
            p = s;
        off_ms = 0;
    } else if (p > 0.0f) {
        if (!off_ms) {
            off_ms = now;
            p_from = p;
        }
        float x = (float) (now - off_ms) / 1000.0f / S32_OUT;
        x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
        p = x >= 1.0f ? 0.0f : p_from * (1.0f - x * x * (3.0f - 2.0f * x));
    }
    g_s32.p = p;
    return fin;
}

// ======================================================================================================================================================
// ps41 H "behind-the-body hold" (rec40a A 165.03-172.07 s, B 186.57-190.47 s: after an execution the lens stood in FRONT of and above our own
// fighter for 4-7 s; A 18764-18810 'xcam: hand-back ... azimuth ended 185 deg', B 22570-22615 '-16 deg'; camrec with spm 0 / xcam 0 'cyaw +163 / -176'
// constant = the ENGINE camera owns the lens and still looks the way it did before the move turned / moved our body; no engine heading setter here).
// When a move camera of ours (execution / takedown / finisher camera via xcam, the takedown rig push) hands back and the engine lens stands more than
// S41B_FAR round from behind our body's facing, the hand-back does not land on the engine lens: one arc round our head (the hand-back's own time) to SP's
// idle spot relative to the BODY's facing (0.80 m beside on the engine camera's own shoulder side, 0.90 m behind, 1.55 m up, looking along the facing
// 16 deg down, game fov; a ray from our head shortens the arm in front of geometry), held until the engine lens is within S41B_NEAR of ours, the camera
// is steered (the ps40 stand-in: the engine heading turned > 12 deg in 0.3 s), a duel rig / another camera of ours starts, or S41B_MAX_S passed.
// Last stage of the chain (icam_filter, after the lens guard). Not while downed / dead. t2r-ps41-cam-behind-old.txt = the plain hand-back. Logs "behindcam:".
#define S41B_FAR 1.7453f        // rad = 100 deg
#define S41B_NEAR 0.7854f       // rad = 45 deg
#define S41B_MAX_S 6.0f         // s [TUNING ps41]: the hold's hard time-out
#define S41B_REL 0.6f           // s: the release ease onto the engine camera
#define S41B_REL_TO 1.0f        // s: the time-out's ease; [TUNING ps41] also a steered release over an arc of 180 deg (0.6 s up to 90 deg, linear between)
#define S41B_IN 0.6f            // s [TUNING ps41]: the way in when the move has no timed hand-back of its own (rig push)
static volatile int g_ps41b = 1;
#define S42B_SIDE 0.60f         // m [TUNING ps42]: the hold's lens beside our root
#define S42B_BACK 1.60f         // m [TUNING ps42]: behind it
#define S42B_UP 1.65f           // m [TUNING ps42]: above it
#define S42B_AHEAD 4.40f        // m [TUNING ps42]: the looked-at point in front of our body
#define S42B_DROP 1.06f         // m [TUNING ps42]: under the lens (10 deg down over 6 m)
static struct {
    volatile int on;            // 0 = off, 1 = on its way in / held, 2 = releasing (main thread writes)
    volatile LONG gen;          // bumped at every engage and release start: the render thread starts a new arc from the shown pose
    volatile double t0;
    volatile float len;         // the running arc (gsh_now s)
    volatile float H[3], F[2], piv[3];  // main -> render: the lens, our body's facing (unit, horizontal), the pivot (root + 1.5 m)
    volatile float ep[3], ef[2];
    volatile int e_ok;
    volatile double e_t;        // render -> main: the engine lens + its horizontal +Z axis
    volatile int rdone;         // render -> main: the release arc reached the incoming camera
} g_s41b;
static int SpmFwdSign(void);
// render thread, icam_filter's last stage: r = the camera after every other stage, loc = the engine's
static void *S41bFilter(uint8_t *dst, const float *loc, void *r)
{
    static float buf[2][32] __attribute__((aligned(16)));
    static struct {
        LONG gen;
        int ok, dok;
        float P0[3], Q0[4], A0, R0, Dp;
    } S[2];
    uint8_t *m = IcamManager();
    int k = !m ? -1 : dst == m + 0x130 ? 0 : dst == m + 0x550 ? 1 : -1;
    if (k < 0 || !r || IsBadReadPtr(loc, 0x80) || IsBadReadPtr(r, 0x80))
        return r;
    if (k == 0) {
        static const float Z[3] = { 0, 0, 1 };
        float f[3], q[4] = { loc[4], loc[5], loc[6], loc[7] };
        QRot(q, Z, f);
        float hl = __builtin_sqrtf(f[0] * f[0] + f[2] * f[2]);
        if (hl > 0.05f && hl < 1.5f && __builtin_fabsf(loc[0]) < 1e6f
            && __builtin_fabsf(loc[1]) < 1e6f
            && __builtin_fabsf(loc[2]) < 1e6f) {
            g_s41b.ep[0] = loc[0];
            g_s41b.ep[1] = loc[1];
            g_s41b.ep[2] = loc[2];
            g_s41b.ef[0] = f[0] / hl;
            g_s41b.ef[1] = f[2] / hl;
            g_s41b.e_t = GshNow();
            g_s41b.e_ok = 1;
        }
    }
    int on = g_s41b.on;
    if (!on) {
        S[k].ok = 0;
        return r;
    }
    const float *U = (const float *) r;
    double now = GshNow();
    float T[3] = { g_s41b.H[0], g_s41b.H[1], g_s41b.H[2] }, pv[3] =
        { g_s41b.piv[0], g_s41b.piv[1], g_s41b.piv[2] };
    float tg[3] = { T[0] + g_s41b.F[0] * 4.806f, T[1] - 1.378f, T[2] + g_s41b.F[1] * 4.806f };  // 5 m along the facing, SP's -16 deg pitch
    // ps42 H2 (rec41a A 11631 'behindcam: ... our behind-the-body lens is kept (0.80 m beside our right shoulder / 0.90 m behind / 1.55 m up', clip A
    // 62.4-68.6: our own fighter cropped at the left edge): a lens 0.80 m beside / 0.90 m behind looking ALONG the facing has our body 42 deg off its
    // axis - outside a 35 deg half fov; and 16 deg down from 1.55 m crops the head. The hold's lens: S42B_SIDE beside / S42B_BACK behind / S42B_UP up,
    // looking at a point S42B_AHEAD in front of our BODY (toe-in: our root at screen x -0.38, head and hips in frame), S42B_DROP under the lens (-10 deg).
    if (g_ps42x) {
        tg[0] = pv[0] + g_s41b.F[0] * S42B_AHEAD;
        tg[1] = T[1] - S42B_DROP;
        tg[2] = pv[2] + g_s41b.F[1] * S42B_AHEAD;
    }
    LONG g = g_s41b.gen;
    if (!S[k].ok || S[k].gen != g) {    // a new arc: from what was shown last frame (the move camera on its way out / our own held lens)
        const float *b = g_lshowOk[k]
            && now - g_lshowT[k] < 0.1 ? g_lshow[k] : U;
        memcpy(S[k].P0, b, 12);
        memcpy(S[k].Q0, b + 4, 16);
        S[k].ok = 1;
        S[k].gen = g;
        S[k].dok = 0;
        S[k].A0 = __builtin_atan2f(S[k].P0[0] - pv[0], S[k].P0[2] - pv[2]);
        S[k].R0 =
            __builtin_sqrtf((S[k].P0[0] - pv[0]) * (S[k].P0[0] - pv[0]) +
                            (S[k].P0[2] - pv[2]) * (S[k].P0[2] - pv[2]));
    }
    float len = g_s41b.len, x =
        len > 0.02f ? (float) (now - g_s41b.t0) / len : 1.0f;
    x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    float s = x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
    if (on == 2 && x >= 1.0f) {
        if (k == 0)
            g_s41b.rdone = 1;
        return r;
    }
    float D[3], Dq[4], P[3], Q[4];
    if (on == 1) {
        memcpy(D, T, sizeof D);
        if (!S39LookAt(T, tg, S[k].Q0, Dq))
            return r;
    } else {
        memcpy(D, U, sizeof D);
        memcpy(Dq, U + 4, sizeof Dq);
    }
    if (x >= 1.0f) {
        memcpy(P, D, sizeof P);
        memcpy(Q, Dq, sizeof Q);
    } else {                    // round our head, the arc's way kept from its first frame; over a wide arc the lens looks at our head through the middle
        float a1 = __builtin_atan2f(D[0] - pv[0], D[2] - pv[2]), r1 =
            __builtin_sqrtf((D[0] - pv[0]) * (D[0] - pv[0]) +
                            (D[2] - pv[2]) * (D[2] - pv[2]));
        float da = __builtin_remainderf(a1 - S[k].A0, 6.2831853f);
        if (S[k].dok)
            da = S[k].Dp + __builtin_remainderf(a1 - S[k].A0 - S[k].Dp,
                                                6.2831853f);
        S[k].Dp = da;
        S[k].dok = 1;
        float big = (__builtin_fabsf(da) - 0.5f) / 0.7f;
        big = big < 0.0f ? 0.0f : big > 1.0f ? 1.0f : big;
        float rl = S[k].R0 + (r1 - S[k].R0) * s, rr =
            rl + __builtin_sinf(3.14159265f * s) * big * (rl <
                                                          0.9f ? 0.9f -
                                                          rl : 0.0f), a =
            S[k].A0 + da * s;
        if (rr < 0.2f)
            rr = 0.2f;
        P[0] = pv[0] + __builtin_sinf(a) * rr;
        P[1] = S[k].P0[1] + (D[1] - S[k].P0[1]) * s;
        P[2] = pv[2] + __builtin_cosf(a) * rr;
        float Lq[4], e1 = x / 0.25f, e2 = (x - 0.45f) / 0.55f;
        e1 = e1 > 1.0f ? 1.0f : e1;
        e1 = e1 * e1 * (3.0f - 2.0f * e1);
        e2 = e2 < 0.0f ? 0.0f : e2 > 1.0f ? 1.0f : e2;
        e2 = e2 * e2 * (3.0f - 2.0f * e2);
        memcpy(Q, S[k].Q0, sizeof Q);
        if (big > 0.01f && S39LookAt(P, pv, S[k].Q0, Lq)) {
            Pc5Slerp(Q, Lq, e1 * big);
            Pc5Slerp(Q, Dq, e2 * big + s * (1.0f - big));
        } else
            Pc5Slerp(Q, Dq, s);
    }
    for (int i = 0; i < 3; i++)
        if (!(__builtin_fabsf(P[i]) < 1e6f))
            return r;
    for (int i = 0; i < 4; i++)
        if (!(__builtin_fabsf(Q[i]) <= 1.01f))
            return r;
    memcpy(buf[k], r, sizeof buf[k]);
    buf[k][0] = P[0];
    buf[k][1] = P[1];
    buf[k][2] = P[2];
    buf[k][4] = Q[0];
    buf[k][5] = Q[1];
    buf[k][6] = Q[2];
    buf[k][7] = Q[3];
    return buf[k];
}

// main thread, every tick (spm_tick)
static void S41bTick(uint64_t now, float dt)
{
    static uint64_t cfg_at, rj;
    static int first = 1, far_n, hb_was, told, nlog, sgn = 1, f_ok, mv_x;
    static double mv_t, on_t, rel_t, y_t;
    static char mv_name[64];
    static float fx, fz = 1.0f, rx, ry, rz, arm = 1.0f, armt =
        1.0f, rlen, y_ref, eng0;
    static LONG gen37;
    if (first || now - cfg_at >= 2000) {
        cfg_at = now;
        int v =
            GetFileAttributesA("t2r-ps41-cam-behind-old.txt") ==
            INVALID_FILE_ATTRIBUTES;
        if (v != g_ps41b || first)
            Xlog("behindcam: ps41 t2r-ps41-cam-behind-old.txt %s",
                 v ?
                 "absent - after an execution / takedown / finisher camera the lens is kept behind our body while the engine camera stands > 100 deg round from behind it: ON"
                 : "present - the plain hand-back onto the engine camera");
        {
            int v2 =
                GetFileAttributesA("t2r-ps41-cam-vis-old.txt") ==
                INVALID_FILE_ATTRIBUTES;
            if (v2 != g_ps41v || first)
                Xlog("spmelee cam: ps41 t2r-ps41-cam-vis-old.txt %s",
                     v2 ?
                     "absent - the victim's side shot and the attacker's authored takedown camera are chosen by who covers the victim (torso capsules + rays), one late cut to the other side: ON"
                     : "present - the ps40 choice");
            g_ps41v = v2;
        }
        g_ps41b = v;
        first = 0;
    }
    double t = GshNow();
    int xin = g_xcamOn && g_xcam_role == 1 && !g_xcam_out
        && g_xcam_w > 0.5f, rin = g_s32.on != 0;
    static int mv_ax;           // ps44 R12: the move was an execution that played its 2013 authored track on our (the attacker's) screen
    if (xin || rin) {
        mv_t = t;
        mv_x = xin;
        snprintf(mv_name, sizeof mv_name, "%s",
                 xin ? g_xcamName : "the takedown rig push");
        mv_ax = xin && g_ps44[PS44_ATTEXEC] && g_xcamTrk && !g_xcamShiv
            && strncmp(g_xcamName, "ctxdown", 7) != 0;
    }
    int hb = !xin && !rin && mv_t > 0.0
        && ((mv_x && g_xcamOn && g_xcam_out && g_xcam_role == 1)
            || t - mv_t < 0.6);
    if (mv_ax && !g_s44x.req && !g_ps45[PS45_PEXEC])
        hb = 0;                 // (ps44 R12: its own blend-out hands back - no behind hold; ps45 P1: the hold again AFTER the track)
    int on = g_s41b.on;
    static double f_until;      // ps44 R4: until then the way in from a two-shot runs (no release)
    int force = g_s44x.req > 0;
    if (force && !g_ps41b) {
        g_s44x.req = -1;
        force = 0;
    }
    if (!hb && !on && !force) {
        if (rj) {
            RayT(rj);
            rj = 0;
        }
        far_n = 0;
        hb_was = 0;
        f_ok = 0;
        arm = armt = 1.0f;
        return;
    }
    float pb[16];
    const float *me = PlayerLocator(pb);
    float hp = LocalHp();
    int alive = me && PsLocOk(me) && MatchSubstate() == 6 && !g_downed
        && !g_fcam_on && !g_icam_on && !(hp >= 0.0f && hp <= 0.001f);
    if (!alive) {
        if (force)
            g_s44x.req = -1;
        if (rj) {
            RayT(rj);
            rj = 0;
        }
        if (on) {
            g_s41b.on = 0;
            if (LOGCAP(nlog, 300))
                Xlog("behindcam: ps41 - let go at once after %.0f ms: we are down / dead / out of the match (frame %llu)", (t - on_t) * 1000.0, (unsigned long long) g_frame);
        }
        far_n = 0;
        f_ok = 0;
        return;
    }
    // our body: root + facing (eased), the lens spot, the head -> lens ray
    {
        const float *q = me + 4;
        float ax = 2.0f * (q[0] * q[2] + q[3] * q[1]), az =
            1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]), l =
            __builtin_sqrtf(ax * ax + az * az);
        if (l > 0.2f) {
            ax /= l;
            az /= l;
        } else {
            ax = fx;
            az = fz;
        }
        if (!f_ok
            || (me[0] - rx) * (me[0] - rx) + (me[2] - rz) * (me[2] - rz) >
            4.0f) {
            fx = ax;
            fz = az;
            rx = me[0];
            ry = me[1];
            rz = me[2];
            f_ok = 1;
        } else {
            float kf = 1.0f - __builtin_expf(-dt / 0.25f), kr =
                1.0f - __builtin_expf(-dt / 0.10f);
            fx += (ax - fx) * kf;
            fz += (az - fz) * kf;
            l = __builtin_sqrtf(fx * fx + fz * fz);
            if (l > 0.1f) {
                fx /= l;
                fz /= l;
            } else {
                fx = ax;
                fz = az;
            }
            rx += (me[0] - rx) * kr;
            ry += (me[1] - ry) * kr;
            rz += (me[2] - rz) * kr;
        }
    }
    int eok = g_s41b.e_ok && t - g_s41b.e_t < 0.25;
    float ex = g_s41b.ep[0] - rx, ez = g_s41b.ep[2] - rz, el =
        __builtin_sqrtf(ex * ex + ez * ez);
    float angb = eok && el > 0.15f ? __builtin_acosf(fminf(1.0f, fmaxf(-1.0f, -(ex * fx + ez * fz) / el))) : 0.0f;      // the engine lens, round from straight behind our body
    if (!on && eok) {           // (the shoulder: the engine camera's own - its lens beside its view line through our root; kept while the hold runs)
        float vs = (float) SpmFwdSign(), vx = g_s41b.ef[0] * vs, vz =
            g_s41b.ef[1] * vs;
        sgn = (ex * -vz + ez * vx) >= 0.0f ? 1 : -1;
    }
    float cp44[3] = { 0, 0, 0 }, cf44[3];
    int cok44 = force && RcamGet(cp44, cf44);
    if (cok44)
        sgn = ((cp44[0] - rx) * -fz + (cp44[2] - rz) * fx) >= 0.0f ? 1 : -1;    // ps44 R4: the shoulder on the two-shot lens's side (the shorter arc, away from our face)
    float sd42 = g_ps42x ? S42B_SIDE : 0.80f, bk42 = g_ps42x ? S42B_BACK : 0.90f;       // ps42 H2 (s41b_filter's note)
    float pv[3] = { rx, ry + 1.5f, rz }, C[3] =
        { rx - fz * (float) sgn * sd42 - fx * bk42,
ry + (g_ps42x ? S42B_UP : 1.55f), rz + fx * (float) sgn * sd42 - fz * bk42 };
    float cl =
        __builtin_sqrtf((C[0] - pv[0]) * (C[0] - pv[0]) +
                        (C[1] - pv[1]) * (C[1] - pv[1]) + (C[2] -
                                                           pv[2]) * (C[2] -
                                                                     pv
                                                                     [2]));
    if (rj) {
        g_rayErr = 0;
        float tr = RayT(rj);
        rj = 0;
        if (!g_rayErr) {
            armt = tr >= 0.0f
                && tr < 0.98f ? (tr * (cl + 0.25f) - 0.25f) / cl : 1.0f;
            armt = armt < 0.25f ? 0.25f : armt > 1.0f ? 1.0f : armt;
        }
    }
    {
        float E[3] =
            { C[0] + (C[0] - pv[0]) / cl * 0.25f,
       C[1] + (C[1] - pv[1]) / cl * 0.25f,
       C[2] + (C[2] - pv[2]) / cl * 0.25f };
        rj = WorldRayKick(pv, E);
    }
    arm +=
        (armt - arm) * (1.0f -
                        __builtin_expf(-dt /
                                       (armt < arm ? 0.05f : 0.30f)));
    for (int i = 0; i < 3; i++) {
        g_s41b.H[i] = pv[i] + (C[i] - pv[i]) * arm;
        g_s41b.piv[i] = pv[i];
    }
    g_s41b.F[0] = fx;
    g_s41b.F[1] = fz;
    float yaw = eok ? __builtin_atan2f(g_s41b.ef[0], g_s41b.ef[1]) : y_ref;
    if (force) {                // ps44 R4 (the note at ps44_cfg): the two-shot of a finisher / takedown / execution ends - this lens takes over from the shown pose
        float arcd = -1.0f, nearb = 99.0f;
        const char *cw = NULL;
        if (!cok44)
            cw = "the shown lens is not readable";
        else {
            float a0 =
                __builtin_atan2f(cp44[0] - pv[0], cp44[2] - pv[2]), a1 =
                __builtin_atan2f(g_s41b.H[0] - pv[0], g_s41b.H[2] - pv[2]),
                da = __builtin_remainderf(a1 - a0, 6.2831853f);
            float r0 =
                __builtin_sqrtf((cp44[0] - pv[0]) * (cp44[0] - pv[0]) +
                                (cp44[2] - pv[2]) * (cp44[2] - pv[2])),
                r1 =
                __builtin_sqrtf((g_s41b.H[0] - pv[0]) * (g_s41b.H[0] -
                                                         pv[0]) +
                                (g_s41b.H[2] - pv[2]) * (g_s41b.H[2] -
                                                         pv[2]));
            float af = __builtin_atan2f(fx, fz), ob[16];
            const float *ot =
                g_s44x.oh ? PuppetLocator(g_s44x.oh, ob) : NULL;
            int front = 0;
            arcd = __builtin_fabsf(da);
            for (int i = 1; i <= 8; i++) {
                float u = (float) i / 8.0f, a = a0 + da * u, rr =
                    r0 + (r1 - r0) * u, qx =
                    pv[0] + __builtin_sinf(a) * rr, qz =
                    pv[2] + __builtin_cosf(a) * rr;
                if (i < 8
                    &&
                    __builtin_fabsf(__builtin_remainderf
                                    (a - af, 6.2831853f)) < 0.5236f)
                    front = 1;  // within 30 deg of straight in front of our body
                float d1 =
                    __builtin_sqrtf((qx - rx) * (qx - rx) +
                                    (qz - rz) * (qz - rz));
                if (d1 < nearb)
                    nearb = d1;
                if (ot && XvFin3(ot)) {
                    float d2 =
                        __builtin_sqrtf((qx - ot[0]) * (qx - ot[0]) +
                                        (qz - ot[2]) * (qz - ot[2]));
                    if (d2 < nearb)
                        nearb = d2;
                }
            }
            cw = arcd >
                S44X_ARC ? "the arc round our head is over 120 deg" : front
                ? "the arc would pass in front of our body" : nearb <
                0.60f ? "the arc would pass within 0.6 m of a body" : NULL;
        }
        float len = cw ? 0.0f : S44X_LEN;
        g_s41b.len = len;
        g_s41b.t0 = t;
        g_s41b.rdone = 0;
        InterlockedIncrement(&g_s41b.gen);
        MemoryBarrier();
        g_s41b.on = 1;
        MemoryBarrier();
        g_s44x.off = 1;
        g_s44x.req = 0;
        on_t = t;
        y_ref = yaw;
        y_t = t;
        gen37 = g_s37Gen;
        eng0 = angb;
        far_n = 0;
        hb_was = 1;
        told = 1;
        f_until = t + (double) len;
        snprintf(mv_name, sizeof mv_name, "the two-shot of %s",
                 g_s44x.name);
        if (LOGCAP(nlog, 300))
            Xlog("paircam: ps44 exit - the two-shot of %s hands DIRECTLY to the behind-the-body lens (%.2f m beside our %s shoulder / %.2f m behind, arm %.0f %% clear): %s (%.0f deg round our head from the shown lens, the arc's nearest pass to a body %.2f m%s%s); no camera under it is shown; the behind lens releases by its own rules after that (engine camera %.0f deg round from behind our body; t2r-ps44-cam-exit-old.txt = ps43's ease onto the camera under it; frame %llu)", g_s44x.name, (double) sd42, sgn > 0 ? "right" : "left", (double) bk42, (double) (arm * 100.0f), cw ? "ONE CUT" : "one ease of 500 ms along the arc on the far side from our face", (double) (arcd * 57.29578f), (double) nearb, cw ? "; " : "", cw ? cw : "", (double) (angb * 57.29578f), (unsigned long long) g_frame);
        return;
    }
    if (!on) {
        int free41 = g_ps41b && eok && !(g_spm.ok && g_spm.w > 0.02f)
            && !(g_pcam.w > 0.001f) && !g_pc5.on;
        if (!hb_was) {
            hb_was = 1;
            told = 0;
            far_n = 0;
        }
        // ps42 H2 (A 9487 'hand-back from fist-stomp-execute-left: the engine camera stands 92 deg round from behind our body (limit 100): the plain
        // hand-back', clip A 25.9-31: the stale engine lens over our own shoulder, head cropped): the limit is S42_ENG_RAD (60 deg) [TUNING ps42]
        if (free41 && angb > (g_ps42x ? S42_ENG_RAD : S41B_FAR))
            far_n++;
        else
            far_n = 0;
        if (far_n >= 2) {
            float len = S41B_IN;
            if (g_ehbOn) {
                len =
                    (float) g_ehb_len -
                    (float) ((PsyncMs() - g_ehb_t0) / 1000.0);
                len = len < 0.3f ? 0.3f : len > 1.5f ? 1.5f : len;
            }
            if (mv_ax && g_ps45[PS45_PEXEC]) {  // ps45 P1 (the note at PS45_PEXEC): after an authored attacker execution - one smooth way of S45P_MIN..S45P_MAX
                float l0 = len;
                len =
                    len < S45P_MIN ? S45P_MIN : len >
                    S45P_MAX ? S45P_MAX : len;
                if (LOGCAP(nlog, 300))
                    Xlog("behindcam: ps45 postexec - the 2013 authored camera of %s has ended on the attacker's screen with the engine lens %.0f deg round from behind our body: eased round our head to the behind-the-body follow lens over %.0f ms (the hand-back had %.0f ms left; no cut), then handed back by the ps41 rules (t2r-ps45-cam-postexec-old.txt = ps44's plain hand-back; frame %llu)", mv_name, (double) (angb * 57.29578f), (double) (len * 1000.0f), (double) (l0 * 1000.0f), (unsigned long long) g_frame);
            }
            g_s41b.len = len;
            g_s41b.t0 = t;
            g_s41b.rdone = 0;
            InterlockedIncrement(&g_s41b.gen);
            MemoryBarrier();
            g_s41b.on = 1;
            on_t = t;
            y_ref = yaw;
            y_t = t;
            gen37 = g_s37Gen;
            eng0 = angb;
            far_n = 0;
            if (LOGCAP(nlog, 300))
                Xlog("behindcam: ps41 - engine camera %.0f deg round from behind our body after %s: our behind-the-body lens is kept (0.80 m beside our %s shoulder / 0.90 m behind / 1.55 m up, arm %.0f %% clear, eased in over %.0f ms round our head; released when the engine lens is within 45 deg, the camera is steered, a rig / camera move starts, or after %.0f s; frame %llu)", (double) (angb * 57.29578f), mv_name, sgn > 0 ? "right" : "left", (double) (arm * 100.0f), (double) (len * 1000.0f), (double) S41B_MAX_S, (unsigned long long) g_frame);
            if (g_ps42x && LOGCAP(nlog, 300))
                Xlog("behindcam: ps42 - the hold's lens is %.2f m beside / %.2f m behind / %.2f m up, looking at a point %.1f m in front of our body (our fighter at screen x -0.38, not cropped); engaged over %.0f deg (was 100) (t2r-ps42-cam-exec-old.txt = ps41; frame %llu)", (double) S42B_SIDE, (double) S42B_BACK, (double) S42B_UP, (double) S42B_AHEAD, (double) (S42_ENG_RAD * 57.29578f), (unsigned long long) g_frame);
        } else if (!told && eok && t - mv_t > 0.1) {
            told = 1;
            if (LOGCAP(nlog, 300))
                Xlog("behindcam: ps41 - hand-back from %s: the engine camera stands %.0f deg round from behind our body (limit 100)%s: the plain hand-back unless it passes the limit before the hand-back ends (frame %llu)", mv_name, (double) (angb * 57.29578f), !g_ps41b ? ", switched off by t2r-ps41-cam-behind-old.txt" : !free41 ? ", a duel rig / pin camera owns the lens" : "", (unsigned long long) g_frame);
        }
        return;
    }
    // engaged: the release
    float hx = g_s41b.H[0] - rx, hz = g_s41b.H[2] - rz, hl =
        __builtin_sqrtf(hx * hx + hz * hz);
    float ango = eok && el > 0.15f && hl > 0.15f ? __builtin_acosf(fminf(1.0f, fmaxf(-1.0f, (ex * hx + ez * hz) / (el * hl)))) : 3.14159265f;   // the engine lens round from OUR lens
    const char *cut = NULL, *why = NULL;
    float rl = S41B_REL;
    if (!g_ps41b)
        cut = "switched off (t2r-ps41-cam-behind-old.txt)";
    else if (gen37 != g_s37Gen)
        cut = "our body was teleported";
    else if ((g_xcamOn && !g_xcam_out) || g_pc5.on || g_s34.vf
             || g_s32.on)
        cut =
            "another camera move of ours starts (it blends / cuts in from our lens)";
    if (cut) {
        g_s41b.on = 0;
        if (LOGCAP(nlog, 300))
            Xlog("behindcam: ps41 - let go at once after %.0f ms: %s (frame %llu)", (t - on_t) * 1000.0, cut, (unsigned long long) g_frame);
        return;
    }
    if (on == 2) {
        if (g_s41b.rdone || t - rel_t > (double) rlen + 0.15)
            g_s41b.on = 0;
        return;
    }
    int steered = 0;
    if (t - y_t >= 0.3) {
        steered = eok && t - on_t > (double) g_s41b.len
            &&
            __builtin_fabsf(__builtin_remainderf(yaw - y_ref, 6.2831853f))
            > 0.2094f;
        y_ref = yaw;
        y_t = t;
    }
    if (t < f_until) {
    }                           // ps44 R4: the way in from a two-shot runs to its end first
    else if (g_spm.ok && g_spm.w > 0.02f) {
        why = "a duel rig starts";
        rl = 0.3f;
    } else if (ango < S41B_NEAR)
        why = "the engine lens is within 45 deg of ours";
    else if (steered) {
        why =
            "the camera is being steered (the engine heading turned > 12 deg in 0.3 s)";
        float u = (ango - 1.5708f) / 1.5708f;
        u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
        rl = S41B_REL + (S41B_REL_TO - S41B_REL) * u;
    } else if (t - on_t > (double) g_s41b.len + (double) S41B_MAX_S) {
        why = "the hard time-out";
        rl = S41B_REL_TO;
    }
    if (why) {
        rlen = rl;
        rel_t = t;
        g_s41b.len = rl;
        g_s41b.t0 = t;
        g_s41b.rdone = 0;
        InterlockedIncrement(&g_s41b.gen);
        MemoryBarrier();
        g_s41b.on = 2;
        if (LOGCAP(nlog, 300))
            Xlog("behindcam: ps41 - released after %.0f ms: %s - eased onto the incoming camera over %.0f ms (the engine lens stood %.0f deg round from behind our body at the start, now %.0f deg, %.0f deg from our lens; after %s; frame %llu)", (t - on_t) * 1000.0, why, (double) (rl * 1000.0f), (double) (eng0 * 57.29578f), (double) (angb * 57.29578f), (double) (ango * 57.29578f), mv_name, (unsigned long long) g_frame);
    }
}

// ======================================================================================================================================================
// ps43 - the pair-frame two-shot's users (the plan: the block at ps43_cfg).
// VICTIM (M1): s43_vic_step runs in spc_tick5 before the side decision of the finisher side shot; s43_vic_lens hands the planned lens to the 'fixed once'
// code (ps39 C4) - the aim, the cut in, the hold and the ease-out stay as they were. The ps41 late cut stays as the net under it (it re-fits the old way).
static int S43VicStep(uint64_t now, uint64_t t_on, const float *me,
                        const float *ot, uint64_t h, int wok,
                        const float *wp, const float *wn, float rfx,
                        float rfz, float vfD, float *sc)
{
    if (!g_ps43[PS43_VIC] || !me || !ot)
        return 0;
    if (g_s43.view != 1 || g_s43.t0 != t_on) {
        S43Reset();
        g_s43.view = 1;
        g_s43.t0 = t_on;
    }                           // a new shot
    if (g_s43.st == 0) {
        if (!PsyncOwn() && now - t_on < 200
            && !(g_s44m.early == 1 && g_s44m.t0 == t_on))
            return 1;           // (the pair's clips are not known here yet; ps44 R13: an early shot plans from the live roots now)
        S43Build(1, me, ot, h, wok, wp, wn, 2.00f, 3.00f, vfD, S34_VF_UP,
                  1);
        return 1;
    }
    if (g_s43.st == 1)
        S43Score(NULL);
    if (g_s43.st != 2)
        return 0;
    // the planned spot is the decision: the two old side rays do not vote, the side is the one the lens stands on (the attacker's right = (-fz, fx))
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 3; i++)
            g_pc5.rt[k][i] = 1.0f;
    g_pc5.frac = 1.0f;
    g_pc5.side =
        ((g_s43.E[0] - g_s43.M[0]) * -rfz +
         (g_s43.E[2] - g_s43.M[2]) * rfx) >= 0.0f ? 0 : 1;
    sc[0] = sc[1] = 0.0f;
    return 0;
}

static void S43VicLens(uint64_t t_on, float *E)
{                               // spc_tick5, where the finisher side shot's lens is fixed
    if (!g_ps43[PS43_VIC] || g_s43.view != 1 || g_s43.st != 2
        || g_s43.t0 != t_on || g_s41v.flipped)
        return;
    static int nl;
    if (LOGCAP(nl, 200))
        Xlog("paircam: ps43 - the victim's finisher side shot takes the PLANNED lens %.2f %.2f %.2f (the side spot of the first ticks' roots was %.2f %.2f %.2f, %.2f m from it); it holds to the ease-out (t2r-ps43-cam-vic-old.txt = the old spot; frame %llu)", (double) g_s43.E[0], (double) g_s43.E[1], (double) g_s43.E[2], (double) E[0], (double) E[1], (double) E[2], (double) __builtin_sqrtf((E[0] - g_s43.E[0]) * (E[0] - g_s43.E[0]) + (E[2] - g_s43.E[2]) * (E[2] - g_s43.E[2])), (unsigned long long) g_frame);
    E[0] = g_s43.E[0];
    E[1] = g_s43.E[1];
    E[2] = g_s43.E[2];
}

// ATTACKER (M2 / M3 / M5): the chain's LAST stage (icam_filter, after the behind-the-body hold) shows the planned lens looking at the pair's midpoint -
// whatever the rig / the authored track under it does. In: an ease of S43A_IN round the midpoint along the short arc, or ONE cut when that arc is over
// S43A_CUT_ARC or passes within 0.6 m of a body. Held: the lens stands (a live-root plan rides the slow midpoint), the aim follows the midpoint (tau
// 0.35 s); our body's yaw is not an input. Out: one ease of S43A_OUT onto whatever the chain shows then (the behind-the-body hold, the duel rig). A
// takedown's shot ends at the move's end (the victim half), S43A_HOLD_MS after the attacker's release at the latest, on a new swing of ours, when
// S43A_GRACE_MS after the release our root has moved S42_HOLD_M or the engine heading has turned 20 deg (steered) - not on the release itself. An
// execution's shot ends when its 2013 camera starts its way out. Logs "paircam:".
#define S43A_IN 0.25f           // s [TUNING ps43]
#define S43A_OUT 0.50f          // s [TUNING ps43]
#define S43A_CUT_ARC 1.2217f    // rad = 70 deg
#define S43A_HOLD_MS 2500       // ms [TUNING ps43]
#define S43A_GRACE_MS 350       // ms [TUNING ps43]: the attacker half's blend-out still moves our root after the release
#define S43A_UP 1.50f           // m over the roots
#define S43A_AIM 1.05f          // m over the roots: chest height [TUNING ps43]
#define S43A_UP_EXEC 1.30f      // m [TUNING ps43]: the victim is on the ground
#define S43A_AIM_EXEC 0.70f     // m [TUNING ps43]
static struct {
    volatile int on;            // 0 = off, 1 = on its way in / held, 2 = releasing (main thread writes)
    volatile LONG gen;          // bumped at every start / release: the render thread starts a new arc from the shown pose
    volatile double t0;
    volatile float len;         // the running arc (gsh_now s); len 0 = a cut
    volatile float E[3], L[3], piv[3];  // main -> render: the lens, the looked-at point, the arc's pivot (the pair's midpoint)
    volatile int rdone;         // render -> main: the release arc reached the incoming camera
} g_s43a;
// render thread, icam_filter's last stage: r = the camera after every other stage
static void *S43Filter(uint8_t *dst, const float *loc, void *r)
{
    static float buf[2][32] __attribute__((aligned(16)));
    static struct {
        LONG gen;
        int ok, dok;
        float P0[3], Q0[4], A0, R0, Dp;
    } S[2];
    (void) loc;
    uint8_t *m = IcamManager();
    int k = !m ? -1 : dst == m + 0x130 ? 0 : dst == m + 0x550 ? 1 : -1;
    if (k < 0 || !r || IsBadReadPtr(r, 0x80))
        return r;
    int on = g_s44x.off ? 0 : g_s43a.on;        // (ps44 R4: the behind-the-body lens took this shot's exit - s41b_tick)
    if (!on) {
        S[k].ok = 0;
        return r;
    }
    const float *U = (const float *) r;
    double now = GshNow();
    float T[3] = { g_s43a.E[0], g_s43a.E[1], g_s43a.E[2] }, tg[3] =
        { g_s43a.L[0], g_s43a.L[1], g_s43a.L[2] }, pv[3] =
        { g_s43a.piv[0], g_s43a.piv[1], g_s43a.piv[2] };
    LONG g = g_s43a.gen;
    if (!S[k].ok || S[k].gen != g) {    // a new arc: from what was shown last frame
        const float *b = g_lshowOk[k]
            && now - g_lshowT[k] < 0.1 ? g_lshow[k] : U;
        memcpy(S[k].P0, b, 12);
        memcpy(S[k].Q0, b + 4, 16);
        S[k].ok = 1;
        S[k].gen = g;
        S[k].dok = 0;
        S[k].A0 = __builtin_atan2f(S[k].P0[0] - pv[0], S[k].P0[2] - pv[2]);
        S[k].R0 =
            __builtin_sqrtf((S[k].P0[0] - pv[0]) * (S[k].P0[0] - pv[0]) +
                            (S[k].P0[2] - pv[2]) * (S[k].P0[2] - pv[2]));
    }
    float len = g_s43a.len, x =
        len > 0.02f ? (float) (now - g_s43a.t0) / len : 1.0f;
    x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    float s = x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
    if (on == 2 && x >= 1.0f) {
        if (k == 0)
            g_s43a.rdone = 1;
        return r;
    }
    float D[3], Dq[4], P[3], Q[4];
    if (on == 1) {
        memcpy(D, T, sizeof D);
        if (!S39LookAt(T, tg, S[k].Q0, Dq))
            return r;
    } else {
        memcpy(D, U, sizeof D);
        memcpy(Dq, U + 4, sizeof Dq);
    }
    if (x >= 1.0f) {
        memcpy(P, D, sizeof P);
        memcpy(Q, Dq, sizeof Q);
    } else {                    // round the pair's midpoint, the arc's way kept from its first frame
        float a1 = __builtin_atan2f(D[0] - pv[0], D[2] - pv[2]), r1 =
            __builtin_sqrtf((D[0] - pv[0]) * (D[0] - pv[0]) +
                            (D[2] - pv[2]) * (D[2] - pv[2]));
        float da = __builtin_remainderf(a1 - S[k].A0, 6.2831853f);
        if (S[k].dok)
            da = S[k].Dp + __builtin_remainderf(a1 - S[k].A0 - S[k].Dp,
                                                6.2831853f);
        S[k].Dp = da;
        S[k].dok = 1;
        float rl = S[k].R0 + (r1 - S[k].R0) * s, a = S[k].A0 + da * s;
        P[0] = pv[0] + __builtin_sinf(a) * rl;
        P[1] = S[k].P0[1] + (D[1] - S[k].P0[1]) * s;
        P[2] = pv[2] + __builtin_cosf(a) * rl;
        memcpy(Q, S[k].Q0, sizeof Q);
        Pc5Slerp(Q, Dq, s);
    }
    for (int i = 0; i < 3; i++)
        if (!(__builtin_fabsf(P[i]) < 1e6f))
            return r;
    for (int i = 0; i < 4; i++)
        if (!(__builtin_fabsf(Q[i]) <= 1.01f))
            return r;
    memcpy(buf[k], r, sizeof buf[k]);
    buf[k][0] = P[0];
    buf[k][1] = P[1];
    buf[k][2] = P[2];
    buf[k][4] = Q[0];
    buf[k][5] = Q[1];
    buf[k][6] = Q[2];
    buf[k][7] = Q[3];
    return buf[k];
}

// ps44 R7 (the note at S44H_MIN): render thread, after the lens guard - the duel lens is never closer than S44H_MIN to our live head joint
static void *S44hFilter(uint8_t *dst, const float *loc, void *r)
{
    static float buf[2][32] __attribute__((aligned(16)));
    (void) loc;
    uint8_t *m = IcamManager();
    int k = !m ? -1 : dst == m + 0x130 ? 0 : dst == m + 0x550 ? 1 : -1;
    if (k < 0 || !r || IsBadReadPtr(r, 0x80) || !g_ps44[PS44_HEAD])
        return r;
    float w = g_spm.w;
    if (!g_spm.ok || !(w > 0.02f) || !g_s44h.ok || g_xcamOn || g_s41b.on
        || g_s43a.on || g_pc5.on || g_fcam_on || g_icam_on || g_downed)
        return r;
    double now = GshNow();
    if (now - g_s44h.t > 0.15)
        return r;
    float H[3] = { g_s44h.H[0], g_s44h.H[1], g_s44h.H[2] };
    const float *U = (const float *) r;
    float rmin = S44H_MIN * (w < 0.3f ? w / 0.3f : 1.0f);       // (fades in / out with the duel framing: no pop)
    float d[3] = { U[0] - H[0], U[1] - H[1], U[2] - H[2] }, dl =
        __builtin_sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(dl < rmin) || !(dl == dl))
        return r;
    float q0[4] = { U[4], U[5], U[6], U[7] }, f[3];
    SpmFwd(q0, f);
    float dep = rmin - dl;
    if (dl < 0.03f) {
        d[0] = -f[0];
        d[1] = 0.0f;
        d[2] = -f[2];
    } else {
        d[0] /= dl;
        d[1] /= dl;
        d[2] /= dl;
    }
    d[1] += dep / S44H_MIN * 1.2f;      // up and over: the deeper in, the higher
    float ul = __builtin_sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(ul > 1e-3f))
        return r;
    float P[3] =
        { H[0] + d[0] / ul * rmin, H[1] + d[1] / ul * rmin,
   H[2] + d[2] / ul * rmin };
    float tg[3] = { U[0] + f[0] * 3.0f, U[1] + f[1] * 3.0f, U[2] + f[2] * 3.0f }, Q[4]; // re-aimed at the point the lens looked at
    if (!S39LookAt(P, tg, q0, Q))
        return r;
    for (int i = 0; i < 3; i++)
        if (!(__builtin_fabsf(P[i]) < 1e6f))
            return r;
    memcpy(buf[k], r, sizeof buf[k]);
    buf[k][0] = P[0];
    buf[k][1] = P[1];
    buf[k][2] = P[2];
    buf[k][4] = Q[0];
    buf[k][5] = Q[1];
    buf[k][6] = Q[2];
    buf[k][7] = Q[3];
    if (k == 0)
        InterlockedIncrement(&g_s44h.n);
    return buf[k];
}

// main thread, every tick (spm_tick)
static void S43AttTick(uint64_t now, float dt)
{
    static int st, kind, xhand, nl, live;       // st: 0 idle, 1 planning, 2 shown, 3 on its way out; kind: 1 finisher / takedown, 2 execution
    static uint64_t oh, t_req, t_on, rel_at, until;
    static float Ls[3], L0[2], E0[3], rp[2], ry;
    static int rp_ok;
    static LONG gen37;
    static char nm[64];
    static uint64_t t_x4, hid_at;
    static int rp44, rp_done;   // ps44: the exit request's tick; the victim hidden behind our body since; a re-plan runs / was taken
    int xex = g_ps43[PS43_EXEC] && g_xcamOn && g_xcam_role == 1
        && !g_xcamShiv && !g_xcam_out && g_xcam_h
        && strncmp(g_xcamName, "ctxdown", 7) != 0;
    {                           // ps44 R12 (the note at PS44_ATTEXEC): the attacker's execution plays its chosen 2013 track - the two-shot only for the procedural fallback
        static int on12, n12, cam12;
        static char nm12[64];
        int ex12 = g_ps44[PS44_ATTEXEC] && g_xcamOn && g_xcam_role == 1
            && !g_xcamShiv && strncmp(g_xcamName, "ctxdown", 7) != 0;
        if (ex12 && xex && (g_xcam_choice_state < 2 || g_xcamTrk))
            xex = 0;
        if (ex12 && g_xcam_choice_state >= 2 && g_xcamTrk) {
            if (!on12) {
                on12 = 1;
                n12 = 0;
                cam12 = XcamCamIx(g_xcamTrk);
                snprintf(nm12, sizeof nm12, "%s", g_xcamName);
                Xlog("paircam: ps44 attexec %s - the attacker's screen plays the 2013 authored camera %d (no pair-frame two-shot, no behind-the-body hold after it; t2r-ps44-cam-attexec-old.txt = ps43; frame %llu)", nm12, cam12, (unsigned long long) g_frame);
            }
            if (g_xcam_w > 0.001f)
                n12++;
        } else if (on12) {
            on12 = 0;
            Xlog("paircam: ps44 attexec %s - 2013 camera %d played on %d tick(s) (frame %llu)", nm12, cam12, n12, (unsigned long long) g_frame);
        }
    }
    if (!xex)
        xhand = 0;
    if (st == 4) {              // ps44 R4: the exit was handed to the behind-the-body lens (s41b_tick, one tick later)
        if (g_s44x.off) {
            g_s43a.on = 0;
            MemoryBarrier();
            g_s44x.off = 0;
            st = 0;
        } else if (g_s44x.req < 0 || now - t_x4 > 120) {
            g_s44x.req = 0;
            g_s43a.len = S43A_OUT;
            g_s43a.t0 = GshNow();
            g_s43a.rdone = 0;
            MemoryBarrier();
            InterlockedIncrement(&g_s43a.gen);
            g_s43a.on = 2;
            st = 3;
            if (LOGCAP(nl, 300))
                Xlog("paircam: ps44 exit - the behind-the-body lens did not take the two-shot of %s (we are down / dead / it is switched off): ps43's ease of 500 ms onto the camera under it (frame %llu)", nm, (unsigned long long) g_frame);
        } else
            return;
    }
    if (st == 3) {
        if (g_s43a.rdone
            || GshNow() - g_s43a.t0 > (double) S43A_OUT + 0.3) {
            g_s43a.on = 0;
            st = 0;
        } else if (!g_s43q.h && !(xex && !xhand))
            return;             // (a new move during the way out: planned from here, the arc restarts from the shown pose)
    }
    if (st == 0 || st == 3 || (st == 2 && g_s43q.h)) {
        if (g_s43q.h && now - g_s43q.t < 300) {
            kind = 1;
            oh = g_s43q.h;
            until = g_s43q.t + (uint64_t) (g_s43q.dur * 1000.0f);
            snprintf(nm, sizeof nm, "%s", g_s43q.name);
            st = 1;
            t_req = now;
        } else if (xex && !xhand) {
            kind = 2;
            oh = g_xcam_h;
            until = now + 12000;
            snprintf(nm, sizeof nm, "%s", g_xcamName);
            st = 1;
            t_req = now;
            xhand = 1;
        }
        g_s43q.h = 0;
        if (st != 1)
            return;
    }
    if (st == 2 && kind == 1 && xex && !xhand) {
        kind = 2;
        oh = g_xcam_h;
        until = now + 12000;
        snprintf(nm, sizeof nm, "%s", g_xcamName);
        st = 1;
        t_req = now;
        xhand = 1;
    }                           // an execution right out of a takedown's hold
    float pb[16], ab[16];
    const float *me = PlayerLocator(pb), *ot =
        oh ? PuppetLocator(oh, ab) : NULL;
    int bad = !me || !ot || !XvFin3(me) || !XvFin3(ot) || g_downed
        || g_downDying || MatchSubstate() != 6
        || !g_ps43[kind == 2 ? PS43_EXEC : PS43_ATT];
    float aim = kind == 2 ? S43A_AIM_EXEC : S43A_AIM;
    if (st == 1) {
        if (rp44 && (bad || now - t_req > 400)) {
            rp44 = 0;
            g_s44BanOn = 0;
            S43Reset();
            st = 2;
            return;
        }                       // (a re-plan that fails keeps the shot)
        if (bad || now - t_req > 400) {
            S44UnderOff(now);
            S43Reset();
            st = g_s43a.on ? 2 : 0;
            if (LOGCAP(nl, 300))
                Xlog("paircam: ps43 %s - no two-shot (%s): the camera under it (frame %llu)", nm, bad ? "a body is not readable / we are down" : "the plan did not finish in 400 ms", (unsigned long long) g_frame);
            if (st == 2) {
                g_s43a.len = S43A_OUT;
                g_s43a.t0 = GshNow();
                g_s43a.rdone = 0;
                MemoryBarrier();
                InterlockedIncrement(&g_s43a.gen);
                g_s43a.on = 2;
                st = 3;
            }
            return;
        }
        if (g_s43.view != 2 || g_s43.t0 != t_req) {
            S43Reset();
            g_s43.view = 2;
            g_s43.t0 = t_req;
        }
        if (g_s43.st == 0) {
            if (!rp44 && (kind == 1 || g_ps44[PS44_EXEC]) && !PsyncOwn()
                && now - t_req < 150)
                return;         // (the pair's clips are not known here yet)
            // ps44 R2: an execution's plan samples its clips too (use_clip 2: S44_TX; the live roots when they cannot be read); a re-plan: the live roots
            S43Build(2, me, ot, oh, 0, NULL, NULL,
                      kind == 2 ? 2.00f : 1.60f, kind == 2 ? 2.80f : 2.40f,
                      0.0f, kind == 2 ? S43A_UP_EXEC : S43A_UP,
                      rp44 ? 0 : kind ==
                      1 ? 1 : g_ps44[PS44_EXEC] ? 2 : 0);
            return;
        }
        float cp[3], cf[3];
        int cok = RcamGet(cp, cf);
        if (g_s43.st == 1)
            S43Score(cok ? cp : NULL);
        if (rp44 && g_s43.st != 2) {
            rp44 = 0;
            g_s44BanOn = 0;
            S43Reset();
            st = 2;
            if (LOGCAP(nl, 300))
                Xlog("paircam: ps44 %s - the re-plan found no other clear spot: the shot stays (frame %llu)", nm, (unsigned long long) g_frame);
            return;
        }
        if (g_s43.st != 2) {
            S44UnderOff(now);
            st = g_s43a.on ? 2 : 0;
            if (st == 2) {
                g_s43a.len = S43A_OUT;
                g_s43a.t0 = GshNow();
                g_s43a.rdone = 0;
                MemoryBarrier();
                InterlockedIncrement(&g_s43a.gen);
                g_s43a.on = 2;
                st = 3;
            }
            return;
        }
        float mx = g_s43.M[0], mz = g_s43.M[2];
        Ls[0] = 0.5f * (me[0] + ot[0]);
        Ls[1] = 0.5f * (me[1] + ot[1]) + aim;
        Ls[2] = 0.5f * (me[2] + ot[2]);
        L0[0] = Ls[0];
        L0[1] = Ls[2];
        memcpy(E0, g_s43.E, sizeof E0);
        live = !g_s43.clip;
        // the way in: the short arc round the midpoint, or one cut
        float arc = 9.0f, nearb = 99.0f;
        if (cok) {
            float a0 = __builtin_atan2f(cp[0] - mx, cp[2] - mz), a1 =
                __builtin_atan2f(E0[0] - mx, E0[2] - mz), da =
                __builtin_remainderf(a1 - a0, 6.2831853f);
            float r0 =
                __builtin_sqrtf((cp[0] - mx) * (cp[0] - mx) +
                                (cp[2] - mz) * (cp[2] - mz)), r1 =
                __builtin_sqrtf((E0[0] - mx) * (E0[0] - mx) +
                                (E0[2] - mz) * (E0[2] - mz));
            arc = __builtin_fabsf(da);
            for (int i = 1; i <= 8; i++) {
                float u = (float) i / 8.0f, a = a0 + da * u, rr =
                    r0 + (r1 - r0) * u, px =
                    mx + __builtin_sinf(a) * rr, pz =
                    mz + __builtin_cosf(a) * rr;
                float d1 =
                    __builtin_sqrtf((px - me[0]) * (px - me[0]) +
                                    (pz - me[2]) * (pz - me[2])), d2 =
                    __builtin_sqrtf((px - ot[0]) * (px - ot[0]) +
                                    (pz - ot[2]) * (pz - ot[2]));
                if (d1 < nearb)
                    nearb = d1;
                if (d2 < nearb)
                    nearb = d2;
            }
        }
        int cut = !(arc <= S43A_CUT_ARC) || nearb < 0.60f || rp44;
        if (rp44 && LOGCAP(nl, 300))
            Xlog("paircam: ps44 %s - re-planned from the live roots: ONE cut to spot %d of %d, lens %.2f %.2f %.2f (%.0f deg round the midpoint from the spot that lost the victim; once per shot; frame %llu)", nm, g_s43.best, S43_NC, (double) E0[0], (double) E0[1], (double) E0[2], (double) (arc < 9.0f ? arc * 57.29578f : -1.0f), (unsigned long long) g_frame);
        for (int i = 0; i < 3; i++) {
            g_s43a.E[i] = E0[i];
            g_s43a.L[i] = Ls[i];
        }
        g_s43a.piv[0] = mx;
        g_s43a.piv[1] = g_s43.M[1] + 1.2f;
        g_s43a.piv[2] = mz;
        g_s43a.len = cut ? 0.0f : S43A_IN;
        g_s43a.t0 = GshNow();
        g_s43a.rdone = 0;
        MemoryBarrier();
        InterlockedIncrement(&g_s43a.gen);
        g_s43a.on = 1;
        st = 2;
        gen37 = g_s37Gen;
        hid_at = 0;
        g_s44BanOn = 0;
        if (rp44) {
            rp44 = 0;
            return;
        }                       // (a re-plan keeps the shot's clock, release state and end rules)
        t_on = now;
        rel_at = 0;
        rp_ok = 0;
        rp_done = 0;
        if (LOGCAP(nl, 300))
            Xlog("paircam: ps43 %s on the attacker's screen - the pair-frame two-shot: lens %.2f %.2f %.2f, %.2f m from the pair's midpoint, looking at it %.2f m over the roots; in by %s (%.0f deg round the midpoint from the shown lens, the arc's nearest pass to a body %.2f m); %s; out by one ease of %.0f ms (%s = the ps42 camera; frame %llu)", nm, (double) E0[0], (double) E0[1], (double) E0[2], (double) g_s43.D, (double) aim, cut ? "ONE CUT" : "an ease of 250 ms", (double) (arc < 9.0f ? arc * 57.29578f : -1.0f), (double) nearb, kind == 2 ? "held until the execution camera starts its way out" : "held to the move's end / 2.5 s after our release / our swing / 0.45 m of our own movement / steering", (double) (S43A_OUT * 1000.0f), kind == 2 ? "t2r-ps43-cam-exec-old.txt" : "t2r-ps43-cam-att-old.txt", (unsigned long long) g_frame);
        return;
    }
    if (st == 2) {
        const char *why = NULL;
        if (bad)
            why = "a body is not readable / we are down / switched off";
        else if (g_ps44[PS44_TP] ? g_s43e.tp_ms > t_on : gen37 !=
                 g_s37Gen)
            why = "our body was teleported";
        else {
            if (gen37 != g_s37Gen) {   // ps44 R1: a stage restart (the rig's own cut / side swap under the shot) is not a teleport
                gen37 = g_s37Gen;
                if (LOGCAP(nl, 300))
                    Xlog("paircam: ps44 %s - the camera stages under the two-shot restarted (a rig cut / side swap; our root moved under 3 m): not a teleport, the shot holds (%llu ms in; t2r-ps44-cam-tp-old.txt = ps43: the shot ends on it by a cut; frame %llu)", nm, (unsigned long long) (now - t_on), (unsigned long long) g_frame);
            }
            float k = 1.0f - __builtin_expf(-dt / 0.35f), tx =
                0.5f * (me[0] + ot[0]), ty =
                0.5f * (me[1] + ot[1]) + aim, tz = 0.5f * (me[2] + ot[2]);
            if (g_ps44[PS44_AIM]) {     // ps44 R9: the midpoint of our chest and the victim's chest / pelvis (live joints) - a victim that falls away from its root stays in the shot
                float p0[3], c0[3], h0[3], p1[3], c1[3], h1[3];
                static int nla;
                static uint64_t la;
                XvBody(SID_PLAYER, me, p0, c0, h0);
                XvBody(oh, ot, p1, c1, h1);
                if (XvFin3(c0) && XvFin3(c1) && XvFin3(p1)
                    && __builtin_fabsf(c0[1] - me[1]) < 2.2f
                    && __builtin_fabsf(c1[1] - ot[1]) < 2.2f
                    && __builtin_fabsf(p1[1] - ot[1]) < 2.2f
                    && (c0[0] - me[0]) * (c0[0] - me[0]) + (c0[2] -
                                                            me[2]) *
                    (c0[2] - me[2]) < 4.0f
                    && (c1[0] - ot[0]) * (c1[0] - ot[0]) + (c1[2] -
                                                            ot[2]) *
                    (c1[2] - ot[2]) < 6.25f) {
                    float vx = 0.5f * (c1[0] + p1[0]), vy =
                        0.5f * (c1[1] + p1[1]), vz =
                        0.5f * (c1[2] + p1[2]);
                    float jx = 0.5f * (c0[0] + vx), jy =
                        0.5f * (c0[1] + vy), jz =
                        0.5f * (c0[2] + vz), off =
                        __builtin_sqrtf((jx - tx) * (jx - tx) +
                                        (jy - ty) * (jy - ty) + (jz -
                                                                 tz) *
                                        (jz - tz));
                    if (off > 0.35f && now - la > 1500 && LOGCAP(nla, 120)) {
                        la = now;
                        Xlog("paircam: ps44 %s - the two-shot's aim is the midpoint of our chest and the victim's chest / pelvis, %.2f m from the roots' midpoint (the victim's body %.2f m from its root; t2r-ps44-cam-aim-old.txt = the roots' midpoint; frame %llu)", nm, (double) off, (double) __builtin_sqrtf((vx - ot[0]) * (vx - ot[0]) + (vz - ot[2]) * (vz - ot[2])), (unsigned long long) g_frame);
                    }
                    if (!live) {
                        tx = jx;
                        tz = jz;
                    }           // (a live-root plan's lens rides the roots' midpoint: its aim keeps that frame horizontally)
                    ty = jy;
                }
            }
            if (g_ps44[PS44_EXEC] && !rp_done && now - t_on > 400) {    // ps44 R2: the victim's head + chest behind our own body for S44_HID_MS - one re-plan, one cut
                float E[3] = { g_s43a.E[0], g_s43a.E[1], g_s43a.E[2] };
                if (!S44Hidden(E, SID_PLAYER, me, oh, ot))
                    hid_at = 0;
                else if (!hid_at)
                    hid_at = now;
                else if (now - hid_at >= S44_HID_MS) {
                    float bx = E[0] - 0.5f * (me[0] + ot[0]), bz =
                        E[2] - 0.5f * (me[2] + ot[2]), bl =
                        __builtin_sqrtf(bx * bx + bz * bz);
                    rp_done = 1;
                    hid_at = 0;
                    if (bl > 0.3f) {
                        g_s44Ban[0] = bx / bl;
                        g_s44Ban[1] = bz / bl;
                        g_s44BanOn = 1;
                        rp44 = 1;
                        st = 1;
                        t_req = now;
                        S43Reset();
                        g_s43.view = 2;
                        g_s43.t0 = t_req;
                        if (LOGCAP(nl, 300))
                            Xlog("paircam: ps44 %s - the victim's head + chest have been hidden behind our own body for %d ms from the two-shot's lens %.2f %.2f %.2f (%llu ms in): ONE re-plan from the live roots, the spots within 50 deg of this one are out (t2r-ps44-cam-exec-old.txt = no re-plan; frame %llu)", nm, S44_HID_MS, (double) E[0], (double) E[1], (double) E[2], (unsigned long long) (now - t_on), (unsigned long long) g_frame);
                        return;
                    }
                }
            }
            Ls[0] += (tx - Ls[0]) * k;
            Ls[1] += (ty - Ls[1]) * k;
            Ls[2] += (tz - Ls[2]) * k;
            for (int i = 0; i < 3; i++)
                g_s43a.L[i] = Ls[i];
            if (live) {
                g_s43a.E[0] = E0[0] + Ls[0] - L0[0];
                g_s43a.E[2] = E0[2] + Ls[2] - L0[1];
                g_s43a.piv[0] = g_s43.M[0] + Ls[0] - L0[0];
                g_s43a.piv[2] = g_s43.M[2] + Ls[2] - L0[1];
            }
            if (kind == 2) {
                if (!xex)
                    why =
                        "the execution camera starts its way out / ended";
                else if (now >= until)
                    why = "the 12 s limit";
            } else {
                if (g_cdSfx.att_rel && !rel_at && now - t_on > 200)
                    rel_at = now;
                float ey =
                    g_s41b.e_ok ? __builtin_atan2f(g_s41b.ef[0],
                                                   g_s41b.ef[1]) : 0.0f;
                if (rel_at && !rp_ok && now - rel_at >= S43A_GRACE_MS) {
                    rp_ok = 1;
                    rp[0] = me[0];
                    rp[1] = me[2];
                    ry = ey;
                }
                if (now >= until)
                    why = "the move is over (the victim half's end)";
                else if (rel_at && now - rel_at > S43A_HOLD_MS)
                    why = "2.5 s after our release";
                else if (rel_at && MduSwinging())
                    why = "a new swing of ours";
                else if (now - t_on > 300 && !rel_at && !PsyncOwn())
                    why = "the paired move ended early";
                else if (rp_ok
                         && (me[0] - rp[0]) * (me[0] - rp[0]) + (me[2] -
                                                                 rp[1]) *
                         (me[2] - rp[1]) > S42_HOLD_M * S42_HOLD_M)
                    why = "we walked 0.45 m after our release";
                else if (rp_ok && g_s41b.e_ok
                         &&
                         __builtin_fabsf(__builtin_remainderf
                                         (ey - ry, 6.2831853f)) > 0.349f)
                    why =
                        "the camera is steered (the engine heading turned 20 deg after our release)";
            }
        }
        if (why && g_ps44[PS44_EXIT] && !bad && !(g_ps44[PS44_TP] ? g_s43e.tp_ms > t_on : gen37 != g_s37Gen)) {        // ps44 R4: straight to the behind-the-body lens
            snprintf(g_s44x.name, sizeof g_s44x.name, "%s", nm);
            g_s44x.oh = oh;
            g_s44x.off = 0;
            MemoryBarrier();
            g_s44x.req = 1;
            st = 4;
            t_x4 = now;
            if (g_s32.on && g_s32.ph == oh)
                g_s32.on = 0;   // (the rig push of this move ends with its shot: the behind lens does not start under a running push)
            if (LOGCAP(nl, 300))
                Xlog("paircam: ps44 %s - the two-shot ends after %llu ms (%s; our release %s): it holds one more tick, then the behind-the-body lens takes over from it (frame %llu)", nm, (unsigned long long) (now - t_on), why, rel_at ? "came during it" : "not seen", (unsigned long long) g_frame);
            return;
        }
        if (why) {
            int cut =
                g_ps44[PS44_TP] ? g_s43e.tp_ms > t_on : gen37 != g_s37Gen;
            g_s43a.len = cut ? 0.0f : S43A_OUT;
            g_s43a.t0 = GshNow();
            g_s43a.rdone = 0;
            MemoryBarrier();
            InterlockedIncrement(&g_s43a.gen);
            g_s43a.on = 2;
            st = 3;
            if (LOGCAP(nl, 300))
                Xlog("paircam: ps43 %s - the two-shot ends after %llu ms (%s; our release %s): %s onto the camera under it (frame %llu)", nm, (unsigned long long) (now - t_on), why, rel_at ? "came during it" : "not seen", cut ? "a cut" : "one ease of 500 ms", (unsigned long long) g_frame);
        }
    }
}

// ps45 S1 (the note at PS45_SIDE): main thread. A rig side swap that would be ONE cut asks for a path first; anything else is the old request.
static int S45sArc(int v, const float *cp, float px, float pz, float a0,
                    float da, float R0, float R1, float ox, float oz)
{                               // kicks g_s45s.job[v]; 1 = kept
    float Q0[3] = { cp[0], cp[1], cp[2] }, nb = 99.0f;
    for (int i = 1; i <= 2 * S45S_NR; i++) {    // (the body test on twice the legs)
        float u = (float) i / (float) (2 * S45S_NR), a = a0 + da * u, r =
            R0 + (R1 - R0) * u, qx = px + __builtin_sinf(a) * r, qz =
            pz + __builtin_cosf(a) * r;
        float d =
            __builtin_sqrtf((qx - ox) * (qx - ox) + (qz - oz) * (qz - oz));
        if (d < nb)
            nb = d;
    }
    g_s45s.da[v] = da;
    g_s45s.nb[v] = nb;
    g_s45s.ok[v] = nb >= S45S_BODY;
    for (int i = 0; i < S45S_NR; i++)
        g_s45s.job[v][i] = 0;
    if (!g_s45s.ok[v])
        return 0;
    for (int i = 1; i <= S45S_NR; i++) {
        float u = (float) i / (float) S45S_NR, a = a0 + da * u, r =
            R0 + (R1 - R0) * u;
        float Q1[3] =
            { px + __builtin_sinf(a) * r, cp[1],
      pz + __builtin_cosf(a) * r };
        g_s45s.job[v][i - 1] = WorldRayKickM(Q0, Q1, g_spmPh, 0x1b07);
        memcpy(Q0, Q1, sizeof Q0);
    }
    return 1;
}

static void S45sFlip(LONG kind, uint64_t now)
{
    int cut = kind == 2 || (g_s42Side && g_s32.p > 0.3f);      // (spm25_rig's cut42)
    if (!g_ps45[PS45_SIDE] || !cut) {
        InterlockedExchange(&g_s39_flip, kind);
        return;
    }
    if (g_s45s.st == 1)
        return;                 // (one already asked: its answer comes next tick)
    float cp[3], cf[3];
    const char *why = NULL;
    float px = g_spm.px, pz = g_spm.pz, ox = g_spm.ex, oz = g_spm.ez, lx =
        ox - px, lz = oz - pz, ll = __builtin_sqrtf(lx * lx + lz * lz);
    if (!g_spm.ok)
        why = "the duel framing is off";
    else if (!RcamGet(cp, cf))
        why = "the shown camera is not readable";
    else if (!(ll > 0.3f && ll < 12.0f))
        why = "the pair's line is not readable";
    if (why) {
        InterlockedExchange(&g_s39_flip, kind);
        static int nl;
        if (LOGCAP(nl, 80))
            Xlog("spmelee cam: ps45 sideease - the rig side swap (%s) is ONE CUT: %s (frame %llu)", kind == 2 ? "head floor" : "side-on shot", why, (unsigned long long) g_frame);
        return;
    }
    float ldx = lx / ll, ldz = lz / ll, side =
        g_s25.side >= 0.0f ? 1.0f : -1.0f;
    float az40 = SPM25_AZ + ((g_ps40[PS40_SIDE] && g_s32.ctx) || g_s42Side ? (S40_AZ_CTX - SPM25_AZ) * g_s32.p : 0.0f);        // (spm25_rig's spot)
    float tg = __builtin_atan2f(-ldx, -ldz) - side * az40, a0 =
        __builtin_atan2f(cp[0] - px, cp[2] - pz);
    float R0 =
        __builtin_sqrtf((cp[0] - px) * (cp[0] - px) +
                        (cp[2] - pz) * (cp[2] - pz)), R1 = g_spm.bt > 0.5f
        && g_spm.bt < 2.0f ? g_spm.bt : SPM25_DIST;
    if (R1 < R0)
        R1 = R0;
    float da0 = __builtin_remainderf(tg - a0, 6.2831853f), da1 =
        da0 > 0.0f ? da0 - 6.2831853f : da0 + 6.2831853f;
    g_s45s.kind = kind;
    g_s45s.t0 = now;
    g_s45s.R0 = R0;
    S45sArc(0, cp, px, pz, a0, da0, R0, R1, ox, oz);
    if (__builtin_fabsf(da1) <= S45M_MAXARC)
        S45sArc(1, cp, px, pz, a0, da1, R0, R1, ox, oz);
    else {
        g_s45s.da[1] = da1;
        g_s45s.nb[1] = 0.0f;
        g_s45s.ok[1] = 0;
        for (int i = 0; i < S45S_NR; i++)
            g_s45s.job[1][i] = 0;
    }
    g_s45s.st = 1;
}

static void S45sTick(uint64_t now)
{                               // spm_tick: the path answers, one tick after s45s_flip
    if (g_s45s.st != 1 || now == g_s45s.t0)
        return;
    int bl[2] = { -1, -1 }, pick = -1;
    for (int v = 0; v < 2; v++)
        for (int i = 0; i < S45S_NR; i++) {
            uint64_t *j = &g_s45s.job[v][i];
            if (*j) {
                g_rayErr = 0;
                float t = RayT(*j);
                *j = 0;
                if (bl[v] < 0 && (g_rayErr || (t >= 0.0f && t < 0.97f)))
                    bl[v] = i;
            }
        }
    g_s45s.st = 0;
    for (int v = 0; v < 2 && pick < 0; v++)
        if (g_s45s.ok[v] && bl[v] < 0)
            pick = v;
    const char *nm = g_s45s.kind == 2 ? "head floor" : "side-on shot";
    static int nl;
    if (pick >= 0) {
        float da = g_s45s.da[pick], x =
            __builtin_fabsf(da) / 3.14159265f, T =
            S45S_T0 + (S45S_T1 - S45S_T0) * (x > 1.0f ? 1.0f : x);
        g_s45sDsg = da >= 0.0f ? 1 : -1;
        g_s45sT = T;
        MemoryBarrier();
        InterlockedExchange(&g_s39_flip, 3);
        if (LOGCAP(nl, 120))
            Xlog("spmelee cam: ps45 sideease - the rig side swap (%s) is an EASED SWING, not a cut: %s, %.0f deg round our pivot over %.0f ms (smoothstep), path clear (legs %d at the lens height, the partner's root %.2f m off at the closest; the short way %s; t2r-ps45-cam-sideease-old.txt = the ONE cut; frame %llu)", nm, pick ? "the other way round" : "the short way", (double) (__builtin_fabsf(da) * 57.29578f), (double) (T * 1000.0f), S45S_NR, (double) g_s45s.nb[pick], pick ? (g_s45s.ok[0] ? "blocked by geometry" : "passes near the partner") : "taken", (unsigned long long) g_frame);
    } else {
        InterlockedExchange(&g_s39_flip, g_s45s.kind);
        if (LOGCAP(nl, 120))
            Xlog("spmelee cam: ps45 sideease - the rig side swap (%s) is ONE CUT (fallback): every path is blocked - the short way %.0f deg: %s, the other way %.0f deg: %s (frame %llu)", nm, (double) (__builtin_fabsf(g_s45s.da[0]) * 57.29578f), g_s45s.ok[0] ? (bl[0] >= 0 ? "geometry on its path" : "?") : "passes near the partner", (double) (__builtin_fabsf(g_s45s.da[1]) * 57.29578f), __builtin_fabsf(g_s45s.da[1]) > S45M_MAXARC ? "longer than 200 deg" : g_s45s.ok[1] ? (bl[1] >= 0 ? "geometry on its path" : "?") : "passes near the partner", (unsigned long long) g_frame);
    }
}

static int S45sSwing(void)
{
    double n = GshNow();
    return (g_ps45[PS45_SIDE] && n < g_s45sUntil) || (g_ps46
                                                       && n < g_s46Until);
}                               // melee_camcont.c: ccg / cam2 pass the swing through (ps46: the engage ease too)

static void SpmTick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    static uint64_t t_ms;
    static float mx, mz;
    static int init, logged, was_on;
    static float ps23_wv, piv_x, piv_z, pin_x, pin_z;
    static int piv_ok, pin_ok;
    static uint64_t pin_h;      // ps23: the weight spring's speed; the slewed pivots (ours / the partner's)
    {
        static int rd;
        if (!rd) {
            rd = 1;
            g_ps23Piv =
                GetFileAttributesA("t2r-ps23-cam-pivot-old.txt") ==
                INVALID_FILE_ATTRIBUTES;
        }
    }
    uint64_t now = GetTickCount64();
    float dt = t_ms ? (float) (now - t_ms) / 1000.0f : 0.0f;
    t_ms = now;
    if (dt > 0.1f)
        dt = 0.1f;
    Ps25Cfg(now);              // meleefov.c: the ps25 opt-out files
    Ps32Cfg(now);
    Ps39Cfg(now);
    Ps40Cfg(now);
    Ps42Cfg(now);
    S41bTick(now, dt);         // ps41 H: the behind-the-body hold after a move camera's hand-back
    Ps43Cfg(now);
    Ps44Cfg(now);
    Ps45Cfg(now);              // ps45 (the note at PS45_*; t2r-ext.c S45X_*)
    Ps46Cfg(now);              // ps46 E1: the engage ease (the note at S46_*)
    S45sTick(now);             // ps45 S1: a side swing's path answers
    S43qTrack(now);            // ps43 Q1: the engage cut's planned spot (opt-out, teleport watch, proof line)
    S43AttTick(now, dt);      // ps43: the attacker's pair-frame two-shot (finisher / takedown / execution)
    {                           // ps41 G1 (t2r-ext.c xcam_filter, melee_camcont.c s41_roll): the execution camera's entry cut + roll clamp
        static uint64_t at41;
        static LONG lc, lr;
        if (!at41 || now - at41 > 2000) {
            at41 = now;
            int v =
                GetFileAttributesA("t2r-ps41-cam-xcut-old.txt") ==
                INVALID_FILE_ATTRIBUTES;
            static int fl = 1;
            if (v != g_ps41x || fl)
                Xlog("xcam: ps41 t2r-ps41-cam-xcut-old.txt %s",
                     v ?
                     "absent - execution cameras: ONE cut instead of a blend-in swing > 100 deg, roll clamped to +-10 deg, the shiv victim's shot side chosen at the start: ON"
                     : "present - the ps40 behaviour");
            g_ps41x = v;
            fl = 0;
        }
        g_s41x_exec = S40ExecCam();
        LONG c = g_s41xCuts;
        if (c != lc) {
            lc = c;
            Xlog("xcam: ps41 - execution camera %s taken on ONE cut: its 450 ms blend-in would have swung %.0f deg round our head (> 100; cut #%ld, frame %llu)", g_xcamName, (double) g_s41xDeg, (long) c, (unsigned long long) g_frame);
        }
        LONG r = g_s41r.n;
        static uint64_t rt;
        if (r != lr && now - rt > 1000) {
            lr = r;
            rt = now;
            Xlog("xcam: ps41 - execution camera %s roll clamped to +-10 deg on %ld frame(s) so far (authored / shaken roll up to %.0f deg; frame %llu)", g_xcamName, (long) r, (double) g_s41r.mx, (unsigned long long) g_frame);
        }
    }
    {
        static int exw;
        int ex = S40ExecCam();
        static int nle;         // ps40 E5 (melee_camcont.c ccg_filter / cam2_filter)
        if (ex && !exw && LOGCAP(nle, 60))
            Xlog("camcont: ps40 - execution camera %s: the continuity stages pass it through (rec39a A 12569-12578: in duel mode they absorbed its blend-in, 'OUR FILTER rotation jump ... residual 33.9 deg', then showed the rest as jumps) (frame %llu)", g_xcamName, (unsigned long long) g_frame);
        exw = ex;
    }
    {
        static LONG c39;
        LONG c = g_s39_cuts;
        static int nl39;        // ps39 C7: the rig's cuts (render thread, spm25_rig)
        if (c != c39) {
            c39 = c;
            g_spm.bmax = 0.0f;
            g_spm_rbm = 0.0f;
            g_s40NofadeUntil = now + 1000;    // (ps40: the allowed lens length was measured along the old line; E7: the engine's near-camera hide of the puppets is judged from ITS camera, which may still stand in the partner - not for the next second)
            if (LOGCAP(nl39, 120))
                Xlog("spmelee cam: ps39 rig cut - the rig's lens stood more than %.0f deg off its spot (on the partner's side of us: a latch with the partner beside / behind the lens, a body that passed us): its spot is taken on ONE CUT, the look-at and the continuity stages restart on it (cut %ld, frame %llu)", (double) (S39_FAR_RAD * 57.29578f), (long) c, (unsigned long long) g_frame);
        }
    }
    {
        static uint64_t at37;
        static int f37 = 1;     // ps37 opt-out
        if (f37 || now - at37 >= 2000) {
            at37 = now;
            int v =
                GetFileAttributesA("t2r-ps37-cam-reset-old.txt") ==
                INVALID_FILE_ATTRIBUTES;
            if (f37 || v != g_ps37)
                Xlog("spmelee cam: ps37 t2r-ps37-cam-reset-old.txt %s",
                     v ?
                     "absent - teleport reset + no flip correction with our head behind the lens: ON"
                     : "present - the OLD (ps36) behaviour");
            g_ps37 = v;
            f37 = 0;
        }
    }
    if (g_s32FbReq) {         // ps35: the attacker's authored takedown camera found every shot blocked (xcam_choose_tick) - the rig takes the rest of the move
        g_s32FbReq = 0;
        // ps38 (rec37a A 14654 'exec cam: ctxdown:pe-fist-wall-abby role 1: 2013 AUTHORED camera ...' = the pin ESCAPE pair's own camera, started by
        // melee_duel_pin.c - not by ps35's contextual rule; 14656 'best authored camera 1 blocked on 48 of ~45 rays - not kept, procedural
        // fallback', 14798 'ps35 takedown camera check - ctxdown:pe-fist-wall-abby ...: both heads in frame on 20 % of 144 ticks ..., the lens came within
        // 0.13 m of a body axis'): the ps35 fallback only knew takedowns that came through cd_cam_tracks (its note was from another move, 'left' negative),
        // so the escape kept the procedural fallback camera inside the bodies. Now any attacker-side 'ctxdown:' camera whose every authored shot is blocked
        // is stopped before it shows: a pinned victim's over-the-shoulder shot is held for the rest of the pair (the escape plays in it), else the duel rig
        // takes it, else the game camera.
        if (g_xcamOn && g_xcam_role == 1 && g_xcam_w < 0.5f) {
            float el = (float) (g_t60 - g_xcam_t0) / 60.0f, left =
                g_xcam_kill - el, off35 = g_xcamTrkOff + el;
            uint64_t oh = g_xcam_h;
            char nm38[64];
            snprintf(nm38, sizeof nm38, "%s", g_xcamName);
            if (left > 0.4f) {
                XcamHardStop("ps35: every authored shot is blocked");
                if (g_s34Tb && g_pc5.on && g_pc5.role == 1
                    && g_pc5.slot >= 0
                    && PuppetHandle((uint32_t) g_pc5.slot) == oh) {
                    g_s34.keep = now + (uint64_t) (left * 1000.0f);
                    static int nl38;
                    if (LOGCAP(nl38, 80))
                        Xlog("spmelee cam: ps38 %s - its authored camera is blocked by geometry on every candidate: the pin's over-the-shoulder shot is held for the pair's %.0f ms (frame %llu)", nm38, (double) (left * 1000.0f), (unsigned long long) g_frame);
                } else
                    if (S32FinForce
                        (oh, left, nm38,
                         "its authored camera is blocked by geometry on every candidate"))
                    S32FinKicks(off35);
            }
        }
    }
    int fin = S32FinTick(now);        // ps32: the attacker's finisher push runs (the rig keeps the camera through the paired move)
    {
        int xa = g_xcamOn && (g_xcam_role == 1 || g_xcamPesc) && !strncmp(g_xcamName, "ctxdown:", 8);        // ps35: the proof line of the attacker's takedown camera (ps38: + the pinner's escape camera, + the live guard)
        S35TkCheck(xa
                     || g_s32.on, xa ? g_xcam_h : g_s32.ph,
                     xa ? g_xcamName : g_s35n.name,
                     !xa ? "the duel rig, pushed in" : g_xcam_role ==
                     1 ? "the clip's authored camera" :
                     "the pinner's escape camera", xa);
    }
    SpcTick(dt);
    // v5-cam7 (after the pin-escape counter the camera swung toward the wall, both bodies left the frame, then it swung back;
    // A log 39406 'cam2: takedown/finisher/exec camera -> duel framing (would step 58.4 deg / 1.87 m) - blended over 700 ms'): the takedown camera's
    // blend-out went to the RAW game camera (its heading never followed the pair) and the framing came back only after it - two swings. In a duel the
    // framing runs again under the blend-out / hand-back (icam_filter hands it to xcam_filter as the way-out target), so the release lands on it.
    // Not for the down camera (role 3) or while we are down. t2r-cam7-no-xout.txt (re-read every 2 s) = the old order.
    {
        static int v = -1;
        static uint64_t at;
        uint64_t n2 = GetTickCount64();
        if (v < 0 || n2 - at >= 2000) {
            at = n2;
            FILE *f = ExeSideOpen("t2r-cam7-no-xout.txt", "r");
            v = f != NULL;
            if (f)
                fclose(f);
        }
        int xo = !v && g_cam2On && g_xcamOn && g_xcam_out
            && g_xcam_role != 3 && !g_downed && !g_downDying
            && CcgDuelCtx();
        if (xo != g_spmXout) {
            static int nl;
            if (xo && nl++ < 40)
                Xlog("spmelee cam: framing runs under the %s blend-out (duel) - the way out lands on the duel framing", g_xcam_role == 1 ? "attacker's" : "takedown camera's");
            g_spmXout = xo;
        }
    }
    int hard = MatchSubstate() != 6 || SpmOff() || (g_xcamOn
                                                      && !g_spmXout)
        || g_fcam_on || g_icam_on || g_downed || g_downDying
        || g_uc4DyingUntil || (PsyncActive() && !SpmInExchange()
                                 && !(g_cam2On && MduEngaged()) && !fin)
        || (!fin && MfkBusy()) || MfOtherOwner();
    // a clip showed very weird camera behaviour after aiming after leaving a pin: the duel look-at turned the over-shoulder AIM camera, whose forward
    // test flips when the camera sits beside the player - B's view spun round several times in a second. Aiming is gunplay: the framing fades out (soft path).
    if (PadCmdOn("aim"))
        hard = 1;
    static float ho_w, ho_yw;
    static int ho;
    if (hard && g_xcamOn && g_spm.ok && !g_downed && !g_downDying
        && MatchSubstate() == 6 && (ho || g_spm.w > 0.01f
                                     || g_spm.yw > 0.01f)) {
        // (the finisher looked "instant": the duel framing switched off on the finisher camera's first frame - a cut to the raw game camera, then the
        // takedown camera's 0.45 s blend from there). The framing now hands over: frozen on its last target, its weights fall with the takedown camera's
        // blend-in (icam_filter blends the takedown camera from this framed view).
        if (!ho) {
            ho = 1;
            ho_w = g_spm.w;
            ho_yw = g_spm.yw;
            static int nl;
            if (nl++ < 40)
                Xlog("spmelee cam: hand-over to the takedown camera (framing %.2f, look-at %.2f fade with its blend-in)", (double) ho_w, (double) ho_yw);
        }
        float k = 1.0f - g_xcam_w;
        k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
        if (k > 0.01f) {
            g_spm.w = ho_w * k;
            g_spm.yw = ho_yw * k;
            g_spmHo = 1;
            return;
        }
    }
    ho = 0;
    g_spmHo = 0;
    int soft = hard && MatchSubstate() == 6 && !SpmOff() && !g_xcamOn
        && !g_fcam_on && !g_icam_on && !g_downed && !g_downDying
        && !g_uc4DyingUntil && g_spm.ok;
    if (soft && (g_spm.w > 0.01f || g_spm.yw > 0.01f)) {        // (dodge counter: a synced pair outside the duel switched the framing off in one frame - a hard cut; it fades now, frozen on its last target)
        float f = __builtin_expf(-dt / 0.09f);
        g_spm.w *= f;
        g_spm.yw *= f;
        init = 0;
        was_on = 0;
        return;
    }
    if (hard) {
        g_spm.w = 0;
        g_spm.ok = 0;
        g_spm.yw = 0;
        init = 0;
        was_on = 0;
        g_spm.bmax = 0.0f;
        g_spm_rbm = 0.0f;
        if (MatchSubstate() != 6 || SpmOff())
            g_s25Lock = 1;
        if (g_spmRjob) {
            RayT(g_spmRjob);
            g_spmRjob = 0;
        }
        if (g_spm_rjob2) {
            RayT(g_spm_rjob2);
            g_spm_rjob2 = 0;
        }
        return;
    }
    int win = (g_mf_melee_ms && now - g_mf_melee_ms < 450) || (g_spm_ev_ms && now - g_spm_ev_ms < 1200) || MduEngaged() || fin;        // (A/B: a pinned victim between blows lost the framing - its camera looked away from the pinner; the duel holds it)
    float px = 0, pz = 0, ex = 0, ez = 0;
    int have = 0, pok = 0, tp37 = 0;
    // (ps25 release: our root is read while the framing fades out too - its pivot follows us to the end; the partner search needs the window as before)
    float pb[16];
    const float *pl = (win
                       || (g_ps25[PS25_RELEASE]
                           && g_spm.ok)) ? PlayerLocator(pb) : NULL;
    if (pl && __builtin_isfinite(pl[0]) && __builtin_isfinite(pl[2])) {
        px = pl[0];
        pz = pl[2];
        pok = 1;
        {
            static float tpx, tpz;
            static int tp_ok;
            static uint64_t tp_at;      // ps37: an own-body teleport / respawn (cam2_filter's note in melee_camcont.c)
            float tdx = px - tpx, tdz = pz - tpz, td =
                __builtin_sqrtf(tdx * tdx + tdz * tdz);
            if (g_ps46 && g_ps37 && tp_ok && td > 3.0f && now - tp_at > S46_TP_FRESH) { // ps46 E1: the last sample is from an earlier melee window - not a teleport
                g_s46.tps++;
                g_s46.tpd = td;
                g_s46.tpg = now - tp_at;
                tp_ok = 0;
                static int nl46;
                if (LOGCAP(nl46, 80))
                    Xlog("spmelee cam: ps46 ps37 watch - our root %.1f m off a sample %llu ms old (an earlier melee window): NOT a teleport, the sample is re-taken (no reset; s43q_track's every-tick watch flags a real one; %ld so far, frame %llu)", (double) td, (unsigned long long) g_s46.tpg, (long) g_s46.tps, (unsigned long long) g_frame);
            }
            if (g_ps37 && tp_ok && td > 3.0f) {
                InterlockedIncrement(&g_s37Gen);
                tp37 = 1;
                g_spm.w = 0;
                g_spm.yw = 0;
                g_spm.ok = 0;
                init = 0;
                was_on = 0;
                g_spm.bmax = 0.0f;
                g_spm_rbm = 0.0f;
                g_s32.hok = 0;
                g_s32.zl = 0.0f;
                static int nl;
                if (LOGCAP(nl, 120))
                    Xlog("spmelee cam: ps37 reset - our root moved %.1f m in one tick (teleport / respawn): the duel camera's kept state (rig side + azimuth, look-at heading, flip corrections, head filters, latch) is dropped and re-acquired from the live camera (frame %llu)", (double) td, (unsigned long long) g_frame);
            }
            tpx = px;
            tpz = pz;
            tp_ok = 1;
            tp_at = now;
        }
        uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
        int dsl = g_cam2On && MduEngaged()? MduPartnerSlot() : -1;  // v5-cam2: in a duel the framing is on the PARTNER (the nearest enemy could be a third player)
        float bd = (dsl >= 0 || fin) ? 6.0f * 6.0f : 4.5f * 4.5f;
        for (uint32_t sl = 0; win && sl < T2R_MAX_PEERS; sl++) {
            int fsl = fin && sl != self && PuppetHandle(sl) == g_s32.ph;       // ps32: the finisher's victim stays the framed partner through its down / death (rec21 A 14252: the duel ends 2 frames into the finisher, 14341 'downed: slot 1')
            if (fin ? !fsl
                : (sl == self || IsTeammate(sl) || g_peerDowned[sl]
                   || g_puppetDeadHere[sl] || (dsl >= 0
                                                 && (int) sl != dsl)))
                continue;
            uint8_t *peer = ((fn_peer) T2rPeer) (sl);
            if (!peer || ((!peer[4] || peer[5]) && !fsl))
                continue;
            uint64_t h = PuppetHandle(sl);
            float ab[16];
            const float *al = h ? PuppetLocator(h, ab) : NULL;
            if (!al || !__builtin_isfinite(al[0])
                || !__builtin_isfinite(al[2]))
                continue;
            float dx = al[0] - px, dz = al[2] - pz, d2 =
                dx * dx + dz * dz, dy = al[1] - pl[1];
            if (d2 > bd || dy < -1.5f || dy > 1.5f)
                continue;
            bd = d2;
            ex = al[0];
            ez = al[2];
            g_spmEy = al[1];
            have = 1;
            g_spmPh = h;
        }
    }
    // ps23: the pivots the render thread frames with (fpx / fpz ours, fex / fez the partner's) follow the raw roots (spm_slew above); the rays below keep the raw ones
    float fpx = px, fpz = pz, fex = ex, fez = ez;
    if (have && g_ps23Piv) {
        if (!init) {
            piv_ok = 0;
            pin_ok = 0;
        }
        if (pin_h != g_spmPh) {
            pin_ok = 0;
            pin_h = g_spmPh;
        }
        float j0 = SpmSlew(&piv_x, &piv_z, px, pz, dt, &piv_ok), j1 =
            SpmSlew(&pin_x, &pin_z, ex, ez, dt, &pin_ok);
        fpx = piv_x;
        fpz = piv_z;
        fex = pin_x;
        fez = pin_z;
        static int nlp;
        if ((j0 > 0.25f || j1 > 0.25f) && LOGCAP(nlp, 120))
            Xlog("spmelee cam: ps23 pivot - %s stepped %.2f m in one tick; the framing pivot follows at <= %.0f m/s (trails <= %.2f m), no lens step (frame %llu)", j0 >= j1 ? "our root" : "the partner's root", (double) (j0 >= j1 ? j0 : j1), (double) SPM_PIV_MPS, (double) SPM_PIV_LAG, (unsigned long long) g_frame);
    }
    float tgt = have && !MpnPinnedNow()? 1.0f : 0.0f; // (A/B: pinned to a van, the pull-in put the victim's camera inside the van - the engine camera's own collision keeps it out; the look-at stays)
    if (SpmPs23Engage() && tgt > g_spm.w) {   // ps23 (meleefov.c, SPM_IN_OMEGA): the weight comes in on a critically damped spring at SP's engage times, not on a tau-70 ms exponential
        if (g_spm.w < 0.002f)
            ps23_wv = 0.0f;
        int wn = (int) (dt * 240.0f) + 1;
        float wh = dt / (float) wn, wx = g_spm.w, wv = ps23_wv;
        for (int i = 0; i < wn; i++) {
            wv +=
                (SPM_IN_OMEGA * SPM_IN_OMEGA * (tgt - wx) -
                 2.0f * SPM_IN_OMEGA * wv) * wh;
            wx += wv * wh;
        }
        if (wv < 0.0f)
            wv = 0.0f;
        if (wx > tgt) {
            wx = tgt;
            wv = 0.0f;
        }
        g_spm.w = wx;
        ps23_wv = wv;
    } else {
        ps23_wv = 0.0f;
        g_spm.w += (tgt - g_spm.w) * (1.0f - __builtin_expf(-dt / (tgt > g_spm.w ? 0.07f : g_ps25[PS25_RELEASE] ? SPM25_OUT_TAU : 0.08f)));     // (ps25 release, below)
    }
    // ps25 (the spec block at the top): OUR melee activity = SP's is-current-camera-melee. A duel swing of ours / our attacker half of a pair,
    // SPM25_ACT_MS after its last tick; the engine's own attack state extends that swing through its follow-through (never starts it: a hit reaction
    // on us must not count). It drives (a) the rig latch: the rig + look-at + engine lock frame the partner only for a fighter who has swung in this
    // duel, and let go SPM25_RIG_KEEP_MS after his last swing; until then his camera is his own - uc2 B 39576 'cam2: victim engage by slot 0 ... the duel
    // camera turns to it now (fast look-at + orbit)', camrec 39579 'cyaw -0.8' -> 39642 'cyaw -107.4' (vistrace world yaw +52 -> -52 deg) on a player who
    // never pressed anything, then 'lat +0.7..+1.1 back -0.18..+0.9' beside its own body; SP: hit outside the melee camera the camera turns 0.10 deg/frame
    // (p90 0.59). t2r-ps25-cam-victim-old.txt = the earlier victim engage. (b) the fov (meleefov.c g_ps25_fw). (c) the lens height (g_s25.up).
    static uint64_t s25_own_ms;
    static int s25_rig, s25_free_was;
    if (tp37) {
        s25_rig = 0;
        s25_own_ms = 0;
    }                           // ps37: the latch does not survive a teleport
    int s25_duel = MduEngaged() || fin;        // (ps32: the finisher the rig keeps is duel + our own move to its end)
    if (MduSwinging() || (MduInPair() && !MduVicPairOn()) || fin)
        s25_own_ms = now;
    // ps27 (the camera turns to the attacker once you are hit, before you have swung; ps26 bot run: 14 of 15
    // out-of-frame samples were a player who had not swung yet): a blow that LANDS on us in this duel (its -hit pair on us, or a plain blow's reaction) engages
    // the rig like our own swing does. A swing that is only coming does not (SP: the camera stays the player's). t2r-ps27-cam-hit-latch-old.txt = own swing only.
    // ps40 E6 (rec39a B 10675 'ps25 victim - in a duel without a swing of ours: the camera keeps the player's heading', clip B 1.2-6.57: 5.4 s
    // through two dodges and the first exchange with the opponent off-screen; A 18245 'ps27 hit latch' only at the blow). The rule (not SP data; SP's
    // lock-on turns to the enemy when a fight engages; the ps27 rule turned at the blow - this turns at THEIR swing start, which is
    // what makes the dodge readable): a duel partner's swing registered on us engages the rig like our own swing, and the rig walks to its spot in ~0.35 s
    // - unless the camera is being steered: no stick / mouse reader here, so 'steered' = the engine camera's own heading turned faster than 40 deg/s
    // within the last 0.3 s while the duel lock is not on it [TUNING ps40].
    {
        static float ly40;
        static int lk40;
        static uint64_t steer_at;
        float ey40 = g_cst.eng_yaw;
        if (lk40 && dt > 0.001f && !g_s25Lock
            &&
            __builtin_fabsf(__builtin_remainderf(ey40 - ly40, 6.2831853f))
            / dt > 0.70f)
            steer_at = now;
        ly40 = ey40;
        lk40 = 1;
        if (g_ps40[PS40_ENGAGE] && s25_duel && !s25_rig && S40Threat()
            && now - steer_at > 300) {
            static int ne;
            if (LOGCAP(ne, 80))
                Xlog("spmelee cam: ps40 engage latch - the partner's swing is registered on us and the camera is not being steered: the rig engages now and eases to its spot within ~350 ms (frame %llu)", (unsigned long long) g_frame);
            s25_own_ms = now;
            g_s40Boost = now + 350;
        }
        // ps42 H4 (rec41a B 17418 'ps37 reset - our root moved 4.0 m in one tick' + 'DUEL: u21e3 ENGAGE with slot 1 (our swing)' + 'ps25 victim - in a
        // duel without a swing of ours: the camera keeps the player's heading' on ONE tick, camrec 17421 'lat -1.37 back -0.41 ... cyaw +98': our own lethal
        // swing was resolved on its first frame, the swing flag never reached this tick, the stale lens stood in FRONT of our fighter through the kill, clip
        // B 118.40-118.53): a duel only ever engages on a registered swing (ours or the partner's) - the engage itself latches the rig, on the engage tick,
        // unless the camera is being steered (the ps40 stand-in above). t2r-ps42-cam-engage-old.txt = the swing flags only.
        {
            static int d42;
            if (g_ps42[PS42_ENG] && s25_duel && !d42 && !s25_rig
                && now - steer_at > 300) {
                static int n42;
                if (LOGCAP(n42, 80))
                    Xlog("spmelee cam: ps42 engage latch - the duel engaged this tick (a swing is registered): the rig latches now%s (frame %llu)", tp37 ? ", on the tick of a teleport" : "", (unsigned long long) g_frame);
                s25_own_ms = now;
                g_s40Boost = now + 350;
            }
            d42 = s25_duel;
        }
    }
    if (s25_duel && (MduVicPairOn() || MhrRunning())
        && GetFileAttributesA("t2r-ps27-cam-hit-latch-old.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        if (!s25_rig) {
            static int nh;
            if (LOGCAP(nh, 60))
                Xlog("spmelee cam: ps27 hit latch - a blow landed on us before a swing of ours: the rig engages (frame %llu)", (unsigned long long) g_frame);
        }
        s25_own_ms = now;
    }
    uint64_t s25_age = s25_own_ms ? now - s25_own_ms : ~0ULL;
    int s25_eng = g_ps25OwnMs && now - g_ps25OwnMs < SPM25_ACT_MS;
    int s25_act = s25_age < SPM25_ACT_MS || (s25_age < SPM25_ACT_FOLLOW_MS
                                             && s25_eng);
    if (!s25_duel || s25_age >= SPM25_RIG_KEEP_MS)
        s25_rig = 0;
    if (s25_duel && have && s25_act && !s25_rig) {
        s25_rig = 1;
        static int nl;
        if (LOGCAP(nl, 80))
            Xlog("spmelee cam: ps25 rig latched - our own melee move runs in this duel: SP's rig frames the partner (%.0f deg off the line on the kept side, <= %.1f deg/frame; frame %llu)", (double) (SPM25_AZ * 57.29578f), (double) (SPM25_ORB_RATE * 57.29578f / 60.0f), (unsigned long long) g_frame);
    }
    {
        int fr = g_ps25[PS25_VICTIM] && s25_duel && have && !s25_rig;
        if (fr != s25_free_was) {
            s25_free_was = fr;
            static int nl;
            if (LOGCAP(nl, 80)) {
                if (fr)
                    Xlog("spmelee cam: ps25 victim - in a duel without a swing of ours: the camera keeps the player's heading (no look-at, no orbit, no engine duel lock), SP distance + height only (frame %llu)", (unsigned long long) g_frame);
                else
                    Xlog("spmelee cam: ps25 victim - the free camera ends (our swing, or the duel is over; frame %llu)", (unsigned long long) g_frame);
            }
        }
        g_s25Lock = !g_ps25[PS25_VICTIM] || s25_rig;
    }
    // ps32 transitions (rec21 A 12018 'ps25 fov - no melee move of ours' -> 12031 'the 75 fov comes in (weight 0.57)', camrec 12018 'up 1.69 ... fov
    // 75.1' -> 12030 'up 1.60 ... fov 73.6' -> 12045 'up 1.64 ... fov 74.9': a 13-frame gap in our activity pumped the fov by 1.9 deg and the lens by 9 cm;
    // the same at 12180 / 12408 / 15568 / 15655 / 15788, 39 'ps25 fov' + 39 'ps25 height' toggles in the match): the fov weight left on an exponential (tau
    // 0.25 s) from the first idle tick - 45 % gone after 9 frames - and the height the same (tau 0.20 s). SP leaves on a delayed S-curve: fov 75.0 for 4
    // frames, 74.7 at +9, 73.1 at +15, 70.2 at +30; height unchanged for 9 frames, 1.54 at +30 - a gap of a few frames between two moves shows nothing.
    // Both now leave on that curve from the value they have (S32_FOV_OUT_* / S32_UP_OUT_*). t2r-ps32-cam-trans-old.txt = the exponentials.
    {
        float ft = (s25_act || s25_eng) ? 1.0f : 0.0f, f = g_ps25Fw;
        static int fwas = -1;
        static uint64_t f_off;
        static float f_from;
        if (ft > f || !g_ps32[PS32_TRANS]) {
            f += (ft - f) * (1.0f -
                             __builtin_expf(-dt /
                                            (ft >
                                             f ? 0.05f :
                                             SPM25_FOV_TAU_OUT)));
            f_off = 0;
        } else if (ft < 0.5f && f > 0.0f) {
            if (!f_off) {
                f_off = now;
                f_from = f;
            }
            float x =
                ((float) (now - f_off) / 1000.0f -
                 S32_FOV_OUT_DELAY) / S32_FOV_OUT;
            x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
            f = f_from * (1.0f - x * x * (3.0f - 2.0f * x));
        } else
            f_off = 0;
        g_ps25Fw = f;
        int fon = ft > 0.5f;
        if (g_ps25[PS25_FOV] && fon != fwas && g_spm.w > 0.5f) {
            fwas = fon;
            static int nl;
            if (LOGCAP(nl, 120)) {
                if (fon)
                    Xlog("spmelee cam: ps25 fov - our melee move runs: the 75 fov comes in (weight %.2f, frame %llu)", (double) f, (unsigned long long) g_frame);
                else
                    Xlog("spmelee cam: ps25 fov - no melee move of ours: the fov eases back to the explore fov over ~%.0f ms (weight %.2f, frame %llu)", (double) (3000.0f * SPM25_FOV_TAU_OUT), (double) f, (unsigned long long) g_frame);
            }
        }
    }
    // ps25 height (uc2 A / B 'camrec: ... up 1.62' on every settled frame): SP's lens is 1.67-1.70 m above the root in a swing / its follow-through /
    // a hit and 1.55 m with no move (in 45 frames, out 20). t2r-ps25-cam-height-old.txt = 1.62 m flat.
    {
        int mv = s25_act || (g_mfkBlowMs && now - g_mfkBlowMs < 1000);
        float ut = mv ? SPM25_UP_MOVE : SPM25_UP_IDLE;
        static int uwas = -1;
        static float u = SPM25_UP_IDLE, u_from;
        static uint64_t u_off;  // (ps32: the eased height is its own state - g_s25.up carries the finisher push on top)
        if (!init && g_spm.w < 0.05f) {
            u = SPM25_UP_IDLE;
            u_off = 0;
        }
        if (mv || !g_ps32[PS32_TRANS]) {
            u += (ut - u) * (1.0f -
                             __builtin_expf(-dt /
                                            (ut >
                                             u ? SPM25_UP_TAU_IN :
                                             SPM25_UP_TAU_OUT)));
            u_off = 0;
        } else {                // ps32 transitions (block comment above): SP's delayed S-curve out
            if (!u_off) {
                u_off = now;
                u_from = u;
            }
            float x =
                ((float) (now - u_off) / 1000.0f -
                 S32_UP_OUT_DELAY) / S32_UP_OUT;
            x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
            u = u_from + (SPM25_UP_IDLE - u_from) * x * x * (3.0f -
                                                             2.0f * x);
        }
        // ps35 (rec23 A 10107 'ps32 finisher ctxdown:wall-combo-head-smash ... (wall / cover: distance 1.26 -> 0.98 m, lens 1.36 m up, pitch -7.0 deg, fov
        // to SP's 63 ...)', then 10159 'ps25 frame check - WARNING: BOTH fighters out of frame (our head x +0.08 y +1.77, partner's head x -1.02 y +1.24 ... fov
        // 63.1)', 10231 'our head ... y +1.52', 10312 'y +1.56 / +1.10'): SP's 1.34 m / -3.4 deg / fov 63 were measured on two fighters BENT over low cover;
        // ours stood upright at the wall - a 1.36 m lens pitched -7 deg at 0.98 m with a 19 deg half-height can not hold a 1.7 m head (26 deg up). The push is
        // driven by where the two heads ARE (their joints, smoothed 0.12 s): the lens is never more than 0.22 m under the higher head (up to the melee 1.69
        // m), the pitch rises until that head sits at y +0.70 (spm_pitch), and the zoom gives way (g_s32.zl, meleefov.c) when the two heads are too far apart
        // for it. Bent fighters still get SP's numbers.
        float fu = g_s32.ctx ? S32_UP_CTX : S32_UP_FREE;
        if (fin && have && pl) {
            float p3[3], c3[3], h3[3], ab2[16], hy0 = pl[1] + 1.62f, hy1 =
                g_spmEy + 1.62f, ex1 = g_spm.ex, ez1 = g_spm.ez;
            XvBody(SID_PLAYER, pl, p3, c3, h3);
            if (XvFin3(h3) && __builtin_fabsf(h3[1] - pl[1]) < 2.2f)
                hy0 = h3[1];
            const float *al2 =
                g_spmPh ? PuppetLocator(g_spmPh, ab2) : NULL;
            if (al2 && XvFin3(al2)) {
                XvBody(g_spmPh, al2, p3, c3, h3);
                if (XvFin3(h3) && __builtin_fabsf(h3[1] - al2[1]) < 2.2f) {
                    hy1 = h3[1];
                    ex1 = h3[0];
                    ez1 = h3[2];
                }
            }
            float kh =
                g_s32.hok ? 1.0f - __builtin_expf(-dt / 0.12f) : 1.0f;
            g_s32.hy0 =
                g_s32.hok ? g_s32.hy0 + (hy0 - g_s32.hy0) * kh : hy0;
            g_s32.hy1 =
                g_s32.hok ? g_s32.hy1 + (hy1 - g_s32.hy1) * kh : hy1;
            g_s32.hok = 1;
            float hm =
                (g_s32.hy0 >
                 g_s32.hy1 ? g_s32.hy0 : g_s32.hy1) - pl[1] - 0.22f;
            if (hm > SPM25_UP_MOVE)
                hm = SPM25_UP_MOVE;
            if (hm > fu)
                fu = hm;
            // the zoom limit: half the angle between the two heads from the shown lens + 8 deg must fit 90 % of the finisher's half fov
            float cp[3], cf[3], zt = 0.0f;
            if (RcamGet(cp, cf)) {
                float v0[3] =
                    { px - cp[0], g_s32.hy0 - cp[1], pz - cp[2] }, v1[3] =
                    { ex1 - cp[0], g_s32.hy1 - cp[1], ez1 - cp[2] };
                float l0 =
                    __builtin_sqrtf(v0[0] * v0[0] + v0[1] * v0[1] +
                                    v0[2] * v0[2]), l1 =
                    __builtin_sqrtf(v1[0] * v1[0] + v1[1] * v1[1] +
                                    v1[2] * v1[2]);
                if (l0 > 0.2f && l1 > 0.2f) {
                    float cd =
                        (v0[0] * v1[0] + v0[1] * v1[1] +
                         v0[2] * v1[2]) / (l0 * l1);
                    cd = cd > 1.0f ? 1.0f : cd < -1.0f ? -1.0f : cd;
                    float need = 0.5f * __builtin_acosf(cd) + 0.14f, tb =
                        __builtin_tanf((g_mfBase >
                                        30.0f ? g_mfBase : 70.0f) * 0.5f *
                                       0.0174533f);
                    float hf = 0.9f * __builtin_atanf(tb * (g_s32.ctx ? S32_FOV_CTX : S32_FOV_FREE) * 9.0f / 16.0f), hm2 = 0.9f * __builtin_atanf(tb * SPM_FOV * 9.0f / 16.0f); // (the frame's HEIGHT is the tight side)
                    if (need > hf && hm2 > hf + 0.005f) {
                        zt = (need - hf) / (hm2 - hf);
                        zt = zt > 1.0f ? 1.0f : zt;
                    }
                }
            }
            g_s32.zl +=
                (zt - g_s32.zl) * (1.0f - __builtin_expf(-dt / 0.3f));
            // ps40 E3 (rec39a A 20330 'ps35 takedown ctxdown:wall-combo-head-smash - its authored camera is blocked by geometry on every candidate:
            // the duel rig keeps the camera and pushes in', clip A 198.83-200: our backpack over 60 %): the push stood 43 deg off the line attacker -> victim
            // -> wall, behind our own back. For a wall / cover takedown the rig swings to S40_AZ_CTX (side-on, spm25_rig) with the push; when the victim's head
            // is hidden behind our own body from there for 0.25 s it takes the other side, once.
            if (((g_ps40[PS40_SIDE] && g_s32.ctx) || g_s42Side) && g_s32.p > 0.3f) {   // (ps42 H1: every tick of every takedown the rig holds, at the bodies' current placement)
                static S39OccT o40;
                static uint64_t oc_at, t0f;
                static int flp;
                float cq[3], cg[3];
                if (t0f != g_s32.t0) {
                    t0f = g_s32.t0;
                    flp = 0;
                    oc_at = 0;
                }
                if (RcamGet(cq, cg)) {
                    float hp[3] = { ex1, g_s32.hy1, ez1 };
                    int oc = S39Occ(&o40, cq, hp, g_spmPh);
                    if (!oc)
                        oc_at = 0;
                    else if (!oc_at)
                        oc_at = now;
                    else if (!flp && now - oc_at > 250) {
                        flp = 1;
                        S45sFlip(1, now);
                        static int nl4;
                        if (LOGCAP(nl4, 80))
                            Xlog("spmelee cam: ps40 - wall / cover takedown on the rig: the victim's head is hidden behind our own body from the %s side: the rig takes the other side (frame %llu)", g_s25.side >= 0.0f ? "RIGHT" : "LEFT", (unsigned long long) g_frame);
                    }
                }
            }
            {
                static int nlh;
                static uint64_t lh_at;
                if (now - lh_at >= 1000 && LOGCAP(nlh, 200)) {
                    lh_at = now;
                    Xlog("spmelee cam: ps35 finisher push - heads %.2f / %.2f m above our root (ours / the partner's), lens target %.2f m up, zoom held back %.0f %% (frame %llu)", (double) (g_s32.hy0 - pl[1]), (double) (g_s32.hy1 - pl[1]), (double) fu, (double) (g_s32.zl * 100.0f), (unsigned long long) g_frame);
                }
            }
        } else if (!fin) {
            g_s32.hok = 0;
            g_s32.zl +=
                (0.0f - g_s32.zl) * (1.0f - __builtin_expf(-dt / 0.3f));
        }
        g_s25.up = u + (fu - u) * g_s32.p;      // ps32: the finisher push lowers the lens (s32_fin_begin); ps35: not under the heads
        if (g_ps25[PS25_HEIGHT] && mv != uwas && g_spm.w > 0.5f) {
            uwas = mv;
            static int nl;
            if (LOGCAP(nl, 120))
                Xlog("spmelee cam: ps25 height - lens target %.2f m above our root (%s; now %.2f, frame %llu)", (double) ut, mv ? "a melee move of ours / a blow" : "no move", (double) u, (unsigned long long) g_frame);
        }
    }
    // ps25: no re-centre in a duel or on the way out (the rig owns the lens's side; the sideways shift toward the pair's midpoint took up to 0.35 m off SP's
    // 0.80 m beside the line - uc2 A in the line frame 0.56-0.73 m, matchdiag 44008 / 44128 / 44248). Eased, so a duel that starts inside a free-swing framing does not pop.
    {
        float rt = (g_ps25[PS25_RIG]
                    && (s25_duel || !have)) ? 0.0f : 1.0f, r = g_s25.rc;
        if (!init && g_spm.w < 0.05f)
            r = rt;
        else
            r += (rt - r) * (1.0f - __builtin_expf(-dt / 0.2f));
        g_s25.rc = r;
    }
    // melee_duel.c: inside a duel the camera also turns to frame the opponent (SP's melee camera: the hit player's view stayed on the old heading while its body
    // snapped to the attacker). Weight eases in over ~0.15 s, out over ~0.3 s; the target is stored for the render thread (spm_yaw).
    // The camera got thrown to the left on every counter: a takedown camera (counter / escape / finisher) hands back to the game camera,
    // whose own heading never followed the look-at; the look-at then came back in 0.15 s - a throw toward the opponent. After a takedown camera it eases in over 0.8 s.
    static uint64_t xc_end;
    if (g_xcamOn)
        xc_end = now;
    float tin = xc_end && now - xc_end < 2500 ? 0.8f : 0.15f;
    if (g_cam2On && now < g_cam2BoostUntil)
        tin = 0.05f;            // v5-cam2: a victim engage - the attacker is framed inside its windup (cam2_tick)
    // ps25 release (uc2 A 44317 'DUEL: END ... (the partner ended it)' -> 44318 'DUEL lock released', camrec 44319-44337 'spm 0.82 -> 0.02' in 18
    // frames, then 44348 'camcont: ... cause: path our framing -> game camera; duel framing OFF (hard); look-at weight 0.21 -> 0'): when the partner dropped
    // out (downed), (1) the look-at / orbit target was overwritten with 0, 0 - the world origin - on that tick while the look-at weight was still ~1, (2) the
    // framing weight fell with tau 80 ms and the look-at with tau 300 ms, (3) the look-at was cut to 0 the moment the framing weight was gone. Now the last
    // partner position is kept, both weights fall together with tau SPM25_OUT_TAU (SP: 0.5-1.0 s back to the explore camera), our pivot keeps following us
    // and the framing ends when both are gone. t2r-ps25-cam-release-old.txt = the old three.
    {
        int rel = g_ps25[PS25_RELEASE];
        static int rel_was;
        float yt = (have && s25_duel
                    && (s25_rig || !g_ps25[PS25_VICTIM])) ? 1.0f : 0.0f;
        g_s39Yt = yt > 0.5f;   // ps39 (spm25_rig)
        int q43 = S43qTick(now, yt > 0.5f, have && pl, px, pl ? pl[1] : 0.0f, pz, ex, ez, tp37);       // ps43 Q1: 1 = the engage cut's spot is being planned (rays out) - the rig starts on the next tick
        g_spm.yw +=
            (yt - g_spm.yw) * (1.0f -
                               __builtin_expf(-dt /
                                              (yt >
                                               g_spm.
                                               yw ? tin : rel ?
                                               SPM25_OUT_TAU : 0.3f)));
        if (g_spm.yw < 0.002f)
            g_spm.yw = 0;
        if (q43)
            g_spm.yw = 0;
        if (have || !rel) {
            g_spm.ex = fex;
            g_spm.ez = fez;
        }
        g_spm.py = pl ? pl[1] : g_spm.py;
        g_spm.ey = (have || rel) ? g_spmEy : g_spm.py;
        int out = rel && !have && g_spm.ok && (g_spm.w > 0.02f
                                               || g_spm.yw > 0.02f);
        if (out && !rel_was) {
            static int nl;
            if (LOGCAP(nl, 80))
                Xlog("spmelee cam: ps25 release - the partner is out of the framing: framing %.2f + look-at %.2f blend out together over ~%.0f ms on the kept target (frame %llu)", (double) g_spm.w, (double) g_spm.yw, (double) (3000.0f * SPM25_OUT_TAU), (unsigned long long) g_frame);
        }
        rel_was = out;
        if (!have && rel && g_spm.ok && pok) {  // our pivot follows us through the way out
            float nx = px, nz = pz;
            if (g_ps23Piv) {
                SpmSlew(&piv_x, &piv_z, px, pz, dt, &piv_ok);
                nx = piv_x;
                nz = piv_z;
            }
            g_spm.px = nx;
            g_spm.pz = nz;
        }
    }
    if (g_spm.w < 0.002f && !have
        && (!g_ps25[PS25_RELEASE] || g_spm.yw < 0.002f)) {
        g_spm.w = 0;
        g_spm.ok = 0;
        g_spm.yw = 0;
        init = 0;
        was_on = 0;
        return;
    }
    if (have) {
        float tmx = 0.5f * (fpx + fex), tmz = 0.5f * (fpz + fez);
        if (!init) {
            mx = tmx;
            mz = tmz;
            init = 1;
        } else {
            float k = 1.0f - __builtin_expf(-dt / 0.1f);
            mx += (tmx - mx) * k;
            mz += (tmz - mz) * k;
        }
        g_spm.px = fpx;
        g_spm.pz = fpz;
        g_spm.mx = mx;
        g_spm.mz = mz;
        MemoryBarrier();
        g_spm.ok = 1;
        if (!was_on) {
            was_on = 1;
            static int nl;
            if (g_ps25[PS25_RIG]) {     // (ps25: the old line's 'pull-in 0.45 m' was the pre-v5-feel2 constant, and a duel has no re-centre any more)
                if (nl++ < 40)
                    Xlog("spmelee cam: framing on - enemy %.1f m away, lens to SP's %.2f m / %.2f-%.2f m up, %s, fov x%.3f while our melee move runs (SP 70 -> 75), hit shake 0.052-0.087 m (ps25)", (double) __builtin_sqrtf((ex - px) * (ex - px) + (ez - pz) * (ez - pz)), (double) SPM25_DIST, (double) SPM25_UP_IDLE, (double) SPM25_UP_MOVE, s25_duel ? (s25_rig ? "duel: SP rig on the kept shoulder side, no re-centre" : g_ps25[PS25_VICTIM] ? "duel, no swing of ours yet: the camera stays the player's" : "duel: rig + victim engage (old)") : "no duel: re-centre 50% (max 0.35 m), no look-at", (double) SPM_FOV);
            } else if (nl++ < 40)
                Xlog("spmelee cam: framing on - enemy %.1f m away, pull-in %.2f m, re-centre %.0f%% (max %.2f m), fov x%.3f (SP 70 -> 75), hit shake peak %.3f m x class", (double) __builtin_sqrtf((ex - px) * (ex - px) + (ez - pz) * (ez - pz)), (double) SPM_PULL, (double) (SPM_CENTRE * 100), (double) SPM_CENTRE_MAX, (double) SPM_FOV, 0.045);
        }
    }
    // ps25b frame check (after the reversed view of build ps25): where OUR head and the PARTNER's head are on screen in the SHOWN camera (rcam_get =
    // the final render camera after every filter; x, y in half-widths / half-heights from the centre, + = right / up, |x|, |y| <= 1 = in frame, 9.99 =
    // behind the lens; fov = the held one, 16:9). One line a second in a duel, and one warning per episode when either is out of frame for > 0.5 s.
    // SP: own head -0.24, +0.35, enemy head +0.16, +0.49 (mirrored x on the left shoulder).
    if (have && s25_duel && g_spm.ok && pok) {
        static uint64_t fc_at, fc_out;
        static int fc_warned, nlf, nlw;
        float cp[3], cf[3];
        if (RcamGet(cp, cf)) {
            float fh = __builtin_sqrtf(cf[0] * cf[0] + cf[2] * cf[2]);
            float fv = g_mfHold
                && g_mf_held > 30.0f ? g_mf_held : g_mfBase >
                30.0f ? g_mfBase : 70.0f;
            float th = __builtin_tanf(fv * 0.5f * 0.0174533f), tv =
                th * 9.0f / 16.0f;
            float sx[2] = { 9.99f, 9.99f }, sy[2] = { 9.99f, 9.99f };
            if (fh > 0.2f && fh == fh) {
                float rx = -cf[2] / fh, rz = cf[0] / fh;        // screen right: the right of a heading f = (-fz, fx)
                float ux = -cf[1] * cf[0] / fh, uy = fh, uz = -cf[1] * cf[2] / fh;      // screen up (no roll)
                float H[2][3] =
                    { { px, pl[1] + 1.55f, pz }, { ex, g_spmEy + 1.55f,
                                                  ez } };
                for (int i = 0; i < 2; i++) {
                    float vx = H[i][0] - cp[0], vy = H[i][1] - cp[1], vz =
                        H[i][2] - cp[2], dep =
                        vx * cf[0] + vy * cf[1] + vz * cf[2];
                    if (dep > 0.05f) {
                        sx[i] = (vx * rx + vz * rz) / dep / th;
                        sy[i] = (vx * ux + vy * uy + vz * uz) / dep / tv;
                    }
                }
            }
            int in0 = sx[0] >= -1.0f && sx[0] <= 1.0f && sy[0] >= -1.0f
                && sy[0] <= 1.0f, in1 = sx[1] >= -1.0f && sx[1] <= 1.0f
                && sy[1] >= -1.0f && sy[1] <= 1.0f;
            static S39OccT oc25;
            int occ1 = 0;       // ps39 C5: the partner's head behind our own body / geometry = not in frame
            if (g_ps39[PS39_MF] && in1) {
                float hp[3] = { ex, g_spmEy + 1.55f, ez };
                occ1 = S39Occ(&oc25, cp, hp, g_spmPh);
                if (occ1)
                    in1 = 0;
            }
            if (in0 && in1) {
                fc_out = 0;
                fc_warned = 0;
            } else {
                if (!fc_out)
                    fc_out = now;
                if (!fc_warned && now - fc_out > 500) {
                    fc_warned = 1;
                    if (LOGCAP(nlw, 60))
                        Xlog("spmelee cam: ps25 frame check - WARNING: %s out of frame for > 0.5 s (our head x %+.2f y %+.2f, partner's head x %+.2f y %+.2f; %s, framing %.2f, look-at %.2f, fov %.1f, frame %llu)", !in0 && !in1 ? "BOTH fighters" : !in0 ? "OUR head" : occ1 ? "the PARTNER's head (inside the frustum but HIDDEN behind our own body / geometry, ps39)" : "the PARTNER's head", (double) sx[0], (double) sy[0], (double) sx[1], (double) sy[1], s25_rig ? "rig latched" : g_ps25[PS25_VICTIM] ? "no swing of ours yet: the player's own camera (expected when he looks elsewhere)" : "victim engage (old)", (double) g_spm.w, (double) g_spm.yw, (double) fv, (unsigned long long) g_frame);
                }
            }
            if (now - fc_at >= 1000) {
                fc_at = now;
                if (LOGCAP(nlf, 300))
                    Xlog("spmelee cam: ps25 frame check - our head x %+.2f y %+.2f, partner's head x %+.2f y %+.2f (SP -0.24 +0.35 / +0.16 +0.49, x mirrored on the left shoulder; %s, look-at %.2f, fov %.1f, frame %llu)", (double) sx[0], (double) sy[0], (double) sx[1], (double) sy[1], in0 && in1 ? "both in frame" : occ1 && in0 ? "the PARTNER HIDDEN behind our own body / geometry" : "OUT OF FRAME", (double) g_spm.yw, (double) fv, (unsigned long long) g_frame);
            }
        }
    }
    // ps25: what the rig did, for the next recording (render thread values): the ENGINE camera's lens and the shown lens in the line frame (+ = the right of
    // the line us -> partner), every 30 ticks while the rig is in, capped
    if (g_ps25[PS25_RIG] && g_spm.ok && g_spm.yw > 0.5f && g_s25.dok) {
        static uint32_t tk;
        static int nl;
        if ((tk++ % 30) == 0 && LOGCAP(nl, 240))
            Xlog("spmelee cam: ps25 rig - line frame: engine base %+.2f m beside / %.2f m behind, shown %+.2f / %.2f (SP 0.80 on the kept side / 0.90), side %s, weight %.2f, up %.2f, fov weight %.2f (frame %llu)", (double) g_s25.eb_r, (double) g_s25.eb_b, (double) g_s25.sh_r, (double) g_s25.sh_b, g_s25.side >= 0.0f ? "RIGHT" : "LEFT", (double) g_s25.w, (double) g_s25.up, (double) g_ps25Fw, (unsigned long long) g_frame);
    }
    // v5-feel2 round 3: SP's lens distance for this pair distance, and how far OUT the lens may go (spm_pos): a ray from our head (0.3 m out, beside our
    // capsule) back toward the engine camera's side to the wanted spot + SPM_CAM_MARGIN, kicked here and read next tick. A hit pulls the allowed length in
    // at once (tau 60 ms), a clear ray lets it out at 1.2 m/s. Locator rays, mask 0x1b07 (cars / props / bodies too), our own body ignored.
    if (have && pl) {
        {
            float p44[3], c44[3], h44[3];
            XvBody(SID_PLAYER, pl, p44, c44, h44);     // ps44 R7: our live head joint for the head floor (s44h_filter)
            if (XvFin3(h44) && __builtin_fabsf(h44[1] - pl[1]) < 2.2f
                && (h44[0] - pl[0]) * (h44[0] - pl[0]) + (h44[2] -
                                                          pl[2]) *
                (h44[2] - pl[2]) < 1.0f) {
                g_s44h.ok = 0;
                MemoryBarrier();
                g_s44h.H[0] = h44[0];
                g_s44h.H[1] = h44[1];
                g_s44h.H[2] = h44[2];
                g_s44h.t = GshNow();
                MemoryBarrier();
                g_s44h.ok = 1;
            } else
                g_s44h.ok = 0;
        }
        uint64_t job = g_spmRjob;
        float jlen = g_spm_rlen, bm = g_spm_rbm;
        static int logged_b;
        float pd =
            __builtin_sqrtf((ex - px) * (ex - px) + (ez - pz) * (ez - pz));
        float bt =
            pd <= 0.9f ? SPM_CAM_BACK_CLOSE : pd <=
            1.4f ? SPM_CAM_BACK_CLOSE + (SPM_CAM_BACK -
                                         SPM_CAM_BACK_CLOSE) * (pd -
                                                                0.9f) /
            0.5f : pd <=
            1.8f ? SPM_CAM_BACK + (SPM_CAM_BACK_FAR - SPM_CAM_BACK) * (pd -
                                                                       1.4f)
            / 0.4f : SPM_CAM_BACK_FAR;
        if (g_ps25[PS25_RIG])
            bt = SPM25_DIST;    // ps25: SP 1.27 / 1.24 / 1.27 m for a pair 0.6-0.9 / 0.9-1.5 / 1.5-2.5 m apart - one distance
        if (g_s32.p > 0.0f)
            bt += ((g_s42Side ? S42_DIST_SIDE : g_s32.ctx ? S32_DIST_CTX : S32_DIST_FREE) - bt) * g_s32.p;     // ps32: the finisher push comes in (s32_fin_begin); ps42 H1: the side-on two-shot stands off
        g_spm.bt = bt;
        if (job) {
            g_rayErr = 0;
            float t = RayT(job);
            job = 0;
            int e1 = g_rayErr;
            g_rayErr = 0;
            float t2 = g_spm_rjob2 ? RayT(g_spm_rjob2) : -2.0f;
            int e2 = g_rayErr || !g_spm_rjob2;
            g_spm_rjob2 = 0;
            // v5-cam4 (A/B v5-int14 B 15546, our victim half of a pair 0.5-1 m from the partner: 'lens to us 0.19 m (engine) / 0.37 m (framing)' - our ray
            // stopped ~0.67 m out too): mask 0x1b07 stops on characters, so the PARTNER's body standing beside / behind the camera line pulled our lens in
            // (SP: NPC bodies never pull its melee camera in). A hit within 0.45 m of the partner's body axis is the partner: the same ray cast again with
            // the partner ignored decides (a stop in its first 12 % = our own capsule at the start: clear). Walls / props / cars still stop it.
            if (!e1 && t >= 0.0f && t < 1.0f
                && g_spm_rcx * g_spm_rcx + g_spm_rcz * g_spm_rcz > 0.5f) {
                float hd = 0.3f + t * jlen, hx = px + g_spm_rcx * hd, hz =
                    pz + g_spm_rcz * hd, ox = hx - ex, oz = hz - ez;
                if (ox * ox + oz * oz < 0.45f * 0.45f) {
                    if (!e2) {
                        g_spmRhit[1]++;
                        t = t2 >= 0.0f && t2 < 0.12f ? -1.0f : t2;
                        if (t2 >= 0.0f && t2 < 0.12f)
                            g_spmRhit[2]++;
                    }
                } else
                    g_spmRhit[0]++;
            }
            g_rayErr = e1;
            if (!g_rayErr) {
                float al = t >= 0.0f
                    && t < 1.0f ? 0.3f + t * jlen - SPM_CAM_MARGIN : 9.0f;
                g_spmRal = al;
                if (!(bm > 0.0f))
                    bm = al < 9.0f ? al : bt;
                else if (al < bm) {
                    // v5-cam4 (real walls keep a smooth pull-in, <= 0.10 m/frame unless the lens would enter the wall): the allowed length comes
                    // in at most 6 m/s (0.10 m per 60 fps frame); only the part that would leave the lens INSIDE the surface (beyond hit - 5 cm) is cut at once
                    float nb = bm - fminf(bm - al, dt * 6.0f), inwall =
                        al + SPM_CAM_MARGIN - 0.05f;
                    if (nb > inwall) {
                        nb = inwall;
                        static uint32_t nw;
                        if ((nw++ & 31) == 0)
                            Xlog("spmelee cam: a surface came within the lens's distance - the lens cut in to %.2f m (in front of it), the rest eased (x%u)", (double) nb, nw);
                    }
                    bm = nb;
                } else
                    bm = fminf(al, bm + dt * 1.2f);
                if (bm < 0.3f)
                    bm = 0.3f;
                g_spm.bmax = bm;
                if (!logged_b++)
                    Xlog("spmelee cam: SP lens distance %.2f m for the pair %.2f m apart (%s), allowed out to %.2f m by the head-to-lens ray", (double) bt, (double) pd, g_ps25[PS25_RIG] ? "ps25: SP 1.26 at every pair distance" : "SP 1.30 close / 1.22 / 1.18 far", (double) bm);
            }
        }
        float cx = g_pcam.px - px, cz = g_pcam.pz - pz, cl =
            __builtin_sqrtf(cx * cx + cz * cz);
        if (g_ps25[PS25_RIG] && g_spm.yw > 0.5f && g_s25.dok) {
            cx = g_s25.dx;
            cz = g_s25.dz;
            cl = 1.0f;
        }                       // ps25: the ray runs along the RIG's lens line (the shown one), not the engine camera's
        {
            static float ldx, ldz;
            static int ldok;    // v5-cam4: an engine lens collapsed onto our head keeps the ray on the last direction (spm_pos does the same)
            if (cl > 0.3f && cl < 10.0f) {
                ldx = cx / cl;
                ldz = cz / cl;
                ldok = 1;
            } else if (ldok && cl <= 0.3f) {
                cx = ldx;
                cz = ldz;
                cl = 1.0f;
            }
        }
        if (cl > 0.2f && cl < 10.0f) {
            cx /= cl;
            cz /= cl;
            float H[3] =
                { px + cx * 0.3f, pl[1] + SPM_CAM_UP, pz + cz * 0.3f },
                S[3] =
                { px + cx * (bt + SPM_CAM_MARGIN), pl[1] + SPM_CAM_UP,
           pz + cz * (bt + SPM_CAM_MARGIN) };
            jlen = bt + SPM_CAM_MARGIN - 0.3f;
            job = WorldRayKickM(H, S, 0, 0x1b07);
            g_spm_rjob2 = job
                && g_spmPh ? WorldRayKickM(H, S, g_spmPh, 0x1b07) : 0;
            g_spm_rcx = cx;
            g_spm_rcz = cz;
        }
        g_spmRjob = job;
        g_spm_rlen = jlen;
        g_spm_rbm = bm;
        // ps39 C6 (rec38a A 18262 'spmelee cam: a surface came within the lens's distance - the lens cut in to 0.85 m', 18263 'camcont: inward step
        // (collision pull-in) ... 0.38 m ... allowed 0.85 m, newest ray answer 0.60 m', camrec 18258-18306 'dist 0.41 .. 0.72' for 0.9 s; clip A 170.13-171.03:
        // our own head over 50-70 % of the picture): a surface on the rig's kept side pulled the lens in along its line onto our head. SP swaps to the free
        // side. While the allowed length is under S39_SWAP_BELOW a second ray tests the OTHER side's lens line (partner ignored, a stop in its first 12 % =
        // our own capsule); clear on three answers after 150 ms = the rig swaps its side (spm25_rig: the lens swings round behind us at the rig's rate, the
        // head-to-lens ray follows it and lets the length back out), at most once in 1.5 s. Both sides blocked = the lens rises up to 0.35 m over our head.
        {
            static uint64_t sjob, blk_at, swap_at;
            static int clr_n, told;
            static float rz;
            int rig_on = g_ps39[PS39_SWAP] && g_ps25[PS25_RIG]
                && g_spm.yw > 0.5f && g_s25.dok;
            float oth = -1.0f;
            // ps40 E1 (rec39a B 22319 'ps39 rig side swap - the kept LEFT side is blocked at 0.36 m ..., the other side is clear', then 22374 'held at
            // 0.30 m by a surface and the other side is not clear'; clip B 197.47-198.63: the lens inside our shoulder / head, a wall on both sides): (1) the
            // other side's ray ignored the partner and took a stop in its first 12 % for our own capsule - a wall 0.36 m behind us read as CLEAR. It ignores
            // our own body now and any stop is a stop. (2) the lens came in along its line to 0.23-0.31 m from our root at head height (camrec 22311-22317)
            // before anything else happened, and the swap then swung it round at that radius. The lens now RISES with the pull-in, from the first cm: 0.70 m
            // up at 0.30 m allowed length (0.76 m or more from our head at every length), pitched down with it - in, over our head, and out again on the
            // free side; a swing whose arc is blocked passes over the top the same way (the head-to-lens ray runs along the shown line every tick).
            if (sjob) {
                g_rayErr = 0;
                float t = RayT(sjob);
                sjob = 0;
                if (!g_rayErr)
                    oth = (t >= (g_ps40[PS40_OVER] ? 0.0f : 0.12f)
                           && t <
                           1.0f) ? 0.3f + t * (bt + SPM_CAM_MARGIN -
                                               0.3f) -
                        SPM_CAM_MARGIN : 9.0f;
            }
            int blocked = rig_on && bm > 0.0f && bm < S39_SWAP_BELOW;
            if (!blocked) {
                blk_at = 0;
                clr_n = 0;
                told = 0;
            } else {
                if (!blk_at)
                    blk_at = now;
                if (oth >= 0.0f)
                    clr_n = oth >= bt - 0.05f ? clr_n + 1 : 0;
                float lx = ex - px, lz = ez - pz, ll =
                    __builtin_sqrtf(lx * lx + lz * lz);
                if (ll > 0.3f) {
                    float az =
                        __builtin_atan2f(-lx, -lz) - g_s25.side * SPM25_AZ,
                        sdx = __builtin_sinf(az), sdz = __builtin_cosf(az);
                    float H2[3] =
                        { px + sdx * 0.3f, pl[1] + SPM_CAM_UP,
                  pz + sdz * 0.3f }, S2[3] =
{ px + sdx * (bt + SPM_CAM_MARGIN), pl[1] + SPM_CAM_UP, pz + sdz * (bt + SPM_CAM_MARGIN) };
                    sjob =
                        WorldRayKickM(H2, S2,
                                         g_ps40[PS40_OVER] ? 0 : g_spmPh,
                                         0x1b07);
                }
                if (clr_n >= 3 && now - blk_at > 150
                    && now - swap_at > 1500 && !S43SwapWait(now)) {
                    swap_at = now;
                    clr_n = 0;
                    S45sFlip(1, now);
                    static int nls;
                    if (LOGCAP(nls, 80))
                        Xlog("spmelee cam: ps39 rig side swap - the kept %s side is blocked at %.2f m (a surface on the lens's line), the other side is clear: the lens swings round behind us to the %s side (frame %llu)", g_s25.side >= 0.0f ? "RIGHT" : "LEFT", (double) bm, g_s25.side >= 0.0f ? "LEFT" : "RIGHT", (unsigned long long) g_frame);
                }
            }
            {
                static int hn44;        // ps44 R7: the kept side pulled in, the lens within S44H_MIN of our live head, the other side clear - ONE cut to it
                float cp4[3], cf4[3], d4 = 9.0f;
                if (g_ps44[PS44_HEAD] && blocked && g_s44h.ok
                    && RcamGet(cp4, cf4))
                    d4 = __builtin_sqrtf((cp4[0] - g_s44h.H[0]) * (cp4[0] -
                                                                   g_s44h.
                                                                   H[0]) +
                                         (cp4[1] - g_s44h.H[1]) * (cp4[1] -
                                                                   g_s44h.
                                                                   H[1]) +
                                         (cp4[2] - g_s44h.H[2]) * (cp4[2] -
                                                                   g_s44h.
                                                                   H[2]));
                hn44 = d4 < S44H_MIN + 0.10f ? hn44 + 1 : 0;
                if (hn44 >= 2 && clr_n >= 1 && now - swap_at > 1500
                    && !S43SwapWait(now)) {
                    hn44 = 0;
                    swap_at = now;
                    clr_n = 0;
                    S45sFlip(2, now);
                    static int nl44;
                    if (LOGCAP(nl44, 80))
                        Xlog("spmelee cam: ps44 head floor - the kept %s side is pulled in to %.2f m and the shown lens is %.2f m from our live head: the other side is clear - ONE cut to the %s side (frame %llu)", g_s25.side >= 0.0f ? "RIGHT" : "LEFT", (double) bm, (double) d4, g_s25.side >= 0.0f ? "LEFT" : "RIGHT", (unsigned long long) g_frame);
                }
            }
            float rt = blocked && now - blk_at > 250
                && now - swap_at > 900 ? fminf(0.35f,
                                               (S39_SWAP_BELOW -
                                                bm) * 0.9f) : 0.0f;
            if (g_ps40[PS40_OVER]) {
                float q = rig_on
                    && bm > 0.0f ? (0.95f - bm) / 0.65f : 0.0f;
                q = q < 0.0f ? 0.0f : q > 1.0f ? 1.0f : q;
                rt = q * (g_ps41 ? 0.30f : 0.70f);
                g_s40Pq = q * (g_ps41 ? 0.12f : 0.35f);        /* ps41 G7 (rec40a B 15564 "rises 0.50 m over our head", clip B 12.33-12.47: a top-down view of our own head): capped */
                rz += (rt - rz) * (1.0f - __builtin_expf(-dt / 0.05f));
                {
                    static int up40;
                    int u = q > 0.3f;
                    static int nlu;
                    if (u && !up40 && LOGCAP(nlu, 120))
                        Xlog("spmelee cam: ps40 over the head - the duel lens is allowed %.2f m by a surface: it rises %.2f m with the pull-in and pitches %.0f deg further down (never into our head; frame %llu)", (double) bm, (double) rt, (double) (g_s40Pq * 57.29578f), (unsigned long long) g_frame);
                    up40 = u;
                }
            } else {
                g_s40Pq = 0.0f;
                rz +=
                    (rt - rz) * (1.0f -
                                 __builtin_expf(-dt /
                                                (rt >
                                                 rz ? 0.15f : 0.35f)));
            }
            if (rz < 0.002f)
                rz = 0.0f;
            g_s39Rz = rz;
            if (rt > 0.1f && !told) {
                told = 1;
                static int nlr;
                if (LOGCAP(nlr, 80))
                    Xlog("spmelee cam: ps39 - the duel lens is held at %.2f m by a surface and the other side is not clear: it rises %.2f m over our head instead of sitting in it (frame %llu)", (double) bm, (double) rt, (unsigned long long) g_frame);
            }
        }
        {
            static uint64_t lt;
            if (!lt)
                lt = now;
            if (now - lt >= 30000) {
                lt = now;
                if (g_spmRhit[0] || g_spmRhit[1])
                    Xlog("spmelee cam: head-to-lens ray hits in 30 s - geometry %u, the partner's body %u (cast again through it; %u of those then clear: our own capsule)", g_spmRhit[0], g_spmRhit[1], g_spmRhit[2]);
                g_spmRhit[0] = g_spmRhit[1] = g_spmRhit[2] = 0;
            }
        }
    } else {
        // v5-cam4 (A/B v5-int14 B 12566: 'inward step 0.0 deg / 0.53 m ... engine lens moved 0.00 m', cause 'framing weight 1.00 -> 0.83'): the partner
        // dropped out of the framing for a tick and the allowed length was zeroed at once - spm_pos then fell back to the engine's own 0.65 m while the
        // framing weight was still ~1: a 0.53 m pull-in in one frame. The allowed length is kept while the framing fades out; only the ray is drained.
        if (g_spm.w < 0.002f) {
            g_spm.bmax = 0.0f;
            g_spm_rbm = 0.0f;
            g_s39Rz = 0.0f;
        }
        if (g_spmRjob) {
            RayT(g_spmRjob);
            g_spmRjob = 0;
        }
        if (g_spm_rjob2) {
            RayT(g_spm_rjob2);
            g_spm_rjob2 = 0;
        }
    }
    (void) logged;
    g_gshAny = 1;
}

// render thread (gsh_filter): pos[3] += pull-in + re-centre + positional hit shake; 1 = anything live. now = gsh_now().
static int SpmPos(double now, float *pos)
{
    float w = g_spm.w;
    int live = 0;
    if (g_spm.ok && w > 0.001f) {
        float dx = g_spm.px - pos[0], dz = g_spm.pz - pos[2], len =
            __builtin_sqrtf(dx * dx + dz * dz);
        // v5-cam4 (A/B v5-int13: every rejected 178 deg flip came with the engine lens collapsing onto our head - camcont 'ENGINE CAMERA position jump
        // 0.8 deg / 1.25 m' at B 12570, 'inward step ... 1.00 m' at 11106 one frame before the flip at 11107; B18 f826-831 next to the truck: the lens in
        // and out by ~0.5 m every frame; int11's 'lens inside / behind B's own head'): below 0.3 m from our root the framing had no direction, skipped
        // itself and showed the engine's collapsed lens inside our head for that frame. It keeps the last direction now (the lens stays on its side and
        // its distance comes from our own head-to-lens ray, g_spm.bmax, cast along that same direction by spm_tick).
        // ps25 (rec15 B 30847 'camdiag: ... LENS STEP > 0.5 m IN ONE FRAME ... shown step 0.74 m (world; our root 0.01 m, engine camera 0.06 m) ...
        // framing w 0.89', A 34704 'LENS WITHIN 0.45 m OF OUR HEAD ... engine camera 0.49 m'): an engine lens 0.3-0.6 m from our pivot still had a direction,
        // but one that turns by tens of degrees for a few cm of engine motion - and the framing pushes the lens out to 1.26 m along it (the lens flipped to
        // the front of our body at the engage). Up to SPM25_DIR_KEEP the last direction is kept (the distance stays the engine's). With the rig in, the
        // direction is the rig's anyway (spm25_rig). t2r-ps25-cam-rig-old.txt = 0.3 m.
        {
            static float lhx, lhz;
            static int lok;
            float keep = g_ps25[PS25_RIG] ? SPM25_DIR_KEEP : 0.3f;
            if (len > keep && len < 8.0f && dx == dx && dz == dz) {
                lhx = dx / len;
                lhz = dz / len;
                lok = 1;
            } else if (lok && len <= 0.3f && g_spm.bmax > 0.0f) {
                pos[0] = g_spm.px - lhx * 0.31f;
                pos[2] = g_spm.pz - lhz * 0.31f;
                dx = lhx * 0.31f;
                dz = lhz * 0.31f;
                len = 0.31f;
                InterlockedIncrement(&g_spmCollapse);
            } else if (lok && len > 0.3f && len <= keep && dx == dx
                       && dz == dz) {
                pos[0] = g_spm.px - lhx * len;
                pos[2] = g_spm.pz - lhz * len;
                dx = lhx * len;
                dz = lhz * len;
            }
        }
        // v5-feel2 (A/B v5-int8: 'cam2: duel framing shown - lens 1.75-1.81 m above our root, 1.10-1.17 m behind'): the MP melee camera already sits ~1.1-1.2 m
        // behind, inside the 1.2-1.6 m fade of the pull-in gate below, so the height move rode on a weight of ~0 and the lens stayed at the engine's ~1.8 m.
        // The height is its own move now, on the framing weight alone: down to SP's 1.61 m above our root (lowering only brings the lens toward our own head
        // height; it is lifted by at most SPM_CAM_RAISE_MAX). The horizontal pull-in keeps its gate.
        if (len > 0.3f && len < 8.0f && dx == dx && dz == dz) {
            float dy =
                g_spm.py + (g_ps25[PS25_HEIGHT] ? g_s25.up : SPM_CAM_UP) -
                pos[1];
            if (dy > SPM_CAM_RAISE_MAX)
                dy = SPM_CAM_RAISE_MAX; // (ps25 height: spm_tick)
            if (dy == dy && dy > -1.5f) {
                pos[1] += dy * w;
                live = 1;
            }
            {
                float rz = g_s39Rz;
                if (rz > 0.0f && rz < 0.8f)
                    pos[1] += rz * w;
            }                   // ps39 C6 (spm_tick): both shoulder sides blocked - over our head
        }
        if (len > 0.3f && len < 8.0f && dx == dx && dz == dz && w > 0.001f) {
            // v5-feel2 round 3 (A14 vs the gold clip: too close - our own backpack / torso filled the centre-right third and hid half the
            // partner; A/B v5-int9 'lens ... 1.00-1.21 m behind us'): the MP melee camera sits ~1.1 m behind, and the pull-in only ever pulled IN - it
            // stayed there. SP's distance for this pair distance (spm_back, measured) is the target now, IN or OUT; OUT only as far as the ray from our
            // head toward the lens is clear (g_spm.bmax, spm_tick - the same pull-in for walls as the pin camera's arm). No 1.2-1.6 m gate any more (it
            // faded the move out exactly where the MP camera sits); the move is continuous in the engine camera's distance, so no on / off pop.
            float want = g_spm.bt > 0.5f ? g_spm.bt : SPM_CAM_BACK, bm =
                g_spm.bmax;
            if (!(bm > 0.0f))
                bm = len;       // no ray answer yet: never out past the engine camera
            if (want > bm)
                want = bm > len ? bm : (len < want ? len : want);       // out only up to the clear length (and never pushed in by the ray past the engine's own spot)
            if (want < len) {
                float flo = 1.1f - 0.25f * g_s32.p, fl =
                    len < flo ? len : flo;
                if (want < fl)
                    want = fl;
            }                   // in: never closer than 1.1 m (or the engine's own, if closer); ps32: 0.85 m at the full finisher push
            float hx = dx / len, hz = dz / len, pull = (len - want) * w;        // + = in toward us, - = out
            float sx = g_spm.mx - g_spm.px, sz = g_spm.mz - g_spm.pz, a =
                sx * hx + sz * hz;
            sx -= a * hx;
            sz -= a * hz;       // midpoint offset perpendicular to the view line
            float rc = g_ps25[PS25_RIG] ? g_s25.rc : 1.0f;      // ps25: no re-centre in a duel / on the way out (spm_tick)
            float sl = __builtin_sqrtf(sx * sx + sz * sz), k =
                SPM_CENTRE * w * rc;
            if (sl * k > SPM_CENTRE_MAX * w * rc)
                k = SPM_CENTRE_MAX * w * rc / sl;
            if (sl > 1e-4f) {
                pos[0] += sx * k;
                pos[2] += sz * k;
            }
            pos[0] += hx * pull;
            pos[2] += hz * pull;
            live = 1;
            float t = (float) (now - g_spm.kt0);
            if (g_spm.kt0 > 0 && t >= 0 && t < 0.5f) {  // SP camera-shake-melee-hit-b lasts 0.5 s; peak 52-87 mm (nr-1031 melee-b: fist/light 0.8 class -> 52 mm, victim 1.87 class -> 87 mm, linear); envelope tau 0.08 s = measured rms/peak ~0.2 (10-18 mm / 52-87 mm)
                if (g_ps32[PS32_SHAKE]) {       // ps32 (melee_feel.c MFK32): SP's lens moves ONCE - a dip of 19-36 mm x scale against the root, +3 .. +10 frames, lowest at +6 - no 13 / 17 Hz sine
                    float xd = (t - 0.05f) / 0.117f;
                    if (xd > 0.0f && xd < 1.0f) {
                        float sd = __builtin_sinf(3.14159265f * xd);
                        pos[1] -= 0.024f * g_spm.kamp * sd * sd;
                    }
                } else {
                    float env =
                        __builtin_expf(-t / 0.08f) * (t <
                                                      0.01f ? t /
                                                      0.01f : 1.0f), pk =
                        0.052f + (g_spm.kamp - 0.8f) * (0.035f / 1.07f);
                    if (pk < 0.052f)
                        pk = 0.052f;
                    if (pk > 0.087f)
                        pk = 0.087f;
                    float s1 =
                        pk * env * __builtin_sinf(6.2831853f * 13.0f * t),
                        s2 =
                        0.6f * pk * env * __builtin_sinf(6.2831853f *
                                                         17.0f * t + 1.0f);
                    pos[0] += -hz * s1;
                    pos[2] += hx * s1;
                    pos[1] += s2;
                }
            }
        }
    }
    return live;
}

// render thread (gsh_filter): the duel look-at. pos = the (pulled-in) render camera position, q = its quaternion (x y z w). Returns the world-yaw (rad, about +Y) to apply: the camera
// turns toward a point 60 % of the way from us to the opponent, SPM_YAW_K of the way (the player's own stick still biases the view), at most +-75 deg. Which axis the camera looks along
// (+Z or -Z of its quaternion) is judged each call: it sits behind the player, so its forward points at us.
#define SPM_YAW_K 1.0f          // always on the right like SP: full framing, no stick bias
#define SPM_YAW_MAX 3.1416f
#define SPM_CAM_FWD_SIGN -1     // the engine camera looks along -Z of its quaternion (A/B logs: learnt sign -1 every time)
// ps25b (build ps25 had neither character in frame during a whole fight; ablogs-ps25rig A camrec 31209 'cyaw -157.0'
// 12 frames after the latch, 31257 'pitch +13.8 cyaw +144.0', 31299 'lat +1.03 back +0.72 ... pitch +16.5 cyaw +144.5'; matchdiag 31265: us 84.0 / 331.9,
// partner 84.2 / 334.6, lens 84.9 / 331.3, vistrace 31264 dir 0.49 0.29 -0.82 = EXACTLY opposite the aim between the two fighters, pitched +16.5 where the
// aim is -16.5): the render camera's view axis is +Z of its quaternion (t2r-ext.c rcam_capture: 'the forward axis of the camera quaternion is +Z (xcam /
// intro cam calibration, sign 1)'; the SP recorder's camera the same), NOT -Z. With the earlier constant -1 the look-at took -Z for the view: a camera
// looking AT us read as looking away (every 'cam2: ... look-at turn ... (gate 1, al/dist -0.93, sign -9)' line; 'the view looked away from us on 2012
// test(s) in 30 s' at A 26348), so its own gate switched it off - the duel's yaw + pitch look-at has not run while the camera faced the
// fight, the ENGINE's melee camera did the aiming (uc2 A 'us +21.7 / partner +2.8 deg off the view centre'). ps25 took that gate out inside the rig, and
// the look-at then did what the wrong sign says: it turned -Z onto the aim = the view 180 deg away from it, after which spm_pitch's gate (same sign)
// opened and pitched -Z to -16.5 deg = the view to +16.5. Inside the rig the sign is +1 (yaw and pitch); t2r-ps25-cam-rig-old.txt keeps -1 + the gate.
static int SpmFwdSign(void)
{
    return g_ps25[PS25_RIG] ? 1 : SPM_CAM_FWD_SIGN;
}

// duel timeline (melee_duel_tl.c, the duel camera lock used to spin the view ~90-180 deg within a few frames when the opponent's position jumped):
// the look-at's ABSOLUTE world heading (camera heading + spm_yaw's turn) follows its target eased (tau DTL_CAM_TAU) and at most DTL_CAM_YAW_RATE; the
// pitch turn the same at DTL_CAM_PITCH_RATE. Absolute, so a flip of the forward-sign test or a game-camera jump does not move the view either.
// Set by the main thread (dtl_on); render thread reads it.
static volatile int g_dtlCamOn;
static volatile LONG g_spmSgnFlips;   // v5-cam3: the camera forward-sign contradicted the learnt one at high confidence (spm_yaw; dtl_tick logs)
static volatile LONG g_dtlCamJumps;   // look-at target jumps > 34 deg the limiter absorbed (render thread counts, dtl_tick logs)
#define DTL_CAM_YAW_RATE 3.4907f        // rad/s = 200 deg/s
#define DTL_CAM_PITCH_RATE 1.5708f      // rad/s = 90 deg/s
#define DTL_CAM_TAU 0.10f
static float DtlCamStep(float err, float dt, float rate)
{
    float st = err * (1.0f - __builtin_expf(-dt / DTL_CAM_TAU)), mx =
        rate * dt;
    return st > mx ? mx : st < -mx ? -mx : st;
}

static float g_dtlc_yout, g_dtlc_yv;    // the look-at's world heading (rad) + its turn speed (v5-cam2 spring); cam2_orbit carries it with the orbit
#define CAM2_YAW_OMEGA 10.0f    // rad/s: look-at spring (critically damped: ~0.45 s to settle, no velocity step at its start / end)
#define CAM2_YAW_RATE 4.1888f   // rad/s = 240 deg/s (SP render camera in a fight: max 5.6 deg per 60 fps frame = 336 deg/s, p99 1.6 deg)
#define CAM2_BOOST_OMEGA 16.0f  // a victim engage: the attacker framed inside its windup
#define CAM2_BOOST_RATE 10.472f // rad/s = 600 deg/s (the OLD cap of both springs: t2r-cam-boost-old.txt)
// cam-boost (bot run B 22.73-22.93 'duel lock camera 180 whip in 3-4 frames'; log [58209]-[58211] camwrite displayed 10-16 deg per 60 fps frame =
// 600-960 deg/s while SP's whole render camera in a fight peaks at 336 deg/s, 5.6 deg per frame - melee_rec, see CAM2_YAW_RATE): the orbit spring and the look-at
// spring each ran to 600 deg/s at the same time (sum up to 1200). Now the look-at to SP's 336 and the orbit to half of it: the sum stays under ~500 deg/s, a
// 180 deg attacker behind us is framed in ~0.6 s instead of 0.3 s; omega unchanged. Tuning, labelled: SP's own number is 336 for the whole camera.
#define CAM2_BOOST_LOOK 5.8643f // rad/s = 336 deg/s
#define CAM2_BOOST_ORB 2.9322f  // rad/s = 168 deg/s
static float DtlCamYaw(float cur, float d)
{                               // cur = the camera's world heading (rad), d = spm_yaw's turn; returns the limited turn
    static double t;
    static int ok;
    static LONG gen37;
    if (g_ps37 && gen37 != g_s37Gen) {
        gen37 = g_s37Gen;
        ok = 0;
    }                           // ps37: a teleport - the look-at's absolute heading restarts on the live camera
    double now = GshNow();
    float dt = ok ? (float) (now - t) : 0.0f;
    if (!ok || dt > 0.25f || dt < 0.0f) {
        ok = 1;
        t = now;
        g_dtlc_yout = __builtin_remainderf(cur + d, 6.2831853f);
        g_dtlc_yv = 0.0f;
        return d;
    }
    if (dt < 0.001f)
        return __builtin_remainderf(g_dtlc_yout - cur, 6.2831853f);     // another copy of the same frame's camera
    t = now;
    float e = __builtin_remainderf(cur + d - g_dtlc_yout, 6.2831853f);
    if (__builtin_fabsf(e) > 0.6f)
        InterlockedIncrement(&g_dtlCamJumps); // (no xlog on the render thread)
    if (g_cam2On) {            // v5-cam2: critically damped spring with a speed cap (the old ease started every turn at its top speed: a whip on the first frame)
        int bo = GetTickCount64() < g_cam2BoostUntil;
        float om = bo ? CAM2_BOOST_OMEGA : CAM2_YAW_OMEGA, vm =
            bo ? (g_cam2BoostOld ? CAM2_BOOST_RATE : CAM2_BOOST_LOOK) :
            CAM2_YAW_RATE;
        // ps25 (uc2 A vistrace 43987 'yaw -180' -> 44019 '+119': 61 deg in 32 frames, 16 deg in 7 at its peak; SP per frame p90 1.0 / p99 2.2 deg,
        // 18 deg (p90) over 30 frames): inside the rig the look-at turns at SP's p99 per frame. t2r-ps25-cam-rig-old.txt = 240 deg/s.
        if (g_ps25[PS25_RIG] && !bo) {
            om = SPM25_LOOK_OMEGA;
            vm = SPM25_LOOK_RATE;
            // ps25b: a target far off the view (the partner beside / behind the camera at the latch: A 31196 'the engine turned us +52 deg', +125 deg by the
            // contact) is reached at up to the old 240 deg/s (4 deg/frame, SP's max 4.2) - at 2.2 deg/frame the fight stayed out of frame for a second
            float ea = __builtin_fabsf(e), s = (ea - 0.35f) / 0.5f;
            s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
            vm +=
                (CAM2_YAW_RATE - SPM25_LOOK_RATE) * s * s * (3.0f -
                                                             2.0f * s);
        }
        int n = (int) (dt * 240.0f) + 1;
        if (n > 32)
            n = 32;
        float h = dt / (float) n, x = e, v = g_dtlc_yv;
        for (int i = 0; i < n; i++) {
            v += (om * om * x - 2.0f * om * v) * h;
            v = v > vm ? vm : v < -vm ? -vm : v;
            x -= v * h;
        }
        g_dtlc_yv = v;
        g_dtlc_yout =
            __builtin_remainderf(g_dtlc_yout + (e - x), 6.2831853f);
    } else {
        g_dtlc_yv = 0.0f;
        g_dtlc_yout =
            __builtin_remainderf(g_dtlc_yout +
                                 DtlCamStep(e, dt, DTL_CAM_YAW_RATE),
                                 6.2831853f);
    }
    return __builtin_remainderf(g_dtlc_yout - cur, 6.2831853f);
}

static float DtlCamPitch(float d)
{
    static double t;
    static float out;
    static int ok;
    double now = GshNow();
    float dt = ok ? (float) (now - t) : 0.0f;
    if (!ok || dt > 0.25f || dt < 0.0f) {
        ok = 1;
        t = now;
        out = d;
        return d;
    }
    if (dt < 0.001f)
        return out;
    t = now;
    out += DtlCamStep(d - out, dt, DTL_CAM_PITCH_RATE);
    return out;
}

static float SpmYaw(const float *pos, const float *q)
{
    float w = g_spm.yw;
    g_cst.gated = -1;
    g_cst.c = -9.0f;            // (v5-cam4 snapshot: -1 = returned before the gates)
    if (!g_spm.ok || !(w > 0.002f) || !pos || !q)
        return 0.0f;
    float fx = 2.0f * (q[0] * q[2] + q[3] * q[1]), fz =
        1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]), fl =
        __builtin_sqrtf(fx * fx + fz * fz);
    if (!(fl > 0.2f))
        return 0.0f;
    float px = g_spm.px, pz = g_spm.pz, ex = g_spm.ex, ez = g_spm.ez;
    float al = ((px - pos[0]) * fx + (pz - pos[2]) * fz) / fl, pd =
        __builtin_sqrtf((px - pos[0]) * (px - pos[0]) +
                        (pz - pos[2]) * (pz - pos[2]));
    // v5-cam3 (v5-int11, B 'cam2: engine camera jump in duel framing (178.7 deg / 0.02 m in one frame)' x27 of 204 at >= 90 deg, B 60 fps clip
    // f1038 -> 1039 a hard cut to the opposite side): the forward-sign test below decided per frame whether the camera looks along +Z or -Z of its
    // quaternion. The engine's camera convention never changes, but the test flips whenever the lens is beside / at / past our head (collision pull-in,
    // the body ping-pong) - and the duel timeline's ABSOLUTE heading (dtl_cam_yaw: output = spring heading + (true forward - the guessed one)) turns a
    // wrong guess into exactly a 180 deg turn of the shown view at an unchanged lens. The sign is learnt now only where the test is unambiguous (|al| /
    // distance > 0.8) and kept; a later contradiction at that confidence is counted (g_spm_sgn_flips, 'spmelee cam: ... forward-sign' in dtl's summary).
    // (weird camera behavior when hitting once and then looking away from the enemy; A log: 'forward-sign test contradicted the
    // learnt sign 462 time(s) in 30 s' over 377 duel frames, and the code below never kept it - `sgn = sg` overwrote it): turning the view away from us puts
    // OUR body behind the lens, which the test reads as 'forward is +Z' with high confidence - so the sign flipped, the look-at / pitch aimed the wrong way
    // (about 180 deg off) and the spring swung the shown view toward / away from the enemy every few frames. The convention is the engine's and constant
    // (every A/B log: learnt -1, forward = -Z of the quaternion), so it is a constant now (SPM_CAM_FWD_SIGN); the test is only counted. A camera looking away
    // from us now has a NEGATIVE cosine and the look-at gates itself out (below), i.e. the player's look-away is honoured and the framing eases back in when
    // the view comes back (SP's melee camera never rotates the view at all).
    {
        float c0 = pd > 0.05f ? __builtin_fabsf(al) / pd : 0.0f;
        int sg = al < 0.0f ? -1 : 1;
        int fs = SpmFwdSign();        // ps25b: +1 inside the rig (the view axis is +Z), the earlier constant otherwise
        if (c0 > 0.8f && sg != fs)
            InterlockedIncrement(&g_spmSgnFlips);     // (= frames the view looks away from us)
        if (fs < 0) {
            fx = -fx;
            fz = -fz;
            al = -al;
        }
        g_cst.sgn = fs * 10 + sg;       // v5-cam4: learnt sign x 10 + this frame's test     // the camera's forward is -Z
    }
    // (the forward-sign test is unreliable when the camera sits beside the player (aim / lock-on cameras): the look-at fades out as the
    //  camera's forward stops pointing at the player, instead of flipping 180 deg between frames)
    // v5-cam3: with the duel timeline's absolute heading a gate that returned 0 showed the raw engine heading for that frame - a cut by the spring's whole
    // lead (up to 180 deg). Gated frames now hand the spring a zero turn (it eases back onto the engine heading) instead of skipping it.
    int gated = 0;
    // ps25 (uc2 A 45652 'cam2: duel framing shown (336 settled frames) - ... us +21.7 deg / partner +2.8 deg off the view centre (SP +11.8 / -11.1)',
    // camrec own root at x -0.37 / head -0.53 of the half-width, SP -0.18 / -0.24): the gate weighs the look-at by where the ENGINE camera's heading has
    // us - with the lens moved off the engine's line (pull-in, re-centre, orbit) that heading has us tens of degrees off, so the look-at stayed partial and
    // the view sat between the engine's aim (the partner centred) and the framing's. Inside the rig the lens and the heading are both ours: no gate (aiming
    // still ends the framing, spm_tick). t2r-ps25-cam-rig-old.txt = the gate.
    if (!g_ps25[PS25_RIG]) {
        float c = pd > 0.05f ? al / pd : 0.0f, f2 = (c - 0.5f) / 0.3f;
        f2 = f2 < 0.0f ? 0.0f : f2 > 1.0f ? 1.0f : f2;
        w *= f2 * f2 * (3.0f - 2.0f * f2);
    }
    if (!(w > 0.002f))
        gated = 1;
    float tx = px + 0.6f * (ex - px) - pos[0], tz =
        pz + 0.6f * (ez - pz) - pos[2];
    if (!gated) {
        float td = __builtin_sqrtf(tx * tx + tz * tz), f3 =
            (td - 0.5f) / 0.4f;
        if (!(f3 > 0.0f))
            gated = 1;
        else if (f3 < 1.0f)
            w *= f3 * f3 * (3.0f - 2.0f * f3);
    }                           // (was a hard 0.5 m cut - a snap)
    g_cst.gated = gated;
    g_cst.c = pd > 0.05f ? al / pd : -9.0f;
    if (gated)
        return g_dtlCamOn ? DtlCamYaw(__builtin_atan2f(fx, fz),
                                          0.0f) : 0.0f;
    // v5-feel2: SP frames the two bodies symmetric about the view centre (melee_rec: player +11.8 deg, NPC -11.1 deg off it; the old aim at the 60 %
    // point put us near the edge and the partner in the middle). Aim at the bisector of the directions to us and to the partner.
    float ap = __builtin_atan2f(px - pos[0], pz - pos[2]), ae =
        __builtin_atan2f(ex - pos[0], ez - pos[2]);
    float d = ap + (g_ps25[PS25_RIG] ? SPM25_AIM : 0.5f) * __builtin_remainderf(ae - ap, 6.2831853f) - __builtin_atan2f(fx, fz);        // (ps25: SP's own root -0.18 / enemy root +0.13 of the half-width)
    d = __builtin_remainderf(d, 6.2831853f);
    if (!(d == d))
        return 0.0f;
    d *= SPM_YAW_K * w;
    d = d > SPM_YAW_MAX ? SPM_YAW_MAX : d <
        -SPM_YAW_MAX ? -SPM_YAW_MAX : d;
    if (g_dtlCamOn)
        d = DtlCamYaw(__builtin_atan2f(fx, fz), d);
    return d;
}

// render thread (gsh_filter), after spm_yaw: the duel look-at PITCH (A's camera after B's breakout was above his head, B hardly visible).
// The camera tilts toward a point 60 % of the way to the opponent at chest height (root + 1.15 m), SPM_PITCH_K of the way, at most +-25 deg, with the duel
// yaw weight. The local-X rotation's sign is judged numerically (the camera's forward may be +Z or -Z of its quaternion, as in spm_yaw).
#define SPM_PITCH_K 1.0f
#define SPM_PITCH_MAX 0.70f
static void SpmFwd(const float *q, float *f)
{
    f[0] = 2.0f * (q[0] * q[2] + q[3] * q[1]);
    f[1] = 2.0f * (q[1] * q[2] - q[3] * q[0]);
    f[2] = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
}

static void SpmRotx(const float *q, float a, float *o)
{                               // q * rot(local X, a)
    float s = __builtin_sinf(a * 0.5f), c = __builtin_cosf(a * 0.5f);
    o[0] = q[3] * s + q[0] * c;
    o[1] = q[1] * c + q[2] * s;
    o[2] = q[2] * c - q[1] * s;
    o[3] = q[3] * c - q[0] * s;
}

static void SpmPitch(const float *pos, float *q)
{
    float w = g_spm.yw;
    g_cst.pitch = 0.0f;
    if (!g_spm.ok || !(w > 0.002f) || !pos || !q)
        return;
    float f[3];
    SpmFwd(q, f);
    float px = g_spm.px, pz = g_spm.pz;
    float sg = (float) SpmFwdSign();  // (was a per-frame test - it flipped when the view looked away from us, see spm_yaw; ps25b: +1 inside the rig)
    float tx = px + 0.6f * (g_spm.ex - px) - pos[0], tz =
        pz + 0.6f * (g_spm.ez - pz) - pos[2];
    float ty =
        g_spm.py + 0.6f * (g_spm.ey - g_spm.py) + SPM_LOOK_UP - pos[1],
        hd = __builtin_sqrtf(tx * tx + tz * tz);
    if (!(hd > 0.5f) || !(ty == ty))
        return;
    {
        float f3 = (hd - 0.5f) / 0.4f;
        if (f3 < 1.0f)
            w *= f3 * f3 * (3.0f - 2.0f * f3);
        float al = (px - pos[0]) * f[0] + (pz - pos[2]) * f[2], pd =
            __builtin_sqrtf((px - pos[0]) * (px - pos[0]) +
                            (pz - pos[2]) * (pz - pos[2])), c =
            pd > 0.05f ? sg * al / pd : 0.0f, f2 = (c - 0.5f) / 0.3f;
        f2 = f2 < 0.0f ? 0.0f : f2 > 1.0f ? 1.0f : f2;
        w *= f2 * f2 * (3.0f - 2.0f * f2);
        if (!(w > 0.002f))
            return;
    }
    // v5-feel2 (A/B v5-int8: pitch -20.6 to -24.5 deg vs SP -16.5): a look point at a fixed height pitched the view by the pair's distance - our duel pairs
    // stand 0.6-1.3 m apart with the lens ~1.15 m behind, SP's 1.55 m with 1.22 m, so the same point sat much steeper below. SP's pitch is the target now
    // (SPM_PITCH_SP, measured), plus the slope to the partner (its root above / below ours at 60 % of the way).
    float want =
        SPM_PITCH_SP +
        (g_s32.ctx ? S32_PITCH_CTX : S32_PITCH_FREE) * g_s32.p +
        __builtin_atan2f(0.6f * (g_spm.ey - g_spm.py), hd);
    want -= g_s40Pq;           // ps40 E1 (spm_tick): a lens raised over our head looks further down
    if (g_s32.p > 0.01f && g_s32.hok) { // ps35 (spm_tick's note at g_s25.up): the push looks UP as far as the higher head needs to sit at y +0.70
        float d0 =
            __builtin_sqrtf((px - pos[0]) * (px - pos[0]) +
                            (pz - pos[2]) * (pz - pos[2])), d1 =
            __builtin_sqrtf((g_spm.ex - pos[0]) * (g_spm.ex - pos[0]) +
                            (g_spm.ez - pos[2]) * (g_spm.ez - pos[2]));
        float fvh = g_mf_held > 30.0f ? g_mf_held : 70.0f, tv =
            __builtin_tanf(fvh * 0.5f * 0.0174533f) * 9.0f / 16.0f;
        float t0 =
            d0 > 0.3f ? __builtin_atan2f(g_s32.hy0 - pos[1], d0) : -1.5f,
            t1 =
            d1 > 0.3f ? __builtin_atan2f(g_s32.hy1 - pos[1], d1) : -1.5f;
        float need = (t0 > t1 ? t0 : t1) - __builtin_atanf(0.70f * tv);
        if (need == need && need > want)
            want += (need - want) * g_s32.p;
    }
    float fy = sg * f[1], cur =
        __builtin_asinf(fy > 1.0f ? 1.0f : fy < -1.0f ? -1.0f : fy), d =
        (want - cur) * SPM_PITCH_K * w;
    if (!(d == d))
        return;
    d = d > SPM_PITCH_MAX ? SPM_PITCH_MAX : d <
        -SPM_PITCH_MAX ? -SPM_PITCH_MAX : d;
    if (g_dtlCamOn)
        d = DtlCamPitch(d);   // duel timeline: the pitch turn eased + rate-limited
    if (__builtin_fabsf(d) < 0.003f)
        return;
    g_cst.pitch = d;
    float o[4], t[3];
    SpmRotx(q, d, o);
    SpmFwd(o, t);
    if (__builtin_fabsf(sg * t[1] - __builtin_sinf(want)) >
        __builtin_fabsf(fy - __builtin_sinf(want)))
        SpmRotx(q, -d, o);
    float l =
        __builtin_sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] +
                        o[3] * o[3]);
    if (l > 0.5f)
        for (int i = 0; i < 4; i++)
            q[i] = o[i] / l;
}

// v5-cam2, render thread (gsh_filter, after spm_pos, before the look-at): the duel camera ORBITS round us toward the side of the partner when the
// partner is more than ~30-50 deg off the camera -> us line (beside / behind us: a victim engaged from the side or the back, a partner circling). An
// in-place turn cannot frame someone standing next to the camera; SP's melee camera swings round the player. Only the part beyond ~0.7 rad is
// orbited (the look-at frames the rest in place); its weight is the look-at weight; a critically damped spring (fast on a victim engage); the arm
// comes in by up to 25 % while orbited (fewer walls in the swing - the arm is NOT ray-tested, open). pos = the (pulled-in) camera position, moved here;
// returns the orbit (rad about +Y) for the caller to apply to the rotation, 0 = none. The look-at's world heading is carried with it (no fight).
// ps25 (the spec block at the top of this file), render thread: SP's melee camera RIG. The old orbit (below, t2r-ps25-cam-rig-old.txt) left the lens
// where the ENGINE camera had it and only swung it by the part of the partner's bearing beyond ~40 deg, so the shoulder side was whatever side of the
// camera -> us line the partner happened to be on: uc2 A camrec 44010 'lat -0.09' -> 44013 '+0.05' -> 44025 '+0.43' -> 44043 '-0.07' (left of the body
// through swing 2), 43965 'lat +0.43 ... cyaw -37.6' -> 43989 'lat -1.25 back +0.58 ... cyaw +38.7' through the sprint attack; rec15 A 495 of 869 framed
// frames on the left; and its arm shrank by up to 25 % while orbited (uc2 B 39615-39642 'dist 1.00-1.06'). SP: the lens stands SPM25_AZ off the line
// player -> enemy on the side it was on when the melee camera came on, 0 side changes in 39 of 43 episodes, and turns 0.5 deg on a frame where the body
// turns 8. Here the lens's world azimuth round our (eased) pivot is a state of its own, W: it starts on the incoming lens, takes the side that lens is on
// in the line frame (right when within SPM25_SIDE_DEAD of the line) and follows 'behind the line + side x SPM25_AZ' on a critically damped spring capped
// at SPM25_ORB_RATE. The incoming lens is turned onto W by the look-at weight (0 for a fighter who has not swung), its radius is kept (spm_pos set SP's
// distance), and the look-at's absolute heading is carried with W's own step. Body yaw and the engine camera's azimuth do not enter at full weight.
static float Spm25Rig(float *pos, int reset)
{
    static double t;
    static int ok, sok, lok;
    static float W, WV, A, side = 1.0f, ldx, ldz = 1.0f;
    static double sw_t0;
    static float sw_W0, sw_da, sw_tg0, sw_T = S45S_T0;  // ps45 S1: the running side swing (sw_t0 0 = none)
    if (reset || !pos || !sok) {
        sw_t0 = 0.0;
        g_s45sUntil = 0.0;
        g_s46Until = 0.0;
    }
    if (reset || !pos) {
        ok = 0;
        sok = 0;
        lok = 0;
        A = 0.0f;
        g_s25.dok = 0;
        g_s25.w = 0.0f;
        g_s43qh.t = 0.0;
        g_s43qh.R = 0.0f;
        g_s43qh.k = 0.0f;
        return 0.0f;
    }
    static LONG gen37;
    static double cut_t, flip_t, wfull, init41;
    if (g_ps37 && gen37 != g_s37Gen) {
        gen37 = g_s37Gen;
        ok = 0;
        sok = 0;
        lok = 0;
        A = 0.0f;
        g_s25.dok = 0;
    }                           // ps37: a teleport - the kept side / azimuth / line are re-taken
    double now = GshNow();
    float dt = ok ? (float) (now - t) : 0.0f;
    float px = g_spm.px, pz = g_spm.pz, rx = pos[0] - px, rz =
        pos[2] - pz, rl = __builtin_sqrtf(rx * rx + rz * rz), w = g_spm.yw;
    if (!(rl == rl) || !(rl > 0.2f) || !(rl < 12.0f)) {
        g_cam2Orb = 0.0f;
        return 0.0f;
    }
    if (now < wfull && g_s39Yt)
        w = 1.0f;               // ps39: after a rig cut the lens stands on the rig's spot at once (the main thread's ease of the look-at weight may still be on its way)
    if (g_ps43q && (g_s43e.st == 1 || g_s43e.st == 3 || g_s43e.st == 4))
        w = 0.0f;               // ps43 Q1: the engage cut's spot is being planned - the rig starts when it is there (spm_tick holds the look-at weight at 0 too; ps46: 4 = the engage ease's path rays out)
    float az_e = __builtin_atan2f(rx, rz);      // the incoming lens's azimuth round our pivot
    if (!(ok && dt < 0.001f && dt >= 0.0f)) {   // (else: another copy of the same frame's camera - the same rig)
        if (!ok) {
            ok = 1;
            sok = 0;
            dt = 0.0f;
        } else if (dt > 0.25f || dt < 0.0f) {
            WV = 0.0f;
            dt = 0.0f;
        }                       // a hitch: the lens stays where the rig has it
        t = now;
        float ox = g_spm.ex - px, oz = g_spm.ez - pz, ol =
            __builtin_sqrtf(ox * ox + oz * oz);
        if (ol == ol && ol > 0.3f && ol < 12.0f) {
            ldx = ox / ol;
            ldz = oz / ol;
            lok = 1;
        }                       // the line us -> partner (closer than 0.3 m: the last one)
        if (!(w > 0.002f) || !lok) {
            sok = 0;
            A = 0.0f;
            g_s25.dok = 0;
            g_s41.rok = 0;
            g_s43qh.t = 0.0;
            g_s43qh.R = 0.0f;
            g_s43qh.k = 0.0f;
        } else {
            float lat_r = rx * -ldz + rz * ldx; // + = the incoming lens is on the right of the line (the right of a heading f = (-fz, fx))
            int cut42 = 0, sw45 = 0;
            LONG fv44 = 0;
            if (!sok) {
                sok = 1;
                W = az_e;
                WV = 0.0f;
                side = lat_r >= -SPM25_SIDE_DEAD ? 1.0f : -1.0f;
                init41 = now;
            } else if (g_ps39[PS39_SWAP]
                       && (fv44 = InterlockedExchange(&g_s39_flip, 0))) {
                side = -side;
                flip_t = now;
                sw45 = fv44 == 3;
                cut42 = !sw45 && ((g_s42Side && g_s32.p > 0.3f)
                                  || fv44 == 2);
            }                   /* ps45 S1: 3 = the eased swing (s45s_tick) */
            /* ps44 R7: 2 = the head floor's swap, a cut */
            // ps39 C6 (spm_tick): the kept side is blocked, the other is clear
            float az40 = SPM25_AZ + ((g_ps40[PS40_SIDE] && g_s32.ctx) || g_s42Side ? (S40_AZ_CTX - SPM25_AZ) * g_s32.p : 0.0f);        // ps40 E3 (spm_tick): side-on for a wall / cover takedown; ps42 H1: for every takedown the rig holds
            float tg40 = __builtin_atan2f(-ldx, -ldz) + side * az40;
            float e = __builtin_remainderf(tg40 - W, 6.2831853f), w0;
            if (!(e == e))
                e = 0.0f;
            // ps42 H1: the side-on two-shot's ONE correction (spm_tick: the victim's head hidden from the kept side for 0.25 s) is a CUT to the other side -
            // a walk of 160 deg at the rig's rate would pass straight behind our own back for over a second
            if (cut42) {
                W = __builtin_remainderf(W + e, 6.2831853f);
                WV = 0.0f;
                e = 0.0f;
                cut_t = now;
                wfull = now + 0.4;
                gen37 = InterlockedIncrement(&g_s37Gen);
                InterlockedIncrement(&g_s42_fcuts);
            }
            if (sw45) {         // ps45 S1 (the note at PS45_SIDE): the way round s45s_tick tested; the swing itself runs at the spring below
                float da = e;
                int dsg = g_s45sDsg;
                if (dsg > 0 && da < 0.0f)
                    da += 6.2831853f;
                else if (dsg < 0 && da > 0.0f)
                    da -= 6.2831853f;
                sw_T = g_s45sT;
                sw_T = sw_T < 0.2f ? 0.2f : sw_T > 0.6f ? 0.6f : sw_T;
                sw_t0 = now;
                sw_W0 = W;
                sw_da = da;
                sw_tg0 = tg40;
                wfull = now + (double) sw_T + 0.15;
                g_s45sUntil = now + (double) sw_T + 0.02;
                InterlockedIncrement(&g_s45sN);
            }
            // ps39 C7 (rec38a: A 8189 'ps25 rig - line frame: engine base -0.22 m beside / -1.50 m behind' (the partner 0.9 m away: BETWEEN us and the lens,
            // clip A 1.87-2.13 its coat over the frame); A 14027 'engine base +0.37 / -1.21 ... shown +0.42 / -1.19' after 14005 'YAW SNAP ... -165 deg' at a swing
            // start facing away (clip A 99.20-99.80: own head ghost, partner out 0.6 s); B 18835 'engine base -1.09 / -0.63' at the hit latch 18825 (clip B
            // 143.3-144.1)): the rig starts on the incoming lens and walks to its spot at SP's 1.9 deg a frame - right for a lens that is behind us, 0.6-1.5 s
            // through / past the partner for one that stands on the partner's side. More than S39_FAR_RAD off its spot the rig takes the spot on ONE cut
            // (look-at weight full, the look-at and the continuity stages restart: g_s37_gen), at most once a second; and within 0.8 m of the partner's axis
            // the walk runs up to 3x faster (the lens is never left in the partner's body).
            // ps42 H4: at the engage a lens more than S42_ENG_RAD (60 deg) off the rig's spot, or standing on the partner's side of our body (ahead of
            // us along the line = the front of our own fighter in the shot), takes the spot on the cut - was 100 deg (rec41a B 17421 above; A 8120 / B 18301
            // 'ps39 rig cut' one frame after the latch are the same rule at 100 deg). Under that: the ps40 engage ease. t2r-ps42-cam-engage-old.txt = 100 deg.
            int e42 = g_ps42[PS42_ENG] && (__builtin_fabsf(e) > S42_ENG_RAD
                                           || __builtin_sinf(W) * ldx +
                                           __builtin_cosf(W) * ldz > 0.0f);
            int e44 = 0;        // ps44 R6 (the note at S44E_RAD): 40 deg off the spot, or the partner's head out of the shown frame (> 45 deg off the incoming view)
            if (g_ps44[PS44_ENG] && !e42 && g_ps42[PS42_ENG]) {
                float vx = -rx / rl, vz = -rz / rl, qx = ox - rx, qz = oz - rz, ql = __builtin_sqrtf(qx * qx + qz * qz);        // the incoming view (lens -> our pivot) vs lens -> the partner
                int oof = ol == ol && ol > 0.3f && ql > 0.3f && (vx * qx + vz * qz) / ql < 0.707f;      // (> 45 deg off: outside the frame's half width)
                if (__builtin_fabsf(e) > S44E_RAD || oof) {
                    e44 = 1;
                    g_s44eDeg = __builtin_fabsf(e) * 57.29578f;
                    g_s44eOof = oof;
                }
            }
            // ps43 Q1 (the block at S43Q_*): the cut's spot is the main thread's plan from the two live roots (azimuth, radius, height, side) - the incoming
            // lens only decides WHETHER to cut, and not even that after a teleport; no second rig cut for S43Q_GAP
            int q1 = g_ps43q, qok = q1 && g_s43e.st == 2
                && GetTickCount64() - g_s43e.ms < S43Q_FRESH_MS, qtp = qok
                && g_s43e.tp;
            if (g_ps39[PS39_LATCH] && g_s39Yt && (!g_ps41 || now - init41 < 0.5) && (__builtin_fabsf(e) > S39_FAR_RAD || e42 || e44 || qtp) && /* ps41 G5: only at engage, never mid-duel */
                now - cut_t > (q1 ? S43Q_GAP : 1.0) && now - flip_t > 1.5) {
                int ez46 = 0, cw46 = -1;
                float da46 = 0.0f;      // ps46 E1 (the note at S46_*): the engage EASES onto the planned spot when its path was clear
                if (g_ps46) {
                    if (!q1 || !qok || g_s46.ms != g_s43e.ms)
                        cw46 = 5;
                    else if (qtp)
                        cw46 = 6;
                    else if (!(rl >= S46_RMIN && rl <= S46_RMAX))
                        cw46 = 7;
                    else if (g_s46.pick < 0)
                        cw46 = g_s46.why >= 0 ? g_s46.why : 5;
                    else {
                        float Ws = az_e + w * __builtin_remainderf(W - az_e, 6.2831853f), pd = g_s46.pda;       // (the lens where it is shown now: A stays continuous at w = 1)
                        da46 =
                            __builtin_remainderf(g_s43e.az - Ws,
                                                 6.2831853f);
                        if (pd > 0.0f && da46 < 0.0f)
                            da46 += 6.2831853f;
                        else if (pd < 0.0f && da46 > 0.0f)
                            da46 -= 6.2831853f;
                        if (!(da46 == da46)
                            || __builtin_fabsf(da46 - pd) > S46_DRIFT)
                            cw46 = 8;
                        else {
                            ez46 = 1;
                            W = Ws;
                        }
                    }
                }
                if (ez46) {
                    side = g_s43e.side;
                    tg40 = __builtin_atan2f(-ldx, -ldz) + side * az40;
                    sw_T = g_s46.pT;
                    sw_T = sw_T < 0.2f ? 0.2f : sw_T > 0.6f ? 0.6f : sw_T;
                    sw_t0 = now;
                    sw_W0 = W;
                    sw_da = da46;
                    sw_tg0 = tg40;
                    wfull = now + (double) sw_T + 0.15;
                    g_s46Until = now + (double) sw_T + 0.02;
                    g_s43qh.R0 = g_s43e.R;
                    g_s43qh.up = g_s43e.up;
                    g_s43e.c_fb = g_s43e.fb;
                    g_s43qh.tin = sw_T;
                    g_s43qh.t = now;
                    g_s43qh.wh =
                        g_s43e.fb >=
                        3 ? now + (double) sw_T + S43Q_WHOLD : 0.0;
                    g_s46.cdeg = __builtin_fabsf(da46) * 57.29578f;
                    g_s46.cT = sw_T;
                    g_s46.cR0 = rl;
                    g_s46.cR1 = g_s43e.R;
                    g_s46.cnb = g_s46.pnb;
                    e = 0.0f;
                    WV = 0.0f;
                    cut_t = now;
                    w = 1.0f;
                    g_s43Ecut = GetTickCount64();
                    InterlockedIncrement(&g_s46.n);
                } else {
                    if (g_ps46 && cw46 >= 0) {
                        g_s46.cwhy = cw46;
                        InterlockedIncrement(&g_s46.nc);
                    }
                    g_s43qh.tin = 0.0f;
                    if (!(__builtin_fabsf(e) > S39_FAR_RAD) && e42)
                        InterlockedIncrement(&g_s42_ecuts);
                    if (e44 && !e42 && !(__builtin_fabsf(e) > S39_FAR_RAD)
                        && !qtp)
                        InterlockedIncrement(&g_s44eCuts);
                    if (q1) {
                        g_s43e.c_rl = rl;
                        g_s43e.c_e = e;
                        g_s43e.c_tp = qtp;
                    }
                    W = __builtin_remainderf(W + e, 6.2831853f);
                    WV = 0.0f;
                    e = 0.0f;
                    cut_t = now;
                    wfull = now + 0.4;
                    if (q1) {
                        if (qok) {
                            side = g_s43e.side;
                            W = __builtin_remainderf(g_s43e.az,
                                                     6.2831853f);
                            g_s43qh.R0 = g_s43e.R;
                            g_s43qh.up = g_s43e.up;
                            g_s43e.c_fb = g_s43e.fb;
                            g_s43qh.wh =
                                g_s43e.fb >= 3 ? now + S43Q_WHOLD : 0.0;
                            tg40 =
                                __builtin_atan2f(-ldx, -ldz) + side * az40;
                            e = g_s43e.fb >=
                                3 ? 0.0f : __builtin_remainderf(tg40 - W,
                                                                6.2831853f);
                            if (!(e == e))
                                e = 0.0f;
                        } else {
                            float r0 = g_spm.bt > 0.5f
                                && g_spm.bt < 2.0f ? g_spm.bt : SPM25_DIST;
                            g_s43qh.R0 = r0 < S43Q_RMIN ? S43Q_RMIN : r0;
                            g_s43qh.up = 0.0f;
                            g_s43qh.wh = 0.0;
                            g_s43e.c_fb = -1;
                        }
                        g_s43qh.t = now;
                        InterlockedIncrement(&g_s43e.cuts);
                    }
                    gen37 = InterlockedIncrement(&g_s37Gen);
                    g_spm.yw = 1.0f;
                    w = 1.0f;
                    InterlockedIncrement(&g_s39_cuts);
                    g_s43Ecut = GetTickCount64();      // (ps43: no side swap for S43_SWAP_MS after it - spm_tick)
                }
            }
            w0 = W;
            // ps40 E8 (rec39a A 15272-15332 'ps25 rig - ... shown +0.36 / 1.21', '-0.13 / 1.25', '+0.11 / 1.25', side RIGHT, weight 1.00; clip
            // 113.4-115.4: the opponent hidden behind our own back): the two fighters circled, the line turned at ~1.5 rad/s and a critically damped spring
            // trails a steady turn by 2 x rate / omega = 43 deg - the lens stood straight behind us instead of 0.80 m beside. The line's own turn rate
            // (low-passed 0.3 s, capped at the rig's rate) leads the spring by that lag.
            {
                static float ltg, tv40;
                static int ltok;
                if (ltok && dt > 0.001f) {
                    float dv =
                        __builtin_remainderf(tg40 - ltg, 6.2831853f) / dt;
                    dv = dv > SPM25_ORB_RATE ? SPM25_ORB_RATE : dv <
                        -SPM25_ORB_RATE ? -SPM25_ORB_RATE : dv;
                    tv40 +=
                        (dv - tv40) * (1.0f - __builtin_expf(-dt / 0.3f));
                } else
                    tv40 = 0.0f;
                ltg = tg40;
                ltok = 1;
                if (g_ps40[PS40_LAG] && tv40 == tv40)
                    e += 2.0f * tv40 / SPM25_ORB_OMEGA;
            }
            float rc39 = SPM25_ORB_RATE, om40 = SPM25_ORB_OMEGA;
            if (GetTickCount64() < g_s40Boost) {
                rc39 = 5.0f;
                om40 = 11.0f;
            }                   // ps40 E6 (spm_tick): the engage ease - on its spot in ~0.35 s
            if (g_ps39[PS39_LATCH]) {
                float qx = px + __builtin_sinf(W) * rl - g_spm.ex, qz =
                    pz + __builtin_cosf(W) * rl - g_spm.ez, dq =
                    __builtin_sqrtf(qx * qx + qz * qz);
                if (dq < 0.8f)
                    rc39 *=
                        1.0f + 2.0f * (dq <
                                       0.4f ? 1.0f : (0.8f - dq) / 0.4f);
            }
            if (g_ps43q && now < g_s43qh.wh) {
                e = 0.0f;
                WV = 0.0f;
            }                   // ps43 Q1: the raised fallback spot is held (both shoulders are blocked)
            if (sw_t0 > 0.0) {  // ps45 S1: the side swing - W along the tested way round, smoothstep, following the line's own turn since its start
                float x = (float) (now - sw_t0) / sw_T, dtg =
                    __builtin_remainderf(tg40 - sw_tg0, 6.2831853f);
                if (!(dtg == dtg))
                    dtg = 0.0f;
                if (!(x >= 0.0f) || x >= 1.0f) {
                    W = sw_W0 + sw_da + dtg;
                    sw_t0 = 0.0;
                    e = __builtin_remainderf(tg40 - W, 6.2831853f);
                    if (!(e == e))
                        e = 0.0f;
                } else {
                    float sx = x * x * (3.0f - 2.0f * x);
                    W = sw_W0 + (sw_da + dtg) * sx;
                    e = 0.0f;
                }
                WV = 0.0f;
            }
            int n = (int) (dt * 240.0f) + 1;
            if (n > 32)
                n = 32;
            float h = dt / (float) n;
            for (int i = 0; i < n && dt > 0.0f; i++) {
                WV += (om40 * om40 * e - 2.0f * om40 * WV) * h;
                WV = WV > rc39 ? rc39 : WV < -rc39 ? -rc39 : WV;
                W += WV * h;
                e -= WV * h;
            }
            g_dtlc_yout = __builtin_remainderf(g_dtlc_yout + w * (W - w0), 6.2831853f); // the look-at's absolute heading turns with the lens
            W = __builtin_remainderf(W, 6.2831853f);
            A = w * __builtin_remainderf(W - az_e, 6.2831853f);
            if (!(A == A))
                A = 0.0f;
            g_s25.side = side;
            g_s25.w = w;
            g_s25.eb_r = lat_r;
            g_s25.eb_b = -(rx * ldx + rz * ldz);
            g_s43qh.R = 0.0f;
            g_s43qh.k = 0.0f;   // ps43 Q1: the cut's radius / height, eased to the chain's own over S43Q_HOLD
            if (g_s43qh.t > 0.0) {
                float el43 = (float) (now - g_s43qh.t), ti43 =
                    g_s43qh.tin > 0.0f ? g_s43qh.tin : 0.0f, k43;
                if (ti43 > 0.0f && el43 < ti43) {
                    k43 = el43 / ti43;
                    k43 =
                        k43 >=
                        0.0f ? k43 * k43 * (3.0f - 2.0f * k43) : 0.0f;
                    if (k43 < 1e-4f)
                        k43 = 1e-4f;
                }               // ps46 E1: eased in with the swing
                else {
                    k43 = 1.0f - (el43 - ti43) / S43Q_HOLD;
                    k43 = k43 > 0.0f
                        && k43 <=
                        1.0f ? k43 * k43 * (3.0f - 2.0f * k43) : -1.0f;
                }
                if (k43 > 0.0f && g_ps43q) {
                    g_s43qh.k = k43;
                    g_s43qh.R = rl + (g_s43qh.R0 - rl) * k43;
                } else
                    g_s43qh.t = 0.0;
            }
            if (g_ps41o && w > 0.985f && g_spm.w > 0.985f) {    // ps41 step 0: the rig's own radius
                if (!g_s41.rok || now - g_s41.t > 0.1) {
                    g_s41.R = g_s43qh.R > 0.2f ? g_s43qh.R : rl;
                    g_s41.rok = 1;
                } else {
                    float d = rl - g_s41.R, mo = S41_R_OUT * dt, mi =
                        S41_R_IN * dt;
                    if (d > mo || d < -mi)
                        InterlockedIncrement(&g_s41.n_r);
                    d = d > mo ? mo : d < -mi ? -mi : d;
                    g_s41.R += d;
                }
                g_s41.t = now;
                InterlockedIncrement(&g_s41.n_own);
            } else
                g_s41.rok = 0;
        }
        g_cam2Orb = A;
    }
    int o41 = g_ps41o && g_s41.rok && now - g_s41.t < 0.05
        && g_s41.R > 0.2f;
    int q1o = !o41 && g_s43qh.R > 0.2f, q1h = g_s43qh.k > 0.0f; // ps43 Q1: the engage cut's own radius / height
    if (A == 0.0f && !o41 && !q1o && !q1h)
        return 0.0f;
    float sn = __builtin_sinf(A), cs = __builtin_cosf(A), nx =
        rx * cs + rz * sn, nz = -rx * sn + rz * cs;
    if (o41) {
        float s41 = g_s41.R / rl;
        nx *= s41;
        nz *= s41;
        rl = g_s41.R;
    } else if (q1o) {
        float s43 = g_s43qh.R / rl;
        nx *= s43;
        nz *= s43;
        rl = g_s43qh.R;
    }
    pos[0] = px + nx;
    pos[2] = pz + nz;
    if (q1h) {
        float dy43 = g_spm.py + g_s25.up + g_s43qh.up - pos[1];
        if (dy43 == dy43 && dy43 > -3.0f && dy43 < 3.0f)
            pos[1] += dy43 * g_s43qh.k;
    }
    g_s25.sh_r = nx * -ldz + nz * ldx;
    g_s25.sh_b = -(nx * ldx + nz * ldz);
    g_s25.dx = nx / rl;
    g_s25.dz = nz / rl;
    g_s25.dok = 1;
    return A != 0.0f ? A : (q1o || q1h) ? 1e-6f : A;    // (the caller takes the moved lens only on a non-zero turn)
}

static float Cam2Orbit(float *pos)
{
    static double t;
    static float A, V, lastA;
    static int ok;
    double now = GshNow();
    float dt = ok ? (float) (now - t) : 0.0f;
    if (!g_cam2On || !g_spm.ok || !pos) {
        ok = 0;
        A = V = lastA = 0.0f;
        g_cam2Orb = 0.0f;
        Spm25Rig(pos, 1);
        return 0.0f;
    }
    if (g_ps25[PS25_RIG]) {
        ok = 0;
        A = V = lastA = 0.0f;
        return Spm25Rig(pos, 0);
    }                           // ps25: SP's rig owns the lens's azimuth (above)
    if (!(ok && dt < 0.001f && dt >= 0.0f)) {   // (else: another copy of the same frame's camera - same orbit)
        if (!ok || dt > 0.25f || dt < 0.0f) {
            ok = 1;
            V = 0.0f;
            dt = 0.0f;
        }
        t = now;
        float px = g_spm.px, pz = g_spm.pz, cx = px - pos[0], cz =
            pz - pos[2], ox = g_spm.ex - px, oz = g_spm.ez - pz;
        float T = 0.0f;
        if (MduEngaged() && g_spm.yw > 0.002f && cx * cx + cz * cz > 0.64f
            && ox * ox + oz * oz > 0.09f) {
            float e =
                __builtin_remainderf(__builtin_atan2f(ox, oz) -
                                     __builtin_atan2f(cx, cz), 6.2831853f),
                x = __builtin_fabsf(e);
            float f = x < 0.5f ? 0.0f : x < 0.9f ? (x - 0.5f) * (x - 0.5f) / 0.8f : x - 0.7f;   // C1-continuous dead zone (the look-at frames the first ~35 deg)
            T = (e < 0.0f ? -f : f) * g_spm.yw;
            if (!(T == T))
                T = 0.0f;
        }
        int bo = GetTickCount64() < g_cam2BoostUntil;
        float om = bo ? CAM2_BOOST_OMEGA : 9.0f, vm =
            bo ? (g_cam2BoostOld ? CAM2_BOOST_RATE : CAM2_BOOST_ORB) :
            CAM2_YAW_RATE;
        int n = (int) (dt * 240.0f) + 1;
        if (n > 32)
            n = 32;
        float h = dt / (float) n;
        for (int i = 0; i < n && dt > 0.0f; i++) {
            V += (om * om * (T - A) - 2.0f * om * V) * h;
            V = V > vm ? vm : V < -vm ? -vm : V;
            A += V * h;
        }
        if (__builtin_fabsf(A) < 1e-4f && T == 0.0f) {
            A = 0.0f;
            V = 0.0f;
        }
        g_dtlc_yout = __builtin_remainderf(g_dtlc_yout + (A - lastA), 6.2831853f);      // the look-at's absolute heading moves with the orbit
        lastA = A;
        g_cam2Orb = A;
    }
    if (A == 0.0f)
        return 0.0f;
    float px = g_spm.px, pz = g_spm.pz, rx = pos[0] - px, rz =
        pos[2] - pz, sn = __builtin_sinf(A), cs = __builtin_cosf(A);
    float a = __builtin_fabsf(A) / 1.6f, sc =
        1.0f - 0.25f * (a > 1.0f ? 1.0f : a);
    pos[0] = px + sc * (rx * cs + rz * sn);
    pos[2] = pz + sc * (-rx * sn + rz * cs);
    return A;
}
