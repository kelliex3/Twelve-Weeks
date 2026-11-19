#include "net-transport.hpp"
#include "net-debug.hpp"
#include "relay-protocol.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace net {

    int EnsureWinsock()
    {
        static bool up = false;
        if (up) return 0;
        WSADATA wsa;
        int r = WSAStartup(MAKEWORD(2, 2), &wsa);
        TO1_LOG("[NET] WSAStartup: %d\n", r);
        if (r != 0) return r;
        up = true;
        return 0;
    }

}

namespace net::transport {

    static SOCKET g_sock = INVALID_SOCKET;
    static int    g_lastError = 0;

    static bool        g_relayMode = false;
    static sockaddr_in g_relayAddr{};
    static uint32_t    g_relaySession = 0;
    static uint8_t     g_relaySlot = 0;
    static bool        g_relayRegistered = false;
    static uint64_t    g_relayLastReg = 0;

    static const int      kRelayMaxPayload = RELAY_MAX_PAYLOAD;
    static const uint32_t kVirtBase = 0xFF000000u;
    static const uint16_t kVirtPort = 1;

    static sockaddr_in VirtualAddrForSlot(uint8_t slot)
    {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(kVirtBase | slot);
        a.sin_port = htons(kVirtPort);
        return a;
    }

    static bool SlotForVirtualAddr(const sockaddr_in& a, uint8_t* slot)
    {
        uint32_t h = ntohl(a.sin_addr.s_addr);
        if ((h & 0xFFFFFF00u) != kVirtBase) return false;
        if (slot) *slot = static_cast<uint8_t>(h & 0xFF);
        return true;
    }

    static void SendRegister()
    {
        RelayRegister reg{};
        reg.type = RELAY_REGISTER;
        reg.sessionId = g_relaySession;
        reg.slot = g_relaySlot;
        sendto(g_sock, reinterpret_cast<const char*>(&reg), sizeof(reg), 0,
            reinterpret_cast<const sockaddr*>(&g_relayAddr), sizeof(g_relayAddr));
    }

    static void RelayMaintain()
    {
        uint64_t now = GetTickCount64();
        uint64_t interval = g_relayRegistered ? 10000ull : 250ull;
        if (now - g_relayLastReg >= interval) {
            SendRegister();
            g_relayLastReg = now;
        }
    }

    bool Open(uint16_t bindPort)
    {
        if (g_sock != INVALID_SOCKET)
            Close();

        if (int r = net::EnsureWinsock(); r != 0) { g_lastError = r; return false; }

        g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_sock == INVALID_SOCKET) {
            g_lastError = WSAGetLastError();
            TO1_LOG("[NET] socket() error: %d\n", g_lastError);
            return false;
        }

        u_long nonBlocking = 1;
        ioctlsocket(g_sock, FIONBIO, &nonBlocking);

        if (bindPort != 0) {
            sockaddr_in localAddr{};
            localAddr.sin_family = AF_INET;
            localAddr.sin_port = htons(bindPort);
            localAddr.sin_addr.s_addr = INADDR_ANY;

            if (::bind(g_sock, reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) == SOCKET_ERROR) {
                g_lastError = WSAGetLastError();
                TO1_LOG("[NET] bind(%u) error: %d\n", bindPort, g_lastError);
                closesocket(g_sock);
                g_sock = INVALID_SOCKET;
                return false;
            }
        }

