// friends13.c - the in-game FRIENDS / PARTY panel (the in-game ui has a friends button, P for party, next to the lobby
// widget on the right; having it only in the launcher is not enough). Included after clan13.c (it reuses gearup13.c's fit / fade / repeat helpers). Log prefix "friendsui:".
//
// WHERE: a button strip "P  FRIENDS" (key tile P, a red badge with the number of friend requests + party invites waiting, and "n ONLINE" / "PARTY 2/4") sits directly under the LOBBY
// box on the right of the MP menu (to1/main-menu) and of FIND MATCH (to1/find-match-menu): x 2500..3682, y 1560..1680 of the 3840 x 2160 canvas (tools/features/friends13_widgets.py).
// P (keyboard), the gamepad's menu-next (RB) or a click on the strip opens the page to1/friends13 (only on those two pages, never once the pre-match sequence / a match runs).
//
// PAGE: left PARTY (members with LEADER / READY / presence, then READY and LEAVE / DISBAND), right FRIENDS (ADD FRIEND typed in place, party invites, friend requests, friends online first,
// requests you sent). Keys: W / S / arrows / stick move, A / D / Tab / Q / E / LB / RB switch the list, Enter / SELECT does the row's main action (INVITE, ACCEPT, JOIN, MAKE LEADER,
// READY, LEAVE, type a name), X / Delete / the pad's menu-action the second one (REMOVE, DECLINE, KICK, CANCEL), R = ready, L = leave the party, P / Esc / BACK close.
// ADD FRIEND: Enter on the first row opens a text field over it (animated underline, blinking caret, placeholder): letters, digits, space - _ . : [ ], Ctrl+V pastes, Left / Right / Home / End
// move the caret, Backspace / Delete erase (the engine's pop of the page on Backspace is undone, see feature_friends13), Enter sends, Esc cancels typing, a second Esc closes (rework:
// text fits its boxes, rows come in staggered, the page fades out on close, requests / invites pop up as a bottom-right card with Y / N, the toast block below).
//
// DATA: the daemon's friends service (tools/social.py), op 0xD8 [token][ver 1][lang][sub][arg] -> 0xD9 JSON, asked by this file's own short connection (like notice.c), parsed by friends13.h.
// The state is read every 3 s while the page is up, every 15 s on the menus otherwise (never in a match); an action goes first and its reply is the fresh state. A newly arrived
// friend request / party invite that no server notice announced pops as a toast (ntf_push) on the menus; notice.c keeps the server lines of a running match until the match is over.
// t2r-no-ingame-friends.txt next to the exe: off (no button, no page, no poll).
#include "friends13.h"
static void NtfSound(void);    // notify.c: the toast chime
#define FR_NL 6
#define FR_RROWS 7
#define FR_NPM 4
static int g_frOff = -1;
static Fr13T g_f13;            // the last state (main thread only)
static uint64_t FrPageId(void)
{
    static uint64_t id;
    if (!id)
        id = PageId("friends13");
    return id;
}

static int Fr13Enabled(void)
{
    if (g_frOff < 0) {
        FILE *f = ExeSideOpen("t2r-no-ingame-friends.txt", "r");
        g_frOff = f != NULL;
        if (f)
            fclose(f);
        Xlog("friendsui: in-game FRIENDS panel %s",
             g_frOff ? "off (t2r-no-ingame-friends.txt)" : "on");
    }
    return !g_frOff;
}

#define FRT(en, pl) (NtPl() ? (pl) : (en))
// ------------------------------------------------------------------ the network (own short connection per request, one at a time)
static volatile LONG g_fr_busy, g_fr_have;      // have: 1 = a reply is in g_fr_rep, 2 = connected but no valid answer, 3 = no connection
static char g_frRep[24000];
static volatile LONG g_frReplen;
static struct {
    uint8_t pl[64 + 3 + 104];
    int n;
} g_frJob;
static DWORD WINAPI FrWorker(LPVOID arg)
{
    (void) arg;
    HMODULE ws = LoadLibraryA("ws2_32.dll");
    T2rSin a;
    T2rNet net;
    LONG res = 3;
    if (ws && HmServer(&a) && T2rNetLoad(&net, ws)) {
        static uint8_t frame[3 + 64 + 3 + 104];
        static char rep[sizeof g_frRep + 16];
        int n = g_frJob.n + 1;
        frame[0] = (uint8_t) n;
        frame[1] = (uint8_t) (n >> 8);
        frame[2] = 0xD8;
        memcpy(frame + 3, g_frJob.pl, (size_t) g_frJob.n);
        int sent = 0;
        int got =
            T2rRpc(&net, &a, frame, n + 2, rep, (int) sizeof rep - 1,
                    2500, 3500, 3500, &sent);
        if (sent)
            res = 2;
        int ln = got >= 3 ? (rep[0] & 0xff) + ((rep[1] & 0xff) << 8) : 0;
        if (ln >= 2 && got >= ln + 2 && (uint8_t) rep[2] == 0xD9
            && ln - 1 < (int) sizeof g_frRep) {
            memcpy(g_frRep, rep + 3, (size_t) ln - 1);
            InterlockedExchange(&g_frReplen, ln - 1);
            res = 1;
            SrvMarkAlive();
        }
    }
    InterlockedExchange(&g_fr_have, res);
    InterlockedExchange(&g_fr_busy, 0);
    return 0;
}

static int FrStart(int sub, const char *arg)
{                               // 1 = a request went out
    static char tok[65];
    if (g_fr_busy)
        return 0;
    if (!tok[0] && !ClanToken(tok)) {
        tok[0] = 0;
        return 0;
    }
    memcpy(g_frJob.pl, tok, 64);
    g_frJob.pl[64] = 1;
    g_frJob.pl[65] = (uint8_t) NtPl();
    g_frJob.pl[66] = (uint8_t) sub;
    size_t al = arg ? strlen(arg) : 0;
    if (al > 100)
        al = 100;
    if (al)
        memcpy(g_frJob.pl + 67, arg, al);
    g_frJob.n = 67 + (int) al;
    InterlockedExchange(&g_fr_busy, 1);
    HANDLE h = CreateThread(NULL, 0, FrWorker, NULL, 0, NULL);
    if (!h) {
        InterlockedExchange(&g_fr_busy, 0);
        return 0;
    }
    CloseHandle(h);
    return 1;
}

// ------------------------------------------------------------------ state
enum { A_NONE, A_TYPE, A_INVITE, A_ACCEPT, A_JOIN, A_DECLINE_P,
        A_DECLINE_R, A_REMOVE, A_CANCEL, A_PROMOTE, A_KICK, A_READY,
        A_LEAVE, A_ADD };
static struct {
    int have, fails, ok_once, dead, ver;
    uint64_t next_poll, ok_at;
    int act_pending, act_sub, act_inflight;     // a queued write (sub / arg), the write whose reply is awaited (sub; 0 none)
    char act_arg[64];
    int act_code;               // the A_* of the write in flight (for the message)
    int nf_before;
    uint64_t tseen[32];
    int ntseen;                 // friend request / party invite ids a toast was already made for (hashes)
    char msg[200];
    uint64_t msg_until;
    int last_badge, last_nf, last_nm, last_ni, last_npi;
} N13;
static struct {
    int up, closing, dirty, col, foc[2], off[2], typing, blen, stage;
    char buf[40];
    uint64_t enter_ms, back_f, sfx_ms, t_u, t_d, bs_t0;
    int h_sel, h_u, h_d, h_l, h_r, h_back, h_esc, h_pv, h_nx, h_act,
        hk[256], h_click;
    int hv_col, hv_row, hv_back, hv_sel, hv_act;
    uint64_t click_f;           // frame of the last click (the engine's menu-select that rides on a mouse click is dropped, like gear up)
    int caret;                  // typing: byte offset of the caret in buf (always on a character boundary)
    uint64_t typ_ms, edit_ms, bs_ms, close_ms, rep_t[4];        // typing started / last edit (caret stays solid) / a Backspace was last seen down / the close fade began / key-repeat timers (left, right, backspace, delete)
    float inp_a, ph_a;          // the text field's and the placeholder's smoothed alpha
    uint64_t rw[2][8];          // row group handles (re-resolved with every fill)
    float rfx[2][8];            // focus nudge of a row (0..1)
    int rq[2][8], rqx[2][8];    // last written alpha / x step of a row (-1 = nothing written yet)
    int ul_q, gl_q, ia_q, pa_q; // last written steps of the field's underline / glow / group alpha / placeholder alpha
} F13;
static struct {
    int valid;
    int typing, blen, caret, col, foc[2], off[2];
    char buf[40];
    uint64_t typ_ms;
} K13;                          // typing state carried over the engine's pop on Backspace (the page is pushed again)
static const char *FrErr(const char *c)
{
    if (!strcmp(c, "self"))
        return FRT("THAT IS YOU", "TO TY");
    if (!strcmp(c, "unknown"))
        return
            FRT
            ("NO PLAYER WITH THAT NAME OR STEAM ID (THEY MUST HAVE PLAYED ONCE)",
             "NIE ZNALEZIONO GRACZA O TEJ NAZWIE LUB ID STEAM");
    if (!strcmp(c, "ambiguous"))
        return FRT("MORE THAN ONE PLAYER HAS THAT NAME - USE THE STEAM ID",
                   "KILKU GRACZY MA TĄ NAZWĘ - UŻYJ ID STEAM");
    if (!strcmp(c, "already"))
        return FRT("YOU ARE ALREADY FRIENDS", "JESTEŚCIE JUŻ ZNAJOMYMI");
    if (!strcmp(c, "pending"))
        return FRT("YOU ALREADY SENT THIS PLAYER A REQUEST",
                   "ZAPROSZENIE JUŻ WYSŁANE");
    if (!strcmp(c, "limit"))
        return FRT("THE LIMIT OF FRIENDS OR OPEN REQUESTS IS REACHED",
                   "OSIĄGNIĘTO LIMIT ZNAJOMYCH LUB ZAPROSZEŃ");
    if (!strcmp(c, "expired"))
        return FRT("THAT REQUEST OR INVITE IS NO LONGER VALID",
                   "TO ZAPROSZENIE JEST JUŻ NIEAKTUALNE");
    if (!strcmp(c, "rate"))
        return FRT("TOO MANY ACTIONS - WAIT A MOMENT",
                   "ZA DUŻO AKCJI - POCZEKAJ CHWILĘ");
    if (!strcmp(c, "notfriend"))
        return FRT("YOU CAN ONLY INVITE FRIENDS",
                   "ZAPRASZAĆ MOŻNA TYLKO ZNAJOMYCH");
    if (!strcmp(c, "offline"))
        return FRT("THAT FRIEND IS OFFLINE", "TEN ZNAJOMY JEST OFFLINE");
    if (!strcmp(c, "notleader"))
        return FRT("ONLY THE PARTY LEADER CAN DO THAT",
                   "TYLKO LIDER GRUPY MOŻE TO ZROBIĆ");
    if (!strcmp(c, "full"))
        return FRT("THE PARTY IS FULL", "GRUPA JEST PEŁNA");
    if (!strcmp(c, "notmember"))
        return FRT("THAT PLAYER IS NOT IN YOUR PARTY",
                   "TEN GRACZ NIE JEST W TWOJEJ GRUPIE");
    if (!strcmp(c, "noparty"))
        return FRT("YOU ARE NOT IN A PARTY", "NIE MASZ GRUPY");
    if (!strcmp(c, "match"))
        return FRT("NOT POSSIBLE DURING A MATCH",
                   "NIEMOŻLIWE W TRAKCIE MECZU");
    return FRT("SOMETHING WENT WRONG - TRY AGAIN",
               "COŚ POSZŁO NIE TAK - SPRÓBUJ PONOWNIE");
}

static void FrMsg(const char *text, int good)
{
    snprintf(N13.msg, sizeof N13.msg, "<font color=\"#%s\">%s</font>",
             good ? "7CB494" : "F2625A", text);
    N13.msg_until = GetTickCount64() + 6000;
    F13.dirty = 1;
}

static const char *FrOkText(int code, int became_friends)
{
    switch (code) {
    case A_ADD:
        return became_friends ? FRT("FRIEND ADDED",
                                    "ZNAJOMY DODANY") :
            FRT("FRIEND REQUEST SENT", "ZAPROSZENIE WYSŁANE");
    case A_ACCEPT:
        return FRT("REQUEST ACCEPTED", "ZAPROSZENIE PRZYJĘTE");
    case A_INVITE:
        return FRT("PARTY INVITE SENT", "ZAPROSZENIE DO GRUPY WYSŁANE");
    case A_JOIN:
        return FRT("YOU JOINED THE PARTY", "DOŁĄCZASZ DO GRUPY");
    case A_LEAVE:
        return FRT("YOU LEFT THE PARTY", "OPUSZCZASZ GRUPĘ");
    case A_PROMOTE:
        return FRT("NEW PARTY LEADER", "NOWY LIDER GRUPY");
    case A_KICK:
        return FRT("REMOVED FROM THE PARTY", "USUNIĘTO Z GRUPY");
    case A_REMOVE:
        return FRT("FRIEND REMOVED", "ZNAJOMY USUNIĘTY");
    case A_DECLINE_R:
    case A_DECLINE_P:
    case A_CANCEL:
        return FRT("DONE", "GOTOWE");
    default:
        return "";
    }
}

static void FtPush(int kind, const char *id, const char *name,
                    const char *text, float secs, uint64_t delay_ms);
static int FrToastEnabled(void);
enum { FT_REQ, FT_PINV, FT_GOOD, FT_BAD, FT_INFO };
// ------------------------------------------------------------------ toasts for what the server notices did not announce (and the dedupe against them)
static struct {
    char t[300];
    uint64_t at;
} g_fr_pl[8];
static int g_frPlN;
static void Fr13NoteSeen(const char *text)
{                               // notice.c: a friends / party line was shown or queued
    snprintf(g_fr_pl[g_frPlN & 7].t, sizeof g_fr_pl[0].t, "%s",
             text ? text : "");
    g_fr_pl[g_frPlN & 7].at = GetTickCount64();
    g_frPlN++;
}

static int FrNoted(const char *name)
{
    uint64_t now = GetTickCount64();
    for (int i = 0; i < 8; i++)
        if (g_fr_pl[i].t[0] && now - g_fr_pl[i].at < 600000 && name[0]
            && strstr(g_fr_pl[i].t, name))
            return 1;
    return 0;
}

