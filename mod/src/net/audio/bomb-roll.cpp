// ported from tools/ext/bomb_roll.c (br_feed / br_impact / br_roll / br_steer).
// All rules live in bomb-roll-core.hpp; the engine / audio / net / clock seams stay here.

#include "bomb-roll.hpp"

#include "bomb-roll-pcm.hpp"

#include "boot.hpp"
#include "log.hpp"               // script.hpp's template diagnostics use TO1_LOG
#include "engine/flag.hpp"
#include "engine/object.hpp"
#include "engine/script.hpp"     // DcValue, DC::GetFunctionAddress / DC::Call

#include <safetyhook.hpp>

#include <windows.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace net::audio::bomb_roll {

    namespace {

        namespace core = net::audio::bomb_roll::core;
        namespace sink = net::audio::bomb_roll::pcm;

        core::State g;

        // The net slot SetNextFeedSlot(int) stamped for the next projectile / Feed.
        // -1 is our own throw, exactly like slot < 0 in the ext.
        thread_local int t_nextSlot = -1;

        int NextFeedSlot() noexcept { return t_nextSlot; }

        void NoteHookFailure(const char* why) noexcept
        {
            static bool logged = false;
            (void)why;                          // TO1_LOG is compiled out in release
            if (logged)
                return;
            logged = true;
            TO1_LOG("[BOMBROLL] throw-projectile hook not installed (%s); "
                    "Watch() stays available\n", why);
        }

        // ---- one-time native throw hook -------------------------------------------

        SafetyHookInline s_throwHook;
        bool s_hookTried = false;

        // ext's DcCallFunc: DcValue __fastcall(int argc, DcValue* argv, uint64_t second).
        // Return val[0] is the spawned projectile SID; argv[2] is the thrown item SID.
        DcValue __fastcall ThrowProjectileHook(int argc, DcValue* argv,
                                              uint64_t second) noexcept
        {
            DcValue ret = s_throwHook.fastcall<DcValue>(argc, argv, second);
            const uint64_t sid = ret.val[0];
            const int kind = (argv && argc > 2) ? KindFromSid(argv[2].val[0]) : 0;
            if (sid && kind)
                Watch(sid, kind, NextFeedSlot());
            t_nextSlot = -1;                    // one-shot, even when this throw is not ours
            return ret;
        }

        // ---- owner-side helper tables ---------------------------------------------

        struct WatchEntry {
            uint64_t h = 0;
            int      kind = 0;
            int      slot = -1;
            uint64_t seenMs = 0;
        };

        struct ClockEntry {
            uint64_t h = 0;
            double   q = 0.0;                  // QPC stamp of the last feed of this handle
        };

        struct Pending {
            core::FrameResult result;
            uint64_t          h = 0;
            int               kind = 0;
        };

        constexpr int kQueueMax = 16;           // at most 16 results / 32 cues per tick

        std::array<WatchEntry, core::kRing> g_watch{};
        std::array<ClockEntry, core::kRing> g_clock{};
        std::array<Pending, kQueueMax>      g_pending{};
        int                                 g_pendingN = 0;
        int                                 g_logN = 0;
        bool                                g_configured = false;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }
        bool Off() noexcept
        {
            return engine::flag::Present("t2r-no-bomb-roll.txt", 120);
        }
        bool NativeOn() noexcept
        {
            return engine::flag::Present("t2r-bomb-roll-native.txt", 120);
        }
        bool ValidKind(int kind) noexcept
        {
            return kind == static_cast<int>(core::Kind::PipeBomb)
                || kind == static_cast<int>(core::Kind::StunSmoke)
                || kind == static_cast<int>(core::Kind::Molotov);
        }

        void EnsureConfigured() noexcept
        {
            if (g_configured)
                return;
            for (int i = 0; i < core::kGroupCount; ++i) {
                const core::WaveGroup group = static_cast<core::WaveGroup>(i);
                core::SetWaveCount(g, group, sink::WaveCount(group));
            }
            g.baseRate = sink::Rate();
            g_configured = true;
        }

        double QpcNow() noexcept
        {
            LARGE_INTEGER q{};
            QueryPerformanceCounter(&q);
            return static_cast<double>(q.QuadPart);
        }

        double QpcInvFreq() noexcept
        {
            static double inv = 0.0;
            if (inv == 0.0) {
                LARGE_INTEGER f{};
                if (QueryPerformanceFrequency(&f) && f.QuadPart > 0)
                    inv = 1.0 / static_cast<double>(f.QuadPart);
            }
            return inv;
        }

        // Find / age one clock record, ext-style: exact handle, else free / stale,
        // else the least recently stamped one. The old QPC value is returned.
        double StampClock(uint64_t h, double q) noexcept
        {
            ClockEntry* hit = nullptr;
            ClockEntry* free = nullptr;
            ClockEntry* old = &g_clock[0];
            for (ClockEntry& c : g_clock) {
                if (c.h == h)
                    hit = &c;
                if (!c.h)
                    free = &c;
                if (c.q < old->q)
                    old = &c;
            }
            ClockEntry& e = hit ? *hit : (free ? *free : *old);
            const double prev = e.h == h ? e.q : 0.0;
            e.h = h;
            e.q = q;
            return prev;
        }

        void ClearClock(uint64_t h) noexcept
        {
            for (ClockEntry& c : g_clock)
                if (c.h == h)
                    c = ClockEntry{};
        }

        WatchEntry& SlotForWatch(uint64_t h, int kind, int slot, uint64_t now) noexcept
        {
            WatchEntry* hit = nullptr;
            WatchEntry* free = nullptr;
            WatchEntry* old = &g_watch[0];
            for (WatchEntry& w : g_watch) {
                if (w.h == h)
                    hit = &w;
                if (!w.h || now - w.seenMs > core::kStaleMs) {
                    if (!free)
                        free = &w;
                }
                if (w.seenMs < old->seenMs)
                    old = &w;
            }
            WatchEntry& e = hit ? *hit : (free ? *free : *old);
            if (e.h && e.h != h) {
                const uint64_t previous = e.h;
                core::Forget(g, previous);
                ClearClock(previous);
            }
            e.h = h;
            e.kind = kind;
            e.slot = slot;
            e.seenMs = now;
            return e;
        }

        bool Watched(uint64_t h) noexcept
        {
            for (const WatchEntry& w : g_watch)
                if (w.h == h)
                    return true;
            return false;
        }

        void DropPending(uint64_t h) noexcept
        {
            int keep = 0;
            for (int i = 0; i < g_pendingN; ++i) {
                if (g_pending[static_cast<std::size_t>(i)].h == h)
                    continue;
                if (keep != i)
                    g_pending[static_cast<std::size_t>(keep)] = g_pending[static_cast<std::size_t>(i)];
                ++keep;
            }
            g_pendingN = keep;
        }

        void DropHandle(uint64_t h) noexcept
        {
            core::Forget(g, h);
            ClearClock(h);
            DropPending(h);
            for (WatchEntry& w : g_watch)
                if (w.h == h)
                    w = WatchEntry{};
        }

        void QueueResult(const core::FrameResult& r, uint64_t h, int kind) noexcept
        {
            if (!r.n)
                return;
            if (g_pendingN >= kQueueMax) {
                for (int i = 1; i < g_pendingN; ++i)
                    g_pending[static_cast<std::size_t>(i - 1)] = g_pending[static_cast<std::size_t>(i)];
                --g_pendingN;
            }
            Pending& p = g_pending[static_cast<std::size_t>(g_pendingN++)];
            p.result = r;
            p.h = h;
            p.kind = kind;
        }

        // One br_feed with the owner's clocks. dt is QPC seconds; nowMs drives every
        // ext cooldown / staleness check. Cues are queued for Tick(), not played here.
        void FeedImpl(uint64_t h, int kind, int slot, const float pos[3],
                      uint64_t nowMs, double q) noexcept
        {
            EnsureConfigured();
            const double prev = StampClock(h, q);
            const double dt = (prev > 0.0 && q >= prev)
                ? (q - prev) * QpcInvFreq() : 0.0;
            QueueResult(core::Feed(g, h, kind, slot, pos, nowMs, dt), h, kind);
        }

        bool LogCapped() noexcept
        {
            return g_logN < 160 ? (++g_logN, true) : false;
        }

    } // namespace

    void Install()
    {
        if (ExtOwns())
            return;
        if (s_hookTried)
            return;
        s_hookTried = true;
        t_nextSlot = -1;

        const std::uintptr_t address = DC::GetFunctionAddress<"throw-projectile">();
        if (!address || address == ~static_cast<std::uintptr_t>(0)) {
            NoteHookFailure("DC address missing");
            return;                             // Watch() still works for net paths
        }
        try {
            s_throwHook = safetyhook::create_inline(
                reinterpret_cast<void*>(address),
                reinterpret_cast<void*>(&ThrowProjectileHook));
        } catch (...) {
            NoteHookFailure("create_inline threw");
            return;
        }
        if (!s_throwHook) {
            NoteHookFailure("address already hooked or unsupported");
            return;
        }
        TO1_LOG("[BOMBROLL] hooked throw-projectile at %p\n",
                reinterpret_cast<void*>(address));
    }

    void SetNextFeedSlot(int slot)
    {
        if (ExtOwns())
            return;
        t_nextSlot = slot;
    }

    void Watch(uint64_t handle, int kind, int slot)
    {
        if (ExtOwns() || !handle || !ValidKind(kind))
            return;
        (void)SlotForWatch(handle, kind, slot, GetTickCount64());
    }

    void Unwatch(uint64_t handle)
    {
        if (ExtOwns() || !handle)
            return;
        DropHandle(handle);
    }

    void Feed(uint64_t h, int kind, int slot, const float pos[3])
    {
        if (ExtOwns())
            return;
        if (Off() || !h || !pos || !ValidKind(kind)
            || !std::isfinite(pos[0] + pos[1] + pos[2])) {
            t_nextSlot = -1;
            return;
        }
        if (slot < 0)
            slot = NextFeedSlot();
        t_nextSlot = -1;
        if (Watched(h))
            return;                             // Tick() owns this handle's clock
        FeedImpl(h, kind, slot, pos, GetTickCount64(), QpcNow());
    }

    void Steer(uint64_t h)
    {
        if (ExtOwns() || !h)
            return;
        core::Steer(g, h);
    }

    void Tick()
    {
        if (ExtOwns())
            return;
        if (Off()) {
            g_pendingN = 0;
            return;
        }
        EnsureConfigured();
        const uint64_t now = GetTickCount64();

        // Watch-list sampler: the ext saw every projectile through ob_tick / the
        // g_rproj loop; here the net path registers a handle and we read its world
        // position. A missing / non-finite locator drops the handle, as does staleness.
        for (const WatchEntry& entry : g_watch) {
            if (!entry.h)
                continue;
            if (now - entry.seenMs > core::kStaleMs) {
                DropHandle(entry.h);
                continue;
            }
            float w[8] = {};
            if (!engine::object::EntityWorld(static_cast<StringId64>(entry.h), w)
                || !std::isfinite(w[0] + w[1] + w[2])) {
                DropHandle(entry.h);
                continue;
            }
            const float pos[3] = { w[0], w[1], w[2] };
            FeedImpl(entry.h, entry.kind, entry.slot, pos, now, QpcNow());
        }

        // ext br_impact's A/B probe: try the SP event on the bomb and skip our wave
        // when the engine accepted it (a missing native answers ~0ULL). Rolls never
        // probe, and molotov has no SP event in this pair.
        const bool native = NativeOn();
        for (int i = 0; i < g_pendingN; ++i) {
            const Pending& p = g_pending[static_cast<std::size_t>(i)];
            bool acceptedByEngine = false;
            if (native && p.result.type == core::FrameResult::Type::Impact) {
                const char* ev = nullptr;
                if (p.kind == static_cast<int>(core::Kind::StunSmoke))
                    ev = "sfx-stunbomb-bounce";
                else if (p.kind == static_cast<int>(core::Kind::PipeBomb))
                    ev = "sfx-pipe-bomb-imp";
                if (ev) {
                    const uint64_t r = DC::Call<"play-sound-at-joint">(
                        SID(ev), p.h, 0ull, 0ull, 0ull);
                    acceptedByEngine = r != ~0ULL;
                    if (LogCapped())
                        TO1_LOG("[BOMBROLL] native %s on %llx -> %llx%s\n", ev,
                                static_cast<unsigned long long>(p.h),
                                static_cast<unsigned long long>(r),
                                acceptedByEngine ? " (wave skipped)" : "");
                }
            }
            if (acceptedByEngine)
                continue;
            for (int c = 0; c < p.result.n; ++c)
                (void)sink::Play(p.result.cues[static_cast<std::size_t>(c)]);
        }
        g_pendingN = 0;
    }

    void Reset()
    {
        if (ExtOwns())
            return;
        core::Reset(g);
        g_watch.fill(WatchEntry{});
        g_clock.fill(ClockEntry{});
        g_pending.fill(Pending{});
        g_pendingN = 0;
        t_nextSlot = -1;
        // The throw hook and the data configuration stay installed / cached; Reset
        // clears runtime state only (matches replication's level-reset helper).
    }

    int KindFromSid(uint64_t sid) noexcept
    {
        if (ExtOwns())
            return 0;
        if (sid == 0xcf576d288d014eacULL)          // nail / pipe bomb
            return static_cast<int>(core::Kind::PipeBomb);
        if (sid == 0x8502a9c43b674354ULL           // stun bomb
            || sid == 0xa2aa922866d0479dULL)       // smoke bomb
            return static_cast<int>(core::Kind::StunSmoke);
        if (sid == 0x2a3164cb4a1c95d9ULL)          // molotov
            return static_cast<int>(core::Kind::Molotov);
        return 0;
    }

    bool Active() noexcept
    {
        if (ExtOwns())
            return false;
        const uint64_t now = GetTickCount64();
        for (const core::Tracker& t : g.slots)
            if (t.h && now - t.seenMs <= core::kStaleMs)
                return true;
        return false;
    }

} // namespace net::audio::bomb_roll
