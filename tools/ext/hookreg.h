// hookreg.h - one registry of every code / data patch the ext writes into an image (tests/test_hook_overlap.py).
// Why: install_sendevent_hook patched the winmm function install_send_hook also patches; the second installer's prologue check
// failed ('send hook: prologue mismatch, not installed') and a build shipped with every ext peer message silently disabled.
// How: t2r_vprot() (a VirtualProtect wrapper, #defined over VirtualProtect right after this header) calls hr_note() for every call that
// makes an IMAGE range writable (exe / winmm / any dll: the stubs and heap objects are private memory and not tracked). hr_note logs
// "HOOK CONFLICT" (once per installer pair) when another installer already patched an overlapping range. The prologue-check failures use
// hook_miss(): "HOOK CONFLICT" when the bytes are already patched by an earlier installer, else "HOOK FAIL" (other exe build / unknown). Both
// prefixes are greppable and make tools/tests/ab_asserts.py FAIL. A re-patch of a range by the SAME function (toggles) is not a conflict.
// Needs before inclusion: <windows.h>, <stdint.h>, <stdio.h>, <stdarg.h>, <string.h>, xlog(), g_xlog_nolimit.
#ifndef T2R_HOOKREG_H
#define T2R_HOOKREG_H
#define HR_MAX 1024
typedef struct {
    uintptr_t lo, hi;
    const char *who;
} HrEnt;
static HrEnt g_hr[HR_MAX];
static int g_hr_n, g_hr_conf, g_hr_fail, g_hr_over;
static volatile LONG g_hrLk;
#define HR_LOG_MAX 200
static int g_hrLogged;
// intentional chains: {installer A, installer B} patching overlapping bytes ON PURPOSE (B wraps / re-patches A). None today: tests/test_hook_overlap.py
// has the static allow-list for the pairs that exist in the source, keep both in step.
static const char *const HR_ALLOW[][2] = { { "", "" } };

static int HrAllowed(const char *a, const char *b)
{
    for (int i = 0; HR_ALLOW[i][0][0]; i++)
        if ((!strcmp(HR_ALLOW[i][0], a) && !strcmp(HR_ALLOW[i][1], b))
            || (!strcmp(HR_ALLOW[i][0], b) && !strcmp(HR_ALLOW[i][1], a)))
            return 1;
    return 0;
}

static void HrLock(void)
{
    for (int i = 0; InterlockedCompareExchange(&g_hrLk, 1, 0) != 0; i++) {
        if (i > 100000)
            return;
        YieldProcessor();
    }
}

static void HrUnlock(void)
{
    InterlockedExchange(&g_hrLk, 0);
}

static void HrSay(const char *fmt, ...)
{
    if (g_hrLogged >= HR_LOG_MAX)
        return;
    g_hrLogged++;
    char b[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    int was = g_xlogNolimit;
    g_xlogNolimit = 1;         // (the release rate limiter keys on the format pointer: these lines are few and must all be written)
    Xlog("%s", b);
    g_xlogNolimit = was;
}

// first registered range overlapping [lo, hi) from another installer (any installer when who == NULL); skips allow-listed pairs
static int HrFind(uintptr_t lo, uintptr_t hi, const char *who,
                   HrEnt *out)
{
    int hit = 0;
    for (int i = 0; i < g_hr_n && !hit; i++)
        if (g_hr[i].lo < hi && lo < g_hr[i].hi
            && (!who || !HrAllowed(g_hr[i].who, who))) {
            *out = g_hr[i];
            hit = 1;
        }
    return hit;
}

// has this installer already recorded exactly this range? (a repeated patch of the same bytes: the caller skips the image query)
static int HrHave(const void *p, size_t n, const char *who)
{
    uintptr_t lo = (uintptr_t) p, hi = lo + n;
    int r = 0;
    HrLock();
    for (int i = 0; i < g_hr_n && !r; i++)
        if (g_hr[i].lo == lo && g_hr[i].hi == hi
            && !strcmp(g_hr[i].who, who))
            r = 1;
    HrUnlock();
    return r;
}

// an installer is about to write [p, p+n): record it, report an overlap with another installer's range (once per pair)
static void HrNote(const void *p, size_t n, const char *who)
{
    uintptr_t lo = (uintptr_t) p, hi = lo + n;
    if (!n || hi < lo)
        return;
    HrEnt o = { 0, 0, NULL };
    int hit = 0, dup = 0;
    HrLock();
    for (int i = 0; i < g_hr_n; i++) {
        if (g_hr[i].lo < hi && lo < g_hr[i].hi) {
            if (!strcmp(g_hr[i].who, who)) {
                dup = 1;
                continue;
            }                   // the same function again (toggle / second site): fine
            if (!HrAllowed(g_hr[i].who, who) && !hit) {
                o = g_hr[i];
                hit = 1;
            }
        }
    }
    int exact = 0;
    for (int i = 0; i < g_hr_n; i++)
        if (g_hr[i].lo == lo && g_hr[i].hi == hi
            && !strcmp(g_hr[i].who, who))
            exact = 1;
    if (!exact) {
        if (g_hr_n < HR_MAX)
            g_hr[g_hr_n++] = (HrEnt) {
            lo, hi, who};
        else
        g_hr_over++;
    }
    if (hit)
        g_hr_conf++;
    HrUnlock();
    (void) dup;
    if (hit)
        HrSay
            ("HOOK CONFLICT: %s patches %p+%u but %s already patched %p+%u (two installers on the same bytes: the second one's prologue check fails or it corrupts the first)",
             who, (void *) lo, (unsigned) n, o.who, (void *) o.lo,
             (unsigned) (o.hi - o.lo));
}

// a prologue / signature check failed at [p, p+n): HOOK CONFLICT when an earlier installer already patched those bytes, else HOOK FAIL
static void HookMiss(const void *p, size_t n, const char *who,
                       const char *fmt, ...)
{
    char msg[500];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    HrEnt o = { 0, 0, NULL };
    int hit = 0;
    if (p && n) {
        HrLock();
        hit = HrFind((uintptr_t) p, (uintptr_t) p + n, who, &o);
        g_hr_fail++;
        if (hit)
            g_hr_conf++;
        HrUnlock();
    } else {
        HrLock();
        g_hr_fail++;
        HrUnlock();
    }
    if (hit)
        HrSay
            ("HOOK CONFLICT: %s: %s [bytes %p+%u were already patched by %s (%p+%u) - installer order / overlap]",
             who, msg, p, (unsigned) n, o.who, (void *) o.lo,
             (unsigned) (o.hi - o.lo));
    else
        HrSay("HOOK FAIL: %s", msg);
    if (strstr(msg, "stub memory") || strstr(msg, "cave memory")
        || strstr(msg, "within reach") || strstr(msg, "rel32"))
        HrSay
            ("!!!!!! STUB POOL: a hook could not get memory within rel32 reach (%s) - the winmm .t2rstub section is missing/full or the exe pool failed; see 'stub pool:' lines",
             who);
}

static void HrSummary(void)
{
    HrSay
        ("HOOK registry: %d patched range(s) recorded, %d conflict(s), %d failed check(s)%s",
         g_hr_n, g_hr_conf, g_hr_fail, g_hr_over ? " (table full)" : "");
}

#define HOOK_MISS(p, n, ...) HookMiss((p), (size_t)(n), __func__, __VA_ARGS__)
#define HOOK_FAIL(...) HookMiss(NULL, 0, __func__, __VA_ARGS__)
#endif
