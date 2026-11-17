// clancard.h - the pure core of the generic corner card: clan member STATUS UPDATES (hungry / sad / sick / dead ..., tools/clan_status.py) and clan perk pops
// (clanperk.c) share one card: headshot (member + expression) + name banner + item icon + one line. No engine calls: parse of the daemon's status tail, the moment gate, card text.
// Tested offline by tests/clancard_test.c; clancard.c wires it into the game.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
enum { CC_PERK = 0, CC_HUNGRY, CC_SAD, CC_SICK, CC_DEAD, CC_INJURED, CC_RECOVERED, CC_NEW, CC_DEPRESSED, CC_SHOCK, CC_KINDS };  // 1..7 = the wire status codes of tools/clan_status.py; 8, 9 arrive in the extended token scx<k> (an old ext reads sci<k> = 2)
enum { CC_EX_NEUTRAL, CC_EX_HAPPY, CC_EX_HUNGRY, CC_EX_SAD, CC_EX_SICK, CC_EX_DEAD };   // headshot expression = the portraits' image id suffix
typedef struct {
    int kind, expr, look, female, role,
        icon /* clanperk category 1..5, 0 = none */ ;
    char name[24];
    char line[160];
    float secs;
    char pic[24]
        /* portrait "<model>_<expression>" (c13p icon), "" = silhouette */
        ;
} CcCardT;
#define CC_EV_MAX 4
typedef struct {
    int n;
    struct {
        int code, day, female, look;
        char name[24];
    } e[CC_EV_MAX];
} CcStatusT;

static int CcExprOf(int kind)
{
    switch (kind) {
    case CC_HUNGRY:
        return CC_EX_HUNGRY;
    case CC_SAD:
        return CC_EX_SAD;
    case CC_SICK:
    case CC_INJURED:
        return CC_EX_SICK;
    case CC_DEAD:
        return CC_EX_DEAD;
    case CC_RECOVERED:
    case CC_PERK:
        return CC_EX_HAPPY;
    default:
        return CC_EX_NEUTRAL;
    }
}

// the moment gate: a status update is NEVER a card in the game any more (in game they appear only in the ESC menu list, clancard.c cc_pause_tick; the
// launcher shows them as notes); a perk pop ONLY in the match at the spawn moment (one time, at spawn, no other moment)
static int CcAllowed(int kind, int in_match, int respawn_moment)
{
    return kind == CC_PERK ? (in_match && respawn_moment) : 0;
}

