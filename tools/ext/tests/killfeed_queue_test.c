// Offline stress test of the killfeed.c queue protocol: N writer threads (hooks / MSG_KFWEAPON) + one consumer (the tick),
// same locking and bounds as kf_queue / kf_weapon_recv / feature_killfeed. Checks: the index never leaves [0, 8], nothing is lost or
// duplicated (every pushed id is either consumed once or counted as dropped on a full queue).
// Build: gcc -std=gnu2x -O2 -o killfeed_queue_test killfeed_queue_test.c && ./killfeed_queue_test   (exit 0 = pass)
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef struct {
    int id;
    uint64_t wait_until;
} EvT;
static EvT q[8];
static int qn;
static SRWLOCK lk = SRWLOCK_INIT;
static volatile LONG dropped, pushed, bad;
static int seen[1 << 20];
static void push(int id)
{
    EvT ev = { id, 0 };
    AcquireSRWLockExclusive(&lk);
    if (qn < 0 || qn > 8)
        InterlockedIncrement(&bad);
    if (qn >= 0 && qn < 8) {
        q[qn++] = ev;
        InterlockedIncrement(&pushed);
    } else
        InterlockedIncrement(&dropped);
    ReleaseSRWLockExclusive(&lk);
}

static DWORD WINAPI Writer(LPVOID a)
{
    int base = (int) (intptr_t) a * 100000;
    for (int i = 0; i < 60000; i++) {
        push(base + i);
        if ((i & 7) == 0)
            Sleep(0);
    }
    return 0;
}

static volatile LONG stop;
static DWORD WINAPI Consumer(LPVOID a)
{
    (void) a;
    for (;;) {
        int fin = stop;
        EvT ready[8];
        int nr = 0, n = 0;
        AcquireSRWLockExclusive(&lk);
        if (qn < 0 || qn > 8)
            InterlockedIncrement(&bad);
        for (int i = 0; i < qn && i < 8; i++) {
            if (q[i].wait_until)
                q[n++] = q[i];
            else
                ready[nr++] = q[i];
        }
        qn = n;
        ReleaseSRWLockExclusive(&lk);
        for (int i = 0; i < nr; i++)
            seen[ready[i].id]++;
        if (fin && !nr)
            break;
        Sleep(0);
    }
    return 0;
}

int main(void)
{
    HANDLE w[3], c = CreateThread(NULL, 0, Consumer, NULL, 0, NULL);
    for (int i = 0; i < 3; i++)
        w[i] =
            CreateThread(NULL, 0, Writer, (LPVOID) (intptr_t) (i + 1), 0,
                         NULL);
    WaitForMultipleObjects(3, w, TRUE, INFINITE);
    InterlockedExchange(&stop, 1);
    WaitForSingleObject(c, INFINITE);
    int total = 0, dup = 0;
    for (int i = 0; i < (1 << 20); i++) {
        total += seen[i] > 0;
        dup += seen[i] > 1;
    }
    printf("pushed %ld dropped %ld consumed %d dup %d bad-index %ld\n",
           pushed, dropped, total, dup, bad);
    return (bad || dup || total != pushed) ? 1 : 0;
}
