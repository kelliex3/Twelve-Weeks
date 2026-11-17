// Prone pull-out victim feel. SP reference (re/sp-sandbox-runs/nr-1031, NR scar forest, Ellie pulled out by an NPC and killed, 01:12):
//   the MUSIC does nothing special: the combat loop keeps playing through the drag and the struggle; only the death blow turns it into the set's
//   transition-down (decay), empty 4.4 s later. The drama is everything else:
//   grab (drag-sync start)  camera-shake-melee-hit-b (0.1 / 0.5 s) + melee-standing-finisher-strong rumble, vox-eff-gen-meatshield-grabbed-combat +150 ms
//   struggle (window)       override-render-settings *zgame-player-damage-struggle* (prio 32767, every frame) + DOF on the attacker, struggle grunts
//   blows on the victim     camera-shake-melee-hit-b again, *zgame-player-damage-struggle-spike*, vox-eff-gen-fight-take-lrg-blnt
//   fail / death            sfx-ellie-death-pullout-fail + vox-eff-gen-player-death-pullout-fail, screen-blood-splatter, music -> the set's decay
// Ours: grab = camera kick + grabbed grunt, the struggle screen while the drag runs, fail = the death-pullout sfx + vox + decay of the playing set;
// escape = the loop stays (it never stopped). Requests to feature_combat_music via g_ms_pull (1 grab = hold, 2 escape, 3 fail = decay).
// t2r-no-pullout-sting.txt next to the exe: off. t2r-pullout-riser.txt = the earlier grab riser / end tag instead of SP's hold / decay. Logs 'pullout sting: ...'.
// Included from t2r-ext.c after shiv_sting.c.
#define PUS_HOLD_MS 900
static uint64_t g_pusSpikeUntil;

static uint64_t g_pus_t0, g_pus_say_at, g_pus_blow_at, g_pus_vox_n;

static int g_pus_on, g_pus_dof_on, g_pus_esc;

// SP timeline (re/sp-melee-run-1031.md "Prone pull-out on the player"), times from the drag-sync start (SP +0.80 s) = our grab:
//   +0.13 s vox grabbed-combat (done) | +0.90 s vox-eff-gen-infected-struggle | +2.50 s vox-eff-gen-player-choked-enter | +2.95 s struggle clips: force-camera-dof-focus on the puller
//   every frame | blow (+5.0 s in SP, here the fail pair's blow moment): vox take-lrg-blnt + camera-shake-melee-hit-b (0.1, 2.5 s) + *zgame-player-damage-struggle-spike* | escape +0.3 s vox infected-struggle-escape
// Each part has an opt-out file next to the exe (GetFileAttributesA every ~2 s): t2r-pullout-camswing-off.txt (melee_pullout.c), t2r-pullout-dof-off.txt, t2r-pullout-spike-off.txt, t2r-pullout-vox-off.txt.
#define PUS_SWING_FOV0 0.9498f  // SP lens 70 at the start of the swing / 73.7 (= the game fov, XCAM_T2_DEFAULT_FOV 73.74) at its end
#define PUS_DOF_AT_MS 2950
#define PUS_SPIKE_MS 900        // the spike override holds ~0.9 s after the blow
static int PusOpt(const char *f, int *v, uint64_t *at)
{
    return PulFlag(f, v, at);
}


// (force-camera-dof-focus(<puppet handle>, ...) crashed on its first call - exe+133da80 locator read on the handle, int99 [11902].
//  Opt-in t2r-pullout-dof-on.txt until the native's object argument is known.)
static int PusDofOff(void)
{
    static int v = -1;
    static uint64_t at;
    return !PusOpt("t2r-pullout-dof-on.txt", &v, &at);
}


static int PusSpikeOff(void)
{
    static int v = -1;
    static uint64_t at;
    return PusOpt("t2r-pullout-spike-off.txt", &v, &at);
}


static int PusVoxOff(void)
{
    static int v = -1;
    static uint64_t at;
    return PusOpt("t2r-pullout-vox-off.txt", &v, &at);
}


static int PusSwingOff(void)
{
    static int v = -1;
    static uint64_t at;
    return PusOpt("t2r-pullout-camswing-off.txt", &v, &at);
}


