// loadjson.c - the launcher's GEAR UP <-> the game's custom loadouts, by NAME (t2r-ext include, after skills.c).
//
// Two files next to tlou-ii.exe (contract: release/launcher/API.md "Loadout files"):
//   t2r-loadout.json           written by the game: the 6 custom loadouts + active pick, weapon ids ("rifle-mpx5", "pistol-beretta" +
//                              "supp":1 for the suppressed row) and skill keys ("SK_REVIVER" + level). Rewritten whenever it changes (and once
//                              at start), atomically (tmp + rename). Also records "launcher_rev": the launcher file rev applied last.
//   t2r-loadout-launcher.json  written by the launcher (same shape, rev = unix ms). When its rev is newer than "launcher_rev" the game applies
//                              it in a menu (not in a match, not on the GEAR UP pages, no pending mid-match pick): names -> rows of the
//                              live tables, unknown ids keep that slot's current value, a loadout over 13 LP keeps its old value, then
//                              the same path as an in-game save (g_lo / g_sk4 -> t2r-loadout.txt + t2r-skills.txt, lobby object -> the
//                              server PROFILE as usual), then t2r-loadout.json is rewritten so both agree.
// The row indexes (t2r-loadout.txt) are never exposed: they depend on tables built at run time.

#define LJ_K(x) [x] = #x
static const char *const LJ_SK[SK_COUNT] = {
    LJ_K(SK_ZOOM), LJ_K(SK_EXPLO), LJ_K(SK_REVIVER), LJ_K(SK_BRAWLER),
        LJ_K(SK_COVERT), LJ_K(SK_EARS), LJ_K(SK_STRAT), LJ_K(SK_HAWK),
        LJ_K(SK_CRAFTER),
    LJ_K(SK_FAT), LJ_K(SK_SHARP), LJ_K(SK_MARATHON), LJ_K(SK_COLLECTOR),
        LJ_K(SK_EXEC), LJ_K(SK_DMGMARK), LJ_K(SK_GUNSLINGER),
        LJ_K(SK_BOMB),
    LJ_K(SK_SCAV), LJ_K(SK_AWARE), LJ_K(SK_FORT), LJ_K(SK_AGILITY),
        LJ_K(SK_LONEWOLF), LJ_K(SK_SECOND), LJ_K(SK_JACK), LJ_K(SK_LUCKY),
        LJ_K(SK_LETHAL),
    LJ_K(SK_RIPOSTE), LJ_K(SK_GRIP), LJ_K(SK_BLADE), LJ_K(SK_HEAVY),
        LJ_K(SK_STEADY), LJ_K(SK_FOOTWORK),
};

// ---- a tiny JSON reader (objects, arrays, strings, numbers, true / false / null), in place on a buffer
typedef struct LjN {
    char t;
    double num;
    const char *s, *key;
    struct LjN *kid, *next;
} LjN;                         // t: o a s n b z(null)
static struct {
    char *p;
    LjN pool[640];
    int n, err;
} Lj;
static void LjWs(void)
{
    while (*Lj.p == ' ' || *Lj.p == '\t' || *Lj.p == '\r' || *Lj.p == '\n')
        Lj.p++;
}

static LjN *LjNew(char t)
{
    static LjN dummy;
    if (Lj.n >= (int) (sizeof Lj.pool / sizeof *Lj.pool)) {
        Lj.err = 1;
        memset(&dummy, 0, sizeof dummy);
        dummy.t = 'z';
        return &dummy;
    }
    LjN *n = &Lj.pool[Lj.n++];
    memset(n, 0, sizeof *n);
    n->t = t;
    return n;
}

static char *LjStr(void)
{
    char *w, *s;
    if (*Lj.p != '"') {
        Lj.err = 1;
        return (char *) "";
    }
    Lj.p++;
    s = w = Lj.p;
    while (*Lj.p && *Lj.p != '"') {
        if (*Lj.p == '\\' && Lj.p[1]) {
            Lj.p++;
            switch (*Lj.p) {
            case 'n':
                *w++ = '\n';
                break;
            case 't':
                *w++ = '\t';
                break;
            case 'r':
                *w++ = '\r';
                break;
            case 'u':
                *w++ = '?';
                for (int i = 0; i < 4 && Lj.p[1] && Lj.p[1] != '"'; i++)
                    Lj.p++;
                break;
            default:
                *w++ = *Lj.p;
                break;
            }
            Lj.p++;
        } else
            *w++ = *Lj.p++;
    }
    if (*Lj.p != '"') {
        Lj.err = 1;
        *w = 0;
        return s;
    }
    *w = 0;
    Lj.p++;
    return s;
}

