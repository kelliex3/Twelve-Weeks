// ported from tools/ext/immersion.c (NEAR MISS: MSG_SHOTRAY send/recv + ImmNmTick whizz)

#include "near-miss.hpp"

#include "alertsting.hpp"

#include "engine/flag.hpp"
#include "engine/object.hpp"
#include "engine/pcm.hpp"
#include "engine/raycast.hpp"
#include "engine/script.hpp"
#include "log.hpp"

#include "net/game/player-status.hpp"
#include "net/match/match.hpp"
#include "net/match/net-player.hpp"
#include "net/online/net-session.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdlib>

#ifdef T2R_DATA_BULLET_BY
#include "sp_bullet_by.h"   // generated (private): sb_blob, SB_OFF, SB_LEN, SB_RATE, SB_COUNT
#endif

namespace net::audio::near_miss {

    namespace {

        constexpr uint64_t kFreshMs     = 400;    // ext: ignore a line older than 400 ms
        constexpr uint64_t kDelayMs     = 120;    // ext: wait out the hit check before a whizz
        constexpr uint64_t kWhizCdMs    = 150;    // ext: 150 ms between whizzes
        constexpr uint64_t kHitCancelMs = 250;    // ext: a hit from that slot cancels the whizz
        constexpr uint64_t kSendCdMs    = 66;     // ext: at most ~15 lines a second
        constexpr uint64_t kLosMs       = 300;    // ext: refresh the shooter LOS verdicts
        constexpr float    kHeadUpM     = 0.6f;   // ext: head = own locator + 0.6 m on Y
        constexpr float    kMinAlong    = 1.0f;   // ext: behind / at the muzzle is not a near miss
        constexpr float    kMaxLat      = 2.5f;   // ext: live code (the header's ~1.5 m is stale)
        constexpr float    kMaxLen      = 120.0f; // ext: lenM == 0 -> unknown -> 120 m
        constexpr float    kLosRange    = 45.0f;  // ext: warm LOS only for shooters within 45 m
        constexpr int      kRing        = 16;     // ext SRAY_RING
        constexpr int      kMaxPending  = 4;      // ext pend[4]

        struct RayRec {
            std::atomic<uint32_t> seq{ 0 };        // 0 = unpublished; the publication marker
            int      slot = -1;
            uint64_t ms = 0;
            float    o[3] = {};
            float    d[3] = {};
            float    len = 0.0f;
        };

        struct Pending {
            uint64_t due = 0;
            uint64_t shot = 0;
            int      slot = -1;
            float    lat = 0.0f;
            float    along = 0.0f;
            float    cp[3] = {};
        };

        RayRec                  g_ring[kRing];
        std::atomic<uint32_t>   g_seq{ 0 };
        Pending                 g_pending[kMaxPending];
        int                     g_npending = 0;
        uint64_t                g_hitByMs[kMaxNetPlayers]  = {};
        bool                    g_losBlocked[kMaxNetPlayers] = {};   // unknown = clear (optimistic)
        uint64_t                g_losJob[kMaxNetPlayers]   = {};
        uint64_t                g_lastWhiz = 0, g_lastSend = 0, g_lastLosAll = 0;
        uint32_t                g_srayLast = 0;

        bool SenderOn() noexcept  { return engine::flag::Absent("t2r-no-shotray.txt"); }
        bool ReceiverOn() noexcept { return engine::flag::Absent("t2r-no-near-miss.txt"); }

        // Left/right pan of `cp` from the camera listener's point of view (ext SfxSpatial's
        // pan term). Only consumed on the private-data PCM path; the alert sting is 2D.
#ifdef T2R_DATA_BULLET_BY
        float PanOf(const float cp[3]) noexcept
        {
            Vector3 lp;
            if (!engine::raycast::CameraPosition(lp))
                return 0.0f;
            const Vector3* d = DC::Call<"camera-get-dir-through-reticle", const Vector3>();
            if (!d)
                return 0.0f;
            const float fx = d->x, fy = d->y, fz = d->z;
            const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
            if (!std::isfinite(fl) || fl < 0.01f)
                return 0.0f;
            // right = normalize(cross(forward, world up))
            float rx = -fz, ry = 0.0f, rz = fx;
            const float rl = std::sqrt(rx * rx + rz * rz);
            if (rl < 0.01f)
                return 0.0f;
            rx /= rl; rz /= rl;
            (void)fy;
            const float vx = cp[0] - lp.x, vy = cp[1] - lp.y, vz = cp[2] - lp.z;
            const float vl = std::sqrt(vx * vx + vy * vy + vz * vz);
            if (vl < 0.01f)
                return 0.0f;
            float pan = (vx * rx + vz * rz) / vl;
            if (pan < -1.0f) pan = -1.0f;
            if (pan > 1.0f) pan = 1.0f;
            return pan;
        }
#endif // T2R_DATA_BULLET_BY

