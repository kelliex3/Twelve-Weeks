// Joel locomotion for the MALE looks (re/joelanims/ANALYSIS.md, tools/models/joel_mm.py). Included by t2r-ext.c after the tick helpers.
//
// Data (build, T2R_JOEL_MM=1): five renamed paks anim-joel-player-mm-* are resident in every MP match. They hold the contributed Part I Joel clips under
// NEW names plus rebuilt pose databases (MOTION_MATCH_SET_1 objects joel-player-<x>), so a remote viewer can resolve the clips a male sender's
// AnimCmdList names, whatever his own look is. Nothing else changes: melee, prone, aim, guns and the injured sets stay on the stock Abby / Ellie data.
//
// Selection: the locomotion settings DC (dc1/motion-matching-settings/motion-matching-settings-abby.bin + -ellie.bin) holds one 0x708-byte record per
// motion-matching set; the record's qword at +0x6f8 is the NAME of the pose database (the pak's MOTION_MATCH_SET_1 object, e.g. abby-player-explore-walk),
// +0x700 is the record's DC type sid 0x6c8560e6f65267ec. Several rows share a record (explore-walk: 3, explore-run: 5, ...). For a male life this module
// rewrites +0x6f8 of the resident records from the stock set sid to the joel set sid; for a female life (or t2r-no-joel-mm.txt) it writes the stock sid
// back. Only our OWN player runs motion matching (remote players are puppets replaying the sender's clip names), so a per-process switch is per player.
//
// UNCERTAIN (offline static work could not settle it; the log answers it in one A/B run):
//   1. that the resident records still hold the sid (not a resolved pointer): "joelmm: scan" finds N slots per set; 0 = the loader resolved them;
//   2. that the motion matcher re-reads the record at (re)initialisation, i.e. a switch before the spawn takes effect for the life ("joelmm: apply" lines
//      carry the frame and the match substate; the visible gait must change on the next respawn - if it only changes after a state change, the matcher
//      caches the pose database pointer and the hook must move to the lookup, see re/joelanims/live-trace.md).
// Log prefix "joelmm:". t2r-no-joel-mm.txt next to the exe = always stock (re-read every 2 s).

#define JM_N 9
#define JM_SLOTS 16
static const struct {
    const char *stock, *joel;
} JM_SETS[JM_N] = {
    { "abby-player-explore-run", "joel-player-explore-run" },
        { "abby-player-explore-walk", "joel-player-explore-walk" },
    { "abby-player-tense-run", "joel-player-tense-run" },
        { "ellie-player-crouch", "joel-player-crouch" },
    { "ellie-player-tense-sprint", "joel-player-tense-sprint" },
        { "ellie-player-tense-walk", "joel-player-tense-walk" },
    { "ellie-player-tense-goto-smallstep",
     "joel-player-tense-goto-smallstep" },
    { "ellie-player-uneasy-run", "joel-player-uneasy-run" },
        { "ellie-player-uneasy-walk", "joel-player-uneasy-walk" },
};

#define JM_TYPE 0x6c8560e6f65267ecULL
static uint64_t jm_stock[JM_N], jm_joel[JM_N];
static uint64_t *volatile jm_slot[JM_N][JM_SLOTS];
static volatile int jm_nslot[JM_N];
static volatile int jm_scanning, jm_scan_done, jm_scan_gen;
static int jm_tries, jm_state = -1;     // jm_state: 1 = joel written, 0 = stock written, -1 = nothing yet
static uint64_t jm_next_check, jm_off_at;
static int jm_off;

static DWORD WINAPI JmScan(void *arg)
{
    (void) arg;
    uint64_t t0 = GetTickCount64();
    MEMORY_BASIC_INFORMATION mi;
    uint8_t *a = NULL;
    static uint8_t buf[1 << 20];
    uint64_t read_mb = 0;
    int n_all = 0, ro_hits = 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (VirtualQuery(a, &mi, sizeof mi) == sizeof mi) {
        uint8_t *b = (uint8_t *) mi.BaseAddress;
        if (mi.State == MEM_COMMIT
            && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            && (mi.
                Protect & (PAGE_READWRITE | PAGE_READONLY |
                           PAGE_WRITECOPY)) && mi.Type != MEM_IMAGE) {
            int rw = (mi.Protect & PAGE_READWRITE) != 0;
            for (size_t off = 0; off < mi.RegionSize; off += sizeof buf) {
                SIZE_T want =
                    mi.RegionSize - off <
                    sizeof buf ? mi.RegionSize - off : sizeof buf, got = 0;
                if (!ReadProcessMemory
                    (GetCurrentProcess(), b + off, buf, want, &got)
                    || got < 16)
                    continue;
                read_mb += got;
                for (size_t i = 0; i + 16 <= got; i += 8) {
                    if (*(uint64_t *) (buf + i + 8) != JM_TYPE)
                        continue;
                    uint64_t v = *(uint64_t *) (buf + i);
                    for (int k = 0; k < JM_N; k++) {
                        if (v != jm_stock[k] && v != jm_joel[k])
                            continue;
                        if (!rw) {      // a read-only / copy-on-write copy (a mapped file?): counted and logged, never written
                            if (++ro_hits <= 6)
                                Xlog("joelmm: scan - %s at %p is in a NON-writable region (type %lx prot %lx): not patched", JM_SETS[k].stock, (void *) (b + off + i), (unsigned long) mi.Type, (unsigned long) mi.Protect);
                            break;
                        }
                        if (jm_nslot[k] < JM_SLOTS)
                            jm_slot[k][jm_nslot[k]] =
                                (uint64_t *) (b + off + i);
                        jm_nslot[k]++;
                        n_all++;
                        // context for the live check: the record's first floats (+0x00 flag, +0x04..) are plain data in the file; a record shows
                        // them 0x6f8 bytes below the slot. Log the first 3 hits per set.
                        if (jm_nslot[k] <= 3
                            && (uint8_t *) (b + off + i) - 0x6f8 >=
                            b + off + 0 && i >= 0x6f8)
                            Xlog("joelmm: scan hit %s #%d at %p (%s), record start %02x%02x%02x%02x %02x%02x%02x%02x, region type %lx prot %lx", JM_SETS[k].stock, jm_nslot[k], (void *) (b + off + i), v == jm_joel[k] ? "joel" : "stock", buf[i - 0x6f8], buf[i - 0x6f8 + 1], buf[i - 0x6f8 + 2], buf[i - 0x6f8 + 3], buf[i - 0x6f8 + 4], buf[i - 0x6f8 + 5], buf[i - 0x6f8 + 6], buf[i - 0x6f8 + 7], (unsigned long) mi.Type, (unsigned long) mi.Protect);
                        break;
                    }
                }
                Sleep(0);
            }
        }
        a = b + mi.RegionSize;
        if ((uint64_t) a >= 0x7fffffff0000ULL)
            break;
    }
    Xlog("joelmm: scan #%d done in %llu ms (read %llu MB): %d record slot(s), %d read-only copies%s", jm_tries, (unsigned long long) (GetTickCount64() - t0), (unsigned long long) (read_mb >> 20), n_all, ro_hits, n_all ? "" : " - the resident settings hold no pose-database sid (loader resolves them?), the module stays idle");
    for (int k = 0; k < JM_N; k++)
        Xlog("joelmm:   %-36s %d slot(s)", JM_SETS[k].stock, jm_nslot[k]);
    jm_scan_done = 1;
    jm_scanning = 0;
    return 0;
}

