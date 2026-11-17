// Downed music (the closer to bleedout, the darker it gets; built from Nora's interrogation themes).
// While WE are downed (g_downed) TLOU2 SP's Nora interrogation set (music1/613_joelfail_torture_nora_*: an adaptive escalating set, 120 bpm, every loop a whole number
// of 4 s phrases) plays 2D through the PCM mixer, stereo as a hard-panned voice pair, at the options menu's MUSIC volume (exmus_user_volume).
// The bleed-out fraction f (the same clock as the downed HUD bar: elapsed minus the paused time over g_bleed_secs) picks the STAGE:
//   0 hall pulse  (B_HallPulseLoop, dark low uneasy)   f < 0.30
//   1 QT loop 1   (D_QTLoop1)                           0.30 <= f < 0.52
//   2 QT loop 2   (E_QTLoop2)                           0.52 <= f < 0.76
//   3 QT loop 3   (E2_QTLoop3, the loud one)            0.76 <= f   (the last ~3.6 s of a 15 s bleed, held)
// and a smooth level path tgt(f) (dnm_db: -27 dBFS RMS at f 0 rising to -10.5 from f 0.9) sets the gain of the playing stage (rms-matched, so the rise is gradual
// across the stage changes). Stage changes are BEAT-SYNCED: the new stage's voice starts at the music's next bar line (2 s) with a 150 ms equal-power crossfade, at
// the same phase of the 4 s pattern (its offset = the playing voice's position, read in the same mixer lock). A hit while downed cuts the bleed time: f jumps, and
// the new stage comes in at the next BEAT (0.5 s) with a 100 ms fade. A teammate reviving us (bleed paused) holds f, so the stage loop just keeps looping.
// Loops wrap seamlessly: every stage wave carries DM_TAIL samples of its own start appended; once the voice passes the loop point a second voice starts at the same
// phase and the two (identical audio) cross-fade linearly over 100 ms.
//   start : 0.3 s after the down, 250 ms fade-in; the combat music is already cut by the down's rogue-player-death cut and is kept cut (hold pushed every frame);
//           it comes back 1.5 s after the music ends.
//   end   : revived -> 0.8 s fade-out; executed (paired move) -> 0.3 s (the execution victim cue takes over: exec_music.c); dead / match left the playing state -> 0.4 s.
//   death : (an ending tail plays into the death screen and the death stinger ducks under it) when a downed player dies
//           (the bleed-out runs out, or a shot / blow finishes them) the set's own ending, F_QTClimax (pt4 oneshot, 7 s: a hit that decays), starts on the NEXT BEAT of the
//           playing stage (at most 0.5 s) while the stage fades out under it over 300 ms, and rings on into the death screen. The death sting (death_ui.c) is ducked
//           under it frame by frame (down to 15 %, back up as the tail decays); the death drone's own sidechain (death_theme.c) already ducks under any long 2D voice.
//           An execution keeps its own handover (no tail).
// Client-side, only we hear it. Opt-out: t2r-no-downed-music.txt next to the exe. Logs 'downed music: ...'.
// Included from t2r-ext.c after exec_music.c (uses exmus_user_volume, pcm_play_voice, g_mixv, feel_flag); dnmus_tick runs from execmus_tick.
#include "downed_music.h"
#include "death_sting.h"        // DS_N (the death sting's wave, defined in death_ui.c)
extern const int16_t ds_blob[];
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl dm_blob\ndm_blob:\n.incbin \"downed_music.pcm\"\n.text\n");
extern const int16_t dm_blob[];
#define DNM_LEVEL 1.0f          // x user music volume (the stages are stored at their loudest level on the path, so the gain never exceeds 1)
#define DNM_START_MS 300
#define DNM_START_FADE_MS 250
#define DNM_XF_MS 150           // stage change (equal power)
#define DNM_XF_HIT_MS 100       // stage change after a hit
#define DNM_WRAP_MS 100         // loop wrap (linear: identical audio)
#define DNM_LOOK 0.05f          // stage look-ahead (fraction) so the bar line just before a threshold already switches
static const float DNM_BAR_S = 2.0f, DNM_BEAT_S = 0.5f;
static const float DNM_STAGE_F[DM_STAGES] = { 0.0f, 0.30f, 0.52f, 0.76f };

