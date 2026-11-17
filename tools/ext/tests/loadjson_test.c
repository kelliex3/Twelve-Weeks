// Offline harness for tools/ext/loadjson.c: the REAL loadjson.c is #included, the engine / loadout reads are stubbed.
// Driven by tools/tests/test_loadjson_c.py.   Build: gcc -std=gnu2x -O1 -g -o loadjson_test loadjson_test.c   (windows.h: mingw)
//   loadjson_test export <dir> <active_kind> <active_i> <n_populated> <id>...   writes <dir>\t2r-loadout.json (lj_export(1)); every table lists <id>...
//   loadjson_test import <dir> <file>                                              runs lj_import on <file> copied to <dir>\t2r-loadout-launcher.json, prints the resulting loadouts
//   loadjson_test fuzz <dir> <seed-file> <iterations> <rng-seed>                  hostile / garbled / oversized launcher files through lj_parse + lj_import
//   loadjson_test state <dir>                                                      prints g_lj_applied as lj_state_load reads it from <dir>\t2r-loadout.json
// Exit 0 = ran; the fuzz run exits 3 if something the game would not accept was applied.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include "../skills.h"

#define VMA(x) ((uintptr_t)g_winmm + ((x) - 0x180000000ULL))
#define N_CUSTOM 6
#define N_CLASSES 8
static uint8_t *g_winmm;
static const uint8_t *g_largeTab;      // NULL: the large table is the engine's at VMA(0x1800A0DB0)
static char g_loadoutPath[MAX_PATH];
typedef struct {
    uint32_t w[3];
    uint64_t k[3];
} LoadoutT;
static LoadoutT g_lo[N_CUSTOM];
static uint64_t g_sk4[N_CUSTOM];
enum { LO_NONE };
static int g_loActive, g_loState, g_loPending, g_in_match;
static struct {
    int ok;
} g_loSnap;
static struct {
    int nlv;
} SK[SK_COUNT];
static uint8_t g_small[24][0x38], g_purch[24][0x40];

static void Xlog(const char *fmt, ...)
{
    (void) fmt;
}

static void MsgHook(const char *t, float s, int f, uint64_t id)
{
    (void) t;
    (void) s;
    (void) f;
    (void) id;
}

static DWORD FaPoll(const char *p)
{
    return GetFileAttributesA(p);
}

static uint8_t MatchSubstate(void)
{
    return g_in_match ? 6 : 0;
}

static int GlPickRefused(int row)
{
    return row == 5;
}

static uint8_t *WeaponRow(int slot, uint32_t i)
{
    (void) slot;
    return g_small[i];
}

static uint8_t *LoPurchTab(void)
{
    return &g_purch[0][0];
}

static int SkId(uint64_t v)
{
    int id = (int) ((v >> 40) & 0xff);
    return (v & SK_TAG) && id > 0 && id < SK_COUNT ? id : 0;
}

static int SkLv(uint64_t v)
{
    int id = SkId(v), lv = (int) ((v >> 36) & 0xf);
    return id && lv >= 1 && lv <= SK[id].nlv ? lv : 0;
}

static uint64_t SkNorm(uint64_t v)
{
    return SkLv(v) ? v : 0;
}

static uint64_t *Sk4Slot(const LoadoutT *l)
{
    return &g_sk4[l - g_lo];
}

static int LoCost(const LoadoutT *l)
{                               // stand-in rule (the real one reads the engine tables): 2 LP per gun, 1 per skill level
    int c = 0;
    for (int i = 0; i < 3; i++)
        if (l->w[i] != 0xffffffffu)
            c += 2;
    for (int i = 0; i < 3; i++)
        c += SkLv(SkNorm(l->k[i]));
    return c + SkLv(SkNorm(*Sk4Slot(l)));
}

static int LoValid(const LoadoutT *l)
{
    return LoCost(l) <= 13;
}

static uint32_t LoClassRow(int slot, const char *spec)
{
    const uint32_t *cnt = (const uint32_t *) VMA(0x1800A0DA4);
    for (uint32_t i = 0; i < cnt[slot]; i++) {
        const uint8_t *row =
            slot == 2 ? LoPurchTab() + i * 0x40 : slot == 0
            && !g_largeTab ? (const uint8_t *) VMA(0x1800A0DB0) +
            i * 0x38 : WeaponRow(slot, i);
        const char *id = *(const char **) (row + 0x10);
        if (id && !strcmp(id, spec))
            return i;
    }
    return 0xffffffffu;
}

