// Remote players' footsteps, 2013 Factions range (notes in this header). Included by t2r-ext.c before t2r_tick.
//
// Why: a remote player is an NPC puppet on our screen, so the engine plays its steps as NPC foley (sfx-mlb-heel/toe-<surface>,
// sfx-info-light category foley-npc: mix level -5..-8 dB, the SP stealth-design step levels). Captures:
// a SPRINTING enemy only became audible at ~10 m (clear at ~5 m), while his jump grunts / landings carried from 25-35 m (distances
// from B's on-screen height in A's 960x540 view, D_camera ~ 1000 / height px, cross-checked with a ~5.7 m/s sprint; the steps sit
// 30-40 dB under a grunt at 30 m in the recording). The data: sfx-info-light of those events = {flags, category, half floats
// [0, 0, -4, 4], [55, 0.8, 0, 0.6], [1, 0.01]} - the same family as TLOU1's own (PS3 audio.bin: Joel's heel [60, 0.6, 0, 0.7],
// mlb heel [55, 1.0, 0, 0.7]), so the event data is not a cut-down MP curve; the NPC foley level / bus is what keeps them faint. In 2013
// Factions you hear an enemy run from far off (TLOU1 MP listen mode reaches 35 m, *player-listen-mode-settings-mp*; SP AI noise
// radii walk 4 / run 6 / sprint 15 / crouch 0.5 m, *noise-settings-default*: sprint >> run >> walk >> crouch).
//
// What: for every remote puppet we detect its foot plants (ankle joints vs its root, the animation the sender plays; cadence from the
// speed if the joints do not answer) and play TLOU2's own MLB step (tools/rip_t2_footsteps.py: srf-stone-asphalt heeltoe-sprint /
// heeltoe-run / heel-vslow / stealth-heel + a srf-dirt-hard layer) through the ext PCM mixer, positional (pan from the camera, as
// every 3D ext sound). Gait from the puppet's ground speed and head height:
//          gain   full within   silent at
//   sprint 0.34   3 m           35 m
//   run    0.26   3 m           28 m
//   walk   0.13   2 m           16 m
//   crouch 0.045  1 m            7 m     (near silent; prone: nothing)
// roll-off ((dmax - d) / (dmax - d0))^2. Inside 5 m the engine's own NPC steps are audible too, so ours fade to `near` (0.5) of
// their level at 1.5 m (smoothstep 1.5..5 m) instead of doubling the near field. A peer with Agility II (2013: "nearly impossible
// to hear while moving") is played at 15 %.
//
// DEFAULT OFF since the root fix: the data build retunes audio.bin mix-default's foley-npc / foley-gear-npc rows
// (dc_data_patches.make_audio_mix_overlay), so the engine's own per-surface, per-gait NPC steps carry at the 2013 range; this
// PCM layer would double them. Opt in (fallback / comparison): t2r-pcm-footsteps.txt next to the exe (checked every 5 s).
// t2r-footsteps.txt (re-read every 5 s), lines:
//   sprint|run|walk|crouch <gain> <full m> <silent m>
//   near <keep 0..1> <from m> <to m>
//   speed <walk m/s> <run m/s> <sprint m/s>        (defaults 0.5 2.6 4.6)
//   crouch-head <m>                                 (head above root below this = crouched; default 1.25, prone below 0.6)
//   cadence 1                                       (force the speed cadence, no ankle detection)
// Log prefix "footsteps:".

#include "footsteps.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl fs_blob\nfs_blob:\n.incbin \"footsteps.pcm\"\n.text\n");
extern const int16_t fs_blob[];

static const char *const FS_NAME[FS_TIERS] =
    { "sprint", "run", "walk", "crouch" };
static struct {
    float gain, d0, dmax;
} g_fs_tier[FS_TIERS] = {
    { 0.34f, 3.0f, 35.0f }, { 0.26f, 3.0f, 28.0f }, { 0.13f, 2.0f, 16.0f },
        { 0.045f, 1.0f, 7.0f }
};

static float g_fsNearKeep = 0.5f, g_fs_near_lo = 1.5f, g_fs_near_hi =
    5.0f;
static float g_fsVWalk = 0.5f, g_fs_v_run = 2.6f, g_fs_v_sprint =
    4.6f, g_fs_crouch_head = 1.25f;
static int g_fsForceCadence;

