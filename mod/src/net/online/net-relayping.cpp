#include "net-relayping.hpp"
#include "net-config.hpp"
#include "net-transport.hpp"
#include "relay-protocol.h"
#include "net-debug.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace net::relayping {

    // ---- tuning -----------------------------------------------------------------
    static const uint64_t kProbeIntervalMs = 1000;   // one probe per relay per second
    static const uint64_t kProbeTimeoutMs  = 1000;   // a probe unanswered this long = a miss
    static const int      kSamples         = 4;      // report the min RTT over the last N probes
    static const uint16_t kMaxRttMs        = 60000;  // clamp below MM_PING_UNREACHABLE (0xFFFF)

    // ---- per-relay measurement --------------------------------------------------
    struct Target {
        std::string ip;                       // kept to detect a config change
        uint16_t    port      = 0;
        sockaddr_in addr{};
        bool        validAddr = false;        // false if the ip wasn't a dotted IPv4
        bool        awaiting  = false;        // a probe is outstanding
        uint32_t    nonce     = 0;            // nonce of the outstanding probe
        uint64_t    sentMs    = 0;            // when the last probe was sent
        uint16_t    ring[kSamples];           // recent RTTs; MM_PING_UNREACHABLE = miss/empty
        int         ringPos   = 0;
    };

    static std::vector<Target> g_targets;
    static SOCKET   g_sock   = INVALID_SOCKET;
    static uint32_t g_nextNonce = 1;          // never 0 (keeps a fresh nonce distinct from cleared)

    // ---- helpers ----------------------------------------------------------------
    // Best (min) valid RTT currently in a target's ring, or MM_PING_UNREACHABLE if the
    // last kSamples probes all missed (or none have completed yet).
    static uint16_t reportPing(const Target& t)
    {
        uint16_t best = MM_PING_UNREACHABLE;
        for (int k = 0; k < kSamples; ++k)
            if (t.ring[k] < MM_PING_UNREACHABLE && t.ring[k] < best) best = t.ring[k];
        return best;
    }

    // Rebuild the target list from net::config whenever it changes (or first appears).
    // Resets all measurements -- the relay set changed, so old RTTs are meaningless.
    static void rebuildIfNeeded()
    {
        int n = net::config::RelayCount();
        if (n > MM_MAX_RELAYS) n = MM_MAX_RELAYS;

        bool same = (n == static_cast<int>(g_targets.size()));
        for (int i = 0; same && i < n; ++i) {
            net::config::RelayInfo ri = net::config::Relay(i);
            if (g_targets[i].ip != ri.ip || g_targets[i].port != ri.port) same = false;
        }
        if (same) return;

        g_targets.clear();
        for (int i = 0; i < n; ++i) {
            net::config::RelayInfo ri = net::config::Relay(i);
            Target t;
            t.ip   = ri.ip ? ri.ip : "";
            t.port = ri.port;
            t.addr.sin_family = AF_INET;
            t.addr.sin_port   = htons(ri.port);
            t.addr.sin_addr.s_addr = inet_addr(t.ip.c_str());
            t.validAddr = (t.addr.sin_addr.s_addr != INADDR_NONE) && !t.ip.empty();
            for (int k = 0; k < kSamples; ++k) t.ring[k] = MM_PING_UNREACHABLE;
            g_targets.push_back(std::move(t));
        }
        TO1_LOG("[PING] relay targets: %d\n", static_cast<int>(g_targets.size()));
    }

    static bool ensureSocket()
    {
        if (g_sock != INVALID_SOCKET) return true;
        if (net::EnsureWinsock() != 0) return false;
        g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_sock == INVALID_SOCKET) return false;
        u_long nb = 1;
        ioctlsocket(g_sock, FIONBIO, &nb);
        return true;
    }

    static void recordSample(Target& t, uint16_t rttMs)
    {
        t.ring[t.ringPos] = rttMs;
        t.ringPos = (t.ringPos + 1) % kSamples;
        t.awaiting = false;
    }

    static void sendProbe(Target& t, uint64_t now)
    {
        RelayPing p;
        p.type  = RELAY_PING;
        p.nonce = g_nextNonce++;
        if (g_nextNonce == 0) g_nextNonce = 1;
        t.nonce    = p.nonce;
        t.sentMs   = now;
        t.awaiting = true;
        sendto(g_sock, reinterpret_cast<const char*>(&p), sizeof(p), 0,
            reinterpret_cast<const sockaddr*>(&t.addr), sizeof(t.addr));
    }
    static void drainReplies(uint64_t now)
    {
        for (;;) {
            sockaddr_in from{};
            int         fromLen = sizeof(from);
            uint8_t     buf[64];
            int n = recvfrom(g_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n == SOCKET_ERROR) break;
            if (n < static_cast<int>(sizeof(RelayPing))) continue;
            if (buf[0] != RELAY_PONG) continue;

            RelayPing p; memcpy(&p, buf, sizeof(p));
            for (auto& t : g_targets) {
                if (t.awaiting && t.nonce == p.nonce) {
                    uint64_t rtt = (now >= t.sentMs) ? now - t.sentMs : 0;
                    uint16_t v   = (rtt >= kMaxRttMs) ? kMaxRttMs : static_cast<uint16_t>(rtt);
                    recordSample(t, v);
                    break;
                }
            }
        }
    }

    void Tick()
    {
        rebuildIfNeeded();
        if (g_targets.empty()) return;         // nothing to ping (no relays known yet)
        if (!ensureSocket())   return;

        uint64_t now = GetTickCount64();
        drainReplies(now);                     // credit replies before timing out probes

        for (auto& t : g_targets) {
            if (!t.validAddr) continue;        // bad/unresolvable ip: stays unreachable
            if (t.awaiting && now - t.sentMs > kProbeTimeoutMs)
                recordSample(t, MM_PING_UNREACHABLE);        // missed: record as unreachable
            if (!t.awaiting && now - t.sentMs >= kProbeIntervalMs)
                sendProbe(t, now);
        }
    }

    void Fill(uint16_t out[MM_MAX_RELAYS])
    {
        for (int i = 0; i < MM_MAX_RELAYS; ++i)
            out[i] = (i < static_cast<int>(g_targets.size())) ? reportPing(g_targets[i])
                                                              : MM_PING_UNREACHABLE;
    }

}