        void WarmLos(const float head[3], uint64_t now)
        {
            if (now < g_lastLosAll)
                return;
            g_lastLosAll = now + kLosMs;
            const ::net::match::Match& m = ::net::match::Get();
            for (int s = 0; s < kMaxNetPlayers; ++s) {
                const NetPlayer& p = m.players[s];
                if (!p.active || !p.hasState || p.IsLocal()
                    || NetPlayers::SlotIsFriendly(s))
                    continue;
                const PlayerNetState& ps = p.State();
                const float dx = ps.posX - head[0], dz = ps.posZ - head[2];
                if (!std::isfinite(dx + dz) || dx * dx + dz * dz > kLosRange * kLosRange) {
                    // Out of warm range: no fresh LOS verdict, so fall back to optimistic
                    // (a wall verdict left over from when they were close would wrongly
                    // cancel a legitimate near miss). Ext plays when it has no LOS either.
                    g_losJob[s] = 0;
                    g_losBlocked[s] = false;
                    continue;
                }
                g_losJob[s] = engine::raycast::KickWorldRay(
                    Vector3(head[0], head[1], head[2]),
                    Vector3(ps.posX, ps.posY + kHeadUpM, ps.posZ),
                    engine::raycast::kMaskStaticWorld, SID("player"));
            }
        }

        void ReadLos()
        {
            for (int s = 0; s < kMaxNetPlayers; ++s) {
                if (!g_losJob[s])
                    continue;
                const float rt = engine::raycast::RayT(g_losJob[s]);
                if (rt == -1.0f)
                    continue;                       // result pending: keep the last verdict
                if (rt >= 0.0f)
                    g_losBlocked[s] = rt < 0.98f;   // a wall between the shooter and us
                else
                    g_losBlocked[s] = false;        // no result / refused: optimistic clear
                g_losJob[s] = 0;
            }
        }

    } // namespace

    void OnLocalShot()
    {
        if (!net::player_status::MatchLive() || !SenderOn())
            return;
        const uint64_t now = GetTickCount64();
        if (g_lastSend && now - g_lastSend < kSendCdMs)
            return;

        Vector3 cam;
        if (!engine::raycast::CameraPosition(cam))
            return;
        const Vector3* dp = DC::Call<"camera-get-dir-through-reticle", const Vector3>();
        if (!dp)
            return;
        float d[3] = { dp->x, dp->y, dp->z };
        const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (!std::isfinite(dd) || dd < 0.25f || dd > 4.0f)
            return;
        const float inv = 1.0f / std::sqrt(dd);
        for (int j = 0; j < 3; ++j)
            d[j] *= inv;

        float w[8];
        if (!engine::object::LocalPlayerWorld(w)
            || !std::isfinite(w[0] + w[1] + w[2]))
            return;
        float o[3] = { cam.x, cam.y, cam.z };
        const float chest[3] = { w[0], w[1] + kHeadUpM, w[2] };
        float t = (chest[0] - o[0]) * d[0]
                + (chest[1] - o[1]) * d[1]
                + (chest[2] - o[2]) * d[2];
        if (t < 0.0f) t = 0.0f;
        if (t > 6.0f) t = 6.0f;
        for (int j = 0; j < 3; ++j)
            o[j] += t * d[j];
        if (!std::isfinite(o[0] + o[1] + o[2])
            || std::fabs(o[0]) > 1e5f || std::fabs(o[1]) > 1e5f || std::fabs(o[2]) > 1e5f)
            return;

        ShotRayEvent evt{};
        evt.reserved = 0;
        for (int j = 0; j < 3; ++j)
            evt.origin[j] = o[j];
        for (int j = 0; j < 3; ++j)
            evt.dir[j] = QuantDir(d[j]);
        evt.lenM = 0;      // no cheap synchronous hit distance: receiver uses 120 m
        net::session::SendEvent(NEVT_SHOTRAY, evt);
        g_lastSend = now;
    }

    void OnShotRay(int slot, const ShotRayEvent& evt)
    {
        if (slot < 0 || slot >= kMaxNetPlayers)
            return;
        const float o[3] = { evt.origin[0], evt.origin[1], evt.origin[2] };
        const float d[3] = { DequantDir(evt.dir[0]), DequantDir(evt.dir[1]), DequantDir(evt.dir[2]) };
        if (!std::isfinite(o[0] + o[1] + o[2])
            || std::fabs(o[0]) > 1e5f || std::fabs(o[1]) > 1e5f || std::fabs(o[2]) > 1e5f
            || !DirValid(d))
            return;
        const float inv = 1.0f / std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);

