// slowdown.h - slows things down: a lot of systems played animations too fast / moved characters too fast, so movement speed is 10% lower. One global flag: t2r-no-slowdown.txt next to the exe reverts everything below
// (re-read every 2 s; the movement multipliers follow within 30 frames, the blend / rate constants on their next use).
//   SD_MOVE  = the factor on player locomotion (the game's motion-match speed multipliers, skills.c sk_movement; the downed crawl curves)
//   sd_f(calm, orig) = a calmer constant (softer blend, lower forced rate) or the original with the opt-out
// Every client applies the same factor to its own body; puppets are positioned from the owner's streamed poses (netsmooth measures velocity
// from the samples), so no peer-side speed table exists that could disagree.
#ifndef T2R_SLOWDOWN_H
#define T2R_SLOWDOWN_H
#define SD_MOVE 0.90f
#define SD_SYSTEMS 5            // calmed systems besides locomotion: death-reaction blend, downed-melee blends, whiff-recovery rate, down-entry time, downed crawl
static int SdOff(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        FILE *f = ExeSideOpen("t2r-no-slowdown.txt", "r");
        v = f != NULL;
        if (f)
            fclose(f);
    }
    return v;
}

static float SdMul(void)
{
    return SdOff()? 1.0f : SD_MOVE;
}

static float SdF(float calm, float orig)
{
    return SdOff()? orig : calm;
}
#endif