static const LoadoutT *LoOf(int idx)
{
    return &g_lo[idx % N_CUSTOM];
}

static void LoWriteObj(const LoadoutT *l)
{
    (void) l;
}

static void LoSave(void)
{
}

static void Sk4Save(void)
{
}

#include "../loadjson.c"

static char *g_tab[3][24];
static int g_ntab[3];           // engine slot -> ids (0 large, 1 small, 2 purchasable)
static char *g_stdTab[3][6] = {
    { "rifle-mpx5", "shotgun-shorty", "bow-ellie",
     "shotgun-baserri-over-under", "rifle-mpx5", "rifle-mpx5" },
    { "pistol-beretta", "pistol-beretta-supp", "pistol-beretta",
     "pistol-beretta", "pistol-beretta", "pistol-beretta" },
    { "shotgun-shorty", "bow-ellie", "shotgun-shorty", "shotgun-shorty",
     "shotgun-shorty", "shotgun-shorty" }
};

static void SetTables(void)
{
    uint32_t *cnt = (uint32_t *) VMA(0x1800A0DA4);
    for (int s = 0; s < 3; s++) {
        cnt[s] = (uint32_t) g_ntab[s];
        for (int i = 0; i < g_ntab[s]; i++) {
            uint8_t *row =
                s == 0 ? (uint8_t *) VMA(0x1800A0DB0) + i * 0x38 : s ==
                1 ? g_small[i] : g_purch[i];
            *(const char **) (row + 0x10) = g_tab[s][i];
        }
    }
}

static void StdTables(void)
{
    for (int s = 0; s < 3; s++) {
        g_ntab[s] = 6;
        memcpy(g_tab[s], g_stdTab[s], sizeof g_stdTab[s]);
    }
}

static void LoadTables(const char *file)
{                               // lines "<slot> <id>", ids kept byte for byte (no escapes); at most 24 per slot
    static char data[65536];
    FILE *f = fopen(file, "rb");
    size_t n = f ? fread(data, 1, sizeof data - 1, f) : 0;
    if (f)
        fclose(f);
    data[n] = 0;
    for (char *l = strtok(data, "\n"); l; l = strtok(NULL, "\n")) {
        int s = l[0] - '0';
        if (s >= 0 && s < 3 && l[1] == ' ' && g_ntab[s] < 24)
            g_tab[s][g_ntab[s]++] = l + 2;
    }
}

static void InitCommon(const char *dir)
{
    g_winmm = calloc(1, 0x200000);
    snprintf(g_loadoutPath, sizeof g_loadoutPath, "%s\\x", dir);
    for (int i = 1; i < SK_COUNT; i++)
        SK[i].nlv = i == SK_LETHAL ? 2 : 3;
}

static void PrintState(void)
{
    printf("active %d\n", g_loActive);
    for (int i = 0; i < N_CUSTOM; i++) {
        const LoadoutT *l = &g_lo[i];
        printf("c%d %d %d %d %llx %llx %llx %llx\n", i, (int) l->w[1],
               (int) l->w[0], (int) l->w[2], (unsigned long long) l->k[0],
               (unsigned long long) l->k[1], (unsigned long long) l->k[2],
               (unsigned long long) g_sk4[i]);
    }
}

