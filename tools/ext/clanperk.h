// clanperk.h - the pure core of the clan supply perks (tools/clan_perks.py, re/clan-supply-boosters.md). No engine calls: parsing of the daemon's perk tail,
// the per-spawn plan, the report packing, the card text. Tested offline by tests/clanperk_test.c; clanperk.c wires it into the game.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#define CP_N 5                  // perks, in clan_perks.PERKS order = the wire category 1..5
enum { CP_MEDKIT, CP_CLIP, CP_PARTS, CP_STAMINA, CP_BLEED };
typedef struct {
    int cat, cap, female, look, role;
    char name[24];
    char pic[24];
} CpPerkT;
typedef struct {
    int n, streak;
    CpPerkT p[CP_N];
} CpStateT;                   // p[k] = the perk of wire category k + 1 (cat 0 = not granted)
static const int CP_CAP_MAX[CP_N] = { 2, 3, 2, 1, 2 };  // the daemon's per-match caps (clan_perks.CAP): anything above is clamped

// "pk=3 pkc0=1 pkn0=2 pkg0=1 pkl0=1 pkr0=3 ... pkm0=Lila_B" anywhere in the state text. An old daemon sends none (n = 0). Unknown / malformed tokens are skipped.
static void CpParseTail(const char *t, CpStateT *o)
{
    int cat[CP_N] = { 0 }, cap[CP_N] = { 0 }, fem[CP_N] =
        { 0 }, look[CP_N] = { 0 }, role[CP_N] = { 0 }, cnt = 0, streak = 0;
    char nm[CP_N][24], pc[CP_N][24];
    memset(nm, 0, sizeof nm);
    memset(pc, 0, sizeof pc);
    memset(o, 0, sizeof *o);
    if (!t)
        return;
    while (*t) {
        while (*t == ' ')
            t++;
        const char *e = t;
        while (*e && *e != ' ')
            e++;
        char tok[64];
        size_t L = (size_t) (e - t);
        if (L > 0 && L < sizeof tok) {
            memcpy(tok, t, L);
            tok[L] = 0;
            char *eq = strchr(tok, '=');
            if (eq && tok[0] == 'p' && tok[1] == 'k') {
                *eq++ = 0;
                const char *k = tok + 2;
                if (!*k)
                    cnt = atoi(eq);
                else if (k[0] == 's' && !k[1])
                    streak = atoi(eq);  // pks = the clan's loss streak (matches in a row lost)
                else if ((k[0] == 'c' || k[0] == 'n' || k[0] == 'g'
                          || k[0] == 'l' || k[0] == 'r' || k[0] == 'm'
                          || k[0] == 'p') && k[1] >= '0'
                         && k[1] < '0' + CP_N && !k[2]) {
                    int i = k[1] - '0', v = atoi(eq);
                    switch (k[0]) {
                    case 'c':
                        cat[i] = v;
                        break;
                    case 'n':
                        cap[i] = v;
                        break;
                    case 'g':
                        fem[i] = v != 0;
                        break;
                    case 'l':
                        look[i] = v & 3;
                        break;
                    case 'r':
                        role[i] = v;
                        break;
                    case 'p':
                        for (int q = 0; q < 23 && eq[q]; q++)
                            pc[i][q] = (isalnum((unsigned char) eq[q])
                                        || eq[q] == '_'
                                        || eq[q] == '-') ? eq[q] : '_';
                        break;
                    default:
                        for (int q = 0; q < 23 && eq[q]; q++)
                            nm[i][q] = eq[q] == '_' ? ' ' : eq[q];
                    }
                }
            }
        }
        t = e;
    }
    o->streak = streak < 0 ? 0 : streak > 9 ? 9 : streak;
    if (cnt < 0)
        cnt = 0;
    if (cnt > CP_N)
        cnt = CP_N;
    for (int i = 0; i < cnt; i++) {
        if (cat[i] < 1 || cat[i] > CP_N || cap[i] < 1)
            continue;
        CpPerkT *p = &o->p[cat[i] - 1];
        p->cat = cat[i];
        p->cap =
            cap[i] >
            CP_CAP_MAX[cat[i] - 1] ? CP_CAP_MAX[cat[i] - 1] : cap[i];
        p->female = fem[i];
        p->look = look[i];
        p->role = role[i];
        memcpy(p->name, nm[i], sizeof p->name);
        memcpy(p->pic, pc[i], sizeof p->pic);
        if (!p->name[0])
            strcpy(p->name, "Your clan");
        o->n++;
    }
}

// WHEN the help comes (abundant supplies + a losing streak): at a spawn, when the player is STRUGGLING. bad = lives lost in a row without downing
// an enemy (counted from the player's down, cleared by the player's own down of an enemy), gap = enemy reinforcements - ours (0 when unknown), streak = the clan's lost matches in
// a row (daemon 'pks='), spawn_no = this match's spawn count (1 = the first).  Returns 0 = not struggling, else the reason: 1 two lives lost in a row, 2 one life lost while the
// team is 4+ reinforcements behind or the clan has lost 2+ matches in a row, 3 the clan has lost 3+ matches in a row and this is the first spawn of the match.
enum { CP_WHY_NONE, CP_WHY_BAD2, CP_WHY_BEHIND, CP_WHY_STREAK };
static int CpStruggling(int bad, int gap, int streak, int spawn_no)
{
    if (bad >= 2)
        return CP_WHY_BAD2;
    if (bad >= 1 && (gap >= 4 || streak >= 2))
        return CP_WHY_BEHIND;
    if (streak >= 3 && spawn_no == 1)
        return CP_WHY_STREAK;
    return CP_WHY_NONE;
}