static LjN *LjVal(int depth)
{
    LjWs();
    if (depth > 8 || Lj.err) {
        Lj.err = 1;
        return LjNew('z');
    }
    if (*Lj.p == '{' || *Lj.p == '[') {
        int obj = *Lj.p == '{';
        char close = obj ? '}' : ']';
        LjN *o = LjNew(obj ? 'o' : 'a'), *last = NULL;
        Lj.p++;
        LjWs();
        if (*Lj.p == close) {
            Lj.p++;
            return o;
        }
        for (;;) {
            const char *key = NULL;
            LjWs();
            if (obj) {
                key = LjStr();
                LjWs();
                if (*Lj.p != ':') {
                    Lj.err = 1;
                    break;
                }
                Lj.p++;
            }
            LjN *v = LjVal(depth + 1);
            v->key = key;
            if (last)
                last->next = v;
            else
                o->kid = v;
            last = v;
            LjWs();
            if (Lj.err)
                break;
            if (*Lj.p == ',') {
                Lj.p++;
                continue;
            }
            if (*Lj.p == close) {
                Lj.p++;
                break;
            }
            Lj.err = 1;
            break;
        }
        return o;
    }
    if (*Lj.p == '"') {
        LjN *n = LjNew('s');
        n->s = LjStr();
        return n;
    }
    if (!strncmp(Lj.p, "true", 4)) {
        Lj.p += 4;
        LjN *n = LjNew('b');
        n->num = 1;
        return n;
    }
    if (!strncmp(Lj.p, "false", 5)) {
        Lj.p += 5;
        return LjNew('b');
    }
    if (!strncmp(Lj.p, "null", 4)) {
        Lj.p += 4;
        return LjNew('z');
    }
    char *e;
    double d = strtod(Lj.p, &e);
    if (e == Lj.p) {
        Lj.err = 1;
        return LjNew('z');
    }
    Lj.p = e;
    LjN *n = LjNew('n');
    n->num = d;
    return n;
}

static LjN *LjParse(char *buf)
{
    Lj.p = buf;
    Lj.n = 0;
    Lj.err = 0;
    LjN *r = LjVal(0);
    return Lj.err || r->t != 'o' ? NULL : r;
}

static LjN *LjGet(const LjN *o, const char *key)
{
    if (!o || o->t != 'o')
        return NULL;
    for (LjN * k = o->kid; k; k = k->next)
        if (k->key && !strcmp(k->key, key))
            return k;
    return NULL;
}

static LjN *LjIdx(const LjN *a, int i)
{
    if (!a || a->t != 'a')
        return NULL;
    LjN *k = a->kid;
    while (k && i-- > 0)
        k = k->next;
    return k;
}

// ---- paths, files
static void LjPath(char *p, size_t n, const char *name)
{
    snprintf(p, n, "%s", g_loadoutPath);
    char *s = strrchr(p, '\\');
    if (s)
        snprintf(s + 1, n - (size_t) (s + 1 - p), "%s", name);
}

static int LjRead(const char *path, char *buf, size_t cap)
{                               // whole file (<= cap-1 bytes) -> buf, NUL-terminated; -1 on failure
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    int big = fgetc(f) != EOF;
    fclose(f);
    if (big)
        return -1;
    buf[n] = 0;
    return (int) n;
}

static long long LjNowMs(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long) (u.QuadPart / 10000ULL) - 11644473600000LL;
}

// ---- names <-> rows
static const char *LjRowId(int slot, uint32_t r)
{                               // the id string of a row of the live tables (NULL: none / out of range)
    const uint32_t *cnt = (const uint32_t *) T2R_SYM(kLargeWeaponCounts);
    if (slot < 0 || slot > 2 || r == 0xffffffffu || r >= cnt[slot])
        return NULL;
    const uint8_t *row = slot == 2 ? LoPurchTab() + r * 0x40
        : slot == 0
        && !g_largeTab ? (const uint8_t *) T2R_SYM(kLargeWeaponTab) +
        r * 0x38 : WeaponRow(slot, r);
    const char *id = *(const char **) (row + 0x10);
    if (!id)
        return NULL;
    for (const char *c = id; *c; c++)
        if (*c == '"' || *c == '\\' || (uint8_t) * c < 0x20)
            return NULL;
    return id;
}

