// rsdiag.c: which render-settings REGIONS contain the camera, once a second in a match ("rsdiag:" lines in t2r-ext.log).  Why: the winter
// Supermarket shows blinding white in two zones and nothing offline explains it (no exposure region covers them); this log names, for the spot where the
// user stands, every region of the map's level set that holds the camera (priority order), its render-settings symbol and the symbol's tonemap
// light range / set point / bloom intensity (decoded from the DC modules by tools/maps/gen_rsdiag.py into rsdiag_regions.h).  Regions with task gating
// (start-task / end-task / task lists) are listed as "gated": SP switches them by story state, MP never does, so the engine should ignore them - if a
// gated one shows up where the screen goes white, that is the answer.
// What it does NOT read: the engine's live blended render-settings block / auto-exposure value (its address is not known; no RE of the exe was done).  The
// "effective" tonemap numbers are the highest-priority UNGATED region that defines each value (the base cloudy region defines them everywhere), i.e. what the
// DC data asks for; "rsdiag JUMP" is logged when those change between two checks.  Cost: one box scan of ~150 regions per second.  Opt-out: t2r-no-rsdiag.txt.
#include "rsdiag_regions.h"
static int RsdOff(void)
{
    
static int off = -1;
    
if (off < 0) {
        FILE *f = ExeSideOpen("t2r-no-rsdiag.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
        if (off)
            Xlog("rsdiag: off (t2r-no-rsdiag.txt)");
    }
    
return off;

}


static void RsdiagTick(void)
{
    
static uint64_t last_ms, last_log;
    static char prev[1200];
    static float peff[3] = { 1e9f, 1e9f, 1e9f };
    static int inside;
    
if (MatchSubstate() != 6 || RsdOff())
        return;
    
uint64_t now = GetTickCount64();
    
if (now - last_ms < 1000)
        return;
    
last_ms = now;
    
float cp[3], cd[3];
    
if (!CamPosDir(cp, cd))
        return;
    
if (cp[0] < RSD_BOUNDS_X0 || cp[0] > RSD_BOUNDS_X1
         || cp[2] < RSD_BOUNDS_Z0 || cp[2] > RSD_BOUNDS_Z1) {
        
if (inside) {
            inside = 0;
            Xlog("rsdiag: camera left the %s region table (%.1f %.1f %.1f)", RSD_MAP, (double) cp[0], (double) cp[1], (double) cp[2]);
        }
        
return;
    
}
    
inside = 1;
    
char names[1200];
    int n = 0, ng = 0, k = 0;
    names[0] = 0;
    
float eff[3] = { NAN, NAN, NAN };
    float bloom = NAN;
    
for (unsigned i = 0; i < RSD_N; i++) {     // table is sorted by priority, highest first
        const RsdRegion *r = &g_rsdRegions[i];
        
if (cp[0] < r->lo[0] || cp[0] > r->hi[0] || cp[1] < r->lo[1]
             || cp[1] > r->hi[1] || cp[2] < r->lo[2] || cp[2] > r->hi[2])
            continue;
        
if (r->gated) {
            ng++;
            continue;
        }
        
n++;
        
if (__builtin_isnan(eff[0]) && !__builtin_isnan(r->tmin))
            eff[0] = r->tmin;
        
if (__builtin_isnan(eff[1]) && !__builtin_isnan(r->tmax))
            eff[1] = r->tmax;
        
if (__builtin_isnan(eff[2]) && !__builtin_isnan(r->tsp))
            eff[2] = r->tsp;
        
if (__builtin_isnan(bloom) && !__builtin_isnan(r->bloom))
            bloom = r->bloom;
        
if (k < (int) sizeof names - 160) {
            
k +=
                snprintf(names + k, sizeof names - (size_t) k, " %s[%d %s",
                         r->name, (int) r->prio, r->sym);
            
if (!__builtin_isnan(r->tmin + r->tmax))
                k += snprintf(names + k, sizeof names - (size_t) k,
                              " ev %.1f..%.1f", (double) r->tmin,
                              (double) r->tmax);
            
k += snprintf(names + k, sizeof names - (size_t) k, "]");
        
}
    
} 
int change = strcmp(prev, names) != 0;
    
int jump = peff[0] < 1e8f && (fabsf(eff[0] - peff[0]) > 0.01f
                                   || fabsf(eff[1] - peff[1]) > 0.01f
                                   || fabsf(eff[2] - peff[2]) > 0.01f);
    
if (change || jump || now - last_log >= 5000) {
        
last_log = now;
        
Xlog
            ("rsdiag: cam (%.1f %.1f %.1f) dir (%.2f %.2f %.2f) %d ungated region(s) + %d gated (ignored); tonemap %s%.2f..%.2f set %.2f bloom %.2f:%s",
             
(double) cp[0], (double) cp[1], (double) cp[2],
             (double) cd[0], (double) cd[1], (double) cd[2], n, ng,
             jump ? "JUMP " : "", (double) eff[0], (double) eff[1],
             (double) eff[2], 
(double) bloom, names);
        
if (jump)
            Xlog("rsdiag JUMP: tonemap range %.2f..%.2f set %.2f -> %.2f..%.2f set %.2f", (double) peff[0], (double) peff[1], (double) peff[2], (double) eff[0], (double) eff[1], (double) eff[2]);
    
}
    
if (change) {
        strncpy(prev, names, sizeof prev - 1);
        prev[sizeof prev - 1] = 0;
    }
    
peff[0] = eff[0];
    peff[1] = eff[1];
    peff[2] = eff[2];

}