#define FS_PRONE_HEAD 0.60f
#define FS_LIFT 0.055f          // ankle this far above its planted height = foot in the air (armed)
#define FS_PLANT 0.025f         // back within this of the planted height = foot plant
#define FS_REFRACT_MS 170.0     // one foot cannot plant twice faster than this
#define FS_ANKLE_FAIL 45        // frames without both ankles before the speed cadence takes over (retried every 5 s)
#define FS_AGILITY2 0.0f        // their Agility II (MSG_SK_SYNC byte 8, effective level): silent at every gait (2013; it was 15 % before, with a loud sprint)

static struct fs_slot {
    uint64_t h;
    int have;
    float last[3];
    double last_ms;
    float v, vy;                // smoothed ground speed / vertical speed (m/s)
    float headh;                // head above root (m)
    int head_ok;
    float base[2];              // planted ankle height above root per foot
    int armed[2];
    double step_ms[2], any_ms;
    int ankles;                 // 1 = ankle plants, 0 = speed cadence
    int ankle_miss;
    double ankle_retry_ms;
    float phase;                // cadence fallback
    int tier, last_wave;
    unsigned steps, played, tier_n[FS_TIERS];
    float dsum;
    double sum_ms;
} g_fs[T2R_MAX_PEERS];

static void FsConfig(void)
{
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (at && now - at < 5000)
        return;
    at = now;
    FILE *f = ExeSideOpen("t2r-footsteps.txt", "r");
    if (!f)
        return;
    char ln[160];
    while (fgets(ln, sizeof ln, f)) {
        char k[24];
        float a, b, c;
        int n = sscanf(ln, "%23s %f %f %f", k, &a, &b, &c);
        if (n < 2)
            continue;
        int t = -1;
        for (int i = 0; i < FS_TIERS; i++)
            if (!strcmp(k, FS_NAME[i]))
                t = i;
        if (t >= 0 && n == 4 && a >= 0.0f && a <= 2.0f && b >= 0.0f
            && c > b) {
            g_fs_tier[t].gain = a;
            g_fs_tier[t].d0 = b;
            g_fs_tier[t].dmax = c;
        } else if (!strcmp(k, "near") && n == 4 && a >= 0.0f && a <= 1.0f
                   && c > b) {
            g_fsNearKeep = a;
            g_fs_near_lo = b;
            g_fs_near_hi = c;
        } else if (!strcmp(k, "speed") && n == 4 && a > 0.0f && b > a
                   && c > b) {
            g_fsVWalk = a;
            g_fs_v_run = b;
            g_fs_v_sprint = c;
        } else if (!strcmp(k, "crouch-head") && a > 0.3f && a < 2.0f)
            g_fs_crouch_head = a;
        else if (!strcmp(k, "cadence"))
            g_fsForceCadence = a != 0.0f;
    }
    fclose(f);
    static int logged;
    if (!logged++)
        Xlog("footsteps: t2r-footsteps.txt: sprint %.3f %.0f-%.0f m, run %.3f %.0f-%.0f m, walk %.3f %.0f-%.0f m, crouch %.3f %.0f-%.0f m, near %.2f %.1f-%.1f m, speeds %.1f/%.1f/%.1f, crouch head %.2f%s", (double) g_fs_tier[0].gain, (double) g_fs_tier[0].d0, (double) g_fs_tier[0].dmax, (double) g_fs_tier[1].gain, (double) g_fs_tier[1].d0, (double) g_fs_tier[1].dmax, (double) g_fs_tier[2].gain, (double) g_fs_tier[2].d0, (double) g_fs_tier[2].dmax, (double) g_fs_tier[3].gain, (double) g_fs_tier[3].d0, (double) g_fs_tier[3].dmax, (double) g_fsNearKeep, (double) g_fs_near_lo, (double) g_fs_near_hi, (double) g_fsVWalk, (double) g_fs_v_run, (double) g_fs_v_sprint, (double) g_fs_crouch_head, g_fsForceCadence ? ", cadence forced" : "");
}

// 2013-like level of a step `tier` heard at d metres (before the near-field share)
static float FsDistGain(int tier, float d)
{
    float d0 = g_fs_tier[tier].d0, dm = g_fs_tier[tier].dmax;
    if (d <= d0)
        return 1.0f;
    if (d >= dm)
        return 0.0f;
    float t = (dm - d) / (dm - d0);
    return t * t;
}

static float FsNear(float d)
{
    if (d >= g_fs_near_hi)
        return 1.0f;
    if (d <= g_fs_near_lo)
        return g_fsNearKeep;
    float t = (d - g_fs_near_lo) / (g_fs_near_hi - g_fs_near_lo);
    t = t * t * (3.0f - 2.0f * t);
    return g_fsNearKeep + (1.0f - g_fsNearKeep) * t;
}