static unsigned long long rs;
static unsigned Rnd(void)
{
    rs = rs * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned) (rs >> 33);
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    InitCommon(argv[2]);
    if (!strcmp(argv[1], "export") && argc >= 7) {
        g_loActive =
            !strcmp(argv[3],
                    "custom") ? atoi(argv[4]) : N_CUSTOM + atoi(argv[4]);
        int np = atoi(argv[5]);
        LoadTables(argv[6]);
        SetTables();
        for (int i = 0; i < N_CUSTOM; i++) {
            LoadoutT *l = &g_lo[i];
            l->w[0] = l->w[1] = l->w[2] = 0xffffffffu;
            if (i >= np)
                continue;       // an empty slot: no guns, no skills
            if (g_ntab[1])
                l->w[1] = (uint32_t) (i % g_ntab[1]);
            if (g_ntab[0])
                l->w[0] = (uint32_t) ((i * 5 + 1) % g_ntab[0]);
            if (g_ntab[2] && i % 3 != 2)
                l->w[2] = (uint32_t) ((i * 7 + 2) % g_ntab[2]);
            for (int j = 0; j < 3; j++)
                l->k[j] =
                    SK_V(1 + (i * 4 + j) % (SK_COUNT - 1),
                         1 + (i + j) % 2);
            g_sk4[i] = SK_V(1 + (i * 4 + 3) % (SK_COUNT - 1), 1 + i % 2);
        }
        LjStateLoad();
        g_ljApplied = 1234567890123LL;
        LjExport(1);
        return 0;
    }
    if (!strcmp(argv[1], "state")) {
        LjStateLoad();
        printf("%lld\n", g_ljApplied);
        return 0;
    }
    if (!strcmp(argv[1], "import") && argc >= 4) {
        StdTables();
        SetTables();
        for (int i = 0; i < N_CUSTOM; i++) {
            g_lo[i].w[0] = g_lo[i].w[2] = 0xffffffffu;
            g_lo[i].w[1] = 0;
        }
        char p[MAX_PATH];
        LjPath(p, sizeof p, "t2r-loadout-launcher.json");
        CopyFileA(argv[3], p, FALSE);
        g_ljApplied = 0;
        int r = LjImport();
        printf("applied %d\n", r);
        PrintState();
        return 0;
    }
    if (!strcmp(argv[1], "fuzz") && argc >= 6) {
        StdTables();
        SetTables();
        static char seed[16384];
        FILE *f = fopen(argv[3], "rb");
        size_t sn = f ? fread(seed, 1, sizeof seed - 1, f) : 0;
        if (f)
            fclose(f);
        int iters = atoi(argv[4]);
        rs = strtoull(argv[5], 0, 10);
        static char big[200000];
        char p[MAX_PATH];
        LjPath(p, sizeof p, "t2r-loadout-launcher.json");
        static const char *junk[] =
            { "{", "}", "[", "]", "\"", "\\", "\\u", "\\u12", ",", ":",
"null", "true", "1e999", "-0", "nan", "inf", "0x7fffffff", "\"id\"", "\"custom\"",
"\"rev\":9", "\"skills\"", "{\"a\":", "[[[[[[[[[[", "\xff\xfe", "\\\"" };
        unsigned long long t = 133000000000000000ULL;
        int applied = 0, parsed = 0;
        for (int it = 0; it < iters; it++) {
            size_t n = 0;
            int mode = (int) (Rnd() % 9);
            if (mode == 8) {    // oversize / pool exhaustion / depth / giant strings
                int k = (int) (Rnd() % 4);
                if (k == 0) {
                    n = (size_t) snprintf(big, sizeof big,
                                          "{\"rev\":5,\"custom\":[");
                    for (int i = 0; i < 5000 && n < sizeof big - 40; i++)
                        n += (size_t) snprintf(big + n, sizeof big - n,
                                               "{\"small\":{\"id\":\"a\"}},");
                    n += (size_t) snprintf(big + n, sizeof big - n, "1]}");
                } else if (k == 1) {
                    n = (size_t) snprintf(big, sizeof big,
                                          "{\"rev\":5,\"custom\":[{\"small\":{\"id\":\"");
                    memset(big + n, 'A', 150000);
                    n += 150000;
                    n += (size_t) snprintf(big + n, sizeof big - n,
                                           "\"}}]}");
                } else if (k == 2) {
                    for (int i = 0; i < 20000; i++)
                        big[n++] = '[';
                    n += (size_t) snprintf(big + n, sizeof big - n,
                                           "{\"rev\":5}");
                } else {
                    n = (size_t) snprintf(big, sizeof big,
                                          "{\"rev\":5,\"custom\":[{\"small\":{\"id\":\"pistol-beretta");
                    for (int i = 0; i < 30000; i++)
                        n += (size_t) snprintf(big + n, sizeof big - n,
                                               "\\u00");
                }
            } else {
                memcpy(big, seed, sn);
                n = sn;
                int muts = 1 + (int) (Rnd() % 8);
                for (int m = 0; m < muts; m++) {
                    size_t at = n ? Rnd() % n : 0;
                    switch (mode) {
                    case 0:
                        if (n)
                            n = at;
                        break;  // truncate
                    case 1:
                        if (n)
                            big[at] = (char) Rnd();
                        break;  // byte flip
                    case 2:{
                            const char *j =
                                junk[Rnd() % (sizeof junk / sizeof *junk)];
                            size_t L = strlen(j);
                            if (n + L < sizeof big) {
                                memmove(big + at + L, big + at, n - at);
                                memcpy(big + at, j, L);
                                n += L;
                            }
                            break;
                        }
                    case 3:
                        if (n > 4) {
                            size_t L = Rnd() % 20;
                            if (at + L > n)
                                L = n - at;
                            memmove(big + at, big + at + L, n - at - L);
                            n -= L;
                        }
                        break;  // delete a span
                    case 4:
                        for (size_t i = 0; i < 40 && n; i++)
                            big[Rnd() % n] = "{}[],:\"\\ 0"[Rnd() % 11];
                        break;
                    case 5:
                        if (n < 100000) {
                            memcpy(big + n, seed, sn);
                            n += sn;
                        }
                        break;  // doubled
                    case 6:
                        for (size_t i = 0; i + 4 < n; i++)
                            if (!memcmp(big + i, "\"lv\"", 4) && Rnd() % 2) {
                                memcpy(big + i + 4, ":99", 3);
                                break;
                            }
                        break;
                    case 7:
                        if (n)
                            memset(big + at, 'x', Rnd() % (n - at + 1));
                        break;
                    }
                }
            }
            if (n >= sizeof big)
                n = sizeof big - 1;
            big[n] = 0;
            {                   // the reader alone, any size, on a buffer that ends exactly before a no-access page (a read or write past the NUL faults the run)
                size_t body = (n + 1 + 4095) & ~(size_t) 4095;
                char *reg =
                    VirtualAlloc(NULL, body + 4096,
                                 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE),
                    *copy = reg + body - (n + 1);
                DWORD old;
                VirtualProtect(reg + body, 4096, PAGE_NOACCESS, &old);
                memcpy(copy, big, n + 1);
                LjN *r = LjParse(copy);
                parsed += r != NULL;
                VirtualFree(reg, 0, MEM_RELEASE);
            }
            f = fopen(p, "wb");
            fwrite(big, 1, n, f);
            fclose(f);          // the real path: whole-file read (16 KB cap), gate, parse, apply
            HANDLE h =
                CreateFileA(p, FILE_WRITE_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                            OPEN_EXISTING, 0, 0);
            if (h != INVALID_HANDLE_VALUE) {
                t += 10000000ULL;
                FILETIME ft = { (DWORD) t, (DWORD) (t >> 32) };
                SetFileTime(h, 0, 0, &ft);
                CloseHandle(h);
            }
            g_ljApplied = 0;
            g_in_match = (int) (Rnd() % 2);
            for (int i = 0; i < N_CUSTOM; i++) {
                g_lo[i].w[0] = g_lo[i].w[2] = 0xffffffffu;
                g_lo[i].w[1] = 0;
                memset(g_lo[i].k, 0, sizeof g_lo[i].k);
                g_sk4[i] = 0;
            }
            applied += LjImport();
            for (int i = 0; i < N_CUSTOM; i++) {        // whatever was applied is a loadout the game accepts
                const LoadoutT *l = &g_lo[i];
                for (int s = 0; s < 3; s++)
                    if (l->w[s] != 0xffffffffu && l->w[s] >= 6u) {
                        printf("BAD row %u\n", l->w[s]);
                        return 3;
                    }
                if (LoCost(l) > 13) {
                    printf("BAD cost %d\n", LoCost(l));
                    return 3;
                }
                for (int s = 0; s < 3; s++)
                    if (l->k[s] && !SkLv(l->k[s])) {
                        printf("BAD skill %llx\n",
                               (unsigned long long) l->k[s]);
                        return 3;
                    }
            }
            if (g_loActive < 0 || g_loActive >= N_CUSTOM + N_CLASSES) {
                printf("BAD active %d\n", g_loActive);
                return 3;
            }
            g_loActive = 0;
        }
        printf("fuzz ok %d iterations, %d parsed, %d applied\n", iters,
               parsed, applied);
        return 0;
    }
    return 2;
}