static int LjWsel(char *b, size_t n, int slot, uint32_t r)
{
    const char *id = LjRowId(slot, r);
    if (!id)
        return snprintf(b, n, "null");
    size_t L = strlen(id);
    int supp = L > 5 && !strcmp(id + L - 5, "-supp");
    return snprintf(b, n, "{\"id\":\"%.*s\",\"supp\":%d}",
                    (int) (supp ? L - 5 : L), id, supp);
}

static int LjSsel(char *b, size_t n, uint64_t v)
{
    v = SkNorm(v);
    int id = SkId(v), lv = SkLv(v);
    if (!id || !lv || !LJ_SK[id])
        return snprintf(b, n, "null");
    return snprintf(b, n, "{\"id\":\"%s\",\"lv\":%d}", LJ_SK[id], lv);
}

// the part of the file that depends on the loadouts only (compared to skip rewrites)
// (b is a pointer here: SNLEFT(b, o) would be sizeof(char *) - o = 8 - o, i.e. every append after the first 8 bytes wrote nothing and the file ended after "custom":[ )
#define LJ_LEFT(o) ((size_t)(o) < n ? n - (size_t)(o) : (size_t)0)
static int LjBody(char *b, size_t n)
{
    LoNmLoad();
    int o =
        snprintf(b, n,
                 "\"active\":{\"kind\":\"%s\",\"i\":%d},\"custom\":[",
                 LoSlot(g_loActive) >= 0 ? "custom" : "class",
                 LoSlot(g_loActive) >=
                 0 ? LoSlot(g_loActive) : LoCls(g_loActive));
    for (int i = 0; i < g_loN && o < (int) n; i++) {   // by custom slot: the CREATE LOADOUT ones follow the first six (the array is g_lo_n long)
        const LoadoutT *l = &g_lo[i];
        uint64_t s4 = *Sk4Slot(l);
        o += snprintf(b + o, LJ_LEFT(o), "%s{\"small\":", i ? "," : "");
        o += LjWsel(b + o, LJ_LEFT(o), 1, l->w[1]);
        o += snprintf(b + o, LJ_LEFT(o), ",\"large\":");
        o += LjWsel(b + o, LJ_LEFT(o), 0, l->w[0]);
        o += snprintf(b + o, LJ_LEFT(o), ",\"purch\":");
        o += LjWsel(b + o, LJ_LEFT(o), 2, l->w[2]);
        o += snprintf(b + o, LJ_LEFT(o), ",\"skills\":[");
        for (int j = 0; j < 4 && o < (int) n; j++) {
            if (j)
                o += snprintf(b + o, LJ_LEFT(o), ",");
            o += LjSsel(b + o, LJ_LEFT(o), j < 3 ? l->k[j] : s4);
        }
        // the name (t2r-ext.c lo_nm_*): lo_nm_clean keeps no quote, backslash or control byte, so it goes in as it is ("" = the default label)
        o += snprintf(b + o, LJ_LEFT(o), "],\"lp\":%d,\"name\":\"%s\"}",
                      LoCost(l), g_loNm[i]);
    }
    o += snprintf(b + o, LJ_LEFT(o), "]");
    return o;
}

#undef LJ_LEFT

static long long g_ljApplied = -1;     // the launcher file rev applied last (persisted as "launcher_rev" in t2r-loadout.json)
static char g_ljLast[8192];
static void LjStateLoad(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    g_ljApplied = 0;
    char p[MAX_PATH];
    static char buf[16384];
    LjPath(p, sizeof p, "t2r-loadout.json");
    if (LjRead(p, buf, sizeof buf) < 0)
        return;
    // A build wrote this file cut after "custom":[ (lj_body bug) - it does not parse, but "launcher_rev" sits in its first bytes: read it raw first so
    // the in-game edits made since that launcher file are not overwritten by one more re-apply of it (the logs show "applied rev ..." on every start)
    const char *raw = strstr(buf, "\"launcher_rev\":");
    long long rawrev = raw ? strtoll(raw + 15, NULL, 10) : 0;
    LjN *r = LjParse(buf), *v = LjGet(r, "launcher_rev");
    if (v && v->t == 'n' && v->num > 0)
        g_ljApplied = (long long) v->num;
    else if (!r && rawrev > 0) {
        g_ljApplied = rawrev;
        Xlog("launcher loadouts: t2r-loadout.json did not parse (PT80 cut file) - launcher_rev %lld kept", rawrev);
    }
}

