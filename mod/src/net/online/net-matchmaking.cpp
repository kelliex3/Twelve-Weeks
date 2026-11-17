#include <winsock2.h>
#include <windows.h>

#include "net-matchmaking.hpp"
#include "net-relayping.hpp"
#include "net-transport.hpp"
#include "net-account.hpp"
#include "net-debug.hpp"
#include "loader/hwid.hpp"

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace net::matchmaking {


    static constexpr uint32_t packIpv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
    }
    static constexpr uint32_t kMmAddrMask = 0x5A3C9E17u;
    // was tools/patch_winmm.py (server retarget): the shipped base binary carries the matchmaker
    // IP and port as XOR-obfuscated immediates that a deploy-time patch rewrote. A source build
    // sets them here; override at configure time with
    //   -DTO1_MM_IP_A=.. -DTO1_MM_IP_B=.. -DTO1_MM_IP_C=.. -DTO1_MM_IP_D=..   -DTO1_MM_PORT=..
#if defined(TO1_MM_IP_A) && defined(TO1_MM_IP_B) && defined(TO1_MM_IP_C) && defined(TO1_MM_IP_D)
    static constexpr uint32_t kMmAddrEnc = packIpv4(TO1_MM_IP_A, TO1_MM_IP_B, TO1_MM_IP_C, TO1_MM_IP_D) ^ kMmAddrMask;
#elif TO1_LOCAL_MATCHMAKER
    static constexpr uint32_t kMmAddrEnc = packIpv4(127, 0, 0, 1) ^ kMmAddrMask;
#else
    static constexpr uint32_t kMmAddrEnc = packIpv4(168, 119, 115, 162) ^ kMmAddrMask; // Nuremberg (EU) matchmaker
#endif

    // ---- RELEASE vs DEV matchmaker ------------------------------------------------
    // Both instances run on the SAME box and share one accounts.db, so only the port differs.
    // The dev instance exists so the matchmaker can be restarted/broken without dropping live
    // players; nothing about accounts, tokens or logins changes between them.
    //
    //   RELEASE : clients 7001, admin 7002
    //   DEV     : clients 7003, admin 7004   (7002 was already the release admin port)
    //
    // The switch itself lives in net-common.hpp (one line, next to kModVersion, which appends
    // " DEV" so the build announces itself on the main menu). Do not define it here.
    static const uint16_t kMatchmakerPortRelease = 7001;
    static const uint16_t kMatchmakerPortDev     = 7003;
