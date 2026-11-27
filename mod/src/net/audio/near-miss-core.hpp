#pragma once

// Pure core for the immersion.c near-miss port: wire quantisation, closest approach,
// ring / pending state machine, hit cancellation, LOS verdicts, pan and gain. No engine,
// audio, match or clock calls live here; near-miss.cpp supplies all of those. Tests:
// mod/tests/near_miss_core_test.cpp.

#include "net/protocol/net-protocol.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace net::audio::near_miss::core {

    // ext immersion.c constants. The LOS ones are from t2r-ext.c's shared los_gate.
    inline constexpr uint64_t kFreshMs     = 400;    // ignore a line older than 400 ms
    inline constexpr uint64_t kDelayMs     = 120;    // wait out the hit check before a whizz
    inline constexpr uint64_t kWhizCdMs    = 150;    // between whizzes
    inline constexpr uint64_t kHitCancelMs = 250;    // a hit from that slot cancels the whizz
    inline constexpr uint64_t kSendCdMs    = 66;     // at most ~15 lines a second
    inline constexpr uint64_t kFireStampMs = 300;    // ext: a clip drop counts only with fire this recent
    inline constexpr uint64_t kLosStaleT60 = 90;     // los_gate: forget a target after 1.5 s
    inline constexpr uint64_t kLosKickT60  = 9;      // los_gate: re-kick every 150 ms
    inline constexpr uint64_t kLosWarmMs   = 300;    // ext imm_nm_tick: warm nearby verdicts every 300 ms
    inline constexpr float    kHeadUpM     = 0.6f;   // head = own root + 0.6 m on Y
    inline constexpr float    kLosEyeUpM   = 1.5f;   // los_gate ray start = our root + 1.5 m
    inline constexpr float    kLosChestUpM = 0.6f;   // los_gate first target height
    inline constexpr float    kLosHeadUpM  = 1.3f;   // los_gate second target height
    inline constexpr float    kMinAlong    = 1.0f;   // behind / at the muzzle is not a near miss
    inline constexpr float    kMaxLat      = 2.5f;   // live ext code (the header's ~1.5 m is stale)
    inline constexpr float    kMaxLen      = 120.0f; // ext: lenM == 0 -> unknown -> 120 m
    inline constexpr float    kLosRange    = 45.0f;  // warm only shooters within 45 m
    inline constexpr float    kLosCloseM   = 0.8f;   // los_gate: closer than this is open
    inline constexpr float    kLosBlockedT = 0.85f;  // veto only when BOTH rays hit before this
    inline constexpr float    kLosInsideT  = 0.02f;  // both at the origin: inside a collider
    inline constexpr float    kAimDirMinSq = 0.25f;  // ext aim helper direction sanity (min |d|^2)
    inline constexpr float    kAimDirMaxSq = 4.0f;   // ext aim helper direction sanity (max |d|^2)
    inline constexpr float    kAimSlideMax = 6.0f;   // ext: muzzle slide along the ray, at most 6 m
    inline constexpr int      kRing        = 16;     // ext SRAY_RING
    inline constexpr int      kLosEntries  = 8;      // ext g_los[8]
    inline constexpr int      kMaxPending  = 4;      // ext pend[4]

    // ---- geometry / wire ----------------------------------------------------------

    struct ClosestApproach {
        float t;        // projection along the ray from origin (upper-clamped to len)
        float lat;      // lateral distance from head to the closest point
        float cp[3];    // closest point on the segment
    };

    // Closest approach of the segment o + t*d, t in [0, len], to `head`. `d` must be unit
    // length. The lower bound (t >= 1 m from the muzzle) is a policy gate applied by
    // ShotIsNear, kept out of the geometry so it stays pure.
    inline ClosestApproach ClosestPointOnSegment(
        const float o[3], const float d[3], float len, const float head[3]) noexcept
    {
        float t = (head[0] - o[0]) * d[0]
                + (head[1] - o[1]) * d[1]
                + (head[2] - o[2]) * d[2];
        if (t > len)
            t = len;
        const float cp[3] = { o[0] + t * d[0], o[1] + t * d[1], o[2] + t * d[2] };
        const float dx = head[0] - cp[0];
        const float dy = head[1] - cp[1];
        const float dz = head[2] - cp[2];
        return { t, std::sqrt(dx * dx + dy * dy + dz * dz), { cp[0], cp[1], cp[2] } };
    }

    inline bool ShotIsNear(const ClosestApproach& a,
                           float minAlong = kMinAlong, float maxLat = kMaxLat) noexcept
    {
        return a.t >= minAlong && a.lat <= maxLat;
    }

    // int8 direction quantisation used on the wire (d * 127, rounded away from zero).
    inline int8_t QuantDir(float d) noexcept
    {
        const float v = d * 127.0f + (d < 0.0f ? -0.5f : 0.5f);
        if (v >= 127.0f) return 127;
        if (v <= -127.0f) return -127;
        return static_cast<int8_t>(static_cast<int>(v));
    }

    inline float DequantDir(int8_t q) noexcept
    {
        return static_cast<float>(q) / 127.0f;
    }

    inline bool DirValid(const float d[3]) noexcept
    {
        const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        return dd >= 0.5f && dd <= 1.5f;
    }

    inline bool ShotValid(const float o[3], const float d[3]) noexcept
    {
        return std::isfinite(o[0] + o[1] + o[2])
            && std::fabs(o[0]) <= 1e5f && std::fabs(o[1]) <= 1e5f && std::fabs(o[2]) <= 1e5f
            && DirValid(d);
    }

    // ext imm_sray_send_tick's aim-helper validation: finite origin + direction, with
    // |d|^2 in [0.25, 4] (the ext normalises afterwards).
    inline bool AimRayValid(const float o[3], const float d[3]) noexcept
    {
        const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        return std::isfinite(o[0] + o[1] + o[2] + d[0] + d[1] + d[2])
            && dd >= kAimDirMinSq && dd <= kAimDirMaxSq;
    }

    // Slide the camera-ray origin to the point on the ray nearest our chest, clamped to
    // [0, 6] m. The ext skips the slide entirely when no finite local root is available.
    inline void SlideOriginToChest(float o[3], const float d[3], const float chest[3]) noexcept
    {
        float t = (chest[0] - o[0]) * d[0]
                + (chest[1] - o[1]) * d[1]
                + (chest[2] - o[2]) * d[2];
        if (t < 0.0f) t = 0.0f;
        if (t > kAimSlideMax) t = kAimSlideMax;
        for (int j = 0; j < 3; ++j)
            o[j] += t * d[j];
    }

    // ext's "shot" detection: the held clip went down on the SAME weapon, with a fire
    // state seen in the last 300 ms. A reload / weapon swap never counts.
    inline bool ClipDecrementShot(int prevClip, uint64_t weapon, uint64_t prevWeapon,
                                  int clip, uint64_t lastFire, uint64_t now) noexcept
    {
        return prevClip >= 0 && weapon == prevWeapon && clip < prevClip
            && lastFire != 0 && now - lastFire < kFireStampMs;
    }

    // ext immersion.c: full within 0.25 m, 0.45x at 2.5 m, and the unfloored distance tail.
    // The ext does not clamp the past-140 m tail negative; keep that exact (a lenM of 255
    // can reach it), so no silent divergence is hidden here.
    inline float WhizzGain(float lat, float along) noexcept
    {
        float lf = (lat - 0.25f) / 2.25f;
        if (lf < 0.0f) lf = 0.0f;
        if (lf > 1.0f) lf = 1.0f;
        const float rf = along < 10.0f ? 1.0f : 1.0f - (along - 10.0f) / 130.0f;
        return 0.95f * (1.0f - 0.55f * lf) * rf;
    }

    // ext's default camera-right estimate is (fwd.z, 0, -fwd.x); its one-time screen-probe
    // sign calibration is unverified until a camera is up, so use that default sign. The
    // listener source is supplied by the owner (render camera first, like SfxSpatial).
    inline float PanOf(const float cp[3], const float listener[3],
                       const float fwd[3]) noexcept
    {
        const float rx = fwd[2], rz = -fwd[0];
        const float rl = std::sqrt(rx * rx + rz * rz);
        const float vx = cp[0] - listener[0];
        const float vz = cp[2] - listener[2];
        const float hl = std::sqrt(vx * vx + vz * vz);
        if (rl < 1e-3f || hl < 0.5f)
            return 0.0f;                        // sfx_spatial returns clear with pan 0
        float pan = (vx * rx + vz * rz) / (hl * rl);
        if (pan > 0.85f) pan = 0.85f;
        if (pan < -0.85f) pan = -0.85f;
        return pan;
    }

    // ext SfxLr: equal-power, centre = 1.0 on both channels.
    inline void PanLr(float gain, float pan, float& l, float& r) noexcept
    {
        const float th = (pan + 1.0f) * 0.78539816f;
        l = gain * 1.41421356f * std::cos(th);
        r = gain * 1.41421356f * std::sin(th);
    }

    // The ext's shared xorshift32 (t2r_rand), seeded from the clock on first use.
    inline uint32_t NextRand(uint64_t& state, uint64_t nowMs) noexcept
    {
        uint64_t x = state;
        if (!x)
            x = (nowMs * 0x9E3779B97F4A7C15ULL) | 1ULL;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state = x;
        return static_cast<uint32_t>(x >> 33);
    }

    inline int PickWave(uint64_t& state, uint64_t nowMs, int count) noexcept
    {
        return count > 0 ? static_cast<int>(NextRand(state, nowMs)
                                            % static_cast<uint32_t>(count)) : 0;
    }

    // ---- state machine ------------------------------------------------------------

    // ext g_sray_ring[]: `seq` is the publish flag AND the sequence number. 0 means the
    // record is being filled; the writer stores it last (release) after all other fields.
    // `slot` is the peer slot the ext stored as uint32_t (T2R_MAX_PEERS-validated by the
    // caller), never a sentinel.
    struct ShotRecord {
        std::atomic<uint32_t> seq{ 0 };
        uint32_t slot = 0;
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

    // ext's los_ent, keyed by a caller-supplied target handle (the native owner passes
    // NetPlayer::npcSid / the puppet handle). Unknown verdict (-1) is CLOSED, exactly
    // like the ext; a wall needs two agreeing rays.
    struct LosEntry {
        uint64_t handle = 0;
        uint64_t j1 = 0, j2 = 0;
        uint64_t kickFrame = 0, lastKick = 0, lastUse = 0;
        int      verdict = -1;
        int      streak = 0;
    };

    struct State {
        std::array<ShotRecord, kRing>   ring{};
        std::atomic<uint32_t>           seq{ 0 };  // ext's interlocked g_sray_ri
        uint32_t                        seen = 0;  // ext's sr_last (sequence, not a slot)
        std::array<Pending, kMaxPending> pending{};
        int                             nPending = 0;
        std::array<uint64_t, kMaxNetPlayers> hitByMs{};
        std::array<LosEntry, kLosEntries>   los{};
        uint64_t                        lastWhiz = 0, lastSend = 0;
        uint64_t                        nextLos = 0;
        uint64_t                        rng = 0;
    };

    inline bool SendReady(const State& s, uint64_t now) noexcept
    {
        return s.lastSend == 0 || now - s.lastSend >= kSendCdMs;
    }

    inline void NoteSent(State& s, uint64_t now) noexcept { s.lastSend = now; }

    inline bool WarmReady(const State& s, uint64_t now) noexcept { return now >= s.nextLos; }
    inline void NoteWarmed(State& s, uint64_t now) noexcept { s.nextLos = now + kLosWarmMs; }

    // Snapshot of the interlocked write index (ext: `uint32_t send = g_sray_ri`).
    inline uint32_t RingEnd(const State& s) noexcept
    {
        return s.seq.load(std::memory_order_acquire);
    }

    inline void StoreShot(State& s, uint32_t slot, uint64_t now,
                          const float o[3], const float d[3], float len) noexcept
    {
        // InterlockedIncrement equivalent: the returned index is 1-based and monotonic.
        const uint32_t i = s.seq.fetch_add(1, std::memory_order_relaxed) + 1u;
        ShotRecord& r = s.ring[i & (kRing - 1)];
        r.seq.store(0, std::memory_order_relaxed);   // unpublish while filling
        r.slot = slot;
        r.ms = now;
        for (int j = 0; j < 3; ++j) {
            r.o[j] = o[j];
            r.d[j] = d[j];
        }
        r.len = len;
        r.seq.store(i, std::memory_order_release);   // publish after all fields are filled
    }

    // int overload kept for the existing tests / callers; the wire slot is uint32_t.
    inline void StoreShot(State& s, int slot, uint64_t now,
                          const float o[3], const float d[3], float len) noexcept
    {
        StoreShot(s, static_cast<uint32_t>(slot), now, o, d, len);
    }

    inline void RewindSeen(State& s, uint32_t end) noexcept
    {
        if (end - s.seen > static_cast<uint32_t>(kRing))
            s.seen = end - static_cast<uint32_t>(kRing);
    }

    inline const ShotRecord& At(const State& s, uint32_t q) noexcept
    {
        return s.ring[q & (kRing - 1)];
    }

    inline bool RingLive(const State& s, uint32_t q) noexcept
    {
        return At(s, q).seq.load(std::memory_order_acquire) == q;
    }

    inline void AdvanceSeen(State& s, uint32_t end) noexcept { s.seen = end; }

    inline bool Fresh(const ShotRecord& r, uint64_t now) noexcept
    {
        return now - r.ms <= kFreshMs;
    }

    inline bool QueueReady(const State& s, uint64_t now,
                           const ShotRecord& r, bool enemy) noexcept
    {
        if (!enemy || r.slot >= static_cast<uint32_t>(kMaxNetPlayers))
            return false;
        if (!Fresh(r, now) || s.nPending >= kMaxPending)
            return false;
        return s.lastWhiz == 0 || now - s.lastWhiz >= kWhizCdMs;
    }

    inline void Queue(State& s, const ShotRecord& r,
                      const ClosestApproach& a, uint64_t now) noexcept
    {
        Pending& p = s.pending[s.nPending++];
        p.due = now + kDelayMs;
        p.shot = r.ms;
        p.slot = r.slot;
        p.lat = a.lat;
        p.along = a.t;
        for (int j = 0; j < 3; ++j)
            p.cp[j] = a.cp[j];
    }

    inline void NoteHit(State& s, int slot, uint64_t now) noexcept
    {
        if (slot >= 0 && slot < kMaxNetPlayers)
            s.hitByMs[slot] = now;
    }

    // A hit from that slot after the line, within 250 ms, means the shot connected.
    inline bool HitCancels(const State& s, int slot, uint64_t shotMs) noexcept
    {
        return slot >= 0 && slot < kMaxNetPlayers
            && s.hitByMs[slot] != 0 && s.hitByMs[slot] + kHitCancelMs >= shotMs;
    }

    inline bool ReadyToWhizz(const State& s, uint64_t now,
                             int slot, uint64_t shotMs) noexcept
    {
        if (HitCancels(s, slot, shotMs))
            return false;
        return s.lastWhiz == 0 || now - s.lastWhiz >= kWhizCdMs;
    }

    // ext only starts the 150 ms cooldown when the PCM voice actually started.
    inline void NoteWhizz(State& s, uint64_t now, bool played) noexcept
    {
        if (played)
            s.lastWhiz = now;
    }

    inline void DropPending(State& s, uint32_t end) noexcept
    {
        s.nPending = 0;
        s.seen = end;
    }

    // ext g_los[8]: an exact handle hit, else the least-recently-used entry (ties keep the
    // lowest index), cleared like the ext's memset with verdict = -1 before first use.
    // Callers pass NetPlayer::npcSid / the puppet handle, never a net slot.
    inline LosEntry& LosFor(State& s, uint64_t handle) noexcept
    {
        LosEntry* hit = nullptr;
        LosEntry* old = &s.los[0];
        for (LosEntry& e : s.los) {
            if (e.handle == handle) hit = &e;
            if (e.lastUse < old->lastUse) old = &e;
        }
        if (!hit) {
            hit = old;
            *hit = LosEntry{};
            hit->handle = handle;
            hit->verdict = -1;
        }
        return *hit;
    }

    // ext los_gate: two async static-world rays from our root+1.5 m to the target's
    // +0.6/+1.3 m. A veto needs BOTH rays blocked before 0.85; clear flips at once,
    // blocked needs two agreeing samples; t < 0.02 on both says the cast started inside
    // geometry, which is not a wall. `kick` / `rayT` are supplied by the native owner.
    // The ext's legacy t2r-los-inside-ps38.txt opt-out (which restores the wall veto in
    // that case) is not ported: the corrected rule is always used, and the file stays read-only.
    template<class Kick, class RayT>
    inline bool LosSample(LosEntry& e, uint64_t t60, uint64_t frame,
                          const float ourRoot[3], const float targetRoot[3],
                          Kick&& kick, RayT&& rayT)
    {
        const float dx = targetRoot[0] - ourRoot[0];
        const float dz = targetRoot[2] - ourRoot[2];
        if (e.lastUse == 0 || t60 - e.lastUse > kLosStaleT60) {
            e.verdict = -1;
            e.streak = 0;
            e.j1 = e.j2 = 0;
        }
        e.lastUse = t60;
        if (dx * dx + dz * dz < kLosCloseM * kLosCloseM) {
            e.verdict = 1;
            e.j1 = e.j2 = 0;
            return true;
        }
        if ((e.j1 || e.j2) && frame > e.kickFrame) {
            const float t1 = rayT(e.j1);
            const float t2 = rayT(e.j2);
            bool b1 = t1 >= 0.0f && t1 < kLosBlockedT;
            bool b2 = t2 >= 0.0f && t2 < kLosBlockedT;
            if (b1 && b2 && t1 < kLosInsideT && t2 < kLosInsideT)
                b1 = b2 = false;
            const bool clear = !(b1 && b2);
            e.j1 = e.j2 = 0;
            if (e.verdict < 0)
                e.verdict = clear ? 1 : 0;
            else if (clear == (e.verdict == 1))
                e.streak = 0;
            else if (clear) {
                e.verdict = 1;
                e.streak = 0;
            } else if (++e.streak >= 2) {
                e.verdict = 0;
                e.streak = 0;
            }
        }
        if (!e.j1 && !e.j2 && t60 - e.lastKick >= kLosKickT60) {
            const float a[3] = { ourRoot[0], ourRoot[1] + kLosEyeUpM, ourRoot[2] };
            const float c[3] = { targetRoot[0], targetRoot[1] + kLosChestUpM, targetRoot[2] };
            const float h[3] = { targetRoot[0], targetRoot[1] + kLosHeadUpM, targetRoot[2] };
            e.j1 = kick(a, c);
            e.j2 = kick(a, h);
            e.kickFrame = frame;
            e.lastKick = t60;
        }
        return e.verdict == 1;
    }

    // Single-entry form (tests / callers that own an entry directly). The handle is
    // stamped into the entry, which is what the cache lookup matches on.
    template<class Kick, class RayT>
    inline bool LosGate(LosEntry& e, uint64_t handle, uint64_t t60, uint64_t frame,
                        const float ourRoot[3], const float targetRoot[3],
                        Kick&& kick, RayT&& rayT)
    {
        e.handle = handle;
        return LosSample(e, t60, frame, ourRoot, targetRoot, kick, rayT);
    }

    // Cache form used by the native owner: h == 0 is an unknown handle and is OPEN,
    // exactly like ext's early `if (!h || !mp || !tp) return 1`.
    template<class Kick, class RayT>
    inline bool LosGate(State& s, uint64_t handle, uint64_t t60, uint64_t frame,
                        const float ourRoot[3], const float targetRoot[3],
                        Kick&& kick, RayT&& rayT)
    {
        if (!handle)
            return true;
        return LosGate(LosFor(s, handle), handle, t60, frame,
                       ourRoot, targetRoot, kick, rayT);
    }

    // State holds atomics, so it cannot be assigned wholesale.
    inline void Reset(State& s) noexcept
    {
        for (ShotRecord& r : s.ring) {
            r.seq.store(0, std::memory_order_relaxed);
            r.slot = 0;
            r.ms = 0;
            for (int j = 0; j < 3; ++j) { r.o[j] = 0.0f; r.d[j] = 0.0f; }
            r.len = 0.0f;
        }
        s.seq.store(0, std::memory_order_relaxed);
        s.seen = 0;
        for (Pending& p : s.pending) p = Pending{};
        s.nPending = 0;
        for (uint64_t& t : s.hitByMs) t = 0;
        for (LosEntry& e : s.los) e = LosEntry{};
        s.lastWhiz = 0;
        s.lastSend = 0;
        s.nextLos = 0;
        s.rng = 0;
    }

} // namespace net::audio::near_miss::core
