// Execution victim music: while WE are the one being executed, one of three TLOU2 music cues (random) rises from silence
// (exp curve over ~3.3 s, baked into the wave by tools/rip_exec_music.py) and is cut with a 0.3 s fade when the death screen shows.
// TLOU2's music events are silent in MP, so it plays through our PCM mixer as two hard-panned voices. Client-side only; never more
// than one at a time (a new start cuts the old one); also faded at match end / on a death-ui reset, and hard-capped at EXMUS_CAP_MS.
// Level: EXMUS_LEVEL (below a gunshot) x the options menu's MUSIC volume (option slot 1, same read as the sfx slider).
#include "exec_music.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl exmus_blob\nexmus_blob:\n.incbin \"exec_music.pcm\"\n.text\n");
extern const int16_t exmus_blob[];
#define EXMUS_LEVEL 0.85f
#define EXMUS_CAP_MS 12500      // the whole 12 s cue (it keeps playing over the death screen)
#define EXMUS_FADE_MS 300
static struct {
    int a, b;
    const int16_t *l;
    uint64_t t0;
    int fading;
} g_exmus = { -1, -1, NULL, 0, 0 };

static float ExmusUserVolume(void)
{
    float s = 1.0f, m = 1.0f, d = 0.0f;
    if (!SfxOptionRaw(&s, &m, &d))
        return 1.0f;
    m = m > 1.5f ? m / 100.0f : m;
    m = m < 0.0f ? 0.0f : m > 1.0f ? 1.0f : m;
    // (every custom sound on the same volumes) the game's slider taper (squared, as sfx_user_volume) x the general volume
    return m * m * GeneralVolume();
}

static int ExmusLive(void)
{
    if (g_exmus.a < 0 || g_mixState != 1)
        return 0;
    int on = 0;
    EnterCriticalSection(&g_mixCs);
    for (int k = 0; k < 2; k++) {
        int v = k ? g_exmus.b : g_exmus.a;
        if (v >= 0 && g_mixv[v].on
            && g_mixv[v].src == g_exmus.l + (k ? EXMUS_N : 0))
            on = 1;
    }
    LeaveCriticalSection(&g_mixCs);
    return on;
}

static void ExmusCut(int fade_ms)
{
    if (g_exmus.a < 0)
        return;
    if (g_mixState == 1) {
        EnterCriticalSection(&g_mixCs);
        for (int k = 0; k < 2; k++) {
            int v = k ? g_exmus.b : g_exmus.a;
            if (v < 0 || !g_mixv[v].on
                || g_mixv[v].src != g_exmus.l + (k ? EXMUS_N : 0))
                continue;       // slot already reused by another sound
            if (fade_ms <= 0)
                g_mixv[v].on = 0;
            else {
                g_mixv[v].fadeLeft = g_mixv[v].fadeN =
                    fade_ms * MIX_RATE / 1000;
            }
        }
        LeaveCriticalSection(&g_mixCs);
    }
    if (fade_ms <= 0)
        g_exmus.a = g_exmus.b = -1;
}

// Combat music under the execution (it must duck to ~10-15 % for the victim). TLOU2 has no music-only gain: the mixer
// snapshots in the data are mix-rl-*-except-music (they duck everything BUT the music, the wrong way round), mix-bus-music is a bus
// name with no override native, and session volume would duck our own PCM mixer too. So the combat music playlist is CUT (the
// game's own rogue-player-death cut, the one the down stinger uses; skipped while that cut / a sting is still within its hold
// window) and held cut for the whole execution; feature_combat_music puts the current loop back (g_ms_resume_at, the same path
// as after a stinger).
#define EXMUS_RESUME_MS 1500
static struct {
    uint64_t at;
    int pick;
    double seek;
    int cut;
} g_exmusPend;                 // the cue starts at `at` (kill + death screen delay - its hit time)
static void ExecmusMusicHold(uint64_t until)
{
    g_msHoldUntil = until;    // no downward tier change / bed / loop re-queue before it
    g_msResumeAt = until;     // then the combat music comes back (only if it is gone)
}

static void ExecmusFade(const char *why)
{
    g_exmusPend.at = 0;        // a cue still waiting to start never starts after a fade (aborted execution / respawn / match end)
    if (g_exmus.a < 0 || g_exmus.fading)
        return;
    ExecmusMusicHold(GetTickCount64() + EXMUS_FADE_MS + EXMUS_RESUME_MS);
    g_exmus.fading = 1;
    Xlog("exec music: %s - fade-out %d ms (%llu ms into it)", why,
         EXMUS_FADE_MS,
         (unsigned long long) (GetTickCount64() - g_exmus.t0));
    ExmusCut(EXMUS_FADE_MS);
}

// the second of each cue (from its first sound) that must land exactly on the death screen (cue order = rip_exec_music.py NAMES)
static const float EXMUS_HIT[] = { 2.44f, 2.35f, 2.31f, 2.35f, 2.39f }; // (#4: 2.15 + the 0.2 s of silence the preview skipped)

