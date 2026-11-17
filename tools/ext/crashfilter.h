// crashfilter.h - decides WHEN the ext's crash report runs.
// A vectored handler sees every first-chance exception, including the ones the engine / Windows / our own IsBadReadPtr probes handle themselves; running the
// heavy report (and the one-shot t2r-crash-<pid>-1.bin, crashbin.h) there spent the 4-report budget and the one-shot file on harmless exceptions and hitched
// the match. Now:
//   * cf_veh (first-chance, AddVectoredExceptionHandler): the cheap candidate filter (codes, kernel32 / kernelbase AVs, foreign int3) + a rate-limited one-line
//     note + re-arming the top-level filter; it never reports. Two classes are reported right there as before, because no last chance can be relied on:
//     stack overflow (no stack left for anything later) and int3 inside the exe (the engine's fatal asserts).
//   * cf_uef (the process's top-level filter, SetUnhandledExceptionFilter): runs only when no frame handler took the exception = the process dies. It reports
//     (all codes: also heap corruption, uncaught C++ throws) and then CALLS THE FILTER THAT WAS INSTALLED BEFORE US (the game's dump writer) and returns its value,
//     so the game's own .mdmp is written as before. cf_arm() re-installs us whenever someone else replaced us (called at init and on every candidate).
// No heap, no CRT stdio, no lock here: the report callback does the rest.
#ifndef T2R_CRASHFILTER_H
#define T2R_CRASHFILTER_H
#include <windows.h>
#include <stdint.h>

typedef void (*cf_report_fn)(EXCEPTION_POINTERS * ep);  // the heavy report (once per exception; the callback owns its budget)
typedef void (*cf_note_fn)(EXCEPTION_POINTERS * ep);    // cheap one-line first-chance note
static cf_report_fn cf_report;
static cf_note_fn cf_note;
static LPTOP_LEVEL_EXCEPTION_FILTER cf_prev;    // the filter installed before ours (the game's dump writer)
static volatile LONG cf_uef_owner;      // tid inside cf_uef (a filter chain that loops back to us must not recurse)
static volatile LONG cf_notes;  // first-chance notes written
static volatile LONG cf_rep_code, cf_rep_tid;   // last exception reported (dedupe: an int3 reported by the VEH also reaches the top-level filter)
static volatile uintptr_t cf_rep_addr;
#define CF_MAX_NOTES 8

static LONG WINAPI CfUef(EXCEPTION_POINTERS * ep);

static int CfCodeOk(const void *p)
{                               // committed executable memory (VirtualQuery: no SEH, no heap)
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT)
        return 0;
    return (mi.
            Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) !=
        0 && !(mi.Protect & PAGE_GUARD);
}

static void CfArm(void)
{
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(CfUef);
    if (cur && cur != CfUef)
        cf_prev = cur;
}

static void CfInit(cf_report_fn rep, cf_note_fn note)
{
    cf_report = rep;
    cf_note = note;
    CfArm();
}

// 0 = not interesting, 1 = candidate (reported only if it turns out unhandled), 2 = report now
static int CfClassify(const EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_BREAKPOINT)  // (the engine's fatal asserts are int3 = 0x80000003)
        return 0;
    if (code == EXCEPTION_STACK_OVERFLOW)
        return 2;
    HMODULE m = NULL;
    BOOL ok =
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR) ep->ExceptionRecord->ExceptionAddress,
                           &m) && m;
    if (code == EXCEPTION_BREAKPOINT)   // only the engine's own asserts; an int3 a library raises and handles itself is ignored
        return ok && m == GetModuleHandleW(NULL) ? 2 : 0;
    if (code == EXCEPTION_ACCESS_VIOLATION && ok &&     // IsBadReadPtr / IsBadWritePtr probes: handled AVs inside KERNEL32 / KERNELBASE
        (m == GetModuleHandleW(L"kernel32.dll")
         || m == GetModuleHandleW(L"kernelbase.dll")))
        return 0;
    return 1;
}

// returns 1 when this exception (code, address, thread) was already handed to cf_report
static int CfDup(const EXCEPTION_POINTERS *ep)
{
    LONG code = (LONG) ep->ExceptionRecord->ExceptionCode, tid =
        (LONG) GetCurrentThreadId();
    uintptr_t a = (uintptr_t) ep->ExceptionRecord->ExceptionAddress;
    if (cf_rep_code == code && cf_rep_tid == tid && cf_rep_addr == a)
        return 1;
    cf_rep_code = code;
    cf_rep_tid = tid;
    cf_rep_addr = a;
    return 0;
}

static LONG CALLBACK CfVeh(EXCEPTION_POINTERS *ep)
{
    int c = CfClassify(ep);
    if (!c)
        return EXCEPTION_CONTINUE_SEARCH;
    if (c == 2) {
        if (cf_report && !CfDup(ep))
            cf_report(ep);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CfArm();                   // make sure we are the top-level filter when this one turns out fatal (the game may have replaced us)
    if (cf_note && InterlockedIncrement(&cf_notes) <= CF_MAX_NOTES)
        cf_note(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI CfUef(EXCEPTION_POINTERS *ep)
{
    LONG tid = (LONG) GetCurrentThreadId();
    if (cf_uef_owner == tid)
        return EXCEPTION_CONTINUE_SEARCH;       // this thread's filter chain looped back to us (never block another thread: it still must reach the game's writer)
    InterlockedExchange(&cf_uef_owner, tid);
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (cf_report && code != 0x406D1388 && code != 0x40010006
        && !CfDup(ep))
        cf_report(ep);
    LPTOP_LEVEL_EXCEPTION_FILTER p = cf_prev;
    LONG r = EXCEPTION_CONTINUE_SEARCH;
    if (p && p != CfUef && CfCodeOk(p))
        r = p(ep);              // a stale pointer (the module that installed it was unloaded: overlay, driver) must not fault inside the crash path
    InterlockedCompareExchange(&cf_uef_owner, 0, tid);
    return r;
}
#endif
