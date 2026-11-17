// Part I side files (T2R_P1_SIDE builds, re/part1/side-files.md, tools/features/p1_side.py).
//
// The Part I-derived files (level paks, audio banks / streams, converted actors, render-settings pak) are NOT in the winmm blob in such a
// build: the launcher puts them at <exe dir>\t2r-maps\part1\<in-game relative path> and writes <exe dir>\t2r-maps\part1\manifest.txt LAST:
//     p1side 1 <n> <total bytes>
//     <rel path>\t<size>\t<sha256>          (n lines)
// This module only DECIDES and LOOKS UP; it does no hooking:
//   p1s_scan()            once, at start: manifest present, its CRC-32 == T2R_P1_SIDE_CRC (the manifest this DLL was built against), every entry's file
//                         exists with exactly its size (cheap; sha256 is the launcher's job) -> g_p1s_ok, else the Part I maps are switched off
//   p1s_ok()              1 = complete and matching (or the build has no side mode: T2R_P1_SIDE_BUILT 0 = the maps, if any, are in the blob)
//   p1s_find(rel, &size)  the entry for an in-game relative path ("pak68/hun-checkpoint-bookstore.pak") or NULL; -> full path in e->full
//   p1s_read(e, off, buf, n)  size / seek / read of a side file (the engine-facing read for a served open; thread safe, no shared handle)
//   p1s_mask_filter(m)    clears the SP-map bits (T2R_P1_SIDE_MASK) of a map-support mask when the side files are not ok
// Without p1side_built.h (an ext compiled before any DLL build) the module is inert: p1s_ok() = 1, mask untouched.
// Needs from the including file: <windows.h>, <stdio.h>, <string.h>, <stdint.h>, <stdlib.h> and P1S_LOG(fmt, ...) (default: xlog).
#ifndef T2R_P1SIDE_H
#define T2R_P1SIDE_H

#if defined(__has_include)
#if __has_include("p1side_built.h")
#include "p1side_built.h"
#endif
#endif
#ifndef T2R_P1_SIDE_BUILT
#define T2R_P1_SIDE_BUILT 0
#define T2R_P1_SIDE_CRC 0u
#define T2R_P1_SIDE_MASK 0u
#endif
#ifndef P1S_LOG
#define P1S_LOG Xlog
#endif

typedef struct {
    char rel[128];
    char full[MAX_PATH];
    uint64_t size;
} P1sEntry;
static P1sEntry *g_p1sE;
static int g_p1sN;
static int g_p1sOk = -1;       // -1 = not scanned yet

static uint32_t P1sCrc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static int P1sPathok(const char *p)
{                               // relative, forward slashes, no "..", no drive / stream colon
    if (!p[0] || p[0] == '/' || strchr(p, '\\') || strchr(p, ':'))
        return 0;
    for (const char *s = p; *s;) {
        if (s[0] == '.' && s[1] == '.' && (s[2] == '/' || s[2] == 0))
            return 0;
        const char *sl = strchr(s, '/');
        if (!sl)
            break;
        s = sl + 1;
    }
    return 1;
}

static int P1sCmp(const void *a, const void *b)
{
    return strcmp(((const P1sEntry *) a)->rel,
                  ((const P1sEntry *) b)->rel);
}