// "sc=2 sci0=1 scd0=9 scg0=0 scl0=2 sci1=4 ... scm0=Cameron scm1=Ava" anywhere in the state text (tools/clan_status.wire_text). Old daemon: none (n = 0). Malformed tokens are skipped.
static void CcParseStatus(const char *t, CcStatusT *o)
{
    int cnt = 0, xcode[CC_EV_MAX] = { 0 }, code[CC_EV_MAX] =
        { 0 }, day[CC_EV_MAX] = { 0 }, fem[CC_EV_MAX] =
        { 0 }, look[CC_EV_MAX] = { 0 };
    char nm[CC_EV_MAX][24];
    memset(nm, 0, sizeof nm);
    memset(o, 0, sizeof *o);
    const char *p = t ? strstr(t, " sc=") : NULL;
    if (!p)
        return;
    while (*p) {
        while (*p == ' ')
            p++;
        const char *e = p;
        while (*e && *e != ' ')
            e++;
        char tok[64];
        size_t L = (size_t) (e - p);
        if (L > 0 && L < sizeof tok) {
            memcpy(tok, p, L);
            tok[L] = 0;
            char *eq = strchr(tok, '=');
            if (eq && tok[0] == 's' && tok[1] == 'c') {
                *eq++ = 0;
                const char *k = tok + 2;
                if (!*k)
                    cnt = atoi(eq);
                else if ((k[0] == 'i' || k[0] == 'd' || k[0] == 'g'
                          || k[0] == 'l' || k[0] == 'm' || k[0] == 'x')
                         && k[1] >= '0' && k[1] < '0' + CC_EV_MAX
                         && !k[2]) {
                    int i = k[1] - '0', v = atoi(eq);
                    switch (k[0]) {
                    case 'i':
                        code[i] = v;
                        break;
                    case 'x':
                        xcode[i] = v;
                        break;
                    case 'd':
                        day[i] = v;
                        break;
                    case 'g':
                        fem[i] = v != 0;
                        break;
                    case 'l':
                        look[i] = v < 0 ? 0 : v > 3 ? 3 : v;
                        break;
                    case 'm':
                        snprintf(nm[i], sizeof nm[i], "%.23s", eq);
                        for (char *q = nm[i]; *q; q++)
                            if (*q == '_')
                                *q = ' ';
                        break;
                    }
                }
            }
        }
        p = e;
    }
    if (cnt > CC_EV_MAX)
        cnt = CC_EV_MAX;
    for (int i = 0; i < cnt; i++) {
        int ext = xcode[i] == CC_DEPRESSED || xcode[i] == CC_SHOCK;
        if (ext)
            code[i] = xcode[i]; // new clients read the real state, old ones got sad (2)
        if (code[i] < CC_HUNGRY || (code[i] > CC_NEW && !ext))
            continue;
        o->e[o->n].code = code[i];
        o->e[o->n].day = day[i];
        o->e[o->n].female = fem[i];
        o->e[o->n].look = look[i];
        snprintf(o->e[o->n].name, sizeof o->e[o->n].name, "%.23s",
                 nm[i][0] ? nm[i] : "Someone");
        o->n++;
    }
}

// one status update as a card (banner = the member's name, line = what happened; EN, or the Polish line with gender endings)
static void CcStatusCard(CcCardT *c, int code, const char *name,
                           int female, int look, int polish)
{
    static const char *const EN[CC_KINDS] =
        { "", "HAS GONE HUNGRY", "IS GRIEVING", "HAS FALLEN SICK",
"DID NOT MAKE IT", "IS HURT", "IS BACK ON THEIR FEET", "HAS JOINED THE CLAN",
"HAS WITHDRAWN COMPLETELY", "IS IN SHOCK" };
    static const char *const PLM[CC_KINDS] =
        { "", "JEST GLODNY", "JEST W ZALOBIE", "ZACHOROWAL", "NIE PRZEZYL",
"JEST RANNY", "STANAL NA NOGI", "DOLACZYL DO KLANU", "ZAMKNAL SIE W SOBIE", "JEST W SZOKU" };
    static const char *const PLF[CC_KINDS] =
        { "", "JEST GLODNA", "JEST W ZALOBIE", "ZACHOROWALA",
"NIE PRZEZYLA", "JEST RANNA", "STANELA NA NOGI", "DOLACZYLA DO KLANU", "ZAMKNELA SIE W SOBIE",
"JEST W SZOKU" };
    memset(c, 0, sizeof *c);
    c->kind = code;
    c->expr = CcExprOf(code);
    c->look = look;
    c->female = female;
    c->secs = 5.0f;
    snprintf(c->name, sizeof c->name, "%s", name
             && name[0] ? name : "Someone");
    snprintf(c->line, sizeof c->line, "%s", code > 0
             && code <
             CC_KINDS ? (polish ? (female ? PLF[code] : PLM[code]) :
                         EN[code]) : "");
}

// an id of an update for the 'shown once' memory (member name + code + day)
static uint32_t CcEventId(const char *name, int code, int day)
{
    uint32_t h = 2166136261u;
    for (const char *s = name; *s; s++)
        h = (h ^ (uint8_t) * s) * 16777619u;
    return (h ^ (uint32_t) code * 0x9e3779b1u) *
        16777619u ^ (uint32_t) day;
}