static struct {
    int ca, cb, oa, ob;         // current voice pair / the pair fading out (-1 none)
    int cs, os;                 // their stages
    uint64_t xf_t0, first_t0, down_ms, dbg, quick_until, f_t;
    int running, fading, xf, xf_ms, xf_lin, switches;
    float f, tgt_prev;
    int dying, tail_a, tail_b;  // dying: the ending tail is waiting for its beat; tail voices
    uint64_t tail_at, tail_t0;
    int duck_cap;
    float duck_gl, duck_gr;     // the death sting's own gains while we duck it
} g_dnm = {.ca = -1,.cb = -1,.oa = -1,.ob = -1,.tail_a = -1,.tail_b = -1 };

static int DnmOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-no-downed-music.txt", &at, &off, 120);
}

static const int16_t *DnmSrc(int stage, int k)
{
    return dm_blob + DM_OFF[stage] + (size_t) k *(DM_LEN[stage] + DM_TAIL);
}

static int DnmN(int stage)
{
    return DM_LEN[stage] + DM_TAIL;
}

static int DnmLivePair(int a, int b, int stage)
{
    if (a < 0 || b < 0 || g_mixState != 1)
        return 0;
    EnterCriticalSection(&g_mixCs);
    int on = g_mixv[a].on && g_mixv[a].src == DnmSrc(stage, 0)
        && g_mixv[b].on && g_mixv[b].src == DnmSrc(stage, 1);
    LeaveCriticalSection(&g_mixCs);
    return on;
}

static int DnmLive(void)
{
    return DnmLivePair(g_dnm.ca, g_dnm.cb, g_dnm.cs)
        || DnmLivePair(g_dnm.oa, g_dnm.ob, g_dnm.os);
}

static void DnmGain(int a, int b, int stage, float g)
{
    if (a < 0 || b < 0)
        return;
    EnterCriticalSection(&g_mixCs);
    if (g_mixv[a].src == DnmSrc(stage, 0))
        g_mixv[a].tgl = g;
    if (g_mixv[b].src == DnmSrc(stage, 1))
        g_mixv[b].tgr = g;
    LeaveCriticalSection(&g_mixCs);
}

static void DnmKill(int *a, int *b, int stage)
{
    if (*a >= 0 && *b >= 0 && g_mixState == 1) {
        EnterCriticalSection(&g_mixCs);
        if (g_mixv[*a].src == DnmSrc(stage, 0))
            g_mixv[*a].on = 0;
        if (g_mixv[*b].src == DnmSrc(stage, 1))
            g_mixv[*b].on = 0;
        LeaveCriticalSection(&g_mixCs);
    }
    *a = *b = -1;
}

static float DnmSs(float a, float b, float f)
{
    float x = (f - a) / (b - a);
    x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    return x * x * (3.0f - 2.0f * x);
}

// the level path (dBFS RMS of the music) over the bleed-out fraction: dark and low at first, a gradual rise, the peak held from f 0.9
static float DnmDb(float f)
{
    static const float P[][2] =
        { { 0.0f, -27.0f }, { 0.30f, -23.0f }, { 0.52f, -18.5f }, { 0.70f,
                                                                   -14.5f
                                                                   },
        { 0.80f, -11.5f }, { 0.90f, -10.5f }, { 1.0f, -10.5f } };
    if (f <= 0.0f)
        return P[0][1];
    for (int i = 1; i < 7; i++)
        if (f <= P[i][0]) {
            float t = (f - P[i - 1][0]) / (P[i][0] - P[i - 1][0]);
            return P[i - 1][1] + (P[i][1] - P[i - 1][1]) * t;
        }
    return -10.5f;
}

static float DnmStageGain(int stage, float f, float vol)
{
    float g = powf(10.0f, (DnmDb(f) - DM_RMS_DB[stage]) / 20.0f);
    return (g > 1.0f ? 1.0f : g) * vol;
}