// Parse a manifest text (modifies nothing; text need not be NUL terminated beyond n). dir = "...\t2r-maps\part1" (no trailing slash).
// Returns the number of entries or -1 (bad). Entries are sorted by path.
static int P1sParse(const char *text, size_t n, const char *dir,
                     uint64_t want_n, P1sEntry **out, uint64_t *total_out)
{
    char *t = (char *) malloc(n + 1);
    if (!t)
        return -1;
    memcpy(t, text, n);
    t[n] = 0;
    char *save = NULL;
    unsigned long long hn = 0, htot = 0;
    int ver = 0, ok = 1, cnt = 0;
    char *ln = t;
    P1sEntry *e = NULL;
    uint64_t tot = 0;
    for (int first = 1; ln && *ln && ok; first = 0) {
        char *nl = strchr(ln, '\n');
        if (nl)
            *nl = 0;
        size_t L = strlen(ln);
        if (L && ln[L - 1] == '\r')
            ln[L - 1] = 0;
        if (first) {
            if (sscanf(ln, "p1side %d %llu %llu", &ver, &hn, &htot) != 3
                || ver != 1 || hn == 0 || hn > 4096 || (want_n
                                                        && hn != want_n)) {
                ok = 0;
                break;
            }
            e = (P1sEntry *) calloc((size_t) hn, sizeof *e);
            if (!e) {
                ok = 0;
                break;
            }
        } else {
            char *c1 = strchr(ln, '\t'), *c2 =
                c1 ? strchr(c1 + 1, '\t') : NULL;
            if (!c1 || !c2 || cnt >= (int) hn) {
                ok = 0;
                break;
            }
            *c1 = 0;
            *c2 = 0;
            if (strlen(ln) >= sizeof e[0].rel || !P1sPathok(ln)
                || strlen(c2 + 1) != 64) {
                ok = 0;
                break;
            }
            char *end = NULL;
            e[cnt].size = strtoull(c1 + 1, &end, 10);
            if (!end || *end) {
                ok = 0;
                break;
            }
            snprintf(e[cnt].rel, sizeof e[cnt].rel, "%s", ln);
            int w =
                snprintf(e[cnt].full, sizeof e[cnt].full, "%s\\%s", dir,
                         ln);
            if (w <= 0 || w >= (int) sizeof e[cnt].full) {
                ok = 0;
                break;
            }
            for (char *q = e[cnt].full + strlen(dir) + 1; *q; q++)
                if (*q == '/')
                    *q = '\\';
            tot += e[cnt].size;
            cnt++;
        }
        ln = nl ? nl + 1 : NULL;
    }
    (void) save;
    if (ok && (cnt != (int) hn || tot != htot))
        ok = 0;
    free(t);
    if (!ok || !e) {
        free(e);
        return -1;
    }
    qsort(e, (size_t) cnt, sizeof *e, P1sCmp);
    for (int i = 1; i < cnt; i++)
        if (!strcmp(e[i - 1].rel, e[i].rel)) {
            free(e);
            return -1;
        }                       // duplicate path
    *out = e;
    if (total_out)
        *total_out = tot;
    return cnt;
}

// Full check of an installed tree: manifest CRC against the build, every file's size. Returns 1 ok; `why` gets the first problem.
static int P1sCheck(const char *dir, uint32_t want_crc, uint64_t want_n,
                     char *why, size_t whyn)
{
    char mp[MAX_PATH];
    snprintf(mp, sizeof mp, "%s\\manifest.txt", dir);
    FILE *f = fopen(mp, "rb");
    if (!f) {
        snprintf(why, whyn, "no manifest (side files not installed)");
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (8 << 20)) {
        fclose(f);
        snprintf(why, whyn, "manifest size %ld", sz);
        return 0;
    }
    uint8_t *buf = (uint8_t *) malloc((size_t) sz);
    if (!buf || fread(buf, 1, (size_t) sz, f) != (size_t) sz) {
        fclose(f);
        free(buf);
        snprintf(why, whyn, "manifest unreadable");
        return 0;
    }
    fclose(f);
    if (want_crc && P1sCrc32(buf, (size_t) sz) != want_crc) {
        snprintf(why, whyn,
                 "manifest crc %08x != build %08x (other version)",
                 P1sCrc32(buf, (size_t) sz), want_crc);
        free(buf);
        return 0;
    }
    P1sEntry *e = NULL;
    int n =
        P1sParse((const char *) buf, (size_t) sz, dir, want_n, &e, NULL);
    free(buf);
    if (n < 0) {
        snprintf(why, whyn, "manifest malformed");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExA(e[i].full, GetFileExInfoStandard, &fa)) {
            snprintf(why, whyn, "missing %s", e[i].rel);
            free(e);
            return 0;
        }
        if ((((uint64_t) fa.nFileSizeHigh << 32) | fa.nFileSizeLow) !=
            e[i].size) {
            snprintf(why, whyn, "size mismatch %s", e[i].rel);
            free(e);
            return 0;
        }
    }
    free(g_p1sE);
    g_p1sE = e;
    g_p1sN = n;
    return 1;
}

