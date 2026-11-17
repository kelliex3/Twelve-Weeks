// p1_fire_flicker.c - the flicker of the Part I Bookstore fire-barrel lights. Included from immersion.c.
// The build (tools/part1/p1_fires.py) appends the Forest trash can's point light record twice to the LIGHT_TABLE_1 of
// hun-checkpoint-bookstore.pak (point lights 0 and 1; Part I's table had none). The donor's own flicker (ANIMATED_LIGHTS + the
// light-anims ANIM_GROUP played by ProcessDynLight) is an anim object graph that cannot be moved into the Part I pak, so the ext
// writes the flicker into the light the same way the engine's animated lights do:
//   level objects (region manager exe+0x4250be0: +0x1c8 count, +0x1d0 array; the same struct the pak login fills: exe+1b993d0 rbx =
//   package +0x9b0, +0x30 translation, +0x130 xform object, as the level diag reads them; its name / sid field is not known, so the
//   level is the one whose light table 0 holds the two fire records)
//   -> +0x148 u8 num_light_tables, +0x120 runtime light tables (data-loading exe+1b9976a: a 0x68-byte copy per LIGHT_TABLE_1 made by
//   exe+2918440 at login: version 0xa, +0x10 u32 point count, +0x28 heap copy of the 0x54-byte point records; +0x118 = the pak's
//   own tables, no longer read) -> table 0 +0x28 + 0x54 * index.  The engine's animated-light update (exe+1ded080: object +0x40 ->
//   level +0x120 -> [0] -> point +0x28 + idx * 0x54, spot +0x38 * 0x64, ortho +0x48 * 0xc0, then exe+1416565e0 on the record) works
//   on exactly these runtime records, so a write there is what the renderer sees. Every frame the walk is re-done and each record is
//   checked (the whole 0x54 bytes but the intensity word) before the intensity (+0x44, base 6.93 = the can's) gets the flicker.
// Lines in t2r-ext.log: "p1 fire: ..." on change. Opt-out t2r-no-p1-fire-flicker.txt.
#define P1FF_INT 0x44
static const float P1FF_BASEK[2] = { 3.2f, 6.930239f }; // per record: the upstairs barrel (3.2, radius 4.5) and the stairs one (the can's 6.93)

// the two records as the build writes them (p1_fires.light_record); matched whole except the intensity word
static const uint8_t P1FF_PAT[2][0x54] = { 
        { 0x6f, 0xb7, 0x74, 0x3f, 0x82, 0xbf, 0x92, 0x3e, 0xd9, 0x7e, 0x82,
         0x3d, 0x69, 0xc9, 0xed, 0x42, 0xd5, 0xec, 0xb9, 0x40, 0x9a, 0x4c,
         0x88, 0x43, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0x00,
         0x00, 0x80, 0x3f, 0x69, 0xc9, 0xed, 0x42, 0xd5, 0xec, 0xb9, 0x40,
         0x9a, 0x4c, 0x88, 0x43, 0x09, 0x00, 0x06, 0x00, 0x00, 0x00, 0x90,
         0x40, 0x0a, 0xd7, 0xa3, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0xcd, 0xcc, 0x4c, 0x40, 0x00, 0x00, 0x00, 0x00, 0x0a,
         0xd7, 0xa3, 0x3c, 0x00, 0x00, 0x80, 0x3f }, 
{ 0x6f, 0xb7, 0x74,
                                                       0x3f, 0x82, 0xbf,
                                                       0x92, 0x3e, 0xd9,
                                                       0x7e, 0x82, 0x3d,
                                                       0x03, 0xe3, 0xf5,
                                                       0x42, 0xb0, 0x3d,
                                                       0x33, 0x3f, 0x5c,
                                                       0xc2, 0x80, 0x43,
                                                       0x00, 0x00, 0x80,
                                                       0x3f, 0x00, 0x00,
                                                       0x80, 0x3f, 0x00,
                                                       0x00, 0x80, 0x3f,
                                                       0x03, 0xe3, 0xf5,
                                                       0x42, 0xb0, 0x3d,
                                                       0x33, 0x3f, 0x5c,
                                                       0xc2, 0x80, 0x43,
                                                       0x09, 0x00, 0x06,
                                                       0x00, 0x00, 0x00,
                                                       0x20, 0x41, 0x0a,
                                                       0xd7, 0xa3, 0x3c,
                                                       0x00, 0x00, 0x00,
                                                       0x00, 0x00, 0x00,
                                                       0x00, 0x00, 0x85,
                                                       0xc4, 0xdd, 0x40,
                                                       0x00, 0x00, 0x00,
                                                       0x00, 0xcd, 0xcc,
                                                       0xcc, 0x3d, 0x00,
                                                       0x00, 0x80, 0x3f }
};


