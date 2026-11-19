// ported from tools/ext/netdirect.c

#include "net-direct.hpp"

#include <winsock2.h>
#include <windows.h>

#include "log.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "net-transport.hpp"
#include "net-session.hpp"
#include "net-gui2.hpp"
#include "net-player.hpp"
#include "net-replication.hpp"

#include <cstring>

namespace net::direct {

    namespace {

        // ext ND_WINDOW_MS / ND_RING and the wire opcodes
        constexpr uint64_t kWindowMs   = 800;
        constexpr int      kRing       = 128;
        constexpr int      kMaxPayload = 120;
        constexpr int      kMaxWire    = 160;
        constexpr uint8_t  kHello      = 0x06;
        constexpr uint8_t  kAck        = 0x07;
        constexpr uint8_t  kData       = 0x05;
        constexpr uint8_t  kKindMessage = 1;
        constexpr uint8_t  kKindPing   = 2;
        constexpr uint8_t  kKindPong   = 3;

        constexpr uint64_t kOpenMs     = 2000;   // ext: retry the helper every 2 s
        constexpr uint64_t kHelloMs    = 1000;   // ...every 1 s until it answers
        constexpr uint64_t kHelloUpMs  = 5000;   // ...every 5 s while it is up
        constexpr uint64_t kPingMs     = 500;    // direct RTT probe cadence
        constexpr uint64_t kPongFreshMs = 3000;  // a peer needs a recent pong to be used
        constexpr uint64_t kLogMs      = 20000;  // re-log a stable ON decision
        constexpr int      kFlagFrames = 300;    // ~5 s sidecar re-check

        struct DupEntry {
            uint64_t ms = 0;
            uint32_t h = 0;
            uint8_t  slot = 0, type = 0, path = 0, matched = 0;
        };

        struct State {
            int     off = -1, up = 0, delivering = 0;
            SOCKET  s = INVALID_SOCKET;
            uint64_t nextOpen = 0, nextHello = 0, nextPing = 0;
            float   rtt[kMaxNetPlayers]{};
            uint64_t pongMs[kMaxNetPlayers]{};
            int     use[kMaxNetPlayers]{};
            uint64_t logMs[kMaxNetPlayers]{};
            uint32_t tx[kMaxNetPlayers]{}, rx[kMaxNetPlayers]{}, dup[kMaxNetPlayers]{};
            DupEntry ring[kRing];
            int      ri = 0;
        } g_nd;

        // The mod's equivalents of the ext's melee-critical message ids. Everything else stays
        // on the relay. TODO(netdirect): extend this set when the ext's hit-rel / dodge /
        // hitreact / MEX / MV3 / PSYNC producers are ported; those wire ids have no mod NEVT_*.
        bool Eligible(uint8_t type, const uint8_t* buf, uint8_t len)
        {
            if (!buf || len < 1 || len > kMaxPayload)
                return false;
            switch (type) {
            case NEVT_PLAYER_MELEE_ATTACK:
            case NEVT_TAKE_DAMAGE:
                return true;
            default:
                return false;
            }
        }

        uint32_t Hash(uint8_t type, const uint8_t* b, uint8_t len)
        {
            uint32_t h = 2166136261u ^ type;
            for (uint8_t i = 0; i < len; i++)
                h = (h ^ b[i]) * 16777619u;
            return h;
        }

        // 1 = this copy came over the other path within the window (drop it)
        bool Dup(int slot, uint8_t type, const uint8_t* buf, uint8_t len, int path)
        {
            if (slot < 0 || slot >= kMaxNetPlayers)
                return false;
            const uint64_t now = GetTickCount64();
            const uint32_t h = Hash(type, buf, len);
            for (int k = 0; k < kRing; k++) {
                int i = (g_nd.ri - 1 - k + kRing * 2) % kRing;
                const DupEntry& e = g_nd.ring[i];
                if (!e.ms || now - e.ms > kWindowMs)
                    break;
                if (e.slot == slot && e.type == type && e.h == h
                    && e.path != path && !e.matched) {
                    g_nd.ring[i].matched = 1;
                    g_nd.dup[slot]++;
                    return true;
                }
            }
            const int i = g_nd.ri;
            g_nd.ri = (g_nd.ri + 1) % kRing;
            g_nd.ring[i].ms = now;
            g_nd.ring[i].h = h;
            g_nd.ring[i].slot = static_cast<uint8_t>(slot);
            g_nd.ring[i].type = type;
            g_nd.ring[i].path = static_cast<uint8_t>(path);
            g_nd.ring[i].matched = 0;
            return false;
        }