_Static_assert(sizeof EXMUS_HIT / sizeof *EXMUS_HIT == EXMUS_CUES,
               "EXMUS_HIT: one timing per cue");
static void ExecmusPlay(int pick, double seek_s);
static void ExecmusStart(float kill_s)
{
    uint64_t t_now = GetTickCount64();
    int pick = (int) (T2rRand() % EXMUS_CUES);
    // the death screen lands kill_s + DU_DELAY_MS (83) + DU_CUT_MS (70, death_ui.c) after the move starts; the cue's hit second goes there
    double land = (double) kill_s + (83 + 70) / 1000.0, lead =
        land - EXMUS_HIT[pick];
    g_exmusPend.pick = pick;
    g_exmusPend.at =
        lead > 0 ? t_now + (uint64_t) (lead * 1000.0) : t_now;
    g_exmusPend.seek = lead < 0 ? -lead : 0.0;
    g_exmusPend.cut = t_now >= g_msHoldUntil;        // decided now: the hold set below would block execmus_play's own check (cue + combat music together)
    // the combat music keeps playing (no end tag, no silence: "intense gameplay, execution, silence, then the rise" was
    // anticlimactic) and is cut the instant the cue starts (execmus_play): the cue's opening sting takes over from it directly
    ExecmusMusicHold(t_now + (uint64_t) (lead > 0 ? lead * 1000.0 : 0) +
                       EXMUS_CAP_MS + EXMUS_RESUME_MS);
    Xlog("exec music: cue %d hit %.2f s on the death screen: starts in %.2f s (kill %.2f s)%s", pick, (double) EXMUS_HIT[pick], lead > 0 ? lead : 0.0, (double) kill_s, lead < 0 ? " - joined mid-cue" : "");
}

static void ExecmusPlay(int pick, double seek_s)
{
    uint64_t t_now = GetTickCount64();
    if (g_exmusPend.cut)
        MusicPlay("music-playlist-rogue-player-death",
                   "execution victim: combat music cut on the cue's first sound");
    ExecmusMusicHold(t_now + EXMUS_CAP_MS + EXMUS_RESUME_MS); // held cut until the execution music ends (fade / cap / match end)
    if (ExmusLive())
        ExmusCut(0);           // one at a time
    g_exmus.a = g_exmus.b = -1;
    g_exmus.fading = 0;
    const int16_t *l = exmus_blob + EXMUS_OFF[pick], *r = l + EXMUS_N;
    float g = EXMUS_LEVEL * ExmusUserVolume();
    if (g <= 0.0f) {
        Xlog("exec music: cue %d skipped (options music volume 0)", pick);
        return;
    }
    if (!PcmPlayVoice(l, 2, 0.0f, 0.0f, EXMUS_RATE, 0, 0, NULL, 1.0f)
        || g_mixState != 1) {
        Xlog("exec music: mixer off");
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
        const int16_t *src[2] = { l, r };
        int vv[2] = { a, b };
        for (int k = 0; k < 2; k++) {
            int v = vv[k];
            g_mixv[v].src = src[k];
            g_mixv[v].n = EXMUS_N;
            g_mixv[v].pos = seek_s * EXMUS_RATE;
            g_mixv[v].step = (double) EXMUS_RATE / MIX_RATE;
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
        g_exmus.a = a;
        g_exmus.b = b;
        g_exmus.l = l;
        g_exmus.t0 = GetTickCount64();
    }
    Xlog("exec music: victim cue %d (%s) gain %.2f - %s", pick,
         "beg-for-life set", (double) g,
         b >= 0 ? "playing" : "NOT played (voices busy)");
}

// per frame (feature_death_ui): ends the music when the mixer let it finish, at the cap, or when the match left the playing state
static void ShivstingTick(void);       // shiv_sting.c
static void ShivstingCut(int fade_ms, const char *why);
static void DnmusTick(void);   // downed_music.c
static void ExecmusTick(void)
{
    ShivstingTick();
    AlertstingTick();
    DnmusTick();
    if (g_exmusPend.at && GetTickCount64() >= g_exmusPend.at) {
        g_exmusPend.at = 0;
        if (MatchSubstate() == 6)
            ExecmusPlay(g_exmusPend.pick, g_exmusPend.seek);
    }
    if (g_exmus.a < 0)
        return;
    if (!ExmusLive()) {
        g_exmus.a = g_exmus.b = -1;
        ExecmusMusicHold(GetTickCount64() + EXMUS_RESUME_MS);
        return;
    }
    uint64_t el = GetTickCount64() - g_exmus.t0;
    if (el >= EXMUS_CAP_MS)
        ExecmusFade("cap reached");
    else if (MatchSubstate() != 6)
        ExecmusFade("match left the playing state");
}