        const uint32_t i = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
        RayRec& r = g_ring[i & (kRing - 1)];
        r.seq.store(0, std::memory_order_relaxed);  // unpublish while filling
        r.slot = slot;
        r.ms = GetTickCount64();
        for (int j = 0; j < 3; ++j) {
            r.o[j] = o[j];
            r.d[j] = d[j] * inv;
        }
        r.len = evt.lenM ? static_cast<float>(evt.lenM) : kMaxLen;
        r.seq.store(i, std::memory_order_release);  // publish after all fields are filled
    }

    void NoteHitFrom(int slot, uint64_t nowMs) noexcept
    {
        if (slot >= 0 && slot < kMaxNetPlayers)
            g_hitByMs[slot] = nowMs;
    }

    void Tick()
    {
        const uint64_t now = GetTickCount64();
        const uint32_t end = g_seq.load(std::memory_order_acquire);
        const bool live = net::player_status::MatchLive();
        const bool nmOn = ReceiverOn();
        if (!live || !nmOn) {
            g_npending = 0;
            g_srayLast = end;
            return;
        }

        float w[8];
        float mp[3];
        const bool mpOk = engine::object::LocalPlayerWorld(w)
                       && std::isfinite(w[0] + w[1] + w[2]);
        if (mpOk) {
            mp[0] = w[0]; mp[1] = w[1]; mp[2] = w[2];
        }
        const float head[3] = { mp[0], mp[1] + kHeadUpM, mp[2] };
        const ::net::match::Match& m = ::net::match::Get();
        const NetPlayer* local = NetPlayers::Local();

        // Whizzes that waited out the hit check. A hit from that slot within 250 ms of the
        // line means that shot connected, so no whizz. The alert sting fires regardless of
        // whether the private PCM wave was available (ext behaviour).
        for (int i = 0; i < g_npending;) {
            if (now < g_pending[i].due) {
                ++i;
                continue;
            }
            const int slot = g_pending[i].slot;
            const bool hit = g_hitByMs[slot] != 0
                          && g_hitByMs[slot] + kHitCancelMs >= g_pending[i].shot;
            if (!hit && now - g_lastWhiz >= kWhizCdMs) {
#ifdef T2R_DATA_BULLET_BY
                const float gain = WhizzGain(g_pending[i].lat, g_pending[i].along);
                const float pan = PanOf(g_pending[i].cp);
                const int wav = static_cast<int>(std::rand() % SB_COUNT);
                const float th = (pan + 1.0f) * 0.78539816f;   // ext SfxLr equal-power pan
                const float gl = gain * 1.41421356f * std::cos(th);
                const float gr = gain * 1.41421356f * std::sin(th);
                const engine::pcm::Voice v = engine::pcm::Play(
                    sb_blob + SB_OFF[wav], SB_LEN[wav], gl, gr, SB_RATE);
                if (engine::pcm::Live(v))
                    g_lastWhiz = now;
#endif
                net::audio::alertsting::Combat("near miss out of the quiet");
            }
            g_pending[i] = g_pending[--g_npending];
        }

        if (!mpOk) {
            g_srayLast = end;
            return;
        }

        WarmLos(head, now);
        ReadLos();

        // New replicated shot lines. Iterate the ring by sequence, tolerating wraps / gaps.
        if (end - g_srayLast > static_cast<uint32_t>(kRing))
            g_srayLast = end - static_cast<uint32_t>(kRing);
        for (uint32_t q = g_srayLast + 1; q - 1 != end; ++q) {
            const RayRec& r = g_ring[q & (kRing - 1)];
            if (r.seq.load(std::memory_order_acquire) != q)
                continue;
            const int slot = r.slot;
            if (slot < 0 || slot >= kMaxNetPlayers || slot == (local ? local->netSlot : -1))
                continue;
            const NetPlayer& p = m.players[slot];
            if (!p.active || p.IsLocal() || NetPlayers::SlotIsFriendly(slot))
                continue;
            if (now - r.ms > kFreshMs || g_npending >= kMaxPending
                || now - g_lastWhiz < kWhizCdMs)
                continue;
            const float o[3] = { r.o[0], r.o[1], r.o[2] };
            const float d[3] = { r.d[0], r.d[1], r.d[2] };
            const ClosestApproach a = ClosestPointOnSegment(o, d, r.len, head);
            if (!ShotIsNear(a, kMinAlong, kMaxLat))
                continue;
            if (g_losBlocked[slot])
                continue;                           // a wall between shooter and us
            Pending& pnd = g_pending[g_npending++];
            pnd.due = now + kDelayMs;
            pnd.shot = r.ms;
            pnd.slot = slot;
            pnd.lat = a.lat;
            pnd.along = a.t;
            for (int j = 0; j < 3; ++j)
                pnd.cp[j] = a.cp[j];
        }
        g_srayLast = end;
    }

    void Reset()
    {
        g_npending = 0;
        g_seq.store(0, std::memory_order_relaxed);
        g_srayLast = 0;
        g_lastWhiz = g_lastSend = g_lastLosAll = 0;
        for (int i = 0; i < kRing; ++i)
            g_ring[i].seq.store(0, std::memory_order_relaxed);
        for (int s = 0; s < kMaxNetPlayers; ++s) {
            g_hitByMs[s] = 0;
            g_losJob[s] = 0;
            g_losBlocked[s] = false;
        }
    }

} // namespace net::audio::near_miss