// ---- virtual-region builds (T2R_P1_SIDE_BUILT 2): winmm's loader cave (tools/features/p1_side_map.py) maps part1.dat behind the blob at boot and
// leaves its verdict in the descriptor at the start of section ".t2rps": u32 'P1SD', u32 ver, u32 status (0 not run, 1 mapped, 2 absent, 3 failed),
// u32 layout (1 view1 + reserved hole, 2 blob in image, 3 full), u32 step, u32 err, u64 size1, u64 size2, u64 view base, u64 ptr, u64 delta, u64 actual.
typedef struct {
    uint32_t status, layout, step, err;
    uint64_t size1, size2, base, ptr, delta, actual;
} P1sDesc;
static int P1sReadDesc(const uint8_t *m, P1sDesc *d)
{                               // 1 = descriptor found in the module image at m
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *) m;
    if (!m || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    const IMAGE_NT_HEADERS64 *nt =
        (const IMAGE_NT_HEADERS64 *) (m + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    const IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sh[i].Name, ".t2rps", 7))
            continue;
        const uint8_t *s = m + sh[i].VirtualAddress;
        if (*(const uint32_t *) s != 0x44533150u)
            return 0;
        memset(d, 0, sizeof *d);
        d->status = *(const uint32_t *) (s + 8);
        d->layout = *(const uint32_t *) (s + 12);
        d->step = *(const uint32_t *) (s + 16);
        d->err = *(const uint32_t *) (s + 20);
        d->size1 = *(const uint64_t *) (s + 0x18);
        d->size2 = *(const uint64_t *) (s + 0x20);
        d->base = *(const uint64_t *) (s + 0x28);
        d->ptr = *(const uint64_t *) (s + 0x30);
        d->delta = *(const uint64_t *) (s + 0x38);
        d->actual = *(const uint64_t *) (s + 0x40);
        return 1;
    }
    return 0;
}

static const char *P1sStatusName(uint32_t st)
{
    return st == 1 ? "mapped" : st == 2 ? "absent" : st ==
        3 ? "failed" : "not run";
}

static void P1sScan(void)
{
#if T2R_P1_SIDE_BUILT == 2
    P1sDesc d;
    if (!P1sReadDesc
        ((const uint8_t *) GetModuleHandleA("winmm.dll"), &d)) {
        g_p1sOk = 0;
        P1S_LOG
            ("p1 side: winmm has no side-file cave (.t2rps) - the Part I maps are off for this client");
        return;
    }
    g_p1sOk = d.status == 1 && d.layout == 3;
    P1S_LOG
        ("p1 side: %s (status %u layout %u step %u err %08x): blob %llu + side %llu bytes, delta %llx, view %llx%s",
         P1sStatusName(d.status), d.status, d.layout, d.step, d.err,
         (unsigned long long) d.size1, (unsigned long long) d.size2,
         (unsigned long long) d.delta, (unsigned long long) d.base,
         g_p1sOk ? "" :
         " - part1.dat missing / not this build's: the Part I maps are off for this client");
#elif T2R_P1_SIDE_BUILT
    char dir[MAX_PATH], why[160] = "";
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char *sl = strrchr(dir, 92);
    if (!sl) {
        g_p1sOk = 0;
        return;
    }
    strcpy(sl + 1, "t2r-maps\\part1");
    g_p1sOk = P1sCheck(dir, T2R_P1_SIDE_CRC, 0, why, sizeof why);
    if (g_p1sOk)
        P1S_LOG("p1 side: %d files verified in %s (manifest crc %08x)",
                g_p1sN, dir, (unsigned) T2R_P1_SIDE_CRC);
    else
        P1S_LOG
            ("p1 side: Part I side files NOT usable: %s - the Part I maps are off for this client",
             why);
#else
    g_p1sOk = 1;
#endif
}

