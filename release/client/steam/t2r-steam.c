// t2r-steam: Steam helper for Twelve Weeks.
//
//   t2r-steam.exe <out_file> [game_exe=tlou-ii.exe] [--server host:port] [--token file]
//                 [--port N] [--fake-id N] [--log file]
//
// Initializes the Steam API as Spacewar (appid 480) using the genuine Valve
// steam_api64.dll that sits NEXT TO THIS EXE (never the game's copy, which may be
// an emulator on non-Steam installs), writes "<persona name>\n<steamid64>\n" to
// <out_file>, then stays alive while the game runs so Steam shows the player as
// "Playing Spacewar". Exits when the game exits (or if it never starts).
// If Steam isn't running it writes an empty file and exits with code 2.
//
// Steam P2P gameplay (with --server/--token/--port): once the game runs, the helper
// registers with the server (0xF0 [token 64][steamid u64][name len][name][u16 port]).
// When a match forms with every player on Steam, the server sends 0xF2
// [u32 sid][u8 own slot][u8 n][n x (u8 slot, u64 steamid)] and points the game's
// gameplay relay at 127.0.0.1:<port>. The helper then IS that relay, talking the
// same protocol as the server relay:
//   game -> [00][u32 sid][u8 slot]                   hello  -> reply [01]
//   game -> [02][u32 sid][u8 src][u8 dst][payload]   data   -> Steam message to dst
//   Steam message (same bytes)                       -> sent to the game unchanged
// Steam picks a direct connection or a Valve relay near both players.
// --fake-id N (testing on one PC): no Steam; "steamid" N = UDP port 127.0.0.1:N.
//
// Build (MSYS2 UCRT64): gcc -O2 -s -static -o t2r-steam.exe t2r-steam.c -lws2_32 -lwinmm
#include "t2r-steam.h"
static void xlog(const char *fmt, ...)
{
    if (!g_log)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond,
            t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static int game_running(const char *exe)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32 pe = {.dwSize = sizeof pe };
    int found = 0;
    for (BOOL ok = Process32First(snap, &pe); ok;
         ok = Process32Next(snap, &pe)) {
        if (_stricmp(pe.szExeFile, exe) == 0) {
            found = 1;
            break;
        }
    }
    CloseHandle(snap);
    return found;
}

static int alive_file_fresh(void)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(g_alive_file, GetFileExInfoStandard, &a))
        return 0;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER n = {.LowPart = now.dwLowDateTime,.HighPart =
            now.dwHighDateTime }, w = {.LowPart =
            a.ftLastWriteTime.dwLowDateTime,.HighPart =
            a.ftLastWriteTime.dwHighDateTime };
    return n.QuadPart < w.QuadPart + (ULONGLONG) GAME_GONE_MS *10000;
}

static int game_alive(const char *exe)
{
    if (g_alive_file ? alive_file_fresh() : game_running(exe)) {
        if (!g_game_seen)
            xlog("game %s running", exe);
        g_game_seen = GetTickCount();
        return 1;
    }
    return g_game_seen && GetTickCount() - g_game_seen < GAME_GONE_MS;
}

static void write_out(const char *path, const char *name, uint64_t id)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    if (name)
        fprintf(f, "%s\n%llu\n", name, (unsigned long long) id);
    fclose(f);
}

static double qpc_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * (double) t.QuadPart / (double) g_qpf.QuadPart;
}

// A hidden helper next to a game that uses every core: Windows 11 puts such a process in EcoQoS (slow cores,
// timer resolution request ignored => Sleep(1) = 15.6 ms) and the game's threads outrank it. Opt out.
static void pump_priority(void)
{
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);        // was HIGHEST - it preempted game threads ~1000x/s (frame pacing); still above the game (normal), so it is never starved
    typedef BOOL(WINAPI * fn_SPI) (HANDLE, int, LPVOID, DWORD);
    fn_SPI spi =
        (fn_SPI) GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                "SetProcessInformation");
    if (spi) {
        struct {
            ULONG Version, ControlMask, StateMask;
        } t = { 1, 1 | 4, 0 };  // EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION: throttling OFF
        BOOL ok =
            spi(GetCurrentProcess(), 4 /* ProcessPowerThrottling */ , &t,
                sizeof t);
        xlog("pump priority: above normal, power throttling off (%d)", ok);
    }
}

static int peer_send(uint64_t id, const void *data, int len)
{
    if (g_fake) {
        struct sockaddr_in to = {.sin_family = AF_INET,.sin_port =
                htons((u_short) id) };
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(g_fake_sock, data, len, 0, (struct sockaddr *) &to,
               sizeof to);
        return 1;
    }
    uint8_t ident[136] = { 0 };
    IdentSet(ident, id);
    return SendToUser(g_msgs, ident, data, (uint32_t) len, SEND_FLAGS, CHANNEL);        // EResult, 1 = OK
}

