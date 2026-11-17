// ---------------------------------------------------------------- anim job per puppet slot (root cause of "anim clock stands still")
// Pure logic, shared by t2r-ext.c (pa_replay_hook) and tools/ext/tests/animjob_test.c (offline model of the gate).
//
// winmm's replay 0x180027c00 does not replay a list directly: it needs a per-slot "anim job" object at slot+0x4ca8 (128 bytes + joint-count sized
// pose buffers, id of the anim table it was built for at job+0x18). The frame function 0x180028c40 builds that job ONCE per slot: only while
// slot+0x4ca8 == 0, from the table of the object the replay parked at slot+0x60b8 (table = [[a2]+0x50], its id = [table+4]). Nothing ever frees
// or rebuilds it (no other writer of +0x4ca8 exists; NetAnimClear 0x180029300 leaves it alone). The replay gate at 0x180027d7b..0x180027d89
//     eax = [[[a2]+0x50]+4]; if (eax == 0 || eax != [job+0x18]) goto 0x180027e85   (= the puppet's NPC graph, no replay)
// so a puppet whose body has ANOTHER anim table than the one the job was built for (male <-> female: the sender respawns as the other
// body, or the slot is taken over by a joiner with the other body) is never replayed again: the list is still accepted (the receiver only
// compares the header generation), but slot+0x74c8 (clock) and the pose stamps +0x74d4/8 do not move = "anim clock advanced 0/s still N/s".
// It ends when a respawn brings back the body the job was built for. Evidence: logs Y2WQ (slot 3: stall at the male lives 3, 5, 9, advancing in
// the female lives 4, 8) and A9PF (slot 0 / 3 / 5 likewise), see re/netcode.md section 9.
//
// Fix: remember every job seen per slot with its table id; when the puppet's current table id differs from the installed job's, swap in the
// cached job of that id, or (none yet) clear the pointer so winmm's own builder makes one for the new table on its next frame.
#ifndef T2R_ANIMJOB_H
#define T2R_ANIMJOB_H
#include <stdint.h>
#ifndef AJ_READABLE
#define AJ_READABLE(p, n) ((p) != NULL)
#endif
#ifndef AJ_XCHG
#define AJ_XCHG(pp, v) (*(pp) = (v))
#endif
#define AJ_SLOT_JOB 0x4ca8
#define AJ_JOB_ID 0x18
#define AJ_WAYS 4
typedef struct {
    int32_t id;
    uint8_t *job;
} AjEnt;
typedef struct {
    AjEnt e[AJ_WAYS];
    uint32_t rr;
} AjCache;
enum { AJ_OK = 0, AJ_SWAPPED = 1, AJ_DROPPED = 2 };
// slot: base of the winmm slot record (PA_SLOT(s)); cur: id of the table the puppet's replay call carries (0 = unknown: leave everything);
// *old_id: id of the job that was installed (for the log)
static int AjFix(uint8_t *slot, int32_t cur, AjCache *c, int32_t *old_id)
{
    uint8_t **pj = (uint8_t **) (slot + AJ_SLOT_JOB);
    uint8_t *job = *pj;
    if (!job || !cur || !AJ_READABLE(job + AJ_JOB_ID, 4))
        return AJ_OK;
    int32_t jid = *(int32_t *) (job + AJ_JOB_ID);
    if (old_id)
        *old_id = jid;
    int k = -1;
    for (int i = 0; i < AJ_WAYS; i++)
        if (c->e[i].id == jid && c->e[i].job) {
            k = i;
            break;
        }
    if (k < 0) {
        for (int i = 0; i < AJ_WAYS; i++)
            if (!c->e[i].job) {
                k = i;
                break;
            }
    }
    if (k < 0)
        k = (int) (c->rr++ % AJ_WAYS);
    c->e[k].id = jid;
    c->e[k].job = job;          // every installed job is remembered by its table id
    if (jid == cur)
        return AJ_OK;
    for (int i = 0; i < AJ_WAYS; i++)
        if (c->e[i].job && c->e[i].id == cur && c->e[i].job != job
            && AJ_READABLE(c->e[i].job + AJ_JOB_ID, 4)
            && *(int32_t *) (c->e[i].job + AJ_JOB_ID) == cur) {
            AJ_XCHG(pj, c->e[i].job);   // this body's own job is still there: back in at once (no NPC-graph frame)
            return AJ_SWAPPED;
        }
    AJ_XCHG(pj, (uint8_t *) 0); // none yet: winmm's builder (0x180028c40, every frame) makes one for the table parked at +0x60b8
    return AJ_DROPPED;
}
#endif
