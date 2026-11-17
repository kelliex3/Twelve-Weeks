// Shiv victim shock sting a stinger when the shiv sequence starts for the victim, as a shock.
// While WE are the victim of a shiv move (xcam_start role 2: the victim's client, local or learned from the network) a ~1 s TLOU2 SP
// one-shot hit (tools/rip_shiv_sting.py -> shiv_sting.pcm; pool of candidates 02 03 04, random without repeats;
// first sound at sample 0, attack < 20 ms) plays 2D through the PCM mixer at the music volume (exmus_user_volume) on the
// move's first frame. Combat music, if some plays, is cut the same instant (the rogue-player-death cut, same as the execution music) and put back
// by the stinger layer's resume (g_ms_resume_at). Shiv moves are short (shiv-stealth-back kills at 0.36 s, the death sting lands ~0.15 s
// later), so the death sting (death_ui.c) fades this one out first (shivsting_cut). Once per move: a repeat start inside SHS_DEDUPE_MS (psync
// re-send / retry of the same move) is ignored; the attacker never gets it (xcam_start role 1 does not reach shivsting_start).
// t2r-no-shiv-sting.txt next to the exe: off. Logs 'shiv sting: ...'.
// Included from t2r-ext.c after exec_music.c (uses exmus_user_volume, EXMUS_RESUME_MS).
#include "shiv_sting.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl shs_blob\nshs_blob:\n.incbin \"shiv_sting.pcm\"\n.text\n");
extern const int16_t shs_blob[];
#define SHS_LEVEL 0.9f
#define SHS_DEDUPE_MS 2500
#include "shiv_drums.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl shd_blob\nshd_blob:\n.incbin \"shiv_drums.pcm\"\n.text\n");
extern const int16_t shd_blob[];
#define SHD_LEVEL 0.75f
#define SHD_FADE_MS 350
#define SHD_HOLD_MS 4000        // combat music stays cut from the shiv to the death flag (feature_music_stingers then holds it for the whole death screen)
static struct {
    int a, b;
    uint64_t t0, last;
    int pick;
} g_shd = { -1, -1, 0, 0, 0 };  // pick: the take of the pool (SHD_K) playing

static struct {
    int a, b, cue;
    uint64_t t0, last;
} g_shs = { -1, -1, 0, 0, 0 };

static const int16_t *ShsSrc(int cue, int k)
{
    return shs_blob + SHS_OFF[cue] + (k ? SHS_LEN[cue] : 0);
}

static int ShsLive(void)
{
    if (g_shs.a < 0 || g_mixState != 1)
        return 0;
    int on = 0;
    EnterCriticalSection(&g_mixCs);
    for (int k = 0; k < 2; k++) {
        int v = k ? g_shs.b : g_shs.a;
        if (v >= 0 && g_mixv[v].on
            && g_mixv[v].src == ShsSrc(g_shs.cue, k))
            on = 1;
    }
    LeaveCriticalSection(&g_mixCs);
    return on;
}

// ms the running shock sting still has to play (0 = none running)
static int ShivstingRemainingMs(void)
{
    if (!ShsLive())
        return 0;
    int len =
        (int) (SHS_LEN[g_shs.cue] * 1000LL / SHS_RATES[g_shs.cue]), el =
        (int) (GetTickCount64() - g_shs.t0);
    return len > el ? len - el : 0;
}

static void ShivdrumCut(int fade_ms, const char *why); // below
static void ShivstingCut(int fade_ms, const char *why)
{
    ShivdrumCut(fade_ms > 0 && fade_ms < SHD_FADE_MS ? SHD_FADE_MS : fade_ms, why);    // the drums duck under the death sting too (a longer fade than the shock sting's)
    if (g_shs.a < 0)
        return;
    int live = ShsLive();
    if (live) {
        EnterCriticalSection(&g_mixCs);
        for (int k = 0; k < 2; k++) {
            int v = k ? g_shs.b : g_shs.a;
            if (v < 0 || !g_mixv[v].on
                || g_mixv[v].src != ShsSrc(g_shs.cue, k))
                continue;       // slot already reused by another sound
            if (fade_ms <= 0)
                g_mixv[v].on = 0;
            else {
                g_mixv[v].fadeLeft = g_mixv[v].fadeN =
                    fade_ms * MIX_RATE / 1000;
            }
        }
        LeaveCriticalSection(&g_mixCs);
        Xlog("shiv sting: cut (%s) %llu ms in", why,
             (unsigned long long) (GetTickCount64() - g_shs.t0));
    }
    g_shs.a = g_shs.b = -1;
}

// per frame (execmus_tick): forget a finished sting, cut it when the match leaves the playing state
static void ShivdrumTick(void)
{
    if (g_shd.a < 0)
        return;
    int on = 0;
    EnterCriticalSection(&g_mixCs);
    for (int k = 0; k < 2; k++) {
        int v = k ? g_shd.b : g_shd.a;
        if (v >= 0 && g_mixv[v].on
            && g_mixv[v].src ==
            shd_blob + SHD_OFF[g_shd.pick] + (k ? SHD_LEN[g_shd.pick] : 0))
            on = 1;
    }
    LeaveCriticalSection(&g_mixCs);
    if (!on) {
        g_shd.a = g_shd.b = -1;
        return;
    }
    if (MatchSubstate() != 6)
        ShivdrumCut(60, "match left the playing state");
}