static void from_peer(uint64_t id, const uint8_t *data, int len,
                      SOCKET game_sock)
{
    for (int i = 0; i < 16; i++)
        if (M.peer[i] == id && !M.seen[i]) {
            M.seen[i] = 1;
            xlog("peer slot %d (%llu) connected", i,
                 (unsigned long long) id);
        }
    if (len < 1 || data[0] == PUNCH)
        return;
    if (data[0] == 0x05) {      // direct ext message (see above)
        uint32_t sid;
        uint8_t src;
        if (len < 7 || !g_have_ext || !M.active) {
            g_dir_bad++;
            return;
        }
        memcpy(&sid, data + 1, 4);
        src = data[5];
        if (sid != M.sid || src >= 16 || M.peer[src] != id) {
            g_dir_bad++;
            return;
        }
        uint8_t o[2048];
        if (len - 4 > (int) sizeof o)
            return;
        o[0] = 0x05;
        o[1] = src;
        memcpy(o + 2, data + 6, (size_t) (len - 6));
        sendto(game_sock, (const char *) o, len - 4, 0,
               (struct sockaddr *) &g_ext, sizeof g_ext);
        g_dir_in++;
        return;
    }
    if (data[0] == 0x02) {
        DWORD t = GetTickCount();
        for (int i = 0; i < 16; i++)
            if (M.peer[i] == id && P[i].id == id) {
                if (!P[i].rx_run || t - P[i].last_rx > 25000)
                    P[i].rx_run = 0;
                P[i].rx_run++;
                P[i].rx_n++;
                P[i].last_rx = t;
                P[i].tx_since_rx = 0;
                P[i].resets_nf = 0;
            }
        g_last_rx_any = t;
        g_tx_since_any_rx = 0;
    }
    if (data[0] == 0x02 && g_have_game) {
        sendto(game_sock, (const char *) data, len, 0,
               (struct sockaddr *) &g_game, sizeof g_game);
        g_stats_in++;
    }
}

static void poll_peers(SOCKET game_sock)
{
    if (g_fake) {
        uint8_t buf[2048];
        struct sockaddr_in from;
        int fl = sizeof from;
        for (;;) {
            int n =
                recvfrom(g_fake_sock, (char *) buf, sizeof buf, 0,
                         (struct sockaddr *) &from, &fl);
            if (n <= 0)
                break;
            from_peer(ntohs(from.sin_port), buf, n, game_sock);
            fl = sizeof from;
        }
        return;
    }
    void *msgs[64];
    int n, total = 0;
    while ((n = RecvOnChannel(g_msgs, CHANNEL, msgs, 64)) > 0) {
        total += n;
        if (total > g_max_drain_steam)
            g_max_drain_steam = total;
        for (int i = 0; i < n; i++) {
            uint8_t *m = msgs[i];
            const uint8_t *data = *(const uint8_t **) (m + 0x00);
            int len = *(int *) (m + 0x08);
            uint64_t id = IdentGet(m + 0x10);
            from_peer(id, data, len, game_sock);
            MsgRelease(m);
        }
    }
}

// %TEMP%       2r-steam-status.txt = "<1|0> <unix time>": 1 while Steam is up and the server link
// is registered. Rewritten every keepalive (20 s); the game's ext treats a stale file as 0.
static void write_status(int up)
{
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (!n || n > MAX_PATH - 32)
        return;
    strcat(path, "t2r-steam-status.txt");
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%d %lld\n", up, (long long) time(NULL));
    fclose(f);
}

static void srv_send(uint8_t op, const uint8_t * pl, int len);
static void srv_frame(uint8_t op, const uint8_t *pl, int len)
{
    if (!g_srv_ok) {
        g_srv_ok = 1;
        write_status(1);
    }                           // any complete frame = a real server round trip
    if (op == 0xF2 && len >= 6) {
        memset(&M, 0, sizeof M);
        memcpy(&M.sid, pl, 4);
        M.own = pl[4];
        int n = pl[5];
        for (int i = 0; i < n && 6 + i * 9 + 9 <= len; i++) {
            uint8_t slot = pl[6 + i * 9];
            uint64_t id;
            memcpy(&id, pl + 6 + i * 9 + 1, 8);
            if (slot < 16 && slot != M.own)
                M.peer[slot] = id;
        }
        for (int i = 0; i < 16; i++)
            if (P[i].id != M.peer[i]) {
                memset(&P[i], 0, sizeof P[i]);
                P[i].id = M.peer[i];
            }                   // a JIP F2 keeps the running counters
        M.active = 1;
        xlog("match sid %08x (previous %08x), own slot %d, %d players",
             M.sid, g_prev_sid, M.own, n);
        if (!g_no_reset && CloseSession && !g_fake && M.sid != g_prev_sid) {    // rule E: a new match never inherits a session of the old one
            for (int i = 0; i < 16; i++)
                if (M.peer[i]) {
                    uint8_t id[136] = { 0 };
                    IdentSet(id, M.peer[i]);
                    int closed = CloseSession(g_msgs, id);
                    uint8_t b = PUNCH;
                    peer_send(M.peer[i], &b, 1);
                    AcceptSession(g_msgs, id);
                    xlog("peer slot %d (%llu): new match - closed + reopened its Steam session (close rc %d)", i, (unsigned long long) M.peer[i], closed);
                }
        }
        g_prev_sid = M.sid;
    }
    if (op == 0xF5 && len > 8 && len < 1100 && g_utils && ParseLoc
        && EstPing) {
        uint8_t loc[512], out[12];
        char str[1100];
        memcpy(str, pl + 8, len - 8);
        str[len - 8] = 0;
        int32_t ms =
            ParseLoc(g_utils, str, loc) ? EstPing(g_utils, loc) : -1;
        memcpy(out, pl, 8);
        memcpy(out + 8, &ms, 4);
        srv_send(0xF6, out, 12);
    }
}