// perf (feature_loadout_persist showed 290-700 ms stalls in reports): the tmp write + MoveFileEx(WRITE_THROUGH) = a disk flush ran on the
// tick thread (antivirus / a busy disk = a frozen frame). The file is written by a one-shot worker from a heap copy; one write at a time (a change
// that arrives while one is in flight clears g_lj_last so the next tick sends it).
typedef struct {
    char p[MAX_PATH], t[MAX_PATH + 8], *txt;
    size_t len;
    int cls, idx;
} LjWrT;
static volatile LONG g_ljWrBusy;
static DWORD WINAPI LjWriteWorker(LPVOID u)
{
    LjWrT *w = (LjWrT *) u;
    FILE *f = fopen(w->t, "wb");
    int ok = 0;
    if (f) {
        size_t n = fwrite(w->txt, 1, w->len, f);
        fclose(f);
        ok = n == w->len
            && MoveFileExA(w->t, w->p,
                           MOVEFILE_REPLACE_EXISTING |
                           MOVEFILE_WRITE_THROUGH);
        if (!ok)
            DeleteFileA(w->t);
    }
    if (ok)
        Xlog("launcher loadouts: t2r-loadout.json written (%d custom loadouts, active %s %d)", g_loN, w->cls ? "class" : "custom", w->idx);
    free(w->txt);
    free(w);
    InterlockedExchange(&g_ljWrBusy, 0);
    return 0;
}

static void LjExport(int force)
{
    static char body[8192], full[8300];
    LjStateLoad();
    if (LjBody(body, sizeof body) >= (int) sizeof body)
        return;
    if (!force && !strcmp(body, g_ljLast))
        return;
    if (InterlockedCompareExchange(&g_ljWrBusy, 1, 0)) {
        g_ljLast[0] = 0;
        return;
    }                           // a write is still in flight: the next tick retries
    snprintf(g_ljLast, sizeof g_ljLast, "%s", body);
    snprintf(full, sizeof full,
             "{\"format\":1,\"rev\":%lld,\"launcher_rev\":%lld,%s}\n",
             LjNowMs(), g_ljApplied, body);
    LjWrT *w = (LjWrT *) calloc(1, sizeof *w);
    if (w) {
        w->txt = (char *) malloc(strlen(full) + 1);
    }
    if (!w || !w->txt) {
        if (w)
            free(w);
        InterlockedExchange(&g_ljWrBusy, 0);
        g_ljLast[0] = 0;
        return;
    }
    strcpy(w->txt, full);
    w->len = strlen(full);
    LjPath(w->p, sizeof w->p, "t2r-loadout.json");
    snprintf(w->t, sizeof w->t, "%s.tmp", w->p);
    w->cls = LoSlot(g_loActive) < 0;
    w->idx = (w->cls ? LoCls(g_loActive) : LoSlot(g_loActive)) + 1;
    HANDLE th = CreateThread(NULL, 0, LjWriteWorker, w, 0, NULL);
    if (th)
        CloseHandle(th);
    else {
        free(w->txt);
        free(w);
        InterlockedExchange(&g_ljWrBusy, 0);
        g_ljLast[0] = 0;
    }
}

// ---- import
static int LjPickW(int slot, const LjN *n, int allow_none, uint32_t *w,
                     int ci, const char *what)
{                               // 1 = slot kept
    if (!n) {
        Xlog("launcher loadouts: custom %d %s missing, kept", ci + 1,
             what);
        return 1;
    }
    if (n->t == 'z') {
        if (!allow_none) {
            Xlog("launcher loadouts: custom %d %s empty, kept", ci + 1,
                 what);
            return 1;
        }
        *w = 0xffffffffu;
        return 0;
    }
    const char *id = n->t == 's' ? n->s : NULL;
    int supp = 0;
    if (n->t == 'o') {
        const LjN *i = LjGet(n, "id"), *s = LjGet(n, "supp");
        id = i && i->t == 's' ? i->s : NULL;
        supp = s && ((s->t == 'n' || s->t == 'b') && s->num != 0);
    }
    char spec[96];
    if (!id || !*id || strlen(id) > 70) {
        Xlog("launcher loadouts: custom %d %s: bad id, kept", ci + 1,
             what);
        return 1;
    }
    snprintf(spec, sizeof spec, "%s%s", id, supp ? "-supp" : "");
    uint32_t r = LoClassRow(slot, spec);
    if (r == 0xffffffffu) {
        Xlog("launcher loadouts: custom %d %s: unknown id %s, kept",
             ci + 1, what, spec);
        return 1;
    }
    *w = r;
    return 0;
}

