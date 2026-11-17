#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "net-config.hpp"
#include "net-matchmaking.hpp"
#include "net-transport.hpp"
#include "net-account.hpp"
#include "net-debug.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace net::config {

    static const uint64_t kFetchTimeoutMs = 6000;   // connect + fetch budget
    struct Entry { std::string name, preview, task; uint8_t spawns = 0; };
    static std::vector<Entry> g_maps;

    struct RelayEntry { std::string ip; uint16_t port; };
    static std::vector<RelayEntry> g_relays;

    // ---- state ------------------------------------------------------------------
    enum class Phase { Connecting, Receiving };
    static State    g_state = State::Idle;
    static Phase    g_phase = Phase::Connecting;
    static SOCKET   g_sock = INVALID_SOCKET;
    static uint64_t g_startMs = 0;
    static std::vector<uint8_t> g_rx;
    static bool     g_versionRejected = false;   // last fetch refused for a version mismatch?
    static bool     g_tokenRejected = false;     // last fetch refused for an invalid/disabled token?
    static int      g_serverVersion = 0;       // server's protocol version (from MM_REJECT)

    static void closeSock()
    {
        if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
    }

    static void fail(const char* why)
    {
        TO1_LOG("[CFG] %s (err %d)\n", why, WSAGetLastError());
        closeSock();
        g_rx.clear();
        g_state = State::Failed;
    }

    // Parse the MM_CONFIG body:
    //   [u8 len][name] [u8 len][preview] [u8 len][task] [u8 spawns]
    static bool parseConfig(const uint8_t* b, size_t len)
    {
        g_maps.clear();
        g_relays.clear();
        size_t off = 0;
        if (off >= len) return false;
        uint8_t count = b[off++];

        auto getStr = [&](std::string& out) -> bool {
            if (off >= len) return false;
            uint8_t slen = b[off++];
            if (off + slen > len) return false;
            out.assign(reinterpret_cast<const char*>(b) + off, slen);
            off += slen;
            return true;
            };

        for (uint8_t i = 0; i < count; ++i) {
            Entry e;
            if (!getStr(e.name) || !getStr(e.preview) || !getStr(e.task)) return false;
            e.spawns = (off < len) ? b[off++] : 0;
            g_maps.push_back(std::move(e));
        }

        if (off < len) {
            uint8_t rcount = b[off++];
            for (uint8_t i = 0; i < rcount && static_cast<int>(g_relays.size()) < MM_MAX_RELAYS; ++i) {
                if (off >= len) break;
                uint8_t iplen = b[off++];
                if (off + iplen + 2u > len) break; // need ip bytes + 2 port bytes
                RelayEntry e;
                e.ip.assign(reinterpret_cast<const char*>(b) + off, iplen);
                off += iplen;
                e.port = static_cast<uint16_t>(b[off] | (b[off + 1] << 8)); // little endian
                off += 2;
                g_relays.push_back(std::move(e));
            }
        }
        return true;
    }

    static void sendAuthAndGetConfig()
    {
        std::vector<uint8_t> buf;

        MMAuth a{};
        a.version = MM_PROTOCOL_VERSION;
        const char* tok = net::account::Token();
        for (size_t n = 0; n < static_cast<size_t>(MM_TOKEN_LEN) && tok && tok[n]; ++n) a.token[n] = tok[n];
        uint16_t at = static_cast<uint16_t>(1 + sizeof(MMAuth));
        buf.resize(2 + at);
        memcpy(buf.data(), &at, 2);
        buf[2] = MM_AUTH;
        memcpy(buf.data() + 3, &a, sizeof(a));

        size_t base = buf.size();
        uint16_t ct = 1;
        buf.resize(base + 2 + ct);
        memcpy(buf.data() + base, &ct, 2);
        buf[base + 2] = MM_GET_CONFIG;
        send(g_sock, reinterpret_cast<const char*>(buf.data()), static_cast<int>(buf.size()), 0);
    }

    void Begin()
    {
        Reset();
        g_versionRejected = false;
        g_tokenRejected = false;
        g_serverVersion = 0;

        if (net::EnsureWinsock() != 0) { fail("WSAStartup"); return; }

        g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (g_sock == INVALID_SOCKET) { fail("socket"); return; }

        u_long nb = 1;
        ioctlsocket(g_sock, FIONBIO, &nb);

        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(net::matchmaking::ServerPort());
        a.sin_addr.s_addr = net::matchmaking::ServerAddr();

        g_startMs = GetTickCount64();
        g_phase = Phase::Connecting;
        g_state = State::Fetching;

        int r = ::connect(g_sock, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        if (r == 0) {
            sendAuthAndGetConfig();
            g_phase = Phase::Receiving;
        }
        else if (WSAGetLastError() != WSAEWOULDBLOCK) {
            fail("connect");
        }
        // WSAEWOULDBLOCK: connect completes later, pumped in Tick().
    }

    void Tick()
    {
        if (g_state != State::Fetching) return;

        if (GetTickCount64() - g_startMs > kFetchTimeoutMs) { fail("timeout"); return; }

        // --- still connecting: poll for the handshake to complete, then request. ---
        if (g_phase == Phase::Connecting) {
            fd_set wr, ex; FD_ZERO(&wr); FD_ZERO(&ex);
            FD_SET(g_sock, &wr); FD_SET(g_sock, &ex);
            timeval tv{};
            int r = select(0, nullptr, &wr, &ex, &tv);
            if (r == SOCKET_ERROR) { fail("select"); return; }
            if (r > 0) {
                if (FD_ISSET(g_sock, &ex)) { fail("connect refused"); return; }
                if (FD_ISSET(g_sock, &wr)) {
                    int err = 0, len = sizeof(err);
                    getsockopt(g_sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                    if (err != 0) { WSASetLastError(err); fail("connect error"); return; }
                    sendAuthAndGetConfig();
                    g_phase = Phase::Receiving;
                }
            }
            return;
        }

        // --- receiving: drain bytes, then parse the first full frame (MM_CONFIG). ---
        uint8_t tmp[2048];
        for (;;) {
            int n = recv(g_sock, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
            if (n > 0) {
                g_rx.insert(g_rx.end(), tmp, tmp + n);
                if (g_rx.size() > 65536) { fail("rx overflow"); return; }
                continue;
            }
            if (n == 0) { fail("matchmaker closed"); return; }
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
            fail("recv failed");
            return;
        }

        while (g_rx.size() >= 2) {
            uint16_t total;
            memcpy(&total, g_rx.data(), 2);
            if (total == 0) { fail("bad frame"); return; }
            if (g_rx.size() < static_cast<size_t>(2 + total)) return;

            uint8_t        type = g_rx[2];
            const uint8_t* body = g_rx.data() + 3;
            uint16_t       blen = static_cast<uint16_t>(total - 1);

            if (type == MM_CONFIG) {
                if (parseConfig(body, blen)) {
                    closeSock();
                    g_state = State::Ready;
                    TO1_LOG("[CFG] config: %d map(s), %d relay(s)\n", MapCount(), RelayCount());
                }
                else {
                    fail("bad config");
                }
                return;
            }

            if (type == MM_REJECT) {
                int reason = (blen >= 1) ? body[0] : 0;       // [0]=reason, [1]=serverVersion
                if (blen >= 2) g_serverVersion = body[1];
                if (reason == MM_REJECT_INVALID_TOKEN) { g_tokenRejected = true; fail("token rejected"); }
                else                                   { g_versionRejected = true; fail("version rejected"); }
                return;
            }

            if (type == MM_AUTH_OK) {
                if (blen >= sizeof(MMAuthOk)) {
                    MMAuthOk ok; memcpy(&ok, body, sizeof(ok));
                    char nm[MM_NAME_LEN + 1]; memcpy(nm, ok.username, MM_NAME_LEN); nm[MM_NAME_LEN] = 0;
                    net::account::SetName(nm);
                }
                g_rx.erase(g_rx.begin(), g_rx.begin() + 2 + total);
                continue;
            }

            g_rx.erase(g_rx.begin(), g_rx.begin() + 2 + total);
        }
    }

    void Reset()
    {
        closeSock();
        g_rx.clear();
        g_state = State::Idle;
    }

    State GetState() { return g_state; }

    bool VersionRejected() { return g_versionRejected; }
    bool TokenRejected() { return g_tokenRejected; }

    int MapCount() { return static_cast<int>(g_maps.size()); }

    MapInfo Map(int index)
    {
        if (index < 0 || index >= static_cast<int>(g_maps.size()))
            return MapInfo{ "", "", "", 0 };
        const Entry& e = g_maps[index];
        return MapInfo{ e.name.c_str(), e.preview.c_str(), e.task.c_str(), e.spawns };
    }

    int RelayCount() { return static_cast<int>(g_relays.size()); }

    RelayInfo Relay(int index)
    {
        if (index < 0 || index >= static_cast<int>(g_relays.size()))
            return RelayInfo{ "", 0 };
        const RelayEntry& e = g_relays[index];
        return RelayInfo{ e.ip.c_str(), e.port };
    }

}