static uint64_t g_p1ffA;
static int g_p1ffV = -1;

static int P1ffMatch(const uint8_t *p, int k)
{
    return !memcmp(p, P1FF_PAT[k], P1FF_INT)
        && !memcmp(p + P1FF_INT + 4, P1FF_PAT[k] + P1FF_INT + 4,
                   0x4c - P1FF_INT - 4)
        && !memcmp(p + 0x50, P1FF_PAT[k] + 0x50, 4);
}                               // (+0x44 intensity and +0x4c: the fire probe writes them)

static float P1ffFlicker(double t, int k)
{
    
double ph = k * 2.399;     // the two barrels out of step
    double f =
        0.86 + 0.07 * sin(t * 10.7 + ph) + 0.05 * sin(t * 27.3 +
                                                      2.0 * ph) +
        0.04 * sin(t * 4.1 + 0.5 * ph) 
 +0.03 * sin(t * 53.0 +
                                                     3.0 * ph) * sin(t *
                                                                     1.3 +
                                                                     ph);
    
return (float) (P1FF_BASEK[k] * (f < 0.6 ? 0.6 : f > 1.1 ? 1.1 : f));

} 

// -> the runtime point records of the level's light table 0 (NULL + *why when the walk fails)
static uint8_t *P1ffPointsIn(uint8_t *mgr, uint32_t *n_out,
                               const char **why)
{
    
*n_out = 0;
    
if (!mgr || IsBadReadPtr(mgr + 0x1c8, 0x10)) {
        *why = "no region manager";
        return NULL;
    }
    
uint32_t n = *(uint32_t *) (mgr + 0x1c8);
    
uint8_t ** arr = *(uint8_t ***) (mgr + 0x1d0);
    
*why = "no level object has the fire lights";
    
for (uint32_t i = 0;
          i < n && i < 400 && arr && !IsBadReadPtr(arr + i, 8); i++) {
        
uint8_t * o = arr[i];
        
if (!o || IsBadReadPtr(o, 0x150))
            continue;
        
uint8_t nt = o[0x148];
        
uint8_t ** rt = *(uint8_t ***) (o + 0x120);
        
if (!nt || nt > 16 || !rt || IsBadReadPtr(rt, 8) || !rt[0]
             || IsBadReadPtr(rt[0], 0x68))
            continue;
        
uint8_t * lt = rt[0];
        
uint32_t np = *(uint32_t *) (lt + 0x10);
        
uint8_t * pts = *(uint8_t **) (lt + 0x28);
        
 {                     // diagnostic: every level object with a runtime light table is logged once (is the fire light table alive at all, and what does it hold)
            static uint8_t *seen[64];
            static int ns;
            int dup = 0;
            
for (int q = 0; q < ns; q++)
                if (seen[q] == o)
                    dup = 1;
            
if (!dup && ns < 64) {
                
seen[ns++] = o;
                
Xlog
                    ("p1 fire diag: level object %p tables %u table0 version %u points %u spots %u ortho %u points@%p%s",
                     (void *) o, nt, *(uint32_t *) lt, np,
                     
*(uint32_t *) (lt + 0x14), *(uint32_t *) (lt + 0x18),
                     (void *) pts, 
(np && pts
                                     && !IsBadReadPtr(pts,
                                                      0x54)) ? "" :
                     " (no readable points)");
                
if (np && np < 64 && pts && !IsBadReadPtr(pts, 0x54 * np))
                    
for (uint32_t q = 0; q < np && q < 4; q++)
                        
Xlog
                            ("p1 fire diag:   point %u pos %.2f %.2f %.2f radius %.2f intensity %.3f flags %08x vol %.3f",
                             q, *(float *) (pts + 0x54 * q + 12),
                             *(float *) (pts + 0x54 * q + 16),
                             
*(float *) (pts + 0x54 * q + 20),
                             *(float *) (pts + 0x54 * q + 0x34),
                             *(float *) (pts + 0x54 * q + 0x44),
                             *(uint32_t *) (pts + 0x54 * q + 0x30),
                             *(float *) (pts + 0x54 * q + 0x4c));
            
}
        
} 
if (*(uint32_t *) lt != 0xa || np < 2 || np > 64 || !pts
                || IsBadReadPtr(pts, 0x54 * np))
            continue;
        
if (!P1ffMatch(pts, 0) || !P1ffMatch(pts + 0x54, 1))
            continue;
        
*n_out = np;
        
return pts;
    
}
    
return NULL;

}