static int DnmStageOf(float f)
{
    int s = 0;
    for (int i = 1; i < DM_STAGES; i++)
        if (f >= DNM_STAGE_F[i])
            s = i;
    return s;
}

// a voice pair of `stage` starting at sample offset `off` (the mixer plays from any offset), gain 0 (the caller ramps it). Caller holds g_mix_cs.
static int DnmPairLocked(int stage, double off, int *ra, int *rb)
{
    int a = -1, b = -1;
    for (int v = 0; v < MIX_VOICES && b < 0; v++) {
        if (g_mixv[v].on)
            continue;
        if (a < 0)
            a = v;
        else
            b = v;
    }
    if (b < 0)
        return 0;
    int vv[2] = { a, b };
    for (int k = 0; k < 2; k++) {
        int v = vv[k];
        g_mixv[v].src = DnmSrc(stage, k);
        g_mixv[v].n = DnmN(stage);
        g_mixv[v].pos = off;
        g_mixv[v].step = (double) DM_RATE / MIX_RATE;
        g_mixv[v].played = 0;
        g_mixv[v].fadeN = g_dnm.fading ? 1 : 0;
        g_mixv[v].fadeLeft = 0;
        g_mixv[v].gl = g_mixv[v].tgl = 0.0f;
        g_mixv[v].gr = g_mixv[v].tgr = 0.0f;
        g_mixv[v].dyn = 0;
        g_mixv[v].ent = 0;
        g_mixv[v].base = 1.0f;
        memset(g_mixv[v].wp, 0, 12);
        g_mixv[v].on = 1;
    }
    *ra = a;
    *rb = b;
    return 1;
}

static void DnmReset(void)
{
    g_dnm.ca = g_dnm.cb = g_dnm.oa = g_dnm.ob = -1;
    g_dnm.running = g_dnm.fading = g_dnm.xf = 0;
}

static void DnmCut(int fade_ms, const char *why)
{
    g_dnm.running = 0;
    if (!DnmLive()) {
        DnmReset();
        return;
    }
    if (g_dnm.fading && fade_ms > 0)
        return;
    g_dnm.fading = fade_ms > 0;
    EnterCriticalSection(&g_mixCs);
    int p[4] = { g_dnm.ca, g_dnm.cb, g_dnm.oa, g_dnm.ob }, st[4] =
        { g_dnm.cs, g_dnm.cs, g_dnm.os, g_dnm.os };
    for (int k = 0; k < 4; k++) {
        int v = p[k];
        if (v < 0 || !g_mixv[v].on
            || g_mixv[v].src != DnmSrc(st[k], k & 1))
            continue;           // slot already reused by another sound
        if (fade_ms <= 0)
            g_mixv[v].on = 0;
        else if (!g_mixv[v].fadeN) {
            g_mixv[v].fadeLeft = g_mixv[v].fadeN =
                fade_ms * MIX_RATE / 1000;
        }
    }
    LeaveCriticalSection(&g_mixCs);
    Xlog("downed music: %s - fade-out %d ms (%llu ms into it, stage %d, bleed fraction %.2f)", why, fade_ms, (unsigned long long) (GetTickCount64() - g_dnm.first_t0), g_dnm.cs, (double) g_dnm.f);
    if (fade_ms <= 0)
        DnmReset();
    g_msHoldUntil = GetTickCount64() + (uint64_t) fade_ms + 1500;     // the combat music comes back after it
    g_msResumeAt = g_msHoldUntil;
}

