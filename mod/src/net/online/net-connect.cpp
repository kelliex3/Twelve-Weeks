#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "net-connect.hpp"
#include "net-matchmaking.hpp"
#include "net-config.hpp"
#include "net-transport.hpp"
#include "relay-protocol.h"
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace net::connect {

    static const char* kInternetHost = "1.1.1.1";   // Cloudflare
    static const uint16_t kInternetPort = 443;
    static const uint64_t kProbeTimeoutMs = 5000;        // per-check connect timeout
    static const uint64_t kStepDwellMs = 3000;        // hold each step on screen this long
    static const uint64_t kRelayResendMs = 800;        // re-ping the relay this often (UDP can drop a datagram)

    enum Check { CHK_NETSTACK = 0, CHK_INTERNET, CHK_MATCHMAKER, CHK_RELAY, CHK_COUNT };

    enum class Sub { Probe, Dwell };

    static Phase       g_phase = Phase::Idle;
    static int         g_check = 0;                // index of the current check
    static Sub         g_sub = Sub::Probe;       // probing, or dwelling on the result?
    static bool        g_started = false;            // has the current probe begun?
    static bool        g_pass = false;            // did the current check pass?
    static uint64_t    g_resolveMs = 0;                // when the current check resolved
    static SOCKET      g_sock = INVALID_SOCKET;
    static uint64_t    g_probeMs = 0;                // when the current probe started
    static const char* g_status = "";
    static bool        g_netstackOk = false;   // CHK_NETSTACK result (winsock came up)
    static uint32_t    g_relayNonce = 0;             // nonce of the current relay ping (0 = none)
    static uint64_t    g_relaySentMs = 0;            // when the last relay ping went out (for resend)

    static void probeClose()
    {
        if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
    }

    static bool probeBegin(const char* ip, uint16_t port)
    {
        g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (g_sock == INVALID_SOCKET) return false;

        u_long nb = 1;
        ioctlsocket(g_sock, FIONBIO, &nb);

        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = inet_addr(ip);
        if (a.sin_addr.s_addr == INADDR_NONE) { probeClose(); return false; }

        g_probeMs = GetTickCount64();
        int r = ::connect(g_sock, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        if (r == 0) return true;
        if (WSAGetLastError() == WSAEWOULDBLOCK) return true;
        probeClose();
        return false;
    }

    enum class Poll { Pending, Ok, Fail };
    static Poll probePoll()
    {
        if (g_sock == INVALID_SOCKET) return Poll::Fail;

        fd_set wr, ex; FD_ZERO(&wr); FD_ZERO(&ex);
        FD_SET(g_sock, &wr); FD_SET(g_sock, &ex);
        timeval tv{};
        int r = select(0, nullptr, &wr, &ex, &tv);

        if (r > 0) {
            if (FD_ISSET(g_sock, &ex)) { probeClose(); return Poll::Fail; }
            if (FD_ISSET(g_sock, &wr)) {
                int err = 0, len = sizeof(err);
                getsockopt(g_sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                probeClose();
                return err == 0 ? Poll::Ok : Poll::Fail;
            }
        }
        if (GetTickCount64() - g_probeMs > kProbeTimeoutMs) { probeClose(); return Poll::Fail; }
        return Poll::Pending;
    }

    static void resolve(bool pass, const char* status)
    {
        probeClose();
        g_pass = pass;
        g_status = status;
        g_resolveMs = GetTickCount64();
        g_sub = Sub::Dwell;
    }

    static void nextCheck()
    {
        g_started = false;
        g_sub = Sub::Probe;
        if (++g_check >= CHK_COUNT) { g_phase = Phase::Complete; g_status = "COMPLETE"; }
    }

    static void stepProbe(const char* ip, uint16_t port, const char* running, const char* failMsg)
    {
        if (!g_started) {
            g_status = running;
            if (!probeBegin(ip, port)) { resolve(false, failMsg); return; }
            g_started = true;
            return;
        }
        switch (probePoll()) {
        case Poll::Pending: return;
        case Poll::Ok:      resolve(true, running); return;
        case Poll::Fail:    resolve(false, failMsg); return;
        }
    }

    static bool relaySendPing()
    {
        RelayPing p{};
        p.type = RELAY_PING;
        p.nonce = g_relayNonce;

        const int count = net::config::RelayCount();
        bool sentAny = false;
        g_relaySentMs = GetTickCount64();
        for (int i = 0; i < count; ++i) {
            net::config::RelayInfo r = net::config::Relay(i);
            if (!r.ip || !r.ip[0] || r.port == 0) continue;

            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(r.port);
            a.sin_addr.s_addr = inet_addr(r.ip);
            if (a.sin_addr.s_addr == INADDR_NONE) continue;

            if (sendto(g_sock, reinterpret_cast<const char*>(&p), sizeof(p), 0,
                reinterpret_cast<sockaddr*>(&a), sizeof(a)) != SOCKET_ERROR)
                sentAny = true;
        }
        return sentAny;
    }

    static bool relayBegin()
    {
        g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_sock == INVALID_SOCKET) return false;
        u_long nb = 1;
        ioctlsocket(g_sock, FIONBIO, &nb);

        if (++g_relayNonce == 0) g_relayNonce = 1;
        g_probeMs = GetTickCount64();
        if (!relaySendPing()) { probeClose(); return false; }
        return true;
    }

    static Poll relayPoll()
    {
        if (g_sock == INVALID_SOCKET) return Poll::Fail;

        for (;;) {
            sockaddr_in from{}; int fromLen = sizeof(from);
            uint8_t buf[64];
            int n = recvfrom(g_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n == SOCKET_ERROR) break;
            if (n < static_cast<int>(sizeof(RelayPing))) continue;
            if (buf[0] != RELAY_PONG) continue;
            RelayPing p; memcpy(&p, buf, sizeof(p));
            if (p.nonce == g_relayNonce) { probeClose(); return Poll::Ok; }
        }

        uint64_t now = GetTickCount64();
        if (now - g_probeMs > kProbeTimeoutMs) { probeClose(); return Poll::Fail; }
        if (now - g_relaySentMs > kRelayResendMs) relaySendPing();
        return Poll::Pending;
    }

    void Begin()
    {
        Reset();
        g_netstackOk = (net::EnsureWinsock() == 0);
        g_phase = Phase::Running;
        g_check = 0;
        g_sub = Sub::Probe;
        g_started = false;
        g_status = "CHECKING NETWORK";
    }

    void Tick()
    {
        if (g_phase != Phase::Running) return;

        if (g_sub == Sub::Dwell) {
            if (GetTickCount64() - g_resolveMs < kStepDwellMs) return;
            if (g_pass) nextCheck();
            else        g_phase = Phase::Failed;
            return;
        }

        switch (g_check) {
        case CHK_NETSTACK:
            resolve(g_netstackOk, g_netstackOk ? "CHECKING NETWORK" : "NETWORK ERROR");
            break;
        case CHK_INTERNET:
            stepProbe(kInternetHost, kInternetPort,
                "CHECKING INTERNET CONNECTION", "NO INTERNET CONNECTION");
            break;
        case CHK_MATCHMAKER:
            if (!g_started) {
                g_status = "CONNECTING TO SERVER";
                net::config::Begin();
                g_started = true;
                break;
            }
            net::config::Tick();
            switch (net::config::GetState()) {
            case net::config::State::Fetching:
                break;
            case net::config::State::Ready:
                if (net::config::MapCount() < 1 || net::config::RelayCount() < 1)
                    resolve(false, "SERVER UNAVAILABLE");
                else
                    resolve(true, "CONNECTING TO SERVER");
                break;
            default:
                resolve(false,
                    net::config::TokenRejected()   ? "INVALID TOKEN"
                    : net::config::VersionRejected() ? "VERSION OUTDATED, UPDATE REQUIRED"
                    : "SERVER UNAVAILABLE");
                break;
            }
            break;
        case CHK_RELAY:
            if (!g_started) {
                g_status = "CHECKING GAME SERVER";
                if (!relayBegin()) { resolve(false, "CANNOT REACH GAME SERVERS"); break; }
                g_started = true;
                break;
            }
            switch (relayPoll()) {
            case Poll::Pending: break;
            case Poll::Ok:      resolve(true,  "CHECKING GAME SERVER");       break;
            case Poll::Fail:    resolve(false, "CANNOT REACH GAME SERVERS");  break;
            }
            break;
        default:
            break;
        }
    }

    void Reset()
    {
        probeClose();
        net::config::Reset();
        g_phase = Phase::Idle;
        g_check = 0;
        g_sub = Sub::Probe;
        g_started = false;
        g_status = "";
    }

    Phase       GetPhase() { return g_phase; }
    const char* StatusText() { return g_status; }

    float Progress()
    {
        if (g_phase == Phase::Complete) return 1.0f;
        if (g_phase == Phase::Idle)     return 0.0f;
        return static_cast<float>(g_check) / static_cast<float>(CHK_COUNT);
    }
}