#ifndef TO1_MM_PORT
#define TO1_MM_PORT 0   // 0 = use the release/dev default; was tools/patch_winmm.py (server retarget)
#endif
    static const uint16_t kMatchmakerPort =
        TO1_MM_PORT ? static_cast<uint16_t>(TO1_MM_PORT)
                    : (TO1_DEV_MATCHMAKER ? kMatchmakerPortDev : kMatchmakerPortRelease);
    static const uint64_t kConnectTimeoutMs = 10000;   // give up if connect stalls
    static const uint64_t kPingIntervalMs = 3000;    // heartbeat cadence while live
    static const uint64_t kReconnectCooldownMs = 3000;   // back off menu-presence reconnects after a failure

    // ---- state ------------------------------------------------------------------
    static SOCKET   g_sock = INVALID_SOCKET;
    static State    g_state = State::Idle;
    static State    g_onConnect = State::Searching;  // state to enter once the connect completes
    static uint64_t g_connectStartMs = 0;
    static uint64_t g_lastConnectMs = 0;           // last connect attempt (reconnect backoff)
    static uint64_t g_lastPingMs = 0;              // last MM_PING send time

    static std::vector<uint8_t> g_tx;          // pending outgoing bytes
    static std::vector<uint8_t> g_rx;          // accumulated incoming bytes
    // The formed match, the load-barrier outcomes and the server lobby timeline all
    // live on the Match object (match/match.hpp). This file owns the socket and the
    // state machine, and writes them through TheMatch(); IsMatchmade() is the
    // "we have a match" gate.
    static net::match::Match& TheMatch() { return net::match::Get(); }

    static std::vector<std::string> g_lobbyNames;
    static char                 g_broadcast[MM_BROADCAST_MAX + 1] = { 0 };  // last admin announcement
    static bool                 g_haveBroadcast = false;                    // an unshown broadcast is pending
    static bool                 g_versionRejected = false;  // server refused us for a version mismatch
    static int                  g_serverVersion = 0;      // server's protocol version (from MM_REJECT)
    static bool                 g_tokenRejected = false;    // server refused our account token ("INVALID TOKEN")
    static MMLeaderEntry        g_leaderboard[MM_LEADERBOARD_MAX] = {};  // rows from the last MM_LEADERBOARD
    static int                  g_lbCount = 0;                           // valid rows in g_leaderboard
    static uint32_t             g_onlineCount = 0;                       // last online count (MM_ONLINE)
    static uint64_t             g_lastOnlineReqMs = 0;                   // throttle MM_ONLINE_REQ re-sends
    static bool                 g_authed = false;

    static void copyStr(char* dst, size_t dstSize, const char* src)
    {
        size_t i = 0;
        for (; i + 1 < dstSize && src && src[i]; ++i) dst[i] = src[i];
        for (; i < dstSize; ++i) dst[i] = 0;
    }

    static uint64_t ourClientId()
    {
        static uint64_t id = 0;
        if (id == 0) {
            LARGE_INTEGER pc; QueryPerformanceCounter(&pc);
            id = static_cast<uint64_t>(pc.QuadPart)
                ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32)
                ^ GetTickCount64();
            if (id == 0) id = 1;
        }
        return id;
    }

    static void reset()
    {
        if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
        g_tx.clear();
        g_rx.clear();
        TheMatch().ClearMatchmade();
        g_lobbyNames.clear();
        TheMatch().started = false;
        TheMatch().aborted = false;
        g_versionRejected = false;
        g_tokenRejected = false;
        g_haveBroadcast = false;
        g_serverVersion = 0;
        g_authed = false;
    }

    static void fail(const char* why)
    {
        TO1_LOG("[MM] %s (err %d)\n", why, WSAGetLastError());
        reset();
        g_state = State::Failed;
    }

    static void enqueue(uint8_t type, const void* body, uint16_t bodyLen)
    {
        uint16_t total = static_cast<uint16_t>(1 + bodyLen);
        size_t   at = g_tx.size();
        g_tx.resize(at + 2 + total);
        memcpy(&g_tx[at], &total, 2);
        g_tx[at + 2] = type;
        if (bodyLen) memcpy(&g_tx[at + 3], body, bodyLen);
    }

    static void enqueueAuth()
    {
        MMAuth a{};
        a.version = MM_PROTOCOL_VERSION;
        const char* tok = net::account::Token();
        for (size_t n = 0; n < static_cast<size_t>(MM_TOKEN_LEN) && tok && tok[n]; ++n)
            a.token[n] = tok[n];

        std::vector<uint8_t> body(sizeof(a));
        memcpy(body.data(), &a, sizeof(a));
        hwid::Serialize(body);
        enqueue(MM_AUTH, body.data(), static_cast<uint16_t>(body.size()));
    }

    static void pumpSend()
    {
        while (!g_tx.empty()) {
            int n = send(g_sock, reinterpret_cast<const char*>(g_tx.data()),
                static_cast<int>(g_tx.size()), 0);
            if (n == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) return;
                fail("send failed");
                return;
            }
            if (n <= 0) return;
            g_tx.erase(g_tx.begin(), g_tx.begin() + n);
        }
    }

    static void onMatchFound(const uint8_t* body, uint16_t blen)
    {
        if (blen < sizeof(MMMatchFound)) return;
        MMMatchFound mf;
        memcpy(&mf, body, sizeof(mf));

        net::match::MatchInfo& mi = TheMatch().info;
        memset(&mi, 0, sizeof(mi));
        copyStr(mi.relayIp, sizeof(mi.relayIp), mf.relayIp);
        mi.relayPort = mf.relayPort;
        mi.sessionId = mf.sessionId;
        mi.map = mf.map;
        mi.yourSlot = mf.yourSlot;
        mi.yourSpawn = mf.yourSpawn;
        mi.yourTeam = mf.yourTeam;

        const uint8_t* rp = body + sizeof(mf);
        const uint8_t* end = body + blen;
        uint8_t n = 0;
        for (uint8_t i = 0; i < mf.playerCount && n < MM_MAX_SLOTS &&
            rp + sizeof(MMRosterEntry) <= end; ++i) {
            memcpy(&mi.roster[n++], rp, sizeof(MMRosterEntry));
            rp += sizeof(MMRosterEntry);
        }
        mi.playerCount = n;

        TheMatch().source = net::match::Source::Matchmade;
        g_state = State::Matched;
        TheMatch().started = false;
        TheMatch().aborted = false;
        TheMatch().lobbyPhase = -1;
        TheMatch().lobbyCountdown = 0;
        TO1_LOG("[MM] MATCH session=%u slot=%u map=%u spawn=%u team=%s players=%u relay=%s:%u\n",
            mi.sessionId, mi.yourSlot, mi.map, mi.yourSpawn,
            MMTeamName(mi.yourTeam), n, mi.relayIp, mi.relayPort);

        for (uint8_t i = 0; i < mi.playerCount; ++i)
            TO1_LOG("[MM] team: slot %u \"%s\" -> %s%s\n",
                mi.roster[i].slot, mi.roster[i].name,
                MMTeamName(mi.roster[i].team),
                mi.roster[i].slot == mi.yourSlot ? " (you)" : "");
    }

    static void onStart() { TheMatch().started = true; TO1_LOG("[MM] START ? all players loaded, match begins\n"); }
    static void onAbort() { TheMatch().aborted = true; TO1_LOG("[MM] ABORT ? match torn down, returning to menu\n"); }

    static void onReject(const uint8_t* body, uint16_t blen)
    {
        int reason = (blen >= 1) ? body[0] : 0;      // [0]=reason, [1]=serverVersion
        int sv     = (blen >= 2) ? body[1] : 0;
        reset();
        if (reason == MM_REJECT_INVALID_TOKEN) {
            g_tokenRejected = true;                  // -> connecting page shows "INVALID TOKEN"
            TO1_LOG("[MM] REJECT - invalid or disabled account token\n");
        } else {
            g_versionRejected = true;
            g_serverVersion = sv;
            TO1_LOG("[MM] REJECT - version mismatch; server v%d, we are v%d\n", sv, MM_PROTOCOL_VERSION);
        }
        g_state = State::Failed;
    }

    static void onRequeued()
    {
        TheMatch().Reset(net::match::ResetReason::Dissolved);
        g_lobbyNames.clear();
        g_state = State::Searching;
    }

    static void onLeaderboard(const uint8_t* body, uint16_t blen)
    {
        if (blen < 1) return;
        int count = body[0];
        if (count > MM_LEADERBOARD_MAX) count = MM_LEADERBOARD_MAX;
        const size_t need = 1 + static_cast<size_t>(count) * sizeof(MMLeaderEntry);
        if (blen < need) return;
        for (int i = 0; i < count; ++i)
            memcpy(&g_leaderboard[i], body + 1 + i * sizeof(MMLeaderEntry), sizeof(MMLeaderEntry));
        g_lbCount = count;
    }

    static void onOnline(const uint8_t* body, uint16_t blen)
    {
        if (blen >= 4) memcpy(&g_onlineCount, body, 4);
    }

    static void parseRx()
    {
        while (g_rx.size() >= 2) {
            uint16_t total;
            memcpy(&total, g_rx.data(), 2);
            if (total == 0) { fail("bad frame"); return; }
            if (g_rx.size() < static_cast<size_t>(2 + total)) break;

            uint8_t        type = g_rx[2];
            const uint8_t* body = g_rx.data() + 3;
            uint16_t       blen = static_cast<uint16_t>(total - 1);

            if (type == MM_MATCH_FOUND) onMatchFound(body, blen);
            else if (type == MM_QUEUED)      onRequeued();   // (re)entered the pool: searching
            else if (type == MM_START)       onStart();      // load barrier passed: begin
            else if (type == MM_ABORT)       onAbort();      // match torn down: to menu
            else if (type == MM_AUTH_OK) {                   // authenticated: our account name
                if (blen >= sizeof(MMAuthOk)) {
                    MMAuthOk ok; memcpy(&ok, body, sizeof(ok));
                    char nm[MM_NAME_LEN + 1]; memcpy(nm, ok.username, MM_NAME_LEN); nm[MM_NAME_LEN] = 0;
                    net::account::SetName(nm);
                    TO1_LOG("[MM] AUTH OK - signed in as \"%s\"\n", nm);
                }
                g_authed = true;
            }
            else if (type == MM_REJECT)      onReject(body, blen);   // refused (version / invalid token)
            else if (type == MM_LOBBY) {               // server lobby timeline tick
                if (blen >= 2) { TheMatch().lobbyPhase = body[0]; TheMatch().lobbyCountdown = body[1]; }
            }
            else if (type == MM_LOBBY_ROSTER) {
                g_lobbyNames.clear();
                if (blen >= 1) {
                    const uint8_t n = body[0];
                    int off = 1;
                    for (uint8_t i = 0; i < n && off < blen; ++i) {
                        const uint8_t len = body[off++];
                        if (off + len > blen) break;
                        g_lobbyNames.emplace_back(reinterpret_cast<const char*>(body) + off, len);
                        off += len;
                    }
                }
            }
            else if (type == MM_BROADCAST) { // admin announcement
                int n = blen; if (n > MM_BROADCAST_MAX) n = MM_BROADCAST_MAX;
                memcpy(g_broadcast, body, n);
                g_broadcast[n] = 0;
                g_haveBroadcast = true;
            }
            else if (type == MM_PONG) { /* heartbeat ack: liveness only */ }
            else if (type == MM_LEADERBOARD) onLeaderboard(body, blen);
            else if (type == MM_ONLINE)      onOnline(body, blen);

            const size_t consumed = static_cast<size_t>(2 + total);
            if (g_rx.size() >= consumed)
                g_rx.erase(g_rx.begin(), g_rx.begin() + consumed);
            if (g_state == State::Matched || g_state == State::Failed) return;
        }
    }

    static void pumpRecv()
    {
        uint8_t tmp[1024];
        for (;;) {
            int n = recv(g_sock, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
            if (n > 0) {
                g_rx.insert(g_rx.end(), tmp, tmp + n);
                if (g_rx.size() > 8192) { fail("rx overflow"); return; }
                continue;
            }
            if (n == 0) { fail("matchmaker closed"); return; }
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
            fail("recv failed");
            return;
        }
        parseRx();
    }

    static void pumpConnect()
    {
        fd_set wr, ex;
        FD_ZERO(&wr); FD_ZERO(&ex);
        FD_SET(g_sock, &wr);
        FD_SET(g_sock, &ex);
        timeval tv{};
        int r = select(0, nullptr, &wr, &ex, &tv);

        if (r == SOCKET_ERROR) { fail("select failed"); return; }

        if (r > 0) {
            if (FD_ISSET(g_sock, &ex)) { fail("connect refused"); return; }
            if (FD_ISSET(g_sock, &wr)) {
                int err = 0, len = sizeof(err);
                getsockopt(g_sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                if (err != 0) { WSASetLastError(err); fail("connect error"); return; }
                g_state = g_onConnect;                  // connected! (Connected or Searching)
                g_lastPingMs = GetTickCount64();        // first heartbeat ~3s from now
                pumpSend();                             // flush the queued HELLO / QUEUE
                TO1_LOG("[MM] connected (%s)\n",
                    g_onConnect == State::Searching ? "searching" : "at menu");
                return;
            }
        }

        if (GetTickCount64() - g_connectStartMs > kConnectTimeoutMs)
            fail("connect timeout");
    }

    static void beginConnect(State onConnect)
    {
        if (net::EnsureWinsock() != 0) { fail("WSAStartup"); return; }

        g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (g_sock == INVALID_SOCKET) { fail("socket"); return; }

        u_long nb = 1;
        ioctlsocket(g_sock, FIONBIO, &nb);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kMatchmakerPort);
        addr.sin_addr.s_addr = ServerAddr();

        g_onConnect = onConnect;
        int r = connect(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (r == 0) {
            g_state = onConnect;
            g_lastPingMs = GetTickCount64();
            pumpSend();
        }
        else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            g_state = State::Connecting;
            g_connectStartMs = GetTickCount64();
        }
        else {
            fail("connect");
        }
    }

    void Connect(const char* playerName)
    {
        if (g_state == State::Connecting || g_state == State::Connected ||
            g_state == State::Searching  || g_state == State::Matched)
            return;

        uint64_t now = GetTickCount64();
        if (g_state == State::Failed && now - g_lastConnectMs < kReconnectCooldownMs) return;
        g_lastConnectMs = now;

        reset();
        g_state = State::Idle;
        g_versionRejected = false;
        g_tokenRejected = false;
        g_serverVersion = 0;

        // Authenticate FIRST, then announce presence. Both ride the same connect.
        enqueueAuth();
        MMHello h{};
        h.version = MM_PROTOCOL_VERSION;
        h.clientId = ourClientId();
        copyStr(h.name, sizeof(h.name), playerName);
        enqueue(MM_HELLO, &h, sizeof(h));

        TO1_LOG("[MM] connecting %s:%u (menu presence)\n", ServerHost(), kMatchmakerPort);
        beginConnect(State::Connected);
    }

    void FindMatch(const char* playerName, uint8_t mode, uint8_t map,
        uint64_t primaryWeapon, uint64_t secondaryWeapon)
    {
        auto buildQueue = [&](MMQueue& q) {
            q = MMQueue{};
            q.version = MM_PROTOCOL_VERSION;
            q.clientId = ourClientId();
            copyStr(q.name, sizeof(q.name), playerName);
            q.mode = mode;
            q.map = map;
            q.primaryWeapon = primaryWeapon;
            q.secondaryWeapon = secondaryWeapon;
            net::relayping::Fill(q.relayPing);
        };

        if (g_state == State::Connected) {
            MMQueue q; buildQueue(q);
            enqueue(MM_QUEUE, &q, sizeof(q));
            pumpSend();
            g_state = State::Searching;
            TO1_LOG("[MM] queue (mode=%u map=%u)\n", mode, map);
            return;
        }

        if (g_state == State::Connecting) {
            MMQueue q; buildQueue(q);
            enqueue(MM_QUEUE, &q, sizeof(q));
            g_onConnect = State::Searching;
            TO1_LOG("[MM] queue queued behind pending connect (mode=%u map=%u)\n", mode, map);
            return;
        }

        if (g_state == State::Searching || g_state == State::Matched)
            return;

        reset();
        g_state = State::Idle;
        g_versionRejected = false;
        g_tokenRejected = false;
        g_serverVersion = 0;

        enqueueAuth();
        MMQueue q; buildQueue(q);
        enqueue(MM_QUEUE, &q, sizeof(q));

        TO1_LOG("[MM] searching %s:%u (mode=%u map=%u)\n",
            ServerHost(), kMatchmakerPort, mode, map);
        beginConnect(State::Searching);
    }

    void Cancel()
    {
        if (g_sock != INVALID_SOCKET &&
            (g_state == State::Searching || g_state == State::Connecting ||
                g_state == State::Matched)) {
            enqueue(MM_CANCEL, nullptr, 0);
            pumpSend();
        }
        reset();
        g_state = State::Idle;
    }

    void Tick()
    {
        switch (g_state) {
        case State::Connecting:
            pumpConnect();
            break;
        case State::Connected:
        case State::Searching:
        case State::Matched: {
            uint64_t now = GetTickCount64();
            if (now - g_lastPingMs >= kPingIntervalMs) {
                enqueue(MM_PING, nullptr, 0);
                g_lastPingMs = now;
            }
            pumpSend();
            pumpRecv();
            break;
        }
        default:
            break;
        }
    }

    State            GetState() { return g_state; }

    int         LobbyNameCount()      { return static_cast<int>(g_lobbyNames.size()); }
    const char* LobbyName(int i)
    {
        return (i >= 0 && i < static_cast<int>(g_lobbyNames.size())) ? g_lobbyNames[i].c_str() : "";
    }

    bool SlotLoadout(int slot, StringId64& primaryWeapon, StringId64& secondaryWeapon)
    {
        const net::match::Match& M = TheMatch();
        if (!M.IsMatchmade()) return false;
        for (uint8_t i = 0; i < M.info.playerCount && i < MM_MAX_SLOTS; ++i) {
            if (M.info.roster[i].slot == slot) {
                primaryWeapon = M.info.roster[i].primaryWeapon;
                secondaryWeapon = M.info.roster[i].secondaryWeapon;
                return true;
            }
        }
        return false;
    }

    int MyTeam()
    {
        return TheMatch().MyTeam();
    }

    int SlotTeam(int slot)
    {
        const net::match::Match& M = TheMatch();
        if (!M.IsMatchmade()) return -1;
        for (uint8_t i = 0; i < M.info.playerCount && i < MM_MAX_SLOTS; ++i)
            if (M.info.roster[i].slot == slot)
                return static_cast<int>(M.info.roster[i].team);
        return -1;
    }

    bool VersionRejected() { return g_versionRejected; }
    bool TokenRejected() { return g_tokenRejected; }

    void SendReady()
    {
        if (g_state != State::Matched) return;
        enqueue(MM_READY, nullptr, 0);
        pumpSend();
        TO1_LOG("[MM] READY sent (level loaded)\n");
    }

    void ReportResult(int winner)
    {
        if (g_state != State::Matched) return;
        uint8_t w = static_cast<uint8_t>(winner);
        enqueue(MM_RESULT, &w, 1);
        pumpSend();
        TO1_LOG("[MM] RESULT sent (winner=%d)\n", winner);
    }

    bool Authed()
    {
        return g_authed && (g_state == State::Connected || g_state == State::Searching || g_state == State::Matched);
    }

    bool ReportTamper(const MMTamper& report)
    {
        if (!Authed()) return false;
        enqueue(MM_TAMPER, &report, sizeof(report));
        pumpSend();
        TO1_LOG("[MM] TAMPER sent (kind=%u flags=0x%04x pid=%u detail=%s)\n",
            report.kind, report.flags, report.pid, report.detail);
        return true;
    }

    void RequestLeaderboard()
    {
        if (g_state != State::Connected && g_state != State::Searching &&
            g_state != State::Matched)
            return;
        enqueue(MM_LEADERBOARD_REQ, nullptr, 0);
        pumpSend();
    }
    const MMLeaderEntry* LeaderboardEntry(int i)
    {
        return (i >= 0 && i < g_lbCount) ? &g_leaderboard[i] : nullptr;
    }

    void RequestOnlineCount()
    {
        if (g_state != State::Connected && g_state != State::Searching &&
            g_state != State::Matched)
            return;
        const uint64_t now = GetTickCount64();
        if (now - g_lastOnlineReqMs < 3000) return;
        g_lastOnlineReqMs = now;
        enqueue(MM_ONLINE_REQ, nullptr, 0);
        pumpSend();
    
    }
    uint32_t OnlineCount() { return g_onlineCount; }

    bool StartReceived() { return TheMatch().started; }
    bool Aborted() { return TheMatch().aborted; }

    bool TakeBroadcast(char* out, int cap)
    {
        if (!g_haveBroadcast || !out || cap <= 0) return false;
        int n = static_cast<int>(strlen(g_broadcast));
        if (n > cap - 1) n = cap - 1;
        memcpy(out, g_broadcast, n); out[n] = 0;
        g_haveBroadcast = false;
        return true;
    }

    int LobbyPhase() { return TheMatch().lobbyPhase; }
    int LobbyCountdown() { return TheMatch().lobbyCountdown; }

    uint32_t ServerAddr()
    {
        volatile uint32_t enc = kMmAddrEnc;
        return enc ^ kMmAddrMask;
    }

    const char* ServerHost()
    {
        static char buf[16];
        uint32_t a = ServerAddr();
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
            a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, (a >> 24) & 0xFF);
        return buf;
    }

    uint16_t    ServerPort() { return kMatchmakerPort; }
}