static void srv_poll(void)
{
    if (g_srv == INVALID_SOCKET)
        return;
    int n =
        recv(g_srv, (char *) g_srvbuf + g_srvlen,
             sizeof g_srvbuf - g_srvlen, 0);
    if (n == 0 || (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) {
        xlog("server link closed");
        write_status(0);
        g_srv_ok = 0;
        closesocket(g_srv);
        g_srv = INVALID_SOCKET;
        g_srvlen = 0;
        return;
    }
    if (n < 0)
        return;
    g_srvlen += n;
    for (;;) {
        if (g_srvskip > 0) {    // inside an oversized frame: discard it by its length, never parse its bytes
            int d = g_srvlen < g_srvskip ? g_srvlen : g_srvskip;
            memmove(g_srvbuf, g_srvbuf + d, g_srvlen - d);
            g_srvlen -= d;
            g_srvskip -= d;
            if (g_srvskip > 0)
                break;
        }
        if (g_srvlen < 2)
            break;
        int ln = g_srvbuf[0] | g_srvbuf[1] << 8;
        if (ln == 0) {
            memmove(g_srvbuf, g_srvbuf + 2, g_srvlen - 2);
            g_srvlen -= 2;
            continue;
        }
        if (ln + 2 > (int) sizeof g_srvbuf) {
            g_srvskip = ln + 2;
            continue;
        }                       // too big to buffer: skip the whole frame (stream stays in sync)
        if (g_srvlen < ln + 2)
            break;
        srv_frame(g_srvbuf[2], g_srvbuf + 3, ln - 1);
        memmove(g_srvbuf, g_srvbuf + 2 + ln, g_srvlen - 2 - ln);
        g_srvlen -= 2 + ln;
    }
}

static void srv_send(uint8_t op, const uint8_t *pl, int len)
{
    uint8_t f[1200];
    if (g_srv == INVALID_SOCKET || len + 3 > (int) sizeof f)
        return;
    f[0] = (uint8_t) (len + 1);
    f[1] = (uint8_t) ((len + 1) >> 8);
    f[2] = op;
    memcpy(f + 3, pl, len);
    send(g_srv, (const char *) f, len + 3, 0);
}


static DWORD WINAPI srv_connect_thread(LPVOID unused)
{
    (void) unused;
    struct addrinfo hints = {.ai_family = AF_INET,.ai_socktype =
            SOCK_STREAM }, *ai = NULL;
    SOCKET s = INVALID_SOCKET;
    if (!getaddrinfo(g_cs_host, g_cs_port, &hints, &ai) && ai) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET) {
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            int r = connect(s, ai->ai_addr, (int) ai->ai_addrlen);
            int ok = r == 0;
            if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set w, e;
                FD_ZERO(&w);
                FD_ZERO(&e);
                FD_SET(s, &w);
                FD_SET(s, &e);
                struct timeval tv = { 4, 0 };
                ok = select(0, NULL, &w, &e, &tv) > 0 && FD_ISSET(s, &w)
                    && !FD_ISSET(s, &e);
            }
            if (!ok) {
                closesocket(s);
                s = INVALID_SOCKET;
            }
        }
        freeaddrinfo(ai);
    }
    g_cs_new = s;
    InterlockedExchange(&g_cs_busy, 0);
    return 0;
}

// starts a connect attempt (returns at once); srv_adopt() takes the result
static void srv_connect_start(const char *server)
{
    if (g_cs_busy)
        return;
    char host[256];
    snprintf(host, sizeof host, "%s", server);
    char *colon = strrchr(host, ':');
    if (!colon)
        return;
    *colon = 0;
    snprintf(g_cs_host, sizeof g_cs_host, "%s", host);
    snprintf(g_cs_port, sizeof g_cs_port, "%s", colon + 1);
    InterlockedExchange(&g_cs_busy, 1);
    HANDLE h = CreateThread(NULL, 0, srv_connect_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
    else
        InterlockedExchange(&g_cs_busy, 0);
}

static int srv_adopt(const char *server, const char *token,
                     uint64_t steamid, const char *name, int port)
{
    SOCKET s = g_cs_new;
    if (s == INVALID_SOCKET)
        return 0;
    g_cs_new = INVALID_SOCKET;
    if (g_srv != INVALID_SOCKET) {
        closesocket(s);
        return 0;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *) &one,
               sizeof one);
    g_srv = s;
    g_srvlen = 0;
    g_srvskip = 0;
    g_srv_ok = 0;
    g_srv_at = GetTickCount();
    uint8_t pl[64 + 8 + 1 + 255 + 2];
    int nl = (int) strlen(name);
    if (nl > 255)
        nl = 255;
    memcpy(pl, token, 64);
    memcpy(pl + 64, &steamid, 8);
    pl[72] = (uint8_t) nl;
    memcpy(pl + 73, name, nl);
    pl[73 + nl] = (uint8_t) port;
    pl[74 + nl] = (uint8_t) (port >> 8);
    srv_send(0xF0, pl, 75 + nl);
    xlog("registered with %s (steamid %llu, port %d)", server,
         (unsigned long long) steamid, port);
    return 1;
}

// token = the 64-hex "token" value in the game's tlou-ii-mp.json
static int read_token(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strstr(buf, "\"token\"");
    if (!p)
        return 0;
    p = strchr(p + 7, '"');
    if (!p)
        return 0;
    p++;
    for (int i = 0; i < 64; i++) {
        char c = p[i];
        if (!
            ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
             || (c >= 'A' && c <= 'F')))
            return 0;
        out[i] = c;
    }
    out[64] = 0;
    return 1;
}