static uint8_t *P1ffPoints(uint32_t *n_out, const char **why)
{
    
return
        P1ffPointsIn(*(uint8_t **)
                       ((uint8_t *) GetModuleHandleW(NULL) + 0x4250be0),
                       n_out, why);

}


// ---- diagnostic: are the converted level's runtime light tables created at all, and did the staged ProcessDynLight spawn?
// (1) spatial process registry (exe+0x93bfaa0, as rayd_objs): every process whose type name has "Light" or whose name has "ortho" / "rt-" / "fire" is logged with its
//     type, name, level ref chain (+0x1e8 -> [0] -> +0x40 = level object: +0x118 pak tables, +0x120 runtime tables, +0x148 count) and the runtime table 0 counts.
// (2) whole-process memory scan for the two fire point records (the build's bytes but the intensity word): every hit, and every 8-byte holder of a hit address that
//     sits at +0x28 of a 0x68-byte table struct (version 0xa runtime copy / 0xb pak table) is logged with the struct's version and counts.
// one pass over the committed readable memory: a hit = a 0x54-byte fire record (either one, but the intensity word) at an 8-aligned address; kind = 0 / 1 (record index);
// holders mode (hold != NULL): every 8-aligned qword equal to one of hold[] is reported instead.
static int P1ldPass(const uint64_t *hold, int nhold, uint64_t *hits,
                     int *kinds, int max, uint64_t *bytes_out)
{
    
int nh = 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    
uint8_t * a = (uint8_t *) si.lpMinimumApplicationAddress, *top =
        (uint8_t *) si.lpMaximumApplicationAddress;
    
uint8_t * buf =
        (uint8_t *) VirtualAlloc(NULL, 0x410000, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_READWRITE);
    
if (!buf)
        return -1;
    
uint64_t scanned = 0, first0 = *(const uint64_t *) P1FF_PAT[0];
    
while (a < top) {
        
MEMORY_BASIC_INFORMATION mbi;
        
if (!VirtualQuery(a, &mbi, sizeof mbi))
            break;
        
uint8_t * rb = (uint8_t *) mbi.BaseAddress;
        size_t rs = mbi.RegionSize;
        
int ok = mbi.State == MEM_COMMIT
            && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            && 
(mbi.
                 Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                            | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY));
        
if (ok)
            for (size_t off = 0; off < rs; off += 0x400000) {
                
size_t want =
                    rs - off <
                    0x400000 + 0x54 ? rs - off : 0x400000 + 0x54, got = 0;
                
if (!ReadProcessMemory
                     (GetCurrentProcess(), rb + off, buf, want, &got)
                     || got < 8)
                    continue;
                
scanned += got;
                
for (size_t i = 0; i + 8 <= got; i += 8) {
                    
uint64_t q = *(const uint64_t *) (buf + i);
                    
if (hold) {
                        for (int h = 0; h < nhold; h++)
                            if (q == hold[h] && nh < max) {
                                hits[nh] = (uint64_t) (rb + off + i);
                                if (kinds)
                                    kinds[nh] = h;
                                nh++;
                            }
                        continue;
                    }
                    
if (q != first0 || i + 0x54 > got)
                        continue;
                    
for (int k = 0; k < 2; k++) {
                        
const uint8_t *pat = P1FF_PAT[k], *c = buf + i;
                        
if (!memcmp(c, pat, P1FF_INT)
                             && !memcmp(c + P1FF_INT + 4,
                                        pat + P1FF_INT + 4,
                                        0x54 - P1FF_INT - 4) && nh < max) {
                            hits[nh] = (uint64_t) (rb + off + i);
                            kinds[nh] = k;
                            nh++;
                        }
                    
}
                
}
            
}
        
a = rb + rs;
        if (a <= rb)
            break;
    
}
    
