// notice.c - server notices for t2r-ext: "New update available - restart after finishing the match.", "Server restarting in N min",
// the final "Server is shutting down", and free text from the admin site. Included from t2r-ext.c next to gunlimit.c.
//
// Wire (tools/notices.py): every ~6 s, in a match and in the menus, on its own short TCP connection (like op 0xDE):
//   ext -> server  0xBA [64B token][u8 ver=1][u8 lang 0 en / 1 pl][u8 flags: 1 = this game has the in-game FRIENDS panel (friends13.c): request / invite lines then say "Press P"]
//   server -> ext  0xBB "n=<k>\n<id>|<kind>|<param>|<text>\n..."   kind u update (id 0) / r restart (param = seconds left) / s shutting down / t free text / p friends + party line (param = seconds on screen)
// The client renders its own text from kind + param (English, or Polish when the game language is Polish - loc.c g_loc_cur); free text is shown as-is.
// Shown with ntf_push(NTF_SERVER) = the styled toast (notify.c) in a match AND on the menu pages, info icon + a sound; msg_tag() only
// when t2r-old-notify.txt is set or no styled widgets exist (the daemon also pushes the text as an op 0x11 note on a held menu socket). A restart countdown is shown when it first arrives and again at 5 / 2 / 1 min.
// An older server never answers 0xBA: after 3 unanswered tries the ext stops asking. t2r-no-notice.txt next to the exe = off.
static void Fr13NoteSeen(const char *text);   // friends13.c: a friends / party line was shown or queued (its own toasts skip what was announced)
static int Fr13Enabled(void);

static int Fr13ToastLine(const char *text, float secs);      // friends13.c: the friend / party card takes a kind p line (0 = the old toast)
// Friends / party lines (kind p) are never shown over a running match (invites and requests pop up in the menus, not mid-game): they wait here and are shown the
// moment the match is over (the pending request / invite itself stays on the server; the FRIENDS button's badge counts it anyway).
static char g_ntPq[6][300];
static float g_ntPqs[6];
static int g_ntPqn;

static volatile LONG g_nt_busy, g_nt_have;

static char g_ntRep[1100];

