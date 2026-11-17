// t2r-engine.c - DLL-agnostic engine-symbol resolution (see t2r-engine.h).
//
// Included by t2r-ext.c.  t2r_engine_init() is called once, after g_winmm is
// known, next to s8_init().
#include "t2r-engine.h"
#include <string.h>

uintptr_t g_t2rSymbol[T2R_SYMBOL_COUNT];

// The published name, the reference address, and (for code sites) a byte
// signature to scan for in winmm's code.  A signature is only needed where the
// reference address is not enough; every entry falls back to its reference
// address when neither the manifest nor a signature yields it.
typedef struct {
    const char *name;
    uintptr_t legacy;
    const unsigned char *sig;
    int siglen;
} t2rSymDef;

// Reference addresses and names, straight from the shared contract.
static const t2rSymDef g_t2rDef[T2R_SYMBOL_COUNT] = {
#define X(kind, name, va) { #name, (uintptr_t)(va), 0, 0 },
#include "t2r-engine-symbols.def"
#undef X
};

// Byte signatures for the code sites that carry one, copied from the reference
// build (the same bytes the site verifies itself with).  A code site without a
// signature stays on its reference address.
static const unsigned char sigCallIntro[] =
    { 0xE8, 0x94, 0xCF, 0x01, 0x00 };
static const unsigned char sigCallResp[] =
    { 0xE8, 0xDB, 0x91, 0x01, 0x00 };
static const unsigned char sigCallPup[] = { 0xE8, 0x7F, 0xEA, 0xFF, 0xFF };
static const unsigned char sigPrologItem[] =
    { 0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x53, 0x48,
0x83, 0xEC, 0x30 };
static const unsigned char sigPrologRng[] =
    { 0x55, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48,
0x8D, 0x6C, 0x24, 0x30 };
static const unsigned char sigPrologVal[] =
    { 0x55, 0x56, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x6C, 0x24, 0x20,
0x48, 0xC7, 0x45, 0x00, 0xFE, 0xFF, 0xFF, 0xFF };
static const unsigned char sigPrologEns[] =
    { 0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x81, 0xEC, 0xF0,
0x00, 0x00, 0x00 };
static const unsigned char sigPoseB[] =
    { 0x49, 0x3B, 0xBD, 0xE0, 0x09, 0x00, 0x00 };
static const unsigned char sigPoseC[] =
    { 0x49, 0x8B, 0xBD, 0x91, 0x00, 0x00, 0x00 };

typedef struct {
    int idx;
    const unsigned char *sig;
    int len;
} t2rSigDef;
static const t2rSigDef g_t2rSig[] = {
    { kCodeCallIntro, sigCallIntro, sizeof sigCallIntro },
    { kCodeCallResp, sigCallResp, sizeof sigCallResp },
    { kCodeCallPupResp, sigCallPup, sizeof sigCallPup },
    { kCodeMenuPrologItem, sigPrologItem, sizeof sigPrologItem },
    { kCodeMenuPrologRange, sigPrologRng, sizeof sigPrologRng },
    { kCodeMenuPrologVal, sigPrologVal, sizeof sigPrologVal },
    { kCodeMenuPrologEns, sigPrologEns, sizeof sigPrologEns },
    { kCodePosePatchB, sigPoseB, sizeof sigPoseB },
    { kCodeCallSite36000D, sigPoseC, sizeof sigPoseC },
};

#define T2R_SYM_MAGIC   0x53523254u     // "T2RS"
#define T2R_SYM_VERSION 1u

typedef struct {
    uint64_t sid;
    uint64_t rva;
} t2rManifestEntry;
typedef struct {
    uint32_t magic, version, count, reserved;
    t2rManifestEntry entries[];
} t2rManifest;

static uint64_t t2r_sid(const char *s)
{                               // FNV-1a 64, the engine's StringId
    uint64_t h = 0xCBF29CE484222325ULL;
    while (*s) {
        h ^= (unsigned char) *s++;
        h *= 0x100000001B3ULL;
    }
    return h;
}

// PE section header by name (winmm's section table), or NULL.
static const unsigned char *t2r_section(HMODULE mod, const char *want,
                                        uint32_t *size)
{
    const unsigned char *b = (const unsigned char *) mod;
    if (!b)
        return NULL;
    uint32_t nt = *(const uint32_t *) (b + 0x3C);
    const unsigned char *fh = b + nt + 4;
    uint16_t ns = *(const uint16_t *) (fh + 2), osz =
        *(const uint16_t *) (fh + 16);
    const unsigned char *sh = fh + 20 + osz;
    for (int i = 0; i < ns; i++, sh += 40) {
        if (memcmp(sh, want, strlen(want) + 1))
            continue;           // 8-byte name, NUL padded
        if (size)
            *size = *(const uint32_t *) (sh + 8);       // VirtualSize
        return b + *(const uint32_t *) (sh + 12);       // VirtualAddress
    }
    return NULL;
}

static const t2rManifest *t2r_manifest(HMODULE mod)
{
    const t2rManifest *m =
        (const t2rManifest *) t2r_section(mod, ".t2rsym", NULL);
    if (!m || m->magic != T2R_SYM_MAGIC || m->version != T2R_SYM_VERSION)
        return NULL;
    return m;
}

static uintptr_t t2r_scan(const unsigned char *buf, uint32_t n,
                          const unsigned char *sig, int len)
{
    if (!buf || !sig || len <= 0 || (uint32_t) len > n)
        return 0;
    for (uint32_t i = 0; i + len <= n; i++)
        if (buf[i] == sig[0] && !memcmp(buf + i, sig, len))
            return (uintptr_t) (buf + i);
    return 0;
}

void t2r_engine_init(HMODULE winmm)
{
    static int done;
    if (done)
        return;
    done = 1;
    const t2rManifest *m = t2r_manifest(winmm);
    uint32_t textsz = 0;
    const unsigned char *text = t2r_section(winmm, ".text", &textsz);
    uintptr_t base = (uintptr_t) winmm;
    for (int i = 0; i < T2R_SYMBOL_COUNT; i++) {
        // legacy fallback: the reference build's address, rebased onto the loaded image
        uintptr_t addr =
            winmm ? base + (g_t2rDef[i].legacy - T2R_IMAGE_BASE) : 0;
        int found = 0;
        if (m) {                // 1. the mod publishes this symbol by name
            uint64_t sid = t2r_sid(g_t2rDef[i].name);
            for (uint32_t k = 0; k < m->count; k++)
                if (m->entries[k].sid == sid) {
                    addr = base + m->entries[k].rva;
                    found = 1;
                    break;
                }
        }
        if (!found) {           // 2. a code site may be found by signature
            for (size_t k = 0; k < sizeof g_t2rSig / sizeof g_t2rSig[0];
                 k++)
                if (g_t2rSig[k].idx == i) {
                    uintptr_t hit =
                        t2r_scan(text, textsz, g_t2rSig[k].sig,
                                 g_t2rSig[k].len);
                    if (hit)
                        addr = hit;
                    break;
                }
        }
        g_t2rSymbol[i] = addr;  // 3. otherwise the legacy address stands
    }
}