// the ending tail: SP's F_QTClimax (one stereo pair after the stages in dm_blob), full music level
static int DnmTailStart(float vol)
{
    const int16_t *l = dm_blob + DM_END_OFF, *r = l + DM_END_N;
    int a = -1, b = -1;
    if (g_mixState != 1 || vol <= 0.0f)
        return 0;
    EnterCriticalSection(&g_mixCs);
    for (int v = 0; v < MIX_VOICES && b < 0; v++) {
        if (g_mixv[v].on)
            continue;
        if (a < 0)
            a = v;
        else
            b = v;
    }
    if (b >= 0) {
        const int16_t *src[2] = { l, r };
        int vv[2] = { a, b };
        for (int k = 0; k < 2; k++) {
            int v = vv[k];
            g_mixv[v].src = src[k];
            g_mixv[v].n = DM_END_N;
            g_mixv[v].pos = 0.0;
            g_mixv[v].step = (double) DM_RATE / MIX_RATE;
            g_mixv[v].played = 0;
            g_mixv[v].fadeN = 0;
            g_mixv[v].fadeLeft = 0;
            g_mixv[v].gl = g_mixv[v].tgl = k ? 0.0f : vol;
            g_mixv[v].gr = g_mixv[v].tgr = k ? vol : 0.0f;
            g_mixv[v].dyn = 0;
            g_mixv[v].ent = 0;
            g_mixv[v].base = 1.0f;
            memset(g_mixv[v].wp, 0, 12);
            g_mixv[v].on = 1;
        }
        g_dnm.tail_a = a;
        g_dnm.tail_b = b;
        g_dnm.tail_t0 = GetTickCount64();
    }
    LeaveCriticalSection(&g_mixCs);
    return b >= 0;
}

static int DnmTailLive(void)
{
    if (g_dnm.tail_a < 0 || g_mixState != 1)
        return 0;
    EnterCriticalSection(&g_mixCs);
    int on = g_mixv[g_dnm.tail_a].on
        && g_mixv[g_dnm.tail_a].src == dm_blob + DM_END_OFF;
    LeaveCriticalSection(&g_mixCs);
    return on;
}

// every frame: while the tail rings, the death sting is ducked by the tail's own level (peak of its next 80 ms x its gain): down to 15 %, back up as it decays
static void DnmDuckTick(void)
{
    if (g_mixState != 1)
        return;
    int live = DnmTailLive();
    if (!live && !g_dnm.duck_cap) {
        g_dnm.tail_a = g_dnm.tail_b = -1;
        return;
    }
    float duck = 1.0f;
    if (live) {
        float lvl = 0.0f;
        EnterCriticalSection(&g_mixCs);
        int v = g_dnm.tail_a;
        int p0 = (int) g_mixv[v].pos, p1 = p0 + DM_RATE / 12;
        if (p0 < 0)
            p0 = 0;
        if (p1 > DM_END_N)
            p1 = DM_END_N;
        int pk = 0;
        for (int i = p0; i < p1; i += 4) {
            int x = dm_blob[DM_END_OFF + i];
            x = x < 0 ? -x : x;
            if (x > pk)
                pk = x;
        }
        lvl = pk / 32768.0f * g_mixv[v].tgl;
        LeaveCriticalSection(&g_mixCs);
        duck = 1.0f - 0.85f * (lvl / 0.30f > 1.0f ? 1.0f : lvl / 0.30f);
    }
    EnterCriticalSection(&g_mixCs);
    for (int v = 0; v < MIX_VOICES; v++) {
        if (!g_mixv[v].on
            || (g_mixv[v].src != ds_blob
                && g_mixv[v].src != ds_blob + DS_N))
            continue;
        int right = g_mixv[v].src != ds_blob;
        if (!g_dnm.duck_cap) {
            g_dnm.duck_cap = 1;
            g_dnm.duck_gl = g_mixv[v].tgl;
            g_dnm.duck_gr = g_mixv[v].tgr;
            Xlog("downed music: death sting ducked under the ending tail");
        }
        if (right)
            g_mixv[v].tgr = g_dnm.duck_gr * duck;
        else
            g_mixv[v].tgl = g_dnm.duck_gl * duck;
    }
    if (!live)
        g_dnm.duck_cap = 0;     // the tail is over: the sting's gains were put back on the last pass above (duck = 1)
    LeaveCriticalSection(&g_mixCs);
    if (!live)
        g_dnm.tail_a = g_dnm.tail_b = -1;
}