static int g_nt_fail, g_nt_ok_once, g_nt_off = -1;

 
static DWORD WINAPI NtWorker(LPVOID arg)
{
    
uint8_t pl[67];
    
memcpy(pl, (const char *) arg, 67);
    
HMODULE ws = LoadLibraryA("ws2_32.dll");
    
T2rSin a;
    
T2rNet net;
    
LONG res = 3;              // 3 = no connection (server briefly unreachable): ignored; 2 = connected, no valid answer (older server)
    if (ws && HmServer(&a) && T2rNetLoad(&net, ws)) {
        
uint8_t frame[3 + 67];
        
int n = 67 + 1;
        
frame[0] = (uint8_t) n;
        frame[1] = 0;
        frame[2] = 0xBA;
        
memcpy(frame + 3, pl, 67);
        
char rep[sizeof g_ntRep];
        
int sent = 0;
        
int got =
            T2rRpc(&net, &a, frame, n + 2, rep, (int) sizeof rep - 1,
                    2500, 2500, 2500, &sent);
        
if (sent)
            res = 2;
        
int ln = got >= 3 ? (rep[0] & 0xff) + ((rep[1] & 0xff) << 8) : 0;
        
if (ln >= 2 && got >= ln + 2 && (uint8_t) rep[2] == 0xBB
             && ln + 2 < (int) sizeof g_ntRep) {
            
memcpy(g_ntRep, rep + 3, ln - 1);
            
g_ntRep[ln - 1] = 0;
            
res = 1;
            
SrvMarkAlive();
        
}
    
}
    
InterlockedExchange(&g_nt_have, res);
    
InterlockedExchange(&g_nt_busy, 0);
    
return 0;

}


 
static int NtPl(void)
{
    return g_locCur >= 0 && g_locCur < LOC_NLANG
        && LOC_LANGS[g_locCur].code[0] == 'p';
}


 
// text of one notice in the game's language; free text (kind t) and a reason on a restart pass through
static void NtRender(char kind, int param, const char *text, int pl,
                      char *out, size_t n)
{
    
int min = (param + 59) / 60;
    
if (min < 1)
        min = 1;
    
switch (kind) {
    
case 'u':
        snprintf(out, n, "%s",
                 pl ?
                 "Dostępna nowa aktualizacja - uruchom grę ponownie po zakończeniu meczu."
                 :
                 "New update available - restart after finishing the match.");
        break;
    
case 'v':
        snprintf(out, n, "%s",
                 pl ?
                 "Dostępna nowa aktualizacja - uruchom ponownie launcher, aby ją zainstalować."
                 :
                 "A new update is available - restart the launcher to update.");
        break;
    
case 'r':
        
if (text && text[0])
            snprintf(out, n,
                     pl ? "Restart serwera za %d min - %s" :
                     "Server restarting in %d min - %s", min, text);
        
        else
            snprintf(out, n,
                     pl ?
                     "Restart serwera za %d min. Dokończ mecz - twój postęp jest zapisany."
                     :
                     "Server restarting in %d min. Finish your match - your progress is saved.",
                     min);
        
break;
    
case 'p':
        snprintf(out, n, "%s", text ? text : "");
        break;                  // friends + party lines (tools/social.py; already in the game's language: the daemon reads the notice poll's language byte)
    case 's':
        snprintf(out, n, "%s",
                 pl ?
                 "Serwer się wyłącza. Trwające mecze mogą się zakończyć."
                 :
                 "Server is shutting down. Matches in progress may end.");
        break;
    
default:
        snprintf(out, n, pl ? "SERWER: %s" : "SERVER: %s",
                 text ? text : "");
        break;                  // free text from the admin ('SERVER: <text>')
    }

}


 
// Players in a match are forced to close their game when an update drops (with a warning and a countdown, and a message
// explaining their clan progress is not lost), because being in a match of a different update introduces odd bugs. The server sends kind u while
// our build differs from the published one; in a match that starts a 60 s countdown (toasts at 60 / 30 / 10 / 5 s), then the game closes.
// The daemon excuses the leave for the clan (version differs from the target). t2r-no-update-close.txt = only the old 'restart after the match' line.
#define NT_UPD_CLOSE_S 60
static uint64_t g_ntCloseAt;

static int NtUpdCloseOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-no-update-close.txt", &at, &off, 300);
}


static void NtUpdCloseToast(int sec)
{
    
char m[300];
    int pl = NtPl();
    
snprintf(m, sizeof m,
              pl ?
              "Wyszła nowa aktualizacja - gra zamknie się za %d s, żeby ją zainstalować. Postęp klanu NIE przepadnie (bez kary za wyjście). Uruchom launcher ponownie."
              
 :
              "A new update is out - the game closes in %d s to install it. Your clan progress will NOT be lost (no left-early penalty). Restart through the launcher.",
              sec);
    
Xlog("notice: update close countdown %d s", sec);
    
if (!NtfPush(NTF_SERVER, m, sec > 8 ? 8.0f : (float) sec))
        MsgTag(m, sec > 8 ? 8.0f : (float) sec, 0);

} 
static void NtUpdCloseTick(void)
{
    
if (!g_ntCloseAt)
        return;
    
static int last = -1;
    
uint64_t now = GetTickCount64();
    
int sec =
        now >=
        g_ntCloseAt ? 0 : (int) ((g_ntCloseAt - now + 999) / 1000);
    
if (sec != last && (sec == 30 || sec == 10 || sec == 5))
        NtUpdCloseToast(sec);
    
last = sec;
    
if (sec > 0)
        return;
    
Xlog
        ("notice: update close - closing the game now (build differs from the published update)");
    
TerminateProcess(GetCurrentProcess(), 0);

}


static struct {
    int id, band, ctx, used;
} g_nt_seen[8];