static int LjPickSk(const LjN *n, uint64_t *v, uint32_t *used, int ci,
                      int j)
{                               // 1 = slot kept (or cleared for a duplicate)
    if (!n) {
        Xlog("launcher loadouts: custom %d skill %d missing, kept", ci + 1,
             j + 1);
        return 1;
    }
    if (n->t == 'z') {
        *v = 0;
        return 0;
    }
    const LjN *i = LjGet(n, "id"), *l = LjGet(n, "lv");
    int id = 0, lv = l && l->t == 'n' ? (int) l->num : 0;
    if (i && i->t == 's')
        for (int k = 1; k < SK_COUNT; k++)
            if (LJ_SK[k] && !strcmp(LJ_SK[k], i->s))
                id = k;
    if (!id || lv < 1 || lv > SK[id].nlv) {
        Xlog("launcher loadouts: custom %d skill %d: unknown skill or level, kept", ci + 1, j + 1);
        return 1;
    }
    if (*used & (1u << id)) {
        Xlog("launcher loadouts: custom %d skill %d: %s twice, cleared",
             ci + 1, j + 1, LJ_SK[id]);
        *v = 0;
        return 1;
    }
    *used |= 1u << id;
    *v = SK_V(id, lv);
    return 0;
}

// launcher edits apply in real time, in menus AND mid-match. Only while one of our loadout pages is open the file waits (the page
// shows / edits the table itself). Mid-match the table is updated at once and the active loadout becomes a pending pick for the next spawn
// (the same path as picking on the mid-match page: the lobby object is not touched while the old guns are still held).
static int LjGateOpen(void)
{
    if (g_loState != LO_NONE)
        return 0;
    return MatchSubstate() == 6 || (!g_loPending && !g_loSnap.ok);
}