// cross-fade to a new voice pair of `stage` at the SAME PHASE as the playing voice (position read and new voice started inside one mixer lock, so they are
// sample-aligned). lin = loop wrap (identical audio: linear fade), else equal power. A fade still running is cut at once.
static int DnmSwitch(uint64_t now, int stage, int ms, int lin,
                      const char *why)
{
    if (g_mixState != 1)
        return 0;
    if (g_dnm.xf) {
        DnmKill(&g_dnm.oa, &g_dnm.ob, g_dnm.os);
        g_dnm.xf = 0;
    }
    int a = -1, b = -1, ok;
    double off = 0.0, p = 0.0;
    EnterCriticalSection(&g_mixCs);
    int have = g_dnm.ca >= 0 && g_mixv[g_dnm.ca].on
        && g_mixv[g_dnm.ca].src == DnmSrc(g_dnm.cs, 0);
    if (have) {
        p = g_mixv[g_dnm.ca].pos;
        double q = p >= DM_LEN[g_dnm.cs] ? p - DM_LEN[g_dnm.cs] : p;    // phase inside the loop of the playing stage
        off = fmod(q, (double) DM_LEN[stage]);
    } else {                    // nothing playing (start / ended): the music clock
        double t = (double) (now - g_dnm.first_t0) / 1000.0 * DM_RATE;
        off = fmod(t, (double) DM_LEN[stage]);
    }
    ok = DnmPairLocked(stage, off, &a, &b);
    LeaveCriticalSection(&g_mixCs);
    if (!ok) {
        Xlog("downed music: NOT played (voices busy)");
        return 0;
    }
    g_dnm.oa = have ? g_dnm.ca : -1;
    g_dnm.ob = have ? g_dnm.cb : -1;
    g_dnm.os = g_dnm.cs;
    g_dnm.ca = a;
    g_dnm.cb = b;
    g_dnm.cs = stage;
    g_dnm.xf = 1;
    g_dnm.xf_t0 = now;
    g_dnm.xf_ms = ms;
    g_dnm.xf_lin = lin;
    g_dnm.switches++;
    Xlog("downed music: %s - stage %d at %.2f s (xfade %d ms%s)", why,
         stage, off / DM_RATE, ms, lin ? ", loop wrap" : "");
    return 1;
}

