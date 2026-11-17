// Offline model of winmm's per-slot anim job + replay gate and of the animjob.h fix (re/netcode.md section 9).
// Replays the body sequence of slot 3 in a bug report (female lives 2, 4, 8 with the puppet's own anim table gen 647418036,
// male lives 3, 5, 9 on ours, gen 1961772386; stall frames from the log: 41938 .. 46540, 49772 .. 62975, 65816 ..) and of another report's slot 0.
// Build: gcc -std=gnu2x -O2 -o animjob_test animjob_test.c && ./animjob_test    (exit 0 = all checks pass)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../animjob.h"

typedef struct {
    uint8_t raw[0x6200];
} SlotT;                       // winmm slot record: +0x4ca8 job ptr, +0x60b8 parked anim object, +0x74c8 clock is modelled apart
typedef struct {
    SlotT s;
    float clock;
    int clock_init;
    uint32_t stamps;
    int replays, npc;
} ModelT;
typedef struct {
    int32_t table_id;
} TableT;                      // [+4] in the real object; the model keeps just the id

static uint8_t *JobNew(int32_t id)
{
    uint8_t *j = calloc(1, 0x80);
    *(int32_t *) (j + AJ_JOB_ID) = id;
    return j;
}

static uint8_t **Jobp(ModelT *m)
{
    return (uint8_t **) (m->s.raw + AJ_SLOT_JOB);
}

static int32_t *Parked(ModelT *m)
{
    return (int32_t *) (m->s.raw + 0x60b8);
}                               // holds the table id of the parked object in the model (0 = none)

// winmm replay 0x180027c00 gate (0x180027d2f..0x180027d89) + what its tail does to the clock
static void WinmmReplay(ModelT *m, int32_t table_id, float target)
{
    uint8_t *job = *Jobp(m);
    if (!job) {
        *Parked(m) = table_id;
        m->npc++;
        return;
    }                           // 0x180027e7d: park the object, NPC graph
    if (table_id == 0 || table_id != *(int32_t *) (job + AJ_JOB_ID)) {
        m->npc++;
        return;
    }                           // 0x180027d86 jne -> 0x180027e85: NPC graph
    m->replays++;
    m->clock = target;
    m->clock_init = 1;
    m->stamps++;                // clock chase (target tracks the newest list time)
}

// winmm frame function 0x180028c40: build a job only while the slot has none, from the parked object's table
static void WinmmBuild(ModelT *m)
{
    if (*Jobp(m) || !*Parked(m))
        return;
    *Jobp(m) = JobNew(*Parked(m));
}

static int run(const char *name, const int *bodies,
               const int *frames_per_body, int nb, int fix, int verbose)
{
    ModelT m;
    memset(&m, 0, sizeof m);
    AjCache c;
    memset(&c, 0, sizeof c);
    int stalled_total = 0, stalled_after_settle = 0;
    float t = 1000.0f;
    for (int b = 0; b < nb; b++) {
        int stall_frames = 0, first_ok = -1;
        for (int f = 0; f < frames_per_body[b]; f++) {
            float before = m.clock;
            t += 12.0f;
            if (fix)
                AjFix(m.s.raw, bodies[b], &c, NULL);   // pa_replay_hook -> pa_job_check
            WinmmReplay(&m, bodies[b], t);
            WinmmBuild(&m);    // main thread, once per frame
            int adv = m.clock > before;
            if (!adv)
                stall_frames++;
            else if (first_ok < 0)
                first_ok = f;
        }
        stalled_total += stall_frames;
        if (first_ok > 3 || first_ok < 0)
            stalled_after_settle += stall_frames;
        if (verbose)
            printf
                ("  %-8s body %d (table %d): %d of %d frames without clock advance (first advancing frame %d)\n",
                 name, b, bodies[b], stall_frames, frames_per_body[b],
                 first_ok);
    }
    printf
        ("%s %s: clock stalled %d frames (%d beyond the first 3 frames of a life)\n",
         name, fix ? "FIXED  " : "ORIGINAL", stalled_total,
         stalled_after_settle);
    return stalled_after_settle;
}

int main(void)
{
    enum { F = 647418036, M = 1961772386 };
    int rc = 0;
    // Y2WQ slot 3: life 2 female (the job is built for it: the first body winmm sees), then male 3, female 4, male 5, female 8, male 9
    const int y2wq[] = { F, M, F, M, F, M };
    const int y2wq_frames[] = { 7000, 4600, 3200, 12000, 2800, 20000 };
    // A9PF slot 0 (we are female-table there): male life, female 22, ...
    const int a9pf[] = { F, M, F, M };
    const int a9pf_frames[] = { 1000, 10000, 3000, 5000 };
    int o1 = run("Y2WQ", y2wq, y2wq_frames, 6, 0, 1), f1 =
        run("Y2WQ", y2wq, y2wq_frames, 6, 1, 1);
    int o2 = run("A9PF", a9pf, a9pf_frames, 4, 0, 0), f2 =
        run("A9PF", a9pf, a9pf_frames, 4, 1, 0);
    // same-body match (no body change): the fix must do nothing
    const int same[] = { M, M, M };
    const int same_frames[] = { 500, 500, 500 };
    int o3 = run("same", same, same_frames, 3, 0, 0), f3 =
        run("same", same, same_frames, 3, 1, 0);
    if (!(o1 > 10000)) {
        printf("FAIL: the original model does not reproduce the stall\n");
        rc = 1;
    }
    if (!(o2 > 5000)) {
        printf
            ("FAIL: the original model does not reproduce the A9PF stall\n");
        rc = 1;
    }
    if (f1 || f2 || f3 || o3) {
        printf
            ("FAIL: the fix still stalls or the no-change case stalls\n");
        rc = 1;
    }
    // a cached job of the returning body is reused (no new job)
    ModelT m;
    memset(&m, 0, sizeof m);
    AjCache c;
    memset(&c, 0, sizeof c);
    *Jobp(&m) = JobNew(F);
    uint8_t *jf = *Jobp(&m);
    int32_t id;
    AjFix(m.s.raw, F, &c, &id);
    if (AjFix(m.s.raw, M, &c, &id) != AJ_DROPPED || *Jobp(&m) != NULL) {
        printf("FAIL: no drop on a new table\n");
        rc = 1;
    }
    *Parked(&m) = M;
    WinmmBuild(&m);
    uint8_t *jm = *Jobp(&m);
    if (!jm || *(int32_t *) (jm + AJ_JOB_ID) != M) {
        printf("FAIL: rebuild\n");
        rc = 1;
    }
    if (AjFix(m.s.raw, F, &c, &id) != AJ_SWAPPED || *Jobp(&m) != jf) {
        printf("FAIL: cached female job not swapped back\n");
        rc = 1;
    }
    if (AjFix(m.s.raw, M, &c, &id) != AJ_SWAPPED || *Jobp(&m) != jm) {
        printf("FAIL: cached male job not swapped back\n");
        rc = 1;
    }
    if (AjFix(m.s.raw, 0, &c, &id) != AJ_OK) {
        printf("FAIL: unknown table id must not touch the job\n");
        rc = 1;
    }
    printf(rc ? "RESULT: FAIL\n" : "RESULT: ok\n");
    return rc;
}