        g_lastError = 0;
        return true;
    }

    bool OpenRelay(const char* relayIp, uint16_t relayPort, uint32_t sessionId, uint8_t mySlot)
    {
        if (g_sock != INVALID_SOCKET)
            Close();

        if (int r = net::EnsureWinsock(); r != 0) { g_lastError = r; return false; }

        g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_sock == INVALID_SOCKET) {
            g_lastError = WSAGetLastError();
            TO1_LOG("[NET] socket() error: %d\n", g_lastError);
            return false;
        }

        u_long nonBlocking = 1;
        ioctlsocket(g_sock, FIONBIO, &nonBlocking);

        if (!MakeAddr(relayIp, relayPort, &g_relayAddr)) {
            TO1_LOG("[NET] relay: bad address %s\n", relayIp ? relayIp : "(null)");
            closesocket(g_sock);
            g_sock = INVALID_SOCKET;
            return false;
        }

        g_relayMode = true;
        g_relaySession = sessionId;
        g_relaySlot = mySlot;
        g_relayRegistered = false;
        g_relayLastReg = GetTickCount64();
        SendRegister();

        g_lastError = 0;
        TO1_LOG("[NET] relay mode: %s:%u session=%u slot=%u\n",
            relayIp, relayPort, sessionId, mySlot);
        return true;
    }

    bool RelayEndpoint(sockaddr_in* out)
    {
        if (!out || !g_relayMode)
            return false;
        if (ntohl(g_relayAddr.sin_addr.s_addr) != static_cast<uint32_t>(INADDR_LOOPBACK))
            return false;
        *out = g_relayAddr;
        return true;
    }

    void Close()
    {
        if (g_sock != INVALID_SOCKET) {
            closesocket(g_sock);
            g_sock = INVALID_SOCKET;
        }
        g_relayMode = false;
        g_relayRegistered = false;
    }

    bool IsOpen() { return g_sock != INVALID_SOCKET; }

    bool IsRelay() { return g_relayMode; }

    int DatagramOverhead()
    {
        return g_relayMode ? static_cast<int>(sizeof(RelayDataHeader)) : 0;
    }

    sockaddr_in RelayVirtualAddr(uint8_t slot) { return VirtualAddrForSlot(slot); }

    bool RelaySlotForAddr(const sockaddr_in& a, uint8_t* slot) { return SlotForVirtualAddr(a, slot); }

    int Send(const sockaddr_in& to, const void* data, int len)
    {
        if (g_sock == INVALID_SOCKET) return -1;

        if (g_relayMode) {
            uint8_t slot;
            if (!SlotForVirtualAddr(to, &slot)) return -1;
            if (len < 0 || len > kRelayMaxPayload)  return -1;

            uint8_t pkt[sizeof(RelayDataHeader) + kRelayMaxPayload];
            RelayDataHeader hdr{};
            hdr.type = RELAY_DATA;
            hdr.sessionId = g_relaySession;
            hdr.srcSlot = g_relaySlot;
            hdr.destSlot = slot;
            memcpy(pkt, &hdr, sizeof(hdr));
            if (len) memcpy(pkt + sizeof(hdr), data, static_cast<size_t>(len));

            int sent = sendto(g_sock, reinterpret_cast<const char*>(pkt),
                static_cast<int>(sizeof(hdr) + len), 0,
                reinterpret_cast<const sockaddr*>(&g_relayAddr), sizeof(g_relayAddr));
            if (sent == SOCKET_ERROR) { g_lastError = WSAGetLastError(); return -1; }
            return len;
        }

        int sent = sendto(g_sock, static_cast<const char*>(data), len, 0,
            reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        if (sent == SOCKET_ERROR) {
            g_lastError = WSAGetLastError();
            return -1;
        }
        return sent;
    }

    int Recv(void* buf, int cap, sockaddr_in* from)
    {
        if (g_sock == INVALID_SOCKET) return -1;

        if (g_relayMode) {
            RelayMaintain();

            for (;;) {
                sockaddr_in src{};
                int srcLen = sizeof(src);
                uint8_t pkt[sizeof(RelayDataHeader) + kRelayMaxPayload];
                int n = recvfrom(g_sock, reinterpret_cast<char*>(pkt), sizeof(pkt), 0,
                    reinterpret_cast<sockaddr*>(&src), &srcLen);
                if (n == SOCKET_ERROR) {
                    int err = WSAGetLastError();
                    if (err != WSAEWOULDBLOCK) g_lastError = err;
                    return -1;
                }
                if (n < 1) continue;

                uint8_t type = pkt[0];
                if (type == RELAY_REGISTER_ACK) { g_relayRegistered = true; continue; }
                if (type == RELAY_DATA) {
                    if (n < static_cast<int>(sizeof(RelayDataHeader))) continue;
                    RelayDataHeader hdr;
                    memcpy(&hdr, pkt, sizeof(hdr));
                    int payloadLen = n - static_cast<int>(sizeof(hdr));
                    if (payloadLen > cap) payloadLen = cap;
                    if (payloadLen > 0) memcpy(buf, pkt + sizeof(hdr), static_cast<size_t>(payloadLen));
                    if (from) *from = VirtualAddrForSlot(hdr.srcSlot);
                    return payloadLen;
                }
                continue;
            }
        }

        sockaddr_in src{};
        int srcLen = sizeof(src);
        int bytes = recvfrom(g_sock, static_cast<char*>(buf), cap, 0,
            reinterpret_cast<sockaddr*>(&src), &srcLen);
        if (bytes == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK)
                g_lastError = err;
            return -1;
        }
        if (from) *from = src;
        return bytes;
    }

    int LastError() { return g_lastError; }

    bool SameAddr(const sockaddr_in& a, const sockaddr_in& b)
    {
        return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
    }

    bool MakeAddr(const char* ip, uint16_t port, sockaddr_in* out)
    {
        if (!ip || !out) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(ip);
        if (addr.sin_addr.s_addr == INADDR_NONE)
            return false;
        *out = addr;
        return true;
    }

}