        bool Use(int slot)
        {
            if (!g_nd.up || slot <= 0 || slot >= kMaxNetPlayers || g_nd.rtt[slot] <= 0.0f)
                return false;
            // ext: g_psRtt[slot] (the psync RTT through the host). The mod does not port the
            // psync RTT table; the per-peer state-packet ping is the closest relay measurement.
            const int ping = NetPlayers::BySlot(slot).PingMs();
            const float rel = ping > 0 ? static_cast<float>(ping) : 0.0f;
            const uint64_t now = GetTickCount64();
            const bool ok = now - g_nd.pongMs[slot] < kPongFreshMs
                && (rel <= 0.0f || g_nd.rtt[slot] < rel * 0.9f);
            if (ok != (g_nd.use[slot] != 0)
                || (ok && now - g_nd.logMs[slot] > kLogMs)) {
                TO1_LOG("direct melee path slot %d: rtt %.0f vs relay %.0f ms -> %s (sent %u, got %u, relay copies dropped %u)\n",
                    slot, g_nd.rtt[slot], rel, ok ? "ON" : "off",
                    g_nd.tx[slot], g_nd.rx[slot], g_nd.dup[slot]);
                g_nd.use[slot] = ok ? 1 : 0;
                g_nd.logMs[slot] = now;
            }
            return ok;
        }

        void Raw(int dst, uint8_t kind, const uint8_t* pl, int plen)
        {
            uint8_t m[kMaxWire];
            if (g_nd.s == INVALID_SOCKET || !pl || plen < 0 || plen > kMaxWire - 3)
                return;
            m[0] = kData;
            m[1] = static_cast<uint8_t>(dst);
            m[2] = kind;
            if (plen)
                memcpy(m + 3, pl, static_cast<size_t>(plen));
            send(g_nd.s, reinterpret_cast<const char*>(m), 3 + plen, 0);
        }

        void Open()
        {
            sockaddr_in a{};
            if (!net::transport::RelayEndpoint(&a))
                return;                          // not a Steam-helper match
            if (net::EnsureWinsock() != 0)
                return;

            const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == INVALID_SOCKET)
                return;
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            if (connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
                closesocket(s);
                return;
            }
            g_nd.s = s;
            g_nd.up = 0;
            g_nd.nextHello = 0;
            TO1_LOG("direct melee path: socket to the helper on 127.0.0.1:%u\n",
                static_cast<unsigned>(ntohs(a.sin_port)));
        }

        void Close()
        {
            if (g_nd.s != INVALID_SOCKET) {
                closesocket(g_nd.s);
                g_nd.s = INVALID_SOCKET;
            }
            g_nd.up = 0;
            memset(g_nd.rtt, 0, sizeof g_nd.rtt);
            memset(g_nd.pongMs, 0, sizeof g_nd.pongMs);
            memset(g_nd.use, 0, sizeof g_nd.use);
        }