static uint64_t fr_h(const char *kind, const char *id)
{
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = kind; *p; p++)
        h = (h ^ (uint8_t) * p) * 1099511628211ULL;
    for (const char *p = id; *p; p++)
        h = (h ^ (uint8_t) * p) * 1099511628211ULL;
    return h;
}

static int FrTseen(uint64_t h)
{
    for (int i = 0; i < 32 && i < N13.ntseen; i++)
        if (N13.tseen[i] == h)
            return 1;
    return 0;
}

static void FrToasts(const Fr13T *d, int panel_up)
{
    int shown = 0;
    for (int k = 0; k < 2; k++) {
        int n = k ? d->npi : d->ni;
        for (int i = 0; i < n; i++) {
            const FrRT *r = k ? &d->pinv[i] : &d->in[i];
            uint64_t h = fr_h(k ? "i" : "r", r->id);
            if (FrTseen(h))
                continue;
            N13.tseen[N13.ntseen++ & 31] = h;
            if (panel_up)
                continue;
            if (FrToastEnabled()) {   // the card: Y accepts / joins, N declines (an old backlog shows 3 at most, the badge counts the rest)
                if (shown >= 3)
                    continue;
                FtPush(k ? FT_PINV : FT_REQ, r->id, r->name, "",
                        k ? 16.0f : 12.0f, 0);
                shown++;
                continue;
            }
            if (shown >= 2 || FrNoted(r->name))
                continue;
            char m[200];
            if (k)
                snprintf(m, sizeof m,
                         FRT
                         ("PARTY: %s invited you to a party. Press P to open FRIENDS and join.",
                          "GRUPA: %s zaprasza cię do grupy. Wciśnij P, aby otworzyć ZNAJOMYCH i dołączyć."),
                         r->name);
            else
                snprintf(m, sizeof m,
                         FRT
                         ("%s sent you a friend request. Press P to open FRIENDS.",
                          "%s wysyła ci zaproszenie do znajomych. Wciśnij P, aby otworzyć ZNAJOMYCH."),
                         r->name);
            if (NtfPush(NTF_SERVER, m, 9.0f)) {
                shown++;
                Xlog("friendsui: toast \"%s\"", m);
            }
        }
    }
}

static void FrNetTick(uint64_t now, int panel_up, int menu_ok)
{
    LONG have = InterlockedExchange(&g_fr_have, 0);
    if (have == 1) {
        static Fr13T tmp;
        if (Fr13Parse(&tmp, g_frRep, (size_t) g_frReplen)) {
            if (N13.act_inflight) {
                if (!tmp.ok) {
                    FrMsg(FrErr(tmp.err), 0);
                    UiSfx("sfx-menu-error");
                    Xlog("friendsui: action %d refused: %s",
                         N13.act_inflight, tmp.err);
                    if (!panel_up && FrToastEnabled())
                        FtPush(FT_BAD, "", "", FrErr(tmp.err), 5.5f, 0);
                } else {
                    const char *ok_t =
                        FrOkText(N13.act_code, tmp.nf > N13.nf_before);
                    FrMsg(ok_t, 1);
                    Xlog("friendsui: action %d ok", N13.act_inflight);
                    if (!panel_up && FrToastEnabled() && ok_t[0])
                        FtPush(FT_GOOD, "", "", ok_t, 4.5f, 0);
                }
                N13.act_inflight = 0;
            }
            g_f13 = tmp;
            N13.have = 1;
            N13.ver++;
            N13.ok_at = now;
            N13.fails = 0;
            N13.ok_once = 1;
            if (g_f13.nf != N13.last_nf || g_f13.nm != N13.last_nm
                || g_f13.ni != N13.last_ni || g_f13.npi != N13.last_npi) {
                N13.last_nf = g_f13.nf;
                N13.last_nm = g_f13.nm;
                N13.last_ni = g_f13.ni;
                N13.last_npi = g_f13.npi;
                Xlog("friendsui: state: %d friends, %d requests in, %d out, %d party invites, party of %d%s", g_f13.nf, g_f13.ni, g_f13.no, g_f13.npi, g_f13.nm, g_f13.me_leader ? " (leader)" : "");
            }
            FrToasts(&g_f13, panel_up);
            F13.dirty = 1;
        } else {
            Xlog("friendsui: the reply did not parse (%ld bytes)",
                 (long) g_frReplen);
            N13.act_inflight = 0;
            N13.next_poll = now + 10000;
        }
    } else if (have == 2 || have == 3) {
        N13.fails++;
        if (N13.act_inflight) {
            FrMsg(FRT
                   ("NO ANSWER FROM THE SERVER", "SERWER NIE ODPOWIADA"),
                   0);
            if (!panel_up && FrToastEnabled())
                FtPush(FT_BAD, "", "",
                        FRT("NO ANSWER FROM THE SERVER",
                            "SERWER NIE ODPOWIADA"), 5.5f, 0);
            N13.act_inflight = 0;
        }
        N13.next_poll = now + (N13.fails < 3 ? 6000 : 20000);
        if (have == 2 && SrvAliveWithin(60000) && N13.fails >= 3
            && !N13.ok_once) {
            N13.dead = 1;
            Xlog("friendsui: the server does not answer op 0xD8 (older server) - not asking again");
        }
        F13.dirty = 1;
    }
    if (N13.dead || g_fr_busy)
        return;
    if (N13.act_pending) {
        if (FrStart(N13.act_sub, N13.act_arg)) {
            N13.act_pending = 0;
            N13.act_inflight = N13.act_sub;
        }
        return;
    }
    if (!menu_ok || now < N13.next_poll)
        return;
    if (FrStart(1, ""))
        N13.next_poll = now + (panel_up ? 3000 : g_f13.has_party ? 2500 : 15000);       // (in a party the ready tabs / the leader's search follow within a few seconds)
}

static void FrActQueue(int code, int sub, const char *arg)
{
    N13.act_sub = sub;
    snprintf(N13.act_arg, sizeof N13.act_arg, "%s", arg ? arg : "");
    N13.act_code = code;
    N13.act_pending = 1;
    N13.nf_before = g_f13.nf;
    UiSfx("sfx-select");
    Xlog("friendsui: action %d (sub %d %s)", code, sub,
         sub == 3 ? "<typed>" : N13.act_arg);
}

// ------------------------------------------------------------------ the toast (notifications in the bottom right corner, lifted, for friend invites and party invites)
// ONE card (widgets frToast, friends13_widgets.add_toast: x 2480..3720, y 1760..1940 of the 3840 x 2160 canvas = bottom right, inset 120 from the right edge, above the footer, below the
// FRIENDS strip) on the MP menu and FIND MATCH while the menu is really showing (the same moments as the strip). Friend requests and party invites carry the sender's name and the keys
// Y (accept / join) and N (decline) - or a click on the chip; everything else the server says about friends / party (notice lines kind p) is a plain card. More than one waiting: one after the other,
// the card says "+n". Timing: slide in from the right (170 units) + fade over 260 ms (ease-out cubic); hold 12 s (request) / 16 s (party invite) / 4.5 s (result) / the server's seconds (info),
// a progress line under the card runs out meanwhile and stands still while the cursor is on the card; slide out 70 units + fade over 220 ms (ease-in). The card never shows over the FRIENDS page
// (its items are on the list), leaves the moment the menu stops showing (the rest of its time waits) and is never drawn in a match (lines wait for the match end, notice.c).
// t2r-no-friend-toast.txt next to the exe: the old one-line toasts (ntf_push) instead.
#define FT_N 6
#define FT_X 2480.0f
#define FT_Y 1760.0f
#define FT_W 1240.0f
#define FT_H 180.0f
#define FT_IN_MS 260.0f
#define FT_OUT_MS 220.0f
// FT_REQ, FT_PINV, FT_GOOD, FT_BAD, FT_INFO (enum above) = the order of the accent bars tAc0..4
typedef struct {
    int kind;
    char id[40], name[36], text[160];
    float secs;
    uint64_t ready_at;
} FtItemT;
static FtItemT g_ftq[FT_N];
static int g_ftqn, g_ft_off = -1, g_ft_missing;
static struct {
    int st;
    FtItemT it;
    uint64_t t0, t_out, last;
    float left_ms, total_ms;
    int hov, hy, hn, hky, hkn, qa, qx, qb;
    uint64_t gw;
    uint64_t cool;
    int hide;
} T13;                          // st: 0 idle, 1 in, 2 hold, 3 out
static int FrToastEnabled(void)
{
    if (g_ft_off < 0) {
        FILE *f = ExeSideOpen("t2r-no-friend-toast.txt", "r");
        g_ft_off = f != NULL;
        if (f)
            fclose(f);
        Xlog("friendsui: friend / party toast card %s",
             g_ft_off ? "off (t2r-no-friend-toast.txt)" : "on");
    }
    return !g_ft_off && !g_ft_missing;
}

static float FtEase(float k)
{
    if (k < 0.0f)
        k = 0.0f;
    if (k > 1.0f)
        k = 1.0f;
    return 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
}

static const char *FtP(const char *leaf)
{
    static char b[4][64];
    static int n;
    char *o = b[n++ & 3];
    snprintf(o, 64, "anim-group.frToast.%s", leaf);
    return o;
}

static int FrMenuVis(void)
{
    return !g_veilOn && !g_pm.pending && g_loState == LO_NONE
        && MsOnMenu() && g_mfCodeNow < 100 && !g_mhLoad && !g_mhSt
        && !g_lsOwn;
}

static int FtAct(int kind)
{
    return kind == FT_REQ || kind == FT_PINV;
}

static int FtPending(const FtItemT *it)
{                               // an actionable card is only worth showing while the request / invite still waits on the server
    if (!FtAct(it->kind) || !N13.have)
        return 1;
    if (it->kind == FT_REQ) {
        for (int i = 0; i < g_f13.ni; i++)
            if (!strcmp(g_f13.in[i].id, it->id))
                return 1;
    } else
        for (int i = 0; i < g_f13.npi; i++)
            if (!strcmp(g_f13.pinv[i].id, it->id))
                return 1;
    return 0;
}

static void FtPush(int kind, const char *id, const char *name,
                    const char *text, float secs, uint64_t delay_ms)
{
    for (int i = 0; i < g_ftqn; i++) {  // the same card is not queued twice
        if (g_ftq[i].kind == kind
            && (id[0] ? !strcmp(g_ftq[i].id, id) :
                !strcmp(g_ftq[i].text, text)))
            return;
    }
    if (T13.st && T13.it.kind == kind
        && (id[0] ? !strcmp(T13.it.id, id) : !strcmp(T13.it.text, text)))
        return;
    if (g_ftqn >= FT_N) {       // full: the oldest plain card gives way, else the oldest
        int k = 0;
        for (int i = 0; i < g_ftqn; i++)
            if (!FtAct(g_ftq[i].kind)) {
                k = i;
                break;
            }
        memmove(g_ftq + k, g_ftq + k + 1,
                sizeof g_ftq[0] * (size_t) (g_ftqn - k - 1));
        g_ftqn--;
    }
    FtItemT *q = &g_ftq[g_ftqn++];
    memset(q, 0, sizeof *q);
    q->kind = kind;
    q->secs = secs;
    q->ready_at = GetTickCount64() + delay_ms;
    snprintf(q->id, sizeof q->id, "%s", id);
    snprintf(q->name, sizeof q->name, "%s", name);
    snprintf(q->text, sizeof q->text, "%s", text);
    Xlog("friendsui: toast queued kind %d \"%s\" \"%.80s\" %.1f s", kind,
         name, text, secs);
}

// notice.c: a friends / party line of the server (kind p). 1 = the card takes it (it then waits for the state poll, so a request line is dropped when its own card shows), 0 = the old toast
static int Fr13ToastLine(const char *text, float secs)
{
    if (!Fr13Enabled() || !FrToastEnabled() || !text || !text[0])
        return 0;
    char t[160];
    size_t n = strlen(text);
    if (n >= sizeof t) {
        n = 96;
        while (n > 0 && ((unsigned char) text[n] & 0xC0) == 0x80)
            n--;
        memcpy(t, text, n);
        memcpy(t + n, "...", 4);
    } else
        memcpy(t, text, n + 1);
    FtPush(FT_INFO, "", "", t, secs >= 2.0f
            && secs < 60.0f ? secs : 6.0f, 1800);
    N13.next_poll = 0;          // ask for the state now: its request / invite card (with the Y / N keys) replaces the line
    return 1;
}

static int FtNamesPending(const char *text)
{
    for (int i = 0; i < g_f13.ni; i++)
        if (g_f13.in[i].name[0] && strstr(text, g_f13.in[i].name))
            return 1;
    for (int i = 0; i < g_f13.npi; i++)
        if (g_f13.pinv[i].name[0] && strstr(text, g_f13.pinv[i].name))
            return 1;
    return 0;
}

static void FtFlushOld(void)
{                               // no toast widgets in this pak: the queued cards go the old way
    for (int i = 0; i < g_ftqn; i++) {
        char m[200];
        const FtItemT *q = &g_ftq[i];
        if (q->kind == FT_REQ)
            snprintf(m, sizeof m,
                     FRT
                     ("%s sent you a friend request. Press P to open FRIENDS.",
                      "%s wysyła ci zaproszenie do znajomych. Wciśnij P, aby otworzyć ZNAJOMYCH."),
                     q->name);
        else if (q->kind == FT_PINV)
            snprintf(m, sizeof m,
                     FRT
                     ("PARTY: %s invited you to a party. Press P to open FRIENDS and join.",
                      "GRUPA: %s zaprasza cię do grupy. Wciśnij P, aby otworzyć ZNAJOMYCH i dołączyć."),
                     q->name);
        else
            snprintf(m, sizeof m, "%s", q->text);
        NtfPush(NTF_SERVER, m, q->secs);
    }
    g_ftqn = 0;
}

static const char *FtIcon(int kind)
{
    return kind ==
        FT_REQ ? "streaming/t2r/icons/notify-revive.png" : kind ==
        FT_BAD ? "streaming/t2r/icons/notify-warn.png" :
        "streaming/t2r/icons/notify-info.png";
}

