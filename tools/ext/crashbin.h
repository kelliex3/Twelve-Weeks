// crashbin.h - the crash handler's own evidence file: t2r-crash-<pid>-1.bin next to t2r-ext.log.
// Players have only the game's MiniDumpNormal .mdmp (no ext data); WER LocalDumps needs HKLM / admin. So crash_veh writes the raw g_cdiag block,
// the native ring and the last log lines itself, behind a small header. Handler-side rules: no CRT stdio, no heap, no locks, no dbghelp /
// MiniDumpWriteDump (can deadlock on a heap lock held by a suspended thread): CreateFileW + WriteFile + CloseHandle on a path built at startup,
// a static header, one shot per process. Startup (never the handler) keeps the newest CB_KEEP files.
// Layout (little endian), read by tools/reports/cdiag_rings.py (parse_bin) and release/launcher/lib/report.js (binInfo):
//   0 "T2RCBIN1" | 8 u32 ver=1 | 12 u32 hdr_size=128 | 16 u32 pid | 20 u32 tid | 24 u64 process create time (FILETIME) | 32 u32 exception code |
//   36 u32 0 | 40 u64 exception address | 48 u64 frame | 56 u64 info0 | 64 u64 info1 | 72 u32 block_size | 76 u32 dcr_size | 80 i32 dcr_i |
//   84 i32 xr_i | 88 u32 xr_lines | 92 u32 xr_w | 96 char[32] ext build ("Oct 19 2026 12:00:00 (release)") | 128 block | dcr ring | xr_lines x xr_w
//   (log lines oldest first: line k is ring index xr_i - xr_lines + 1 + k)
#ifndef T2R_CRASHBIN_H
#define T2R_CRASHBIN_H
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#define CB_HDR 128
#define CB_KEEP 3               // files in the folder at most, this process's included
#define CB_XR_LINES 64          // log lines kept (of the 512 in the ring)

static wchar_t g_cbPath[MAX_PATH + 40];
static uint8_t g_cbHdr[CB_HDR];
static volatile LONG g_cbDone;
static int g_cbOk;

static void CbPut32(unsigned o, uint32_t v)
{
    memcpy(g_cbHdr + o, &v, 4);
}

static void CbPut64(unsigned o, uint64_t v)
{
    memcpy(g_cbHdr + o, &v, 8);
}

// startup: `dir` = the game folder with a trailing backslash, `build` = the ext build stamp. Deletes the oldest t2r-crash-*.bin so that, with this
// process's file, at most CB_KEEP exist; precomputes the path and the constant header fields. Every error just leaves g_cb_ok = 0.
static void CbInit(const wchar_t *dir, const char *build)
{
    size_t dl = wcslen(dir);
    if (dl + 48 >= sizeof g_cbPath / sizeof *g_cbPath)
        return;
    wchar_t pat[MAX_PATH + 40];
    wcscpy(pat, dir);
    wcscat(pat, L"t2r-crash-*.bin");
    struct {
        wchar_t n[64];
        uint64_t t;
    } f[64];
    int nf = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                || wcslen(fd.cFileName) >= 64 || nf >= 64)
                continue;
            wcscpy(f[nf].n, fd.cFileName);
            f[nf].t =
                ((uint64_t) fd.ftLastWriteTime.dwHighDateTime << 32) | fd.
                ftLastWriteTime.dwLowDateTime;
            nf++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (int i = 1; i < nf; i++) {      // newest first (insertion sort, <= 64)
        int j = i;
        while (j > 0 && f[j].t > f[j - 1].t) {
            __typeof__(f[0]) t = f[j];
            f[j] = f[j - 1];
            f[j - 1] = t;
            j--;
        }
    }
    for (int i = CB_KEEP - 1; i < nf; i++) {
        wchar_t p[MAX_PATH + 40];
        wcscpy(p, dir);
        wcscat(p, f[i].n);
        DeleteFileW(p);
    }
    swprintf(g_cbPath, sizeof g_cbPath / sizeof *g_cbPath,
             L"%ls" L"t2r-crash-%lu-1.bin", dir,
             (unsigned long) GetCurrentProcessId());
    FILETIME c, e, k, u;
    uint64_t ct = 0;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        ct = ((uint64_t) c.dwHighDateTime << 32) | c.dwLowDateTime;
    memset(g_cbHdr, 0, sizeof g_cbHdr);
    memcpy(g_cbHdr, "T2RCBIN1", 8);
    CbPut32(8, 1);
    CbPut32(12, CB_HDR);
    CbPut32(16, GetCurrentProcessId());
    CbPut64(24, ct);
    strncpy((char *) g_cbHdr + 96, build, 31);
    g_cbOk = 1;
}

// crash handler (first report per process only). No allocation, no CRT stdio, no lock. `xr` is the log-line ring (xr_n lines of xr_w bytes, xr_i = last index).
static void CbWrite(uint32_t code, uint64_t addr, uint64_t info0,
                     uint64_t info1, uint32_t tid, uint64_t frame,
                     const void *block, uint32_t bsz, const void *dcr,
                     uint32_t dsz, int32_t dcr_i, const char *xr,
                     uint32_t xr_n, uint32_t xr_w, int32_t xr_i)
{
    if (!g_cbOk || InterlockedExchange(&g_cbDone, 1))
        return;
    int64_t lo = (int64_t) xr_i - (CB_XR_LINES - 1);
    if (lo < 1)
        lo = 1;
    uint32_t nl = xr_i >= 1 ? (uint32_t) ((int64_t) xr_i - lo + 1) : 0;
    CbPut32(20, tid);
    CbPut32(32, code);
    CbPut64(40, addr);
    CbPut64(48, frame);
    CbPut64(56, info0);
    CbPut64(64, info1);
    CbPut32(72, bsz);
    CbPut32(76, dsz);
    CbPut32(80, (uint32_t) dcr_i);
    CbPut32(84, (uint32_t) xr_i);
    CbPut32(88, nl);
    CbPut32(92, xr_w);
    HANDLE h =
        CreateFileW(g_cbPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD w;
    WriteFile(h, g_cbHdr, CB_HDR, &w, NULL);
    WriteFile(h, block, bsz, &w, NULL);
    WriteFile(h, dcr, dsz, &w, NULL);
    for (uint32_t k = 0; k < nl; k++)
        WriteFile(h,
                  xr + (size_t) (((uint32_t) lo + k) & (xr_n - 1)) * xr_w,
                  xr_w, &w, NULL);
    CloseHandle(h);
}
#endif
