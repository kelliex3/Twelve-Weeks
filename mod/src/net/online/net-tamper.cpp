#include "net-tamper.hpp"
#include "net-common.hpp"
#include "log.hpp"
#include "net-matchmaking.hpp"
#include "net-session.hpp"
#include "net-player.hpp"
#include "loader/anti-debugging.hpp"
#include "sid/sid.hpp"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace net::tamper {

    static constexpr uint64_t kDebugIntervalMs   = 3000;
    static constexpr uint64_t kDebugJitterMs     = 1000;
    static constexpr uint64_t kScanIntervalMs    = 30000;
    static constexpr uint64_t kScanJitterMs      = 10000;
    static constexpr uint64_t kFirstCheckDelayMs = 15000;
    static constexpr uint64_t kRereportMs        = 10 * 60 * 1000;
    static constexpr int      kMaxFound          = 4;
    static constexpr int      kProcSlots         = 8;
    static constexpr int      kPendingSlots      = 8;

    struct ProcRecord {
        uint64_t nameHash = 0;
        uint64_t lastReportMs = 0;
    };

    static uint64_t   g_startMs = 0;
    static uint64_t   g_nextDebugMs = 0;
    static uint64_t   g_nextScanMs = 0;
    static uint64_t   g_rng = 0;

    static uint16_t   g_reportedFlags = 0;
    static uint64_t   g_flagsLastReportMs = 0;
    static ProcRecord g_procs[kProcSlots];
    static int        g_procCount = 0;

    static MMTamper   g_pending[kPendingSlots];
    static int        g_pendingCount = 0;
    static int        g_pendingNext = 0;

    static uint64_t nextRandom()
    {
        if (g_rng == 0) {
            LARGE_INTEGER pc; QueryPerformanceCounter(&pc);
            g_rng = static_cast<uint64_t>(pc.QuadPart) ^ 0x9E3779B97F4A7C15ull;
            if (g_rng == 0) g_rng = 1;
        }
        g_rng ^= g_rng << 13;
        g_rng ^= g_rng >> 7;
        g_rng ^= g_rng << 17;
        return g_rng;
    }

    static uint64_t jittered(uint64_t base, uint64_t jitter)
    {
        return base - jitter + (nextRandom() % (2 * jitter + 1));
    }

    static void queuePending(const MMTamper& m)
    {
        for (int i = 0; i < g_pendingCount; ++i) {
            if (g_pending[i].kind == m.kind && memcmp(g_pending[i].detail, m.detail, sizeof(m.detail)) == 0) {
                g_pending[i] = m;
                return;
            }
        }
        if (g_pendingCount < kPendingSlots) {
            g_pending[g_pendingCount++] = m;
            return;
        }
        g_pending[g_pendingNext] = m;
        g_pendingNext = (g_pendingNext + 1) % kPendingSlots;
    }

    static void sendToServer(const MMTamper& m)
    {
#if TO1_LOCAL_MATCHMAKER || TO1_DEV_MATCHMAKER || !defined(TO1_DEBUG)
        if (!net::matchmaking::ReportTamper(m))
            queuePending(m);
#else
        (void)m;
#endif
    }

    static void flushPending()
    {
        if (g_pendingCount == 0 || !net::matchmaking::Authed()) return;
        for (int i = 0; i < g_pendingCount; ++i)
            net::matchmaking::ReportTamper(g_pending[i]);
        g_pendingCount = 0;
        g_pendingNext = 0;
    }

    static void applyLocal(uint16_t flags, const SuspiciousProcessEvent* proc)
    {
        NetPlayer* me = NetPlayers::Local();
        if (!me) return;
        if (flags) me->tamperFlags |= flags;
        if (proc) {
            me->tamperPid = proc->pid;
            memcpy(me->tamperProcess, proc->processName, sizeof(me->tamperProcess));
            me->tamperProcess[sizeof(me->tamperProcess) - 1] = 0;
        }
    }

    static bool sessionSendAllowed()
    {
        return net::session::IsRunning() && net::session::IsConnected() && !net::session::IsHost();
    }

    static void reportDebugger(uint16_t flags, uint64_t now)
    {
        TO1_LOG("[TAMPER] debugger flags=0x%04x\n", flags);
        g_reportedFlags |= flags;
        g_flagsLastReportMs = now;

        MMTamper m{};
        m.kind = MM_TAMPER_DEBUGGER;
        m.flags = flags;
        snprintf(m.detail, sizeof(m.detail), "0x%04x", flags);
        sendToServer(m);

        DebuggingEvent evt{ flags };
        if (sessionSendAllowed())
            net::session::SendEvent(NEVT_DEBUGGING, evt, 0);
        else if (net::session::IsHost())
            applyLocal(flags, nullptr);
    }

    static void reportProcess(const antidebug::FoundProcess& f, uint64_t now)
    {
        TO1_LOG("[TAMPER] process pid=%u name=%s\n", f.pid, f.name);

        MMTamper m{};
        m.kind = MM_TAMPER_PROCESS;
        m.pid = f.pid;
        memcpy(m.detail, f.name, sizeof(m.detail));
        m.detail[sizeof(m.detail) - 1] = 0;
        sendToServer(m);

        SuspiciousProcessEvent evt{};
        evt.pid = f.pid;
        memcpy(evt.processName, f.name, sizeof(evt.processName));
        evt.processName[sizeof(evt.processName) - 1] = 0;
        if (sessionSendAllowed())
            net::session::SendEvent(NEVT_SUSPICIOUS_PROCESS, evt, 0);
        else if (net::session::IsHost())
            applyLocal(0, &evt);
    }

    static void checkDebugger(uint64_t now)
    {
        const uint16_t flags = antidebug::DebuggerFlags();
        if (flags == 0) return;
        const bool fresh = (flags & ~g_reportedFlags) != 0;
        const bool stale = now - g_flagsLastReportMs >= kRereportMs;
        if (fresh || stale)
            reportDebugger(flags, now);
    }

    static ProcRecord& recordFor(uint64_t hash)
    {
        for (int i = 0; i < g_procCount; ++i)
            if (g_procs[i].nameHash == hash) return g_procs[i];
        if (g_procCount < kProcSlots) {
            g_procs[g_procCount] = ProcRecord{ hash, 0 };
            return g_procs[g_procCount++];
        }
        int oldest = 0;
        for (int i = 1; i < kProcSlots; ++i)
            if (g_procs[i].lastReportMs < g_procs[oldest].lastReportMs) oldest = i;
        g_procs[oldest] = ProcRecord{ hash, 0 };
        return g_procs[oldest];
    }

    static void checkProcesses(uint64_t now)
    {
        antidebug::FoundProcess found[kMaxFound];
        const int n = antidebug::ScanProcesses(found, kMaxFound);
        for (int i = 0; i < n; ++i) {
            ProcRecord& r = recordFor(ToStringId64(found[i].name));
            if (r.lastReportMs != 0 && now - r.lastReportMs < kRereportMs) continue;
            r.lastReportMs = now;
            reportProcess(found[i], now);
        }
    }

    void Tick()
    {
        const uint64_t now = GetTickCount64();
        if (g_startMs == 0) {
            g_startMs = now;
            g_nextDebugMs = now + kFirstCheckDelayMs;
            g_nextScanMs = now + kFirstCheckDelayMs + jittered(kScanJitterMs, kScanJitterMs / 2);
        }

        flushPending();

        if (now >= g_nextDebugMs) {
            g_nextDebugMs = now + jittered(kDebugIntervalMs, kDebugJitterMs);
            checkDebugger(now);
        }
        if (now >= g_nextScanMs) {
            g_nextScanMs = now + jittered(kScanIntervalMs, kScanJitterMs);
            checkProcesses(now);
        }
    }

}