static void FtFill(void)
{                               // the card's texts / bars for T13.it (the widgets are hidden or at alpha 0 meanwhile)
    const FtItemT *it = &T13.it;
    int act = FtAct(it->kind);
    for (int k = 0; k < 5; k++) {
        char l[8];
        snprintf(l, sizeof l, "tAc%d", k);
        UiVisible(FtP(l), k == it->kind);
    }
    UiImage(FtP("tIc"), FtIcon(it->kind));
    UiText(FtP("tKick"),
            it->kind == FT_REQ ? FRT("FRIEND REQUEST",
                                     "ZAPROSZENIE DO ZNAJOMYCH") : it->
            kind == FT_PINV ? FRT("PARTY INVITE",
                                  "ZAPROSZENIE DO GRUPY") : FRT("FRIENDS",
                                                                "ZNAJOMI"));
    UiVisible(FtP("tName"), act);
    UiVisible(FtP("tSub"), act);
    UiVisible(FtP("tInfo"), !act);
    if (act) {
        GuFit(FtP("tName"), it->name, 640.0f, 54.0f);
        GuFit(FtP("tSub"),
               it->kind == FT_REQ ? FRT("WANTS TO BE YOUR FRIEND",
                                        "CHCE ZOSTAĆ TWOIM ZNAJOMYM") :
               FRT("INVITES YOU TO A PARTY", "ZAPRASZA CIĘ DO GRUPY"),
               650.0f, 33.0f);
        UiText(FtP("tInfo"), "");
    } else {
        UiText(FtP("tName"), "");
        UiText(FtP("tSub"), "");
        UiText(FtP("tInfo"), it->text);
    }
    static const char *const CH[2] = { "Y", "N" };
    for (int c = 0; c < 2; c++) {
        char l[12];
        snprintf(l, sizeof l, "tKe%s", CH[c]);
        UiVisible(FtP(l), act);
        snprintf(l, sizeof l, "tKt%s", CH[c]);
        UiVisible(FtP(l), act);
        snprintf(l, sizeof l, "tLb%s", CH[c]);
        UiVisible(FtP(l), act);
        const char *lab =
            !act ? "" : c == 0 ? (it->kind ==
                                  FT_REQ ? FRT("ACCEPT",
                                               "PRZYJMIJ") : FRT("JOIN",
                                                                 "DOŁĄCZ"))
            : FRT("DECLINE", "ODRZUĆ");
        GuFit(FtP(l), lab, 232.0f, 38.0f);
    }
    char m[16];
    snprintf(m, sizeof m, "+%d", g_ftqn);
    UiText(FtP("tMore"), g_ftqn > 0 ? m : "");
    WScale(UiW(FtP("tBar")), 1.0f, 1.0f);
    T13.qa = T13.qx = T13.qb = -1;
}

static int FtCursor(float *rx0, float *ry0)
{                               // the cursor relative to the card's resting place (canvas units); 0 = no cursor in the window
    float cx, cy;
    if (!UiCursorCanvas(&cx, &cy))
        return 0;
    *rx0 = cx - FT_X;
    *ry0 = cy - FT_Y;
    return 1;
}

static int FrToastHit(void)
{                               // the cursor is on a showing actionable card (a click there is not for the menu button under it)
    float x, y;
    return (T13.st == 1 || T13.st == 2) && FtAct(T13.it.kind)
        && FtCursor(&x, &y) && x >= 0 && x <= FT_W && y >= 0 && y <= FT_H;
}

static void FtEnd(uint64_t now)
{                               // the card is over: hidden now when this page still has it, else (another page on top) the next menu moment hides it
    uint64_t g = UiW("anim-group.frToast");
    if (g) {
        WAlpha(g, 0.0f);
        UiVisible("anim-group.frToast", 0);
        T13.hide = 0;
    } else
        T13.hide = 1;
    T13.st = 0;
    T13.cool = now + 350;
}

static void FtTick(uint64_t now, int panel, int on_page)
{
    if (!Fr13Enabled())
        return;
    if (!FrToastEnabled()) {
        if (g_ftqn)
            FtFlushOld();
        return;
    }
    if (panel) {                // the FRIENDS page shows them all: no card on top of it, the actionable ones are done
        int k = 0;
        for (int i = 0; i < g_ftqn; i++)
            if (!FtAct(g_ftq[i].kind))
                g_ftq[k++] = g_ftq[i];
        g_ftqn = k;
        if (T13.st && FtAct(T13.it.kind))
            FtEnd(now);
        T13.last = 0;
    }
    int ok = !panel && on_page && FrMenuVis();
    float dt = T13.last ? (float) (now - T13.last) : 16.0f;
    T13.last = now;
    if (dt > 100.0f)
        dt = 100.0f;
    if (T13.st && !ok) {        // the menu stopped showing: the card goes at once, what is left of its time waits for the next menu moment
        FtItemT it = T13.it;
        float left = T13.left_ms;
        FtEnd(now);
        if (left > 1500.0f && FtPending(&it) && g_ftqn < FT_N) {
            memmove(g_ftq + 1, g_ftq, sizeof g_ftq[0] * (size_t) g_ftqn);
            g_ftq[0] = it;
            g_ftq[0].secs = left / 1000.0f;
            g_ftq[0].ready_at = 0;
            g_ftqn++;
        }
        return;
    }
    if (!T13.st) {
        if (!ok || now < T13.cool)
            return;
        if (T13.hide)
            FtEnd(now);
        int pick = -1;
        for (int i = 0; i < g_ftqn && pick < 0; i++) {
            FtItemT *q = &g_ftq[i];
            if (now < q->ready_at)
                continue;
            if (!FtPending(q) || (q->kind == FT_INFO && N13.have && FtNamesPending(q->text))) {      // gone from the server / announced by its own card
                Xlog("friendsui: toast dropped (%s) \"%s\"",
                     FtAct(q->
                            kind) ? "no longer pending" :
                     "its request card shows",
                     FtAct(q->kind) ? q->name : q->text);
                memmove(g_ftq + i, g_ftq + i + 1,
                        sizeof g_ftq[0] * (size_t) (g_ftqn - i - 1));
                g_ftqn--;
                i--;
                continue;
            }
            pick = i;
        }
        if (pick < 0)
            return;
        static int miss;
        uint64_t gw = UiW("anim-group.frToast");
        if (!gw) {
            if (++miss > 240) {
                g_ft_missing = 1;
                Xlog("friendsui: no frToast widgets on this page (older gui2-widgets.pak) - the old one-line toasts stay");
                FtFlushOld();
            }
            return;
        }
        miss = 0;
        T13.it = g_ftq[pick];
        memmove(g_ftq + pick, g_ftq + pick + 1,
                sizeof g_ftq[0] * (size_t) (g_ftqn - pick - 1));
        g_ftqn--;
        T13.gw = gw;
        T13.st = 1;
        T13.t0 = now;
        T13.total_ms = T13.left_ms = T13.it.secs * 1000.0f;
        T13.hky = (GetAsyncKeyState('Y') & 0x8000) != 0;
        T13.hkn = (GetAsyncKeyState('N') & 0x8000) != 0;        // held on arrival = no press
        FtFill();
        WAlpha(gw, 0.0f);
        UiVisible("anim-group.frToast", 1);
        NtfSound();
        Xlog("friendsui: toast shown kind %d \"%s\" %.1f s", T13.it.kind,
             T13.it.name[0] ? T13.it.name : T13.it.text,
             (double) T13.it.secs);
        return;
    }
    uint64_t gw = UiW("anim-group.frToast");
    if (!gw) {
        T13.st = 0;
        return;
    }
    if (gw != T13.gw) {
        T13.gw = gw;
        FtFill();
        UiVisible("anim-group.frToast", 1);
    }                           // the page was rebuilt under us
    int act = FtAct(T13.it.kind);
    float cx, cy;
    int cur = FtCursor(&cx, &cy);
    int hov = cur && cx >= 0 && cx <= FT_W && cy >= 0 && cy <= FT_H;
    int hy = act && hov && cx >= 852 && cx <= 852 + 340 && cy >= 26
        && cy <= 88, hn = act && hov && cx >= 852 && cx <= 852 + 340
        && cy >= 98 && cy <= 160;
    GuFade(FtP("tFillY"), hy);
    GuFade(FtP("tFillN"), hn);
    GuFadeTick(now);
    if (T13.st != 3) {
        if (!hov)
            T13.left_ms -= dt;  // the cursor on the card stops the clock
        if (T13.st == 1 && (float) (now - T13.t0) >= FT_IN_MS)
            T13.st = 2;
        int ky = GameFocused()
            && NeKeyEdge('Y', &T13.hky), kn = GameFocused()
            && NeKeyEdge('N', &T13.hkn);
        int cl = g_clickEdge && act && (hy || hn);
        if (cl) {
            ky = hy;
            kn = hn;
        }
        if (act && (ky || kn)) {
            const FtItemT *it = &T13.it;
            if (ky) {
                if (it->kind == FT_REQ)
                    FrActQueue(A_ACCEPT, 4, it->id);
                else
                    FrActQueue(A_JOIN, 7, it->id);
            } else {
                if (it->kind == FT_REQ)
                    FrActQueue(A_DECLINE_R, 5, it->id);
                else
                    FrActQueue(A_DECLINE_P, 8, it->id);
            }
            Xlog("friendsui: toast %s \"%s\" (%s)",
                 ky ? "accepted" : "declined", it->name,
                 cl ? "click" : "key");
            T13.st = 3;
            T13.t_out = now;
        } else if (T13.left_ms <= 0.0f) {
            T13.st = 3;
            T13.t_out = now;
        } else if (act && !FtPending(&T13.it)) {
            T13.st = 3;
            T13.t_out = now;
        }                       // answered somewhere else / expired
    }
    float a, dx;
    if (T13.st == 1) {
        float e = FtEase((float) (now - T13.t0) / FT_IN_MS);
        a = e;
        dx = (1.0f - e) * 170.0f;
    } else if (T13.st == 2) {
        a = 1.0f;
        dx = 0.0f;
    } else {
        float p = (float) (now - T13.t_out) / FT_OUT_MS;
        if (p >= 1.0f) {
            FtEnd(now);
            return;
        }
        a = 1.0f - p;
        dx = p * p * 70.0f;
    }
    int qa = (int) (a * 32.0f + 0.5f), qx = (int) (dx * 2.0f + 0.5f);
    if (qa != T13.qa) {
        T13.qa = qa;
        WAlpha(gw, (float) qa / 32.0f);
    }
    if (qx != T13.qx) {
        T13.qx = qx;
        WPivot(gw, FT_X + (float) qx * 0.5f, FT_Y);
    }
    float frac = T13.total_ms > 0.0f ? T13.left_ms / T13.total_ms : 0.0f;
    if (frac < 0.0f)
        frac = 0.0f;
    int qb = (int) (frac * 200.0f + 0.5f);
    if (qb != T13.qb) {
        T13.qb = qb;
        WScale(UiW(FtP("tBar")),
                (float) qb / 200.0f >
                0.002f ? (float) qb / 200.0f : 0.002f, 1.0f);
    }
    if (!(g_frame % 90)) {
        T13.qa = T13.qx = -1;
    }                           // re-asserted twice a second (the engine's own page animations never fight it long)
}

// ------------------------------------------------------------------ rows
static int FrLeftN(void)
{
    return g_f13.has_party ? g_f13.nm + 2 : 0;
}

static int FrRightRows(FrRowT *r)
{
    return Fr13Rows(&g_f13, r, 1 + FR_NPI + 2 * FR_RROWS + FR_NF);
}

static int FrMyReady(void)
{
    for (int i = 0; i < g_f13.nm; i++)
        if (g_f13.m[i].me)
            return g_f13.m[i].ready;
    return 0;
}

// the actions of a row: *pri / *sec = A_* (A_NONE when there is none)
static void FrRowActions(int col, int idx, const FrRowT *rows,
                           int nrows, int *pri, int *sec)
{
    *pri = *sec = A_NONE;
    if (col == 0) {
        if (idx < g_f13.nm) {
            if (g_f13.me_leader && !g_f13.m[idx].me) {
                *pri = A_PROMOTE;
                *sec = A_KICK;
            }
        } else if (idx == g_f13.nm)
            *pri = g_f13.me_leader ? A_NONE : A_READY;  // (the leader starts the search, he has nothing to ready up)
        else if (idx == g_f13.nm + 1)
            *pri = A_LEAVE;
        return;
    }
    if (idx < 0 || idx >= nrows)
        return;
    switch (rows[idx].kind) {
    case FR_K_ADD:
        *pri = A_TYPE;
        break;
    case FR_K_PINV:
        *pri = A_JOIN;
        *sec = A_DECLINE_P;
        break;
    case FR_K_REQ:
        *pri = A_ACCEPT;
        *sec = A_DECLINE_R;
        break;
    case FR_K_FRIEND:
        *pri =
            Fr13CanInvite(&g_f13,
                            &g_f13.f[rows[idx].idx]) ? A_INVITE : A_NONE;
        *sec = A_REMOVE;
        break;
    case FR_K_OUT:
        *sec = A_CANCEL;
        break;
    }
}

static const char *FrLabel(int a)
{
    switch (a) {
    case A_TYPE:
        return FRT("ADD A FRIEND", "DODAJ ZNAJOMEGO");
    case A_INVITE:
        return FRT("INVITE TO PARTY", "ZAPROŚ DO GRUPY");
    case A_ACCEPT:
        return FRT("ACCEPT", "PRZYJMIJ");
    case A_JOIN:
        return FRT("JOIN PARTY", "DOŁĄCZ DO GRUPY");
    case A_DECLINE_P:
    case A_DECLINE_R:
        return FRT("DECLINE", "ODRZUĆ");
    case A_REMOVE:
        return FRT("REMOVE FRIEND", "USUŃ ZNAJOMEGO");
    case A_CANCEL:
        return FRT("CANCEL REQUEST", "ANULUJ ZAPROSZENIE");
    case A_PROMOTE:
        return FRT("MAKE LEADER", "ZOSTAŃ LIDEREM");
    case A_KICK:
        return FRT("KICK", "WYRZUĆ");
    case A_READY:
        return FrMyReady()? FRT("UNREADY", "NIEGOTOWY") : FRT("READY UP",
                                                                "GOTOWY");
    case A_LEAVE:
        return g_f13.me_leader ? FRT("DISBAND PARTY",
                                     "ROZWIĄŻ GRUPĘ") :
            FRT("LEAVE PARTY", "OPUŚĆ GRUPĘ");
    default:
        return "";
    }
}

