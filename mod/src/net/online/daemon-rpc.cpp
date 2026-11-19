#include "daemon-rpc.hpp"

#include "net-matchmaking.hpp"
#include "net-transport.hpp"

#include <windows.h>

#include <atomic>

#pragma comment(lib, "ws2_32.lib")

namespace net::daemon {

    namespace {

        std::atomic<uint32_t> g_srvAliveT{ 0 };   // GetTickCount() of the last reply (0 = none yet)
        std::atomic<uint8_t>  g_testFlags{ 0 };   // ext g_srvTestFlags, from the 0xE5 reply

        // xorshift32, jitter only (threads racing on it is harmless).
        uint32_t Rng()
        {
            static std::atomic<uint32_t> s{ 0 };
            uint32_t v = s.load(std::memory_order_relaxed);
            if (!v) {
                v = (static_cast<uint32_t>(GetTickCount()) * 2654435761u)
                    ^ static_cast<uint32_t>(GetCurrentProcessId()) ^ 0x9e3779b9u;
                if (!v) v = 1;
            }
            v ^= v << 13;
            v ^= v >> 17;
            v ^= v << 5;
            s.store(v, std::memory_order_relaxed);
            return v;
        }

    }

    bool Addr(sockaddr_in* out)
    {
        if (!out)
            return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(net::matchmaking::ServerPort());
        a.sin_addr.s_addr = net::matchmaking::ServerAddr();
        if (a.sin_addr.s_addr == 0 || a.sin_port == 0)
            return false;
        *out = a;
        return true;
    }

    bool ConnectTimeout(SOCKET s, const sockaddr_in& a, int ms)
    {
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);

        int r = connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
        bool ok = r == 0;
        if (r != 0) {
            fd_set wr, er;
            FD_ZERO(&wr);
            FD_SET(s, &wr);
            FD_ZERO(&er);
            FD_SET(s, &er);
            timeval tv;
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
            int n = select(0, nullptr, &wr, &er, &tv);
            if (n > 0 && FD_ISSET(s, &wr)) {
                int err = 1, len = sizeof(err);
                ok = getsockopt(s, SOL_SOCKET, SO_ERROR,
                    reinterpret_cast<char*>(&err), &len) == 0 && err == 0;
            }
        }

        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        return ok;
    }

    int FrameState(const char* rep, int got, int cap, int* total)
    {
        if (got < 3)
            return 0;
        const int ln = (rep[0] & 0xff) + ((rep[1] & 0xff) << 8);
        if (total)
            *total = ln + 2;
        if (ln < 1)
            return 0;
        if (ln + 2 > cap)
            return -1;
        return got >= ln + 2 ? 1 : 0;
    }

    int Rpc(const sockaddr_in& a, const void* frame, int flen, char* rep, int cap,
            int connMs, int ioMs, int replyMs, int* sent)
    {
        if (sent)
            *sent = 0;
        if (net::EnsureWinsock() != 0)
            return 0;

        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            return 0;

        DWORD to = static_cast<DWORD>(ioMs);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&to), sizeof(to));

        int got = 0;
        if (ConnectTimeout(s, a, connMs)
            && send(s, reinterpret_cast<const char*>(frame), flen, 0) == flen) {
            if (sent)
                *sent = 1;
            const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(replyMs);
            while (got < cap) {
                const ULONGLONG now = GetTickCount64();
                if (now >= end)
                    break;
                const ULONGLONG rem = end - now;
                to = static_cast<DWORD>(rem < static_cast<ULONGLONG>(ioMs)
                    ? rem : static_cast<ULONGLONG>(ioMs));
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                    reinterpret_cast<const char*>(&to), sizeof(to));
                const int r = recv(s, rep + got, cap - got, 0);
                if (r <= 0)
                    break;
                got += r;
                if (FrameState(rep, got, cap, nullptr) != 0)
                    break;
            }
        }
        closesocket(s);
        return got;
    }

    uint32_t BackoffMs(int fails, uint32_t base, uint32_t cap)
    {
        if (fails < 1)
            return 0;
        uint32_t d = base;
        for (int i = 1; i < fails && d < cap; i++)
            d *= 2;
        if (d > cap)
            d = cap;
        const uint32_t j = d / 4;
        return d - j + (j ? Rng() % (2 * j + 1) : 0);
    }

    void MarkAlive()
    {
        uint32_t t = static_cast<uint32_t>(GetTickCount());
        g_srvAliveT.store(t ? t : 1, std::memory_order_relaxed);
    }

    bool AliveWithin(uint32_t ms)
    {
        const uint32_t t = g_srvAliveT.load(std::memory_order_relaxed);
        return t && static_cast<uint32_t>(GetTickCount()) - t < ms;
    }

    void SetTestFlags(uint8_t flags)
    {
        g_testFlags.store(flags, std::memory_order_relaxed);
    }

    uint8_t TestFlags()
    {
        return g_testFlags.load(std::memory_order_relaxed);
    }

}