        void HandleDatagram(const uint8_t* b, int n, uint64_t now)
        {
            if (b[0] == kAck) {
                if (!g_nd.up) {
                    g_nd.up = 1;
                    TO1_LOG("direct melee path: helper supports it\n");
                }
                return;
            }
            if (b[0] != kData || n < 4)
                return;

            const int src = b[1];
            const uint8_t kind = b[2];
            if (src <= 0 || src >= kMaxNetPlayers || src == net::session::MySlot())
                return;

            if (kind == kKindPing && n >= 11) {
                Raw(src, kKindPong, b + 3, 8);
                return;
            }
            if (kind == kKindPong && n >= 11) {
                double t = 0.0;
                memcpy(&t, b + 3, 8);
                const double rtt = engine::frame::NowMs() - t;
                if (rtt > 0.0 && rtt < 3000.0) {
                    g_nd.rtt[src] = g_nd.rtt[src] > 0.0f
                        ? g_nd.rtt[src] * 0.75f + static_cast<float>(rtt) * 0.25f
                        : static_cast<float>(rtt);
                    g_nd.pongMs[src] = now;
                }
                return;
            }
            if (kind == kKindMessage && n >= 5) {
                const uint8_t type = b[3];
                const uint8_t len = static_cast<uint8_t>(n - 4);
                if (!Eligible(type, b + 4, len))
                    return;
                g_nd.rx[src]++;
                if (Dup(src, type, b + 4, len, 1))
                    return;                      // the relay copy came first
                g_nd.delivering = 1;
                net::replication::HandleNetEvent(static_cast<uint8_t>(src), type, b + 4, len);
                g_nd.delivering = 0;
            }
        }

    }

    void Tick()
    {
        const bool present = engine::flag::Present("t2r-no-direct-melee.txt", kFlagFrames);
        const int off = present ? 1 : 0;
        if (off != g_nd.off) {
            g_nd.off = off;
            if (off) {
                Close();
                TO1_LOG("direct melee path OFF (t2r-no-direct-melee.txt)\n");
            }
            else {
                TO1_LOG("direct melee path ON (t2r-no-direct-melee.txt absent)\n");
            }
        }
        if (g_nd.off)
            return;

        const uint64_t now = GetTickCount64();
        const bool inMatch = net::gui2::InMatch()
            && net::session::Role() == NetRole::Client;
        if (!inMatch) {
            if (g_nd.s != INVALID_SOCKET)
                Close();
            return;
        }
        if (g_nd.s == INVALID_SOCKET) {
            if (now < g_nd.nextOpen)
                return;
            g_nd.nextOpen = now + kOpenMs;
            Open();
            if (g_nd.s == INVALID_SOCKET)
                return;
        }
        if (now >= g_nd.nextHello) {
            const uint8_t h = kHello;
            send(g_nd.s, reinterpret_cast<const char*>(&h), 1, 0);
            g_nd.nextHello = now + (g_nd.up ? kHelloUpMs : kHelloMs);
        }
        if (g_nd.up && now >= g_nd.nextPing) {
            g_nd.nextPing = now + kPingMs;
            double t = engine::frame::NowMs();
            const int me = net::session::MySlot();
            for (int s = 1; s < kMaxNetPlayers; s++)
                if (s != me)
                    Raw(s, kKindPing, reinterpret_cast<const uint8_t*>(&t), 8);
        }

        for (int k = 0; k < 64; k++) {
            uint8_t b[400];
            const int n = recv(g_nd.s, reinterpret_cast<char*>(b), sizeof b, 0);
            if (n <= 0)
                break;
            HandleDatagram(b, n, now);
        }
    }

    void Reset()
    {
        Close();
        g_nd.nextOpen = 0;
        g_nd.nextHello = 0;
        g_nd.nextPing = 0;
        memset(g_nd.pongMs, 0, sizeof g_nd.pongMs);
        memset(g_nd.tx, 0, sizeof g_nd.tx);
        memset(g_nd.rx, 0, sizeof g_nd.rx);
        memset(g_nd.dup, 0, sizeof g_nd.dup);
        memset(g_nd.ring, 0, sizeof g_nd.ring);
        g_nd.ri = 0;
    }

    void OnSend(uint8_t type, const void* data, uint8_t size, uint8_t targetSlot)
    {
        if (!g_nd.up || g_nd.delivering || !data)
            return;
        if (!Eligible(type, static_cast<const uint8_t*>(data), size))
            return;
        if (net::session::Role() != NetRole::Client)
            return;                              // the host already talks to everybody

        const int me = net::session::MySlot();
        uint8_t m[1 + kMaxPayload];
        m[0] = type;
        memcpy(m + 1, data, size);
        for (int s = 1; s < kMaxNetPlayers; s++) {
            if (targetSlot != kBroadcastSlot && targetSlot != s)
                continue;
            if (s == me || !Use(s))
                continue;
            Raw(s, kKindMessage, m, 1 + size);
            g_nd.tx[s]++;
        }
    }

    bool RelayIn(uint8_t senderSlot, uint8_t type, const uint8_t* data, uint8_t size)
    {
        if (!g_nd.up || g_nd.delivering)
            return false;
        if (!Eligible(type, data, size))
            return false;
        return Dup(senderSlot, type, data, size, 0);
    }

}