static void FrStatusWord(const char *st, char *out, size_t n)
{
    static const char *const COL[5] =
        { "8D897B", "B4B4B4", "7FD07A", "F0B429", "3FA9F5" };
    int p = Fr13Presence(st);
    const char *w =
        p == 4 ? FRT("IN A MATCH",
                     "W MECZU") : p == 3 ? FRT("SEARCHING FOR A MATCH",
                                               "SZUKA MECZU") : p ==
        2 ? FRT("IN THE MENU", "W MENU") : p == 1 ? FRT("IN THE LAUNCHER",
                                                        "W LAUNCHERZE") :
        "OFFLINE";
    snprintf(out, n, "<font color=\"#%s\">%s</font>", COL[p], w);
}

static int FrTone(int p)
{
    return p == 4 ? 4 : p == 3 ? 2 : p == 2 ? 1 : 0;
}

static void FrRp(char *b, size_t n, int col, int i, const char *leaf)
{
    snprintf(b, n, "anim-group.%s.%cRow%d.%c%s%d", col ? "rt" : "lf",
             col ? 'r' : 'l', i, col ? 'r' : 'l', leaf, i);
}

static int g_frPlayer = 1;     // 1 except while fr_set_row draws a real player row (not an action row): a badge even for a name the rank table lacks
#define RKB_RES46 120.0f        // = rank_badge_widgets.reserved(46, 80) = 120: the width the badge takes in front of a 46 pt name
static void FrSetRow(int col, int i, int vis, int tone, const char *name,
                       const char *st, const char *tag, const char *ac,
                       int foc, int hov)
{
    char p[160], g[96];
    snprintf(g, sizeof g, "anim-group.%s.%cRow%d", col ? "rt" : "lf",
             col ? 'r' : 'l', i);
    UiVisible(g, vis);
    F13.rw[col][i] = vis ? UiW(g) : 0;
    if (!vis)
        return;
    for (int k = 0; k < 5; k++) {
        snprintf(p, sizeof p, "%s.%cBd%d_%d", g, col ? 'r' : 'l', k, i);
        UiVisible(p, k == tone);
    }
    // every line is measured against its box (the widgets pivot at the edge they align to): a long name / status / action shrinks toward that edge, a name that still does not fit gets "..."
    // rank badge (rankbadge.c; widgets rank_badge_widgets.py, a 46 pt name): a PLAYER row shows the badge and its name moves right by the badge's width; an action row ("ADD A FRIEND" ...) keeps the plain layout
    int rk = -1, wk = 0, has = name && RkbLookup(name, &rk, &wk);
    if (!has && g_frPlayer && name && name[0] && g_rkb_ver > 0) {
        has = 1;
        rk = 0;
        wk = 0;
    }                           // a party member / inviter the table does not list yet: the lowest rank
    {
        char ri[160], rn[160];
        snprintf(ri, sizeof ri, "%s.%cRk%d", g, col ? 'r' : 'l', i);
        snprintf(rn, sizeof rn, "%s.%cRn%d", g, col ? 'r' : 'l', i);
        RankUi(ri, rn, has ? rk : -1, wk);
    }
    FrRp(p, sizeof p, col, i, "Nm");
    WPivot(UiW(p), has ? 36.0f + RKB_RES46 : 36.0f, 40.0f);
    GuFit(p, name, (col ? 920.0f : 680.0f) - (has ? RKB_RES46 : 0.0f),
           47.0f);
    FrRp(p, sizeof p, col, i, "St");
    GuFit(p, st, col ? 850.0f : 620.0f, 33.0f);
    FrRp(p, sizeof p, col, i, "Tg");
    GuFit(p, tag, 540.0f, 33.0f);
    FrRp(p, sizeof p, col, i, "Ac");
    GuFit(p, ac, 560.0f, 33.0f);
    FrRp(p, sizeof p, col, i, "Fill");
    GuFade(p, foc || hov);
    FrRp(p, sizeof p, col, i, "Frame");
    GuFade(p, foc);
}

static void FrWindow(int n)
{                               // the right list's window and focus
    int rows = FR_RROWS;
    if (n <= 0) {
        F13.foc[1] = 0;
        F13.off[1] = 0;
        return;
    }
    if (F13.foc[1] >= n)
        F13.foc[1] = n - 1;
    if (F13.foc[1] < 0)
        F13.foc[1] = 0;
    if (F13.foc[1] < F13.off[1])
        F13.off[1] = F13.foc[1];
    if (F13.foc[1] >= F13.off[1] + rows)
        F13.off[1] = F13.foc[1] - rows + 1;
    if (F13.off[1] > n - rows)
        F13.off[1] = n - rows;
    if (F13.off[1] < 0)
        F13.off[1] = 0;
}

static void FrFill(void)
{
    char a[200], b[200], c[200], d[200];
    int pl = NtPl();
    UiText("anim-group.hd.hdKick",
            FRT("PARTY AND FRIENDS", "GRUPA I ZNAJOMI"));
    UiText("anim-group.hd.hdTitle", FRT("FRIENDS", "ZNAJOMI"));
    int show = N13.have;
    const char *ld = "";
    if (!show)
        ld = !g_fr_busy
            && N13.fails >= 2 ? FRT("THE SERVER DID NOT ANSWER",
                                    "SERWER NIE ODPOWIADA") :
            FRT("LOADING YOUR FRIENDS", "WCZYTYWANIE ZNAJOMYCH");
    UiText("anim-group.ldTx", ld);
    UiVisible("anim-group.ldTx", !show);
    UiVisible("anim-group.lf", show);
    UiVisible("anim-group.rt", show);
    UiText("anim-group.pBackTx", FRT("BACK", "WSTECZ"));
    if (!show) {
        UiText("anim-group.hd.hdMe", "");
        UiText("anim-group.hd.hdId", "");
        UiText("anim-group.msgTx", "");
        UiText("anim-group.keyTx", "");
        UiText("anim-group.pSelTx", "");
        UiVisible("anim-group.pSelIc", 0);
        UiText("anim-group.pActTx", "");
        UiVisible("anim-group.pActIc", 0);
        return;
    }
    GuFit("anim-group.hd.hdMe", g_f13.me_name, 1300.0f, 47.0f);
    snprintf(a, sizeof a, FRT("YOUR FRIEND ID  %s", "TWOJE ID  %s"),
             g_f13.me_id);
    GuFit("anim-group.hd.hdId", a, 1400.0f, 33.0f);
    // ---- left: PARTY
    int ln = FrLeftN();
    if (F13.foc[0] >= ln)
        F13.foc[0] = ln ? ln - 1 : 0;
    if (!ln && F13.col == 0)
        F13.col = 1;
    UiText("anim-group.lf.lTitle", FRT("PARTY", "GRUPA"));
    if (g_f13.has_party) {
        snprintf(a, sizeof a, "%d / %d%s%s", g_f13.nm,
                 g_f13.pmax ? g_f13.pmax : 4,
                 g_f13.me_leader ? FRT("  -  YOU ARE THE LEADER",
                                       "  -  JESTEŚ LIDEREM") : "",
                 g_f13.searching ? FRT("  -  SEARCHING",
                                       "  -  SZUKA MECZU") : "");
        GuFit("anim-group.lf.lSub", a, 980.0f, 33.0f);
    } else
        UiText("anim-group.lf.lSub", "");
    UiText("anim-group.lf.lNone",
            FRT
            ("NO PARTY YET. INVITE A FRIEND FROM THE LIST ON THE RIGHT - A PARTY QUEUES TOGETHER AND PLAYS ON ONE TEAM.",
             "JESZCZE NIE MASZ GRUPY. ZAPROŚ ZNAJOMEGO Z LISTY PO PRAWEJ - GRUPA SZUKA MECZU RAZEM I GRA W JEDNEJ DRUŻYNIE."));
    UiVisible("anim-group.lf.lNone", !ln);
    FrRowT rows[1 + FR_NPI + 2 * FR_RROWS + FR_NF];
    int nr = FrRightRows(rows);
    for (int i = 0; i < FR_NL; i++) {
        int foc = F13.col == 0 && F13.foc[0] == i, hov = F13.hv_col == 0
            && F13.hv_row == i;
        if (i >= ln) {
            FrSetRow(0, i, 0, 0, "", "", "", "", 0, 0);
            continue;
        }
        int pri, sec;
        FrRowActions(0, i, rows, nr, &pri, &sec);
        if (i < g_f13.nm) {
            const FrMT *m = &g_f13.m[i];
            snprintf(a, sizeof a, "%s%s", m->name,
                     m->me ? FRT("  (YOU)", "  (TY)") : "");
            FrStatusWord(m->status, b, sizeof b);
            snprintf(c, sizeof c, "%s%s%s%s",
                     m->leader ? "<font color=\"#3FA9F5\">" : "",
                     m->leader ? FRT("LEADER", "LIDER") : "",
                     m->leader ? "</font>" : "", "");
            if (m->ready && !m->leader) {
                size_t k = strlen(c);
                snprintf(c + k, SNLEFT(c, k),
                         "%s<font color=\"#7FD07A\">%s</font>",
                         m->leader ? "   " : "", FRT("READY", "GOTOWY"));
            }
            FrSetRow(0, i, 1, FrTone(Fr13Presence(m->status)), a, b, c,
                       FrLabel(pri), foc, hov);
            snprintf(d, sizeof d, "anim-group.lf.lRow%d.lTabN%d", i, i);
            UiVisible(d, !m->ready);   // readiness tab: dark grey = not ready, green = ready (the leader always)
            snprintf(d, sizeof d, "anim-group.lf.lRow%d.lTabG%d", i, i);
            UiVisible(d, m->ready);
        } else if (i == g_f13.nm) {
            if (g_f13.me_leader)
                g_frPlayer =
                    0, FrSetRow(0, i, 1, 0,
                                  FRT("YOU START THE SEARCH",
                                      "TY ZACZYNASZ SZUKANIE"),
                                  FRT
                                  ("FIND MATCH ON THE MP MENU - THE PARTY GOES IN ONCE EVERYONE IS READY",
                                   "ZNAJDŹ MECZ W MENU MP - GRUPA IDZIE RAZEM, GDY WSZYSCY SĄ GOTOWI"),
                                  "", "", 0, 0), g_frPlayer = 1;
            else
                g_frPlayer =
                    0, FrSetRow(0, i, 1, FrMyReady()? 1 : 0,
                                  FrLabel(A_READY),
                                  FRT
                                  ("THE LEADER STARTS THE SEARCH AND THE READY PARTY GOES IN TOGETHER",
                                   "LIDER ZACZYNA SZUKANIE, A GOTOWA GRUPA IDZIE RAZEM"),
                                  "", "", foc, hov), g_frPlayer = 1;
        } else {
            g_frPlayer =
                0, FrSetRow(0, i, 1, 0, FrLabel(A_LEAVE),
                              g_f13.
                              me_leader ?
                              FRT("THE PARTY ENDS FOR EVERYONE",
                                  "GRUPA KOŃCZY SIĘ DLA WSZYSTKICH") :
                              FRT("YOU STAY A FRIEND OF EVERYONE IN IT",
                                  "ZOSTAJESZ ZNAJOMYM WSZYSTKICH Z GRUPY"),
                              "", "", foc, hov), g_frPlayer = 1;
        }
    }
    // ---- right: FRIENDS
    FrWindow(nr);
    int on = 0;
    for (int i = 0; i < g_f13.nf; i++)
        if (Fr13Presence(g_f13.f[i].status) > 0)
            on++;
    UiText("anim-group.rt.rTitle", FRT("FRIENDS", "ZNAJOMI"));
    snprintf(a, sizeof a,
             FRT("%d ONLINE  -  %d FRIENDS%s",
                 "%d ONLINE  -  %d ZNAJOMYCH%s"), on, g_f13.nf,
             g_f13.ni ? (g_f13.ni ==
                         1 ? FRT("  -  1 REQUEST",
                                 "  -  1 ZAPROSZENIE") : "") : "");
    if (g_f13.ni > 1)
        snprintf(a + strlen(a), sizeof a - strlen(a),
                 FRT("  -  %d REQUESTS", "  -  %d ZAPROSZEŃ"), g_f13.ni);
    GuFit("anim-group.rt.rSub", a, 1220.0f, 33.0f);
    for (int i = 0; i < FR_RROWS; i++) {
        int idx = F13.off[1] + i;
        int foc = F13.col == 1 && F13.foc[1] == idx, hov = F13.hv_col == 1
            && F13.hv_row == i;
        if (idx >= nr) {
            FrSetRow(1, i, 0, 0, "", "", "", "", 0, 0);
            continue;
        }
        int pri, sec;
        FrRowActions(1, idx, rows, nr, &pri, &sec);
        const FrRowT *r = &rows[idx];
        if (r->kind == FR_K_ADD) {
            if (F13.typing)
                FrSetRow(1, i, 0, 0, "", "", "", "", 0, 0);   // the text field (fr_inp_tick) sits over this slot
            else
                g_frPlayer =
                    0, FrSetRow(1, i, 1, 0,
                                  FRT("ADD A FRIEND", "DODAJ ZNAJOMEGO"),
                                  FRT("BY PLAYER NAME OR STEAM ID",
                                      "PO NAZWIE GRACZA LUB ID STEAM"), "",
                                  FRT("TYPE A NAME", "WPISZ NAZWĘ"), foc,
                                  hov), g_frPlayer = 1;
        } else if (r->kind == FR_K_PINV) {
            const FrRT *x = &g_f13.pinv[r->idx];
            FrSetRow(1, i, 1, 2, x->name,
                       FRT
                       ("<font color=\"#F0B429\">INVITED YOU TO A PARTY</font>",
                        "<font color=\"#F0B429\">ZAPRASZA CIĘ DO GRUPY</font>"),
                       "", FrLabel(pri), foc, hov);
        } else if (r->kind == FR_K_REQ) {
            const FrRT *x = &g_f13.in[r->idx];
            FrSetRow(1, i, 1, 3, x->name,
                       FRT
                       ("<font color=\"#F2625A\">WANTS TO BE YOUR FRIEND</font>",
                        "<font color=\"#F2625A\">CHCE ZOSTAĆ TWOIM ZNAJOMYM</font>"),
                       "", FrLabel(pri), foc, hov);
        } else if (r->kind == FR_K_FRIEND) {
            const FrFT *x = &g_f13.f[r->idx];
            FrStatusWord(x->status, b, sizeof b);
            snprintf(d, sizeof d, "%s",
                     x->
                     party ?
                     FRT("<font color=\"#3FA9F5\">IN YOUR PARTY</font>",
                         "<font color=\"#3FA9F5\">W TWOJEJ GRUPIE</font>")
                     : "");
            FrSetRow(1, i, 1, FrTone(Fr13Presence(x->status)), x->name,
                       b, d, FrLabel(pri), foc, hov);
        } else {
            const FrRT *x = &g_f13.out[r->idx];
            FrSetRow(1, i, 1, 0, x->name,
                       FRT("REQUEST SENT - WAITING FOR AN ANSWER",
                           "ZAPROSZENIE WYSŁANE - CZEKA NA ODPOWIEDŹ"),
                       "", "", foc, hov);
        }
    }
    int more = nr - (F13.off[1] + FR_RROWS);
    snprintf(a, sizeof a, more > 0 ? FRT("+ %d MORE", "+ JESZCZE %d") : "",
             more);
    UiText("anim-group.rt.rMore", more > 0 ? a : "");
    // ---- status line, key hints, prompts
    uint64_t now = GetTickCount64();
    GuFit("anim-group.msgTx", N13.msg_until > now ? N13.msg : "", 3200.0f,
           41.0f);
    GuFit("anim-group.keyTx",
           F13.
           typing ?
           FRT
           ("ENTER SEND     ESC CANCEL     CTRL+V PASTE     LEFT / RIGHT / HOME / END MOVE     BACKSPACE / DELETE ERASE",
            "ENTER WYŚLIJ     ESC ANULUJ     CTRL+V WKLEJ     STRZAŁKI / HOME / END PRZESUWAJĄ     BACKSPACE / DELETE KASUJE")
           : g_padMode ?
           FRT
           ("STICK / D-PAD  SELECT     THE PROMPTS BELOW DO IT     BUMPERS  SWITCH LIST     BACK  CLOSE",
            "GAŁKA / KRZYŻAK ZAZNACZ     PRZYCISKI NA DOLE WYKONUJĄ     ZDERZAKI ZMIEŃ LISTĘ     WSTECZ ZAMKNIJ")
           :
           FRT
           ("CLICK OR W / S SELECT     ENTER OR THE PROMPT BELOW DOES IT     X SECOND ACTION     R READY     L LEAVE PARTY     TAB SWITCH LIST     P OR ESC CLOSE",
            "KLIKNIJ LUB W / S ZAZNACZ     ENTER LUB PRZYCISK NA DOLE WYKONUJE     X DRUGA AKCJA     R GOTOWY     L OPUŚĆ GRUPĘ     TAB ZMIEŃ LISTĘ     P LUB ESC ZAMKNIJ"),
           3200.0f, 33.0f);
    int pri = A_NONE, sec = A_NONE;
    int col = F13.col, idx = F13.foc[col];
    FrRowActions(col, col ? idx : idx, rows, nr, &pri, &sec);
    if (F13.typing) {
        pri = A_NONE;
        sec = A_NONE;
    }
    GuFit("anim-group.pSelTx",
           F13.typing ? FRT("SEND", "WYŚLIJ") : FrLabel(pri), 640.0f,
           49.0f);
    UiVisible("anim-group.pSelIc", F13.typing || pri != A_NONE);
    GuFit("anim-group.pActTx", FrLabel(sec), 640.0f, 49.0f);
    UiVisible("anim-group.pActIc", sec != A_NONE);
    GuFit("anim-group.pTabTx",
           ln ? FRT("SWITCH LIST", "ZMIEŃ LISTĘ") : "", 760.0f, 49.0f);
    UiVisible("anim-group.pPrevIc", ln != 0);
    UiVisible("anim-group.pNextIc", ln != 0);
    (void) pl;
}