VirtualFree(buf, 0, MEM_RELEASE);
    
if (bytes_out)
        *bytes_out = scanned;
    
return nh;

}


static const char *P1ldStr(char *o, size_t cap, const uint8_t *p,
                             size_t off)
{
    
o[0] = 0;
    if (!p || IsBadReadPtr(p + off, 8))
        return o;
    
int k = 0;
    for (;
         k < (int) cap - 1 && !IsBadReadPtr(p + off + k, 1)
         && p[off + k] >= 0x20 && p[off + k] < 0x7f; k++)
        o[k] = (char) p[off + k];
    o[k] = 0;
    return o;

}


static void P1ldProc(uint8_t *pr, const char *how, int *lights)
{
    
char ty[48] = "?", nm[48];
    
const char *tp = *(const char **) (pr + 0x38);
    
if (tp && !IsBadReadPtr(tp, 40)) {
        int k = 0;
        for (; k < 40 && tp[k] >= 0x20 && tp[k] < 0x7f; k++)
            ty[k] = tp[k];
        ty[k] = 0;
    }
    
P1ldStr(nm, sizeof nm, pr, 0x50);
    
if (!strstr(ty, "Light") && !strstr(nm, "ortho") && !strstr(nm, "rt-")
         && !strstr(nm, "fire"))
        return;
    
(*lights)++;
    
uint8_t * r1e8 = *(uint8_t **) (pr + 0x1e8);
    
uint8_t * lv = r1e8
        && !IsBadReadPtr(r1e8, 8) ? *(uint8_t **) r1e8 : NULL;
    
uint8_t * lo = lv
        && !IsBadReadPtr(lv, 0x48) ? *(uint8_t **) (lv + 0x40) : NULL;
    
Xlog
        ("p1 light diag: [%s] process %u type %s name %s %p +0x1e8 %p -> %p -> level obj %p",
         how, *(uint32_t *) (pr + 0x94), ty, nm, (void *) pr,
         (void *) r1e8, (void *) lv, (void *) lo);
    
if (lo && !IsBadReadPtr(lo, 0x150)) {
        
uint8_t ** pt = *(uint8_t ***) (lo + 0x118), **rt =
            *(uint8_t ***) (lo + 0x120);
        
Xlog("p1 light diag:   level obj tables %u pak %p runtime %p",
              (unsigned) lo[0x148], (void *) pt, (void *) rt);
        
if (rt && !IsBadReadPtr(rt, 8) && rt[0]
             && !IsBadReadPtr(rt[0], 0x68)) {
            
uint8_t * t = rt[0];
            
Xlog
                ("p1 light diag:   runtime table 0 version %u points %u spots %u ortho %u pts %p",
                 *(uint32_t *) t, *(uint32_t *) (t + 0x10),
                 *(uint32_t *) (t + 0x14), *(uint32_t *) (t + 0x18),
                 *(void **) (t + 0x28));
        
}
    
}

} 