static void FsStep(uint32_t slot, struct fs_slot *s, int foot,
                    const char *how)
{
    int tier = s->tier;
    float g0, pan = 0.0f, d = 0.0f, bp[3];
    const char *why = "";
    s->steps++;
    if (!SfxSpatial(s->h, NULL, &g0, &pan, &d, bp, &why)) {
        static int nl;
        if (LOGCAP(nl, 10))
            Xlog("footsteps: slot %u step dropped (%s)", slot, why);
        return;
    }
    float g = g_fs_tier[tier].gain * FsDistGain(tier, d) * FsNear(d);
    if (SkPeer(slot, SK_AGILITY) >= 2)
        g *= FS_AGILITY2;       // 2013 Agility II: "nearly impossible to hear while moving"
    s->dsum += d;
    if (g < 0.003f)
        return;
    uint32_t r =
        (uint32_t) (GetTickCount64() * 2654435761u) ^ (uint32_t) (slot *
                                                                  977u +
                                                                  s->
                                                                  steps *
                                                                  131u);
    int c = FS_TIER[tier].count, k = (int) (r % (uint32_t) c);
    if (c > 1 && FS_TIER[tier].first + k == s->last_wave)
        k = (k + 1 + (int) ((r >> 8) % (uint32_t) (c - 1))) % c;
    int w = FS_TIER[tier].first + k;
    s->last_wave = w;
    g *= 0.9f + 0.2f * (float) ((r >> 16) & 0xff) / 255.0f;     // take-to-take level spread
    float l, rr;
    SfxLr(g, 1.0f, pan, &l, &rr);
    int ok =
        PcmPlayLr(fs_blob + FS_WAVE[w].off, (int) FS_WAVE[w].n, l, rr,
                    FS_RATE);
    if (ok)
        s->played++;
    static int nl;
    if (nl < 60) {
        nl++;
        Xlog("footsteps: slot %u %s step (%s, foot %d) at %.1f m, speed %.1f m/s, head %.2f m -> gain %.3f pan %+.2f%s", slot, FS_NAME[tier], how, foot, (double) d, (double) s->v, (double) s->headh, (double) g, (double) pan, ok ? "" : " (mixer full)");
    }
}