static int g_ntNext;

 
// restart countdown bands: 3 = more than 5 min, 2 = 2-5 min, 1 = 1-2 min, 0 = under 1 min; a notice is shown again when its band drops
static int NtBand(int sec)
{
    return sec > 300 ? 3 : sec > 120 ? 2 : sec > 60 ? 1 : 0;
}


 
static void NtApply(const char *rep)
{
    
const char *t = rep;
    
while (*t && *t != '\n')
        t++;
    
int ctx = MatchSubstate() == 6;
    
int pl = NtPl();
    
while (*t) {
        
if (*t == '\n')
            t++;
        
char line[400];
        
const char *e = strchr(t, '\n');
        
size_t ln = e ? (size_t) (e - t) : strlen(t);
        
if (ln >= sizeof line)
            ln = sizeof line - 1;
        
memcpy(line, t, ln);
        line[ln] = 0;
        
t = e ? e : t + ln;
        
char *p1 = strchr(line, '|'), *p2 =
            p1 ? strchr(p1 + 1, '|') : NULL, *p3 =
            p2 ? strchr(p2 + 1, '|') : NULL;
        
if (!p3)
            continue;
        
*p1 = *p2 = *p3 = 0;
        
int id = atoi(line), param = atoi(p2 + 1);
        
char k = p1[1];
        
const char *text = p3 + 1;
        
if (k == 'u' && ctx && !g_ntCloseAt && !NtUpdCloseOff()) {
            
g_ntCloseAt = GetTickCount64() + NT_UPD_CLOSE_S * 1000ULL;
            
Xlog
                ("notice: an update is out while we play an older build in a match - closing the game in %d s",
                 NT_UPD_CLOSE_S);
            
NtUpdCloseToast(NT_UPD_CLOSE_S);
            
continue;
        
}
        
int slot = -1;
        
for (int i = 0; i < 8; i++)
            if (g_nt_seen[i].used && g_nt_seen[i].id == id) {
                slot = i;
                break;
            }
        
int band = k == 'r' ? NtBand(param) : 0;
        
int show = 0;
        
if (slot < 0) {
            slot = g_ntNext++ & 7;
            g_nt_seen[slot].used = 1;
            g_nt_seen[slot].id = id;
            g_nt_seen[slot].band = 99;
            g_nt_seen[slot].ctx = -1;
            show = 1;
        }
        
        else if (k == 'r')
            show = band < g_nt_seen[slot].band;
        
        else
            show = 0;           // update / text / shutting down: once per id (the queued toast carries over menu -> match)
        if (!show)
            continue;
        
g_nt_seen[slot].band = band;
        g_nt_seen[slot].ctx = ctx;
        
char m[300];
        
NtRender(k, param, text, pl, m, sizeof m);
        
if (!m[0])
            continue;
        
if (k == 'p' && MsgIsKick(m) && KnSet(m)) {
            Xlog("notice: kick line -> kick notice box (kicknote.c)");
            continue;
        }                       // (the box on the MP menu; a duplicate of the 0x11 text is dropped there)
        if (k == 'p' && MsgIsKick(m) && g_kickMs
            && GetTickCount64() - g_kickMs < 120000ULL) {
            Xlog("notice: kick line already shown through the session note - skipped");
            continue;
        }                       // admin kick: the 0x11 toast came first
        if (k == 'p') {
            
Fr13NoteSeen(m);
            
if (MatchSubstate() == 6) {       // a running match: kept until it is over (nt_tick), not lost
                int dup = 0;
                
for (int q = 0; q < g_ntPqn; q++)
                    if (!strcmp(g_ntPq[q], m))
                        dup = 1;
                
if (!dup) {
                    
if (g_ntPqn >= 6) {
                        memmove(g_ntPq[0], g_ntPq[1],
                                sizeof g_ntPq[0] * 5);
                        memmove(&g_ntPqs[0], &g_ntPqs[1],
                                sizeof g_ntPqs[0] * 5);
                        g_ntPqn = 5;
                    }
                    
snprintf(g_ntPq[g_ntPqn], sizeof g_ntPq[0], "%s",
                              m);
                    g_ntPqs[g_ntPqn] =
                        param >= 2 ? (float) (param >
                                              600 ? 600 : param) : 8.0f;
                    g_ntPqn++;
                    
Xlog
                        ("notice: friends line held until the match is over (%d waiting): \"%s\"",
                         g_ntPqn, m);
                
}
                
continue;
            
}
        
}
        
Xlog("notice: kind %c id %d param %d -> \"%s\"", k, id, param, m);
        
float nsecs = (k == 't' || k == 'p') && param >= 2 ? (float) (param > 600 ? 600 : param) : 8.0f;       // free text: the admin's seconds on screen (param)
        if (k == 'p' && Fr13ToastLine(m, nsecs))
            return;             // the friend / party card (friends13.c)
        if (!NtfPush(NTF_SERVER, m, nsecs))
            MsgTag(m, nsecs, 0);
        
return;                // one at a time: the next one is shown by the next poll
    }

}


 
static void NtTick(void)
{
    
static uint64_t next_ask;
    
static char tok[65];
    
if (g_nt_off < 0) {
        
FILE * f = ExeSideOpen("t2r-no-notice.txt", "r");
        
g_nt_off = f != NULL;
        
if (f)
            fclose(f);
        
Xlog("notice: %s",
              g_nt_off ? "off (t2r-no-notice.txt)" :
              "on - server notices polled every 6 s");
    
}
    
if (g_nt_off)
        return;
    
NtUpdCloseTick();
    
if (g_ntPqn && MatchSubstate() != 6) {   // the match is over: the held friends / party lines
        for (int q = 0; q < g_ntPqn; q++)
            if (!Fr13ToastLine(g_ntPq[q], g_ntPqs[q])
                && !NtfPush(NTF_SERVER, g_ntPq[q], g_ntPqs[q]))
                MsgTag(g_ntPq[q], g_ntPqs[q], 0);
        
Xlog("notice: %d held friends line(s) shown after the match",
              g_ntPqn);
        
g_ntPqn = 0;
    
}
    
LONG have = InterlockedExchange(&g_nt_have, 0);
    
static int streak;         // polls in a row that got no notice frame (outage or older server): the poll spreads out, 6 s -> 30 s
    uint64_t now = GetTickCount64();
    
if (have == 1) {
        g_nt_fail = 0;
        g_nt_ok_once = 1;
        streak = 0;
        NtApply(g_ntRep);
    }
    
    else if (have == 2 || have == 3) {
        
streak++;
        
            // 'older server' = reachable, never answers 0xBA. A dead tunnel / restarting daemon looks the same on one socket (connect ok, no reply),
            // so it only counts while the daemon has answered something else lately (outage.h srv_alive_within) - an outage no longer ends notices for the session.
            if (have == 2 && SrvAliveWithin(60000) && ++g_nt_fail >= 3
                && !g_nt_ok_once) {
            g_nt_off = 1;
            Xlog("notice: the server does not answer 0xBA (older server) - not asking again");
            return;
        }
    
}
    
if (now < next_ask || g_nt_busy)
        return;
    
if (!tok[0] && !ClanToken(tok)) {
        tok[0] = 0;
        return;
    }
    
static uint8_t req[67];
    
memcpy(req, tok, 64);
    
req[64] = 1;
    req[65] = (uint8_t) NtPl();
    req[66] = Fr13Enabled()? 1 : 0;    // byte 66: this client has the in-game FRIENDS panel (the server words request / invite lines "press P")
    next_ask = now + 6000;
    
if (streak >= 2)
        next_ask = now + T2rBackoffMs(streak - 1, 6000, 30000);
    
InterlockedExchange(&g_nt_busy, 1);
    
HANDLE h = CreateThread(NULL, 0, NtWorker, req, 0, NULL);
    
if (!h) {
        InterlockedExchange(&g_nt_busy, 0);
        return;
    }
    
CloseHandle(h);

}


