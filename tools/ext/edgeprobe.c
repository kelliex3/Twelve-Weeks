// edgeprobe.c - dev test log for the player's nearby climb / vault edges (Bookstore rails: which FEATURE_DB edges does the engine really see?).
//
// Hooks the exit of NearbyEdgeMgr::GatherEdgesSphereUnsorted (exe+a865a0, nearby-edge-mgr.cpp; exit at a8682d = `mov eax, r15d; vzeroupper`, 6 bytes):
//   rcx(a) = list {u32 count, u32 cap, entries*}, entries 0x20 B, first qword = the RAW edge record (the FEATURE_DB record as loaded: 64 B,
//   P0 P1 NUp NFace flags[4] ... prev next, u16 index at +62); [rsp+0x30] = query sphere {x y z r}; [rsp+0x40] = the player's nearby-edge set
//   {u32 count, ..., items* at +8 (0x40 B each, first qword = raw edge), bounding sphere at +0x40}; r15d = number of edges returned; r13d = mask.
// Off unless t2r-edgeprobe.txt exists next to the exe (polled); not installed in T2R_RELEASE builds. At most one report per 500 ms.
//
// Lines (prefix 'edgeprobe:'):
//   gather   n returned / set count / mask / query sphere / set bounding sphere, then up to 24 returned edges: raw ptr, array base (raw - 64*index),
//            index, flags0..3, endpoints
//   base     once per edge array (= per level FEATURE_DB) first seen: its base, how many consecutive records carry index 0,1,2.. (the count the engine
//            can reach in memory), and whether the records from index 1188 on (our appended range) are there
//   near     the records of every known array within the query sphere, how many of them the returned list holds and how many the set holds, and the
//            first 8 that are in memory but NOT returned (index, flags, endpoints, in set yes / no)
// Reading it: stand at a working Part I box edge, a window, then at a rail. `near` rows with idx >= 1188 that are never returned = the engine does not
// gather our appended edges (array count / set filter); returned but no vault = classification / probe side.
#ifndef T2R_EDGEPROBE_C
#define T2R_EDGEPROBE_C

#ifdef T2R_RELEASE
static void EpInstall(void)
{
} 
#else   /* 
 */