static int P1sOk(void)
{
    if (g_p1sOk < 0)
        P1sScan();
    return g_p1sOk;
}

#if T2R_P1_SIDE_BUILT == 2
// Live 170 (reports of massive stutters on Bookstore): the engine reads part1.dat through the memory-mapped view
// (winmm's chunk hook memcpy), so every first touch is a 4 KB demand page fault = a small synchronous disk read (Part I itself streams the
// same data with big async file reads). Measured page faults per 10 s: Supermarket 2-50k, Bookstore 200k-2M (16 GB RAM: the whole match;
// 32 GB: the first ~4 min, then 2-5k once the file is cached). Fix: once a Part I map is picked, one background thread asks Windows to read
// the whole side view in big sequential IOs (PrefetchVirtualMemory, Win8+, a hint: never faults, never blocks the game).
static volatile LONG g_p1sPf;
static DWORD WINAPI P1sPrefetchThread(LPVOID unused)
{
    (void) unused;
    P1sDesc d;
    if (!P1sReadDesc((const uint8_t *) GetModuleHandleA("winmm.dll"), &d)
        || d.status != 1 || d.layout != 3 || !d.ptr || !d.size2) {
        P1S_LOG("p1 prefetch: no mapped side view - skipped");
        return 0;
    }
    typedef struct {
        PVOID va;
        SIZE_T n;
    } PfRange;
    typedef BOOL(WINAPI * pf_fn) (HANDLE, ULONG_PTR, PfRange *, ULONG);
    pf_fn pf =
        (pf_fn) (void *) GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                        "PrefetchVirtualMemory");
    if (!pf) {
        P1S_LOG
            ("p1 prefetch: PrefetchVirtualMemory not available - skipped");
        return 0;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    uint64_t t0 = GetTickCount64();
    PfRange r =
        { (PVOID) (uintptr_t) (d.ptr + d.size1), (SIZE_T) d.size2 };
    BOOL ok = pf(GetCurrentProcess(), 1, &r, 0);
    P1S_LOG
        ("p1 prefetch: side view %llx + %llu bytes -> %s (err %lu) in %llu ms",
         (unsigned long long) (uintptr_t) r.va, (unsigned long long) r.n,
         ok ? "requested" : "FAILED",
         ok ? 0ul : (unsigned long) GetLastError(),
         (unsigned long long) (GetTickCount64() - t0));
    return 0;
}