static void FootstepsTick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    static uint64_t on_at;
    static int on, said;
    if (!said && MatchSubstate() == 6) {
        said = 1;
        Xlog("footsteps: remote steps = the engine's NPC foley at the retuned mix-default (data build); PCM layer %s (t2r-pcm-footsteps.txt)", FeelFlag("t2r-pcm-footsteps.txt", &on_at, &on, 300) ? "ON" : "off");
    }
    if (!FeelFlag("t2r-pcm-footsteps.txt", &on_at, &on, 300)
        || MatchSubstate() != 6) {
        for (int i = 0; i < T2R_MAX_PEERS; i++)
            g_fs[i].have = 0;
        return;
    }
    FsConfig();
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    double now = PsyncMs();
    for (uint32_t slot = 0; slot < T2R_MAX_PEERS; slot++) {
        struct fs_slot *s = &g_fs[slot];
        uint8_t *peer = slot != me ? ((fn_peer) T2rPeer) (slot) : NULL;
        uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
        if (!h || !peer[4] || peer[5] || g_puppetDeadHere[slot]
            || g_peerDowned[slot] || GxBusySlot(slot)) {
            s->have = 0;
            continue;
        }
        float lb[16];
        const float *loc = PuppetLocator(h, lb);
        if (!loc || !__builtin_isfinite(loc[0])
            || !__builtin_isfinite(loc[1])
            || !__builtin_isfinite(loc[2])) {
            s->have = 0;
            continue;
        }
        if (s->h != h) {        // a new puppet (spawn / respawn / reconnect)
            memset(s, 0, sizeof *s);
            s->h = h;
            s->ankles = 1;
            s->last_wave = -1;
            s->sum_ms = now;
        }
        if (!s->have) {
            memcpy(s->last, loc, 12);
            s->last_ms = now;
            s->have = 1;
            s->v = s->vy = 0.0f;
            s->armed[0] = s->armed[1] = 0;
            s->base[0] = s->base[1] = 1e9f;
            s->phase = 0.0f;
            continue;
        }
        float dt = (float) ((now - s->last_ms) * 0.001);
        if (dt < 0.004f)
            continue;
        if (dt > 0.25f)
            dt = 0.25f;
        float dx = loc[0] - s->last[0], dz = loc[2] - s->last[2], dy =
            loc[1] - s->last[1];
        float vi = __builtin_sqrtf(dx * dx + dz * dz) / dt, vyi = dy / dt;
        memcpy(s->last, loc, 12);
        s->last_ms = now;
        if (vi > 15.0f) {
            s->v = 0.0f;
            s->armed[0] = s->armed[1] = 0;
            continue;
        }                       // teleport / respawn snap
        float a = dt / 0.15f;
        if (a > 1.0f)
            a = 1.0f;
        s->v += (vi - s->v) * a;
        s->vy += (vyi - s->vy) * a;
        // stance: head height above the root (every 4th frame)
        if (!((g_frame + slot) % 4)) {
            float hd[3];
            s->head_ok = GxJointPos(h, "headb", hd);
            if (s->head_ok)
                s->headh = hd[1] - loc[1];
        }
        int prone = s->head_ok && s->headh < FS_PRONE_HEAD;
        int crouch = s->head_ok && !prone && s->headh < g_fs_crouch_head;
        int tier =
            crouch ? FS_CROUCH : s->v >
            g_fs_v_sprint * SdMul()? FS_SPRINT : s->v >
            g_fs_v_run * SdMul()? FS_RUN : FS_WALK;
        s->tier = tier;
        int moving = s->v > g_fsVWalk && !prone && __builtin_fabsf(s->vy) < 2.0f;     // airborne (jump / drop): the engine's land sounds
        // ankle plants
        int use_ankles = s->ankles && !g_fsForceCadence;
        if (!s->ankles && !g_fsForceCadence
            && now - s->ankle_retry_ms > 5000.0) {
            s->ankles = 1;
            s->ankle_miss = 0;
            use_ankles = 1;
        }
        if (use_ankles) {
            float la[3], ra[3];
            int ok = GxJointPos(h, "l_ankle", la)
                && GxJointPos(h, "r_ankle", ra);
            if (!ok) {
                if (++s->ankle_miss >= FS_ANKLE_FAIL) {
                    s->ankles = 0;
                    s->ankle_retry_ms = now;
                    static int nl;
                    if (LOGCAP(nl, 8))
                        Xlog("footsteps: slot %u ankle joints do not answer - speed cadence", slot);
                }
            } else {
                s->ankle_miss = 0;
                float hf[2] = { la[1] - loc[1], ra[1] - loc[1] };
                for (int f = 0; f < 2; f++) {
                    if (!__builtin_isfinite(hf[f]) || hf[f] < -1.0f
                        || hf[f] > 1.5f)
                        continue;
                    if (s->base[f] > 1e8f)
                        s->base[f] = hf[f];
                    if (hf[f] < s->base[f])
                        s->base[f] = hf[f];
                    else {
                        float k = dt / 1.5f;
                        if (k > 1.0f)
                            k = 1.0f;
                        s->base[f] += (hf[f] - s->base[f]) * k;
                    }           // follows slopes / stance slowly
                    if (hf[f] >
                        s->base[f] + (crouch ? FS_LIFT * 0.7f : FS_LIFT))
                        s->armed[f] = 1;
                    else if (s->armed[f] && hf[f] < s->base[f] + FS_PLANT
                             && now - s->step_ms[f] > FS_REFRACT_MS) {
                        s->armed[f] = 0;
                        s->step_ms[f] = now;
                        if (moving) {
                            s->any_ms = now;
                            FsStep(slot, s, f, "ankle");
                        }
                    }
                }
            }
        } else if (moving) {    // speed cadence: sprint 0.30 s, run 0.36 s, walk 0.55 s, crouch 0.60 s per step
            static const float IV[FS_TIERS] =
                { 0.30f, 0.36f, 0.55f, 0.60f };
            s->phase += dt / IV[tier];
            if (s->phase >= 1.0f) {
                s->phase -= 1.0f;
                if (s->phase > 1.0f)
                    s->phase = 0.0f;
                s->any_ms = now;
                FsStep(slot, s, (int) (s->steps & 1), "cadence");
            }
        } else
            s->phase = 0.5f;    // the first step comes half a period after starting to move
        if (moving)
            s->tier_n[tier]++;
        if (now - s->sum_ms > 15000.0) {        // summary per puppet
            if (s->steps)
                Xlog("footsteps: slot %u last 15 s: %u steps (%u played), mean %.1f m, %s plants, frames sprint %u run %u walk %u crouch %u", slot, s->steps, s->played, (double) (s->dsum / (float) s->steps), s->ankles && !g_fsForceCadence ? "ankle" : "cadence", s->tier_n[0], s->tier_n[1], s->tier_n[2], s->tier_n[3]);
            s->steps = s->played = 0;
            s->dsum = 0.0f;
            memset(s->tier_n, 0, sizeof s->tier_n);
            s->sum_ms = now;
        }
    }
}