static int EpOk(const void *p, size_t n)
{
    
        // one VirtualQuery per memory region (the first version queried per record = ~10^5 calls per report; every gather call of one vault attempt then took
        // ~0.5 s and the job thread looked frozen)
    static __thread const uint8_t *lo, *hi;
    
if (p && lo && (const uint8_t *) p >= lo
         && (const uint8_t *) p + n <= hi)
        return 1;
    
MEMORY_BASIC_INFORMATION m;
    
if (!p || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT
         || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return 0;
    
lo = (const uint8_t *) m.BaseAddress;
    hi = lo + m.RegionSize;
    
return (const uint8_t *) p + n <= hi;

} 
static int ep_on(void)
{
    
static uint64_t at;
    static int v;
    return FeelFlag("t2r-edgeprobe.txt", &at, &v, 60);

}


typedef struct {
    const uint8_t *base;
    int n;
} EpBaseT;

static EpBaseT g_epBase[8];

static int g_epNb;

static const uint8_t *EpRawBase(const uint8_t *raw)
{
    
        // Known edge tables first - set items and edge records sit in different regions, so ep_ok's one-region cache missed on every call
        // (~500 VirtualQuery per report = 8-9 s freeze on a vault attempt)
        for (int i = 0; i < g_epNb; i++) {
        
const uint8_t *b = g_epBase[i].base;
        
if (raw >= b && raw < b + (size_t) g_epBase[i].n * 64
             && ((size_t) (raw - b) & 63) == 0)
            return b;
    
}
    
if (!EpOk(raw, 64))
        return NULL;
    
uint16_t idx = *(const uint16_t *) (raw + 62);
    
return raw - (size_t) idx *64;

} 
static void EpNoteBase(const uint8_t *base)
{
    
if (!base)
        return;
    
for (int i = 0; i < g_epNb; i++)
        if (g_epBase[i].base == base)
            return;
    
if (g_epNb >= 8)
        return;
    
int n = 0;
    
while (n < 65535 && EpOk(base + (size_t) n * 64, 64)
            && *(const uint16_t *) (base + (size_t) n * 64 + 62) ==
            (uint16_t) n)
        n++;
    
g_epBase[g_epNb].base = base;
    g_epBase[g_epNb].n = n;
    g_epNb++;
    
int g_ = g_xlogNolimit;
    g_xlogNolimit = 1;
    
Xlog("edgeprobe: base %p contiguous records %d (index field 0..%d)%s",
          (const void *) base, n, n - 1,
          
n > 1188 ? " - records 1188.. are in memory" : n ==
          1188 ? " - EXACTLY 1188: the appended range is NOT reachable" :
          "");
    
if (n > 0) {
        
const uint8_t *r = base + (size_t) (n - 1) * 64;
        
Xlog
            ("edgeprobe: base %p last record idx %d flags %02x %02x %02x %02x p0 %.2f %.2f %.2f p1 %.2f %.2f %.2f",
             (const void *) base, n - 1, r[48], r[49], r[50], r[51],
             
*(const float *) r, *(const float *) (r + 4),
             *(const float *) (r + 8), *(const float *) (r + 12),
             *(const float *) (r + 16), *(const float *) (r + 20));
    
}
    
g_xlogNolimit = g_;

} 
static void EpEdgeLine(const char *tag, const uint8_t *raw,
                             const uint8_t *base, int inset)
{
    
const float *f = (const float *) raw;
    
Xlog
        ("edgeprobe: %s raw %p base %p idx %u flags %02x %02x %02x %02x p0 %.2f %.2f %.2f p1 %.2f %.2f %.2f nf %.2f %.2f %.2f%s",
         tag, (const void *) raw, (const void *) base,
         
*(const uint16_t *) (raw + 62), raw[48], raw[49], raw[50],
         raw[51], f[0], f[1], f[2], f[3], f[4], f[5], f[9], f[10], f[11],
         
inset < 0 ? "" : inset ? " inset" : " NOT-in-set");

} 
void
    __attribute__((used)) ep_gather(const uint8_t *list,
                                    const uint8_t *set, const float *sph,
                                    uint32_t count, uint32_t mask)
{
    
static volatile LONG busy;
    static uint64_t last;
    
if (!ep_on())
        return;
    
uint64_t now = GetTickCount64();
    
if (now < last + 500 || InterlockedCompareExchange(&busy, 1, 0) != 0)
        return;
    
uint64_t t0 = now;
    
int g_ = g_xlogNolimit;
    g_xlogNolimit = 1;
    
if (EpOk(set, 0x50) && EpOk(sph, 16) && EpOk(list, 16)) {
        
uint32_t scnt = *(const uint32_t *) set;
        
const uint8_t *items = *(const uint8_t * const *) (set + 8);
        
const float *ss = (const float *) (set + 0x40);
        
Xlog
            ("edgeprobe: gather n=%u setcount=%u mask=%08x sphere %.2f %.2f %.2f r%.2f setsphere %.2f %.2f %.2f r%.2f",
             count, scnt, mask, sph[0], sph[1], sph[2], sph[3], 
ss[0],
             ss[1], ss[2], ss[3]);
        
const uint8_t *ent = *(const uint8_t * const *) (list + 8);
        
uint32_t ln = *(const uint32_t *) list;
        
if (ln > 4096)
            ln = 4096;
        
uint32_t lnv = 0;
        
for (uint32_t i = 0;
              i < ln && ent && EpOk(ent + (size_t) i * 32, 32); i++) {
            
lnv = i + 1;
            
const uint8_t *raw =
                *(const uint8_t * const *) (ent + (size_t) i * 32);
            
const uint8_t *b = EpRawBase(raw);
            
EpNoteBase(b);
            
if (i < 24 && b)
                EpEdgeLine("ret", raw, b, -1);
        
}
        
if (scnt > 4096)
            scnt = 4096;
        
uint32_t scv = 0;
        
for (uint32_t i = 0;
              i < scnt && items && EpOk(items + (size_t) i * 64, 8);
              i++) {
            scv = i + 1;
            EpNoteBase(EpRawBase
                         (*(const uint8_t * const *)
                          (items + (size_t) i * 64)));
        } 
ln = lnv;
        scnt = scv;
        
for (int k = 0; k < g_epNb; k++) {
            
const uint8_t *base = g_epBase[k].base;
            int N = g_epNb ? g_epBase[k].n : 0;
            
int within = 0, inret = 0, inset = 0, miss = 0;
            
for (int j = 0; j < N; j++) {
                
const uint8_t *r = base + (size_t) j * 64;
                const float *f = (const float *) r;
                
float mx = (f[0] + f[3]) * .5f, my =
                    (f[1] + f[4]) * .5f, mz = (f[2] + f[5]) * .5f;
                
float dx = mx - sph[0], dy = my - sph[1], dz =
                    mz - sph[2];
                
if (dx * dx + dy * dy + dz * dz > sph[3] * sph[3])
                    continue;
                
within++;
                
int ir = 0, is = 0;
                
for (uint32_t i = 0; i < ln && ent; i++)
                    if (*(const uint8_t * const *) (ent + (size_t) i * 32)
                        == r) {
                        ir = 1;
                        break;
                    }
                
for (uint32_t i = 0; i < scnt && items && !is; i++)
                    if (*(const uint8_t * const *)
                        (items + (size_t) i * 64) == r)
                        is = 1;
                
inret += ir;
                inset += is;
                
if (!ir && miss < 8) {
                    miss++;
                    EpEdgeLine("miss", r, base, is);
                }
            
}
            
Xlog
                ("edgeprobe: near base %p records %d within sphere %d, returned %d, in set %d",
                 (const void *) base, N, within, inret, inset);
    
} 
}
    
g_xlogNolimit = g_;
    
last = GetTickCount64();   // the 500 ms pause starts when the report is done
    if (last - t0 > 50)
        Xlog("edgeprobe: report took %u ms", (unsigned) (last - t0));
    
InterlockedExchange(&busy, 0);

} 
static void EpInstall(void)
{
    
uint8_t * exe = (uint8_t *) GetModuleHandleW(NULL), *site =
        exe + 0xa8682d;
    
static const uint8_t expect[14] =
        { 0x41, 0x8b, 0xc7, 0xc5, 0xf8, 0x77, 0x48, 0x8b, 0x8c, 0x24, 0xa0,
0x00, 0x00, 0x00 };
    
if (memcmp(site, expect, sizeof expect)) {
        Xlog("HOOK FAIL edgeprobe: GatherEdgesSphereUnsorted exit differs, not installed");
        return;
    }
    
uint8_t * c = AllocNear(exe + 0xa80000, 0x1000);
    
if (!c) {
        Xlog("edgeprobe: no memory near the exe");
        return;
    }
    
int n = 0;
    
#define EMIT(...) do { const uint8_t b_[] = {__VA_ARGS__}; memcpy(c + n, b_, sizeof b_); n += sizeof b_; } while (0)
        EMIT(0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53); // push rax rcx rdx r8 r9 r10 r11 (56 B)
    EMIT(0x48, 0x81, 0xec, 0x98, 0x00, 0x00, 0x00);     // sub rsp, 0x98 (keeps 16 B alignment)
    static const uint8_t xs[6] = { 0x84, 0x8c, 0x94, 0x9c, 0xa4, 0xac };
    
for (int i = 0; i < 6; i++)
        EMIT(0x0f, 0x11, xs[i], 0x24, (uint8_t) (0x30 + 16 * i), 0x00, 0x00, 0x00);     // movups [rsp+0x30+16i], xmm_i
    EMIT(0x48, 0x89, 0xf1);     // mov rcx, rsi   (list)
    EMIT(0x48, 0x8b, 0x94, 0x24, 0x10, 0x01, 0x00, 0x00);       // mov rdx, [rsp+0x110]  (= function [rsp+0x40]: the set)
    EMIT(0x4c, 0x8b, 0x84, 0x24, 0x00, 0x01, 0x00, 0x00);       // mov r8,  [rsp+0x100]  (= function [rsp+0x30]: query sphere)
    EMIT(0x45, 0x89, 0xf9);     // mov r9d, r15d
    EMIT(0x4c, 0x89, 0x6c, 0x24, 0x20); // mov [rsp+0x20], r13   (mask, 5th arg)
    EMIT(0x48, 0xb8); {
        uint64_t fn = (uint64_t) (uintptr_t) & ep_gather;
        memcpy(c + n, &fn, 8);
        n += 8;
    }                           // mov rax, ep_gather
    EMIT(0xff, 0xd0);           // call rax
    for (int i = 0; i < 6; i++)
        EMIT(0x0f, 0x10, xs[i], 0x24, (uint8_t) (0x30 + 16 * i), 0x00, 0x00, 0x00);     // movups xmm_i, [rsp+0x30+16i]
    EMIT(0x48, 0x81, 0xc4, 0x98, 0x00, 0x00, 0x00);     // add rsp, 0x98
    EMIT(0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58);     // pop r11 r10 r9 r8 rdx rcx rax
    EMIT(0x41, 0x8b, 0xc7, 0xc5, 0xf8, 0x77);   // the replaced instructions: mov eax, r15d; vzeroupper
    EMIT(0xe9, 0, 0, 0, 0);
    *(int32_t *) (c + n - 4) = (int32_t) ((exe + 0xa86833) - (c + n));  // jmp back (mov rcx, [rsp+0xa0] ...)
#undef EMIT
    DWORD old;
    
VirtualProtect(c, 0x1000, PAGE_EXECUTE_READ, &old);
    
VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old);
    
site[0] = 0xe9;
    *(int32_t *) (site + 1) = (int32_t) (c - (site + 5));
    
site[5] = 0x90;
    
VirtualProtect(site, 6, old, &old);
    
FlushInstructionCache(GetCurrentProcess(), site, 6);
    
Xlog
        ("edgeprobe: installed (log is written while t2r-edgeprobe.txt exists next to the exe)");

} 
#endif  /* 
 */
#endif  /* 
 */
