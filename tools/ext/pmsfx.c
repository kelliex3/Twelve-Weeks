// ---------------------------------------------------------------- post-match flow sounds: the No Return end-of-run score sounds
// Included by t2r-ext.c before the clan post-match page (uses pcm_play_rate / pcm_play_voice / g_mixv / g_mix_cs / exe_side_open). Log prefix 'pm sfx:'.
// Waves of gbl-hud-rl.bnk (tools/rip_postmatch_sfx.py -> postmatch_sfx.pcm + .h, mono s16 48 kHz), played 2D through the PCM mixer - a
// play-sound handle of the bank's events proves nothing in MP. Which wave is which was measured against the
// recorded No Return scores page (re/nr_ui_1102, rip_postmatch_sfx.py header):
//   PMSK_ENTER  hud-rl-overlay-transition-a                       page opens (sfx-hud-rl-score-screen-enter)
//   PMSK_COUNT  ui-hud-rl-score-final-count-up-fast-lp            the rolling count loop, runs exactly as long as the numbers count, then fades out
//   PMSK_END    ui-hud-rl-score-final-count-up-fast-single-end    a count lands (accent)
//   PMSK_STAMP  ui-hud-rl-score-final-grade-stamp-b-big-1         the total lands (NR: with hfx-ui-score-stamp)
//   PMSK_PAGE   ui-mp-meta-rewards-segment-up-b-01                the next post-match page comes in (NR page-meta push)
// t2r-no-postmatch-sfx.txt next to the exe: off.
#include "postmatch_sfx.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl pms_blob\npms_blob:\n.incbin \"postmatch_sfx.pcm\"\n.globl pms_blob_end\npms_blob_end:\n.text\n");
extern const int16_t pms_blob[], pms_blob_end[];
enum { PMSK_ENTER, PMSK_COUNT, PMSK_END, PMSK_STAMP, PMSK_PAGE };
static int OvOff(const char *file, int *flag, uint64_t * chk);
static int16_t g_pmsCnt[PMS_COUNT_N];  // the count loop as played (its length follows the count; the mixer reads the buffer in place)
static int PmsOff(void)
{
    static int off = -1;
    static uint64_t chk;
    return OvOff("t2r-no-postmatch-sfx.txt", &off, &chk);
}

static void PmsPlay(int kind)
{
    static const int o[] =
        { PMS_ENTER_OFF, PMS_COUNT_OFF, PMS_END_OFF, PMS_STAMP_OFF,
PMS_PAGE_OFF }, n[] =
{ PMS_ENTER_N, PMS_COUNT_N, PMS_END_N, PMS_STAMP_N, PMS_PAGE_N };
    // The end-of-round currency count sound effects were very loud: the ripped waves are peak-normalised (0.79), so the old 0.35-0.45
    // played them far above NR's own bus level (count loop rms 0.24); about 40 % of that now
    static const float g[] = { 0.16f, 0.13f, 0.15f, 0.18f, 0.16f };
    static int nlog;
    if (kind < 0 || kind > PMSK_PAGE || PmsOff()
        || (int) (pms_blob_end - pms_blob) < o[kind] + n[kind])
        return;
    int ok = PcmPlayRate(pms_blob + o[kind], n[kind], g[kind], PMS_RATE);
    if (nlog < 24) {
        nlog++;
        Xlog("pm sfx: cue %d (%d samples) -> %s", kind, n[kind],
             ok ? "playing" : "mixer refused");
    }
}

static void PmsCountCut(int fade_ms)
{                               // the count is over (or skipped): the loop fades out
    if (g_mixState != 1)
        return;
    EnterCriticalSection(&g_mixCs);
    for (int v = 0; v < MIX_VOICES; v++)
        if (g_mixv[v].on && g_mixv[v].src == g_pmsCnt && !g_mixv[v].fadeN)
            g_mixv[v].fadeLeft = g_mixv[v].fadeN =
                (fade_ms > 0 ? fade_ms : 1) * MIX_RATE / 1000;
    LeaveCriticalSection(&g_mixCs);
}

static void PmsCountStart(int ms)
{                               // the count loop for a count of ms milliseconds (at most the wave: 2.4 s)
    if (PmsOff()
        || (int) (pms_blob_end - pms_blob) < PMS_COUNT_OFF + PMS_COUNT_N)
        return;
    PmsCountCut(1);
    int n = ms * (PMS_RATE / 1000) + PMS_RATE / 10;
    if (n > PMS_COUNT_N)
        n = PMS_COUNT_N;
    if (n < 4800)
        n = 4800;
    memcpy(g_pmsCnt, pms_blob + PMS_COUNT_OFF, (size_t) n * 2);
    int k = n < 2880 ? n : 2880;        // 60 ms fade-out into the end of the buffer
    for (int i = 0; i < k; i++)
        g_pmsCnt[n - k + i] =
            (int16_t) (g_pmsCnt[n - k + i] * (float) (k - i) / (float) k);
    int ok = PcmPlayRate(g_pmsCnt, n, 0.13f, PMS_RATE);      // was 0.35 (too loud)
    Xlog("pm sfx: count loop %d ms -> %s", ms,
         ok ? "playing" : "mixer refused");
}