// Close + re-open the Steam session to one peer (fresh connection, fresh send buffer and congestion state; the next send re-creates it because of
// AutoRestartBrokenSession, the other end sees the old connection close and does the same). Run from the 1 s tick.
//  A: the peer carried gameplay (>= 30 packets), then nothing for 2.5 s while we sent >= 8 KB to it (the engine gives up on a silent host at ~4 s).
//  B: nothing at all from ANYONE for 3.5 s since the game's hello although we sent >= 20 KB (a re-join into a wedged session): reopen every peer we
//     sent >= 8 KB to, at most 3 times each with 4 s between. A peer that is merely loading sends us nothing, but then someone else usually does; a
//     host alone with one loading joiner costs it up to 3 harmless re-opens.
static void peer_watch(DWORD now)
{
    if (g_fake || !CloseSession || !M.active)
        return;
    int did = 0;
    for (int i = 0; i < 16; i++)
        if (M.peer[i] && P[i].id == M.peer[i] && P[i].rx_run
            && now - P[i].last_rx > 25000)
            P[i].rx_run = 0;
    for (int i = 0; i < 16; i++) {
        if (!M.peer[i] || P[i].id != M.peer[i])
            continue;
        if (now - P[i].last_reset < 2500)
            continue;
        DWORD age = now - P[i].last_rx;
        int why = 0;
        if (P[i].rx_run >= 30 && age >= 2500 && age <= 25000
            && P[i].tx_since_rx >= 8000)
            why = 1;
        else if (!P[i].rx_run && g_tx_since_any_rx >= 20000
                 && now - g_last_rx_any >= 3500 && P[i].tx_since_rx >= 8000
                 && P[i].resets_nf < 3 && now - P[i].last_reset >= 4000)
            why = 2;
        if (!why && !g_no_reset && g_hello_at && P[i].resets_nf < 3
            && !P[i].rx_n) {
            if (P[i].tx_n >= 3 && P[i].first_tx
                && now - P[i].first_tx >= 2500
                && now - P[i].last_reset >= 3000)
                why = 3;
            else if (!P[i].tx_n && now - g_hello_at >= 6000
                     && now - g_last_rx_any < 3000 && P[i].resets_nf < 2
                     && now - P[i].last_reset >= 6000)
                why = 4;
        }
        if (!why)
            continue;
        uint8_t id[136] = { 0 };
        IdentSet(id, M.peer[i]);
        // In one live match the host's gameplay stopped at 20:25:24, rule A re-opened the session at :28 (right),
        // then rule B closed it again at :32, :36 and :40 while Steam was still re-connecting it (state 2, ping -1, every send refused with 25) - a re-connect
        // over the relays takes ~5 s, B fires every 4 s, so each close threw the handshake away. It connected at :45, 5 s after B's 3rd and last close, one
        // second before the game's 15 s host timeout ended the match for the joiner. A session that is connecting / finding a route is never closed by A-D;
        // one stuck there is rule E2's (6 s, below).
        if (SessionInfo) {
            uint8_t info[1024] = { 0 }, st[128] = { 0 };
            int state = SessionInfo(g_msgs, id, info, st);
            if (state == 1 || state == 2) {
                if (!P[i].wait_logged) {
                    P[i].wait_logged = 1;
                    xlog("peer slot %d (%llu): rule %c wants a re-open, but its Steam session is still connecting (state %d) - left alone (rule E2 takes it after 6 s)", i, (unsigned long long) M.peer[i], "?ABCD"[why], state);
                }
                continue;
            }
        }
        P[i].wait_logged = 0;
        if (why >= 3) {
            P[i].first_tx = now;
            P[i].resets_nf++;
        }
        int closed = CloseSession(g_msgs, id);
        uint8_t b = PUNCH;
        peer_send(M.peer[i], &b, 1);    // opens the new session at once
        AcceptSession(g_msgs, id);
        P[i].last_reset = now;
        P[i].resets++;
        if (why == 2)
            P[i].resets_nf++;
        else
            P[i].rx_run = 0;
        P[i].tx_since_rx = 0;
        did = 1;
        xlog("peer slot %d (%llu): %s - closed + reopened its Steam session (reset %u, close rc %d; sent %llu / got %llu gameplay packets, %llu refused, last result %d)", i, (unsigned long long) M.peer[i], why == 1 ? "gameplay stopped arriving from it while we keep sending (was flowing)" : why == 3 ? "HALF-OPEN: we sent it gameplay since the hello, not one packet came back" : why == 4 ? "HALF-OPEN: nothing either way with it since the hello while other peers flow" : "nothing from anyone since the game started although we send", P[i].resets, closed, (unsigned long long) P[i].tx_n, (unsigned long long) P[i].rx_n, (unsigned long long) P[i].fail_n, P[i].last_res);
    }
    // E2 (seen between two players: both helpers held the session in state 1 "connecting" for 70-80 s - the game gave up at 30 s with
    //  "Could not reach host (timed out)" / a lone host - while the same helper connected a third player in 4 s): a session still connecting / finding a route
    //  after 6 s is closed and re-opened (fresh signaling), at most 4 times, 6 s apart; its end reason / debug text is logged. Steam-level, not the game-level
    //  C/D rules (those cascaded on loading peers), so on by default; t2r-no-conn-reset.txt = off.
    static int cn_off = -1;
    static DWORD cn_chk;
    if (cn_off < 0 || now - cn_chk > 5000) {
        cn_chk = now;
        cn_off =
            GetFileAttributesA("t2r-no-conn-reset.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    for (int i = 0; SessionInfo && !cn_off && i < 16; i++) {
        if (!M.peer[i] || P[i].id != M.peer[i])
            continue;
        uint8_t id[136] = { 0 }, info[1024] = { 0 }, st[128] = { 0 };
        IdentSet(id, M.peer[i]);
        int state = SessionInfo(g_msgs, id, info, st);
        if (state != 1 && state != 2) {
            P[i].conn_since = 0;
            continue;
        }
        if (!P[i].conn_since) {
            P[i].conn_since = now ? now : 1;
            continue;
        }
        if (now - P[i].conn_since < 6000 || P[i].resets_cn >= 4
            || now - P[i].last_reset < 6000)
            continue;
        DWORD cdur = now - P[i].conn_since;
        char dbg[129] = { 0 }, desc[129] = { 0 };
        memcpy(dbg, info + 184, 128);
        memcpy(desc, info + 312, 128);
        int closed = CloseSession(g_msgs, id);
        uint8_t b = PUNCH;
        peer_send(M.peer[i], &b, 1);
        AcceptSession(g_msgs, id);
        if (P[i].resets_cn == 1 && !g_ice_off && g_set_cfg) {   // 2nd stall: ICE (NAT punch) is the usual hang - new sessions go over Valve's relays only
            g_ice_off = 1;
            int rc =
                g_set_cfg(g_utils, 104 /* P2P_Transport_ICE_Enable */ , 0);
            xlog("steam: ICE off for new sessions (relay only) after a 2nd stuck connect (rc %d)", rc);
        }
        P[i].last_reset = now;
        P[i].resets++;
        P[i].resets_cn++;
        P[i].conn_since = 0;
        xlog("peer slot %d (%llu): STUCK CONNECTING - state %d for %lu ms, closed + reopened its Steam session (re-open %u of 4, close rc %d) | end reason %d, debug \"%s\", connection \"%s\", relay pop %08x remote pop %08x", i, (unsigned long long) M.peer[i], state, (unsigned long) cdur, P[i].resets_cn, closed, *(int *) (info + 180), dbg, desc, *(uint32_t *) (info + 168), *(uint32_t *) (info + 172));
    }
    if (did)
        g_last_rx_any = now;    // B waits 3.5 s again before the next round
}

// ---------------------------------------------------------------- main
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: t2r-steam.exe <out_file> [game_exe] [--server host:port] "
                "[--token file] [--port N] [--fake-id N] [--log file]\n");
        return 1;
    }
    const char *out = argv[1];
    const char *game = "tlou-ii.exe", *server = NULL, *token_file =
        NULL, *log_file = NULL;
    int port = 0;
    uint64_t fake_id = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--server") && i + 1 < argc)
            server = argv[++i];
        else if (!strcmp(argv[i], "--token") && i + 1 < argc)
            token_file = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fake-id") && i + 1 < argc)
            fake_id = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)
            log_file = argv[++i];
        else if (!strcmp(argv[i], "--alive-file") && i + 1 < argc)
            g_alive_file = argv[++i];
        else
            game = argv[i];
    }
    g_fake = fake_id != 0;
    {
        char *e = getenv("T2R_PEER_RESET");
        if (e && *e && *e != '0')
            g_no_reset = 0;
    }

    // Work from our own folder: steam_appid.txt (480) + genuine steam_api64.dll live here.
    char dir[MAX_PATH], dll[MAX_PATH + 32];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char *slash = strrchr(dir, '\\');
    if (slash)
        *slash = 0;
    if (log_file)
        g_log = fopen(log_file, "w");
    SetCurrentDirectoryA(dir);
    if (GetFileAttributesA("t2r-peer-reset.txt") !=
        INVALID_FILE_ATTRIBUTES)
        g_no_reset = 0;
    xlog("peer session reset rules C/D/E (half-open, new match): %s",
         g_no_reset ? "off (default)" : "ON (opt-in)");
    SetEnvironmentVariableA("SteamAppId", "480");
    SetEnvironmentVariableA("SteamGameId", "480");
    snprintf(dll, sizeof dll, "%s\\steam_api64.dll", dir);

    fn_Void Shutdown = NULL, RunCallbacks = NULL;
    uint64_t steamid = fake_id;
    const char *name = "fake";
    if (!g_fake) {
        HMODULE api =
            LoadLibraryExA(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!api) {
            write_out(out, NULL, 0);
            return 3;
        }
#define SYM(t, n) (t)GetProcAddress(api, n)
        fn_Init Init = SYM(fn_Init, "SteamAPI_Init");
        Shutdown = SYM(fn_Void, "SteamAPI_Shutdown");
        RunCallbacks = SYM(fn_Void, "SteamAPI_RunCallbacks");
        fn_Iface Friends = SYM(fn_Iface, "SteamAPI_SteamFriends_v017");
        fn_Iface User = SYM(fn_Iface, "SteamAPI_SteamUser_v023");
        fn_GetPersonaName GetName =
            SYM(fn_GetPersonaName,
                "SteamAPI_ISteamFriends_GetPersonaName");
        fn_GetSteamID GetID =
            SYM(fn_GetSteamID, "SteamAPI_ISteamUser_GetSteamID");
        fn_Iface Msgs =
            SYM(fn_Iface,
                "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
        fn_Iface Utils =
            SYM(fn_Iface, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
        fn_InitRelay InitRelay =
            SYM(fn_InitRelay,
                "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
        IdentSet =
            SYM(fn_IdentSet,
                "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
        IdentGet =
            SYM(fn_IdentGet,
                "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
        SendToUser =
            SYM(fn_SendToUser,
                "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
        RecvOnChannel =
            SYM(fn_RecvOnChannel,
                "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
        AcceptSession =
            SYM(fn_AcceptSession,
                "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
        CloseSession =
            SYM(fn_CloseSession,
                "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
        MsgRelease =
            SYM(fn_MsgRelease,
                "SteamAPI_SteamNetworkingMessage_t_Release");
        if (!Init || !Shutdown || !RunCallbacks || !Friends || !User
            || !GetName || !GetID) {
            write_out(out, NULL, 0);
            return 3;
        }
        if (!Init()) {
            write_out(out, NULL, 0);
            return 2;
        }                       // Steam not running
        name = GetName(Friends());
        steamid = GetID(User());
        write_out(out, name, steamid);
        {                       // which app Steam really runs us as (Linux / Proton: the Linux steamclient reads the unix environment - must be 480 like every
            // other helper, else P2P sessions with them never connect)
            typedef void *(*fn_Utils)(void);
            typedef uint32_t(*fn_AppID) (void *);
            fn_Utils SUtils = SYM(fn_Utils, "SteamAPI_SteamUtils_v010");
            fn_AppID GetAppID =
                SYM(fn_AppID, "SteamAPI_ISteamUtils_GetAppID");
            if (SUtils && GetAppID && SUtils())
                xlog("steam app id %u%s", GetAppID(SUtils()),
                     GetAppID(SUtils()) ==
                     480 ? "" :
                     " - NOT 480: P2P with other players will not connect");
        }
        if (Msgs && Utils && InitRelay && IdentSet && IdentGet
            && SendToUser && RecvOnChannel && AcceptSession
            && MsgRelease) {
            g_msgs = Msgs();
            InitRelay(Utils());
            g_utils = Utils();
            GetLocalLoc =
                SYM(fn_GetLocalLoc,
                    "SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation");
            LocToStr =
                SYM(fn_LocToStr,
                    "SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString");
            ParseLoc =
                SYM(fn_ParseLoc,
                    "SteamAPI_ISteamNetworkingUtils_ParsePingLocationString");
            EstPing = SYM(fn_EstPing, "SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost");  // warm up Valve relay routes before the match
            // Steam's default send rate cap is below what the host sends (~450 packets/s to
            // each peer), so packets queued in a 512 KB buffer and arrived seconds late.
            // Raise the cap and keep the buffer small: a late gameplay packet is worse than
            // a dropped one (unreliable sends over the limit are dropped).
            fn_SetCfgInt SetCfg =
                SYM(fn_SetCfgInt,
                    "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32");
            g_set_cfg = (int (*)(void *, int, int)) SetCfg;
            if (SetCfg) {
                int a = SetCfg(Utils(), 10 /* SendRateMin */ , 512 * 1024);
                int b =
                    SetCfg(Utils(), 11 /* SendRateMax */ ,
                           4 * 1024 * 1024);
                int c =
                    SetCfg(Utils(), 9 /* SendBufferSize */ , 32 * 1024);
                xlog("send rate 512 KB/s..4 MB/s, buffer 32 KB (%d %d %d)",
                     a, b, c);
            }
            SessionInfo =
                SYM(fn_SessionInfo,
                    "SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo");
        }
    } else {
        write_out(out, name, steamid);
    }
    int p2p = server && token_file && port && (g_fake || g_msgs);

    // Wait for the game; Steam shows "Playing Spacewar" from here on.
    DWORD waited = 0;
    DWORD wait_max = g_alive_file ? 1800000 : 180000;   // Linux external: the player starts the game after t2r-linux.py
    while (!game_alive(game) && waited < wait_max) {    // wait up to 3 min for launch
        if (RunCallbacks)
            RunCallbacks();
        Sleep(500);
        waited += 500;
    }
    if (!g_game_seen)
        xlog("game %s not seen within %lu min",
             g_alive_file ? g_alive_file : game,
             (unsigned long) (wait_max / 60000));
    xlog("steam id %llu, server link %s", (unsigned long long) steamid,
         p2p ? "on" : "off (no server/token/port or Steam networking)");
    if (!p2p) {
        while (game_alive(game)) {
            if (RunCallbacks)
                RunCallbacks();
            Sleep(1000);
        }
        if (Shutdown)
            Shutdown();
        return 0;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    timeBeginPeriod(1);
    SOCKET gs = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in la = {.sin_family = AF_INET,.sin_port =
            htons((u_short) port) };
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(gs, (struct sockaddr *) &la, sizeof la)) {
        xlog("bind %d failed", port);
        p2p = 0;
    }
    u_long nb = 1;
    ioctlsocket(gs, FIONBIO, &nb);
    QueryPerformanceFrequency(&g_qpf);
    pump_priority();
    {                           // the game sends ~450 packets/s in bursts: room for seconds of them, not 64 KB
        int rb = 4 * 1024 * 1024, sb = 1024 * 1024, grb = 0, gsb = 0, l =
            sizeof grb;
        setsockopt(gs, SOL_SOCKET, SO_RCVBUF, (const char *) &rb,
                   sizeof rb);
        setsockopt(gs, SOL_SOCKET, SO_SNDBUF, (const char *) &sb,
                   sizeof sb);
        getsockopt(gs, SOL_SOCKET, SO_RCVBUF, (char *) &grb, &l);
        l = sizeof gsb;
        getsockopt(gs, SOL_SOCKET, SO_SNDBUF, (char *) &gsb, &l);
        xlog("loopback socket buffers: rcv %d snd %d bytes", grb, gsb);
    }
    if (g_fake) {
        g_fake_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        struct sockaddr_in fa = la;
        fa.sin_port = htons((u_short) fake_id);
        bind(g_fake_sock, (struct sockaddr *) &fa, sizeof fa);
        ioctlsocket(g_fake_sock, FIONBIO, &nb);
    }
    // Windows reports ICMP port-unreachable as a recv error on UDP; ignore it.
    BOOL f = FALSE;
    DWORD br;
    WSAIoctl(gs, _WSAIOW(IOC_VENDOR, 12), &f, sizeof f, NULL, 0, &br, NULL,
             NULL);
    if (g_fake)
        WSAIoctl(g_fake_sock, _WSAIOW(IOC_VENDOR, 12), &f, sizeof f, NULL,
                 0, &br, NULL, NULL);

    char token[65] = { 0 };
    DWORD last_loc = 0, last_conn = 0, last_ka = 0, last_slow =
        0, last_stats = 0;
    double last_loop_ms = qpc_ms();
    while (p2p) {
        DWORD now = GetTickCount();
        {
            double t = qpc_ms();
            if (t - last_loop_ms > g_max_gap_ms)
                g_max_gap_ms = t - last_loop_ms;
            last_loop_ms = t;
            g_loops++;
        }
        if (now - last_slow >= 1000) {
            last_slow = now;
            if (!game_alive(game))
                break;
            if (RunCallbacks)
                RunCallbacks();
            // the connect runs in a worker thread (never blocks the P2P pump), so it also retries while a match is running
            if (g_srv == INVALID_SOCKET && g_cs_new != INVALID_SOCKET
                && (token[0] || read_token(token_file, token)))
                srv_adopt(server, token, steamid, name, port);
            if (g_srv == INVALID_SOCKET && !g_cs_busy
                && now - last_conn >= 5000) {
                last_conn = now;
                if (token[0] || read_token(token_file, token))
                    srv_connect_start(server);
            }
            if (g_srv != INVALID_SOCKET && now - last_ka >= 20000) {
                last_ka = now;
                srv_send(0xF3, NULL, 0);
                if (g_srv_ok)
                    write_status(1);
            }
            if (g_srv != INVALID_SOCKET && !g_srv_ok
                && now - g_srv_at >= SRV_OK_MS) {
                g_srv_ok = 1;
                write_status(1);
            }                   // survived: an older daemon sends no ack
            if (g_srv != INVALID_SOCKET && g_utils && GetLocalLoc
                && LocToStr && now - last_loc >= 30000) {
                uint8_t loc[512];
                char str[1100] = { 0 };
                if (GetLocalLoc(g_utils, loc) >= 0) {
                    last_loc = now;
                    LocToStr(g_utils, loc, str, sizeof str);
                    srv_send(0xF4, (const uint8_t *) str,
                             (int) strlen(str));
                }
            }
            peer_watch(now);
            for (int i = 0; M.active && i < 16; i++)
                if (M.peer[i]) {
                    if (!M.seen[i]) {
                        uint8_t b = PUNCH;
                        peer_send(M.peer[i], &b, 1);
                    }
                    if (!g_fake) {
                        uint8_t id[136] = { 0 };
                        IdentSet(id, M.peer[i]);
                        AcceptSession(g_msgs, id);
                    }
                }
            if (now - last_stats >= 10000) {
                last_stats = now;
                if (g_stats_out || g_stats_in)
                    xlog("gameplay packets out %llu in %llu", g_stats_out,
                         g_stats_in);
                if (g_dir_out || g_dir_in || g_dir_bad)
                    xlog("direct ext packets out %llu in %llu refused %llu", g_dir_out, g_dir_in, g_dir_bad);
                xlog("pump: %.0f loops, longest gap between loops %.1f ms, most datagrams waiting at once: loopback %.0f / Steam %.0f, " "most bytes pending in the loopback buffer %lu", g_loops, g_max_gap_ms, g_max_drain_gs, g_max_drain_steam, (unsigned long) g_max_pending);
                g_max_gap_ms = g_max_drain_gs = g_max_drain_steam =
                    g_loops = 0;
                g_max_pending = 0;
                for (int i = 0; SessionInfo && M.active && i < 16; i++)
                    if (M.peer[i]) {
                        uint8_t id[136] = { 0 }, info[1024] =
                            { 0 }, st[128] = { 0 };
                        IdentSet(id, M.peer[i]);
                        int state = SessionInfo(g_msgs, id, info, st);
                        xlog("peer slot %d: state %d ping %d ms, quality local %.2f remote %.2f (1.00 = no loss), " "out %.0f pkt/s %.0f B/s, in %.0f pkt/s %.0f B/s, rate %d B/s, " "pending %d B, queue %lld us | gameplay sent %llu got %llu refused %llu (last result %d) resets %u, %u ms since its last packet", i, state, *(int *) (st + 4), *(float *) (st + 8), *(float *) (st + 12), *(float *) (st + 16), *(float *) (st + 20), *(float *) (st + 24), *(float *) (st + 28), *(int *) (st + 32), *(int *) (st + 36), (long long) *(int64_t *) (st + 48), (unsigned long long) P[i].tx_n, (unsigned long long) P[i].rx_n, (unsigned long long) P[i].fail_n, P[i].last_res, P[i].resets, P[i].rx_n ? (unsigned) (now - P[i].last_rx) : 0u);
                    }
            }
        }
        srv_poll();

        uint8_t buf[2048];
        struct sockaddr_in from;
        int fl = sizeof from;
        int n, drained = 0;
        {
            u_long pend = 0;
            if (!ioctlsocket(gs, FIONREAD, &pend) && pend > g_max_pending)
                g_max_pending = pend;
        }
        while ((n =
                recvfrom(gs, (char *) buf, sizeof buf, 0,
                         (struct sockaddr *) &from, &fl)) > 0) {
            fl = sizeof from;
            drained++;
            g_last_pkt = GetTickCount();
            g_pkt_seen = 1;
            if (buf[0] == 0x00 && n >= 6) {     // hello
                if (!g_have_game || memcmp(&g_game, &from, sizeof from)) {
                    xlog("game hello sid %08x slot %d",
                         *(uint32_t *) (buf + 1), buf[5]);
                    for (int i = 0; i < 16; i++) {
                        uint64_t keep = P[i].id;
                        memset(&P[i], 0, sizeof P[i]);
                        P[i].id = keep;
                    }           // counters of this game run only
                    g_tx_since_any_rx = 0;
                    g_last_rx_any = GetTickCount();
                    g_hello_at = g_last_rx_any;
                }
                g_game = from;
                g_have_game = 1;
                uint8_t ack = 0x01;
                sendto(gs, (const char *) &ack, 1, 0,
                       (struct sockaddr *) &from, sizeof from);
            } else if (buf[0] == 0x06) {        // the ext's hello (direct melee path)
                if (!g_have_ext || memcmp(&g_ext, &from, sizeof from))
                    xlog("direct path: ext hello from port %u",
                         (unsigned) ntohs(from.sin_port));
                g_ext = from;
                g_have_ext = 1;
                uint8_t ack = 0x07;
                sendto(gs, (const char *) &ack, 1, 0,
                       (struct sockaddr *) &from, sizeof from);
            } else if (buf[0] == 0x05 && n >= 3 && M.active) {  // ext message -> dst slot, straight over Steam
                uint8_t dst = buf[1];
                if (dst < 16 && M.peer[dst] && n - 2 <= 1500) {
                    uint8_t o[1600];
                    o[0] = 0x05;
                    memcpy(o + 1, &M.sid, 4);
                    o[5] = M.own;
                    memcpy(o + 6, buf + 2, (size_t) (n - 2));
                    peer_send(M.peer[dst], o, n + 4);
                    g_dir_out++;
                }
            } else if (buf[0] == 0x02 && n >= 7 && M.active) {  // data -> dst slot
                g_game = from;
                g_have_game = 1;
                g_last_data = GetTickCount();
                uint8_t dst = buf[6];
                if (dst < 16 && M.peer[dst]) {
                    int r = peer_send(M.peer[dst], buf, n);
                    g_stats_out++;
                    if (P[dst].id == M.peer[dst]) {
                        if (!P[dst].tx_n)
                            P[dst].first_tx = GetTickCount();
                        P[dst].tx_n++;
                        P[dst].tx_since_rx += (uint64_t) n;
                        g_tx_since_any_rx += (uint64_t) n;
                        if (r != 1) {
                            P[dst].fail_n++;
                            P[dst].last_res = r;
                        }
                    }
                }
            }
        }
        if (drained > g_max_drain_gs)
            g_max_drain_gs = drained;
        poll_peers(gs);
        // This loop used to run at 500 wakes/s at above-normal priority with
        // a 1 ms system timer for as long as the game lives - in single player too, where nothing ever talks to it. Idle (no datagram from the
        // game for 10 s): normal priority, the 1 ms timer period released, a 20 ms wake (the server link is in the wait set, a game packet
        // wakes it at once); everything comes back at the game's first packet of the next match / lobby.
        {
            int idle = !g_pkt_seen
                || (LONG) (GetTickCount() - g_last_pkt) > 10000;
            if (idle != g_idle_mode) {
                g_idle_mode = idle;
                if (idle) {
                    timeEndPeriod(1);
                    SetPriorityClass(GetCurrentProcess(),
                                     NORMAL_PRIORITY_CLASS);
                    SetThreadPriority(GetCurrentThread(),
                                      THREAD_PRIORITY_NORMAL);
                } else {
                    timeBeginPeriod(1);
                    SetPriorityClass(GetCurrentProcess(),
                                     ABOVE_NORMAL_PRIORITY_CLASS);
                    SetThreadPriority(GetCurrentThread(),
                                      THREAD_PRIORITY_ABOVE_NORMAL);
                }
                xlog("pump: %s (game traffic %s)",
                     idle ?
                     "idle mode - 20 ms wake, normal priority, timer period released"
                     : "active mode - 2 ms wake, above normal",
                     idle ? "none for 10 s" : "back");
            }
        }
        // wake at once when the game sends something (a plain Sleep(1) is up to 15.6 ms when timers are coarse),
        // at the latest after 2 ms to poll Steam's receive queue
        {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(gs, &rf);
            if (g_idle_mode && g_srv != INVALID_SOCKET)
                FD_SET(g_srv, &rf);
            struct timeval tv = { 0, g_idle_mode ? 20000 : 2000 };
            select(0, &rf, NULL, NULL, &tv);
        }                       // 2 ms (was 1): half the wakes beside the game; data from the game still wakes it at once; the 4 MB loopback buffer holds seconds
    }
    xlog("game exited");
    write_status(0);
    if (Shutdown)
        Shutdown();
    return 0;
}