#define P1LD_NODE(p, off) ({ uint8_t *nd_ = (p) && !IsBadReadPtr((p) + 0x80, 8) ? *(uint8_t **)((p) + 0x80) : NULL; \
    uint8_t * x_ = nd_
    && !IsBadReadPtr(nd_ + (off), 8) ? *(uint8_t **)(nd_ + (off)) : NULL;
\
x_ && !IsBadReadPtr(x_ + 8, 8) ? *(uint8_t **)(x_ + 8) : NULL;
}) 
 static uint8_t *volatile g_p1fp_rec[8];

static int g_p1fpKind[8];
static volatile int g_p1fpNrec;

static DWORD WINAPI P1ldThread(LPVOID unused)
{
    
(void) unused;
    
uint8_t * exe = (uint8_t *) GetModuleHandleW(NULL);
    
Xlog("p1 light diag: start");
    
 {                         // (1a) the registered processes + (1b) the whole process tree from the root above the first one
        uint8_t *reg = exe + 0x93bfaa0, *first = NULL;
        int seen = 0, lights = 0;
        
if (!IsBadReadPtr(reg, 0x4080))
            for (int w = 0; w < 16; w++) {
                
uint64_t bits =
                    *(volatile uint64_t *) (reg + 0x4000 + w * 8);
                
while (bits) {
                    
int b = __builtin_ctzll(bits);
                    bits &= bits - 1;
                    
const uint8_t *e = reg + (size_t) (w * 64 + b) * 16;
                    
uint8_t * node = *(uint8_t * const *) e;
                    uint32_t pid = *(const uint32_t *) (e + 8);
                    
if (!node || !pid || IsBadReadPtr(node + 8, 8))
                        continue;
                    
uint8_t * pr = *(uint8_t **) (node + 8);
                    
if (!pr || IsBadReadPtr(pr, 0x1f0)
                         || *(uint32_t *) (pr + 0x94) != pid)
                        continue;
                    
seen++;
                    if (!first)
                        first = pr;
                    
P1ldProc(pr, "registry", &lights);
                
}
            
}
        
Xlog("p1 light diag: registry %d processes, %d light-ish", seen,
              lights);
        
uint8_t * root = first, *up;
        
for (int k = 0;
              root && k < 64 && (up = P1LD_NODE(root, 0x28)) && up != root;
              k++)
            root = up;
        
int walked = 0, tl = 0, ntypes = 0;
        char types[16][40];
        
for (uint8_t * p = root; p && walked < 60000; walked++) {
            
if (!IsBadReadPtr(p, 0x1f0)) {
                
P1ldProc(p, "tree", &tl);
                
if (ntypes < 16) {
                    
char ty[40];
                    const char *tp = *(const char **) (p + 0x38);
                    
ty[0] = 0;
                    if (tp && !IsBadReadPtr(tp, 40)) {
                        int k = 0;
                        for (; k < 39 && tp[k] >= 0x20 && tp[k] < 0x7f;
                             k++)
                            ty[k] = tp[k];
                        ty[k] = 0;
                    }
                    
int dup = 0;
                    for (int q = 0; q < ntypes; q++)
                        if (!strcmp(types[q], ty))
                            dup = 1;
                    
if (!dup && ty[0])
                        strcpy(types[ntypes++], ty);
                
}
            
}
            
uint8_t * nx = P1LD_NODE(p, 0x30);
            
while (!nx && p && p != root) {
                nx = P1LD_NODE(p, 0x38);
                if (!nx)
                    p = P1LD_NODE(p, 0x28);
            }
            
p = nx;
        
}
        
char tl_s[700];
        tl_s[0] = 0;
        size_t q = 0;
        
for (int i = 0; i < ntypes && q < sizeof tl_s - 50; i++)
            q += (size_t) snprintf(tl_s + q, sizeof tl_s - q, " %s",
                                   types[i]);
        
Xlog
            ("p1 light diag: process tree from root %p: %d processes walked, %d light-ish; first types:%s",
             (void *) root, walked, tl, tl_s);
    
} 
 {                      // (2) the memory scan, with timings, every time
        uint64_t hits[32];
        int kinds[32];
        uint64_t bytes = 0;
        uint64_t t0 = GetTickCount64();
        
Xlog("p1 light diag: memory scan 1 (fire records) start");
        
int nh = P1ldPass(NULL, 0, hits, kinds, 32, &bytes);
        
Xlog
            ("p1 light diag: memory scan 1 done: %d record copies (%d ms, %llu MB scanned)%s",
             nh, (int) (GetTickCount64() - t0),
             (unsigned long long) (bytes >> 20),
             nh < 0 ? " (alloc failed)" : "");
        
for (int i = 0; i < nh; i++)
            Xlog("p1 light diag:   record %d copy at %llx", kinds[i],
                 (unsigned long long) hits[i]);
        
 {                     // fire probe: keep the heap copies (not our own module's pattern constant) for the Ctrl+F6 light-off step
            HMODULE me = NULL;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR) & P1ldThread, &me);
            uint64_t m0 = (uint64_t) me, msz = 0;
            if (me) {
                const uint8_t *pe =
                    (const uint8_t *) me +
                    *(const int32_t *) ((const uint8_t *) me + 0x3c);
                msz = *(const uint32_t *) (pe + 0x50);
            }                   // SizeOfImage
            for (int i = 0; i < nh && g_p1fpNrec < 8; i++) {
                uint64_t m1 = m0 + msz;
                if (hits[i] >= m0 && hits[i] < m1)
                    continue;
                g_p1fp_rec[g_p1fpNrec] = (uint8_t *) hits[i];
                g_p1fpKind[g_p1fpNrec++] = kinds[i];
            }
            Xlog("fire probe: %d light record copies outside the ext kept for the light-off step", g_p1fpNrec);
        }
        