// ------------------------------------------------------------------ the mouse
static void FrScan(void)
{
    float cx, cy;
    F13.hv_col = F13.hv_row = -1;
    F13.hv_back = F13.hv_sel = F13.hv_act = 0;
    if (!UiCursorCanvas(&cx, &cy))
        return;
    static const int LY[FR_NL] =
        { 540, 710, 880, 1050, 1260, 1410 }, LH[FR_NL] =
        { 150, 150, 150, 150, 130, 130 };
    int ln = FrLeftN();
    if (cx >= 300 && cx <= 1700)
        for (int i = 0; i < ln && i < FR_NL; i++)
            if (cy >= LY[i] && cy <= LY[i] + LH[i])
                F13.hv_col = 0, F13.hv_row = i;
    if (cx >= 1900 && cx <= 3540)
        for (int i = 0; i < FR_RROWS; i++)
            if (cy >= 540 + 160 * i && cy <= 540 + 160 * i + 140)
                F13.hv_col = 1, F13.hv_row = i;
    if (F13.hv_col == 1) {
        FrRowT rows[1 + FR_NPI + 2 * FR_RROWS + FR_NF];
        if (F13.off[1] + F13.hv_row >= FrRightRows(rows))
            F13.hv_col = F13.hv_row = -1;
    }
    F13.hv_back = cx >= 250 && cx <= 730 && cy >= 1880 && cy <= 1970;
    F13.hv_sel = cx >= 740 && cx < 1500 && cy >= 1880 && cy <= 1970;    // the footer prompts are the action buttons (select only moves, like gear up)
    F13.hv_act = cx >= 1500 && cx < 2260 && cy >= 1880 && cy <= 1970;
}

static int Fr13ClickHit(void)
{
    if (!F13.up)
        return 0;
    FrScan();
    return F13.hv_row >= 0 || F13.hv_back || F13.hv_sel || F13.hv_act;
}

// ------------------------------------------------------------------ text entry
// The ADD FRIEND field: a real one-line editor. Typed: letters, digits, space - _ . : [ ] (Ctrl+V pastes the same set); Left / Right / Home / End move the caret, Backspace / Delete (and Ctrl+Backspace for a
// word) erase with key repeat, Enter sends, Esc cancels typing (a second Esc closes the page). Backspace is also the engine's menu-back: it pops the page by itself, so the page is pushed
// again the same frame with the text, the caret and the focus intact (the names page does the same, feature_names; the pad's B / a right click still cancel).
#define FR_BUFMAX 31
static int FrOkch(unsigned char c)
{
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z') || c == ' ' || c == '-' || c == '_'
        || c == '.' || c == ':' || c == '[' || c == ']';
}

static void FrEdited(void)
{
    F13.edit_ms = GetTickCount64();
    F13.dirty = 1;
}

static int FrPrevCh(int i)
{
    if (i > 0)
        i--;
    while (i > 0 && ((unsigned char) F13.buf[i] & 0xC0) == 0x80)
        i--;
    return i;
}

static int FrNextCh(int i)
{
    if (i < F13.blen)
        i++;
    while (i < F13.blen && ((unsigned char) F13.buf[i] & 0xC0) == 0x80)
        i++;
    return i;
}

static void FrCut(int from, int to)
{                               // erase buf[from..to), the caret goes to from
    if (from < 0 || to > F13.blen || from >= to)
        return;
    memmove(F13.buf + from, F13.buf + to, (size_t) (F13.blen - to + 1));
    F13.blen -= to - from;
    F13.caret = from;
    FrEdited();
}

static void FrIns(const char *t, int n)
{                               // insert n bytes at the caret (whole characters only)
    if (n <= 0 || F13.blen + n > FR_BUFMAX)
        return;
    memmove(F13.buf + F13.caret + n, F13.buf + F13.caret,
            (size_t) (F13.blen - F13.caret + 1));
    memcpy(F13.buf + F13.caret, t, (size_t) n);
    F13.blen += n;
    F13.caret += n;
    FrEdited();
}

static void FrBufAdd(char ch)
{
    if (ch == ' ' && !F13.caret)
        return;
    FrIns(&ch, 1);
}