static void P1sPrefetchOnce(void)
{
    if (InterlockedCompareExchange(&g_p1sPf, 1, 0) != 0)
        return;
    HANDLE h = CreateThread(NULL, 0, P1sPrefetchThread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
    else {
        g_p1sPf = 0;
        P1S_LOG("p1 prefetch: CreateThread failed (%lu)",
                (unsigned long) GetLastError());
    }
}

// ---- Part I reads as FILE reads, like Part I itself. winmm's chunk reader 0x180002e10 does
// buf = HeapAlloc(n); memcpy(buf, [0x1801398d0] + rel, n) (call 0x18009dbf0 at 0x180002f70); decrypt(buf). Every chunk of part1.dat thus came from
// the mapped view = 4 KB demand faults, synchronous, re-faulted whenever Windows dropped the pages (16 GB PCs: 150k-1.2M faults / 10 s on
// Bookstore even with the prefetch). That one call now goes to p1s_copy: a source inside the side view is read with ReadFile at the file offset
// (positional OVERLAPPED read on a synchronous handle = thread safe, kernel read-ahead, big IOs); everything else, and any failed read, is the
// plain memcpy. The bytes are identical (the view IS the file), so decryption after it is unchanged. t2r-p1-mmap.txt next to the exe = the old path.
static HANDLE g_p1rFile = INVALID_HANDLE_VALUE;
static volatile LONG g_p1rOpen;        // 0 not tried, 1 open, 2 failed
static uint64_t g_p1r_lo, g_p1r_hi;     // side view [lo, hi)
static volatile LONG64 g_p1r_n, g_p1r_bytes, g_p1r_fail, g_p1r_us,
    g_p1r_max_us;
static void *(*g_p1r_memcpy)(void *, const void *, size_t);
static void *P1sCopy(void *dst, const void *src, size_t n)
{
    uint64_t s = (uint64_t) (uintptr_t) src;
    if (g_p1rOpen == 1 && n && s + n > g_p1r_lo && s < g_p1r_hi) {
        uint8_t *d = (uint8_t *) dst;
        if (s < g_p1r_lo) {
            size_t k = (size_t) (g_p1r_lo - s);
            g_p1r_memcpy(d, src, k);
            d += k;
            s += k;
            n -= k;
        }                       // the part before the side view
        size_t tail =
            s + n > g_p1r_hi ? (size_t) (s + n - g_p1r_hi) : 0, mid =
            n - tail;
        LARGE_INTEGER t0, t1, fq;
        QueryPerformanceCounter(&t0);
        uint64_t fo = s - g_p1r_lo;
        size_t done = 0;
        int ok = 1;
        while (done < mid) {
            DWORD want =
                (DWORD) (mid - done >
                         0x40000000u ? 0x40000000u : mid - done), got = 0;
            OVERLAPPED ov = { 0 };
            ov.Offset = (DWORD) (fo + done);
            ov.OffsetHigh = (DWORD) ((fo + done) >> 32);
            if (!ReadFile(g_p1rFile, d + done, want, &got, &ov) || !got) {
                ok = 0;
                break;
            }
            done += got;
        }
        if (!ok) {
            InterlockedIncrement64(&g_p1r_fail);
            g_p1r_memcpy(d + done, (const void *) (uintptr_t) (s + done),
                         mid - done);
        }                       // fallback: the mapped view
        if (tail)
            g_p1r_memcpy(d + mid, (const void *) (uintptr_t) g_p1r_hi,
                         tail);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&fq);
        LONG64 us =
            (LONG64) ((t1.QuadPart -
                       t0.QuadPart) * 1000000 /
                      (fq.QuadPart ? fq.QuadPart : 1));
        InterlockedIncrement64(&g_p1r_n);
        InterlockedAdd64(&g_p1r_bytes, (LONG64) mid);
        InterlockedAdd64(&g_p1r_us, us);
        LONG64 mx = g_p1r_max_us;
        while (us > mx
               && InterlockedCompareExchange64(&g_p1r_max_us, us,
                                               mx) != mx)
            mx = g_p1r_max_us;
        return dst;
    }
    return g_p1r_memcpy(dst, src, n);
}