if (nh > 0) {
            
uint64_t holders[48];
            int hk[48];
            t0 = GetTickCount64();
            
Xlog("p1 light diag: memory scan 2 (holders) start");
            
int nhd = P1ldPass(hits, nh, holders, hk, 48, &bytes);
            
Xlog
                ("p1 light diag: memory scan 2 done: %d holder(s) (%d ms)",
                 nhd, (int) (GetTickCount64() - t0));
            
for (int j = 0; j < nhd; j++) {
                
uint8_t * tb = (uint8_t *) (holders[j] - 0x28);
                
if (IsBadReadPtr(tb, 0x68)) {
                    Xlog("p1 light diag:   holder %llx of copy %d: struct unreadable", (unsigned long long) holders[j], hk[j]);
                    continue;
                }
                
Xlog
                    ("p1 light diag:   holder %llx of copy %d: table struct version %u points %u spots %u ortho %u",
                     (unsigned long long) holders[j], hk[j],
                     *(uint32_t *) tb, *(uint32_t *) (tb + 0x10),
                     *(uint32_t *) (tb + 0x14), *(uint32_t *) (tb + 0x18));
        
} 
}
        
Xlog("p1 light diag: done");
    
} 
return 0;

}


#undef P1LD_NODE
// ---- fire probe (dev builds only): the 2nd-floor barrel "fireball" bisect. Ctrl+F6 in a match steps through the suspects; each
// step switches ONE thing off (the others back on) and logs it: 1 = the 2nd-floor barrel's light (point record 0), 2.. = one grafted fire
// spawner of that barrel hidden every frame (hide-object-for-one-frame on its spawner name; its return value is logged), last = all back on.
static int g_p1fpStep;

