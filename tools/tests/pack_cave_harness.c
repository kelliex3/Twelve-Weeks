// Offline harness: RUNS the winmm data-file cave (tools/features/pack_file.py) in a fake image at 0x180000000.
//   gcc -std=gnu2x -O1 -o pack_cave_harness.exe pack_cave_harness.c      (MSYS2 UCRT64)
//   pack_cave_harness.exe <section.bin> <section VA hex> <entry VA hex> <dir> <size1> <size2>
// <dir>\fake.dll stands for winmm.dll (only its directory matters), <dir>\t2r-maps\main\t2r-data.bin = the data file,
// <dir>\t2r-maps\part1\part1.dat = the Part I side file (either may be absent / wrong). IAT slots point at kernel32 except
// GetModuleFileNameW (-> <dir>\fake.dll), MessageBoxA (prints FATAL <title> | <first line>) and TerminateProcess (prints the RESULT, exits 0).
// Prints  RESULT status=<n> layout=<n> step=<n> err=<hex> mstatus=<n> mstep=<n> cache=<0 | 1 hit | 2 written | 3 not written> ret=<1 resumed | 0 fail branch> d8=<size>
//   then  TIME cave <ms>
//   then  MEM full-ok | MEM view1-ok-hole-reserved | MEM view1-ok | MEM BAD <what>
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static char g_dir[MAX_PATH];
static uint64_t g_secva;