static void P1sFilereadInstall(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    {
        FILE *f = ExeSideOpen("t2r-p1-mmap.txt", "r");
        if (f) {
            fclose(f);
            P1S_LOG("p1 read: t2r-p1-mmap.txt - mapped view reads kept");
            return;
        }
    }
    P1sDesc d;
    if (!P1sReadDesc((const uint8_t *) GetModuleHandleA("winmm.dll"), &d)
        || d.status != 1 || d.layout != 3 || !d.ptr || !d.size2) {
        P1S_LOG("p1 read: no mapped side view - not installed");
        return;
    }
    uint8_t *call = (uint8_t *) T2R_CODE(kCodeP1SideCall);
    uint8_t *mc = (uint8_t *) T2R_CODE(kCodeP1SideMc);
    if (call[0] != 0xE8 || call + 5 + *(int32_t *) (call + 1) != mc
        || memcmp(call - 0x10, "\x48\x8b\x15", 3)) {
        HOOK_FAIL("p1 read: chunk reader call differs - not installed");
        return;
    }
    char path[MAX_PATH];
    HMODULE wm = GetModuleHandleA("winmm.dll");
    DWORD pl = wm ? GetModuleFileNameA(wm, path, MAX_PATH) : 0;
    char *sl = pl ? strrchr(path, 92) : NULL;
    if (!sl || (size_t) (sl - path) + 32 >= sizeof path) {
        P1S_LOG("p1 read: winmm path unknown - not installed");
        return;
    }
    strcpy(sl + 1, "t2r-maps\\part1\\part1.dat");
    HANDLE h =
        CreateFileA(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
    LARGE_INTEGER sz = { 0 };
    if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &sz)
        || (uint64_t) sz.QuadPart < d.size2) {
        P1S_LOG
            ("p1 read: cannot open %s (%lu) or too small - not installed",
             path, (unsigned long) GetLastError());
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        return;
    }
    g_p1rFile = h;
    g_p1r_lo = d.ptr + d.size1;
    g_p1r_hi = g_p1r_lo + d.size2;
    g_p1r_memcpy = (void *(*)(void *, const void *, size_t))(void *) mc;
    uint8_t *stub = AllocNear((uint8_t *) g_winmm, 0x1000);
    if (!stub) {
        CloseHandle(h);
        g_p1rFile = INVALID_HANDLE_VALUE;
        return;
    }
    stub[0] = 0x48;
    stub[1] = 0xB8;
    *(uint64_t *) (stub + 2) = (uint64_t) (uintptr_t) P1sCopy; // mov rax, imm64
    stub[10] = 0xFF;
    stub[11] = 0xE0;            // jmp rax
    DWORD old;
    VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old);
    InterlockedExchange(&g_p1rOpen, 1);
    VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old);
    InterlockedExchange((volatile LONG *) (call + 1), (LONG) (stub - (call + 5)));      // (unaligned on x64 is still one locked write)
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    P1S_LOG
        ("p1 read: chunk reads of part1.dat [%llx, %llx) are file reads now (ReadFile, %s)",
         (unsigned long long) g_p1r_lo, (unsigned long long) g_p1r_hi,
         path);
}

static void P1sFilereadStats(void)
{                               // once per 30 s while reads happen
    static uint64_t next;
    static LONG64 last_n;
    uint64_t now = GetTickCount64();
    if (now < next)
        return;
    next = now + 30000;
    LONG64 n = g_p1r_n;
    if (n == last_n)
        return;
    LONG64 dn = n - last_n;
    last_n = n;
    LONG64 us = InterlockedExchange64(&g_p1r_us, 0), mx =
        InterlockedExchange64(&g_p1r_max_us, 0), by =
        InterlockedExchange64(&g_p1r_bytes, 0);
    P1S_LOG
        ("p1 read: last 30 s %lld chunk reads, %.1f MB, avg %.2f ms, max %.1f ms, failed (mapped fallback) %lld total",
         (long long) dn, (double) by / 1048576.0,
         dn ? (double) us / 1000.0 / (double) dn : 0.0,
         (double) mx / 1000.0, (long long) g_p1r_fail);
}
#else
static void P1sPrefetchOnce(void)
{
}

static void P1sFilereadInstall(void)
{
}

static void P1sFilereadStats(void)
{
}
#endif

static const P1sEntry *P1sFind(const char *rel)
{
    if (!P1sOk() || !g_p1sN)
        return NULL;
    int lo = 0, hi = g_p1sN - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(rel, g_p1sE[mid].rel);
        if (!c)
            return &g_p1sE[mid];
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return NULL;
}

// read n bytes at off (clamped to the entry's size); own handle per call (the engine may read from several threads). -> bytes read, -1 error
static int64_t P1sRead(const P1sEntry *e, uint64_t off, void *buf,
                        uint32_t n)
{
    if (!e || off >= e->size)
        return e ? 0 : -1;
    if (n > e->size - off)
        n = (uint32_t) (e->size - off);
    HANDLE h =
        CreateFileA(e->full, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG) off;
    DWORD got = 0;
    int ok = SetFilePointerEx(h, at, NULL, FILE_BEGIN)
        && ReadFile(h, buf, n, &got, NULL);
    CloseHandle(h);
    return ok ? (int64_t) got : -1;
}

static uint32_t P1sMaskFilter(uint32_t m)
{
    if (!T2R_P1_SIDE_MASK || P1sOk())
        return m;
    return m & ~(uint32_t) T2R_P1_SIDE_MASK;
}
#endif