#ifndef T2R_RELEASE
static const char *const P1FP_SP[] =
    { "fx-barrel-1", "fx-barrel-smoke-1", "fx-bonfire-tiny-41",
"fx-bonfire-tiny-42", "fx-bonfire-tiny-44", 
"fx-bonfire-tiny-45", "fx-bonfire-tiny-46",
"fx-fire-looped-packed-window-med-3", "fx-fire-looped-packed-window-sm-11",

"fx-fire-looped-packed-window-sm-12", "fx-fireb-embers-1", "fx-fireB-smoke-1", "fx-heat-distort-7"
};


#define P1FP_N ((int)(sizeof P1FP_SP / sizeof *P1FP_SP))
static void P1fpTick(int in_match) {
    
 if (!in_match) {
        g_p1fpStep = 0;
        return;
    }
    
static int down;
    
int k = (GetAsyncKeyState(VK_CONTROL) & 0x8000)
        && (GetAsyncKeyState(VK_F6) & 0x8000);
    
if (k && !down && GameFocused()) {
        
g_p1fpStep = (g_p1fpStep + 1) % (P1FP_N + 3);
        
if (g_p1fpStep == 0)
            Xlog("fire probe: step 0 - everything back on");
        
        else if (g_p1fpStep == 1)
            Xlog("fire probe: step 1 - 2nd-floor barrel LIGHT off (point record 0 intensity 0)");
        
        else if (g_p1fpStep == 2)
            Xlog("fire probe: step 2 - 2nd-floor barrel light on, its +0x4c word 0 (volumetric scale?)");
        
        else
            Xlog("fire probe: step %d - spawner t2r-fire-barrel-main-%s hidden", g_p1fpStep, P1FP_SP[g_p1fpStep - 3]);
    
}
    
down = k;
    
 {                         // step 1: the light record copies the diag scan found (~3-4 min after the level loaded) get intensity 0 / back to their value
        static float keep[8];
        static int off;
        
int want = g_p1fpStep == 1;
        
if (want != off) {
            
for (int i = 0; i < g_p1fpNrec; i++) {
                
uint8_t * r = g_p1fp_rec[i];
                DWORD old;
                
if (!r || IsBadReadPtr(r, 0x54)
                     || !VirtualProtect(r + P1FF_INT, 4, PAGE_READWRITE,
                                        &old))
                    continue;
                
if (want) {
                    keep[i] = *(volatile float *) (r + P1FF_INT);
                    *(volatile float *) (r + P1FF_INT) = 0.0f;
                }
                
                else
                    *(volatile float *) (r + P1FF_INT) = keep[i];
                
VirtualProtect(r + P1FF_INT, 4, old, &old);
                
Xlog("fire probe:   light record %d copy %p intensity %s",
                      g_p1fpKind[i], (void *) r,
                      want ? "-> 0" : "restored");
            
} 
if (want && !g_p1fpNrec)
                Xlog("fire probe:   no light record copy found yet (the scan runs ~4 min after Bookstore loads)");
            
off = want;
        
}
    
}
    
if (g_p1fpStep >= 3) {
        
char nm[96];
        snprintf(nm, sizeof nm, "t2r-fire-barrel-main-%s",
                 P1FP_SP[g_p1fpStep - 3]);
        
uint64_t a[2] = { FNV64(nm), 0 }, r =
            DcCall(FNV64("hide-object-for-one-frame"), 1, a);
        
static int last_step = -1;
        static uint64_t last_r = 0x5a5a;
        
if (last_step != g_p1fpStep || last_r != r) {
            last_step = g_p1fpStep;
            last_r = r;
            Xlog("fire probe:   hide-object-for-one-frame(%s) -> %llx", nm,
                 (unsigned long long) r);
        }
    
}

} 
#else   /* 
 */