static void FrTypingStart(void)
{
    F13.typing = 1;
    F13.blen = 0;
    F13.caret = 0;
    F13.buf[0] = 0;
    F13.col = 1;
    F13.foc[1] = 0;
    F13.off[1] = 0;
    F13.typ_ms = F13.edit_ms = GetTickCount64();
    F13.ul_q = F13.gl_q = -1;
    for (int vk = 8; vk < 0xC0; vk++)
        F13.hk[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;      // whatever is held now is no new press
    F13.hk[VK_RETURN] = 1;
    F13.dirty = 1;
}

static void FrTypingStop(void)
{
    F13.typing = 0;
    F13.blen = 0;
    F13.caret = 0;
    F13.buf[0] = 0;
    F13.dirty = 1;
    for (int vk = 8; vk < 0xC0; vk++)
        F13.hk[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void FrPaste(void)
{
    if (!OpenClipboard(NULL))
        return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *w = (const wchar_t *) GlobalLock(h);
        if (w) {
            char u[200];
            int n =
                WideCharToMultiByte(CP_UTF8, 0, w, (int) wcsnlen(w, 40), u,
                                    (int) sizeof u - 1, NULL, NULL);
            GlobalUnlock(h);
            for (int i = 0; i < n;) {   // whole characters of the allowed set (no markup characters can get into the field)
                int l = 1;
                unsigned char c = (unsigned char) u[i];
                if (c >= 0xF0)
                    l = 4;
                else if (c >= 0xE0)
                    l = 3;
                else if (c >= 0xC0)
                    l = 2;
                if (i + l > n)
                    break;
                if (FrOkch(c) && !(c == ' ' && !F13.caret)) {
                    if (F13.blen + l > FR_BUFMAX)
                        break;
                    FrIns(u + i, l);
                }
                i += l;
            }
        }
    }
    CloseClipboard();
}

static int FrKrep(int vk, int slot, uint64_t now)
{                               // a key's press + repeat (first after 400 ms, then every 45 ms)
    int on = (GetAsyncKeyState(vk) & 0x8000) != 0, fire = 0;
    if (on && !F13.hk[vk]) {
        fire = 1;
        F13.rep_t[slot] = now + 400;
    } else if (on && now >= F13.rep_t[slot]) {
        fire = 1;
        F13.rep_t[slot] = now + 45;
    }
    F13.hk[vk] = on;
    return fire;
}

static void FrTypingTick(uint64_t now)
{
    int shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0, ctrl =
        (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    for (int vk = 'A'; vk <= 'Z'; vk++) {
        if (!NeKeyEdge(vk, &F13.hk[vk]))
            continue;
        if (ctrl) {
            if (vk == 'V')
                FrPaste();
            continue;
        }
        FrBufAdd((char) (shift ? vk : vk + 32));
    }
    for (int vk = '0'; vk <= '9'; vk++)
        if (NeKeyEdge(vk, &F13.hk[vk]) && !shift && !ctrl)
            FrBufAdd((char) vk);
    for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; vk++)
        if (NeKeyEdge(vk, &F13.hk[vk]))
            FrBufAdd((char) ('0' + vk - VK_NUMPAD0));
    if (NeKeyEdge(VK_SPACE, &F13.hk[VK_SPACE]))
        FrBufAdd(' ');
    if (NeKeyEdge(0xBD, &F13.hk[0xBD]))
        FrBufAdd(shift ? '_' : '-');
    if (NeKeyEdge(0xBE, &F13.hk[0xBE]))
        FrBufAdd('.');
    if (NeKeyEdge(0xBA, &F13.hk[0xBA]) && shift)
        FrBufAdd(':');
    if (NeKeyEdge(0xDB, &F13.hk[0xDB]) && !shift)
        FrBufAdd('[');
    if (NeKeyEdge(0xDD, &F13.hk[0xDD]) && !shift)
        FrBufAdd(']');
    if ((GetAsyncKeyState(VK_BACK) & 0x8000))
        F13.bs_ms = now;
    if (FrKrep(VK_BACK, 2, now)) {
        if (ctrl) {
            int to = F13.caret;
            while (to > 0 && F13.buf[to - 1] == ' ')
                to--;
            while (to > 0 && F13.buf[to - 1] != ' ')
                to--;
            FrCut(to, F13.caret);
        } else
            FrCut(FrPrevCh(F13.caret), F13.caret);
    }
    if (FrKrep(VK_DELETE, 3, now))
        FrCut(F13.caret, FrNextCh(F13.caret));
    if (FrKrep(VK_LEFT, 0, now)) {
        F13.caret = FrPrevCh(F13.caret);
        FrEdited();
    }
    if (FrKrep(VK_RIGHT, 1, now)) {
        F13.caret = FrNextCh(F13.caret);
        FrEdited();
    }
    if (NeKeyEdge(VK_HOME, &F13.hk[VK_HOME])) {
        F13.caret = 0;
        FrEdited();
    }
    if (NeKeyEdge(VK_END, &F13.hk[VK_END])) {
        F13.caret = F13.blen;
        FrEdited();
    }
    NeKeyEdge(VK_UP, &F13.hk[VK_UP]);
    NeKeyEdge(VK_DOWN, &F13.hk[VK_DOWN]);     // (held state kept current)
}

// the field's look, every frame it is up: alpha in / out (about 170 ms), the underline growing from the centre (260 ms, ease-out cubic), a faint glow breathing on it, the placeholder fading out
// with the first character (about 150 ms), the caret = a "|" in the text drawn in a grey that pulses between the card and the text colour (solid for 450 ms after every edit, then a 1.06 s cosine
// blink; same glyph width in every state, so the text never jitters). Writes only when a quantised value changes.
static void FrInpTick(uint64_t now)
{
    static uint64_t last;
    float dt = last ? (float) (now - last) : 16.0f;
    last = now;
    if (dt > 60.0f)
        dt = 60.0f;
    float tg = F13.typing ? 1.0f : 0.0f;
    F13.inp_a +=
        (tg - F13.inp_a) * (1.0f -
                            expf(-dt / (F13.typing ? 55.0f : 45.0f)));
    if (fabsf(tg - F13.inp_a) < 0.02f)
        F13.inp_a = tg;
    uint64_t g = UiW("anim-group.rt.inp");
    int on = g && (F13.inp_a > 0.0f || F13.typing);
    if (g)
        UiVisible("anim-group.rt.inp", on);
    if (!on)
        return;
    int qa = (int) (F13.inp_a * 24.0f + 0.5f);
    if (qa != F13.ia_q) {
        F13.ia_q = qa;
        WAlpha(g, (float) qa / 24.0f);
    }
    float p =
        F13.typing ? FtEase((float) (now - F13.typ_ms) / 260.0f) : 1.0f;
    float sx = p > 0.001f ? p : 0.001f;
    int qs = (int) (sx * 64.0f + 0.5f);
    if (qs != F13.ul_q) {
        F13.ul_q = qs;
        uint64_t u = UiW("anim-group.rt.inp.inpUl");
        WScale(u, sx, 1.0f);
        WAlpha(u, 0.94f);
        WScale(UiW("anim-group.rt.inp.inpGlow"), sx, 1.0f);
    }
    float pulse = 0.5f + 0.5f * sinf((float) (now % 100000) / 380.0f);
    int qg =
        F13.typing ? (int) (p * (0.05f + 0.05f * pulse) * 60.0f +
                            0.5f) : 0;
    if (qg != F13.gl_q) {
        F13.gl_q = qg;
        WAlpha(UiW("anim-group.rt.inp.inpGlow"), (float) qg / 60.0f);
    }
    float pt = (F13.typing && F13.blen == 0) ? 1.0f : 0.0f;
    F13.ph_a += (pt - F13.ph_a) * (1.0f - expf(-dt / 40.0f));
    if (fabsf(pt - F13.ph_a) < 0.02f)
        F13.ph_a = pt;
    int qp = (int) (F13.ph_a * 24.0f + 0.5f);
    if (qp != F13.pa_q) {
        F13.pa_q = qp;
        uint64_t ph = UiW("anim-group.rt.inp.inpPh");
        WAlpha(ph, (float) qp / 24.0f);
        WVisible(ph, qp > 0);
    }
    if (!F13.typing)
        return;                 // (fading out: the card keeps the last text)
    GuFit("anim-group.rt.inp.inpLbl",
           FRT("ADD A FRIEND  -  PLAYER NAME OR STEAM ID",
               "DODAJ ZNAJOMEGO  -  NAZWA GRACZA LUB ID STEAM"), 1060.0f,
           33.0f);
    GuFit("anim-group.rt.inp.inpPh",
           FRT("TYPE A NAME OR A STEAM ID", "WPISZ NAZWĘ LUB ID STEAM"),
           1060.0f, 47.0f);
    GuFit("anim-group.rt.inp.inpKey", FRT("ENTER SENDS", "ENTER WYSYŁA"),
           410.0f, 33.0f);
    char c[32];
    snprintf(c, sizeof c, "%d / %d", F13.blen, FR_BUFMAX);
    UiText("anim-group.rt.inp.inpCnt", c);
    uint64_t t = now - F13.edit_ms;
    float lv =
        t <
        450 ? 1.0f : 0.5f +
        0.5f * cosf((float) (t - 450) * (6.2831853f / 1060.0f));
    int lq = (int) (lv * 5.0f + 0.5f);
    unsigned grey = 0x1A + (unsigned) ((0xF0 - 0x1A) * lq / 5);
    char tx[120];
    snprintf(tx, sizeof tx, "%.*s<font color=\"#%02X%02X%02X\">|</font>%s",
             F13.caret, F13.buf, grey, grey, grey, F13.buf + F13.caret);
    GuFit("anim-group.rt.inp.inpTx", tx, 1090.0f, 55.0f);
}

// ------------------------------------------------------------------ the page
static void FrSfx(void)
{
    uint64_t now = GetTickCount64();
    if (now - F13.sfx_ms >= 60) {
        F13.sfx_ms = now;
        UiSfx("sfx-cycle");
    }
}

static void FrEnter(void)
{
    int restore = K13.valid;
    uint64_t now = GetTickCount64();
    memset(&F13, 0, sizeof F13);
    F13.up = 1;
    F13.enter_ms = now;
    F13.dirty = 1;
    F13.col = g_f13.has_party ? 0 : 1;
    F13.hv_col = F13.hv_row = -1;
    F13.h_sel = F13.h_u = F13.h_d = F13.h_l = F13.h_r = F13.h_back =
        F13.h_esc = F13.h_pv = F13.h_nx = F13.h_act = F13.h_click = 1;
    F13.t_u = F13.t_d = ~0ULL;
    for (int vk = 8; vk < 0xC0; vk++)
        F13.hk[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;      // held when it opens
    memset(F13.rq, -1, sizeof F13.rq);
    memset(F13.rqx, -1, sizeof F13.rqx);
    F13.ul_q = F13.gl_q = F13.ia_q = F13.pa_q = -1;
    N13.next_poll = 0;
    N13.msg_until = 0;
    GuFadeReset();
    memset(g_gu_fit, 0, sizeof g_gu_fit);
    WAlpha(UiW("anim-group"), 0.0f);
    if (restore) {              // pushed again after the engine's pop on Backspace: same text, caret, focus; no entrance
        F13.typing = K13.typing;
        F13.blen = K13.blen;
        F13.caret = K13.caret;
        F13.col = K13.col;
        memcpy(F13.foc, K13.foc, sizeof F13.foc);
        memcpy(F13.off, K13.off, sizeof F13.off);
        memcpy(F13.buf, K13.buf, sizeof F13.buf);
        F13.typ_ms = K13.typ_ms;
        F13.edit_ms = now;
        F13.inp_a = F13.typing ? 1.0f : 0.0f;
        F13.ph_a = F13.typing && !F13.blen ? 1.0f : 0.0f;
        F13.enter_ms = now - 800;
        F13.bs_ms = now;
        F13.rep_t[2] = now + 400;
        K13.valid = 0;
        Xlog("friendsui: page up again (text kept, %d characters, caret %d)", F13.blen, F13.caret);
        return;
    }
    Xlog("friendsui: page up");
}

static void FrClose(const char *how)
{
    if (F13.closing)
        return;
    F13.closing = 1;
    F13.close_ms = GetTickCount64();
    Xlog("friendsui: back (%s)", how);
}

// social.py: S_STATE 1, S_SEARCH 2, S_ADD 3, S_ACCEPT 4, S_REMOVE 5, S_INVITE 6, S_JOIN 7, S_DECLINE 8, S_LEAVE 9, S_KICK 10, S_PROMOTE 11, S_READY 12
static void FrDo(int code, int col, int idx, const FrRowT *rows, int nr)
{
    if (code == A_NONE)
        return;
    char id[44] = "";
    if (col == 0 && idx < g_f13.nm)
        snprintf(id, sizeof id, "%s", g_f13.m[idx].id);
    else if (col == 1 && idx >= 0 && idx < nr) {
        const FrRowT *r = &rows[idx];
        if (r->kind == FR_K_PINV)
            snprintf(id, sizeof id, "%s", g_f13.pinv[r->idx].id);
        else if (r->kind == FR_K_REQ)
            snprintf(id, sizeof id, "%s", g_f13.in[r->idx].id);
        else if (r->kind == FR_K_FRIEND)
            snprintf(id, sizeof id, "%s", g_f13.f[r->idx].id);
        else if (r->kind == FR_K_OUT)
            snprintf(id, sizeof id, "%s", g_f13.out[r->idx].id);
    }
    switch (code) {
    case A_TYPE:
        FrTypingStart();
        UiSfx("sfx-select");
        break;
    case A_INVITE:
        FrActQueue(code, 6, id);
        break;
    case A_ACCEPT:
        FrActQueue(code, 4, id);
        break;
    case A_JOIN:
        FrActQueue(code, 7, id);
        break;
    case A_DECLINE_P:
        FrActQueue(code, 8, id);
        break;
    case A_DECLINE_R:
    case A_REMOVE:
    case A_CANCEL:
        FrActQueue(code, 5, id);
        break;
    case A_PROMOTE:
        FrActQueue(code, 11, id);
        break;
    case A_KICK:
        FrActQueue(code, 10, id);
        break;
    case A_READY:
        FrActQueue(code, 12, FrMyReady()? "0" : "1");
        break;
    case A_LEAVE:
        FrActQueue(code, 9, "");
        break;
    default:
        break;
    }
}

static void FrSendTyped(void)
{
    while (F13.blen > 0 && F13.buf[F13.blen - 1] == ' ') {
        F13.blen--;
        F13.buf[F13.blen] = 0;
    }
    if (F13.blen < 2) {
        FrMsg(FRT
               ("TYPE AT LEAST 2 CHARACTERS", "WPISZ CO NAJMNIEJ 2 ZNAKI"),
               0);
        UiSfx("sfx-menu-error");
        F13.caret = F13.blen;
        FrEdited();
        return;
    }
    char t[40];
    snprintf(t, sizeof t, "%s", F13.buf);
    FrActQueue(A_ADD, 3, t);
    FrTypingStop();
}

static void FrPageAlpha(float f)
{
    static int lq = -1;
    static uint64_t le;
    if (F13.enter_ms != le) {
        le = F13.enter_ms;
        lq = -1;
    }
    int q = (int) (f * 24.0f + 0.5f);
    if (q != lq) {
        lq = q;
        WAlpha(UiW("anim-group"), (float) q / 24.0f);
    }
}

// the rows come in one after the other (50 ms apart, the right list 90 ms later; 240 ms each, ease-out cubic: fade + a 60-unit slide from the side of their list) and the focused row
// leans 10 units to the right (a 70 ms follow); everything is written only when its quantised step changes
static void FrRowsAnim(uint64_t now)
{
    static uint64_t last;
    static const int LYY[FR_NL] = { 540, 710, 880, 1050, 1260, 1410 };
    float dt = last ? (float) (now - last) : 16.0f;
    last = now;
    if (dt > 60.0f)
        dt = 60.0f;
    float k = 1.0f - expf(-dt / 70.0f);
    for (int col = 0; col < 2; col++)
        for (int i = 0; i < (col ? FR_RROWS : FR_NL); i++) {
            uint64_t h = F13.rw[col][i];
            if (!h)
                continue;
            float p =
                FtEase(((float) (now - F13.enter_ms) -
                         (float) (50 * i + 90 * col)) / 240.0f);
            int ri = col ? F13.off[1] + i : i;
            float f = (F13.col == col && ri == F13.foc[col]
                       && !F13.typing) ? 1.0f : 0.0f;
            F13.rfx[col][i] += (f - F13.rfx[col][i]) * k;
            if (fabsf(f - F13.rfx[col][i]) < 0.02f)
                F13.rfx[col][i] = f;
            float dx =
                (1.0f - p) * (col ? 60.0f : -60.0f) +
                F13.rfx[col][i] * 10.0f;
            int q = (int) (p * 24.0f + 0.5f), qx =
                (int) (dx * 2.0f + (dx < 0 ? -0.5f : 0.5f));
            if (q != F13.rq[col][i]) {
                F13.rq[col][i] = q;
                WAlpha(h, (float) q / 24.0f);
            }
            if (qx != F13.rqx[col][i]) {
                F13.rqx[col][i] = qx;
                WPivot(h, (col ? 1900.0f : 300.0f) + (float) qx * 0.5f,
                        col ? 540.0f + 160.0f * i : (float) LYY[i]);
            }
        }
}

static void FrPageTick(uint64_t now, uint64_t top)
{
    (void) top;
    if (!F13.up)
        FrEnter();
    if (F13.closing) {          // leaving: the page fades out over 170 ms (ease-out), then pops
        if (F13.closing == 1) {
            float p = (float) (now - F13.close_ms) / 170.0f;
            if (p >= 1.0f) {
                F13.closing = 2;
                PopPage();
            } else
                FrPageAlpha(1.0f - FtEase(p));
            GuFadeTick(now);
        }
        return;
    }
    // ---- input
    int sel =
        MenuEdge(0x6695c5b7921bb5ddULL /* menu-select */ , &F13.h_sel);
    int up = GuRep(&F13.h_u, &F13.t_u, FNV64("menu-up")), down =
        GuRep(&F13.h_d, &F13.t_d, FNV64("menu-down"));
    int left = MenuEdge(FNV64("menu-left"), &F13.h_l), right =
        MenuEdge(FNV64("menu-right"), &F13.h_r);
    int back = MenuEdge(FNV64("menu-back"), &F13.h_back);
    int bp = MenuEdge(FNV64("menu-prev"), &F13.h_pv), bn =
        MenuEdge(FNV64("menu-next"), &F13.h_nx);
    int pad_act = MenuEdge(FNV64("menu-action"), &F13.h_act);
    int esc_now = GameFocused()
        && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0, esc = esc_now
        && !F13.h_esc;
    F13.h_esc = esc_now;
    int foc_win = GameFocused();
    int click = g_clickEdge;
    FrScan();
    int wn = WheelNotches();
    if (F13.typing) {
        if (foc_win)
            FrTypingTick(now);
        // Backspace is menu-back too: it edits here (and the engine's pop of the page is undone, feature_friends13); only the pad's B / a right click cancel typing
        int kb_bs = (GetAsyncKeyState(VK_BACK) & 0x8000) || (F13.bs_ms
                                                             && now -
                                                             F13.bs_ms <
                                                             300);
        int pad_back = back && !kb_bs;
        if (esc || pad_back) {
            FrTypingStop();
            esc = 0;
        } else if (foc_win && NeKeyEdge(VK_RETURN, &F13.hk[VK_RETURN]))
            FrSendTyped();
        else if (click && !F13.hv_back
                 && !(F13.hv_col == 1 && F13.hv_row == 0
                      && F13.off[1] == 0))
            FrTypingStop();   // a click anywhere else leaves the field
        back = 0;
        if (!F13.hv_back)
            click = 0;
        sel = up = down = left = right = bp = bn = pad_act = wn = 0;
    }
    if (esc || (click && F13.hv_back)) {
        FrClose(esc ? "Esc" : "BACK clicked");
        return;
    }
    if (back)
        F13.back_f = g_frame ? g_frame : 1;     // the engine pops a pushed page on menu-back by itself; judged 6 frames later
    if (F13.back_f && g_frame >= F13.back_f + 6) {
        F13.back_f = 0;
        if (TopPage() == FrPageId()) {
            FrClose("menu-back");
            return;
        }
    }
    int kp = foc_win && !F13.typing && NeKeyEdge('P', &F13.hk['P']);
    if (kp) {
        FrClose("P");
        return;
    }
    FrRowT rows[1 + FR_NPI + 2 * FR_RROWS + FR_NF];
    int nr = FrRightRows(rows), ln = FrLeftN();
    int kx = 0, kr = 0, kl = 0, ktab = 0;
    if (foc_win && !F13.typing) {
        kx = NeKeyEdge('X', &F13.hk['X']) | NeKeyEdge(VK_DELETE,
                                                          &F13.
                                                          hk[VK_DELETE]);
        kr = NeKeyEdge('R', &F13.hk['R']);
        kl = NeKeyEdge('L', &F13.hk['L']);
        ktab =
            NeKeyEdge(VK_TAB, &F13.hk[VK_TAB]) | NeKeyEdge('Q',
                                                               &F13.
                                                               hk['Q']) |
            NeKeyEdge('E', &F13.hk['E']);
    }
    if (!N13.have) {
        sel = up = down = 0;
        kx = kr = kl = 0;
    }
    int col = F13.col, n = col ? nr : ln;
    int foc = F13.foc[col];
    if (up && n) {
        foc = (foc + n - 1) % n;
        FrSfx();
    }
    if (down && n) {
        foc = (foc + 1) % n;
        FrSfx();
    }
    if (wn && n) {
        foc -= wn;
        if (foc < 0)
            foc = 0;
        if (foc >= n)
            foc = n - 1;
        FrSfx();
    }
    int sw = ktab || bp || bn || left || right;
    if (sw && ln) {
        col = !col;
        F13.col = col;
        n = col ? nr : ln;
        foc = F13.foc[col];
        FrSfx();
    }
    // clicking on a member that has invited you used to select and accept automatically, so now it works like gear up: a click on a row ONLY moves the selection;
    // an action happens by ENTER / the pad's SELECT / X / the pad's ACTION or a click on the prompt in the footer (the prompt names the action). The menu-select the
    // engine raises with a mouse click is dropped (a few frames after any click).
    if (click)
        F13.click_f = g_frame ? g_frame : 1;
    if (sel && F13.click_f && g_frame - F13.click_f <= 8) {
        sel = 0;
        Xlog("friendsui: menu-select ignored: it came with a click (select only moves)");
    }
    int clk_row = 0;
    if (click && F13.hv_row >= 0) {
        int ci = F13.hv_col, ri =
            F13.hv_col ? F13.off[1] + F13.hv_row : F13.hv_row;
        col = ci;
        F13.col = col;
        foc = ri;
        n = col ? nr : ln;
        FrSfx();
        clk_row = 1;
    }
    if (click && !clk_row && F13.hv_sel && !F13.typing)
        sel = 1;                // the footer prompt (primary action of the focused row)
    if (click && !clk_row && F13.hv_act && !F13.typing)
        pad_act = 1;            // the footer prompt (second action)
    if (foc >= n)
        foc = n ? n - 1 : 0;
    if (foc != F13.foc[col] || col != F13.col) {
        F13.foc[col] = foc;
        F13.col = col;
        F13.dirty = 1;
    }
    int pri, sec;
    FrRowActions(col, F13.foc[col], rows, nr, &pri, &sec);
    if (sel) {
        if (pri != A_NONE)
            FrDo(pri, col, F13.foc[col], rows, nr);
        else if (n)
            UiSfx("sfx-menu-error");
    }
    if (kx || pad_act) {
        if (sec != A_NONE)
            FrDo(sec, col, F13.foc[col], rows, nr);
    }
    if (kr && g_f13.has_party && !g_f13.me_leader)
        FrDo(A_READY, 0, g_f13.nm, rows, nr);
    if (kl && g_f13.has_party)
        FrDo(A_LEAVE, 0, g_f13.nm + 1, rows, nr);
    if (sel || kx || pad_act || kr || kl || up || down || sw)
        F13.dirty = 1;
    static const uint32_t OFF_MS[] = { 33, 100, 250, 670, 1500, 3000 };
    if (F13.stage < (int) (sizeof OFF_MS / sizeof OFF_MS[0])
        && now - F13.enter_ms >= OFF_MS[F13.stage]) {
        F13.stage++;
        F13.dirty = 1;
        GuFadeReassert();
    }
    {                           // the hover highlight follows the cursor
        static int lh = -9, lc = -9;
        if (lh != F13.hv_row || lc != F13.hv_col) {
            lh = F13.hv_row;
            lc = F13.hv_col;
            F13.dirty = 1;
        }
    }
    if (N13.msg_until && now > N13.msg_until) {
        N13.msg_until = 0;
        F13.dirty = 1;
    }
    {
        static int lpd = -1;
        if (lpd != g_padMode) {
            lpd = g_padMode;
            F13.dirty = 1;
        }
    }                           // the key hint line follows the device in use
    if (F13.dirty || !(g_frame % 90)) {
        F13.dirty = 0;
        FrFill();
    }
    FrInpTick(now);
    FrRowsAnim(now);
    GuFadeTick(now);
    {                           // page fade-in: 0 -> 1 over 220 ms (ease-out), in 1/24 steps
        float f = FtEase((float) (now - F13.enter_ms) / 220.0f);
        FrPageAlpha(f);
    }
}

// ------------------------------------------------------------------ the button on the MP menu / FIND MATCH
static int g_fr_held_p, g_fr_h_nx;      // P / menu-next as of the last frame either the button or the page looked (closing the page with P must not reopen it)
static int FrBtnCursor(void)
{                               // the cursor is over the strip (canvas units = friends13_widgets BTN_*)
    float cx, cy;
    return UiCursorCanvas(&cx, &cy) && cx >= 2500.0f && cx <= 3682.0f
        && cy >= 1560.0f && cy <= 1680.0f;
}

static int Fr13BtnOver(void)
{
    return g_frOff == 0 && (FrBtnCursor() || FrToastHit());
}

// before pressing find match the lobby shows the party member in one of its rows instead of 1/8: winmm fills the
// lobby rows from its name vector only while searching (session state 3); on the MP menu it writes row 0 (us) alone. The party mates
// go into rows 1.. here, and the count follows; a party that ends clears them once.
static void FrLobbyPartyRows(void)
{
    static int shown;
    int st = *(volatile int *) T2R_SYM(kPartyStatus), want = N13.have && g_f13.has_party && st >= 0 && st <= 2 && FrMenuVis();        // idle / connecting / connected only: from searching (3) on winmm owns the rows (a found player's row went blank, then showed our mate)
    char p[96], a[24];
    if (!want) {
        if (shown && st >= 0 && st <= 2) {
            for (int r = 1; r <= shown; r++) {
                snprintf(p, sizeof p,
                         "anim-group.lobbyPanel.lobbyList.Player%d.nameLabel",
                         r);
                UiText(p, "");
            }
            UiText("anim-group.lobbyPanel.lobbyCount", "1 / 8");
        }
        shown = 0;              // searching / matched: never touch winmm's rows
        return;
    }
    int r = 1;
    for (int i = 0; i < g_f13.nm && r < 8; i++) {
        if (g_f13.m[i].me || !g_f13.m[i].name[0])
            continue;
        snprintf(p, sizeof p,
                 "anim-group.lobbyPanel.lobbyList.Player%d.nameLabel",
                 r++);
        UiText(p, g_f13.m[i].name);
    }
    for (int k = r; k <= shown; k++) {
        snprintf(p, sizeof p,
                 "anim-group.lobbyPanel.lobbyList.Player%d.nameLabel", k);
        UiText(p, "");
    }
    if (r - 1 != shown)
        Xlog("friendsui: lobby list shows %d party mate%s before FIND MATCH", r - 1, r == 2 ? "" : "s");
    shown = r - 1;
    snprintf(a, sizeof a, "%d / 8", r);
    UiText("anim-group.lobbyPanel.lobbyCount", a);
}

// ------------------------------------------------------------------ the party rule + the lobby tabs
// Party members do not have to search, only ready up (their find match button becomes ready up): a NON-leader member's FIND MATCH row reads READY UP / UNREADY; the press is
// a ready toggle (op 0xD8 sub 12), never a search - fr_search_gate (called by t2r-ext's mmreq hook, the moment the engine sends the search request) says so and the page is popped
// again; the daemon (social.py search_gate) refuses to queue such a member as well. The LEADER's FIND MATCH searches for the whole party, but only when every member is ready
// (otherwise a deny note names who is not). While the leader searches, a READY member's client pushes the search page by itself (autofind's push: winmm's page tick starts the
// search) so the party queues together; a member who backs out of that search is set unready. Ready flags reset when a match forms (daemon start_match).
// Ready-up rework:
// - a member's press is handled on the MP menu itself (modepick -> fr_member_ready_press): the ready toggle, no search page at all;
// - the leader is never held back: FIND MATCH always searches, the daemon waits for the members (PARTY-WAIT) and every READY member's client follows;
// - members join a party READY and stay ready after a match (daemon), so the usual round is: the leader presses FIND MATCH, everyone goes in.
static uint64_t g_pqAuto;      // GetTickCount64 of our own follow-the-leader push (its mmreq must go through); also set by move_rejoin_tick
static int FrInParty(void)
{
    return N13.have && g_f13.has_party && g_f13.nm >= 2;
}

// 0 = the search request goes on; 1 = a member's READY UP / UNREADY press (toggled here, the caller pops the page); 2 = the leader is held back (a mate is not ready)
static int FrPartyMember(void)
{
    return FrInParty() && !g_f13.me_leader;
}

static int FrSearchGate(void)
{
    if (!FrInParty())
        return 0;
    uint64_t now = GetTickCount64();
    if (g_f13.me_leader) {
        for (int i = 0; i < g_f13.nm; i++) {
            const FrMT *m = &g_f13.m[i];
            if (!m->me && !m->leader && !m->ready)
                Xlog("friendsui: leader searches, %s is not ready yet (the daemon waits for it)", m->name);
        }
        return 0;
    }
    if (g_pqAuto && now - g_pqAuto < 8000) {
        g_pqAuto = 0;
        return 0;
    }
    Xlog("friendsui: a member's search request outside the follow-the-leader push - page popped, ready state unchanged");
    return 1;
}

// a member's FIND MATCH (READY UP / UNREADY) press on the MP menu: the toggle only (the row label follows the party state)
static void FrMemberReadyPress(void)
{
    static uint64_t last;
    uint64_t now = GetTickCount64();
    if (now - last < 600)
        return;                 // (one press = one toggle: a held key or a double click never flips it back)
    last = now;
    int r = FrMyReady();
    FrActQueue(A_READY, 12, r ? "0" : "1");
    Xlog("friendsui: member's FIND MATCH = %s%s",
         r ? "UNREADY" : "READY UP", !r
         && g_f13.lsearch ? " (the leader searches: we follow)" : "");
}

static void FrPartyTick(uint64_t now, uint64_t top, uint64_t p_main)
{
    static int lat, in, pr;
    static uint64_t pushed_at;
    static char lab_set;
    int mem = FrInParty() && !g_f13.me_leader && FrMenuVis();
    if (mem || lab_set) {       // the menu row: FIND MATCH -> READY UP / UNREADY for a member
        const char *t =
            mem ? (FrMyReady()? FRT("UNREADY", "NIEGOTOWY") :
                   FRT("READY UP", "GOTOWY")) : "FIND MATCH";
        if (top == p_main) {
            UiText("anim-group.menuList.FindMatch.label0", t);
            lab_set = mem;
        }
    }
    // The daemon acks every FIND MATCH with the SEARCHING state before its party gate decides, so a request it then refuses
    // (member while the leader is not queued yet, leader while its ready flags differ) left the page up as a fake search: rows "<name> (party lead - not searching)" for ever, nobody queued.
    // Our own status in the party state says we are NOT searching while that page is up for 9 s: pop it, and let the follow-the-leader push try again.
    {
        static uint64_t fake_t0;
        int me_p = -1;
        for (int i = 0; i < g_f13.nm; i++)
            if (g_f13.m[i].me)
                me_p = Fr13Presence(g_f13.m[i].status);
        static uint64_t P_FM;
        if (!P_FM)
            P_FM = PageId("find-match-menu");
        if (FrInParty() && top == P_FM && me_p >= 0 && me_p < 3
            && now - g_pqAuto > 9000) {
            if (!fake_t0)
                fake_t0 = now;
            else if (now - fake_t0 > 9000) {
                fake_t0 = 0;
                g_mmBack = 1;
                lat = 0;
                in = 0;
                Xlog("friendsui: search page up for 9 s but the daemon has us queued nowhere - popped (a retry follows if the leader searches)");
            }
        } else
            fake_t0 = 0;
    }
    int r = FrMyReady();
    if (r && !pr)
        lat = 0;                // (re)readying while the leader searches joins that search
    pr = r;
    int ls = mem && g_f13.lsearch;
    if (!ls) {
        lat = 0;
        in = 0;
        return;
    }
    int st = *(volatile int *) T2R_SYM(kPartyStatus);
    if (in) {
        if (top == p_main && st < 3 && now - pushed_at > 4000) {
            in = 0;
            FrActQueue(A_READY, 12, "0");
            Xlog("friendsui: member left the party search - unready");
        }
        return;
    }
    if (lat || !r || top != p_main || st != 2 || MatchSubstate() == 6)
        return;
    lat = 1;
    in = 1;
    pushed_at = now;
    g_pqAuto = now;
    PushPageNow("to1/find-match-menu");
    Xlog("friendsui: the leader searches and we are ready - search page pushed (the party queues together)");
}

// Team / ready tabs on the lobby rows (widgets teamtab_widgets.py: tabGrey / tabGreen / tabBlue / tabRed per row, the old team symbol icon is zero-size). Once a match is found the
// roster (lobby object +0x1d, 0x32-byte entries, [0] slot, [0x31] & 1 team; rows 1.. = every entry but our own slot, winmm FUN_180065760) gives blue = our team (row 0 = us),
// red = the enemy. Before that, in a party: grey = not ready, green = ready (the leader, a searching mate) - on the MP menu for us and the mates in the rows fr_lobby_party_rows fills.
enum { TB_NONE, TB_GREY, TB_GREEN, TB_BLUE, TB_RED };
static void FrLobbyTabs(void)
{
    static int last[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    static unsigned gen = ~0u;
    static uint64_t when;
    int kind[8] = { 0 };
    int st = *(volatile int *) T2R_SYM(kPartyStatus);
    uint8_t *lo = LobbyObj();
    int fidx[8], fen[8], fn = st >= 3 && TopPage() != PageId("main-menu") ? LobbyRowsSorted(lo, fidx, fen) : -1;    // MATCH FOUND (never on the main MP menu): rows = our team first (blue), the enemy after (red)
    if (fn >= 0) {
        kind[0] = TB_BLUE;
        for (int k = 0; k < fn; k++)
            kind[k + 1] = fen[k] ? TB_RED : TB_BLUE;
    } else if (st >= 3 && TopPage() == PageId("find-match-menu")) {   // searching: every searcher (the daemon's relative [T1] / [T2] marker, cut off in srch_name_write), us included
        kind[0] = TB_BLUE;
        for (int r = 1; r < 8; r++)
            kind[r] =
                g_srchTab[r] == 1 ? TB_BLUE : g_srchTab[r] ==
                2 ? TB_RED : g_srchTab[r] == 3 ? TB_GREY : TB_NONE;
    } else if (FrInParty()) {
        kind[0] = FrMyReady() || g_f13.me_leader ? TB_GREEN : TB_GREY;
        if (st <= 2) {
            int r = 1;
            for (int i = 0; i < g_f13.nm && r < 8; i++) {
                if (g_f13.m[i].me || !g_f13.m[i].name[0])
                    continue;
                kind[r++] = g_f13.m[i].ready ? TB_GREEN : TB_GREY;
            }
        }
    }
    uint64_t now = GetTickCount64();
    int same = 1;
    for (int i = 0; i < 8; i++)
        if (kind[i] != last[i])
            same = 0;
    if (same && gen == g_uiGen && now - when < 1000)
        return;
    static const char *const LEAF[5] =
        { "", "tabGrey", "tabGreen", "tabBlue", "tabRed" };
    char p[112];
    for (int r = 0; r < 8; r++)
        for (int k = 1; k <= 4; k++) {
            snprintf(p, sizeof p,
                     "anim-group.lobbyPanel.lobbyList.Player%d.%s", r,
                     LEAF[k]);
            UiVisible(p, kind[r] == k);
        }
    memcpy(last, kind, sizeof last);
    gen = g_uiGen;
    when = now;
}

// the MATCH FOUND rows list our team first (settext_hook swaps winmm's own writes; this keeps the rows right when the roster changes without a rebuild: late joiner, drop, team move)
static void FrLobbyNames(void)
{
    int idx[8], en[8], st = *(volatile int *) T2R_SYM(kPartyStatus);
    int n = st >= 3
        && TopPage() !=
        PageId("main-menu") ? LobbyRowsSorted(LobbyObj(), idx,
                                                 en) : -1;
    if (n <= 0)
        return;
    uint8_t *lo = LobbyObj();
    for (int k = 0; k < n; k++) {
        char p[96], nm[0x32];
        snprintf(p, sizeof p,
                 "anim-group.lobbyPanel.lobbyList.Player%d.nameLabel",
                 k + 1);
        snprintf(nm, sizeof nm, "%.48s",
                 (const char *) lo + 0x1d + idx[k] * 0x32 + 1);
        if (nm[0])
            UiText(p, nm);
    }
}

// Rank badge on the lobby rows (widgets rkIcon / rkNum in lobby-row + find-match-row; the same row logic as fr_lobby_tabs): row 0 = us (our own clan state), rows 1.. = the
// roster's other players once a match is found, the searchers (srch_name_write keeps their names), else our party mates.
static int FrRowNames(char nm[8][48], int me[8])
{                               // the name each lobby row shows (row 0 = us), the same logic for the rank badges and the party marks
    int used = 0;
    int st = *(volatile int *) T2R_SYM(kPartyStatus);
    uint8_t *lo = LobbyObj();
    int fidx[8], fen[8], fn = st >= 3 ? LobbyRowsSorted(lo, fidx, fen) : -1;  // MATCH FOUND: the same our-team-first order as the names and the tabs
    if (fn >= 0) {
        me[0] = 1;
        for (int k = 0; k < fn; k++)
            snprintf(nm[k + 1], sizeof nm[0], "%.40s",
                     (const char *) lo + 0x1d + fidx[k] * 0x32 + 1);
        used = fn + 1;
    } else if (st >= 3 && TopPage() == PageId("find-match-menu")) {
        me[0] = 1;
        used = 8;
        for (int r = 1; r < 8; r++)
            snprintf(nm[r], sizeof nm[0], "%s", g_srchName[r]);
    } else {                    // the main MP menu, alone or in a party: row 0 is always us (no badge in the main menu - the row only got one in a party)
        me[0] = 1;
        used = 1;
        if (FrInParty() && st <= 2) {
            int r = 1;
            for (int i = 0; i < g_f13.nm && r < 8; i++) {
                if (g_f13.m[i].me || !g_f13.m[i].name[0])
                    continue;
                snprintf(nm[r++], sizeof nm[0], "%.40s", g_f13.m[i].name);
            }
            used = r;
        }
    }
    return used;
}

static void FrLobbyRanks(void)
{
    static char last[8][48];
    static int lastme[8];
    static unsigned gen = ~0u;
    static uint64_t when;
    static LONG ver = -1;
    char nm[8][48] = { { 0 } };
    int me[8] = { 0 };
    int used = FrRowNames(nm, me);
    uint64_t now = GetTickCount64();
    LONG v = g_rkb_ver;
    int same = gen == g_uiGen && ver == v && now - when < 1000
        && !memcmp(last, nm, sizeof last);
    for (int i = 0; i < 8 && same; i++)
        if (lastme[i] != me[i])
            same = 0;
    if (same)
        return;
    for (int r = 0; r < 8; r++) {
        char base[96];
        snprintf(base, sizeof base,
                 "anim-group.lobbyPanel.lobbyList.Player%d", r);
        if (r >= used) {
            char i[120], n[120];
            snprintf(i, sizeof i, "%s.rkIcon", base);
            snprintf(n, sizeof n, "%s.rkNum", base);
            RankUi(i, n, -1, 0);
            continue;
        }
        RkbDrawAt(base, "rkIcon", "rkNum", nm[r], me[r]);
    }
    memcpy(last, nm, sizeof last);
    memcpy(lastme, me, sizeof lastme);
    gen = g_uiGen;
    ver = v;
    when = now;
}

// Party marks on the lobby rows (widgets teamtab_widgets.add_party_marks): a row whose name is one of our party (or our own row, in a party) gets the amber bar + the
// right-aligned tag PARTY LEAD / PARTY over READY / NOT READY. The ready line is for the menu and the not-searching rows (a searching mate is in the search, so no line); once a match
// is found only the bar + role stay. The old " (party - not searching)" suffix is cut off in t2r-ext.c srch_name_write; the grey / green gradient is fr_lobby_tabs.
static void FrLobbyPartyMarks(void)
{
    static int last[8][3];
    static unsigned gen = ~0u;
    static uint64_t when;
    int mk[8][3] = { { 0 } };   // [row] = {party? 0/1, lead? 0/1, ready line 0 none / 1 READY / 2 NOT READY}
    if (FrInParty()) {
        char nm[8][48] = { { 0 } };
        int me[8] = { 0 };
        int used = FrRowNames(nm, me), st =
            *(volatile int *) T2R_SYM(kPartyStatus);
        int found = st >= 3 && TopPage() != PageId("main-menu")
            && LobbyRowsSorted(LobbyObj(), (int[8]) { 0 },
                                 (int[8]) { 0 }) >= 0;
        for (int r = 0; r < used && r < 8; r++) {
            const FrMT *m = NULL;
            if (me[r]) {
                for (int i = 0; i < g_f13.nm; i++)
                    if (g_f13.m[i].me)
                        m = &g_f13.m[i];
            } else if (nm[r][0]) {
                for (int i = 0; i < g_f13.nm && !m; i++)
                    if (!g_f13.m[i].me && g_f13.m[i].name[0]
                        && !_stricmp(g_f13.m[i].name, nm[r]))
                        m = &g_f13.m[i];
            }
            if (!m)
                continue;
            mk[r][0] = 1;
            mk[r][1] = m->leader != 0;
            int idle = st <= 2 || g_srchTab[r] == 3;   // menu, or a mate that is not in the search
            mk[r][2] = found || !idle ? 0 : (m->leader
                                             || m->ready) ? 1 : 2;
        }
    }
    uint64_t now = GetTickCount64();
    if (gen == g_uiGen && now - when < 1000
        && !memcmp(last, mk, sizeof last))
        return;
    char p[112];
    for (int r = 0; r < 8; r++) {
        const char *b = "anim-group.lobbyPanel.lobbyList.Player";
        snprintf(p, sizeof p, "%s%d.partyBar", b, r);
        UiVisible(p, mk[r][0]);
        snprintf(p, sizeof p, "%s%d.pmRole", b, r);
        UiVisible(p, mk[r][0]);
        if (mk[r][0])
            UiText(p,
                    mk[r][1] ? FRT("PARTY LEAD",
                                   "LIDER GRUPY") : FRT("PARTY", "GRUPA"));
        snprintf(p, sizeof p, "%s%d.pmReady", b, r);
        UiVisible(p, mk[r][2] == 1);
        if (mk[r][2] == 1)
            UiText(p, FRT("READY", "GOTOWY"));
        snprintf(p, sizeof p, "%s%d.pmNot", b, r);
        UiVisible(p, mk[r][2] == 2);
        if (mk[r][2] == 2)
            UiText(p, FRT("NOT READY", "NIEGOTOWY"));
    }
    memcpy(last, mk, sizeof last);
    gen = g_uiGen;
    when = now;
}

static void FrButtonTick(uint64_t now)
{
    static int armed, last_vis = -1;
    static uint64_t last_open;
    FrLobbyPartyRows();
    FrLobbyNames();
    FrLobbyTabs();
    FrLobbyRanks();
    FrLobbyPartyMarks();
    {
        static uint64_t P_MAIN;
        if (!P_MAIN)
            P_MAIN = PageId("main-menu");
        FrPartyTick(now, TopPage(), P_MAIN);
    }
    int visible = FrMenuVis();
    if (visible != last_vis) {
        last_vis = visible;
        UiVisible("anim-group.friendsBtn", visible);
    }
    if (!visible)
        return;
    int hov = FrBtnCursor();
    int badge = N13.have ? Fr13Badge(&g_f13) : 0;
    char a[40];
    UiText("anim-group.friendsBtn.frLabel", FRT("FRIENDS", "ZNAJOMI"));
    if (!N13.have)
        a[0] = 0;
    else if (g_f13.has_party)
        snprintf(a, sizeof a, FRT("PARTY %d/%d", "GRUPA %d/%d"), g_f13.nm,
                 g_f13.pmax ? g_f13.pmax : 4);
    else {
        int on = 0;
        for (int i = 0; i < g_f13.nf; i++)
            if (Fr13Presence(g_f13.f[i].status) > 0)
                on++;
        snprintf(a, sizeof a, "%d ONLINE", on);
    }
    UiText("anim-group.friendsBtn.frSub", a);
    UiVisible("anim-group.friendsBtn.frBadgeBg", badge > 0);
    UiVisible("anim-group.friendsBtn.frBadge", badge > 0);
    if (badge > 0) {
        snprintf(a, sizeof a, "%d", badge > 9 ? 9 : badge);
        UiText("anim-group.friendsBtn.frBadge", a);
    }
    GuFade("anim-group.friendsBtn.frFill", hov);
    if (badge != N13.last_badge) {
        N13.last_badge = badge;
        Xlog("friendsui: button badge %d", badge);
    }
    int kp = GameFocused()
        && (GetAsyncKeyState('P') & 0x8000) != 0, edge = kp
        && !g_fr_held_p;
    g_fr_held_p = kp;
    int pad = MenuEdge(FNV64("menu-next"), &g_fr_h_nx);        // RB on the pad; never on the first look (a button held on arrival)
    if (!armed) {
        armed = 1;
        pad = 0;
        edge = 0;
    }
    int clk = g_clickEdge && hov;
    if (!(edge || pad || clk) || now - last_open < 1200)
        return;
    last_open = now;
    Xlog("friendsui: %s on the MP menu - pushing to1/friends13",
         clk ? "click" : edge ? "P" : "menu-next (pad)");
    PushPage("to1/friends13");
}

// ------------------------------------------------------------------ the driver
static void FeatureFriends13(void)
{
    g_fr13Up = 0;
    RkbTick(GetTickCount64()); // the rank table poll + the scoreboard badges (rankbadge.c), menu or match
    if (!Fr13Enabled())
        return;
    static uint64_t P_MAIN, P_FM;
    if (!P_MAIN) {
        P_MAIN = PageId("main-menu");
        P_FM = PageId("find-match-menu");
    }
    uint64_t now = GetTickCount64(), top = TopPage();
    int in_match = MatchSubstate() == 6;
    int panel = top == FrPageId();
    FrNetTick(now, panel, !in_match && MsOnMenu());
    FtTick(now, panel, !in_match && (top == P_MAIN || top == P_FM));
    if (!panel) {
        if (F13.up) {
            F13.up = 0;
            Xlog("friendsui: page left (top now %s)", PageName(top));
            // Backspace while typing is the engine's menu-back: it popped the page. Undo that: the text is saved and the page pushed again (at most 8 times in 3 s)
            static uint64_t win0;
            static int cnt;
            int bs_now = (GetAsyncKeyState(VK_BACK) & 0x8000) != 0;
            if (F13.typing && !F13.closing && GameFocused()
                && (bs_now || (F13.bs_ms && now - F13.bs_ms < 400))
                && (top == P_MAIN || top == P_FM)) {
                if (now - win0 > 3000) {
                    win0 = now;
                    cnt = 0;
                }
                if (cnt++ < 8) {
                    if (bs_now && !F13.hk[VK_BACK])
                        FrCut(FrPrevCh(F13.caret), F13.caret);       // the press the page had not seen yet
                    K13.valid = 1;
                    K13.typing = 1;
                    K13.blen = F13.blen;
                    K13.caret = F13.caret;
                    K13.col = F13.col;
                    K13.typ_ms = F13.typ_ms;
                    memcpy(K13.foc, F13.foc, sizeof K13.foc);
                    memcpy(K13.off, F13.off, sizeof K13.off);
                    memcpy(K13.buf, F13.buf, sizeof K13.buf);
                    F13.typing = 0;
                    PushPageNow("to1/friends13");
                    Xlog("friendsui: the engine popped the page on Backspace - pushed again (text kept)");
                    return;
                }
            }
            F13.typing = 0;
        }
        if (in_match)
            return;
        if (top == P_MAIN || top == P_FM)
            FrButtonTick(now);
        return;
    }
    g_fr13Up = 1;
    FrPageTick(now, top);
    g_fr_held_p = GameFocused() && (GetAsyncKeyState('P') & 0x8000) != 0;
    g_fr_h_nx = F13.h_nx;
}
