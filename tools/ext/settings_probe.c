// settings_probe.c (dev): ND SettingsControl slot-pool probe (int3 at exe+1552b4d = SettingsControl::AllocSetting assert
// "iSlotIndex < m_maxNumSettings", settings.cpp 0xae, "Ran out of settings! index %d max %d", menu, after many menu-world reloads).
// Layout (disassembled from exe+15529f0 AllocSetting): the control object is the exe global at exe+8d60110 (the thunks at 1551060.. pass it in rcx).
// Pool p (the edx argument) owns a 0x200-byte used-bitmap at obj + p*0x200 (64 qwords, bit set = slot used; AllocSetting takes the first zero bit via
// not/tzcnt, then bts), its slot array pointer at obj + 0x9608 + p*8 (slot stride 0xA0), and the limit m_maxNumSettings is a u32 at obj + 0x9990.
// Every 5 s while t2r-settings-probe.txt exists next to the exe: popcount of each pool's bitmap; a line "settings probe: ..." (xlog) when any pool changed
// (pool:used, max, total), plus the first sample. A leak shows as a pool whose count rises by a fixed step at every menu world load and never falls.
// Wiring (tools/ext/t2r-ext.c, next to the other per-frame PF() ticks, e.g. after PF(melee_defmax_tick());):
//   #include "settings_probe.c"      (anywhere above the tick, after xlog)
//   PF(settings_probe_tick());
// ---- settings bucket guard (crash: int3 at exe+15538c1 = SettingsControl::NewSetting assert
// "m_numUsedBuckets < kMaxSettingsBuckets", settings.cpp 0x14d, on a worker thread inside a fist wall pair's victim half).
// NewSetting (exe+15534f0: rcx = the control object exe+8d60110, r8 = the setting's value address, 9th argument = the priority) keeps ONE bucket per
// distinct priority: the priority list is u32[] at obj+0x9860, its count at obj+0x998c, max 0x4b (75); a value address inside
// [exe+834fb70, exe+83541b0) gets +0x791a on its priority first. Buckets are freed when empty (A/B probe: at most 22 in use, max index 21, over
// whole sessions), so 75 means a burst of distinct priorities in one frame. The hook:
//  - logs once per 2 s when the bucket count passes 40: the new priority, the caller (return address) and the whole priority list;
//  - when all 75 buckets are taken and the priority is new, the setting goes into the nearest existing priority instead of the engine's assert
//    (one line per 2 s, "settings guard: ... REDIRECTED"). The value then blends with a neighbouring priority, which beats a crash.
// Opt-out: t2r-settings-guard-off.txt next to the exe (no hook).
typedef uint64_t(*fn_newset) (uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t, uint64_t);
static fn_newset g_newsetTramp;
static volatile LONG g_sguardRedirects;
static uint64_t NewsetHook(uint64_t obj, uint64_t a2, uint64_t val,
                            uint64_t a4, uint64_t a5, uint64_t a6,
                            uint64_t a7, uint64_t a8, uint64_t prio,
                            uint64_t a10, uint64_t a11, uint64_t a12)
{
    uint8_t *o = (uint8_t *) obj, *exe =
        (uint8_t *) GetModuleHandleA(NULL);
    uint32_t n = *(volatile uint32_t *) (o + 0x998c);
    if (n > 40 && n <= 0x4b) {
        uint32_t adj =
            (uint32_t) prio + (val >= (uint64_t) (exe + 0x834fb70)
                               && val <
                               (uint64_t) (exe + 0x83541b0) ? 0x791a : 0);
        const uint32_t *pl = (const uint32_t *) (o + 0x9860);
        int found = 0;
        for (uint32_t i = 0; i < n; i++)
            if (pl[i] == adj) {
                found = 1;
                break;
            }
        if (!found) {
            static volatile ULONGLONG last_log;
            ULONGLONG now = GetTickCount64();
            void *ra = __builtin_return_address(0);
            uint32_t best = adj;
            int redirect = 0;
            if (n >= 0x4b) {    // full: pick the nearest existing priority this address can reach (an in-range address only reaches buckets >= 0x791a)
                uint32_t off = adj - (uint32_t) prio;
                uint64_t bd = ~0ULL;
                for (uint32_t i = 0; i < n; i++) {
                    if (pl[i] < off)
                        continue;
                    uint64_t d = pl[i] > adj ? pl[i] - adj : adj - pl[i];
                    if (d < bd) {
                        bd = d;
                        best = pl[i];
                    }
                }
                if (bd != ~0ULL) {
                    prio =
                        (prio & ~0xffffffffULL) | (uint64_t) (best - off);
                    redirect = 1;
                    InterlockedIncrement(&g_sguardRedirects);
                }
            }
            if (now - last_log >= 2000) {
                last_log = now;
                char b[760];
                size_t k = 0;
                for (uint32_t i = 0; i < n && k < sizeof b - 12; i++)
                    k += (size_t) snprintf(b + k, sizeof b - k, " %u",
                                           pl[i]);
                uintptr_t rr = (uintptr_t) ra - (uintptr_t) exe;
                Xlog("settings guard: %u of 75 buckets in use, new priority %u (value at exe+%llx, caller exe+%llx, thread %lu)%s%s; priorities:%s", n, adj, (unsigned long long) (val - (uint64_t) exe), (unsigned long long) rr, GetCurrentThreadId(), redirect ? " - FULL, REDIRECTED to " : "", redirect ? "the nearest bucket" : "", b);
                if (redirect)
                    Xlog("settings guard: redirected to priority %u (%ld redirects so far)", best, (long) g_sguardRedirects);
            }
        }
    }
    return g_newsetTramp(obj, a2, val, a4, a5, a6, a7, a8, prio, a10, a11,
                          a12);
}