static DWORD WINAPI fake_gmfn(HMODULE m, LPWSTR buf, DWORD n) {
    (void)m;
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\fake.dll", g_dir);
    return (DWORD)MultiByteToWideChar(CP_ACP, 0, p, -1, buf, (int)n) - 1;
}
static void put64(uint64_t va, uint64_t v) { *(uint64_t *)(uintptr_t)va = v; }
static uint64_t get64(uint64_t va) { return *(uint64_t *)(uintptr_t)va; }
static void result(int ret) {
    uint32_t *d = (uint32_t *)(uintptr_t)g_secva;
    printf("RESULT status=%u layout=%u step=%u err=%x mstatus=%u mstep=%u cache=%u ret=%d d8=%llu\n", d[2], d[3], d[4], d[5], d[0x48 / 4], d[0x4C / 4], d[0x50 / 4], ret,
           (unsigned long long)get64(0x1801398D8));
}
static int WINAPI fake_mba(HWND h, LPCSTR text, LPCSTR title, UINT type) {
    (void)h;
    const char *nl = strchr(text, '\r');
    printf("FATAL %s | %.*s | type=%x\n", title, nl ? (int)(nl - text) : (int)strlen(text), text, type);
    return IDOK;
}
static BOOL WINAPI fake_tp(HANDLE p, UINT code) {
    printf("TERMINATE handle=%lld code=%x\n", (long long)(intptr_t)p, code);
    result(-1);
    fflush(stdout);
    ExitProcess(0);
}
static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    _fseeki64(f, 0, SEEK_END); *n = (size_t)_ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc(*n ? *n : 1);
    if (fread(b, 1, *n, f) != *n) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    if (argc < 7) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    uint64_t secva = strtoull(argv[2], NULL, 16), entry = strtoull(argv[3], NULL, 16);
    g_secva = secva;
    snprintf(g_dir, sizeof g_dir, "%s", argv[4]);
    uint64_t size1 = strtoull(argv[5], NULL, 10), size2 = strtoull(argv[6], NULL, 10);
    uint8_t *img = (uint8_t *)VirtualAlloc((void *)0x180000000ULL, 0x200000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (img != (uint8_t *)0x180000000ULL) { printf("NOIMAGE %p\n", img); return 3; }
    size_t sn = 0;
    uint8_t *sec = slurp(argv[1], &sn);
    if (!sec) return 4;
    if (VirtualAlloc((void *)(uintptr_t)(secva & ~0xFFFFULL), ((secva & 0xFFFF) + sn + 0xFFFF) & ~(size_t)0xFFFF, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE) != (void *)(uintptr_t)(secva & ~0xFFFFULL)) { printf("NOSECTION\n"); return 5; }
    memcpy((void *)(uintptr_t)secva, sec, sn);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    put64(0x180129520, (uint64_t)(uintptr_t)GetProcAddress(k, "CreateFileW"));
    put64(0x180129508, (uint64_t)(uintptr_t)GetProcAddress(k, "CloseHandle"));
    put64(0x180129648, (uint64_t)(uintptr_t)GetProcAddress(k, "GetFileSizeEx"));
    put64(0x180129658, (uint64_t)(uintptr_t)GetProcAddress(k, "GetLastError"));
    put64(0x180129668, (uint64_t)(uintptr_t)fake_gmfn);
    put64(0x180129680, (uint64_t)(uintptr_t)GetProcAddress(k, "GetModuleHandleW"));
    put64(0x180129690, (uint64_t)(uintptr_t)GetProcAddress(k, "GetProcAddress"));
    put64(0x1801298B8, (uint64_t)(uintptr_t)GetProcAddress(k, "VirtualAlloc"));
    put64(0x1801298C0, (uint64_t)(uintptr_t)GetProcAddress(k, "VirtualFree"));
    put64(0x1801297D8, (uint64_t)(uintptr_t)GetProcAddress(k, "ReadFile"));
    put64(0x1801298F8, (uint64_t)(uintptr_t)GetProcAddress(k, "WriteFile"));
    put64(0x180129908, (uint64_t)(uintptr_t)fake_mba);
    put64(0x180129880, (uint64_t)(uintptr_t)fake_tp);
    uint8_t *stub = (uint8_t *)malloc(0x10000);                   // stands for the T2ME stub in the image
    memset(stub, 0x5a, 0x10000);
    put64(0x1801398D0, (uint64_t)(uintptr_t)stub);
    put64(0x1801398D8, 0x10000);
    put64(0x1801398E8, 0x1234);
    // thunk @0x180001000: sub rsp,8 ; xor eax,eax (ZF=1: the loader's jne is not taken) ; jmp entry
    uint8_t *t = (uint8_t *)0x180001000ULL;
    uint8_t th[] = {0x48, 0x83, 0xEC, 0x08, 0x31, 0xC0, 0xE9, 0, 0, 0, 0};
    int32_t rel = (int32_t)(entry - (0x180001000ULL + 11));
    memcpy(th + 7, &rel, 4);
    memcpy(t, th, sizeof th);
    uint8_t res[] = {0x48, 0x83, 0xC4, 0x08, 0xB8, 1, 0, 0, 0, 0xC3};      // 0x1800019a6: resumed -> return 1
    memcpy((void *)0x1800019A6ULL, res, sizeof res);
    uint8_t fail[] = {0x48, 0x83, 0xC4, 0x08, 0x31, 0xC0, 0xC3};            // 0x180001adb: the loader's failure exit -> return 0
    memcpy((void *)0x180001ADBULL, fail, sizeof fail);
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    int ret = ((int (*)(void))t)();
    QueryPerformanceCounter(&q1);
    result(ret);
    PROCESS_MEMORY_COUNTERS pmc = {.cb = sizeof pmc};
    K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    printf("TIME cave %.0f ms, working set %.0f MB (peak %.0f MB)\n", (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart,
           pmc.WorkingSetSize / 1048576.0, pmc.PeakWorkingSetSize / 1048576.0);
    if (argc > 7 && !strcmp(argv[7], "nocmp")) return 0;      // real-size files: no second copy in memory
    uint32_t *d = (uint32_t *)(uintptr_t)secva;
    uint64_t d0 = get64(0x1801398D0);
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\t2r-maps\\main\\t2r-data.bin", g_dir);
    size_t dn = 0;
    uint8_t *data = slurp(p, &dn);
    snprintf(p, sizeof p, "%s\\t2r-maps\\part1\\part1.dat", g_dir);
    size_t pn = 0;
    uint8_t *side = slurp(p, &pn);
    uint64_t dl = data && dn >= 0x38 ? *(uint64_t *)(data + 0x30) : 0;          // header +0x30: delta (payload at 0x10000 + delta)
    if (!data || dn < 0x10000 + dl + size1) { printf("MEM BAD no-data\n"); return 0; }
    if ((d0 & 0xFFFF) != dl) { printf("MEM BAD d0-delta\n"); return 0; }
    if (d0 == (uint64_t)(uintptr_t)stub) { printf("MEM BAD d0-still-stub\n"); return 0; }
    if (memcmp((void *)(uintptr_t)d0, data + 0x10000 + dl, (size_t)size1)) { printf("MEM BAD view1-content\n"); return 0; }
    if (!size2) { printf("MEM view1-ok\n"); return 0; }
    if (d[3] == 3) {
        if (!side || pn != size2 + 16 || memcmp((void *)(uintptr_t)(d0 + size1), side, (size_t)size2)) printf("MEM BAD view2-content\n");
        else printf("MEM full-ok\n");
    } else {
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery((void *)(uintptr_t)(d0 + size1), &mbi, sizeof mbi);
        printf(mbi.State == MEM_RESERVE ? "MEM view1-ok-hole-reserved\n" : "MEM view1-ok-hole-other state=%lx\n", (unsigned long)mbi.State);
    }
    return 0;
}