static void DnmusTick(void)
{
    uint64_t now = GetTickCount64();
    DnmDuckTick();
    if (g_dnm.dying && g_dnm.tail_at && now >= g_dnm.tail_at) { // the beat: the tail hits, the stage fades out under it
        g_dnm.tail_at = 0;
        float vol = DNM_LEVEL * ExmusUserVolume();
        int ok = MatchSubstate() == 6 && DnmTailStart(vol);
        Xlog("downed music: ending tail (F_QTClimax, %.1f s) %s",
             DM_END_N / (double) DM_RATE, ok ? "playing" : "NOT played");
        DnmCut(300, "death - the tail takes over");
        g_msHoldUntil =
            now + (uint64_t) (DM_END_N * 1000LL / DM_RATE) + 1500;
        g_msResumeAt = g_msHoldUntil;
    }
    int exec = PsyncRole() == 2 || g_execUntil
        || g_specialExecUntil > g_t60;
    int bad = !g_downed || g_downDying || g_uc4DyingUntil || exec
        || MatchSubstate() != 6;
    if (g_dnm.running || DnmLive()) {
        if (bad && !g_dnm.fading && !g_dnm.dying) {
            if (MatchSubstate() != 6)
                DnmCut(400, "match left the playing state");
            else if (exec)
                DnmCut(300, "executed - the execution music takes over");
            else if (g_downDying || g_uc4DyingUntil
                     || LocalHp() <= 0.5f) {
                // dead (bleed-out or a shot / blow): the ending tail on the next beat of the playing stage
                double p = -1.0;
                EnterCriticalSection(&g_mixCs);
                if (g_dnm.ca >= 0 && g_mixv[g_dnm.ca].on
                    && g_mixv[g_dnm.ca].src == DnmSrc(g_dnm.cs, 0))
                    p = g_mixv[g_dnm.ca].pos;
                LeaveCriticalSection(&g_mixCs);
                float vol = DNM_LEVEL * ExmusUserVolume();
                if (p >= 0.0 && vol > 0.0f && !g_dnm.dying) {
                    double per = DNM_BEAT_S * DM_RATE, rem =
                        (per - fmod(p, per)) / DM_RATE;
                    g_dnm.dying = 1;
                    g_dnm.tail_at =
                        now + (uint64_t) (rem >
                                          0.03 ? (rem -
                                                  0.02) * 1000.0 : 0.0);
                    Xlog("downed music: dead (fraction %.2f, stage %d) - ending tail on the next beat in %.2f s", (double) g_dnm.f, g_dnm.cs, rem);
                } else
                    DnmCut(400, "dead");
            } else
                DnmCut(800, "revived / up");
        }
    }
    // the fade has ended: clear it here, outside the live block above (inside it, a cut whose voices had already stopped left fading = 1 for good,
    // and every later down returned early: the Nora music played only for the first down)
    if (g_dnm.fading && !DnmLive())
        DnmReset();
    if (!g_downed || !g_down_ms) {
        g_dnm.down_ms = 0;
        return;
    }
    if (g_dnm.down_ms != g_down_ms) {   // a new down: the music starts 0.3 s after it (once per down)
        g_dnm.down_ms = g_down_ms;
        g_dnm.running = -1;     // armed
        g_dnm.f = 0.0f;
        g_dnm.f_t = now;
        g_dnm.tgt_prev = 0.0f;
        g_dnm.quick_until = 0;
        g_dnm.switches = 0;
        g_dnm.dbg = 0;
        g_dnm.dying = 0;
        g_dnm.tail_at = 0;
    }
    if (bad || DnmOff() || g_dnm.running == 0 || g_dnm.fading)
        return;
    float tgt =
        (float) ((int64_t) (now - g_down_ms) -
                 (int64_t) g_bleed_pause_ms) / (g_bleedSecs * 1000.0f);
    tgt = tgt < 0.0f ? 0.0f : tgt > 1.0f ? 1.0f : tgt;
    float dt = g_dnm.f_t ? (float) (now - g_dnm.f_t) / 1000.0f : 0.0f;
    g_dnm.f_t = now;
    if (tgt - g_dnm.tgt_prev > 0.06f) {
        g_dnm.quick_until = now + 1500;
        Xlog("downed music: bleed time cut - fraction %.2f -> %.2f",
             (double) g_dnm.tgt_prev, (double) tgt);
    }
    g_dnm.tgt_prev = tgt;
    // f follows the real clock; a hit that cuts the bleed time makes it jump forward, followed within ~0.2 s (80 ms time constant)
    g_dnm.f += (tgt - g_dnm.f) * (dt >= 0.08f ? 1.0f : dt / 0.08f);
    float vol = DNM_LEVEL * ExmusUserVolume();
    if (g_dnm.running == -1) {
        if (now - g_down_ms < DNM_START_MS)
            return;
        if (vol <= 0.0f) {
            Xlog("downed music: skipped (options music volume 0)");
            g_dnm.running = 0;
            return;
        }
        if (!PcmPlayVoice
            (DnmSrc(0, 0), 2, 0.0f, 0.0f, DM_RATE, 0, 0, NULL, 1.0f)
            || g_mixState != 1) {
            Xlog("downed music: mixer off");
            g_dnm.running = 0;
            return;
        }                       // (starts the mixer if needed)
        g_dnm.f = tgt;          // joined late (the start waited): begin at the real fraction
        g_dnm.cs = DnmStageOf(g_dnm.f);
        g_dnm.ca = g_dnm.cb = -1;
        g_dnm.first_t0 = now;
        if (DnmSwitch(now, g_dnm.cs, DNM_START_FADE_MS, 0, "start")) {
            g_dnm.running = 1;
            Xlog("downed music: start %llu ms after the down (bleed %.1f s, fraction %.2f, stage %d)", (unsigned long long) (now - g_down_ms), (double) g_bleedSecs, (double) g_dnm.f, g_dnm.cs);
        } else
            g_dnm.running = 0;
        return;
    }
    // running: keep the combat music cut, follow f
    g_msHoldUntil = now + 1500;
    g_msResumeAt = g_msHoldUntil;
    if (g_dnm.xf) {
        float t = (float) (now - g_dnm.xf_t0) / (float) g_dnm.xf_ms;
        float gi = DnmStageGain(g_dnm.cs, g_dnm.f, vol), go =
            g_dnm.oa >= 0 ? DnmStageGain(g_dnm.os, g_dnm.f, vol) : 0.0f;
        if (t >= 1.0f) {
            DnmKill(&g_dnm.oa, &g_dnm.ob, g_dnm.os);
            g_dnm.xf = 0;
            DnmGain(g_dnm.ca, g_dnm.cb, g_dnm.cs, gi);
        } else {
            float ci = g_dnm.xf_lin ? t : sinf(t * 1.5707963f), co =
                g_dnm.xf_lin ? 1.0f - t : cosf(t * 1.5707963f);
            DnmGain(g_dnm.ca, g_dnm.cb, g_dnm.cs, gi * ci);
            DnmGain(g_dnm.oa, g_dnm.ob, g_dnm.os, go * co);
        }
        return;
    }
    DnmGain(g_dnm.ca, g_dnm.cb, g_dnm.cs,
             DnmStageGain(g_dnm.cs, g_dnm.f, vol));
    double p = -1.0;
    EnterCriticalSection(&g_mixCs);
    if (g_dnm.ca >= 0 && g_mixv[g_dnm.ca].on
        && g_mixv[g_dnm.ca].src == DnmSrc(g_dnm.cs, 0))
        p = g_mixv[g_dnm.ca].pos;
    LeaveCriticalSection(&g_mixCs);
    if (p < 0.0) {              // the voice ended (or its slot was taken): pick up on the music clock
        DnmKill(&g_dnm.ca, &g_dnm.cb, g_dnm.cs);
        DnmSwitch(now, DnmStageOf(g_dnm.f), DNM_START_FADE_MS, 0,
                   "voice ended");
        return;
    }
    if (p >= DM_LEN[g_dnm.cs]) {
        DnmSwitch(now, g_dnm.cs, DNM_WRAP_MS, 1, "loop");
        return;
    }                           // past the loop point: the appended start takes over seamlessly
    int quick = now < g_dnm.quick_until;
    // the stage comes from the real clock (tgt, not the slewed f) looking DNM_LOOK ahead (the switch waits for a bar line, up to 2 s = 0.13 of a 15 s bleed);
    // one stage per bar line, except after a hit (straight to the new stage on the next beat)
    int want = DnmStageOf(tgt + (quick ? 0.0f : DNM_LOOK));
    if (!quick && want > g_dnm.cs + 1)
        want = g_dnm.cs + 1;
    if (want != g_dnm.cs) {
        double period = (quick ? DNM_BEAT_S : DNM_BAR_S) * DM_RATE;
        int ms = quick ? DNM_XF_HIT_MS : DNM_XF_MS;
        double phase = fmod(p, period), rem =
            (period - phase) / DM_RATE, half = ms / 2000.0 + 0.02;
        if (rem <= half || phase / DM_RATE <= 0.04) {   // on the next bar (beat after a hit) line, the fade centred on it
            char why[96];
            snprintf(why, sizeof why,
                     "stage %d -> %d at fraction %.2f (%s-synced)",
                     g_dnm.cs, want, (double) g_dnm.f,
                     quick ? "beat" : "bar");
            DnmSwitch(now, want, ms, 0, why);
        }
    }
    if (now - g_dnm.dbg >= 3000) {
        g_dnm.dbg = now;
        Xlog("downed music: stage %d, fraction %.2f, level %.1f dB, voice at %.2f s, %d switch(es)", g_dnm.cs, (double) g_dnm.f, (double) DnmDb(g_dnm.f), p / DM_RATE, g_dnm.switches);
    }
}