static void SettingsGuardInstall(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    if (GetFileAttributesA("t2r-settings-guard-off.txt") !=
        INVALID_FILE_ATTRIBUTES) {
        Xlog("settings guard: off (t2r-settings-guard-off.txt)");
        return;
    }
    // mov [rsp+10h], rbx; push rbp; push rsi; push rdi; push r12; push r13; push r14; push r15 (16 bytes, no rip-relative operand)
    static const uint8_t P[16] =
        { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
0x55, 0x41, 0x56, 0x41, 0x57 };
    int ok =
        HfixPatch(0x15534F0, P, 16, (void *) NewsetHook,
                   (void **) &g_newsetTramp, "settings NewSetting");
    Xlog("settings guard: NewSetting hook %s",
         ok ? "installed (log past 40 buckets, redirect at 75)" :
         "NOT installed (prologue mismatch)");
}

static void SettingsProbeTick(void)
{
    static uint64_t at;
    static int on = -1;
    static uint64_t chk;
    static uint16_t last[80];
    static int have;
    SettingsGuardInstall();
    uint64_t now = GetTickCount64();
    if (now - at < 5000)
        return;
    at = now;
    if (now - chk > 15000 || on < 0) {
        chk = now;
        on = GetFileAttributesA("t2r-settings-probe.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (!on)
        return;
    uint8_t *obj = (uint8_t *) GetModuleHandleA(NULL) + 0x8d60110;
    if (IsBadReadPtr(obj + 0x9608, 0x390))
        return;
    uint32_t mx = *(volatile uint32_t *) (obj + 0x9990);
    uint16_t cur[80];
    int total = 0, changed = !have, np = 0;
    for (int p = 0; p < 75; p++) {
        const uint64_t *bm = (const uint64_t *) (obj + (size_t) p * 0x200);
        if (IsBadReadPtr(bm, 0x200)) {
            cur[p] = 0;
            continue;
        }
        int n = 0;
        for (int k = 0; k < 64; k++)
            n += __builtin_popcountll(bm[k]);
        cur[p] = (uint16_t) n;
        total += n;
        np = p + 1;
        if (cur[p] != last[p])
            changed = 1;
    }
    if (!changed)
        return;
    char b[900];
    size_t o = 0;
    for (int p = 0; p < np && o < sizeof b - 24; p++)
        if (cur[p])
            o += (size_t) snprintf(b + o, sizeof b - o, " %d:%u", p,
                                   (unsigned) cur[p]);
    memcpy(last, cur, sizeof cur);
    have = 1;
    Xlog("settings probe: max %u, used total %d, pools (index:used)%s", mx,
         total, b);
}