static void ShivdrumCut(int fade_ms, const char *why)
{
    if (g_shd.a < 0)
        return;
    int live = 0;
    EnterCriticalSection(&g_mixCs);
    for (int k = 0; k < 2; k++) {
        int v = k ? g_shd.b : g_shd.a;
        if (v < 0 || !g_mixv[v].on
            || g_mixv[v].src !=
            shd_blob + SHD_OFF[g_shd.pick] + (k ? SHD_LEN[g_shd.pick] : 0))
            continue;
        live = 1;
        if (fade_ms <= 0)
            g_mixv[v].on = 0;
        else {
            g_mixv[v].fadeLeft = g_mixv[v].fadeN =
                fade_ms * MIX_RATE / 1000;
        }
    }
    LeaveCriticalSection(&g_mixCs);
    if (live)
        Xlog("shiv music: drums faded %d ms (%s) %llu ms in", fade_ms, why,
             (unsigned long long) (GetTickCount64() - g_shd.t0));
    g_shd.a = g_shd.b = -1;
}

// xcam_start role 2 with a shiv move name (before shivsting_start): the combat music is cut at the move start (the game's own
// rogue-player-death cut) and held cut through the death screen, and an SP struggle drum cue (tools/rip_shiv_drums.py) plays under the shock sting until the death
// sting ducks it. The attacker is untouched. t2r-no-shiv-music.txt next to the exe: off (the old behaviour: combat music carries on to the death cut).
static void ShivdrumStart(const char *name)
{
    static int off = -1;
    if (off < 0) {
        FILE *f = ExeSideOpen("t2r-no-shiv-music.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
        if (off)
            Xlog("shiv music: off (t2r-no-shiv-music.txt)");
    }
    if (off)
        return;
    uint64_t now = GetTickCount64();
    if (g_shd.last && now - g_shd.last < SHS_DEDUPE_MS)
        return;
    if (MatchSubstate() != 6) {
        Xlog("shiv music: %s skipped - not in a running match", name);
        return;
    }
    g_shd.last = now;
    uint64_t cur = MusicCurrent();
    MusicPlay("music-playlist-rogue-player-death",
               "shiv victim: combat music out");
    g_msHoldUntil = now + SHD_HOLD_MS;
    g_msResumeAt = g_msHoldUntil;   // no tier change / loop re-queue / resume until the death flag takes over the hold
    g_msNowatchUntil = now + SHD_HOLD_MS;     // a loop queued just before the shiv is not retried by the music watch
    float g = SHD_LEVEL * ExmusUserVolume();
    if (g <= 0.0f) {
        Xlog("shiv music: %s - combat music cut (was %llx), drums skipped (options music volume 0)", name, (unsigned long long) cur);
        return;
    }
    if (g_shd.a >= 0)
        ShivdrumCut(0, "new move");
    g_shd.a = g_shd.b = -1;
    {                           // one take of the pool at random per shiv, never the same twice in a row
        static int last = -1;
        int pk = (int) (T2rRand() % (uint32_t) SHD_K);
        if (SHD_K > 1 && pk == last)
            pk = (pk + 1 +
                  (int) (T2rRand() % (uint32_t) (SHD_K - 1))) % SHD_K;
        g_shd.pick = last = pk;
    }
    const int16_t *base = shd_blob + SHD_OFF[g_shd.pick];
    const uint32_t sn = SHD_LEN[g_shd.pick];
    if (!PcmPlayVoice
        (shd_blob, 2, 0.0f, 0.0f, SHD_RATE, 0, 0, NULL, 1.0f)
        || g_mixState != 1) {
        Xlog("shiv music: %s - mixer off", name);
        return;
    }
    EnterCriticalSection(&g_mixCs);
    int a = -1, b = -1;
    for (int v = 0; v < MIX_VOICES && b < 0; v++) {
        if (g_mixv[v].on)
            continue;
        if (a < 0)
            a = v;
        else
            b = v;
    }
    if (b >= 0) {
        const int16_t *src[2] = { base, base + sn };
        int vv[2] = { a, b };
        for (int k = 0; k < 2; k++) {
            int v = vv[k];
            g_mixv[v].src = src[k];
            g_mixv[v].n = sn;
            g_mixv[v].pos = 0.0;
            g_mixv[v].step = (double) SHD_RATE / MIX_RATE;
            g_mixv[v].played = 0;
            g_mixv[v].fadeN = 0;
            g_mixv[v].gl = g_mixv[v].tgl = k ? 0.0f : g;
            g_mixv[v].gr = g_mixv[v].tgr = k ? g : 0.0f;
            g_mixv[v].dyn = 0;
            g_mixv[v].ent = 0;
            g_mixv[v].base = 1.0f;
            memset(g_mixv[v].wp, 0, 12);
            g_mixv[v].on = 1;
        }
    }
    LeaveCriticalSection(&g_mixCs);
    if (b >= 0) {
        g_shd.a = a;
        g_shd.b = b;
        g_shd.t0 = now;
    }
    Xlog("shiv music: %s - combat music cut (was %llx, held %d ms), drums %s %s (%.2f s, gain %.2f)", name, (unsigned long long) cur, SHD_HOLD_MS, b >= 0 ? "playing" : "NOT played (voices busy)", SHD_NAMES[g_shd.pick], sn / (double) SHD_RATE, (double) g);
}

static void ShivstingTick(void)
{
    ShivdrumTick();
    if (g_shs.a < 0)
        return;
    if (!ShsLive()) {
        g_shs.a = g_shs.b = -1;
        return;
    }
    if (MatchSubstate() != 6)
        ShivstingCut(60, "match left the playing state");
}

// xcam_start role 2 with a shiv move name
static void ShivstingStart(const char *name)
{
    static int off = -1;
    if (off < 0) {
        FILE *f = ExeSideOpen("t2r-no-shiv-sting.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
        if (off)
            Xlog("shiv sting: off (t2r-no-shiv-sting.txt)");
    }
    if (off)
        return;
    uint64_t now = GetTickCount64();
    if (g_shs.last && now - g_shs.last < SHS_DEDUPE_MS) {
        Xlog("shiv sting: %s ignored - the same move already stung %llu ms ago", name, (unsigned long long) (now - g_shs.last));
        return;
    }
    if (MatchSubstate() != 6) {
        Xlog("shiv sting: %s skipped - not in a running match", name);
        return;
    }
    g_shs.last = now;
    // the stealth-notification alert as a prominent sting on top, ONLY for a shiv from behind on a victim that was not stunned
    if (strstr(name, "stealth-back")
        && !(g_stun_until && g_t60 < g_stun_until))
        AlertstingPlay(1.0f, "back shiv victim (not stunned)");
    else if (strstr(name, "stealth-back"))
        Xlog("alert sting: back shiv on a stunned victim - none");
    float g = SHS_LEVEL * ExmusUserVolume();
    if (g <= 0.0f) {
        Xlog("shiv sting: %s skipped (options music volume 0)", name);
        return;
    }
    if (ShsLive())
        ShivstingCut(0, "new sting");
    g_shs.a = g_shs.b = -1;
    // the pool (candidates 02 03 04): random, never the same cue twice in a row
    static int prev = -1;
    int cue = SHS_K > 1 ? (int) (T2rRand() % (SHS_K - (prev >= 0))) : 0;
    if (SHS_K > 1 && prev >= 0 && cue >= prev)
        cue++;
    prev = cue;
    const int N = SHS_LEN[cue], RATE = SHS_RATES[cue];
    // combat music carries on under the shock sting (combat music carries on and is cut off with the death stinger, instead of being
    // absent right after the shiv): the death ui's rogue-player-death cut at the death ends it, not this hit
    int cut = 0;
    if (!PcmPlayVoice
        (ShsSrc(cue, 0), 2, 0.0f, 0.0f, RATE, 0, 0, NULL, 1.0f)
        || g_mixState != 1) {
        Xlog("shiv sting: %s - mixer off", name);
        return;
    }                           // (starts the mixer if needed)
    EnterCriticalSection(&g_mixCs);
    int a = -1, b = -1;
    for (int v = 0; v < MIX_VOICES && b < 0; v++) {
        if (g_mixv[v].on)
            continue;
        if (a < 0)
            a = v;
        else
            b = v;
    }
    if (b >= 0) {
        const int16_t *src[2] = { ShsSrc(cue, 0), ShsSrc(cue, 1) };
        int vv[2] = { a, b };
        for (int k = 0; k < 2; k++) {
            int v = vv[k];
            g_mixv[v].src = src[k];
            g_mixv[v].n = N;
            g_mixv[v].pos = 0.0;
            g_mixv[v].step = (double) RATE / MIX_RATE;
            g_mixv[v].played = 0;
            g_mixv[v].fadeN = 0;
            g_mixv[v].gl = g_mixv[v].tgl = k ? 0.0f : g;
            g_mixv[v].gr = g_mixv[v].tgr = k ? g : 0.0f;
            g_mixv[v].dyn = 0;
            g_mixv[v].ent = 0;
            g_mixv[v].base = 1.0f;
            memset(g_mixv[v].wp, 0, 12);
            g_mixv[v].on = 1;
        }
    }
    LeaveCriticalSection(&g_mixCs);
    if (b >= 0) {
        g_shs.a = a;
        g_shs.b = b;
        g_shs.cue = cue;
        g_shs.t0 = now;
    }
    Xlog("shiv sting: %s - %s cue %s (%.2f s, gain %.2f, combat music %s)",
         name, b >= 0 ? "playing" : "NOT played (voices busy)",
         SHS_NAME[cue], N / (double) RATE, (double) g,
         cut ? "cut" : "not cut (none playing / held)");
}