static int LjImport(void)
{
    static FILETIME seen_t;
    static DWORD seen_sz = 0xffffffffu;
    static char buf[16384];
    char p[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA a;
    LjPath(p, sizeof p, "t2r-loadout-launcher.json");
    if (FaPoll(p) == INVALID_FILE_ATTRIBUTES
        || !GetFileAttributesExA(p, GetFileExInfoStandard, &a))
        return 0;               // (no launcher file = answered from the fa_poll cache)
    if (a.ftLastWriteTime.dwLowDateTime == seen_t.dwLowDateTime
        && a.ftLastWriteTime.dwHighDateTime == seen_t.dwHighDateTime
        && a.nFileSizeLow == seen_sz)
        return 0;
    if (!LjGateOpen())
        return 0;               // look again when the menu is free
    seen_t = a.ftLastWriteTime;
    seen_sz = a.nFileSizeLow;
    LjStateLoad();
    if (LjRead(p, buf, sizeof buf) < 0) {
        Xlog("launcher loadouts: t2r-loadout-launcher.json unreadable (or over 16 KB)");
        return 0;
    }
    LjN *root = LjParse(buf), *rv = LjGet(root, "rev"), *cu =
        LjGet(root, "custom");
    if (!root || !rv || rv->t != 'n' || !cu || cu->t != 'a') {
        Xlog("launcher loadouts: t2r-loadout-launcher.json is not a loadout file, ignored");
        return 0;
    }
    long long rev = (long long) rv->num;
    if (rev <= g_ljApplied)
        return 0;
    int fixed = 0;
    const int had_n = g_loN;
    for (int i = 0; i < N_CUSTOM_MAX; i++) {
        const LjN *n = LjIdx(cu, i);
        if (i >= g_loN) {      // CREATE LOADOUT in the launcher: the next slot (an older launcher writes six; nothing is ever removed)
            if (!n || n->t != 'o')
                break;
            g_lo[i] = (LoadoutT) { {
            0xffffffffu, 0, 0xffffffffu}
            , {
            0, 0, 0}
            };
            *Sk4Slot(&g_lo[i]) = 0;
            LoNmSet(i, "", "launcher create");
            g_loN = i + 1;
            Xlog("launcher loadouts: custom %d created", i + 1);
        }
        LoadoutT old = g_lo[i], nl = old;
        uint64_t *sk4 = Sk4Slot(&g_lo[i]), old4 = *sk4, s[4] =
            { old.k[0], old.k[1], old.k[2], old4 };
        int fx = 0;
        if (!n || n->t != 'o') {
            Xlog("launcher loadouts: custom %d missing, kept", i + 1);
            fixed++;
            continue;
        }
        {                       // the name: a launcher older than names writes none (kept); "" = back to the default label
            const LjN *nm = LjGet(n, "name");
            if (nm && nm->t == 's')
                LoNmSet(i, nm->s, "launcher");
        }
        fx +=
            LjPickW(1, LjGet(n, "small"), 0, &nl.w[1], i,
                      "small firearm");
        fx +=
            LjPickW(0, LjGet(n, "large"), 1, &nl.w[0], i,
                      "large firearm");
        fx +=
            LjPickW(2, LjGet(n, "purch"), 1, &nl.w[2], i,
                      "purchasable");
        const LjN *sa = LjGet(n, "skills");
        uint32_t used = 0;
        for (int j = 0; j < 4; j++)
            fx += LjPickSk(LjIdx(sa, j), &s[j], &used, i, j);
        for (int j = 0; j < 3; j++)
            nl.k[j] = s[j];
        g_lo[i] = nl;
        *sk4 = s[3];
        if (!LoValid(&g_lo[i])) {      // over 13 LP, or a row / skill the game refuses: the whole loadout stays as it was
            Xlog("launcher loadouts: custom %d is %d LP (max 13) or invalid, kept as it was", i + 1, LoCost(&g_lo[i]));
            g_lo[i] = old;
            *sk4 = old4;
            fx++;
        }
        fixed += fx;
    }
    int in_match = MatchSubstate() == 6, was = g_loActive;
    const LjN *ac = LjGet(root, "active"), *ak =
        LjGet(ac, "kind"), *ai = LjGet(ac, "i");
    if (ak && ak->t == 's' && ai && ai->t == 'n') {
        int k = (int) ai->num, want = -1;
        if (!strcmp(ak->s, "custom") && k >= 0 && k < g_loN)
            want = LoSlotIdx(k);
        else if (!strcmp(ak->s, "class") && k >= 0 && k < N_CLASSES)
            want = N_CUSTOM + k;
        else {
            Xlog("launcher loadouts: bad active pick, kept");
            fixed++;
        }
        if (want >= 0 && in_match && want != was && GlPickRefused(want)) {
            Xlog("launcher loadouts: active pick %d refused mid-match (team gun limit), kept %d", want, was);
            fixed++;
            want = -1;
        }
        if (want >= 0)
            g_loActive = want;
    }
    g_ljApplied = rev;
    LoSave();
    Sk4Save();
    if (in_match) {
        g_loPending = g_loActive + 1; // spawnkit_hook gives the (updated) table's loadout at the next spawn
        MsgHook("Loadout updated - applies on your next life", 2.5f, 0,
                 0);
    } else
        LoWriteObj(LoOf(g_loActive));
    LjExport(1);
    Xlog("launcher loadouts: applied rev %lld (%d custom loadouts, %d created, %d slots fixed)%s", rev, g_loN, g_loN - had_n, fixed, in_match ? " mid-match - pending to the next spawn" : "");
    return 1;
}

// the player edited a loadout on our GEAR UP pages: a launcher file not applied yet is older than that edit, so it must not overwrite it
// (its rev counts as applied; t2r-loadout.json then reports launcher_rev = that rev and the launcher adopts the game's loadouts)
static void LjUserEdit(void)
{
    static char buf[16384];
    char p[MAX_PATH];
    LjStateLoad();
    LjPath(p, sizeof p, "t2r-loadout-launcher.json");
    if (LjRead(p, buf, sizeof buf) < 0)
        return;
    LjN *rv = LjGet(LjParse(buf), "rev");
    if (rv && rv->t == 'n' && (long long) rv->num > g_ljApplied) {
        g_ljApplied = (long long) rv->num;
        Xlog("launcher loadouts: in-game edit after launcher rev %lld - the launcher file is dropped", g_ljApplied);
    }
}

// from feature_loadout_persist (twice a second, in menus and matches): 1 = a launcher file was applied (the lobby object was rewritten)
static int LjTick(int first)
{
    if (!g_loadoutPath[0])
        return 0;
    LjStateLoad();
    int applied = LjImport();
    if (!applied)
        LjExport(first);
    return applied;
}