static int PullstingOff(void)
{
    
static int off = -1;
    
if (off < 0) {
        FILE *f = ExeSideOpen("t2r-no-pullout-sting.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
        if (off)
            Xlog("pullout sting: off (t2r-no-pullout-sting.txt)");
    }
    
return off;

}


static int PullstingRiser(void)
{
    return GetFileAttributesA("t2r-pullout-riser.txt") !=
        INVALID_FILE_ATTRIBUTES;
}


// the drag is over (no verdict, death sting, match left the playing state): the combat music is free again, the struggle screen stops with the next frame
static void PullstingCut(int fade_ms, const char *why)
{
    
(void) fade_ms;
    
if (!g_pus_on)
        return;
    
g_pus_on = 0;
    g_pus_say_at = 0;
    g_pus_blow_at = 0;
    g_pus_vox_n = 0;
    g_pus_esc = 0;
    g_xcamFovMul = 0.0f;
    
if (g_pus_dof_on) {
        g_pus_dof_on = 0;
        Xlog("pullout sting: struggle depth of field off");
    }
    
Xlog("pullout sting: over (%s) %llu ms in", why,
          (unsigned long long) (GetTickCount64() - g_pus_t0));

} 
static void PullstingGrab(void)
{
    
if (PullstingOff() || MatchSubstate() != 6)
        return;
    
g_pus_t0 = GetTickCount64();
    
g_pus_on = 1;
    
g_msPull = 1;
    
g_msHoldUntil = g_pus_t0 + PUS_HOLD_MS;
    
g_pus_say_at = g_pus_t0 + 150;
    g_pus_blow_at = 0;
    g_pus_vox_n = 0;
    g_pus_esc = 0;
    g_pus_dof_on = 0;
    
MfkAdd(1.7f, 0.0f, 1.0f, 0.0f);   // SP: camera-shake-melee-hit-b on the grab (the victim's heavy-hit kick)
    MfkRumble("melee-standing-finisher-strong-rumble-settings");       // SP trace: start-rumble at the drag sync
    Xlog("pullout sting: grab (drag on us) - camera kick, grabbed grunt in 150 ms, struggle screen on%s", PullstingRiser()? ", combat riser (t2r-pullout-riser.txt)" : ", music held");

}


// Ellie's grunts were audible in the pull-out: the SP victim lines (infected-struggle, player-choked-enter, struggle-escape,
// player-death-pullout-fail) exist only in the SP player voices - on a male look the engine resolves them to Ellie. A male look says the
// generic combat efforts (its own voice) instead; a female look keeps SP's lines (fem_effort_sid swaps the take by kind as before).
static void PusSay(const char *sp_line, const char *male_line, int vxe)
{
    // Her lines were still audible in death on a female look: every MP look takes the generic lines - only Ellie owns the SP ones
    int fem = 0;
    DmSay(SID_PLAYER, fem ? sp_line : male_line, vxe);
    static int nl;
    if (LOGCAP(nl, 20))
        Xlog("pullout sting: victim line %s (%s look)",
             fem ? sp_line : male_line, fem ? "female" : "male");
}

static void PullstingResult(int ok)
{
    
if (PullstingOff() || MatchSubstate() != 6)
        return;
    
g_msPull = ok ? 2 : 3;
    
if (!ok) {
        
UiSfx("sfx-ellie-death-pullout-fail");
        
            // Grunts carried over to the death screen: the death groan here ran from the verdict, 1 s before the punch,
            // on into the death screen. Now the verdict is silent (the sfx only) and the blow says ONE death cry (pullsting tick). t2r-pullout-verdict-vox.txt = old.
            if (GetFileAttributesA("t2r-pullout-verdict-vox.txt") !=
                INVALID_FILE_ATTRIBUTES)
            PusSay("vox-eff-gen-player-death-pullout-fail",
                    "vox-eff-gen-death-med", VXE_DEATH);
    
}
    
if (ok)
        g_pus_esc = (int) (GetTickCount64() - g_pus_t0) + 300;  // SP: vox-eff-gen-infected-struggle-escape 0.3 s after the success clip starts
    Xlog("pullout sting: %s - %s",
         ok ? "success (counter VALID)" : "fail (counter FAILED)",
         ok ? "combat loop stays" :
         "death-pullout sfx + vox, music to the set's decay");

} 

// the outcome pair started on us: a fail's blow lands blow_s from now (the attacker's fail blow, SP +5.8 s): spike + camera kick + take-lrg-blnt then
static void PullstingOutcome(int ok, float blow_s)
{
    
if (!g_pus_on || ok)
        return;
    
if (blow_s < 0.05f)
        blow_s = 0.05f;
    
g_pus_blow_at = GetTickCount64() + (uint64_t) (blow_s * 1000.0f);
    
Xlog
        ("pullout sting: fail pair - the blow lands in %.0f ms (spike %s)",
         (double) (blow_s * 1000.0f),
         PusSpikeOff()? "OFF (t2r-pullout-spike-off.txt)" : "on");

} 

// per frame (pul_tick): hold the tier, the grabbed grunt, the struggle screen (SP calls it every frame; no call = cleared)
static void PullstingTick(void)
{
    
if (!g_pus_on)
        return;
    
if (g_msDead) {
        PullstingCut(0, "dead");
        return;
    }
    
if (MatchSubstate() != 6) {
        PullstingCut(0, "match left the playing state");
        return;
    }
    
if (!g_pulv.on) {
        PullstingCut(0, "pull-out over");
        return;
    }
    
uint64_t now = GetTickCount64();
    
g_msHoldUntil = now + PUS_HOLD_MS;
    
if (g_pus_say_at && now >= g_pus_say_at) {
        g_pus_say_at = 0;
        DmSay(SID_PLAYER, "vox-eff-gen-meatshield-grabbed-combat",
               VXE_PAIN);
    }
    
        // SP vox on the victim (player vox events, absent ones stay silent) and the puller's effort on its puppet (SP's NPC dialog ai-enemy-melee-grab-player-prone* has no MP voice: its fight efforts instead)
        if (!PusVoxOff()) {
        
uint64_t el = now - g_pus_t0, ph =
            ((uint64_t) PuppetHandle((uint32_t) g_pulv.slot));
        
if (g_pus_vox_n == 0 && el >= 900) {
            g_pus_vox_n = 1;
            PusSay("vox-eff-gen-infected-struggle",
                    "vox-eff-gen-fight-take-med", VXE_PAIN);
            if (ph)
                DmSay(ph, "vox-eff-gen-fight-give-grab", VXE_GIVE);
            Xlog("pullout sting: vox infected-struggle on us, puller effort give-grab");
        }
        
if (g_pus_vox_n == 1 && el >= 2500) {
            g_pus_vox_n = 2;
            PusSay("vox-eff-gen-player-choked-enter",
                    "vox-eff-gen-fight-take-lrg", VXE_PAIN);
            if (ph)
                DmSay(ph, "vox-eff-gen-fight-give-lrg-long", VXE_GIVE);
            Xlog("pullout sting: vox player-choked-enter on us, puller effort give-lrg-long");
        }
        
if (g_pus_esc && el >= (uint64_t) g_pus_esc) {
            g_pus_esc = 0;
            PusSay("vox-eff-gen-infected-struggle-escape",
                    "vox-eff-gen-fight-give-struggle-finish", VXE_GIVE);
            Xlog("pullout sting: vox infected-struggle-escape on us");
        }
    
}
    
        // the SP lens: fov 70 -> the game fov over the swing, held (victim xcam only, xcam_track_eval reads g_xcam_fov_mul)
    {
        
float u = (float) (now - g_pus_t0) / (PUS_SWING_S * 1000.0f);
        u = u > 1.0f ? 1.0f : u;
        u = u * u * (3.0f - 2.0f * u);
        
g_xcamFovMul =
            PusSwingOff()? 0.0f : PUS_SWING_FOV0 + (1.0f -
                                                      PUS_SWING_FOV0) * u;
    
} 
        // the fail blow: take-lrg-blnt, camera-shake-melee-hit-b (0.1 / 2.5 s: a long strong kick) and the spike screen for PUS_SPIKE_MS
        if (g_pus_blow_at && now >= g_pus_blow_at) {
        
g_pus_blow_at = 0;
        
if (!PusSpikeOff()) {
            g_pusSpikeUntil = now + PUS_SPIKE_MS;
            MfkAdd(2.2f, 0.0f, 1.0f, 0.0f);
            if (!PusVoxOff())
                DmSay(SID_PLAYER,
                       GetFileAttributesA("t2r-pullout-verdict-vox.txt") !=
                       INVALID_FILE_ATTRIBUTES ?
                       "vox-eff-gen-fight-take-lrg-blnt" :
                       "vox-eff-gen-death-melee-blunt-large",
                       GetFileAttributesA("t2r-pullout-verdict-vox.txt") !=
                       INVALID_FILE_ATTRIBUTES ? VXE_PAIN : VXE_DEATH);
            Xlog("pullout sting: blow - camera kick, spike screen, take-lrg-blnt");
        }
    
}
    
if (g_pusSpikeUntil && now < g_pusSpikeUntil) {
        
uint64_t r2 =
            ImmRsCall(FNV64("*zgame-player-damage-struggle-spike*"),
                        32767, 1.0f);
        
static int ns;
        if (LOGCAP(ns, 3))
            Xlog("pullout sting: struggle spike (override-render-settings *zgame-player-damage-struggle-spike* -> %llx)", (unsigned long long) r2);
    
}
    
        // struggle depth of field: SP calls force-camera-dof-focus(<attacker>, *zoom-camera-dof-settings-struggles*, 8b29524641f00000, 1.0f, 1.0f) every frame from the struggle clips (+2.95 s)
        if (!PusDofOff() && g_pulv.out == 0
            && now - g_pus_t0 >= PUS_DOF_AT_MS) {
        
uint64_t ph = PuppetHandle((uint32_t) g_pulv.slot);
        
uint64_t a[5] =
            { ph, FNV64("*zoom-camera-dof-settings-struggles*"),
   0x8b29524641f00000ull, ImmF2u(1.0f), ImmF2u(1.0f) };
        
uint64_t rd =
            ph ?
            DcCall(MoCamSid
                    ("force-camera-dof-focus",
                     FNV64("force-camera-dof-focus")), 5, a) : ~0ULL;
        
if (!g_pus_dof_on) {
            g_pus_dof_on = 1;
            Xlog("pullout sting: struggle depth of field on (force-camera-dof-focus puller %llx, *zoom-camera-dof-settings-struggles* -> %llx%s)", (unsigned long long) ph, (unsigned long long) rd, ph ? "" : ", no puppet - nothing called");
        }
    
}
    
static int nl;
    
uint64_t r =
        ImmRsCall(FNV64("*zgame-player-damage-struggle*"), 32767, 1.0f);
    
if (LOGCAP(nl, 3))
        Xlog("pullout sting: struggle screen (override-render-settings *zgame-player-damage-struggle* -> %llx)", (unsigned long long) r);

} 