// write `joel` (1) or the stock sids (0) into every slot that still holds one of the two sids
static int JmApply(int joel)
{
    int changed = 0, bad = 0;
    for (int k = 0; k < JM_N; k++) {
        int n = jm_nslot[k] < JM_SLOTS ? jm_nslot[k] : JM_SLOTS;
        for (int s = 0; s < n; s++) {
            uint64_t *p = jm_slot[k][s];
            if (!p || IsBadWritePtr(p, 16)) {
                bad++;
                continue;
            }
            uint64_t cur = p[0];
            if (p[1] != JM_TYPE
                || (cur != jm_stock[k] && cur != jm_joel[k])) {
                bad++;
                continue;
            }                   // the block was freed / reused
            uint64_t want = joel ? jm_joel[k] : jm_stock[k];
            if (cur != want) {
                InterlockedExchange64((volatile LONG64 *) p,
                                      (LONG64) want);
                changed++;
            }
        }
    }
    if (bad) {
        // stale slots (level change re-created the DC): scan again, a few times at most
        jm_scan_done = 0;
        for (int k = 0; k < JM_N; k++)
            jm_nslot[k] = 0;
    }
    Xlog("joelmm: apply %s: %d record(s) rewritten, %d stale slot(s) (frame %llu, match substate %d)", joel ? "JOEL (male life)" : "stock", changed, bad, (unsigned long long) g_frame, (int) MatchSubstate());
    return bad;
}

// Built without the joel paks (T2R_JOEL_MM=0), or the ext compiled before any DLL build: the module is inert. Rewriting the sids then points the
// motion matcher at pose databases that are not resident (night70: sprint carried on walking with no movement key held).
#if __has_include("joelmm_built.h")
#include "joelmm_built.h"
#endif
#ifndef JOELMM_BUILT
#define JOELMM_BUILT 0
#endif

static void JoelmmTick(void)
{
    if (!JOELMM_BUILT)
        return;
    if (!jm_stock[0]) {
        for (int k = 0; k < JM_N; k++) {
            jm_stock[k] = FNV64(JM_SETS[k].stock);
            jm_joel[k] = FNV64(JM_SETS[k].joel);
        }
    }
    if (g_frame < jm_next_check)
        return;
    jm_next_check = g_frame + 30;
    uint8_t st = MatchSubstate();
    if (st < 3 || st > 6) {
        jm_state = -1;
        return;
    }                           // not in a match: the DC is rewritten per level anyway
    uint64_t now = GetTickCount64();
    if (now - jm_off_at > 2000) {
        jm_off_at = now;
        jm_off =
            GetFileAttributesA("t2r-no-joel-mm.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (!jm_scan_done) {
        if (!jm_scanning && jm_tries < 4) {
            jm_tries++;
            jm_scanning = 1;
            for (int k = 0; k < JM_N; k++)
                jm_nslot[k] = 0;
            HANDLE h = CreateThread(NULL, 0, JmScan, NULL, 0, NULL);
            if (h)
                CloseHandle(h);
            else
                jm_scanning = 0;
        }
        return;
    }
    // male unless the life in progress (or, before the first life, the coming one) is a female look; faction looks off = winmm's looks = male
    int female = !FactionLooksOff()
        && IdxIsFemale(g_fl.seq ? g_fl.cur : g_fl.next);
    int want = !female && !jm_off;
    if (want == jm_state)
        return;
    JmApply(want);
    jm_state = want;
    Xlog("joelmm: own life is %s (look %s)%s -> %s locomotion",
         female ? "FEMALE" : "male",
         FactionLooksOff()? "-" : FlName(g_fl.team,
                                            g_fl.seq ? g_fl.cur : g_fl.
                                            next),
         jm_off ? ", t2r-no-joel-mm.txt" : "", want ? "Joel" : "stock");
}