static void P1fpTick(int in_match) {
    (void) in_match;
} 
#endif  /* 
 */
static void ImmP1FireTick(int in_match) {
    
 static unsigned fr;
    static int loaded, state = -1;
    static const char *last_why;
    
static uint8_t *pts;
    static uint32_t np;
    
P1fpTick(in_match);
    
if (!in_match) {
        loaded = 0;
        state = -1;
        last_why = NULL;
        pts = NULL;
        return;
    }
    
if (ImmNoflag("t2r-no-p1-fire-flicker.txt", &g_p1ffA, &g_p1ffV))
         return;
    
if ((fr++ % 60) == 0) {
        
uint64_t sid = FNV64("hun-checkpoint-bookstore"), l =
            DcCall(FNV64("is-level-loaded?"), 1, &sid);
        
loaded = l && l != ~0ULL;
    
}
    
if (!loaded) {
        state = -1;
        pts = NULL;
        return;
    }
    
#ifndef T2R_RELEASE
    {                           // the diagnostic (DEV builds only): once, 8 s after the level is loaded. It reads EVERY committed page of the process twice (30 GB,
        // 100-195 s each: ws 12 -> 25 GB, 0.2-1.5 M page faults / 10 s) = the live 175 Bookstore stutter on 16 / 32 GB PCs.
        // Its only product, the light record copies, feeds the dev-only fire probe (p1fp_tick, Ctrl+F6); a release build has no use for it.
        static uint64_t t_loaded;
        static int started;
        
uint64_t now = GetTickCount64();
        
if (!t_loaded)
            t_loaded = now;
        
if (!started && now - t_loaded > 8000
             && GetFileAttributesA("t2r-p1-lightdiag.txt") !=
             INVALID_FILE_ATTRIBUTES) {
            started = 1;
            CreateThread(NULL, 0, P1ldThread, NULL, 0, NULL);
        }
    
}
    
#endif  /* 
 */
    const char *why = NULL;
    
if (pts
         && (IsBadReadPtr(pts, 0xa8) || !P1ffMatch(pts, 0)
             || !P1ffMatch(pts + 0x54, 1)))
        pts = NULL;
    
if (!pts && (fr % 60) == 1)
        pts = P1ffPoints(&np, &why);   // the walk once a second until found; the found records are re-checked every frame
    if (!pts) {
        
static int nl;
        
if (why && why != last_why && nl < 12) {
            nl++;
            Xlog("immersion: p1 fire: no flicker - %s", why);
        }
        
if (why)
            last_why = why;
        
state = 0;
        
return;
    
}
    
double t = GetTickCount64() / 1000.0;
    
for (int k = 0; k < 2; k++)
        *(volatile float *) (pts + 0x54 * k + P1FF_INT) =
            P1ffFlicker(t, k);
    
 {                         // fire probe (dev, Ctrl+F6): step 1 = record 0 dark, step 2 = record 0 lit with its +0x4c word 0 (restored otherwise)
        static float keep4c = -1.0f;
        
if (keep4c < 0.0f)
            keep4c = *(volatile float *) (pts + 0x4c);
        
if (g_p1fpStep == 1)
            *(volatile float *) (pts + P1FF_INT) = 0.0f;
        
*(volatile float *) (pts + 0x4c) =
            g_p1fpStep == 2 ? 0.0f : keep4c;
    
} 
 
if (state != 1) {
        
static int nl;
        
if (nl < 12) {
            nl++;
            Xlog("immersion: p1 fire: flicker on (runtime light table 0 at %p, %u point lights, records 0 and 1 = the fire lights; intensity %.2f..%.2f, record 0 / 1 bases 3.2 / 6.93)", 
(void *) pts, np, P1FF_BASEK[0] * 0.6, P1FF_BASEK[1] * 1.1);
        }
        
state = 1;
        last_why = NULL;
    
}

} 