#define CP_GRANT_GAP_MS 90000   // at least this long between two help grants of a match
#define CP_N_ITEM 3             // the item perks that a streak can trigger: medkit, clip, crafting set (stamina / bleed are passive)
// WHICH item: among the item perks that are active, below their cap (used[] < cap) and applicable now (ok mask bit per perk), the one used least this match; ties: medkit, clip, parts.
// Returns the perk index or -1.
static int CpPickItem(const CpStateT *s, const int used[CP_N],
                        unsigned ok)
{
    static const int ORDER[CP_N_ITEM] = { CP_MEDKIT, CP_CLIP, CP_PARTS };
    int best = -1;
    for (int k = 0; k < CP_N_ITEM; k++) {
        int i = ORDER[k];
        if (!s->p[i].cat || !(ok & (1u << i)) || used[i] >= s->p[i].cap)
            continue;
        if (best < 0 || used[i] < used[best])
            best = i;
    }
    return best;
}

// one grant per streak: after a grant the help is disarmed until the player downs an enemy again (or a new match starts); and never twice within CP_GRANT_GAP_MS
static int CpGrantDue(int armed, uint64_t now, uint64_t last_grant)
{
    return armed && (!last_grant || now - last_grant >= CP_GRANT_GAP_MS);
}

// extra rounds for the primary's reserve: one clip (mag, 6 when unknown), never above the 2013 reserve cap (cap 0 = none)
static int CpClipExtra(int mag, int have, int cap)
{
    int x = mag > 0 ? mag : 6;
    if (cap > 0 && have + x > cap)
        x = cap - have;
    return x > 0 ? x : 0;
}

// the match report's labels 31 / 32 (nibble per category: 31 = med | ammo << 4 | mat << 8, 32 = food | water << 4), used[] in wire order
static void CpPackUses(const int used[CP_N], int *l31, int *l32)
{
    int u[CP_N];
    for (int i = 0; i < CP_N; i++)
        u[i] = used[i] < 0 ? 0 : used[i] > 15 ? 15 : used[i];
    *l31 = u[0] | (u[1] << 4) | (u[2] << 8);
    *l32 = u[3] | (u[4] << 4);
}

// "TYLER PROVIDED YOU WITH A MEDKIT" (EN) / "TYLER PRZYSLAL CI APTECZKE" (PL, diacritics folded for the raw draw; the verb takes the member's gender)
static void CpCardText(char *out, size_t n, const char *name, int cat,
                         int female, int polish)
{
    static const char *const EN[CP_N] =
        { "PROVIDED YOU WITH A MEDKIT", "PROVIDED YOU WITH A SPARE CLIP",
"PROVIDED YOU WITH CRAFTING SUPPLIES",
        "KEEPS YOU RUNNING", "BOUGHT YOU TIME"
    };
    static const char *const PLM[CP_N] =
        { "PRZYSLAL CI APTECZKE", "PRZYSLAL CI ZAPASOWY MAGAZYNEK",
"PRZYSLAL CI SKLADNIKI DO SKLADANIA", "DODAJE CI SIL W BIEGU", "DAJE CI WIECEJ CZASU" };
    static const char *const PLF[CP_N] =
        { "PRZYSLALA CI APTECZKE", "PRZYSLALA CI ZAPASOWY MAGAZYNEK",
"PRZYSLALA CI SKLADNIKI DO SKLADANIA", "DODAJE CI SIL W BIEGU", "DAJE CI WIECEJ CZASU" };
    int i = cat - 1;
    char up[32];
    size_t q = 0;
    for (const char *s = name && name[0] ? name : "Your clan";
         *s && q + 1 < sizeof up; s++)
        up[q++] = (char) toupper((unsigned char) *s);
    up[q] = 0;
    if (i < 0 || i >= CP_N) {
        snprintf(out, n, "%s", up);
        return;
    }
    snprintf(out, n, "%s %s", up,
             polish ? (female ? PLF[i] : PLM[i]) : EN[i]);
}

// "medkit x2 (Tyler), clip x3 (Wendy)" for the gear up screen
static void CpSummary(char *out, size_t n, const CpStateT *s,
                       int polish)
{
    static const char *const EN[CP_N] =
        { "medkit", "clip", "crafting set", "sprint recovery",
"bleed-out time" };
    static const char *const PL[CP_N] =
        { "apteczka", "magazynek", "zestaw do skladania", "sila w biegu",
"czas wykrwawienia" };
    size_t o = 0;
    if (n)
        out[0] = 0;
    for (int i = 0; i < CP_N; i++) {
        if (!s->p[i].cat)
            continue;
        int w =
            snprintf(out + o, o < n ? n - o : 0, "%s%s x%d (%s)",
                     o ? ", " : "", polish ? PL[i] : EN[i], s->p[i].cap,
                     s->p[i].name);
        if (w < 0 || (size_t) w >= n - o)
            break;
        o += (size_t) w;
    }
}

// voice gate: one line per 20 s, never when the last one is younger
static int CpVoiceDue(uint64_t now, uint64_t last)
{
    return !last || now - last >= 20000;
}
